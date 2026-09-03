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
 * The stat fields are written only by the worker's own thread while it runs,
 * so they need no lock; they are only meaningful for reading after the
 * worker has been joined (load_balancer_worker_stats() enforces that by
 * being called from stop/cleanup paths and by benchmarking after a full
 * stop_load_balancer()).
 */
typedef struct Worker {
    struct LoadBalancer* lb;
    CoreQueue* queue;
    int cpu_id;
    pthread_t thread;
    int started;   /* this slot's pthread_create succeeded */

    long tasks_run;              /* executed by this worker, own or stolen */
    long tasks_stolen_by_me;     /* taken from a peer's queue */
    long voluntary_ctxt_switches;    /* getrusage(RUSAGE_THREAD), sampled at exit */
    long involuntary_ctxt_switches;
} Worker;

/* Snapshot returned by load_balancer_worker_stats(); a copy, not a live view. */
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
 * cpu_id is out of range or the pool was never started. Meant to be called
 * after stop_load_balancer() so the values are final. */
int load_balancer_worker_stats(LoadBalancer* lb, int cpu_id, WorkerStats* out);

#endif
