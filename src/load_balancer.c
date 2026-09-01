#include "load_balancer.h"
#include "logger.h"
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <errno.h>

static pthread_mutex_t active_tasks_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t active_tasks_cond = PTHREAD_COND_INITIALIZER;
static int total_active_tasks = 0;

/* task_wrapper needs the monitor to decrement the per-core counter when the
 * task finishes, but Task has no back-pointer to the balancer, so the pair
 * travels together in this heap-allocated record. */
typedef struct {
    LoadBalancer* lb;
    Task* task;
} TaskRun;

LoadBalancer* init_load_balancer(LoadBalancerConfig* config) {
    if (!config) return NULL;

    LoadBalancer* lb = malloc(sizeof(LoadBalancer));
    if (!lb) return NULL;

    lb->config = config;
    lb->cpu_monitor = init_cpu_monitor(config);
    lb->task_queue = init_task_queue(config->max_tasks);
    atomic_init(&lb->running, 0);
    lb->threads_started = 0;

    if (!lb->cpu_monitor || !lb->task_queue) {
        cleanup_cpu_monitor(lb->cpu_monitor);
        cleanup_task_queue(lb->task_queue);
        free(lb);
        return NULL;
    }

    init_logger(config->log_file_path, config->enable_detailed_logging);
    return lb;
}

void start_load_balancer(LoadBalancer* lb) {
    if (!lb) return;

    atomic_store(&lb->running, 1);

    if (pthread_create(&lb->monitor_thread, NULL, monitor_thread_func, lb) != 0) {
        log_message(LOG_ERROR, "Failed to create monitor thread");
        atomic_store(&lb->running, 0);
        return;
    }

    if (pthread_create(&lb->scheduler_thread, NULL, scheduler_thread_func, lb) != 0) {
        log_message(LOG_ERROR, "Failed to create scheduler thread");
        atomic_store(&lb->running, 0);
        pthread_join(lb->monitor_thread, NULL);
        return;
    }

    lb->threads_started = 1;
    log_message(LOG_INFO, "Load balancer started");
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

int submit_task(LoadBalancer* lb, void (*function)(void*), void* args, TaskPriority priority) {
    if (!lb) return -1;

    Task* task = create_task(function, args, priority);
    if (!task) return -1;

    int result = enqueue_task(lb->task_queue, task);
    if (result != 0) {
        free_task(task);
        return -1;
    }

    return 0;
}

static void track_task_start(void) {
    pthread_mutex_lock(&active_tasks_mutex);
    total_active_tasks++;
    pthread_mutex_unlock(&active_tasks_mutex);
}

static void track_task_complete(void) {
    pthread_mutex_lock(&active_tasks_mutex);
    total_active_tasks--;
    if (total_active_tasks == 0) {
        pthread_cond_broadcast(&active_tasks_cond);
    }
    pthread_mutex_unlock(&active_tasks_mutex);
}

// Wrapper for task execution
static void* task_wrapper(void* arg) {
    TaskRun* run = (TaskRun*)arg;
    Task* task = run->task;
    LoadBalancer* lb = run->lb;

    track_task_start();

    task->function(task->args);

    task->status = STATUS_COMPLETED;
    clock_gettime(CLOCK_MONOTONIC, &task->end_time);

    task->cpu_usage = (task->end_time.tv_sec - task->start_time.tv_sec) +
                      (task->end_time.tv_nsec - task->start_time.tv_nsec) / 1e9;

    /* Release the core's slot. The original code incremented this counter and
     * never decremented it, so find_best_cpu()'s bias term grew without bound
     * and the placement decision drifted away from reality. */
    cpu_monitor_adjust_active_tasks(lb->cpu_monitor, task->assigned_cpu, -1);

    track_task_complete();

    free_task(task);
    free(run);
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
        /* Returns NULL only once the queue is shut down and drained, which is
         * this loop's exit condition. */
        Task* task = dequeue_task(lb->task_queue);
        if (!task) break;

        if (!atomic_load(&lb->running)) {
            task->status = STATUS_FAILED;
            free(task->args);
            free_task(task);
            continue;
        }

        int cpu_id = find_best_cpu(lb->cpu_monitor);
        if (cpu_id < 0) {
            log_message(LOG_WARNING, "No CPU available for task %d", task->task_id);
            free(task->args);
            free_task(task);
            continue;
        }

        TaskRun* run = malloc(sizeof(TaskRun));
        if (!run) {
            log_message(LOG_ERROR, "Out of memory scheduling task %d", task->task_id);
            free(task->args);
            free_task(task);
            continue;
        }
        run->lb = lb;
        run->task = task;

        /* Pin via the thread attributes rather than calling
         * pthread_setaffinity_np() after pthread_create(): the new thread is
         * runnable the instant it is created, so setting affinity afterwards
         * races with it and the first slice can land on the wrong core. */
        pthread_attr_t attr;
        cpu_set_t cpuset;

        pthread_attr_init(&attr);
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        task->assigned_cpu = cpu_id;
        task->status = STATUS_RUNNING;
        clock_gettime(CLOCK_MONOTONIC, &task->start_time);

        cpu_monitor_adjust_active_tasks(lb->cpu_monitor, cpu_id, +1);

        int rc = pthread_create(&task->thread, &attr, task_wrapper, run);
        pthread_attr_destroy(&attr);

        if (rc != 0) {
            log_message(LOG_ERROR, "Failed to create thread for task %d", task->task_id);
            cpu_monitor_adjust_active_tasks(lb->cpu_monitor, cpu_id, -1);
            free(run);
            free(task->args);
            free_task(task);
            continue;
        }

        log_message(LOG_INFO, "Task %d (priority %d) assigned to CPU %d",
                    task->task_id, task->priority, cpu_id);
    }

    return NULL;
}

