#include "load_balancer.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

/*
 * sig_atomic_t is the only type the C standard guarantees can be written by a
 * signal handler and read by the interrupted code without tearing; volatile
 * stops the compiler caching it in a register across the sleep loop.
 */
static volatile sig_atomic_t running = 1;

/*
 * Async-signal-safe: it does nothing but set a flag.
 *
 * The previous version called printf(), log_message() and stop_load_balancer()
 * from here. None of those are async-signal-safe, and stop_load_balancer()
 * takes mutexes — if the signal interrupted a thread that already held one,
 * the handler would deadlock against it. The real shutdown now runs in main().
 */
static void handle_sigint(int signum) {
    (void)signum;
    running = 0;
}

// CPU task that runs for 1-3 seconds
void cpu_task(void* arg) {
    int task_id = *(int*)arg;
    time_t start_time = time(NULL);

    /* rand() keeps its state in a single shared static, so calling it from
     * several task threads at once is a data race. rand_r() keeps the state in
     * a local seeded per task. */
    unsigned int seed = (unsigned int)(time(NULL) ^ (task_id * 2654435761u));

    int duration = (rand_r(&seed) % 3) + 1;
    double result = 0.0;

    time_t current_time;
    do {
        // Perform some CPU-intensive calculations
        for (int i = 0; i < 10000; i++) {
            result += rand_r(&seed) / (double)RAND_MAX;
        }
        current_time = time(NULL);
    } while (difftime(current_time, start_time) < duration);

    log_message(LOG_INFO, "Task %d completed after %d seconds (checksum %.2f)",
                task_id, duration, result);
    /* No free(arg) here: the library owns args and destroys them via the
     * destructor passed to submit_task(). Freeing here would double-free. */
}

void print_usage(const char* program_name) {
    fprintf(stderr, "Usage: %s <num_cores> <num_tasks> [policy]\n", program_name);
    fprintf(stderr, "  num_cores: Number of CPU cores to use (1-%ld)\n", sysconf(_SC_NPROCESSORS_ONLN));
    fprintf(stderr, "  num_tasks: Number of tasks to generate\n");
    fprintf(stderr, "  policy:    round_robin | least_load | predictive (default: predictive)\n");
}

/* Returns 1 and sets *out on a recognized name, 0 otherwise. */
static int parse_policy(const char* name, SchedulingPolicy* out) {
    if (strcmp(name, "round_robin") == 0) { *out = SCHED_ROUND_ROBIN; return 1; }
    if (strcmp(name, "least_load") == 0)  { *out = SCHED_LEAST_LOAD;  return 1; }
    if (strcmp(name, "predictive") == 0)  { *out = SCHED_PREDICTIVE;  return 1; }
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        print_usage(argv[0]);
        return 1;
    }

    // Parse command line arguments
    int num_cores = atoi(argv[1]);
    int num_tasks = atoi(argv[2]);

    SchedulingPolicy policy = SCHED_PREDICTIVE;
    if (argc == 4 && !parse_policy(argv[3], &policy)) {
        fprintf(stderr, "Error: unknown policy '%s'\n", argv[3]);
        print_usage(argv[0]);
        return 1;
    }

    // Validate number of cores
    int max_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores < 1 || num_cores > max_cores) {
        fprintf(stderr, "Error: Invalid number of cores. Must be between 1 and %d\n", max_cores);
        return 1;
    }

    // Validate number of tasks
    if (num_tasks < 1) {
        fprintf(stderr, "Error: Invalid number of tasks. Must be greater than 0\n");
        return 1;
    }

    /* sigaction rather than signal(): signal()'s behaviour around handler
     * reinstatement and syscall restart varies between platforms. */
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    // Initialize load balancer with custom configuration
    LoadBalancerConfig* config = init_default_config();
    if (!config) {
        fprintf(stderr, "Failed to initialize configuration\n");
        return 1;
    }

    // Modify configuration for our needs
    config->max_tasks = num_tasks;
    config->monitoring_interval_ms = 500;  // Monitor every 500ms
    config->enable_detailed_logging = 1;
    config->num_cpus = num_cores;
    config->scheduling_policy = policy;

    LoadBalancer* lb = init_load_balancer(config);
    if (!lb) {
        fprintf(stderr, "Failed to initialize load balancer\n");
        free_config(config);
        cleanup_logger();   /* init_load_balancer opens the logger before it can fail */
        return 1;
    }

    /* Check the result: a failed start used to be silent, and main would go on
     * to submit every task into a queue no scheduler would ever drain, print a
     * full success transcript, and then hang forever in the wait loop. */
    if (start_load_balancer(lb) != 0) {
        fprintf(stderr, "Failed to start load balancer\n");
        cleanup_load_balancer(lb);
        free_config(config);
        cleanup_logger();
        return 1;
    }

    printf("Started load balancer with %d cores and %d tasks (policy: %s)\n",
           num_cores, num_tasks, scheduling_policy_name(policy));
    fflush(stdout);

    unsigned int submit_seed = (unsigned int)time(NULL);

    // Submit tasks
    int submitted = 0;
    for (int i = 0; i < num_tasks && running; i++) {
        int* task_id = malloc(sizeof(int));
        if (!task_id) break;
        *task_id = i + 1;

        /* Keep a plain copy for logging. submit_task() takes ownership of the
         * block on entry — success or failure — so reading *task_id after the
         * call is a use-after-free either way. */
        int id_for_log = *task_id;

        /* % TASK_PRIORITY_LEVELS, so PRIORITY_CRITICAL is actually reachable —
         * the original used % 3 and could never produce it. */
        TaskPriority priority =
            (TaskPriority)(rand_r(&submit_seed) % TASK_PRIORITY_LEVELS);

        /* Passing `free` as the destructor is the point of the design: the
         * ownership decision is visible at the call site instead of being an
         * unwritten rule spread across five files. */
        if (submit_task(lb, cpu_task, task_id, free, priority) == 0) {
            log_message(LOG_INFO, "Submitted task %d with priority %d", id_for_log, priority);
            printf("Submitted task %d (priority %d)\n", id_for_log, priority);
            submitted++;
        } else {
            /* No free(task_id): ownership transferred on entry, so submit_task
             * has already destroyed it. */
            log_message(LOG_ERROR, "Failed to submit task %d", id_for_log);
        }

        // Small delay between submissions to prevent overwhelming the system
        usleep(100000);  // 100ms delay
    }

    printf("All %d task(s) submitted. Waiting for completion (Ctrl+C to stop early)...\n",
           submitted);
    fflush(stdout);

    /* Exit on its own once nothing is queued (globally or on any core) and
     * nothing is executing, so a demo run terminates without needing a
     * keypress. This loop is a convenience, not a correctness mechanism:
     * cleanup_load_balancer() below blocks until every worker has drained
     * regardless. */
    while (running) {
        if (load_balancer_pending_tasks(lb) == 0 &&
            load_balancer_active_tasks(lb) == 0) {
            break;
        }
        usleep(100000);
    }

    if (!running) {
        printf("\nReceived Ctrl+C, initiating graceful shutdown...\n");
        log_message(LOG_INFO, "Received shutdown signal, initiating graceful shutdown");
    }

    /* Cleanup. This order is load-bearing, do not reorder: the first call
     * blocks until every task thread has been joined, which is what makes it
     * safe to free the config and close the log a task might still be using. */
    printf("Cleaning up...\n");
    cleanup_load_balancer(lb);
    free_config(config);
    cleanup_logger();

    printf("Successfully terminated.\n");
    return 0;
}
