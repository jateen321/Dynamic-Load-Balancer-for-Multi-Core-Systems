# CPU Load Balancer

A userspace load balancer that distributes CPU-bound tasks across cores. It
reads per-core utilisation from `/proc/stat`, picks a target core with a
cost function that blends measured load, a moving-average prediction, and the
number of tasks already in flight, then pins each task's thread to that core
with `pthread_setaffinity_np`.

Written as an operating-systems project: the interesting parts are the
concurrency primitives (mutexes, condition variables, atomics), the shutdown
protocol, and the scheduling policy.

## Table of Contents
1. [Quick Start](#quick-start)
2. [Architecture](#architecture)
3. [Components](#components)
4. [Configuration](#configuration)
5. [Core Features](#core-features)
6. [Building](#building)
7. [Verification](#verification)
8. [Known Limitations](#known-limitations)

## Quick Start

```bash
make            # configures and builds into build/
make run        # equivalent to ./build/cpu_balancer 4 20
```

Or directly:

```bash
./build/cpu_balancer <num_cores> <num_tasks>
./build/cpu_balancer 4 20        # 4 cores, 20 tasks
```

The program submits `num_tasks` synthetic CPU-bound tasks, distributes them,
prints per-core statistics while they run, and exits on its own once every task
has finished. `Ctrl+C` at any point performs a graceful shutdown.

File structure:

```
.
├── CMakeLists.txt
├── Makefile
├── config
│   └── cpu_balancer.conf     # template for the not-yet-implemented parser
├── docs
│   └── LEARNING_GUIDE.md     # OS concepts behind each part of the code
├── include
│   ├── config.h
│   ├── cpu_stats.h
│   ├── load_balancer.h
│   ├── logger.h
│   ├── task.h
│   └── task_queue.h
├── README.md
└── src
    ├── config.c
    ├── cpu_stats.c
    ├── load_balancer.c
    ├── logger.c
    ├── main.c
    ├── task.c
    └── task_queue.c
```

### Key Features
- Dynamic task distribution across multiple CPU cores
- Real-time CPU load monitoring from `/proc/stat`
- Priority-based task scheduling (4-level multi-level queue)
- Moving-average load prediction
- CPU affinity pinning, applied before a task thread first runs
- Configurable load thresholds and monitoring interval
- Thread-safe logging
- Graceful shutdown on `SIGINT`

## Architecture

```
Load Balancer
    ├── CPU Monitor
    │   └── CPU Stats (per core)
    ├── Task Queue
    │   └── 4 priority buckets
    ├── Configuration
    └── Logger
```

### Threading Model
- **Monitor thread** — samples `/proc/stat` every `monitoring_interval_ms`,
  updates per-core usage and predictions, flags load imbalance.
- **Scheduler thread** — blocks on the queue, picks a core for each task, and
  spawns a pinned, detached thread to run it.
- **Task threads** — one detached thread per task.
- **Main thread** — submits tasks, waits for drain, owns the shutdown sequence.

`SIGINT` is blocked in the scheduler thread so the signal is always delivered
to main.

## Components

### 1. Load Balancer (`load_balancer.h`)

```c
LoadBalancer* init_load_balancer(LoadBalancerConfig* config);
int  submit_task(LoadBalancer* lb, void (*function)(void*), void* args, TaskPriority priority);
void start_load_balancer(LoadBalancer* lb);
void stop_load_balancer(LoadBalancer* lb);
void cleanup_load_balancer(LoadBalancer* lb);
```

`running` is an `atomic_int`, not a plain `int` — it is written by the shutdown
path and read by both worker threads.

### 2. CPU Monitor (`cpu_stats.h`)

```c
typedef struct {
    int cpu_id;
    double current_usage;
    double *usage_history;
    int history_index;   /* next slot to write (wraps) */
    int history_count;   /* samples held, saturates at load_history_size */
    uint64_t user_time, nice_time, system_time, idle_time;
    uint64_t iowait_time, irq_time, softirq_time, steal_time;
    double temperature;   /* reserved, not currently populated */
    double predicted_load;
    int active_tasks;
} CPUStats;
```

All `CPUStats` fields are guarded by `CPUMonitor::lock`: the monitor thread
writes them, the scheduler thread reads them in `find_best_cpu`.

### 3. Task Queue (`task_queue.h`)

A multi-level queue: one FIFO ring per priority level.

- Four buckets, one per `TaskPriority`
- Dequeue serves the highest-priority non-empty bucket
- FIFO order preserved within a level
- O(1) enqueue and dequeue
- Bounded by `max_tasks`; producers block on `not_full`, consumers on `not_empty`
- A `shutdown` flag in both wait predicates lets blocked threads exit

### 4. Task Management (`task.h`)

```c
typedef enum {
    PRIORITY_LOW = 0,
    PRIORITY_MEDIUM = 1,
    PRIORITY_HIGH = 2,
    PRIORITY_CRITICAL = 3
} TaskPriority;

typedef enum {
    STATUS_PENDING,
    STATUS_RUNNING,
    STATUS_COMPLETED,
    STATUS_FAILED
} TaskStatus;
```

Task IDs come from `__atomic_fetch_add(..., __ATOMIC_SEQ_CST)`.

## Configuration

```c
LoadBalancerConfig* init_default_config(void) {
    LoadBalancerConfig* config = malloc(sizeof(LoadBalancerConfig));
    config->max_tasks = 10;
    config->monitoring_interval_ms = 100;
    config->high_load_threshold = 80.0;
    config->low_load_threshold = 20.0;
    config->load_history_size = 10;
    config->enable_load_prediction = 1;
    config->enable_detailed_logging = 1;
    config->log_file_path = strdup("./cpu_balancer.log");
    config->num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    return config;
}
```

| Parameter | Meaning |
|---|---|
| `max_tasks` | Queue capacity; producers block when full |
| `monitoring_interval_ms` | `/proc/stat` sampling period |
| `high_load_threshold` | Usage above which a core counts as saturated |
| `low_load_threshold` | Usage below which a core counts as idle |
| `load_history_size` | Samples retained per core for the moving average |
| `enable_load_prediction` | Blend predicted load into placement decisions |
| `enable_detailed_logging` | Print per-core stats each interval |
| `num_cpus` | Cores to distribute across |

## Core Features

### 1. CPU Load Monitoring

Per-core jiffie counters in `/proc/stat` are cumulative since boot, so usage is
computed from the delta between two samples:

```c
uint64_t total_delta = total_time - prev_total;
uint64_t idle_delta  = idle_time  - prev_idle;

if (total_delta > 0) {
    cpu->current_usage = 100.0 * (1.0 - ((double)idle_delta / (double)total_delta));
}
```

### 2. Load Prediction

Simple moving average over the samples actually held:

```c
double predict_cpu_load(CPUStats* cpu) {
    double sum = 0.0;
    if (cpu->history_count == 0) return cpu->current_usage;
    for (int i = 0; i < cpu->history_count; i++) sum += cpu->usage_history[i];
    return sum / cpu->history_count;
}
```

The average uses `history_count`, not `history_index` — the latter is a write
cursor that wraps to zero, which would shrink the window to nothing.

### 3. Task Distribution

```c
int find_best_cpu(CPUMonitor* monitor) {
    int best_cpu = -1;
    double lowest_load = 999.9;

    pthread_mutex_lock(&monitor->lock);
    for (int i = 0; i < monitor->num_cpus; i++) {
        double effective_load = monitor->stats[i].current_usage;

        if (monitor->config->enable_load_prediction) {
            effective_load = (effective_load + monitor->stats[i].predicted_load) / 2;
        }
        effective_load += (monitor->stats[i].active_tasks * 10);

        if (effective_load < lowest_load) {
            lowest_load = effective_load;
            best_cpu = i;
        }
    }
    pthread_mutex_unlock(&monitor->lock);
    return best_cpu;
}
```

The `active_tasks * 10` term biases against cores that already have work in
flight. `/proc/stat` usage lags by up to one monitoring interval, so a core
handed a task a moment ago still looks idle; without this term a burst of
submissions all lands on the same core.

### 4. Affinity

Affinity is set on the thread *attributes* before `pthread_create`, not with
`pthread_setaffinity_np` afterwards — a new thread is runnable the instant it
is created, so setting affinity after the fact races with it and the first
scheduling slice can land on the wrong core.

## Building

### Prerequisites
- CMake >= 3.14
- C11 compiler (GCC or Clang)
- pthreads
- Linux (uses `/proc/stat`, `pthread_attr_setaffinity_np`)

### Build

```bash
make                 # or: cmake -S . -B build && cmake --build build
make clean
make tsan            # ThreadSanitizer build
make asan            # AddressSanitizer + UBSan build
```

## Verification

Checked on a 4-core Linux box, GCC 13.3:

| Check | Result |
|---|---|
| `-Wall -Wextra -Wpedantic` | 0 warnings |
| Normal run to completion | exit 0 |
| `SIGINT` mid-run | graceful, ~1.5 s (time spent waiting for in-flight tasks) |
| ThreadSanitizer | 0 data races |
| Valgrind, normal path | `All heap blocks were freed`, 0 errors |
| Valgrind, `SIGINT` path | `All heap blocks were freed`, 0 errors |

## Known Limitations

Stated explicitly rather than implied by omission:

- **`load_config()` is a stub.** It ignores the path and returns the defaults.
  `config/cpu_balancer.conf` is a template for the parser that would consume
  it; no configuration file is actually read.
- **No task migration.** Placement is decided once, when the task is
  scheduled. A task already running on a core is never moved, so a core that
  becomes hot after placement stays hot. `check_load_balance()` logs the
  imbalance but does not act on it.
- **Thread-per-task, not a thread pool.** Every task gets a fresh thread.
  Fine at this scale, wasteful at thousands of short tasks.
- **`CPUStats::temperature` is never populated.** The field exists; nothing
  reads a thermal zone.
- **No unit test suite in-tree.** Correctness was checked with the sanitizers
  and Valgrind runs above.
- **Linux-only.**

See `docs/LEARNING_GUIDE.md` for the operating-systems concepts behind each
component.
