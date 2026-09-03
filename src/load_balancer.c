#include "load_balancer.h"
#include "logger.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <errno.h>

/*
 * One per running task thread, and simultaneously the registry's list node.
 *
 * There is already exactly one heap allocation per in-flight task thread, so
 * making it the node removes the registry's capacity question entirely: no
 * growth policy, no free list, no slot reuse, and no allocation on the
 * completion path. Node lifetime is thread lifetime by construction.
 *
 * `tid` lives here rather than in Task because the task thread frees its own
 * Task — keeping the pthread_t there would leave the joiner reading freed
 * memory.
 */
typedef struct TaskRun {
    struct TaskRun* next;
    LoadBalancer*   lb;
    Task*           task;
    pthread_t       tid;
} TaskRun;

/* ---------------------------------------------------------------------------
 * Task thread registry
 * ------------------------------------------------------------------------ */

/*
 * Invariant, under registry->lock: every TaskRun whose thread was created
 * successfully is owned by exactly one of
 *   (1) its own thread, contributing 1 to live_count, or
 *   (2) the done_head list, awaiting join and free.
 * The (1) -> (2) transition happens in a single critical section, so there is
 * no moment where a thread belongs to neither. Therefore live_count == 0
 * implies done_head holds every outstanding thread.
 */

static int registry_init(TaskThreadRegistry* reg) {
    reg->done_head = NULL;
    reg->live_count = 0;
    reg->closed = 0;

    if (pthread_mutex_init(&reg->lock, NULL) != 0) return -1;

    if (pthread_cond_init(&reg->all_done, NULL) != 0) {
        pthread_mutex_destroy(&reg->lock);
        return -1;
    }

    return 0;
}

static void registry_destroy(TaskThreadRegistry* reg) {
    /* Precondition: live_count == 0 and done_head == NULL. Destroying a mutex
     * another thread may still lock is undefined. */
    pthread_cond_destroy(&reg->all_done);
    pthread_mutex_destroy(&reg->lock);
}

/*
 * Creates the thread WITH THE LOCK HELD, so it is counted before it can run.
 *
 * That ordering is the point. If the count were incremented inside the child
 * (as it used to be), then between dequeue_task() removing the task from the
 * queue and the child reaching its increment, the task would be in neither the
 * queue nor the counter — invisible to both terms of the caller's drain check.
 * A shutdown could then observe "queue empty and nothing active" and free the
 * balancer while a task was still starting up.
 *
 * Returns 0, ECANCELED if the registry is closed, or the pthread_create error.
 * On any non-zero return the caller still owns `run`.
 */
static int registry_spawn(TaskThreadRegistry* reg, const pthread_attr_t* attr,
                          void* (*fn)(void*), TaskRun* run) {
    pthread_mutex_lock(&reg->lock);

    if (reg->closed) {
        pthread_mutex_unlock(&reg->lock);
        return ECANCELED;
    }

    int rc = pthread_create(&run->tid, attr, fn, run);
    if (rc == 0) {
        reg->live_count++;
    }

    pthread_mutex_unlock(&reg->lock);
    return rc;
}

/*
 * Called by a task thread as its LAST action. Nothing may follow it: once
 * live_count hits zero a waiter is free to tear the balancer down.
 */
static void registry_finish(TaskThreadRegistry* reg, TaskRun* run) {
    pthread_mutex_lock(&reg->lock);

    run->next = reg->done_head;
    reg->done_head = run;
    reg->live_count--;

    /* Broadcast while still holding the lock: a waiter that wakes may destroy
     * all_done as soon as it can proceed. */
    if (reg->live_count == 0) {
        pthread_cond_broadcast(&reg->all_done);
    }

    pthread_mutex_unlock(&reg->lock);
}

/*
 * Joins and frees whatever has finished. Called once per scheduler iteration
 * so a long run does not accumulate unjoined thread stacks.
 *
 * Never joins while holding the lock: the joinee's exit path calls
 * registry_finish(), which wants that same lock. Steal the list, unlock, then
 * join — which is also why an intrusive list beats an array here, since
 * stealing it is two pointer assignments.
 */
static void registry_reap(TaskThreadRegistry* reg) {
    pthread_mutex_lock(&reg->lock);
    TaskRun* list = reg->done_head;
    reg->done_head = NULL;
    pthread_mutex_unlock(&reg->lock);

    while (list) {
        TaskRun* next = list->next;
        pthread_join(list->tid, NULL);
        free(list);
        list = next;
    }
}

