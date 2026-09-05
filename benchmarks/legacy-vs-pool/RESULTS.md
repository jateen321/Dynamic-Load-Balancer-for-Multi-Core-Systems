# Worker Pool vs. Thread-per-Task: Before/After Evidence

This compares the current worker-pool design against the design it replaced
— one `pthread_create` per task, joined via a registry — measured on the
same machine, same fixed CPU-bound synthetic task, same seeded workload.
Reproduce it yourself with `benchmarks/legacy-vs-pool/run.sh`.

**"Before" is commit `3c73725`** (the last commit prior to the worker-pool
rewrite in `32cb34e`), checked out into a throwaway git worktree by
`run.sh`. **"After" is the current tree.**

## Methodology

Both sides run the identical synthetic task: a fixed number of rounds of
real computation (not a wall-clock-gated spin). This distinction matters —
an earlier pass at this comparison used a task that spins until a target
*wall-clock* duration elapses, and it produced a misleadingly flattering
number for the old design: under that shape, oversubscribing a core doesn't
lengthen individual tasks much (each thread just keeps polling the clock
until real time has passed, however little CPU it actually got), so
unbounded concurrency looked artificially cheap. A fixed amount of work per
task means contention for the CPU shows up as real, measurable wall-clock
cost, which is what actually matters here.

`old_bench.c` and `new_bench.c` are otherwise-identical drivers built against
each design's public API (`submit_task`, `init_load_balancer`, etc. — this
part of the API is unchanged across the rewrite). `thread_overhead.c` and
`queue_overhead.c` isolate the raw per-task dispatch cost independent of any
workload or scheduling behavior. Machine used below: 4 cpuset-restricted
cores (`nproc` = 4), no CFS bandwidth throttling (`cpu.max` = `-1`).

## 1. Raw per-task dispatch overhead

| | Cost | Ratio |
|---|---|---|
| `pthread_create` + `pthread_join` (old design, per task) | ~0.056–0.067 ms | — |
| `CoreQueue` push + pop (new design, per task) | ~0.0001 ms | **~500–650×** |

This is the purely mechanical cost of the two designs' per-task dispatch
mechanism, independent of scheduling policy or workload shape.

## 2. Peak concurrent OS threads, identical 800-task workload

| Design | Peak threads (sampled from `/proc/<pid>/status` during the run) |
|---|---|
| Old (thread-per-task) | **761** |
| New (worker pool) | **7** (num_cpus workers + monitor + dispatcher + main) |

The old design creates one OS thread per task with no cap; the new design
never creates more than `num_cpus` worker threads, for the life of the
process, regardless of how many tasks are submitted.

## 3. Completion time at a scale where the old design doesn't drop tasks

At small submission bursts the old design completes everything, so a
straight completion-time/throughput comparison is meaningful:

| Tasks | Old: completion time | Old: throughput | New: completion time | New: throughput |
|---|---|---|---|---|
| 50  | 0.141–0.148 s | 337–354 tasks/s | 0.134–0.141 s | 354–375 tasks/s |
| 200 | 0.514–0.546 s | 366–389 tasks/s | 0.496–0.504 s | 397–403 tasks/s |

Comparable, with the new design modestly faster and — see below — far more
consistent run to run.

## 4. Correctness under load — the real finding

At larger, bursty submission counts, the two designs diverge sharply, and
not just on speed:

| Tasks | Old: tasks completed (5 runs) | Old: completion time (5 runs) | New: tasks completed (any run) | New: completion time |
|---|---|---|---|---|
| 800  | 424–518 of 800 (**53–65%**) | 1.08–1.33 s | 800 of 800 (100%, every run) | 1.99–2.01 s |
| 2000 | 388–964 of 2000 (**19–48%**) | 0.98–2.50 s | 2000 of 2000 (100%, every run) | 5.03–5.05 s |

The old design's reported completion times at this scale are **not real
throughput numbers** — they are artifacts of doing dramatically less work
than submitted. Its own logs make this explicit:

```
[WARNING] No CPU available for task 1982
[WARNING] No CPU available for task 1983
...
```

**Root cause:** the old `find_best_cpu()` initializes its "best score so
far" sentinel to `999.9` and only ever accepts a core whose score beats it.
Its scoring adds `active_tasks * 10` per core to bias away from busy cores.
Thread-per-task has no cap on concurrently active tasks per core — under a
submission burst, `active_tasks` on every core can climb past ~100
simultaneously (100 × 10 = 1000 > 999.9), at which point **no core ever
beats the sentinel**, `find_best_cpu()` returns `-1` for every subsequent
task, and the scheduler silently drops each one — logged, never run, never
retried. This is inherently timing-dependent (hence the wide run-to-run
variance above): it depends on how many tasks happen to be mid-flight at
once, which depends on real-time OS scheduling.

The new design's equivalent scoring loop (`select_cpu_scored()` in
`src/load_balancer.c`) uses a `1e18` sentinel specifically to avoid this
class of bug, and — independently — `active_tasks` per core is structurally
bounded near 0–1 in the new design, since each core has exactly one worker
executing one task at a time. Both the symptom and its precondition are
gone by construction, not by raising a magic number.

## Summary

The worker-pool rewrite was motivated by architecture and the "dynamic load
balancing" the project's name promises, not primarily by a performance
complaint about the old design. But measuring the two side by side surfaced
a genuine, severe, silent-data-loss bug in the old design that its own test
workloads never ran at a scale large enough to trigger — thread-per-task's
unbounded concurrency isn't just slower under load, it can quietly discard
most of the work. The worker pool's fixed thread count removes both the
per-task creation cost and the failure mode that depends on unbounded
concurrency to occur.
