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
#include <sys/resource.h>

/* How long a worker's pop_own() blocks before it wakes to check whether it
 * should try stealing, or whether it is time to exit. Short enough that an
 * idle core notices a busy peer quickly; long enough that a steady stream of
 * work never touches the timeout path at all (pop_own returns as soon as a
 * task is pushed, regardless of this value). */
#define WORKER_POLL_MS 20

/* ---------------------------------------------------------------------------
 * Load balancer
 * ------------------------------------------------------------------------ */

LoadBalancer* init_load_balancer(LoadBalancerConfig* config) {
    if (!config) return NULL;

    LoadBalancer* lb = calloc(1, sizeof(LoadBalancer));
    if (!lb) return NULL;

    lb->config = config;
    atomic_init(&lb->running, 0);
    atomic_init(&lb->dispatcher_done, 0);
    atomic_init(&lb->rr_cursor, 0);
    atomic_init(&lb->tasks_in_flight, 0);
    lb->monitor_started = 0;
    lb->dispatcher_started = 0;
    lb->workers = NULL;

    /* Open the logger first, so the constructors below can actually report
     * their failures. A failed log file is not fatal — logging falls back to
     * stderr — but it is worth saying out loud. */
    if (init_logger(config->log_file_path, config->enable_detailed_logging) != 0) {
        fprintf(stderr, "Warning: cannot open log file '%s'; logging to stderr\n",
                config->log_file_path ? config->log_file_path : "(null)");
    }

    lb->cpu_monitor = init_cpu_monitor(config);
    lb->task_queue = init_task_queue(config->max_tasks);

    if (!lb->cpu_monitor || !lb->task_queue) {
        log_message(LOG_ERROR, "Failed to initialize load balancer subsystems");
        cleanup_cpu_monitor(lb->cpu_monitor);
        cleanup_task_queue(lb->task_queue);
        free(lb);
        return NULL;
    }

    return lb;
}

/*
 * Tears down whichever of the first `count` worker slots actually finished
 * pthread_create(), used both when start_load_balancer() fails partway
 * through and (with count == config->num_cpus) during normal shutdown.
 */
static void stop_and_join_workers(LoadBalancer* lb, int count) {
    for (int i = 0; i < count; i++) {
        if (lb->workers[i].queue) core_queue_shutdown(lb->workers[i].queue);
    }
    for (int i = 0; i < count; i++) {
        if (lb->workers[i].started) {
            pthread_join(lb->workers[i].thread, NULL);
            lb->workers[i].started = 0;
        }
    }
}