static void registry_close(TaskThreadRegistry* reg) {
    pthread_mutex_lock(&reg->lock);
    reg->closed = 1;
    pthread_mutex_unlock(&reg->lock);
}

static int registry_live(TaskThreadRegistry* reg) {
    pthread_mutex_lock(&reg->lock);
    int live = reg->live_count;
    pthread_mutex_unlock(&reg->lock);
    return live;
}

/*
 * Waits for every task thread to finish, then joins them all.
 *
 * The 5-second deadline is a diagnostic, not an escape: it logs progress and
 * re-arms. Returning while a thread is still live is exactly what let cleanup
 * destroy this mutex out from under it.
 */
static void registry_join_all(TaskThreadRegistry* reg) {
    pthread_mutex_lock(&reg->lock);

    while (reg->live_count > 0) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 5;

        if (pthread_cond_timedwait(&reg->all_done, &reg->lock,
                                   &deadline) == ETIMEDOUT) {
            log_message(LOG_WARNING, "Still waiting for %d task thread(s)",
                        reg->live_count);
        }
    }

    /* live_count == 0, so by the invariant done_head holds every thread. */
    TaskRun* list = reg->done_head;
    reg->done_head = NULL;
    pthread_mutex_unlock(&reg->lock);

    while (list) {
        TaskRun* next = list->next;
        pthread_join(list->tid, NULL);
        free(list);
        list = next;
    }
}

/* ---------------------------------------------------------------------------
 * Load balancer
 * ------------------------------------------------------------------------ */

LoadBalancer* init_load_balancer(LoadBalancerConfig* config) {
    if (!config) return NULL;

    LoadBalancer* lb = calloc(1, sizeof(LoadBalancer));
    if (!lb) return NULL;

    lb->config = config;
    atomic_init(&lb->running, 0);
    lb->monitor_started = 0;
    lb->scheduler_started = 0;

    /* Open the logger first, so the constructors below can actually report
     * their failures. A failed log file is not fatal — logging falls back to
     * stderr — but it is worth saying out loud. */
    if (init_logger(config->log_file_path, config->enable_detailed_logging) != 0) {
        fprintf(stderr, "Warning: cannot open log file '%s'; logging to stderr\n",
                config->log_file_path ? config->log_file_path : "(null)");
    }

    /* Initialised here rather than in start_load_balancer() so that a balancer
     * which was never started, or whose start failed, still has valid
     * primitives for cleanup_load_balancer() to destroy. */
    if (registry_init(&lb->task_threads) != 0) {
        log_message(LOG_ERROR, "Failed to initialize task thread registry");
        free(lb);
        return NULL;
    }

    lb->cpu_monitor = init_cpu_monitor(config);
    lb->task_queue = init_task_queue(config->max_tasks);

    if (!lb->cpu_monitor || !lb->task_queue) {
        log_message(LOG_ERROR, "Failed to initialize load balancer subsystems");
        cleanup_cpu_monitor(lb->cpu_monitor);
        cleanup_task_queue(lb->task_queue);
        registry_destroy(&lb->task_threads);
        free(lb);
        return NULL;
    }

    return lb;
}

int start_load_balancer(LoadBalancer* lb) {
    if (!lb) return -1;

    if (lb->monitor_started || lb->scheduler_started) {
        log_message(LOG_WARNING, "Load balancer already started");
        return -1;
    }

    atomic_store(&lb->running, 1);

    /* pthread_create returns the error number directly; it does not set errno. */
    int rc = pthread_create(&lb->monitor_thread, NULL, monitor_thread_func, lb);
    if (rc != 0) {
        log_message(LOG_ERROR, "Failed to create monitor thread: %s", strerror(rc));
        atomic_store(&lb->running, 0);
        return -1;
    }
    lb->monitor_started = 1;

    rc = pthread_create(&lb->scheduler_thread, NULL, scheduler_thread_func, lb);
    if (rc != 0) {
        log_message(LOG_ERROR, "Failed to create scheduler thread: %s", strerror(rc));
        atomic_store(&lb->running, 0);
        pthread_join(lb->monitor_thread, NULL);
        lb->monitor_started = 0;   /* pthread_t is now stale; never join it again */
        return -1;
    }
    lb->scheduler_started = 1;

    /* Deliberately not done here: shutting the queue down (it is empty, and
     * doing so would make the object un-restartable) and waiting for task
     * threads (none can exist — only the scheduler creates them). Disposal is
     * cleanup_load_balancer()'s job. */

    log_message(LOG_INFO, "Load balancer started");
    return 0;
}

