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
| `pthread_create` + `pthread_join` (old design, per task) | ~0.056–0.078 ms | — |
| `CoreQueue` push + pop (new design, per task) | ~0.0001 ms | **~500–780×** |

This is the purely mechanical cost of the two designs' per-task dispatch
mechanism, independent of scheduling policy or workload shape. Re-measured on
a later run of this environment (after this project's four subsequent
concurrency/correctness fixes, none of which touch either side of this
comparison) at ~0.078 ms/cycle — consistent with the original range, and
itself an illustration of section 4's point below: absolute timing varies
run to run, the ~500-780× structural gap does not.

## 2. Peak concurrent OS threads, identical 800-task workload

| Design | Peak threads (sampled from `/proc/<pid>/status` during the run) |
|---|---|
| Old (thread-per-task) | **761** (5–15 ms tasks) / **367** (50–100 ms tasks, see §4) |
| New (worker pool) | **7** (num_cpus workers + monitor + dispatcher + main), regardless of task duration |

The old design creates one OS thread per task with no cap; the new design
never creates more than `num_cpus` worker threads, for the life of the
process, regardless of how many tasks are submitted or how long each one
runs. Reconfirmed independently at both duration ranges tested in this
document.

## 3. Completion time at a scale where the old design doesn't drop tasks

At small submission bursts the old design completes everything, so a
straight completion-time/throughput comparison is meaningful:

| Tasks | Old: completion time | Old: throughput | New: completion time | New: throughput |
|---|---|---|---|---|
| 50  | 0.141–0.148 s | 337–354 tasks/s | 0.134–0.141 s | 354–375 tasks/s |
| 200 | 0.514–0.546 s | 366–389 tasks/s | 0.496–0.504 s | 397–403 tasks/s |

Comparable, with the new design modestly faster and — see below — far more
consistent run to run. Reconfirmed on a later run of this environment (throughput
401-457 tasks/s on both sides at these same task counts) — small-scale
parity between the two designs holds regardless of the section 4 timing
variability, since neither run tripped the drop bug at this scale.

## 4. Correctness under load — the real finding

At larger, bursty submission counts, the two designs diverge sharply, and
not just on speed. **This section's numbers are inherently timing-dependent
— see the callout below before treating any specific percentage as a fixed
property of the old design.**

| Tasks | Old: tasks completed (5 runs) | Old: completion time (5 runs) | New: tasks completed (any run) | New: completion time |
|---|---|---|---|---|
| 800  | 424–518 of 800 (**53–65%**) | 1.08–1.33 s | 800 of 800 (100%, every run) | 1.99–2.01 s |
| 2000 | 388–964 of 2000 (**19–48%**) | 0.98–2.50 s | 2000 of 2000 (100%, every run) | 5.03–5.05 s |

> **On reproducing this: it is genuinely timing-sensitive, not flaky in a bad
> way.** The table above was captured with `min_ms`/`max_ms` = 5/15. Re-running
> the exact same command on a later invocation of this same sandboxed
> environment — after this project's four subsequent concurrency/correctness
> fixes, none of which touch this code path or the frozen `3c73725` commit —
> produced **0 dropped tasks in 10 straight runs** at 800 and even 2000 tasks,
> because raw `pthread_create`+`join` cost measured ~30% higher on that
> occasion (~0.078 ms/cycle vs ~0.06 ms/cycle), which was enough to slow the
> old design's dispatch rate below the threshold this bug needs. Raising task
> duration to `min_ms`/`max_ms` = 50/100 (each task now holds its thread
> "active" roughly 5-10x longer, so more pile up concurrently before any
> finish) reproduced it again immediately and far more *consistently* than
> the original parameters: **exactly 364 of 800 and 364 of 2000 tasks
> completed, across 3 runs each, byte-for-byte identical every time** — the
> longer duration apparently makes the dispatch-rate-vs-completion-rate race
> land on the same outcome instead of a wide 19-65% range. Peak concurrent
> threads under those conditions: **367** (old) vs **7** (new) — reconfirming
> section 2's finding under the parameters that actually trigger the bug in
> this environment right now. The takeaway: the bug's *existence* and *root
> cause* are unconditional facts about the code (below); the *exact drop
> percentage* at any specific `num_cores`/`num_tasks`/duration is a function
> of real-time OS scheduling on whatever machine you run it on, and won't
> reproduce identically across machines or even across runs of this sandbox.
> If `run.sh`'s defaults don't reproduce a drop for you, try a wider task
> duration range via its `MIN_MS`/`MAX_MS` environment variables, e.g.:
> `MIN_MS=50 MAX_MS=100 benchmarks/legacy-vs-pool/run.sh 4 "800 2000"` — before
> concluding the bug is gone; it isn't. Its precondition (enough concurrently
> active threads to push every core's score past the sentinel) is just harder
> to hit at very short task durations on a machine with fast thread creation.

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