int start_load_balancer(LoadBalancer* lb) {
    if (!lb) return -1;

    if (lb->monitor_started || lb->dispatcher_started) {
        log_message(LOG_WARNING, "Load balancer already started");
        return -1;
    }

    int num_cpus = lb->config->num_cpus;
    if (num_cpus <= 0) {
        log_message(LOG_ERROR, "Cannot start with num_cpus = %d", num_cpus);
        return -1;
    }

    atomic_store(&lb->running, 1);
    atomic_store(&lb->dispatcher_done, 0);

    lb->workers = calloc((size_t)num_cpus, sizeof(Worker));
    if (!lb->workers) {
        log_message(LOG_ERROR, "Out of memory allocating worker pool");
        atomic_store(&lb->running, 0);
        return -1;
    }

    /*
     * Two passes on purpose. steal_from_peers() and worker_should_exit() read
     * every worker's `.queue` pointer, including peers' — a worker thread can
     * start running (via pthread_create below) the instant it is created,
     * and from that point on those reads race with this thread still writing
     * `.queue` for workers created later. Finishing every allocation here,
     * in a pass that creates no threads, guarantees each `.queue` is fully
     * written before pthread_create() runs for ANY worker; the pthread_create
     * happens-before edge then makes all of them visible to every worker
     * thread, not just the ones created before it.
     */
    for (int i = 0; i < num_cpus; i++) {
        Worker* w = &lb->workers[i];
        w->lb = lb;
        w->cpu_id = i;
        w->started = 0;
        w->tasks_run = 0;
        w->tasks_stolen_by_me = 0;
        w->voluntary_ctxt_switches = 0;
        w->involuntary_ctxt_switches = 0;

        w->queue = init_core_queue();
        if (!w->queue) {
            log_message(LOG_ERROR, "Failed to create core queue for CPU %d", i);
            for (int j = 0; j < i; j++) cleanup_core_queue(lb->workers[j].queue);
            free(lb->workers);
            lb->workers = NULL;
            atomic_store(&lb->running, 0);
            return -1;
        }
    }

    int created = 0;
    for (int i = 0; i < num_cpus; i++) {
        Worker* w = &lb->workers[i];

        /* Pin via thread attributes rather than pthread_setaffinity_np()
         * after creation: the new thread is runnable the instant it is
         * created, so setting affinity afterwards races with it and the
         * first scheduling slice can land on the wrong core. */
        pthread_attr_t attr;
        cpu_set_t cpuset;
        pthread_attr_init(&attr);
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);

        int rc = pthread_create(&w->thread, &attr, worker_thread_func, w);
        pthread_attr_destroy(&attr);

        if (rc != 0) {
            log_message(LOG_ERROR, "Failed to create worker thread for CPU %d: %s",
                        i, strerror(rc));
            /* dispatcher_done can be set now: the dispatcher was never
             * created on this failure path, so it is trivially true that no
             * further push will ever reach any CoreQueue. */
            atomic_store(&lb->dispatcher_done, 1);
            stop_and_join_workers(lb, created);
            for (int j = 0; j < num_cpus; j++) cleanup_core_queue(lb->workers[j].queue);
            free(lb->workers);
            lb->workers = NULL;
            atomic_store(&lb->running, 0);
            atomic_store(&lb->dispatcher_done, 0);
            return -1;
        }

        w->started = 1;
        created++;
    }

    int rc = pthread_create(&lb->monitor_thread, NULL, monitor_thread_func, lb);
    if (rc != 0) {
        log_message(LOG_ERROR, "Failed to create monitor thread: %s", strerror(rc));
        atomic_store(&lb->running, 0);
        atomic_store(&lb->dispatcher_done, 1);
        stop_and_join_workers(lb, created);
        for (int j = 0; j < num_cpus; j++) cleanup_core_queue(lb->workers[j].queue);
        free(lb->workers);
        lb->workers = NULL;
        return -1;
    }
    lb->monitor_started = 1;

    rc = pthread_create(&lb->dispatcher_thread, NULL, dispatcher_thread_func, lb);
    if (rc != 0) {
        log_message(LOG_ERROR, "Failed to create dispatcher thread: %s", strerror(rc));
        atomic_store(&lb->running, 0);
        pthread_join(lb->monitor_thread, NULL);
        lb->monitor_started = 0;
        atomic_store(&lb->dispatcher_done, 1);
        stop_and_join_workers(lb, created);
        for (int j = 0; j < num_cpus; j++) cleanup_core_queue(lb->workers[j].queue);
        free(lb->workers);
        lb->workers = NULL;
        return -1;
    }
    lb->dispatcher_started = 1;

    log_message(LOG_INFO, "Load balancer started: %d worker(s), policy=%s, work_stealing=%s",
                num_cpus, scheduling_policy_name(lb->config->scheduling_policy),
                lb->config->enable_work_stealing ? "on" : "off");
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

/* ---------------------------------------------------------------------------
 * CPU selection
 * ------------------------------------------------------------------------ */

static int select_cpu_round_robin(LoadBalancer* lb) {
    int n = lb->config->num_cpus;
    if (n <= 0) return -1;
    /* fetch_add always advances, so distinct callers never see the same
     * value even if the counter wraps; only the result mod n matters. */
    unsigned int idx = (unsigned int)atomic_fetch_add(&lb->rr_cursor, 1);
    return (int)(idx % (unsigned int)n);
}