void* monitor_thread_func(void* arg) {
    LoadBalancer* lb = (LoadBalancer*)arg;

    while (atomic_load(&lb->running)) {
        update_cpu_stats(lb->cpu_monitor);
        check_load_balance(lb->cpu_monitor);

        if (lb->config->enable_detailed_logging) {
            print_cpu_stats(lb->cpu_monitor);
        }

        usleep(lb->config->monitoring_interval_ms * 1000);
    }

    return NULL;
}

int find_best_cpu(CPUMonitor* monitor) {
    int best_cpu = -1;
    double lowest_load = 999.9;

    /* Read under the monitor lock: these fields are written concurrently by
     * the monitor thread. */
    pthread_mutex_lock(&monitor->lock);

    for (int i = 0; i < monitor->num_cpus; i++) {
        double effective_load = monitor->stats[i].current_usage;

        if (monitor->config->enable_load_prediction) {
            effective_load = (effective_load + monitor->stats[i].predicted_load) / 2;
        }

        /* Bias against cores that already have work in flight. The measured
         * usage from /proc/stat lags by up to one monitoring interval, so a
         * core that was just handed a task still looks idle; this term stops
         * a burst of submissions all landing on the same core. */
        effective_load += (monitor->stats[i].active_tasks * 10);

        if (effective_load < lowest_load) {
            lowest_load = effective_load;
            best_cpu = i;
        }
    }

    pthread_mutex_unlock(&monitor->lock);

    return best_cpu;
}

int submit_task(LoadBalancer* lb, void (*function)(void*), void* args,
                TaskArgsDestructor args_free, TaskPriority priority) {
    /* Ownership of args transfers on entry, so every failure path below must
     * destroy them. The caller has no branch to get wrong. */
    if (!lb || !function) {
        if (args_free) args_free(args);
        return -1;
    }

    Task* task = create_task(function, args, args_free, priority);
    if (!task) {
        /* No Task exists to own args, so this is the one place outside
         * task_release_args() that invokes the destructor directly. */
        if (args_free) args_free(args);
        return -1;
    }

    if (enqueue_task(lb->task_queue, task) != 0) {
        free_task(task);   /* destroys args too */
        return -1;
    }

    return 0;
}

/* Wrapper for task execution */
static void* task_wrapper(void* arg) {
    TaskRun* run = (TaskRun*)arg;
    Task* task = run->task;
    LoadBalancer* lb = run->lb;

    task->function(task->args);

    task->status = STATUS_COMPLETED;
    clock_gettime(CLOCK_MONOTONIC, &task->end_time);

    task->cpu_usage = (task->end_time.tv_sec - task->start_time.tv_sec) +
                      (task->end_time.tv_nsec - task->start_time.tv_nsec) / 1e9;

    /* Release the core's slot. The original code incremented this counter and
     * never decremented it, so find_best_cpu()'s bias term grew without bound
     * and the placement decision drifted away from reality. */
    cpu_monitor_adjust_active_tasks(lb->cpu_monitor, task->assigned_cpu, -1);

    /* free_task() runs the caller's args destructor, which is arbitrary code
     * that may log or touch balancer state. It must therefore run BEFORE the
     * thread is reported finished, while the join still protects it. */
    run->task = NULL;
    free_task(task);

    /* Last statement. `run` now belongs to whoever reaps it; the reaper frees
     * it after pthread_join returns. Nothing may follow this call. */
    registry_finish(&lb->task_threads, run);
    return NULL;
}

