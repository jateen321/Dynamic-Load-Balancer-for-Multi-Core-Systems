#ifndef LOAD_BALANCER_H
#define LOAD_BALANCER_H

#include "config.h"
#include "cpu_stats.h"
#include "task_queue.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

struct LoadBalancer;

/*
 * One per core, created once in start_load_balancer() and joined once in
 * stop_load_balancer() — a genuine pool, not a thread spawned per task. Each
 * worker is pinned to its cpu_id and owns exactly one CoreQueue.
 *
 * The stat fields are single-writer (only this worker's own thread ever
 * writes each one) / multi-reader (any thread may call
 * load_balancer_worker_stats() at any time, pool running or stopped), so they
 * are atomic_long rather than behind a lock: the writer side just needs
 * atomic_fetch_add/atomic_store instead of `x++`, and a reader gets a
 * consistent value via atomic_load without ever blocking on the worker.
 */
typedef struct Worker {
    struct LoadBalancer* lb;
    CoreQueue* queue;
    int cpu_id;
    pthread_t thread;
    int started;   /* this slot's pthread_create succeeded */

    atomic_long tasks_run;              /* executed by this worker, own or stolen */
    atomic_long tasks_stolen_by_me;     /* taken from a peer's queue */
    atomic_long voluntary_ctxt_switches;    /* getrusage(RUSAGE_THREAD), sampled at exit */
    atomic_long involuntary_ctxt_switches;
} Worker;

/* Snapshot returned by load_balancer_worker_stats(); a copy, not a live view.
 * Plain long, not atomic: this struct is filled once and handed back by
 * value/out-pointer, never shared or written concurrently itself — only the
 * live Worker fields it is copied from need to be atomic. */
typedef struct {
    int cpu_id;
    long tasks_run;
    long tasks_stolen_by_me;    /* this worker stole from a peer */
    long tasks_stolen_from_me;  /* a peer stole from this worker's queue */
    long voluntary_ctxt_switches;
    long involuntary_ctxt_switches;
} WorkerStats;

typedef struct LoadBalancer {
    LoadBalancerConfig* config;
    CPUMonitor* cpu_monitor;
    TaskQueue* task_queue;     /* global priority admission queue */
    Worker* workers;           /* config->num_cpus entries; NULL until started */

    pthread_t monitor_thread;
    pthread_t dispatcher_thread;
    int monitor_started;
    int dispatcher_started;

    /* Set once this instance has gone through a real running-to-stopped
     * transition, and never cleared. A restart is impossible to make work
     * correctly after that point: the task queue's `shutdown` flag is
     * permanent (task_queue.c has no "unshutdown"), so a new dispatcher would
     * see the queue already shut down and exit immediately, and re-running
     * start_load_balancer() would also overwrite `workers` — leaking the old
     * array and every worker's CoreQueue, since nothing but
     * cleanup_load_balancer() ever frees them. Rather than let that happen
     * silently, start_load_balancer() checks this flag and refuses outright.
     * It is written only from inside stop_load_balancer()'s
     * atomic_exchange(&lb->running, 0) branch — i.e. only on the call that
     * actually observed `running` transition from 1 to 0 — never from the
     * "never started, or already stopped" branch, so that calling
     * stop_load_balancer()/cleanup_load_balancer() on a LoadBalancer that was
     * never successfully started (or whose start failed) does not falsely
     * poison it against a future start. */
    int stopped;

    /* Set once the dispatcher thread has been joined, i.e. once it is
     * certain no further task will ever be pushed into any CoreQueue. A
     * worker is only allowed to exit once this is set AND every CoreQueue —
     * its own and every peer's — is observed empty. Checking `running` alone
     * is not enough: `running` goes false before the dispatcher finishes
     * draining and cancelling the admission queue. */
    atomic_int dispatcher_done;

    /* Written by stop_load_balancer(), read by dispatcher and workers. */
    atomic_int running;

    atomic_int rr_cursor;        /* round-robin cursor, SCHED_ROUND_ROBIN only */
    atomic_int tasks_in_flight;  /* currently executing on some worker */
} LoadBalancer;

LoadBalancer* init_load_balancer(LoadBalancerConfig* config);

/*
 * Takes ownership of `args` on entry. On return — success OR failure — the
 * caller must not read, write or free `args` again; the library has destroyed
 * it via args_free, or will do so later. Returns 0 on success, -1 on failure.
 */
int submit_task(LoadBalancer* lb, void (*function)(void*), void* args,
                TaskArgsDestructor args_free, TaskPriority priority);

/*
 * Returns 0 on success. On failure returns -1, logs the reason, leaves no
 * thread running, and leaves lb valid — the caller must still dispose of it
 * with cleanup_load_balancer().
 *
 * One-shot: once a LoadBalancer has actually been started and later stopped
 * via stop_load_balancer() (directly, or via cleanup_load_balancer()), it can
 * never be started again — this returns -1 immediately instead of attempting
 * it. A stopped instance's task queue is permanently shut down and its
 * worker/CoreQueue pool is not safely reusable, so restarting in place is not
 * supported; create a fresh LoadBalancer with init_load_balancer() instead. A
 * LoadBalancer that was never started, or whose start_load_balancer() call
 * failed, is not affected by this and may still be started normally.
 */
int start_load_balancer(LoadBalancer* lb);

void stop_load_balancer(LoadBalancer* lb);
void* monitor_thread_func(void* arg);
void* dispatcher_thread_func(void* arg);
void* worker_thread_func(void* arg);

/* Chooses a core for the next task according to config->scheduling_policy.
 * Returns -1 only if num_cpus <= 0. */
int select_cpu(LoadBalancer* lb);

/* Blocks until no task is pending or running. Polls; there is no thread to
 * join here since workers outlive any single task. */
void wait_for_tasks_completion(LoadBalancer* lb);

void cancel_pending_tasks(LoadBalancer* lb);

/* Stops the balancer, joins every worker and the dispatcher, then frees it. */
void cleanup_load_balancer(LoadBalancer* lb);

/* Tasks a worker is actively executing right now (not queued). */
int load_balancer_active_tasks(LoadBalancer* lb);

/* Tasks sitting in the global admission queue or a core's queue, not yet
 * executing. active_tasks + pending_tasks == "not finished yet". */
int load_balancer_pending_tasks(LoadBalancer* lb);

/* Copies worker `cpu_id`'s stats into `out`. Returns 0 on success, -1 if
 * cpu_id is out of range or the pool was never started. Safe to call at any
 * time, including while the pool is running and the worker is actively
 * updating these fields: every Worker-owned counter it reads is atomic_long,
 * and tasks_stolen_from_me is read through core_queue_stolen_total(), which
 * takes the queue's own lock rather than reading CoreQueue::stolen_total
 * directly. A call while running just returns whatever snapshot was true at
 * that instant, not a "final" value. */
int load_balancer_worker_stats(LoadBalancer* lb, int cpu_id, WorkerStats* out);

#endif