/*
 * Shared scoring loop for LEAST_LOAD and PREDICTIVE: both blend measured
 * usage with a bias toward cores that already have work either running or
 * queued; PREDICTIVE additionally blends in the moving-average prediction.
 *
 * The `active_tasks * 10` term compensates for /proc/stat lagging by up to
 * one monitoring interval, so a core just handed work still looks idle. The
 * queue-depth term is the analogous compensation for the *dispatcher*: a
 * burst of submissions should spread across queues, not stack up behind one
 * core just because that core's current_usage hasn't risen yet.
 */
static int select_cpu_scored(LoadBalancer* lb, int use_prediction) {
    CPUMonitor* monitor = lb->cpu_monitor;
    int best_cpu = -1;
    double lowest_load = 1e18;

    pthread_mutex_lock(&monitor->lock);

    for (int i = 0; i < monitor->num_cpus; i++) {
        double effective_load = monitor->stats[i].current_usage;

        if (use_prediction && monitor->config->enable_load_prediction) {
            effective_load = (effective_load + monitor->stats[i].predicted_load) / 2;
        }

        effective_load += (monitor->stats[i].active_tasks * 10);
        effective_load += (core_queue_size(lb->workers[i].queue) * 5);

        if (effective_load < lowest_load) {
            lowest_load = effective_load;
            best_cpu = i;
        }
    }

    pthread_mutex_unlock(&monitor->lock);

    return best_cpu;
}

int select_cpu(LoadBalancer* lb) {
    if (!lb || lb->config->num_cpus <= 0) return -1;

    switch (lb->config->scheduling_policy) {
        case SCHED_ROUND_ROBIN:
            return select_cpu_round_robin(lb);
        case SCHED_LEAST_LOAD:
            return select_cpu_scored(lb, 0);
        case SCHED_PREDICTIVE:
        default:
            return select_cpu_scored(lb, 1);
    }
}

/* ---------------------------------------------------------------------------
 * Submission and execution
 * ------------------------------------------------------------------------ */

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

/* Runs one task to completion on the calling worker thread. `w` need not be
 * the core the dispatcher originally chose: when the task arrived via a
 * steal, w->cpu_id is where it actually ends up executing, and task-
 * >assigned_cpu is set to match — that is the task migration this pool
 * exists to enable. */
static void run_task(LoadBalancer* lb, Worker* w, Task* task) {
    task->assigned_cpu = w->cpu_id;
    task->status = STATUS_RUNNING;
    clock_gettime(CLOCK_MONOTONIC, &task->start_time);

    cpu_monitor_adjust_active_tasks(lb->cpu_monitor, w->cpu_id, +1);
    atomic_fetch_add(&lb->tasks_in_flight, 1);

    task->function(task->args);

    task->status = STATUS_COMPLETED;
    clock_gettime(CLOCK_MONOTONIC, &task->end_time);
    task->cpu_usage = (task->end_time.tv_sec - task->start_time.tv_sec) +
                      (task->end_time.tv_nsec - task->start_time.tv_nsec) / 1e9;

    cpu_monitor_adjust_active_tasks(lb->cpu_monitor, w->cpu_id, -1);
    atomic_fetch_sub(&lb->tasks_in_flight, 1);
    w->tasks_run++;

    log_message(LOG_INFO, "Task %d completed on CPU %d in %.3fs",
                task->task_id, w->cpu_id, task->cpu_usage);

    /* Give the hook a look at the finished task, including timestamps and
     * final assigned_cpu, before it is freed. */
    if (lb->config->on_task_complete) {
        lb->config->on_task_complete(task, lb->config->on_task_complete_user_data);
    }

    free_task(task);
}

/*
 * Looks for the peer with the deepest queue and, if it has more than one
 * task queued, steals one. Scanning for the *most* loaded peer (rather than
 * the first non-empty one) is what makes stealing target an overloaded core
 * specifically, instead of just whichever core happened to be checked first.
 */
