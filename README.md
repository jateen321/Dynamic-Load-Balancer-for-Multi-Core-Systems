# CPU Load Balancer

A userspace load balancer that distributes CPU-bound tasks across cores. It
reads per-core utilisation from `/proc/stat`, picks a target core with a
cost function that blends measured load, a moving-average prediction, and the
number of tasks already in flight, then pins each task's thread to that core
with `pthread_setaffinity_np`.

## Scope

**This is a scheduler simulation for learning, not a production tool.** The
Linux kernel already does this job, and does it better — see
[Why not just use the kernel scheduler?](#why-not-just-use-the-kernel-scheduler)
for the measured reasons why. Nothing here is intended to make a real workload
faster, and it should not be deployed expecting that.

What it *is* good for: a working model of how a scheduler makes placement
decisions, built out of the real primitives — mutexes, condition variables,
atomics, a bounded producer–consumer queue, a multi-level priority queue,
join-based thread lifetime, and CPU affinity. Those concepts are all genuine;
it is their composition into a userspace load balancer that would not ship.

## Table of Contents
1. [Quick Start](#quick-start)
2. [Why not just use the kernel scheduler?](#why-not-just-use-the-kernel-scheduler)
3. [Architecture](#architecture)
4. [Components](#components)
5. [Configuration](#configuration)
6. [Core Features](#core-features)
7. [Building](#building)
8. [Verification](#verification)
9. [Known Limitations](#known-limitations)
10. [Roadmap](#roadmap)

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

### What it implements
- Dynamic task distribution across multiple CPU cores
- Real-time CPU load monitoring from `/proc/stat`
- Priority-based task scheduling (4-level multi-level queue)
- Moving-average load prediction
- CPU affinity pinning, applied before a task thread first runs
- Explicit task-argument ownership via a destructor callback
- Join-based task thread lifetime — no detached threads, no shutdown timeout
- Configurable load thresholds and monitoring interval
- Thread-safe logging with a stderr fallback
- Graceful shutdown on `SIGINT`

## Why not just use the kernel scheduler?

Because you should. This section exists so the project does not quietly imply
otherwise.

Linux (CFS, and EEVDF since 6.6) solves the same problem with advantages this
program structurally cannot obtain:

| | This project | Kernel scheduler |
|---|---|---|
| Load signal | `/proc/stat`, up to one interval stale | Live runqueue lengths |
| Core goes hot after placement | Nothing happens | Migrates the task |
| Core goes idle | Nothing happens | Work stealing, in microseconds |
| Cache / NUMA topology | Unaware | Scheduling domains |
| Decision cost | Two syscalls plus a scan | Nanoseconds, already in the path |

The deeper problem is not accuracy, it is that **pinning disables the mechanism
that would have corrected a bad decision.** Setting an affinity mask tells the
kernel it may not migrate that thread. So the program removes a good dynamic
scheduler and substitutes a worse one working from stale data — and when the
placement turns out wrong, the task is stuck there for its whole runtime.

The `active_tasks * 10` term in `find_best_cpu` is the tell. That constant
exists only to compensate for measurement lag: a core handed a task a moment
ago still looks idle in `/proc/stat`. The kernel needs no such fudge because it
is not guessing.

### Where userspace pinning is genuinely used

Pinning is a real technique — but always in a form this project does not use:

- **HPC** — pinning MPI ranks and OpenMP threads for NUMA locality (`numactl`,
  `hwloc`, `OMP_PROC_BIND`)
- **Low-latency trading** — `isolcpus` + `nohz_full`, one hot thread per
  isolated core
- **Thread-per-core databases** — Seastar, ScyllaDB, Redis
- **Kubernetes CPU Manager** — exclusive cpusets for latency-sensitive pods

The pattern is the same in every case: *static, topology-aware* pinning, used to
eliminate scheduler interference. None of them measure load and place
dynamically, because that is precisely what the kernel already does better.

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
- **Task threads** — one joinable thread per task, tracked in a registry so
  shutdown joins every one of them rather than waiting on a timeout.
- **Main thread** — submits tasks, waits for drain, owns the shutdown sequence.

`SIGINT` is blocked in the scheduler thread so the signal is always delivered
to main.

## Components

### 1. Load Balancer (`load_balancer.h`)

```c
LoadBalancer* init_load_balancer(LoadBalancerConfig* config);
int  start_load_balancer(LoadBalancer* lb);          /* 0 on success, -1 on failure */
int  submit_task(LoadBalancer* lb, void (*function)(void*), void* args,
                 TaskArgsDestructor args_free, TaskPriority priority);
void stop_load_balancer(LoadBalancer* lb);
void cleanup_load_balancer(LoadBalancer* lb);
```

`running` is an `atomic_int`, not a plain `int` — it is written by the shutdown
path and read by both worker threads.

#### Argument ownership

`submit_task` **takes ownership of `args` on entry**. On return — success or
failure — the caller must not touch `args` again; the library destroys it
exactly once via `args_free`, whether the task ran, was cancelled at shutdown,
or was rejected outright.

```c
int* id = malloc(sizeof(int));
submit_task(lb, cpu_task, id, free, PRIORITY_MEDIUM);   /* library owns id now */
```

Pass `NULL` as the destructor to state that the library must *not* free the
args — correct for a stack address, a string literal, a pointer into a
caller-owned arena, or an integer encoded in a pointer. `NULL` means
non-ownership, not "use `free`".

Transfer happens on entry rather than on success so the caller has no branch to
get wrong. The previous design had three contradictory rules — the task function
freed args on success, the library freed them on cancel, and the caller freed
them on submit failure — which no caller could satisfy without reading the
implementation, and which silently leaked any args holding internal pointers.

#### Task thread lifetime

Task threads are joinable and registered before they can run. Shutdown closes
the registry, waits for the live count to reach zero, and joins every thread
before anything is freed.

This matters because a condition variable can only report that a counter
reached zero; `pthread_join` is the only primitive that reports that a thread
will never execute another instruction. The earlier design detached its task
threads and waited on a 5-second timeout, so a task outliving that timeout kept
running while `cleanup_load_balancer` freed the balancer underneath it —
a use-after-free on `lb->cpu_monitor`, reproducible under ASan. The timeout is
now a diagnostic that logs and keeps waiting, never an exit.

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
| `SIGINT` mid-run | graceful, time spent waiting for in-flight tasks |
| ThreadSanitizer, normal path | 0 data races |
| ThreadSanitizer, `SIGINT` path | 0 data races |
| Valgrind, normal path | `All heap blocks were freed`, 0 errors |
| Valgrind, `SIGINT` path | `All heap blocks were freed`, 0 errors |
| ASan: task outliving shutdown | no use-after-free; cleanup blocks until joined |
| ASan: multi-block args cancelled at shutdown | destroyed exactly once, 0 leaks |
| ASan: non-owned args, `NULL` destructor | never freed |
| Priority queue ordering + FIFO within level | pass |

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
- **One logger per process.** The logger's state is file-scope, so two
  LoadBalancer instances in one process would share (and reconfigure) it. The
  active-task accounting is now per-instance, but the logger is not.
- **`stop_load_balancer` is idempotent for sequential calls, not for
  concurrent ones.** Two threads calling it at once is not supported.
- **`CPUStats::temperature` is never populated.** The field exists; nothing
  reads a thermal zone.
- **No unit test suite in-tree.** Correctness was checked with the sanitizers
  and Valgrind runs above.
- **Linux-only.**

## Roadmap

Three directions, in descending order of how much they'd add.

### 1. Turn it into a scheduling-policy benchmark

The project currently *asserts* a placement policy. It would be far more useful
if it **compared** them and reported numbers. Add a `--policy` flag and
implement the classics behind a function pointer:

| Policy | Description |
|---|---|
| `os` | No pinning at all — the kernel baseline |
| `rr` | Round-robin across cores |
| `least-loaded` | What the code does today |
| `random` | Uniform random core |
| `p2c` | Power-of-two-choices: sample two, take the better |
| `steal` | Per-core queues with work stealing |

Then run one workload through each and report makespan, per-core utilisation
spread, and task wait-time p50/p99. This reframes the limitation above as the
finding: *measuring* the gap to the kernel is a genuine result, and
power-of-two-choices getting most of least-loaded's benefit at a fraction of
the sampling cost is worth demonstrating.

Two supporting pieces this needs:

- A `--distribution` flag (uniform / bimodal / heavy-tailed). With today's
  uniform 1–3 s tasks every policy looks identical; heavy-tailed workloads are
  where placement decisions actually matter.
- Metric aggregation — though `Task` already records `create_time`,
  `start_time` and `end_time`, so the instrumentation largely exists.

### 2. Drop the pinning and become a thread pool

Replace thread-per-task with N workers looping on the queue and delete the
affinity code. What remains is a small C thread pool with priority scheduling
and clean shutdown — something C has no standard equivalent for, and therefore
genuinely reusable. The hard parts (bounded multi-level queue, shutdown
protocol, join-based lifetime) already exist.

### 3. Static, topology-aware pinning

Keep affinity but make it static and NUMA/hyperthread-aware, reading
`/sys/devices/system/cpu/`. Narrower, but it is the form pinning actually takes
in production.

### Usability work, independent of the above

- **Implement `load_config`.** It is still a stub that ignores its argument
  while the config file is documented — the largest remaining gap between
  claims and code.
- **Real CLI flags** via `getopt_long` (`--cores`, `--tasks`, `--help`)
  instead of positional arguments.
- **Split the library from the demo** — build `libcpubalancer.a` plus a thin
  `main.c`, so the balancer can be used from other code.
- **Move the test harnesses in-tree** and add a `make test` target.
- **`--json` output** so runs can be plotted.

---

See `docs/LEARNING_GUIDE.md` for the operating-systems concepts behind each
component.
