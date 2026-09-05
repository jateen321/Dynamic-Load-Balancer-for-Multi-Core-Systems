/*
 * Standalone driver for the pre-rewrite (thread-per-task) load balancer,
 * built against commit 3c73725 in a separate worktree.
 *
 * Uses a FIXED-work synthetic task (a set number of rounds of real
 * computation), not a wall-clock-gated spin: a task that exits as soon as
 * `elapsed >= target_ms` finishes in ~target_ms of WALL time almost
 * regardless of how many other threads are contending for the CPU at once,
 * which would hide exactly the cost this comparison exists to measure —
 * thread-per-task's unbounded concurrency (every queued task gets its own
 * thread immediately, with no cap at num_cores) oversubscribing the CPU.
 * Fixed CPU work makes contention show up as real added wall-clock time.
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

/* From calibration on this machine: ~0.0107 ms of solo CPU time per round
 * (2000 rand_r() calls). Kept identical in the new-design driver so both
 * sides run the exact same amount of CPU work per task. */
#define MS_PER_ROUND 0.010684

/* The old API has no completion hook, so this is the only way to find out
 * how many submitted tasks actually ran their full workload versus being
 * silently dropped (e.g. by find_best_cpu() failing to place them). */
static atomic_long g_completed = 0;

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
    atomic_fetch_add(&g_completed, 1);
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
    free(config->log_file_path);
    config->log_file_path = strdup("./old_bench.log");

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

    while (task_queue_size(lb->task_queue) != 0 || load_balancer_active_tasks(lb) != 0) {
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