static Task* steal_from_peers(LoadBalancer* lb, Worker* self) {
    int n = lb->config->num_cpus;
    int victim = -1;
    int victim_depth = 1;   /* core_queue_try_steal already refuses <= 1 */

    for (int i = 0; i < n; i++) {
        if (i == self->cpu_id) continue;
        int depth = core_queue_size(lb->workers[i].queue);
        if (depth > victim_depth) {
            victim_depth = depth;
            victim = i;
        }
    }

    if (victim < 0) return NULL;

    Task* task = core_queue_try_steal(lb->workers[victim].queue);
    if (task) {
        self->tasks_stolen_by_me++;
        log_message(LOG_DEBUG, "CPU %d stole task %d from CPU %d",
                    self->cpu_id, task->task_id, victim);
    }
    return task;
}

/*
 * True only once it is certain no task will ever reach this worker again:
 * the pool is stopping, the dispatcher has been joined (so nothing new can
 * be pushed to any CoreQueue), and every CoreQueue — this one and every
 * peer's — is observed empty.
 *
 * That last scan is safe without a global counter because, once
 * dispatcher_done is set, the total task count across every CoreQueue is
 * monotonically non-increasing: nothing can push again, so anything this
 * function sees as empty stays empty. A peer transiently non-empty just
 * means "not yet" — the loop that calls this tries again next iteration.
 */
static int worker_should_exit(LoadBalancer* lb) {
    if (atomic_load(&lb->running)) return 0;
    if (!atomic_load(&lb->dispatcher_done)) return 0;

    for (int i = 0; i < lb->config->num_cpus; i++) {
        if (core_queue_size(lb->workers[i].queue) > 0) return 0;
    }
    return 1;
}

static void record_thread_rusage(Worker* w) {
    struct rusage usage;
    if (getrusage(RUSAGE_THREAD, &usage) == 0) {
        w->voluntary_ctxt_switches = usage.ru_nvcsw;
        w->involuntary_ctxt_switches = usage.ru_nivcsw;
    }
}

void* worker_thread_func(void* arg) {
    Worker* w = (Worker*)arg;
    LoadBalancer* lb = w->lb;
    sigset_t set;

    /* Block SIGINT here too: with a fixed pool instead of per-task threads,
     * these are now long-lived, and the same reasoning applies — the signal
     * must always be delivered to main, never to a worker. */
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    for (;;) {
        Task* task = core_queue_pop_own(w->queue, WORKER_POLL_MS);

        if (!task && lb->config->enable_work_stealing) {
            task = steal_from_peers(lb, w);
        }

        if (task) {
            run_task(lb, w, task);
            continue;
        }

        if (worker_should_exit(lb)) break;
    }

    record_thread_rusage(w);
    return NULL;
}

void* dispatcher_thread_func(void* arg) {
    LoadBalancer* lb = (LoadBalancer*)arg;
    sigset_t set;

    // Block SIGINT in this thread so it is always delivered to main
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    for (;;) {
        /* Returns NULL only once the queue is shut down and drained, which is
         * this loop's exit condition. */
        Task* task = dequeue_task(lb->task_queue);
        if (!task) break;

        if (!atomic_load(&lb->running)) {
            task->status = STATUS_FAILED;
            free_task(task);
            continue;
        }

        int cpu_id = select_cpu(lb);
        if (cpu_id < 0) {
            log_message(LOG_WARNING, "No CPU available for task %d", task->task_id);
            task->status = STATUS_FAILED;
            free_task(task);
            continue;
        }

        task->assigned_cpu = cpu_id;   /* provisional: may be re-set if stolen */

        if (core_queue_push(lb->workers[cpu_id].queue, task) != 0) {
            /* Only reachable if that core's queue was shut down concurrently,
             * which only happens once running is already false. */
            log_message(LOG_INFO, "Shutting down; dropping task %d", task->task_id);
            task->status = STATUS_FAILED;
            free_task(task);
            continue;
        }

        log_message(LOG_INFO, "Task %d (priority %d) dispatched to CPU %d",
                    task->task_id, task->priority, cpu_id);
    }

    return NULL;
}