int load_balancer_active_tasks(LoadBalancer* lb) {
    (void)lb;

    pthread_mutex_lock(&active_tasks_mutex);
    int active = total_active_tasks;
    pthread_mutex_unlock(&active_tasks_mutex);

    return active;
}

void wait_for_tasks_completion(LoadBalancer* lb) {
    (void)lb;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;   /* bound the wait so shutdown cannot hang */

    pthread_mutex_lock(&active_tasks_mutex);
    while (total_active_tasks > 0) {
        if (pthread_cond_timedwait(&active_tasks_cond,
                                   &active_tasks_mutex, &deadline) == ETIMEDOUT) {
            log_message(LOG_WARNING,
                        "Timed out waiting for %d task(s) to finish",
                        total_active_tasks);
            break;
        }
    }
    pthread_mutex_unlock(&active_tasks_mutex);
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

    /* Idempotent: returns early if someone already stopped us. */
    if (atomic_exchange(&lb->running, 0) == 0) return;

    log_message(LOG_INFO, "Initiating load balancer shutdown");

    /* Unblocks the scheduler thread parked in dequeue_task(). */
    shutdown_task_queue(lb->task_queue);

    if (lb->threads_started) {
        pthread_join(lb->scheduler_thread, NULL);
        pthread_join(lb->monitor_thread, NULL);
        lb->threads_started = 0;
    }

    cancel_pending_tasks(lb);

    /* Detached task threads are still running; give them a bounded window. */
    wait_for_tasks_completion(lb);

    log_message(LOG_INFO, "Load balancer stopped successfully");
}

void cleanup_load_balancer(LoadBalancer* lb) {
    if (!lb) return;

    stop_load_balancer(lb);

    cleanup_cpu_monitor(lb->cpu_monitor);
    cleanup_task_queue(lb->task_queue);

    lb->cpu_monitor = NULL;
    lb->task_queue = NULL;
    lb->config = NULL;   /* owned by the caller, freed via free_config() */

    free(lb);
}
