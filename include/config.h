#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Forward declaration only: config.h must not depend on task.h, since task.h
 * does not depend on config.h and several translation units include config.h
 * without needing the Task layout. */
struct Task;

/*
 * How the dispatcher picks a core for a task. Three points on a spectrum from
 * "ignore load entirely" to "read as much load signal as we have":
 *
 *   ROUND_ROBIN — cycle through cores in order. Ignores load completely; the
 *   baseline every other policy is measured against.
 *   LEAST_LOAD   — current /proc/stat usage plus in-flight task counts, no
 *   history.
 *   PREDICTIVE   — LEAST_LOAD blended with a moving-average prediction of
 *   where each core's load is heading.
 */
typedef enum {
    SCHED_ROUND_ROBIN = 0,
    SCHED_LEAST_LOAD = 1,
    SCHED_PREDICTIVE = 2
} SchedulingPolicy;

/* Human-readable name for logs and benchmark output. Never NULL. */
const char* scheduling_policy_name(SchedulingPolicy policy);

/*
 * Invoked by a worker immediately after a task finishes running (status and
 * timestamps already set) and before the task is freed. `task` is only valid
 * for the duration of the call. This is the extension point benchmarking and
 * other instrumentation use to observe per-task timing without the core
 * library knowing anything about benchmarks.
 */
typedef void (*TaskCompletionHook)(const struct Task* task, void* user_data);

typedef struct {
    int max_tasks;
    int monitoring_interval_ms;
    double high_load_threshold;
    double low_load_threshold;
    int load_history_size;
    int enable_load_prediction;
    int enable_detailed_logging;
    char* log_file_path;
    int num_cpus;
    SchedulingPolicy scheduling_policy;
    /* Idle cores pull pending tasks from a busier core's queue instead of
     * sitting empty. This is what makes "dynamic load balancing" apply to
     * tasks already assigned to a core, not just to placement decisions. */
    int enable_work_stealing;
    /* Optional; NULL means no hook is called. See TaskCompletionHook above. */
    TaskCompletionHook on_task_complete;
    void* on_task_complete_user_data;
} LoadBalancerConfig;

// Initialize with default configuration
LoadBalancerConfig* init_default_config(void);

// Load configuration from file
LoadBalancerConfig* load_config(const char* config_path);

// Free configuration
void free_config(LoadBalancerConfig* config);

#endif