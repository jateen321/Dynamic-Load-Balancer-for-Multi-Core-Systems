# Learning Guide — Dynamic Load Balancer for Multi-Core Systems

This guide is written for someone **new to low-level programming with threads**. It walks
through every operating-systems topic this project touches, in the order that makes them
easiest to learn, and points at the exact lines in *our* code where each idea lives.

The project is deliberately a good teacher: it contains working examples of most concepts
**and** real bugs that demonstrate what happens when you get them wrong. Those bugs are
listed as labs, not as chores — fixing them is the learning.

**How to use this document**

1. Read Part I once, end to end. It builds the vocabulary.
2. From Part II onward, keep the referenced source file open beside the guide.
3. Do the 🔬 **Labs**. They are ordered easiest → hardest.
4. Part VIII is a self-quiz / viva prep.

---

## Table of Contents

**Part I — Foundations**
- [1. Why this project exists](#1-why-this-project-exists)
- [2. Process vs. thread vs. core](#2-process-vs-thread-vs-core)
- [3. Shared memory: the source of all trouble](#3-shared-memory-the-source-of-all-trouble)
- [4. pthreads: the API](#4-pthreads-the-api)

**Part II — The concurrency toolkit**
- [5. Mutexes (mutual exclusion)](#5-mutexes-mutual-exclusion)
- [6. Condition variables and the predicate loop](#6-condition-variables-and-the-predicate-loop)
- [7. The producer–consumer / bounded buffer pattern](#7-the-producerconsumer--bounded-buffer-pattern)
- [8. Race conditions and atomics](#8-race-conditions-and-atomics)
- [9. Deadlock](#9-deadlock)

**Part III — Scheduling and load balancing**
- [10. CPU scheduling theory](#10-cpu-scheduling-theory)
- [11. Priorities, starvation, and aging](#11-priorities-starvation-and-aging)
- [12. Load balancing algorithms](#12-load-balancing-algorithms)
- [13. Load prediction](#13-load-prediction)
- [14. Thread-per-task vs. thread pool](#14-thread-per-task-vs-thread-pool)

**Part IV — Linux systems interfaces**
- [15. `/proc/stat` and CPU accounting](#15-procstat-and-cpu-accounting)
- [16. CPU affinity, cache locality, NUMA](#16-cpu-affinity-cache-locality-numa)
- [17. Signals and graceful shutdown](#17-signals-and-graceful-shutdown)

**Part V — Memory and resources**
- [18. Manual memory management and ownership](#18-manual-memory-management-and-ownership)

**Part VI — Engineering practice**
- [19. Build systems and linking](#19-build-systems-and-linking)
- [20. Debugging concurrency: the tools](#20-debugging-concurrency-the-tools)
- [21. Measuring: does it actually balance?](#21-measuring-does-it-actually-balance)

**Part VII — Labs**
- [22. Full bug inventory](#22-full-bug-inventory)
- [23. Suggested learning path](#23-suggested-learning-path)

**Part VIII — Self-quiz**
- [24. Viva questions](#24-viva-questions)

---

# Part I — Foundations

## 1. Why this project exists

A modern CPU has several **cores**. Each core can run one instruction stream at a time.
If your program does all its work in one stream, you use one core and the other three sit
idle — on a 4-core machine you're wasting 75% of the hardware.

To use all of them you split work into **tasks** and run tasks on different cores
simultaneously. That immediately raises the question this whole project is about:

> **Given a new task and N cores, which core should run it?**

Answering badly gives you one core at 100% while others idle. Answering well spreads the
work. That decision procedure is a **load balancer**, and ours lives in
`find_best_cpu()` at `src/load_balancer.c:58`.

Note that the Linux kernel already does this for you. Our program is a *userspace*
load balancer: it deliberately overrides the kernel by pinning threads to specific cores,
so that we control (and can observe) the placement decision. That's the point — you learn
what the kernel is doing by doing it yourself, worse, and seeing why it's hard.

## 2. Process vs. thread vs. core

Three words that beginners blur together. They are different things.

| Term | What it is | Key property |
|---|---|---|
| **Process** | A running program: its own memory space, file descriptors, PID | Memory is **isolated** from other processes |
| **Thread** | An independent execution path *inside* a process | Threads in one process **share all memory** |
| **Core** | Physical hardware that executes instructions | The kernel decides which thread runs on which core |

The essential picture:

```
  Process (our cpu_balancer program)
  ┌──────────────────────────────────────────────────────┐
  │  SHARED: heap (malloc'd memory), globals, open files │
  │  ┌────────────┐ ┌────────────┐ ┌────────────┐        │
  │  │ main thread│ │  monitor   │ │ scheduler  │  ...   │
  │  │ own stack  │ │ own stack  │ │ own stack  │        │
  │  └────────────┘ └────────────┘ └────────────┘        │
  └──────────────────────────────────────────────────────┘
             ↓ kernel schedules threads onto ↓
  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐
  │ core 0 │ │ core 1 │ │ core 2 │ │ core 3 │
  └────────┘ └────────┘ └────────┘ └────────┘
```

**Each thread gets its own stack** (so local variables are private) but **all threads share
the heap** (so anything from `malloc` is visible to everyone). That single fact causes
every hard bug in Parts II.

### Our threads

Run `./cpu_balancer 4 20` and the process contains:

| Thread | Created at | Job |
|---|---|---|
| main | process start | parse args, submit tasks, wait for Ctrl+C (`src/main.c:51`) |
| monitor | `src/load_balancer.c:37` | every 500 ms, read `/proc/stat`, update per-core load |
| scheduler | `src/load_balancer.c:38` | pull a task off the queue, pick a core, launch it |
| task ×N | `src/load_balancer.c:162` | one short-lived thread per submitted task |

So with 20 tasks you may briefly have 20+ threads on 4 cores. Threads are cheap-ish but not
free — see [§14](#14-thread-per-task-vs-thread-pool).

## 3. Shared memory: the source of all trouble

Here is the whole problem in five lines. Two threads both run:

```c
counter++;          // looks like one step
```

The CPU actually does three steps: **load** `counter` into a register, **add** 1,
**store** it back. Now interleave two threads:

```
 time   Thread A              Thread B            counter in memory
  1     load  → reg=5                                    5
  2                           load  → reg=5              5
  3     add   → reg=6                                    5
  4                           add   → reg=6              5
  5     store                                            6
  6                           store                      6      ← should be 7!
```

One increment was lost. This is a **data race**: two threads access the same memory,
at least one writes, and there is no synchronization ordering them. In C, a data race is
**undefined behavior** — not merely "a wrong number", but the compiler is allowed to
assume it never happens and optimize accordingly.

**Our code has this exact bug.** `src/load_balancer.c:168`:

```c
lb->cpu_monitor->stats[cpu_id].active_tasks++;   // scheduler thread writes
```

while the monitor thread reads and writes the same `stats[]` array in `update_cpu_stats()`
(`src/cpu_stats.c:33`), and `find_best_cpu()` reads it at `src/load_balancer.c:64` —
none of it under a lock. → Lab **B6**.

The fix for shared mutable data is always one of three things:

1. **Don't share it** (give each thread its own copy).
2. **Don't mutate it** (read-only data is always safe to share).
3. **Synchronize** access — mutexes ([§5](#5-mutexes-mutual-exclusion)) or atomics ([§8](#8-race-conditions-and-atomics)).

## 4. pthreads: the API

POSIX threads. Everything is `pthread_*`, everything returns `0` on success and an
**errno-style positive number on failure** (note: *not* `-1` + `errno`, unlike most syscalls).

```c
pthread_t t;
pthread_create(&t, NULL, func, arg);   // start; func has signature void* (*)(void*)
pthread_join(t, &retval);              // block until it finishes, reclaim resources
pthread_detach(t);                     // "I'll never join this" — auto-cleanup on exit
```

**Join or detach — you must do exactly one.** A thread that is neither joined nor detached
leaks its stack and bookkeeping when it exits (a "zombie thread").

Our code:
- Worker tasks are **detached** (`src/load_balancer.c:166`) — fire and forget.
- Monitor and scheduler are **joined**, with a timeout, at `src/load_balancer.c:232` and `:245`
  using `pthread_timedjoin_np` (the `_np` suffix means "non-portable" — a GNU extension).

The `arg` passing idiom is worth staring at, because it's how every thread gets its input:

```c
// main.c:108 — allocate on the HEAP, not the stack
int* task_id = malloc(sizeof(int));
*task_id = i + 1;
submit_task(lb, cpu_task, task_id, priority);
// ... eventually cpu_task() does free(arg)   (main.c:42)
```

Why `malloc` instead of `&i`? Because `i` lives on main's stack and will have changed (or
vanished) by the time the new thread reads it. Heap memory outlives the loop iteration.
The cost is that **ownership** must now be tracked manually — see [§18](#18-manual-memory-management-and-ownership).

🔬 **Lab A1 — first contact.** Write a standalone `race.c`: 4 threads, each incrementing a
shared global 100 000 times, then print the total. Run it 10 times. You will rarely get
400 000. Then fix it with a mutex, and again with `__atomic_fetch_add`. Time all three
versions. This 30-line program teaches more than a chapter of reading.

---

# Part II — The concurrency toolkit

## 5. Mutexes (mutual exclusion)

A **mutex** is a lock. At most one thread holds it at a time. Code between lock and unlock
is a **critical section**, and it executes as if it were atomic with respect to other
threads that use the same mutex.

```c
pthread_mutex_lock(&queue->mutex);
/* critical section: only one thread here at a time */
pthread_mutex_unlock(&queue->mutex);
```

Two ways to create one:

```c
// static, zero-cost, for file-scope locks
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;   // src/logger.c:9

// dynamic, for locks inside malloc'd structs
pthread_mutex_init(&queue->mutex, NULL);                        // src/task_queue.c:20
pthread_mutex_destroy(&queue->mutex);                           // src/task_queue.c:90
```

**Rules that matter**

- A mutex protects **data**, not code. Write down (in a comment) which fields each mutex
  guards. Our code doesn't, and that's why bug **B7** exists.
- **Every** access to guarded data must take the lock — readers included. A reader that
  skips the lock can observe a half-written value.
- Default pthread mutexes are **not recursive**: locking one you already hold deadlocks
  the thread against itself. This is exactly bug **B4**.
- Hold locks for as short a time as possible; never do I/O or `sleep` inside one if you can
  avoid it.

**Our mutexes**

| Mutex | Declared | Intended to guard |
|---|---|---|
| `queue->mutex` | `include/task_queue.h:12` | `tasks[]`, `size`, `front`, `rear` |
| `active_tasks_mutex` | `src/load_balancer.c:10` | `total_active_tasks` |
| `log_mutex` | `src/logger.c:9` | the log `FILE*` (keeps lines from interleaving) |

Notice there is **no mutex for `CPUStats`** — that's the missing one.

## 6. Condition variables and the predicate loop

A mutex answers "may I touch this data?". A **condition variable** answers a different
question: *"the data isn't in the state I need — how do I wait without burning CPU?"*

The naive alternative is a **busy-wait**:

```c
while (queue->size == 0) { }   // spins, eats 100% of a core, terrible
```

A condition variable lets the thread **sleep** until someone says the situation changed:

```c
pthread_cond_wait(&queue->not_empty, &queue->mutex);
```

`pthread_cond_wait` does three things atomically, and understanding this is the single most
important thing in this section:

1. **unlocks** the mutex (so another thread can actually change the data),
2. **sleeps** until signalled,
3. **re-locks** the mutex before returning.

The wake-up side:

```c
pthread_cond_signal(&queue->not_empty);     // wake ONE waiter
pthread_cond_broadcast(&queue->not_empty);  // wake ALL waiters
```

### The `while` loop is mandatory

```c
while (queue->size == 0)                          // ← while, never if
    pthread_cond_wait(&queue->not_empty, &queue->mutex);
```

Two independent reasons:

- **Spurious wakeups.** POSIX explicitly permits `pthread_cond_wait` to return without any
  signal. Your code must tolerate it.
- **Stolen wakeups.** Between the signal and your thread actually re-acquiring the mutex,
  a *third* thread can run and consume the item. You wake up to an empty queue again.

So: a condition variable carries **no state and no memory**. Signalling when nobody is
waiting is a no-op that is simply lost. The *real* state is the predicate (`size == 0`);
the condvar is only a notification mechanism. Re-test the predicate every time.

**In our code:** `src/task_queue.c:31` and `:49` both use `while` correctly. 👍

**But** — the predicate is incomplete. `dequeue_task` waits for "queue non-empty" and
nothing else. At shutdown the queue is empty and stays empty, so the scheduler thread sleeps
in `pthread_cond_wait` **forever**; the `if (!lb->running)` check at `src/load_balancer.c:145`
is never reached, and the `pthread_timedjoin_np` at `:245` always times out. That is bug **B5**,
and it's why the shutdown path is littered with retries and timeouts — they are compensating
for a missing predicate term. The correct shape is:

```c
while (queue->size == 0 && !queue->shutdown)
    pthread_cond_wait(&queue->not_empty, &queue->mutex);
if (queue->shutdown) { pthread_mutex_unlock(&queue->mutex); return NULL; }
```

🔬 **Lab A2.** Change the `while` on `src/task_queue.c:49` to an `if`, then submit tasks from
two threads at once and watch it dequeue a `NULL`/garbage pointer. Change it back. Now you
will never forget why it's a `while`.

## 7. The producer–consumer / bounded buffer pattern

This is the classic OS textbook problem, and `src/task_queue.c` is a textbook
implementation of it. Learn this file properly — the pattern reappears everywhere
(thread pools, pipelines, message queues, Go channels, Java `BlockingQueue`).

**The setup.** A fixed-capacity buffer. Producers add, consumers remove.
Producers must block when it's **full**; consumers must block when it's **empty**.

**The circular buffer.** A fixed array plus two indices that wrap around with `%`:

```c
queue->rear  = (queue->rear  + 1) % queue->capacity;   // task_queue.c:35
queue->front = (queue->front + 1) % queue->capacity;   // task_queue.c:54
```

This gives O(1) enqueue and dequeue with zero allocation per operation, and no shifting of
elements. `size` is tracked separately so you can distinguish "full" from "empty" (both of
which would otherwise look like `front == rear`).

```
capacity 8, size 4:
   idx:  0    1    2    3    4    5    6    7
       [ .  ][ .  ][ T1 ][ T2 ][ T3 ][ T4 ][ .  ][ .  ]
                     ▲                        ▲
                   front                     rear+1
```

**Two condition variables, not one.** `not_empty` wakes consumers, `not_full` wakes
producers. Using a single condvar for both would work only with `broadcast`, and would wake
threads that can't make progress (the "thundering herd").

Our producer/consumer roles:
- **Producer** = the main thread, calling `submit_task` → `enqueue_task` (`src/main.c:114`).
- **Consumer** = the scheduler thread, calling `dequeue_task` (`src/load_balancer.c:142`).

### The gap: priority is stored but never used

`Task` carries a `priority` field (`include/task.h:23`), `main.c:112` assigns a random one,
and the README advertises *"Priority-based ordering"*. **The queue is plain FIFO.** Nothing
in `task_queue.c` ever reads `priority`. A `PRIORITY_CRITICAL` task queued behind ten
`PRIORITY_LOW` ones waits for all ten.

This is the single biggest functional gap in the project, and also the best learning
opportunity — see Lab **C1** and [§11](#11-priorities-starvation-and-aging).

## 8. Race conditions and atomics

A **race condition** is when the result depends on timing between threads. A **data race**
(unsynchronized concurrent access, ≥1 writer) is the specific, always-undefined-behavior kind.

For a single integer, a full mutex is overkill. **Atomic operations** are single machine
instructions that can't be interrupted mid-way:

```c
task->task_id = __atomic_fetch_add(&next_task_id, 1, __ATOMIC_SEQ_CST);   // src/task.c:11
```

This is correct in our code and worth understanding. `__ATOMIC_SEQ_CST` is *sequentially
consistent* ordering — the strongest and easiest to reason about: all threads see all
sequentially-consistent operations in one consistent global order. Weaker orderings
(`ACQUIRE`, `RELEASE`, `RELAXED`) are faster but demand real expertise; default to `SEQ_CST`
until you can prove you need less.

C11 also provides the portable spelling, which is what you'd use in new code:

```c
#include <stdatomic.h>
static atomic_int next_task_id = 0;
int id = atomic_fetch_add(&next_task_id, 1);
```

### `volatile` is NOT a threading tool

`src/main.c:10` has `static volatile int running = 1;`. This is correct **here** and for one
narrow reason: `running` is written by a **signal handler**, and `volatile` stops the
compiler caching it in a register across the `while (running)` loop at `main.c:129`.
(Strictly, the portable type for this is `volatile sig_atomic_t`.)

But `volatile` provides **no atomicity and no memory ordering**. It does *not* make
`counter++` safe across threads. If you remember one thing: *`volatile` is for memory that
changes outside the program's control (hardware registers, signal handlers), not for
inter-thread synchronization.* Use mutexes or atomics for that.

### Our races

| Where | What |
|---|---|
| `src/load_balancer.c:168` | `stats[cpu_id].active_tasks++` unsynchronized vs. monitor thread |
| `src/load_balancer.c:64` | `find_best_cpu` reads `stats[]` while monitor rewrites it |
| `src/load_balancer.c:260` | reads `total_active_tasks` while holding the **wrong mutex** (`queue->mutex` instead of `active_tasks_mutex`) |
| `src/main.c:36` | `rand()` in `cpu_task` — `rand()` keeps global state and is **not thread-safe**; use `rand_r()` with a per-thread seed |
| `src/load_balancer.c:36` | `lb->running` written/read from several threads with no atomic |

## 9. Deadlock

**Deadlock** = two or more threads each waiting for something only the other can release;
nobody ever proceeds. Coffman's four necessary conditions — break any one and deadlock is
impossible:

1. **Mutual exclusion** — a resource is held exclusively.
2. **Hold and wait** — a thread holds one resource while requesting another.
3. **No preemption** — resources can't be forcibly taken back.
4. **Circular wait** — a cycle exists in the "waits for" graph.

The standard practical defence is to break #4: **define a global lock ordering** and have
every thread acquire locks in that order.

### Our deadlock is the simplest possible kind

`cancel_pending_tasks()` at `src/load_balancer.c:184`:

```c
pthread_mutex_lock(&lb->task_queue->mutex);   // :185  acquires the lock
while (lb->task_queue->size > 0) {
    Task* task = dequeue_task(lb->task_queue); // :188  dequeue_task locks it AGAIN
```

`dequeue_task` starts with `pthread_mutex_lock(&queue->mutex)` (`src/task_queue.c:47`).
The mutex is already held **by this same thread**, and default pthread mutexes are not
recursive → the thread blocks waiting for itself. **Self-deadlock.** The function can never
return. Bug **B4**.

Three ways to fix it, in order of preference:

1. **Extract an unlocked helper.** Write `dequeue_task_locked()` that assumes the caller
   holds the mutex; have both `dequeue_task()` (lock → call → unlock) and
   `cancel_pending_tasks()` use it. This is the idiomatic C solution.
2. Don't hold the lock in `cancel_pending_tasks`; just loop on `dequeue_task`.
3. Use `PTHREAD_MUTEX_RECURSIVE`. Works, but usually a sign of muddled design — it lets you
   stop thinking about who owns what.

🔬 **Lab A3.** Run the program (after fixing B2 so it starts), press Ctrl+C, and attach
`gdb -p $(pidof cpu_balancer)`, then `thread apply all bt`. You will see a thread parked in
`__lll_lock_wait` inside `cancel_pending_tasks`. Reading a real deadlock backtrace is a
skill worth having.

---

# Part III — Scheduling and load balancing

## 10. CPU scheduling theory

Vocabulary you need, and where it shows up here:

- **Preemptive vs. cooperative** — Linux preempts: a timer interrupt can suspend a thread
  mid-execution. Our tasks never yield voluntarily; they're preempted.
- **Time slice / quantum** — how long a thread runs before the scheduler reconsiders.
- **Context switch** — saving one thread's registers and loading another's. Costs ~1–5 µs
  directly, and much more indirectly via cache pollution.
- **Throughput** (tasks/sec) vs. **latency** (time for one task) vs. **fairness**. You
  cannot maximize all three; every scheduler picks a trade-off.
- **CPU-bound vs. I/O-bound.** Our `cpu_task` (`src/main.c:24`) is pure CPU-bound: a spin
  loop of floating-point adds. It never blocks, so it uses its entire quantum.

**Classic algorithms** (know these for exams): FCFS, SJF/SRTF, Round Robin, Priority
scheduling, Multilevel Feedback Queue. Our task queue is **FCFS** today; adding priority
makes it **priority scheduling**; adding aging makes it approximately **MLFQ**.

**What Linux actually uses:** CFS (Completely Fair Scheduler) historically, now EEVDF in
recent kernels. CFS tracks each thread's `vruntime` (virtual runtime, weighted by nice
value) in a red-black tree and always runs the thread with the smallest `vruntime` — the
one that has had the least CPU so far. That's how it achieves fairness without fixed
time slices. Worth a paragraph in your report as a comparison with our approach.

**The crucial framing for this project:** the kernel is already load-balancing threads
across cores. By calling `pthread_setaffinity_np` we *forbid* it from migrating our task
threads, and take responsibility ourselves. If our placement is worse than the kernel's, our
program is a pessimization. Being able to *measure* that ([§21](#21-measuring-does-it-actually-balance))
is what turns this from a toy into a real project.

## 11. Priorities, starvation, and aging

Our four levels (`include/task.h:7`):

```c
PRIORITY_LOW = 0, PRIORITY_MEDIUM = 1, PRIORITY_HIGH = 2, PRIORITY_CRITICAL = 3
```

(Note `main.c:112` does `rand() % 3`, so `PRIORITY_CRITICAL` is never generated. Minor bug **B15**.)

**Starvation** is the hazard of any priority scheme: if high-priority work keeps arriving,
low-priority tasks may never run at all. The standard fix is **aging** — a task's effective
priority rises with its waiting time:

```c
effective_priority = base_priority + (now - enqueue_time) / aging_interval;
```

This bounds the wait for any task while still favouring important work. `Task` already
records `create_time` (`src/task.c:20`), so you have everything you need to implement it.

**Priority inversion** — worth knowing even though our code can't hit it: a high-priority
thread waits on a lock held by a low-priority thread, which is itself preempted by a
medium-priority thread. The high-priority thread is effectively blocked by the medium one.
This famously nearly killed the Mars Pathfinder mission in 1997. Fixes: **priority
inheritance** (the lock holder temporarily inherits the waiter's priority — pthreads
supports this via `pthread_mutexattr_setprotocol`) or **priority ceiling**.

🔬 **Lab C1 — the flagship exercise.** Make the queue actually honour priority. Two routes:

- **Easy:** keep the array; on dequeue, scan all `size` entries for the highest priority.
  O(n) per dequeue but ~15 lines and obviously correct. Fine for `max_tasks = 10`.
- **Proper:** replace the circular buffer with a **binary heap** (priority queue). O(log n)
  insert and extract-max. Order by `(priority, create_time)` so equal priorities stay FIFO.

Then add aging on top and demonstrate with a test that a LOW task submitted first still
completes within a bounded time under a flood of HIGH tasks. This single lab covers data
structures, scheduling theory, and starvation in one go.

## 12. Load balancing algorithms

The taxonomy — place ours in it:

| Strategy | How it decides | Trade-off |
|---|---|---|
| **Round robin** | next core, cyclically | trivial; ignores actual load |
| **Least loaded** | core with lowest current load | needs load info; can be stale |
| **Least connections / fewest tasks** | core running fewest tasks | good proxy when tasks are uniform |
| **Random** | pick uniformly at random | surprisingly decent, zero state |
| **Power of two choices** | sample 2 at random, take the better | near-optimal, O(1), very robust |
| **Work stealing** | idle cores steal from busy cores' queues | excellent; used by Go, Java FJP, TBB |

**Ours is least-loaded with a task-count penalty** (`src/load_balancer.c:58`):

```c
effective_load = current_usage;
if (enable_load_prediction)
    effective_load = (current_usage + predicted_load) / 2;   // blend measured & predicted
effective_load += active_tasks * 10;                          // +10% per running task
```

Three things to critique in your report (this is exactly the kind of analysis a good project
report contains):

1. **The magic number 10.** Why 10 and not 5 or 25? It's an unjustified guess that a task
   costs 10 percentage points of a core. Make it a config parameter and measure the effect.
2. **Stale data.** `current_usage` is refreshed every 500 ms
   (`monitoring_interval_ms`, `src/main.c:89`). Tasks are submitted every 100 ms
   (`src/main.c:123`). So five consecutive tasks can be placed using *identical* stale
   readings and all pile onto the same "least loaded" core — the classic **herd effect**.
   The `active_tasks` term is what's meant to prevent this, which is why bug **B6**
   (it's never decremented) is so damaging.
3. **Push-only.** Once placed, a task is pinned for life (`src/load_balancer.c:163`).
   There's no **migration** if the balance later goes bad. `rebalance_threshold` exists in
   the config (`include/config.h:15`) but **nothing in the codebase ever reads it** — the
   entire rebalancing feature is unimplemented. Lab **C2**.

**Push vs. pull.** Push = the scheduler assigns work to a core (ours). Pull = idle cores
fetch work themselves (work stealing). Pull scales better because the decision is
distributed with no central bottleneck.

🔬 **Lab C2.** Implement one alternative strategy — round-robin is 5 lines, power-of-two-choices
is 8 — put the choice behind a config field, and benchmark all three ([§21](#21-measuring-does-it-actually-balance)).
Comparative numbers are what make a project report convincing.

## 13. Load prediction

`predict_cpu_load()` (`src/cpu_stats.c:92`) computes a **simple moving average** (SMA) of
the usage history, and `find_best_cpu` blends it 50/50 with the current reading. The idea:
if a core has been busy recently it will probably be busy next, so don't put work there.

**The bug** (**B9**): the loop runs `for (i = 0; i < cpu->history_index; i++)`. But
`history_index` is a *circular* write cursor that wraps to 0
(`src/cpu_stats.c:71`). Once you've taken `load_history_size` samples it wraps to 0 and the
loop body never executes — the function silently returns `cpu->current_usage`, and the
prediction feature quietly stops existing. Nothing warns you. This is a great example of a
bug that never crashes, never logs, and just makes your feature not work.

Fix: track a separate `samples_filled` count and average over
`min(samples_filled, load_history_size)` entries.

**Better predictors to try:**

- **EWMA** (exponentially weighted moving average) — one line, no history array needed,
  and it's what the Linux kernel actually uses for load tracking:
  ```c
  predicted = alpha * current + (1 - alpha) * predicted;   // alpha ≈ 0.3
  ```
  Larger `alpha` = more responsive; smaller = smoother. Perfect small experiment: plot
  predicted vs. actual for several `alpha` values.
- **Weighted moving average** — linearly weight recent samples more heavily.
- **Linear regression on the last k samples** — captures *trend* (rising vs. falling), not
  just level.

🔬 **Lab C3.** Fix the SMA, add EWMA behind a config flag, log `predicted` and the *next*
`actual` to a CSV, and compute mean absolute error for each predictor. A graph of that is
an excellent report figure.

## 14. Thread-per-task vs. thread pool

We create one thread per task (`src/load_balancer.c:162`) and destroy it when the task ends.
That's the simplest model and it's fine for 20 long tasks. It's terrible at scale:

- Thread creation costs ~10–30 µs and ~8 MB of *virtual* address space for the stack.
- 10 000 tasks = 10 000 create/destroy cycles.
- Nothing bounds concurrency: submit 1000 tasks and you get 1000 runnable threads on 4
  cores, which means massive context-switch thrash — the machine spends its time switching
  rather than working.

A **thread pool** fixes all three: create W worker threads once (typically W = number of
cores), and have each loop forever pulling tasks from the shared queue. Task submission
becomes a queue push with no thread creation at all. Concurrency is bounded by W by
construction.

Notice you **already have every piece**: the thread-safe blocking queue is `task_queue.c`,
and the worker loop is essentially `scheduler_thread_func`. Converting is mostly deletion.

🔬 **Lab D1 — the most valuable refactor here.** Replace thread-per-task with a pool of
`num_cores` workers, each pinned to its own core at startup (pin the *worker*, once, instead
of pinning every task). Benchmark 1000 short tasks before and after. Expect a large,
easily-demonstrable win — and note that pinning workers once also fixes bug **B8**
(the affinity race) for free.

---

# Part IV — Linux systems interfaces

## 15. `/proc/stat` and CPU accounting

`/proc` is a **virtual filesystem**: files that aren't on disk but are generated by the
kernel when you read them. It's the standard way userspace inspects kernel state, and
`top`, `htop`, and `vmstat` all read the same file we do.

```
$ cat /proc/stat
cpu   2255 34 2290 22625563 6290 127 456 0 0 0   ← aggregate (we skip this line)
cpu0  1132 34 1441 11311718 3675  61 210 0 0 0
cpu1  1123  0  849 11313845 2614  66 246 0 0 0
```

Columns, in order — memorize these, they're standard viva material:

| # | Name | Meaning |
|---|---|---|
| 1 | `user` | user-mode time |
| 2 | `nice` | user-mode time for niced (low-priority) processes |
| 3 | `system` | kernel-mode time |
| 4 | `idle` | doing nothing |
| 5 | `iowait` | idle *while* an I/O was outstanding (unreliable — don't trust it) |
| 6 | `irq` | servicing hardware interrupts |
| 7 | `softirq` | servicing software interrupts |
| 8 | `steal` | time the hypervisor gave to another VM (0 on bare metal) |

The units are **jiffies** (clock ticks), where `sysconf(_SC_CLK_TCK)` is usually 100/sec.

**These are cumulative counters since boot**, so a single reading tells you nothing about
current load. You must take **two readings and diff them** — this is the key insight, and
`src/cpu_stats.c:53-67` does it correctly:

```c
total_delta = total_now - total_prev;
idle_delta  = idle_now  - idle_prev;
usage = 100.0 * (1.0 - (double)idle_delta / total_delta);
```

Three problems in our implementation:

- **B10 — uninitialized first reading.** `init_cpu_monitor` (`src/cpu_stats.c:21`) sets
  `current_usage`, `usage_history`, `history_index`, `active_tasks` — but **never** the eight
  `*_time` fields. The first `update_cpu_stats` subtracts garbage from a real value.
  Fix: `memset` the struct (or `calloc`) and discard the first sample.
- **B11 — division by zero.** If two reads happen within the same jiffy, `total_delta == 0`
  and you get `inf`/`NaN`, which then poisons `find_best_cpu`'s comparisons forever
  (every comparison with NaN is false). Guard it.
- **B12 — unchecked `sscanf` and `fgets`.** `sscanf` returns how many fields it converted
  (`src/cpu_stats.c:49`); the return value is ignored, so a short line leaves variables
  uninitialized. Always check.

Also note: we read the first `num_cpus` lines in file order and assume they are cpu0..cpuN.
If the user asks for 2 cores on an 8-core box, we monitor cpu0 and cpu1 only — fine, but
worth a comment.

🔬 **Lab E1.** Write a 40-line `mytop.c` that reads `/proc/stat` twice a second and prints a
per-core usage bar. Compare against `htop` running side by side. Once your numbers match
`htop`'s, you genuinely understand this file.

## 16. CPU affinity, cache locality, NUMA

**Affinity** = restricting which cores a thread may run on. Linux exposes it through a
bitmask type, `cpu_set_t`, with macros:

```c
cpu_set_t cpuset;
CPU_ZERO(&cpuset);                 // clear all bits
CPU_SET(cpu_id, &cpuset);          // allow only this core
pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);   // src/load_balancer.c:163
```

These are GNU extensions, which is why `CMakeLists.txt:17` defines `_GNU_SOURCE`. (Also:
`src/load_balancer.c:8` includes `<bits/cpu-set.h>` directly — that's a glibc internal
header you should never include; `<sched.h>` is the correct one and is already included.
Bug **B14**.)

**Why pin at all?** *Cache locality.* Each core has its own L1/L2 cache. A thread builds up
useful data there; migrating it to another core means every access misses and must be
re-fetched — an L1 hit is ~1 ns, main memory ~100 ns. That's a 100× penalty per miss. Pinning
preserves the **cache warmth**.

**Why not pin?** You override the kernel scheduler, which usually knows better, has global
information, and already implements cache-aware placement. Over-pinning can leave cores idle
while pinned threads queue up on one core.

**NUMA** (Non-Uniform Memory Access): on multi-socket machines each CPU has *its own* memory
bank; reading another socket's memory is significantly slower. So you want a thread on the
same node as its data (`numactl`, `libnuma`, `/sys/devices/system/node/`). Worth a paragraph
in your report; not something this project needs to implement.

**Bug B8 — the affinity race.** `src/load_balancer.c:162-163`:

```c
pthread_create(&task->thread, NULL, task_wrapper, task);      // thread starts running NOW
pthread_setaffinity_np(task->thread, sizeof(cpu_set_t), &cpuset);  // ...pinned afterwards
```

Between those two lines the new thread is already runnable and the kernel may have started
it on any core. Your careful placement decision is applied late, after the fact. Two clean
fixes: set affinity in the **attributes** before creating
(`pthread_attr_setaffinity_np`), or have the thread pin **itself** as its very first action
via `sched_setaffinity(0, ...)`.

🔬 **Lab E2.** Run `./cpu_balancer 4 20`, and in another terminal watch
`watch -n0.5 'ps -L -o pid,tid,psr,comm -p $(pidof cpu_balancer)'`. The `psr` column is the
core each thread is on. Confirm the pinning works. Then comment out the
`pthread_setaffinity_np` call and watch threads wander between cores.

## 17. Signals and graceful shutdown

A **signal** is an asynchronous notification. `SIGINT` is what Ctrl+C sends. The handler
runs by **interrupting whatever the process was doing**, on some thread of the kernel's
choosing.

**Async-signal-safety** is the rule that trips everyone up. A handler may only call
functions from a specific safe list (`write`, `_exit`, `sigaction`, ...). It may **not** call
`printf`, `malloc`, `free`, or most pthread functions. Why: if the interrupted thread was
midway through `malloc`'s internal locking, and your handler calls `malloc` too, you
deadlock inside the allocator — irreproducibly, on maybe 1 run in 1000.

**Our handler breaks this rule thoroughly** (`src/main.c:12`):

```c
void signal_handler(int signum) {
    printf(...);            // NOT safe
    log_message(...);       // NOT safe — fprintf + pthread_mutex_lock
    stop_load_balancer(lb); // very much NOT safe — mutexes, malloc, pthread_join, usleep
}
```

Bug **B13**. **The standard correct pattern** is: the handler does the absolute minimum,
and the real work happens on a normal thread.

```c
static volatile sig_atomic_t shutdown_requested = 0;

void handler(int sig) { (void)sig; shutdown_requested = 1; }   // that's ALL it does

// in main's loop:
while (!shutdown_requested) sleep(1);
stop_load_balancer(lb);        // full cleanup on a normal thread — safe
```

Also prefer **`sigaction()` over `signal()`** — `signal()`'s semantics vary across
platforms (whether the handler is reset after firing, whether syscalls are restarted);
`sigaction` is explicit and portable.

**Signals and threads.** A signal is delivered to *any* thread that hasn't blocked it. If
several threads could handle it, you get non-determinism. The idiom is: block the signal in
all threads, then have one dedicated thread call `sigwait()`. Our code does part of this —
`src/load_balancer.c:134-139` blocks `SIGINT` in the scheduler thread — but the monitor and
task threads never do, so delivery is still arbitrary.

**The shutdown sequence** the README describes (catch → clear flag → cancel pending → drain
active → join threads → free) is the right *design*. The implementation just doesn't work,
because of B4 (deadlock in cancel), B5 (scheduler stuck in `cond_wait`), and B13.
Fixing those three makes the timeouts and triple-broadcast retries at
`src/load_balancer.c:211-217` unnecessary — a good lesson that **defensive hacks are usually
a symptom of a design bug elsewhere**.

🔬 **Lab E3.** Rewrite the shutdown path properly: flag-only handler, a `shutdown` field in
`TaskQueue` folded into both condvar predicates, `dequeue_task` returning `NULL` on
shutdown, and clean joins with no timeouts. Verify it exits within 100 ms of Ctrl+C, with
zero leaks under Valgrind.

---

# Part V — Memory and resources

## 18. Manual memory management and ownership

C has no garbage collector. Every `malloc` needs exactly one `free` — no more, no fewer.
The discipline that makes this tractable is **ownership**: at every moment, exactly one
piece of code is responsible for freeing each allocation. Write the ownership rule in a
comment at every hand-off; ours doesn't, and that's why the bugs below exist.

**The four failure modes**

| Failure | Consequence |
|---|---|
| **Leak** — never freed | memory grows without bound |
| **Double free** — freed twice | heap corruption, often exploitable |
| **Use-after-free** — used after freeing | reads garbage, or worse |
| **Missing NULL check** on `malloc` | crash under memory pressure |

**Our ownership map**, and where it's broken:

```
config       : created in main.c:81, freed in main.c:139
               ✗ ALSO freed by cleanup_cpu_monitor (cpu_stats.c:169) → DOUBLE FREE (B16)
task->args   : malloc'd in main.c:108, freed by cpu_task in main.c:42
               ✗ leaked if the task is cancelled instead of run (load_balancer.c:148, :191)
Task struct  : created in create_task (task.c:8)
               ✗ NEVER freed after successful completion — task_wrapper (load_balancer.c:111)
                 doesn't free it → LEAK of one Task per task run (B17)
LoadBalancer : malloc'd in init_load_balancer (load_balancer.c:15)
               ✗ never freed; no cleanup_load_balancer() exists (main.c:138 is commented out)
monitor/queue: ✗ leaked on the error path at load_balancer.c:27-29
```

**Rules to internalize**

1. Free at the same layer that allocated, wherever possible.
2. Set pointers to `NULL` after freeing — turns a silent use-after-free into a clean crash.
   `cleanup_cpu_monitor` does this well (`src/cpu_stats.c:158`).
3. Check every `malloc` — `init_cpu_monitor` checks the outer allocations but **not** the
   per-CPU `usage_history` at `src/cpu_stats.c:24`, then immediately `memset`s it at `:27`.
4. `free(NULL)` is defined and safe. No need to guard it.
5. A **cleanup function for every init function.** We have `cleanup_task_queue`,
   `cleanup_cpu_monitor`, `cleanup_logger` — but no `cleanup_load_balancer`, and of the
   three that exist, `main` calls **none**.

🔬 **Lab F1.** `valgrind --leak-check=full --show-leak-kinds=all ./cpu_balancer 2 5`.
Write down the number of leaked bytes. Fix the leaks above one at a time, re-running after
each, until it reports "All heap blocks were freed". Then run under
`-fsanitize=address,undefined` and fix whatever *that* finds. This is the single best
exercise in this document for building C instincts.

---

# Part VI — Engineering practice

## 19. Build systems and linking

**The pipeline:** preprocess (`#include`, macros) → compile (`.c` → `.o`) → link (`.o` +
libraries → executable).

**Header guards** — every one of our headers has them (`include/task.h:1`):

```c
#ifndef TASK_H
#define TASK_H
...
#endif
```

Without these, a header included twice yields duplicate type definitions and a compile error.

**Linking.** `CMakeLists.txt:61` links `Threads::Threads` (pthread), `m` (libm, for math),
`rt` (POSIX realtime, for `clock_gettime` on older glibc). `-pthread` at `:14` is a compile
*and* link flag: it defines `_REENTRANT` and pulls in the right library.

**Feature-test macros.** `_GNU_SOURCE` (`CMakeLists.txt:17`) unlocks GNU extensions —
`CPU_SET`, `pthread_setaffinity_np`, `pthread_timedjoin_np`. Without it those don't exist
and you get implicit-declaration errors. Feature-test macros must be defined **before** any
system header is included, which is why it belongs in the build system rather than in a `.c`.

**pkg-config** (`CMakeLists.txt:23`) queries installed libraries for their include and link
flags rather than hard-coding paths.

### Build issues in the repo

- **B18 — `json-c` is a phantom dependency.** `src/config.c:4` includes `<json-c/json.h>`
  and `CMakeLists.txt:23` makes it a `REQUIRED` package, but **no json-c function is ever
  called** — `load_config` (`src/config.c:24`) is a stub that returns the defaults. On a
  machine without json-c installed, the build fails for a library the program doesn't use.
  Either implement config-file parsing or drop the dependency.
- **B19 — the config file is not valid JSON.** `config/cpu_balancer.conf:2` has a stray
  `config/cpu_balancer.conf:` line inside the file, before the `{`. Any parser rejects it.
- **B20 — `Makefile` oddities.** It opens with `#!/bin/bash` (meaningless in a Makefile);
  the `docs` target invokes a `make docs` that CMake never defines; the `test` target runs
  `ctest` although `CMakeLists.txt` has no `enable_testing()` and no tests at all.
- **B21 — `config->num_cpus` is never set** by `init_default_config` (`src/config.c:6`).
  `main.c:91` happens to set it afterwards, but anyone calling `load_config` gets an
  uninitialized value used directly as an array length at `src/cpu_stats.c:12`.

🔬 **Lab G1.** Either implement `load_config()` with json-c and fix the `.conf` file, or
remove json-c entirely and parse a simple `key=value` file with `fgets`+`sscanf`. Either way
the build should stop requiring a library it doesn't use. Add validation: reject
`num_cpus > sysconf(_SC_NPROCESSORS_ONLN)`, negative intervals, etc.

## 20. Debugging concurrency: the tools

Concurrency bugs are timing-dependent, so "run it and see" is a weak method. Learn the tools:

| Tool | Command | Finds |
|---|---|---|
| **ThreadSanitizer** | `gcc -fsanitize=thread -g` | data races — **start here**, it will find B6/B7 immediately |
| **AddressSanitizer** | `gcc -fsanitize=address -g` | use-after-free, overflows, leaks |
| **UBSan** | `gcc -fsanitize=undefined` | UB: overflow, bad shifts, misaligned access |
| **Valgrind memcheck** | `valgrind --leak-check=full` | leaks, uninitialized reads (finds B10) |
| **Helgrind** | `valgrind --tool=helgrind` | lock-order violations, races |
| **GDB** | `gdb -p PID`, `thread apply all bt` | where every thread is stuck — finds B4 |
| **strace** | `strace -f -e trace=futex` | syscall-level view of blocking |
| **perf** | `perf stat -e context-switches,cache-misses` | is the balancing actually helping? |

Note that ASan and TSan cannot be combined in one build; make two.

Two habits that matter more than the tools:

- **Compile with `-Wall -Wextra -Wpedantic` and fix every warning.** The project already sets
  these (`CMakeLists.txt:14`) and currently emits one (unused parameter `lb` in
  `wait_for_tasks_completion`, `src/load_balancer.c:176` — itself a hint that the function
  ignores its argument and uses globals instead).
- **Reproduce races by perturbing timing.** Insert `usleep(rand() % 1000)` at suspicious
  points, or run under heavy load (`stress-ng --cpu 8`). A race that appears 1 run in 1000
  becomes 1 in 5.

🔬 **Lab G2.** Build with `-fsanitize=thread` and run `./cpu_balancer 4 10`. Copy the first
race report into a file, and for each one identify: which two lines, which thread, which
variable, and what the fix is. TSan's output is intimidating for about ten minutes and
indispensable forever after.

## 21. Measuring: does it actually balance?

A load balancer that isn't measured is just a hypothesis. Define your metrics up front:

- **Makespan** — wall-clock time from first submit to last completion. Lower is better.
- **Load imbalance** — the standard deviation of per-core utilization, or
  `(max_load - min_load) / mean_load`. Lower is better; this is *the* headline number.
- **Throughput** — tasks per second.
- **Task latency** — you already record `create_time`, `start_time`, `end_time`
  (`include/task.h:29-31`), so you can report queueing delay (start − create) and execution
  time (end − start) separately. Report p50 and p99, not just the mean — tail latency is
  where scheduling problems show up.
- **Context switches / cache misses** — from `perf stat`.

**Baselines to compare against** (a project report needs comparisons, not one number):

1. No balancer at all — just `pthread_create` per task, let the kernel place it. *This is
   the baseline you must beat.*
2. Round robin.
3. Random / power-of-two-choices.
4. Ours (least-loaded + prediction).

**Vary the workload,** because the ranking changes with it:
- uniform tasks vs. highly variable durations (heavy-tailed is the hard case),
- burst arrivals vs. steady arrivals,
- more tasks than cores vs. fewer,
- CPU-bound (current `cpu_task`) vs. I/O-bound (add a `sleep`-based task to see the
  difference — I/O-bound tasks release their core and change everything).

**Amdahl's law** belongs in the report: if a fraction *s* of the work is inherently serial,
maximum speedup on N cores is `1 / (s + (1-s)/N)`. Our scheduler thread is a serial
bottleneck — every task passes through one thread that takes one mutex. That caps
scalability regardless of core count, and is the theoretical argument for work stealing.

🔬 **Lab H1.** Add a `--benchmark` mode: submit K tasks, run to completion without waiting
for Ctrl+C, and print makespan, per-core utilization, imbalance, and latency percentiles as
CSV. Run every strategy × workload combination, plot the results. This is what turns the
project from "it runs" into "here is evidence it works".

---

# Part VII — Labs

## 22. Full bug inventory

Everything found by reading the code, roughly in the order you should fix it. The **Teaches**
column is why each one is worth your time.

### Blockers — the program doesn't work without these

| # | Location | Bug | Teaches |
|---|---|---|---|
| **B1** | `src/task_queue.c:29` | Stray debug `fprintf(stdout, "enqueue task")` on every enqueue | — |
| **B2** | `src/load_balancer.c:21-24` | Leftover debug `printf` + **`scanf`** inside `init_load_balancer` — **startup blocks waiting for a keypress** | — |
| **B4** | `src/load_balancer.c:185-188` | `cancel_pending_tasks` holds `queue->mutex` then calls `dequeue_task`, which locks it again → **self-deadlock** | §9 |
| **B5** | `src/task_queue.c:49` | `dequeue_task`'s predicate lacks a shutdown term → scheduler sleeps forever at shutdown; the `!lb->running` check at `load_balancer.c:145` is unreachable | §6 |
| **B13** | `src/main.c:12-21` | Signal handler calls `printf`, `log_message`, `stop_load_balancer` — none async-signal-safe | §17 |

### Correctness — it runs, but does the wrong thing

| # | Location | Bug | Teaches |
|---|---|---|---|
| **B6** | `src/load_balancer.c:168` | `active_tasks` incremented but **never decremented** → load estimate grows forever, balancing degrades to round-robin-ish nonsense. Also unsynchronized | §3, §8 |
| **B7** | `src/load_balancer.c:259-261` | Reads `total_active_tasks` under `queue->mutex` while writers use `active_tasks_mutex` — **wrong lock**, so no protection at all | §5 |
| **B9** | `src/cpu_stats.c:97` | SMA loops to `history_index`, which wraps to 0 → prediction silently stops working after `load_history_size` samples | §13 |
| **B10** | `src/cpu_stats.c:21-28` | The eight `*_time` fields are never initialized → first delta computed from garbage (uninitialized read = UB) | §15, §18 |
| **B11** | `src/cpu_stats.c:67` | No guard for `total_delta == 0` → division by zero → `NaN` poisons all later comparisons | §15 |
| **B8** | `src/load_balancer.c:162-163` | Affinity set **after** `pthread_create`; thread may already be running elsewhere | §16 |
| **B3** | `src/task_queue.c` (whole file) | Queue is **FIFO**; `priority` is stored but never used, despite the README claiming priority ordering | §7, §11 |
| **B22** | `src/load_balancer.c` | `rebalance_threshold`, `high/low_load_threshold`, `min_task_runtime_ms` are in the config but **never read anywhere** — features that exist only in the README | §12 |
| **B15** | `src/main.c:112` | `rand() % 3` never generates `PRIORITY_CRITICAL` | — |
| **B23** | `src/main.c:36` | `rand()` is not thread-safe; called from every task thread → use `rand_r()` | §8 |

### Resources

| # | Location | Bug | Teaches |
|---|---|---|---|
| **B16** | `src/cpu_stats.c:169` | `cleanup_cpu_monitor` frees `monitor->config`, which `main.c:139` also frees → **double free**. It also never frees `monitor` itself | §18 |
| **B17** | `src/load_balancer.c:111` | `task_wrapper` never calls `free_task` → one `Task` leaked per task executed | §18 |
| **B24** | `src/load_balancer.c:27-29` | Error path returns `NULL` without freeing `lb`, monitor, or queue | §18 |
| **B25** | `src/main.c:136-139` | `cleanup_task_queue`/`cleanup_cpu_monitor`/`cleanup_logger` are never called; `cleanup_load_balancer` doesn't exist | §18 |
| **B26** | `src/load_balancer.c:148`, `:191` | Cancelled tasks are freed but `task->args` leaks (only `cpu_task` frees it, and it never runs) | §18 |
| **B27** | `src/cpu_stats.c:24` | `malloc` for `usage_history` unchecked, then `memset` at `:27` | §18 |
| **B28** | `src/logger.c:23` | `ctime()` returns a shared static buffer — not thread-safe. (Our mutex saves us, but `ctime_r` is correct) | §8 |
| **B29** | `src/logger.c:12` | `fopen` failure unchecked; `log_message` then silently drops every message | — |

### Build and hygiene

| # | Location | Bug |
|---|---|---|
| **B18** | `src/config.c:4`, `CMakeLists.txt:23` | json-c is a `REQUIRED` dependency that is never used |
| **B19** | `config/cpu_balancer.conf:2` | Stray line makes the file invalid JSON |
| **B20** | `Makefile` | Bogus shebang; `docs`/`test` targets reference things CMake never defines |
| **B21** | `src/config.c:6` | `num_cpus` not initialized by `init_default_config` |
| **B14** | `src/load_balancer.c:8` | Includes glibc-internal `<bits/cpu-set.h>`; should rely on `<sched.h>` |
| **B30** | `src/load_balancer.c:176` | `wait_for_tasks_completion(lb)` ignores `lb` (compiler warning) and is never called |
| **B31** | `src/cpu_stats.c:144` | `#include <stdlib.h>` in the middle of the file |
| **B32** | `src/load_balancer.c:61` | `log_message(LOG_INFO, "%d", num_cpus)` — leftover debug logging on every scheduling decision |
| **B33** | `src/load_balancer.c:143` | `if (!task) continue;` spins hot if `dequeue_task` ever returns `NULL` |
| **B34** | everywhere | No tests at all |

## 23. Suggested learning path

Do these in order. Each stage is a commit, and each teaches the section listed.

**Stage 0 — get it running (½ day)** · §4
Remove the debug `scanf` (B2) and stray prints (B1, B32). Install json-c or remove it (B18).
Build, run `./cpu_balancer 2 5`, and read `cpu_balancer.log`. *Goal: see it work.*

**Stage 1 — make shutdown correct (1 day)** · §6, §9, §17
Fix B13 (flag-only handler), B5 (shutdown predicate in the queue), B4 (self-deadlock).
Delete the timeout/retry hacks at `load_balancer.c:211-217` once they're unnecessary.
*Goal: Ctrl+C exits cleanly in under 100 ms.*

**Stage 2 — fix the memory (1 day)** · §18
B16, B17, B24, B25, B26, B27. Write `cleanup_load_balancer()`. Run Valgrind until clean.
*Goal: "All heap blocks were freed — no leaks are possible."*

**Stage 3 — fix the races (1–2 days)** · §3, §5, §8
Build with TSan. Fix B6 (decrement `active_tasks` in `task_wrapper`, and guard the stats
with a new mutex), B7 (right lock), B23 (`rand_r`), B28 (`ctime_r`).
*Goal: TSan-clean under `./cpu_balancer 4 50`.*

**Stage 4 — make the monitoring truthful (1 day)** · §13, §15
B10, B11, B12, B9. Write Lab E1 (`mytop`) and check your numbers against `htop`.
*Goal: reported per-core usage matches `htop` within a couple of percent.*

**Stage 5 — implement the advertised features (2–3 days)** · §7, §11, §12
Priority queue + aging (Lab C1). Read `rebalance_threshold` and actually rebalance, or
delete it from the config and README (B22). Fix affinity ordering (B8).
*Goal: the README stops lying.*

**Stage 6 — thread pool (1–2 days)** · §14
Lab D1. Replace thread-per-task with `num_cores` pinned workers.
*Goal: 1000 short tasks measurably faster than before.*

**Stage 7 — measure (2–3 days)** · §12, §21
Benchmark mode, alternative strategies, workload variation, graphs.
*Goal: a results table and 3–4 figures you can defend.*

**Stage 8 — tests (1–2 days)** · §20
`enable_testing()` in CMake. Unit-test the queue (FIFO order, priority order, blocking when
full, shutdown wakeup), the `/proc/stat` parser (feed it a fixture file), and the predictor
(known input → known output). Stress-test with 10 000 tasks under TSan.
*Goal: `make test` is green and means something.*

---

# Part VIII — Self-quiz

## 24. Viva questions

Answer these out loud, without looking. If you can, you know the material.

**Threads and memory**
1. What exactly do two threads in one process share, and what do they not?
2. Why must `task_id` in `main.c:108` be `malloc`'d rather than passed as `&i`?
3. Why is `counter++` unsafe across threads when it's one line of C?
4. What does `volatile` guarantee? What does it *not* guarantee? Why is `main.c:10`'s use correct anyway?

**Synchronization**
5. Why must `pthread_cond_wait` be called in a `while` loop and never an `if`? Give both reasons.
6. What three things does `pthread_cond_wait` do atomically, and why must the unlock be part of it?
7. `signal` vs. `broadcast` — when is each correct?
8. Why does our queue need two condition variables rather than one?
9. What happens if you signal a condvar with no waiters?
10. Name the four Coffman conditions. Which one does lock ordering break?
11. Explain the self-deadlock in `cancel_pending_tasks` and give two different fixes.
12. When is an atomic better than a mutex? When is it worse?

**Scheduling and balancing**
13. What is the `find_best_cpu` cost function, and why the magic `* 10`?
14. Why can a "least loaded" balancer with stale readings place five tasks on the same core?
15. Difference between push and pull balancing. Why does work stealing scale better?
16. What is starvation, and how does aging bound it?
17. Explain priority inversion and priority inheritance.
18. How does Linux CFS decide what runs next, and how does that differ from ours?
19. State Amdahl's law and identify the serial bottleneck in this program.

**Systems**
20. Why must you read `/proc/stat` twice? What are the units?
21. What does the `iowait` column mean and why is it unreliable?
22. What is CPU affinity, and give one good reason to use it and one good reason not to.
23. Why is `printf` forbidden in a signal handler? Describe a concrete deadlock it can cause.
24. What is the correct signal-handler pattern, and why is `sigaction` preferred over `signal`?
25. Why does `CMakeLists.txt` define `_GNU_SOURCE`, and why must it come before any header?

**Memory**
26. Who owns `task->args`, and at which point does that ownership transfer?
27. Explain the double free between `cleanup_cpu_monitor` and `main`.
28. Why set a pointer to `NULL` after freeing it?

**Design**
29. Thread-per-task vs. thread pool — three concrete advantages of the pool.
30. How would you prove this balancer beats letting the kernel do it? Which metrics, which baselines?

---

## Further reading

- **Operating Systems: Three Easy Pieces** — Arpaci-Dusseau. Free online. Chapters 25–34
  (Concurrency) are the best introduction to everything in Part II.
- `man 7 pthreads`, `man 7 signal-safety`, `man 5 proc`, `man 2 sched_setaffinity`.
  The `signal-safety` page lists exactly which functions are legal in a handler.
- **The Linux Programming Interface** — Kerrisk. The reference for Part IV.
- **Programming with POSIX Threads** — Butenhof. Old, still the best book on pthreads.
