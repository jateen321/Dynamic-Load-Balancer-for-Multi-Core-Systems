#ifndef LOAD_BALANCER_H
#define LOAD_BALANCER_H

#include "config.h"
#include "cpu_stats.h"
#include "task_queue.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

/* One per running task thread. Defined in load_balancer.c: it doubles as the
 * registry's list node, so its layout is private to the implementation. */
struct TaskRun;

/*
 * Tracks live task threads so shutdown can join them instead of guessing.
 *
 * This replaces the file-scope statics (total_active_tasks, its mutex and its
 * condvar) that used to live in load_balancer.c. Those were shared by every
 * LoadBalancer in the process, so two instances corrupted each other's
 * accounting. Just as importantly, a condition variable can only say "the
 * counter reached zero" — pthread_join is the only primitive that says "this
 * thread will never execute another instruction", which is what freeing the
 * balancer out from under a task thread actually requires.
 *
 * live_count and done_head are guarded by one lock on purpose: they encode a
 * single invariant, and with two locks there is always a window where a
 * thread is counted but not registered.
 */
typedef struct {
    struct TaskRun* done_head;  /* finished, awaiting join and free */
    int             live_count; /* created and not yet finished */
    int             closed;     /* set once shutdown begins: refuse new spawns */
    pthread_mutex_t lock;
    pthread_cond_t  all_done;   /* broadcast when live_count reaches 0 */
} TaskThreadRegistry;

typedef struct {
    LoadBalancerConfig* config;
    CPUMonitor* cpu_monitor;
    TaskQueue* task_queue;
    pthread_t monitor_thread;
    pthread_t scheduler_thread;
    /* Separate flags, not one "threads_started": a partial start must join
     * exactly what it created. A single flag leaves the other pthread_t
     * holding uninitialized garbage, and joining that is undefined. */
    int monitor_started;
    int scheduler_started;
    /* Written by stop_load_balancer(), read by the worker threads. Atomic
     * rather than plain int: concurrent access to a plain int is a data race,
     * and `volatile` orders nothing between threads. */
    atomic_int running;
    TaskThreadRegistry task_threads;
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
void* scheduler_thread_func(void* arg);
int find_best_cpu(CPUMonitor* monitor);

/* Blocks until every task thread has terminated. No timeout: returning early
 * is what allowed cleanup to free the balancer under a live thread. A 5-second
 * diagnostic is logged while waiting. */
void wait_for_tasks_completion(LoadBalancer* lb);

void cancel_pending_tasks(LoadBalancer* lb);

/* Stops the balancer, joins every task thread, then frees it. */
void cleanup_load_balancer(LoadBalancer* lb);

/* Number of task threads created and not yet finished. */
int load_balancer_active_tasks(LoadBalancer* lb);

#endif