int load_balancer_active_tasks(LoadBalancer* lb) {
    if (!lb) return 0;
    return atomic_load(&lb->tasks_in_flight);
}

int load_balancer_pending_tasks(LoadBalancer* lb) {
    if (!lb) return 0;

    int pending = task_queue_size(lb->task_queue);
    if (lb->workers) {
        for (int i = 0; i < lb->config->num_cpus; i++) {
            pending += core_queue_size(lb->workers[i].queue);
        }
    }
    return pending;
}

int load_balancer_worker_stats(LoadBalancer* lb, int cpu_id, WorkerStats* out) {
    if (!lb || !out || !lb->workers) return -1;
    if (cpu_id < 0 || cpu_id >= lb->config->num_cpus) return -1;

    Worker* w = &lb->workers[cpu_id];
    out->cpu_id = w->cpu_id;
    out->tasks_run = w->tasks_run;
    out->tasks_stolen_by_me = w->tasks_stolen_by_me;
    out->tasks_stolen_from_me = w->queue ? w->queue->stolen_total : 0;
    out->voluntary_ctxt_switches = w->voluntary_ctxt_switches;
    out->involuntary_ctxt_switches = w->involuntary_ctxt_switches;
    return 0;
}

void wait_for_tasks_completion(LoadBalancer* lb) {
    if (!lb) return;

    while (load_balancer_pending_tasks(lb) > 0 || load_balancer_active_tasks(lb) > 0) {
        usleep(50000);
    }
}

void cancel_pending_tasks(LoadBalancer* lb) {
    if (!lb) return;

    int dropped = drain_task_queue(lb->task_queue);
    if (lb->workers) {
        for (int i = 0; i < lb->config->num_cpus; i++) {
            dropped += core_queue_drain(lb->workers[i].queue);
        }
    }
    if (dropped > 0) {
        log_message(LOG_INFO, "Cancelled %d pending task(s)", dropped);
    }
}

void stop_load_balancer(LoadBalancer* lb) {
    if (!lb) return;

    if (atomic_exchange(&lb->running, 0) != 0) {
        log_message(LOG_INFO, "Initiating load balancer shutdown");

        /* Unblocks the dispatcher thread parked in dequeue_task(). Every task
         * still in the admission queue at this point gets marked FAILED by
         * the dispatcher's `!running` check above, not dispatched further. */
        shutdown_task_queue(lb->task_queue);

        if (lb->dispatcher_started) {
            pthread_join(lb->dispatcher_thread, NULL);
            lb->dispatcher_started = 0;
        }

        /* Only valid once the dispatcher is truly gone: this is the signal
         * workers use to know no CoreQueue will ever be pushed to again. */
        atomic_store(&lb->dispatcher_done, 1);

        if (lb->workers) {
            stop_and_join_workers(lb, lb->config->num_cpus);
        }

        if (lb->monitor_started) {
            pthread_join(lb->monitor_thread, NULL);
            lb->monitor_started = 0;
        }

        cancel_pending_tasks(lb);
    } else {
        /* Never started, or already stopped: still make every queue safe to
         * free. Workers, if any exist, were already joined by the branch
         * above on the call that actually flipped `running` to 0. */
        shutdown_task_queue(lb->task_queue);
        atomic_store(&lb->dispatcher_done, 1);
        cancel_pending_tasks(lb);
    }

    log_message(LOG_INFO, "Load balancer stopped successfully");
}

void cleanup_load_balancer(LoadBalancer* lb) {
    if (!lb) return;

    stop_load_balancer(lb);

    if (lb->workers) {
        for (int i = 0; i < lb->config->num_cpus; i++) {
            cleanup_core_queue(lb->workers[i].queue);
        }
        free(lb->workers);
        lb->workers = NULL;
    }

    cleanup_cpu_monitor(lb->cpu_monitor);
    cleanup_task_queue(lb->task_queue);

    lb->cpu_monitor = NULL;
    lb->task_queue = NULL;
    lb->config = NULL;   /* owned by the caller, freed via free_config() */

    free(lb);
}
