# CPU Load Balancer

[![CI](https://github.com/jateen321/Dynamic-Load-Balancer-for-Multi-Core-Systems/actions/workflows/ci.yml/badge.svg)](https://github.com/jateen321/Dynamic-Load-Balancer-for-Multi-Core-Systems/actions/workflows/ci.yml)

A userspace load balancer that distributes CPU-bound tasks across a fixed pool
of pinned worker threads, one per core. A dispatcher thread reads a global
priority queue and hands each task to a target core chosen by one of three
interchangeable scheduling policies (round robin, least load, or a
moving-average predictive blend); an idle core steals a pending task from the
most loaded peer's queue instead of sitting empty, so placement isn't the only
thing that's dynamic — tasks already assigned to a core can still migrate
before they start running.

Written as an operating-systems project: the interesting parts are the
concurrency primitives (mutexes, condition variables, atomics), the shutdown
protocol, work stealing, and the scheduling policies.

## Table of Contents
1. [Quick Start](#quick-start)
2. [Architecture](#architecture)
3. [Components](#components)
4. [Configuration](#configuration)
5. [Core Features](#core-features)
6. [Benchmark Suite](#benchmark-suite)
7. [Testing](#testing)
8. [Building](#building)
9. [Continuous Integration](#continuous-integration)
10. [Verification](#verification)
11. [Known Limitations](#known-limitations)

## Quick Start

```bash
make            # configures and builds into build/
make run        # equivalent to ./build/cpu_balancer 4 20
make bench      # equivalent to ./build/cpu_balancer_bench 4 60
```

Or directly:

```bash
./build/cpu_balancer <num_cores> <num_tasks> [policy]
./build/cpu_balancer 4 20                    # 4 cores, 20 tasks, predictive (default)
./build/cpu_balancer 4 20 round_robin        # policy: round_robin | least_load | predictive
```

The program submits `num_tasks` synthetic CPU-bound tasks, distributes them,
prints per-core statistics while they run, and exits on its own once every task
has finished. `Ctrl+C` at any point performs a graceful shutdown.

File structure:

```
.
├── .github
│   └── workflows
│       └── ci.yml            # build -> unit tests -> ASan/UBSan -> ThreadSanitizer
├── CMakeLists.txt
├── Makefile
├── config
│   └── cpu_balancer.conf     # parsed by load_config()
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
├── src
│   ├── benchmark.c
│   ├── config.c
│   ├── cpu_stats.c
│   ├── load_balancer.c
│   ├── logger.c
│   ├── main.c
│   ├── task.c
│   └── task_queue.c
└── tests
    ├── test_common.h
    ├── test_concurrent_producers.c
    ├── test_core_queue.c
    ├── test_select_cpu.c
    ├── test_shutdown.c
    ├── test_task_ownership.c
    ├── test_task_queue_capacity.c
    └── test_task_queue_priority.c
```

### Key Features
- Fixed worker pool — one pinned thread per core, created once, not spawned
  per task
- Per-core work-stealing queues — an idle core pulls pending work from the
  busiest peer instead of waiting
- Three interchangeable scheduling policies: round robin, least load,
  predictive
- Real-time CPU load monitoring from `/proc/stat`
- Priority-based task admission (4-level multi-level queue)
- Moving-average load prediction
- CPU affinity pinning, applied before a worker thread first runs
- Configurable load thresholds and monitoring interval
- Thread-safe logging
- Graceful shutdown on `SIGINT`
- A benchmark suite (`cpu_balancer_bench`) comparing all three policies on
  completion time, throughput, wait time, CPU-utilisation variance, and
  context switches

## Architecture

```
Load Balancer
    ├── CPU Monitor
    │   └── CPU Stats (per core)
    ├── Admission Queue (global, 4 priority buckets)
    ├── Worker Pool (one per core)
    │   └── Core Queue (work-stealing deque, one per worker)
    ├── Configuration
    └── Logger
```

### Threading Model
- **Monitor thread** — samples `/proc/stat` every `monitoring_interval_ms`,
  updates per-core usage and predictions, flags load imbalance.
- **Dispatcher thread** — blocks on the global admission queue, picks a core
  for each task via the configured scheduling policy, and pushes it onto that
  core's own queue. It never runs a task itself and never creates a thread.
- **Worker threads** — a fixed pool, one per core, created once at startup and
  pinned to their core for their entire lifetime. Each pops tasks from its own
  queue in FIFO order; when its queue is empty it steals a task from whichever
  peer has the deepest queue rather than blocking indefinitely. A stolen
  task's `assigned_cpu` is updated to the core that actually ran it — that
  reassignment is the task migration this design is built around.
- **Main thread** — submits tasks, waits for drain, owns the shutdown sequence.

`SIGINT` is blocked in the dispatcher and every worker thread so the signal is
always delivered to main.

## Components

### 1. Load Balancer (`load_balancer.h`)

```c
LoadBalancer* init_load_balancer(LoadBalancerConfig* config);
int  start_load_balancer(LoadBalancer* lb);          /* 0 on success, -1 on failure */
int  submit_task(LoadBalancer* lb, void (*function)(void*), void* args,
                 TaskArgsDestructor args_free, TaskPriority priority);
int  select_cpu(LoadBalancer* lb);                   /* dispatches by scheduling_policy */
void stop_load_balancer(LoadBalancer* lb);
void cleanup_load_balancer(LoadBalancer* lb);

int  load_balancer_active_tasks(LoadBalancer* lb);    /* executing right now */
int  load_balancer_pending_tasks(LoadBalancer* lb);   /* queued, not yet executing */
int  load_balancer_worker_stats(LoadBalancer* lb, int cpu_id, WorkerStats* out);
```

`running` is an `atomic_int`, not a plain `int` — it is written by the shutdown
path and read by the dispatcher and every worker thread.

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

#### Worker pool lifetime

Workers are created once in `start_load_balancer()` — one per core, pinned via
thread attributes before `pthread_create` — and joined once in
`stop_load_balancer()`. No thread is ever created or joined per task; the
per-task cost is now just a queue push and a queue pop.

A worker only exits once it is certain no further task can ever reach it:
`running` is false, the dispatcher thread has been joined (so nothing will
ever be pushed to any core's queue again), and every core's queue — its own
and every peer's — is observed empty. That last condition is checked without a
global counter: once the dispatcher is gone, the total task count across every
core queue only decreases, so a scan that finds everything empty stays correct
even though it isn't atomic across queues.

This mirrors why the previous per-task design tracked threads in a join
registry rather than detaching them: a condition variable can only report that
a counter reached zero, and `pthread_join` is the only primitive that reports
a thread will never execute another instruction. Freeing the balancer while a
worker could still touch it would be a use-after-free, so shutdown always
joins before cleanup runs.

#### Task migration and work stealing

`select_cpu()` picks a core once, when a task is admitted from the global
queue — but that placement is only ever the *first* assignment. Each core owns
its own queue (`CoreQueue` in `task_queue.h`); when a worker finds its queue
empty it looks for the peer with the deepest queue and, if that peer has more
than one task queued, steals the task at the *back* of that peer's queue (the
worker itself always drains its own queue from the *front*, so the two ends
rarely collide). The stolen task's `Task::assigned_cpu` is updated to the
stealing core before it runs — that reassignment is what makes "dynamic load
balancing" apply to a task that was already assigned, not just to the
placement decision.

Stealing only ever moves a task that has not started running yet. A task
already executing is never migrated mid-flight — see
[Known Limitations](#known-limitations).

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
writes them, the dispatcher thread reads them when scoring cores for
`SCHED_LEAST_LOAD` and `SCHED_PREDICTIVE`.

### 3. Task Queue (`task_queue.h`)

Two different queue types, serving two different roles:

**Global admission queue (`TaskQueue`)** — one FIFO ring per priority level,
fed by `submit_task()` and drained by the dispatcher thread.

- Four buckets, one per `TaskPriority`
- Dequeue serves the highest-priority non-empty bucket
- FIFO order preserved within a level
- O(1) enqueue and dequeue
- Bounded by `max_tasks`; producers block on `not_full`, consumers on `not_empty`
- A `shutdown` flag in both wait predicates lets blocked threads exit

**Per-core work-stealing queue (`CoreQueue`)** — one per worker, fed by the
dispatcher and drained by its owning worker.

- Unbounded, doubly-linked node list rather than a capacity-bounded ring: a
  single core can hold more tasks over a run than the admission queue's
  capacity ever had in flight at once, since a core's queue is replenished as
  its worker drains it
- The owner pops from the *front* (FIFO, preserves dispatch order); a thief
  steals from the *back*, and only if the queue holds more than one task —
  opposite ends reduce contention, and never taking the last task keeps an
  idle peer from stripping work out from under the owner the instant it
  arrives
- `core_queue_try_steal()` uses `pthread_mutex_trylock`, not a blocking lock —
  an idle thief must never block waiting on a busy owner
- No priority levels: task priority is already honoured once, by the
  admission queue that fed this core; a worker just drains its own queue in
  the order tasks arrived

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

`init_default_config()` returns the defaults below. `load_config(path)` reads
`config/cpu_balancer.conf`-style JSON — a flat, one-level object — and starts
from those same defaults, overriding only the keys actually present in the
file; a missing/unreadable/malformed file, or a key with the wrong type, logs
a warning and falls back to the default for whatever it couldn't use, never
failing outright. `num_cpus` isn't file-configurable: `main()` always sets it
from argv after loading a config, so a value in the file is accepted but
ignored.

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
    config->scheduling_policy = SCHED_PREDICTIVE;
    config->enable_work_stealing = 1;
    config->on_task_complete = NULL;
    config->on_task_complete_user_data = NULL;
    return config;
}
```

| Parameter | Meaning |
|---|---|
| `max_tasks` | Admission queue capacity; producers block when full |
| `monitoring_interval_ms` | `/proc/stat` sampling period |
| `high_load_threshold` | Usage above which a core counts as saturated |
| `low_load_threshold` | Usage below which a core counts as idle |
| `load_history_size` | Samples retained per core for the moving average |
| `enable_load_prediction` | Blend predicted load into placement decisions |
| `enable_detailed_logging` | Print per-core stats each interval |
| `num_cpus` | Cores to distribute across; also the size of the worker pool |
| `scheduling_policy` | `SCHED_ROUND_ROBIN` \| `SCHED_LEAST_LOAD` \| `SCHED_PREDICTIVE` |
| `enable_work_stealing` | Let an idle core steal from the busiest peer's queue |
| `on_task_complete` | Optional hook invoked after each task finishes, before it is freed; used by the benchmark suite to collect per-task timing without the library knowing anything about benchmarking |

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

### 3. Task Distribution: Scheduling Policies

`select_cpu()` in `load_balancer.c` dispatches to one of three policies based
on `config->scheduling_policy`:

- **`SCHED_ROUND_ROBIN`** — an atomic cursor cycles through cores in order.
  Ignores load entirely; it exists as the baseline the other two are measured
  against.
- **`SCHED_LEAST_LOAD`** — current `/proc/stat` usage, biased against cores
  with work already in flight or queued.
- **`SCHED_PREDICTIVE`** — `SCHED_LEAST_LOAD`'s scoring blended 50/50 with the
  moving-average prediction.

`SCHED_LEAST_LOAD` and `SCHED_PREDICTIVE` share a scoring loop:

```c
double effective_load = stats[i].current_usage;

if (use_prediction && config->enable_load_prediction) {
    effective_load = (effective_load + stats[i].predicted_load) / 2;
}
effective_load += (stats[i].active_tasks * 10);
effective_load += (core_queue_size(workers[i].queue) * 5);
```

The `active_tasks * 10` term biases against cores that already have work in
flight. `/proc/stat` usage lags by up to one monitoring interval, so a core
handed a task a moment ago still looks idle; without this term a burst of
submissions all lands on the same core. The queue-depth term is the analogous
correction for the dispatcher itself: a burst of submissions should spread
across cores' queues immediately, not stack up behind one core just because
its `current_usage` hasn't risen yet.

Whichever policy places a task, work stealing (see
[Task migration and work stealing](#task-migration-and-work-stealing)) can
still move it before it starts running — placement decides where a task
*starts*, not where it necessarily runs.

### 4. Affinity

Affinity is set on the thread *attributes* before `pthread_create`, not with
`pthread_setaffinity_np` afterwards — a new thread is runnable the instant it
is created, so setting affinity after the fact races with it and the first
scheduling slice can land on the wrong core. This now applies to worker
threads (pinned once, at pool startup) rather than to a thread spawned per
task.

## Benchmark Suite

`cpu_balancer_bench` (`src/benchmark.c`) runs the same synthetic workload
under all three scheduling policies, back to back, and prints a comparison
table:

```bash
make bench                                  # 4 cores, 60 tasks/policy
./build/cpu_balancer_bench                  # same, using online core count
./build/cpu_balancer_bench 8 200 10 80 123  # cores, tasks, min_ms, max_ms, seed
```

```
Benchmarking scheduling policies: 4 core(s), 60 task(s)/policy, duration [20, 150] ms, seed 42

Running Round Robin...
Running Least Load...
Running Predictive...

Scheduler      Completion time   CPU imbalance   Throughput      Avg wait    Ctx switches   Migrated
Round Robin    1.4 s             4.5%            44.2 tasks/s    582.7 ms    269            2
Least Load     1.4 s             4.2%            44.2 tasks/s    588.3 ms    238            0
Predictive     1.4 s             2.3%            44.1 tasks/s    586.1 ms    293            2

Least Load completed fastest (1.4 s); Predictive had the least CPU imbalance (2.3%).
```

Each policy runs on a **fresh `LoadBalancer`** but the **same seeded task
duration sequence**, so policy is the only variable across the three rows.
Tasks are a millisecond-scale CPU-bound busy-spin (not `usleep`, which would
make every core look idle regardless of policy).

What each column measures, and how:

| Column | Meaning | Source |
|---|---|---|
| Completion time | Wall clock from first submit to `wait_for_tasks_completion()` returning | `CLOCK_MONOTONIC` around the run |
| CPU imbalance | Population stdev of each core's busy-time fraction of the run | `Task::assigned_cpu` + `Task::cpu_usage`, accumulated via the `on_task_complete` hook |
| Throughput | Tasks completed per second | tasks / completion time |
| Avg wait | Mean of `start_time - create_time` across all tasks | same hook |
| Ctx switches | Sum of voluntary + involuntary context switches across every worker | `load_balancer_worker_stats()`, via `getrusage(RUSAGE_THREAD)` |
| Migrated | Tasks moved by work stealing | `WorkerStats::tasks_stolen_by_me`, summed |

CPU imbalance is deliberately **not** read from `CPUMonitor`'s `/proc/stat`
samples: those reflect whole-system usage, which is noisy and not exclusively
attributable to the benchmark's own tasks, especially on a shared or
virtualised machine. Busy time attributed by `assigned_cpu` is exact and
reproducible instead.

## Testing

`tests/` holds one small, focused binary per concern rather than one
monolithic runner, wired into CMake via CTest — no external test framework, in
keeping with the project's pthread/m/rt-only dependency footprint. Each file
is a plain `main()` using a `CHECK()` macro (`tests/test_common.h`) rather than
`assert()`, since CMake's default Release flags define `NDEBUG`, which would
silently no-op a plain `assert()` under exactly the build CTest runs by
default.

```bash
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

| Test | Covers |
|---|---|
| `test_task_queue_priority` | Global admission queue: highest-priority-first, FIFO within a level |
| `test_task_queue_capacity` | A blocked producer on a full queue unblocks on either free space or shutdown |
| `test_core_queue` | Per-core queue FIFO from the owner; `try_steal` refuses at count ≤ 1 and takes the tail above it; concurrent push/pop/steal loses nothing |
| `test_select_cpu` | All three `SchedulingPolicy` values, white-box against `cpu_monitor->stats` — round robin's cycle order, least-load's and predictive's scoring (including the active-tasks and queue-depth terms, the blended average, and the `enable_load_prediction=0` fallback) |
| `test_task_ownership` | `submit_task`'s destructor-exactly-once contract across success, rejection, and cancellation at shutdown |
| `test_shutdown` | Nothing left pending/active after `stop_load_balancer`; safe to call twice; NULL-`lb` safety |
| `test_concurrent_producers` | Multiple threads racing `enqueue_task`/`submit_task` lose or duplicate nothing |

Most tests run through the public API only; `test_select_cpu` is white-box —
`LoadBalancer`, `CPUMonitor`, `TaskQueue`, and `CoreQueue` are all fully
defined (not opaque) in their headers, so writing directly into
`lb->cpu_monitor->stats[i]` under its lock is how a deterministic scoring
scenario gets built without depending on real `/proc/stat` numbers.

The test suite is what actually caught this project's one confirmed
concurrency bug outside the sanitizer runs already covered above: three
`log_message()` calls (in `enqueue_task`, `core_queue_push`, and the
dispatcher) used to read `task->task_id`/`task->priority` *after* the task
had already been unlocked and handed to a consumer that could pop, run, and
free it first — a real, ASan-reproducible use-after-free once a worker pool
can pick up and finish a task within microseconds of it being queued. Fixed
by capturing those fields into locals before the task becomes visible to
anyone else. `test_shutdown` and `test_concurrent_producers` are what
exercise this path.

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
make bench           # builds and runs the policy comparison
make tsan            # ThreadSanitizer build (all executables, incl. tests)
make asan            # AddressSanitizer + UBSan build (all executables, incl. tests)
```

`cmake` produces a static library, `cpu_balancer_core`, linked into
`cpu_balancer` (`src/main.c`), `cpu_balancer_bench` (`src/benchmark.c`), and
every `tests/test_*` binary; `cmake --install` installs the two main
executables.

## Continuous Integration

`.github/workflows/ci.yml` runs on every push and pull request, one sequential
job so a failure at any stage stops the pipeline there:

```
build  →  unit tests  →  ASan / UBSan  →  ThreadSanitizer
```

Each stage after the first re-runs `ctest --test-dir <dir> --output-on-failure`
against a fresh configure/build of that stage's variant — the sanitizer stages
use the exact same `-fsanitize=...` flags as the `make tsan`/`make asan`
Makefile targets, so a local `make tsan`/`make asan` run reproduces what CI
checks.

## Verification

Checked on a 4-core Linux box, GCC 13.3:

| Check | Result |
|---|---|
| `-Wall -Wextra -Wpedantic` | 0 warnings (all executables, incl. tests) |
| Normal run to completion, all 3 policies | exit 0 |
| `SIGINT` mid-run, all 3 policies | graceful, time spent waiting for in-flight tasks |
| Work stealing observed in normal operation | `tasks_stolen_by_me` > 0 in representative runs |
| `ctest`, Release build | 7/7 tests pass |
| `ctest`, ThreadSanitizer build | 7/7 tests pass, 0 data races, stable across repeated runs |
| `ctest`, ASan/UBSan build | 7/7 tests pass, 0 errors, 0 leaks, stable across repeated runs |
| ThreadSanitizer, `cpu_balancer` normal path, all 3 policies | 0 data races |
| ThreadSanitizer, `cpu_balancer` `SIGINT` path | 0 data races |
| ThreadSanitizer, `cpu_balancer_bench` (concurrent `on_task_complete` hook) | 0 data races |
| Valgrind, normal path | `All heap blocks were freed`, 0 errors |
| Valgrind, `SIGINT` path | `All heap blocks were freed`, 0 errors |
| ASan, `cpu_balancer_bench`, all 3 policies | 0 errors, 0 leaks |
| ASan: task outliving shutdown | no use-after-free; cleanup blocks until joined |
| ASan: multi-block args cancelled at shutdown | destroyed exactly once, 0 leaks |
| ASan: non-owned args, `NULL` destructor | never freed |
| ASan: `load_config()` against adversarial fixtures | 0 errors, 0 leaks (malformed JSON, huge numbers, truncated input, wrong types) |
| Priority queue ordering + FIFO within level (admission queue) | pass |
| `cpu_balancer_bench` output sanity (multiple core/task/seed combinations) | no NaN, plausible ranges |

## Known Limitations

Stated explicitly rather than implied by omission:

- **Migration only ever moves a task that hasn't started running yet.** Work
  stealing takes tasks off a peer's *queue*; a task already executing on a
  core stays there until it finishes — there is no mechanism (and none is
  planned) to preempt and relocate a running task mid-flight. A core that
  gets stuck with one long task can still sit busy while peers steal
  everything else out from under it.
- **One logger per process.** The logger's state is file-scope, so two
  LoadBalancer instances in one process would share (and reconfigure) it. The
  active-task accounting is now per-instance, but the logger is not.
- **`stop_load_balancer` is idempotent for sequential calls, not for
  concurrent ones.** Two threads calling it at once is not supported.
- **`CPUStats::temperature` is never populated.** The field exists; nothing
  reads a thermal zone.
- **`load_config()`'s parser is intentionally minimal.** It handles the flat,
  one-level object this project's config format actually uses (see
  [Configuration](#configuration)); it is not a general JSON parser and
  doesn't try to be one — nested objects/arrays under an unknown key are
  skipped rather than rejected, but a known key given one is a type mismatch
  (falls back to that field's default).
- **Linux-only.**

See `docs/LEARNING_GUIDE.md` for the operating-systems concepts behind each
component.