## 5. Two bugs this re-verification found in the new design itself

Re-running this comparison after the fixes above had already landed
surfaced two more real races — not in the old design, but in the very
"nothing left to do" accounting (`load_balancer_pending_tasks()` /
`load_balancer_active_tasks()` / `wait_for_tasks_completion()`) that this
document's own comparison drivers, and the shipped `cpu_balancer_bench`,
depend on. Both are fixed in `src/load_balancer.c`; both are covered by
`tests/test_shutdown.c`'s `test_hook_never_observed_incomplete_by_a_tight_poller`.

**Bug A — the `on_task_complete` hook could still be running when
`wait_for_tasks_completion()` returned.** A worker used to drop
`tasks_in_flight` (what that function polls) immediately after
`task->function()` returned — before calling the hook. A caller that woke on
"nothing in flight" and immediately read whatever the hook had accumulated
(exactly what `new_bench.c` and `src/benchmark.c` both do) could race the
last task's hook call and silently under-count by one. Caught by
`benchmarks/legacy-vs-pool/run.sh` itself: a comparison run with
`MIN_MS=50 MAX_MS=100` occasionally reported 799 of 800 tasks completed on
the *new* design — one shy, always the very last one. Fixed by moving both
"in flight" decrements to the end of `run_task()`, after the hook has
returned and the task is freed.

**Bug B — a task could be invisible to both counters between leaving the
admission queue and reaching a core queue.** `tasks_in_flight` used to start
counting only once a worker began executing a task; `load_balancer_pending_
tasks()` stops counting a task the instant the dispatcher dequeues it, before
`select_cpu()` and `core_queue_push()` (a handful of function calls) place it
on a worker's queue. In that gap, a task belonged to neither counter. Found
by a corrected version of the same regression test, using a tight
(non-sleeping) watcher thread specifically because this window is far
narrower than `wait_for_tasks_completion()`'s own 50ms poll interval would
ever sample by chance. Fixed by having the dispatcher itself increment
`tasks_in_flight` the instant it dequeues a task, with matching decrements on
every path that fails without ever reaching a worker.

Both fixes were validated the same way every fix in this project has been:
the regression test was confirmed to fail reliably against each bug
individually (by temporarily reverting one fix at a time) and to pass
consistently — 60/60 runs — with both applied, then the full suite was
re-run clean under Release, ThreadSanitizer, and ASan/UBSan. Worth noting
for anyone extending this test file: the first version of the watcher thread
had its own bug (a classic check-then-act gap between reading `hook_done` at
the top of its loop and reading the counters a few lines later) that
produced false failures indistinguishable at first glance from Bug B — the
diagnostic that resolved it was printing `hook_done`'s value at the moment
of the reported violation, which showed the hook had, in fact, already
finished.

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

This was independently re-confirmed after this project's four subsequent
concurrency/correctness fixes (none of which touch the frozen `3c73725`
snapshot or the current worker-pool code paths this comparison exercises):
the raw dispatch-cost ratio and the "old design creates unbounded threads,
new design never exceeds `num_cpus`" finding reproduced immediately; the
*specific* drop percentages from section 4 did not reproduce at the exact
same parameters on that occasion (0 drops in 10 runs, versus the original
19-65%), because the old design's dispatch rate — bottlenecked by
`pthread_create` latency, which itself varies run to run — didn't outpace
task completion by enough to trip the bug at 5-15 ms task durations that
time. Widening task duration to 50-100 ms reproduced it again immediately,
and far more consistently (364/800 and 364/2000, identical across 3 runs
each) than the original parameters did. The bug and its root cause are real
and unconditional; the precise percentage it produces is not a fixed
property of the code, but of the code's behavior under whatever thread-
creation-speed-vs-task-duration ratio your machine happens to produce at
runtime — which is itself further evidence for the summary's own point:
unbounded thread-per-task concurrency makes a program's behavior depend on
real-time OS scheduling in ways a fixed-size worker pool structurally does
not.

The same re-verification pass also found two real bugs on the *new* side
(section 5): both in the "nothing left to do" accounting this very
comparison relies on to know when a run is finished. Neither is related to
the old design or to task migration/work-stealing — they were latent since
the worker-pool rewrite and simply hadn't been exercised by a workload with
enough concurrent completions landing close together in time. Both are
fixed and covered by a regression test that was itself validated against
both the bug it targets and, in the course of writing it, a false-positive
bug of its own.
