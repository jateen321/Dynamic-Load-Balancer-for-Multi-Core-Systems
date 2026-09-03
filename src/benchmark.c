/*
 * Benchmark harness: runs the same synthetic workload under all three
 * scheduling policies, back to back, and compares them on completion time,
 * per-core CPU balance, throughput, task wait time, context switches and
 * work-stealing activity. A separate executable rather than a mode of
 * cpu_balancer, so a normal build/run of the balancer never pays for this.
 */

#include "load_balancer.h"
#include "config.h"
#include "task.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <math.h>

typedef struct {
    int target_ms;
} BenchTaskArgs;

/*
 * Written from the on_task_complete hook, which the library calls from
 * whichever worker thread ran the task — every field here is touched by up
 * to num_cores threads concurrently, so the mutex is not optional.
 */
typedef struct {
    pthread_mutex_t lock;
    int num_cores;
    double total_wait_ms;
    long wait_count;
    double* busy_time_by_core;   /* seconds of task-cpu_usage, summed per core */
} BenchStats;

typedef struct {
    const char* name;
    double completion_s;
    double imbalance_pct;   /* population stdev of per-core busy fraction */
    double throughput;      /* tasks/sec */
    double avg_wait_ms;
    long ctx_switches;
    long migrated;          /* tasks moved by work-stealing */
} PolicyResult;

static double ms_between(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) * 1000.0 +
           (double)(b.tv_nsec - a.tv_nsec) / 1e6;
}

/*
 * Busy-spins for approximately target_ms, checking real elapsed time
 * periodically rather than blocking — usleep() would yield the CPU and make
 * every core look idle no matter which policy is under test, which is
 * exactly the thing this benchmark needs to measure.
 */