void* scheduler_thread_func(void* arg) {
    LoadBalancer* lb = (LoadBalancer*)arg;
    sigset_t set;

    // Block SIGINT in this thread so it is always delivered to main
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    for (;;) {
        /* Reclaim finished threads as we go, so a long run does not
         * accumulate unjoined stacks. */
        registry_reap(&lb->task_threads);

        /* Returns NULL only once the queue is shut down and drained, which is
         * this loop's exit condition. */
        Task* task = dequeue_task(lb->task_queue);
        if (!task) break;

        if (!atomic_load(&lb->running)) {
            task->status = STATUS_FAILED;
            free_task(task);
            continue;
        }

        int cpu_id = find_best_cpu(lb->cpu_monitor);
        if (cpu_id < 0) {
            log_message(LOG_WARNING, "No CPU available for task %d", task->task_id);
            free_task(task);
            continue;
        }

        TaskRun* run = malloc(sizeof(TaskRun));
        if (!run) {
            log_message(LOG_ERROR, "Out of memory scheduling task %d", task->task_id);
            free_task(task);
            continue;
        }
        run->next = NULL;
        run->lb = lb;
        run->task = task;

        /* Pin via the thread attributes rather than calling
         * pthread_setaffinity_np() after pthread_create(): the new thread is
         * runnable the instant it is created, so setting affinity afterwards
         * races with it and the first slice can land on the wrong core.
         *
         * Note there is no PTHREAD_CREATE_DETACHED here: shutdown joins these
         * threads, and joining a detached thread is undefined. */
        pthread_attr_t attr;
        cpu_set_t cpuset;

        pthread_attr_init(&attr);
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);

        task->assigned_cpu = cpu_id;
        task->status = STATUS_RUNNING;
        clock_gettime(CLOCK_MONOTONIC, &task->start_time);

        cpu_monitor_adjust_active_tasks(lb->cpu_monitor, cpu_id, +1);

        int rc = registry_spawn(&lb->task_threads, &attr, task_wrapper, run);
        pthread_attr_destroy(&attr);

        if (rc != 0) {
            if (rc == ECANCELED) {
                log_message(LOG_INFO, "Shutting down; dropping task %d",
                            task->task_id);
            } else {
                log_message(LOG_ERROR, "Failed to create thread for task %d: %s",
                            task->task_id, strerror(rc));
            }
            /* Nothing was registered, so there is no registry state to undo. */
            cpu_monitor_adjust_active_tasks(lb->cpu_monitor, cpu_id, -1);
            task->status = STATUS_FAILED;
            free(run);
            free_task(task);
            continue;
        }

        log_message(LOG_INFO, "Task %d (priority %d) assigned to CPU %d",
                    task->task_id, task->priority, cpu_id);
    }

    return NULL;
}

int load_balancer_active_tasks(LoadBalancer* lb) {
    if (!lb) return 0;
    return registry_live(&lb->task_threads);
}

void wait_for_tasks_completion(LoadBalancer* lb) {
    if (!lb) return;
    registry_join_all(&lb->task_threads);
}

void cancel_pending_tasks(LoadBalancer* lb) {
    if (!lb) return;

    /* Delegates to the queue, which pops through an internal helper while
     * holding its own mutex. The previous version locked the queue mutex here
     * and then called dequeue_task(), which locks the same non-recursive
     * mutex a second time — an unconditional self-deadlock whenever any task
     * was still pending. */
    int dropped = drain_task_queue(lb->task_queue);
    if (dropped > 0) {
        log_message(LOG_INFO, "Cancelled %d pending task(s)", dropped);
    }
}

void stop_load_balancer(LoadBalancer* lb) {
    if (!lb) return;

    if (atomic_exchange(&lb->running, 0) != 0) {
        log_message(LOG_INFO, "Initiating load balancer shutdown");

        /* Unblocks the scheduler thread parked in dequeue_task(). */
        shutdown_task_queue(lb->task_queue);

        /* Scheduler first — it is the one blocked on the queue. */
        if (lb->scheduler_started) {
            pthread_join(lb->scheduler_thread, NULL);
            lb->scheduler_started = 0;
        }
        if (lb->monitor_started) {
            pthread_join(lb->monitor_thread, NULL);
            lb->monitor_started = 0;
        }

        cancel_pending_tasks(lb);
    } else {
        /* Never started, or already stopped: still make the queue safe. */
        shutdown_task_queue(lb->task_queue);
    }

    /* Outside the guard above on purpose. cleanup_load_balancer() calls this
     * function; if a caller already stopped us, an early return here would
     * skip the join and let cleanup destroy the registry while task threads
     * were still using it. Joining twice is a no-op. */
    registry_close(&lb->task_threads);
    registry_join_all(&lb->task_threads);

    log_message(LOG_INFO, "Load balancer stopped successfully");
}

void cleanup_load_balancer(LoadBalancer* lb) {
    if (!lb) return;

    stop_load_balancer(lb);

    /* Belt and braces: stop_load_balancer() always joins, but this makes the
     * precondition for registry_destroy() explicit at the point it matters. */
    registry_join_all(&lb->task_threads);

    cleanup_cpu_monitor(lb->cpu_monitor);
    cleanup_task_queue(lb->task_queue);

    lb->cpu_monitor = NULL;
    lb->task_queue = NULL;
    lb->config = NULL;   /* owned by the caller, freed via free_config() */

    registry_destroy(&lb->task_threads);

    free(lb);
}
