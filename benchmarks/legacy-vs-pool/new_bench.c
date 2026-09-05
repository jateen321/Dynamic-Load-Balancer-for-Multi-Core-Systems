/*
 * Companion to old_bench.c, built against the current (worker-pool) design.
 * Deliberately a separate small driver rather than a mode of
 * src/benchmark.c: src/benchmark.c's spin_task is wall-clock-gated, which is
 * the right choice for comparing scheduling policies against each other
 * within this design's fixed-size worker pool (no oversubscription is
 * possible there either way), but the wrong choice for this specific
 * comparison against a design with unbounded concurrency — see old_bench.c's
 * header comment. This file uses the identical fixed-work task instead, so
 * the two sides do the exact same amount of CPU work per task.
 */

#include "load_balancer.h"
#include "config.h"
#include "task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>

#define MS_PER_ROUND 0.010684

static atomic_long g_completed = 0;

static void on_task_done(const Task* task, void* user_data) {
    (void)task; (void)user_data;
    atomic_fetch_add(&g_completed, 1);
}

typedef struct {
    int rounds;
} BenchTaskArgs;

static double ms_between(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) * 1000.0 +
           (double)(b.tv_nsec - a.tv_nsec) / 1e6;
}

static void spin_task(void* arg) {
    int rounds = ((BenchTaskArgs*)arg)->rounds;
    unsigned int seed = (unsigned int)(uintptr_t)arg ^ (unsigned int)rounds;
    double result = 0.0;

    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < 2000; i++) {
            result += rand_r(&seed) / (double)RAND_MAX;
        }
    }
    (void)result;
}

int main(int argc, char** argv) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <num_cores> <num_tasks> <min_ms> <max_ms> <seed>\n", argv[0]);
        return 1;
    }

    int num_cores = atoi(argv[1]);
    int num_tasks = atoi(argv[2]);
    int min_ms = atoi(argv[3]);
    int max_ms = atoi(argv[4]);
    unsigned int seed = (unsigned int)atoi(argv[5]);

    int min_rounds = (int)(min_ms / MS_PER_ROUND);
    int max_rounds = (int)(max_ms / MS_PER_ROUND);
    if (min_rounds < 1) min_rounds = 1;
    if (max_rounds < min_rounds) max_rounds = min_rounds;

    LoadBalancerConfig* config = init_default_config();
    if (!config) { fprintf(stderr, "config init failed\n"); return 1; }

    config->max_tasks = num_tasks;
    config->num_cpus = num_cores;
    config->enable_detailed_logging = 0;
    config->scheduling_policy = SCHED_PREDICTIVE;   /* matches old design's default (prediction blended in) */
    config->on_task_complete = on_task_done;
    free(config->log_file_path);
    config->log_file_path = strdup("./new_bench.log");

    LoadBalancer* lb = init_load_balancer(config);
    if (!lb) { fprintf(stderr, "lb init failed\n"); return 1; }

    if (start_load_balancer(lb) != 0) {
        fprintf(stderr, "start failed\n");
        cleanup_load_balancer(lb);
        free_config(config);
        return 1;
    }

    unsigned int gen_seed = seed;
    unsigned int span = (unsigned int)(max_rounds - min_rounds + 1);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    int submitted = 0;
    for (int i = 0; i < num_tasks; i++) {
        BenchTaskArgs* args = malloc(sizeof(BenchTaskArgs));
        if (!args) break;
        args->rounds = min_rounds + (int)(rand_r(&gen_seed) % span);

        if (submit_task(lb, spin_task, args, free, PRIORITY_MEDIUM) == 0) {
            submitted++;
        }
    }

    while (load_balancer_pending_tasks(lb) != 0 || load_balancer_active_tasks(lb) != 0) {
        usleep(1000);
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);

    double completion_s = ms_between(t_start, t_end) / 1000.0;
    if (completion_s <= 0.0) completion_s = 1e-9;
    double throughput = submitted / completion_s;

    printf("%d,%d,%d,%ld,%.4f,%.2f\n", num_cores, num_tasks, submitted,
           atomic_load(&g_completed), completion_s, throughput);

    cleanup_load_balancer(lb);
    free_config(config);
    return 0;
}