static void spin_task(void* arg) {
    int target_ms = ((BenchTaskArgs*)arg)->target_ms;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    unsigned int seed = (unsigned int)(uintptr_t)arg ^ (unsigned int)target_ms;
    double result = 0.0;
    double elapsed_ms;

    do {
        for (int i = 0; i < 2000; i++) {
            result += rand_r(&seed) / (double)RAND_MAX;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ms = ms_between(start, now);
    } while (elapsed_ms < target_ms);

    /* log_message locks a mutex and returns immediately (LOG_DEBUG is
     * suppressed with detailed logging off), but it is an opaque call in
     * another translation unit, so it keeps the compiler from deciding the
     * accumulation above is dead and folding the whole loop away. */
    log_message(LOG_DEBUG, "bench task spun %.1fms (checksum %.2f)", elapsed_ms, result);
}

static void on_task_done(const Task* task, void* user_data) {
    BenchStats* stats = (BenchStats*)user_data;
    double wait_ms = ms_between(task->create_time, task->start_time);

    pthread_mutex_lock(&stats->lock);
    stats->total_wait_ms += wait_ms;
    stats->wait_count++;
    if (task->assigned_cpu >= 0 && task->assigned_cpu < stats->num_cores) {
        stats->busy_time_by_core[task->assigned_cpu] += task->cpu_usage;
    }
    pthread_mutex_unlock(&stats->lock);
}

/*
 * Runs num_tasks synthetic tasks under `policy` on a fresh LoadBalancer and
 * fills in *out. Returns 0 on success, -1 if any setup step failed (already
 * logged to stderr) — the caller just skips this policy and carries on.
 */
static int run_policy(SchedulingPolicy policy, int num_cores, int num_tasks,
                       int min_ms, int max_ms, unsigned int seed,
                       PolicyResult* out) {
    LoadBalancerConfig* config = init_default_config();
    if (!config) {
        fprintf(stderr, "Error: failed to allocate configuration for %s\n",
                scheduling_policy_name(policy));
        return -1;
    }

    BenchStats stats = {0};
    stats.num_cores = num_cores;
    stats.busy_time_by_core = calloc((size_t)num_cores, sizeof(double));
    if (!stats.busy_time_by_core || pthread_mutex_init(&stats.lock, NULL) != 0) {
        fprintf(stderr, "Error: failed to allocate benchmark stats for %s\n",
                scheduling_policy_name(policy));
        free(stats.busy_time_by_core);
        free_config(config);
        return -1;
    }

    /* Shared log file across the three runs; *.log is gitignored and this is
     * incidental detail, not the benchmark's output. */
    free(config->log_file_path);
    config->log_file_path = strdup("./cpu_balancer_bench.log");
    config->max_tasks = num_tasks;
    config->num_cpus = num_cores;
    config->scheduling_policy = policy;
    config->enable_detailed_logging = 0;
    config->on_task_complete = on_task_done;
    config->on_task_complete_user_data = &stats;

    LoadBalancer* lb = init_load_balancer(config);
    if (!lb) {
        fprintf(stderr, "Error: failed to initialize load balancer for %s\n",
                scheduling_policy_name(policy));
        pthread_mutex_destroy(&stats.lock);
        free(stats.busy_time_by_core);
        free_config(config);
        return -1;
    }

    if (start_load_balancer(lb) != 0) {
        fprintf(stderr, "Error: failed to start load balancer for %s\n",
                scheduling_policy_name(policy));
        cleanup_load_balancer(lb);
        pthread_mutex_destroy(&stats.lock);
        free(stats.busy_time_by_core);
        free_config(config);
        return -1;
    }

    /* A local copy of the caller's seed: rand_r() advances it in place, so
     * starting every run from the same value (not the same *variable*)
     * reproduces the identical duration sequence for all three policies. */
    unsigned int gen_seed = seed;
    unsigned int span = (unsigned int)(max_ms - min_ms + 1);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    int submitted = 0;
    for (int i = 0; i < num_tasks; i++) {
        BenchTaskArgs* args = malloc(sizeof(BenchTaskArgs));
        if (!args) break;
        args->target_ms = min_ms + (int)(rand_r(&gen_seed) % span);

        if (submit_task(lb, spin_task, args, free, PRIORITY_MEDIUM) == 0) {
            submitted++;
        }
    }

    wait_for_tasks_completion(lb);
    clock_gettime(CLOCK_MONOTONIC, &t_end);

    /* Stopped explicitly (rather than left to cleanup_load_balancer) so that
     * worker_stats below is read only after each worker's exit-time
     * getrusage() has actually run inside the join. */
    stop_load_balancer(lb);

    long ctx_switches = 0;
    long migrated = 0;
    for (int i = 0; i < num_cores; i++) {
        WorkerStats ws;
        if (load_balancer_worker_stats(lb, i, &ws) == 0) {
            ctx_switches += ws.voluntary_ctxt_switches + ws.involuntary_ctxt_switches;
            migrated += ws.tasks_stolen_by_me;
        }
    }

    cleanup_load_balancer(lb);
    free_config(config);

    double completion_s = ms_between(t_start, t_end) / 1000.0;
    if (completion_s <= 0.0) completion_s = 1e-9;   /* guard the divisions below */

    double mean_busy = 0.0;
    for (int i = 0; i < num_cores; i++) {
        mean_busy += stats.busy_time_by_core[i] / completion_s;
    }
    mean_busy /= num_cores;

    double variance = 0.0;
    for (int i = 0; i < num_cores; i++) {
        double d = (stats.busy_time_by_core[i] / completion_s) - mean_busy;
        variance += d * d;
    }
    variance /= num_cores;

    out->name = scheduling_policy_name(policy);
    out->completion_s = completion_s;
    out->imbalance_pct = sqrt(variance) * 100.0;
    out->throughput = submitted / completion_s;
    out->avg_wait_ms = stats.wait_count > 0 ? stats.total_wait_ms / stats.wait_count : 0.0;
    out->ctx_switches = ctx_switches;
    out->migrated = migrated;

    pthread_mutex_destroy(&stats.lock);
    free(stats.busy_time_by_core);

    return 0;
}

static void print_usage(const char* program_name, long max_cores) {
    fprintf(stderr, "Usage: %s [num_cores] [num_tasks] [min_ms] [max_ms] [seed]\n", program_name);
    fprintf(stderr, "  num_cores: 1-%ld (default: number of online cores)\n", max_cores);
    fprintf(stderr, "  num_tasks: synthetic tasks per policy run (default: 60)\n");
    fprintf(stderr, "  min_ms:    minimum task duration in ms (default: 20)\n");
    fprintf(stderr, "  max_ms:    maximum task duration in ms, >= min_ms (default: 150)\n");
    fprintf(stderr, "  seed:      RNG seed; the same duration sequence is reused for every policy (default: 42)\n");
}

int main(int argc, char** argv) {
    long max_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (max_cores < 1) max_cores = 1;

    int num_cores = (int)max_cores;
    int num_tasks = 60;
    int min_ms = 20;
    int max_ms = 150;
    unsigned int seed = 42;

    if (argc > 6) {
        print_usage(argv[0], max_cores);
        return 1;
    }
    if (argc > 1) num_cores = atoi(argv[1]);
    if (argc > 2) num_tasks = atoi(argv[2]);
    if (argc > 3) min_ms = atoi(argv[3]);
    if (argc > 4) max_ms = atoi(argv[4]);
    if (argc > 5) seed = (unsigned int)atoi(argv[5]);

    if (num_cores < 1 || num_cores > max_cores) {
        fprintf(stderr, "Error: num_cores must be between 1 and %ld\n", max_cores);
        print_usage(argv[0], max_cores);
        return 1;
    }
    if (num_tasks < 1) {
        fprintf(stderr, "Error: num_tasks must be greater than 0\n");
        print_usage(argv[0], max_cores);
        return 1;
    }
    if (min_ms < 1 || max_ms < min_ms) {
        fprintf(stderr, "Error: need 1 <= min_ms <= max_ms\n");
        print_usage(argv[0], max_cores);
        return 1;
    }

    printf("Benchmarking scheduling policies: %d core(s), %d task(s)/policy, "
           "duration [%d, %d] ms, seed %u\n\n",
           num_cores, num_tasks, min_ms, max_ms, seed);
    fflush(stdout);

    SchedulingPolicy policies[3] = {SCHED_ROUND_ROBIN, SCHED_LEAST_LOAD, SCHED_PREDICTIVE};
    PolicyResult results[3];
    int n_results = 0;

    for (int i = 0; i < 3; i++) {
        printf("Running %s...\n", scheduling_policy_name(policies[i]));
        fflush(stdout);

        if (run_policy(policies[i], num_cores, num_tasks, min_ms, max_ms, seed,
                        &results[n_results]) == 0) {
            n_results++;
        } else {
            fprintf(stderr, "Warning: %s run failed, skipping it\n",
                    scheduling_policy_name(policies[i]));
        }
    }
    cleanup_logger();

    if (n_results == 0) {
        fprintf(stderr, "Error: no policy run completed successfully\n");
        return 1;
    }

    printf("\n%-14s %-17s %-15s %-15s %-11s %-14s %s\n",
           "Scheduler", "Completion time", "CPU imbalance", "Throughput",
           "Avg wait", "Ctx switches", "Migrated");

    int fastest = 0, most_balanced = 0;
    for (int i = 1; i < n_results; i++) {
        if (results[i].completion_s < results[fastest].completion_s) fastest = i;
        if (results[i].imbalance_pct < results[most_balanced].imbalance_pct) most_balanced = i;
    }

    for (int i = 0; i < n_results; i++) {
        PolicyResult* r = &results[i];
        char col_time[24], col_imb[24], col_tput[24], col_wait[24];
        snprintf(col_time, sizeof(col_time), "%.1f s", r->completion_s);
        snprintf(col_imb, sizeof(col_imb), "%.1f%%", r->imbalance_pct);
        snprintf(col_tput, sizeof(col_tput), "%.1f tasks/s", r->throughput);
        snprintf(col_wait, sizeof(col_wait), "%.1f ms", r->avg_wait_ms);

        printf("%-14s %-17s %-15s %-15s %-11s %-14ld %ld\n",
               r->name, col_time, col_imb, col_tput, col_wait,
               r->ctx_switches, r->migrated);
    }

    printf("\n%s completed fastest (%.1f s); %s had the least CPU imbalance (%.1f%%).\n",
           results[fastest].name, results[fastest].completion_s,
           results[most_balanced].name, results[most_balanced].imbalance_pct);

    return 0;
}
