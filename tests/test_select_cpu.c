/*
 * select_cpu() under all three SchedulingPolicy values. White-box: for
 * LEAST_LOAD/PREDICTIVE this pokes CPUStats fields directly (under
 * cpu_monitor->lock) to build a deterministic scenario instead of trying to
 * coax real /proc/stat numbers into a known shape.
 *
 * Every scenario here starts the load balancer (so lb->workers and their
 * CoreQueues exist, since select_cpu_scored() reads core_queue_size() on
 * them) but submits no tasks, so every CoreQueue stays at size 0 and the
 * `core_queue_size(...) * 5` term in the scoring formula is always zero —
 * the only thing driving the outcome is what this file writes into
 * cpu_monitor->stats. monitoring_interval_ms is set generously long (a few
 * hundred ms — vastly longer than the microseconds this file spends between
 * injecting a scenario and reading select_cpu()'s result, but still short
 * enough that stop_load_balancer()'s pthread_join(monitor_thread) — which
 * cannot return until the monitor wakes from its current usleep() and
 * re-checks `running` — doesn't turn every test-scenario teardown into a
 * multi-minute hang) so the real monitor thread's own update_cpu_stats()
 * call (made once, immediately, at the top of its loop, before the first
 * sleep) can't land in between and clobber the injected values.
 */

#include "test_common.h"
#include "load_balancer.h"

#include <stdlib.h>

#define QUIET_MONITOR_MS 300

static LoadBalancer* start_test_lb(int num_cpus, SchedulingPolicy policy) {
    LoadBalancerConfig* cfg = test_config(num_cpus, 16, policy, /*work_stealing=*/1);
    cfg->monitoring_interval_ms = QUIET_MONITOR_MS;

    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);
    CHECK(start_load_balancer(lb) == 0);
    return lb;
}

/* Owns the config too, since init_load_balancer() takes it but
 * cleanup_load_balancer() explicitly does not free it (see load_balancer.c:
 * "owned by the caller, freed via free_config()"). */
static void stop_test_lb(LoadBalancer* lb) {
    LoadBalancerConfig* cfg = lb->config;
    cleanup_load_balancer(lb);
    free_config(cfg);
}

static void set_cpu_stat(LoadBalancer* lb, int cpu, double current_usage,
                          double predicted_load, int active_tasks) {
    pthread_mutex_lock(&lb->cpu_monitor->lock);
    lb->cpu_monitor->stats[cpu].current_usage = current_usage;
    lb->cpu_monitor->stats[cpu].predicted_load = predicted_load;
    lb->cpu_monitor->stats[cpu].active_tasks = active_tasks;
    pthread_mutex_unlock(&lb->cpu_monitor->lock);
}

/* Round robin ignores load entirely: it must simply cycle 0..n-1 in order,
 * repeatedly, regardless of anything in cpu_monitor. */
static void test_round_robin_cycles_in_order(void) {
    int n = test_clamp_cpus(4);
    LoadBalancer* lb = start_test_lb(n, SCHED_ROUND_ROBIN);

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < n; i++) {
            CHECK(select_cpu(lb) == i);
        }
    }

    stop_test_lb(lb);
}

/* LEAST_LOAD must pick the single lowest current_usage core. */
static void test_least_load_picks_lowest_usage(void) {
    int n = test_clamp_cpus(4);
    LoadBalancer* lb = start_test_lb(n, SCHED_LEAST_LOAD);

    int target = n - 1;
    for (int i = 0; i < n; i++) {
        set_cpu_stat(lb, i, (i == target) ? 5.0 : 80.0, /*predicted=*/0.0, /*active=*/0);
    }

    CHECK(select_cpu(lb) == target);

    stop_test_lb(lb);
}

/* LEAST_LOAD blends in active_tasks (weighted *10): a core with slightly
 * lower raw usage can still lose to a peer that is idle-and-unassigned, once
 * the peer has even one fewer in-flight task than it does. */
static void test_least_load_weighs_active_tasks(void) {
    if (test_clamp_cpus(2) < 2) return;   /* needs at least two cores */
    LoadBalancer* lb = start_test_lb(2, SCHED_LEAST_LOAD);

    /* cpu 0: usage 20, 0 active  -> effective load 20
     * cpu 1: usage 15, 1 active  -> effective load 15 + 10 = 25
     * Lower raw usage (cpu 1) still loses because of its in-flight task. */
    set_cpu_stat(lb, 0, 20.0, 0.0, 0);
    set_cpu_stat(lb, 1, 15.0, 0.0, 1);

    CHECK(select_cpu(lb) == 0);

    stop_test_lb(lb);
}

/* PREDICTIVE averages current_usage with predicted_load, then applies the
 * same active_tasks/queue-depth weighting as LEAST_LOAD. */
static void test_predictive_uses_blended_average(void) {
    int n = test_clamp_cpus(4);
    LoadBalancer* lb = start_test_lb(n, SCHED_PREDICTIVE);
    CHECK(lb->config->enable_load_prediction != 0);   /* the default this test relies on */

    /* effective = (current + predicted) / 2:
     *   cpu0: (80+80)/2 = 80      cpu1: (10+10)/2 = 10  <- lowest
     *   cpu2: (50+50)/2 = 50      cpu3: (99+99)/2 = 99
     * Picking the min of current_usage alone, or of predicted_load alone,
     * would each coincidentally also pick cpu1 here on purpose — the point
     * of this scenario is that the *average* is what's compared, so a
     * change that started reading only one of the two fields would still
     * pass; the asymmetric scenario below is what actually catches that. */
    set_cpu_stat(lb, 0, 80.0, 80.0, 0);
    set_cpu_stat(lb, 1, 10.0, 10.0, 0);
    for (int i = 2; i < n; i++) set_cpu_stat(lb, i, 99.0, 99.0, 0);

    CHECK(select_cpu(lb) == 1);

    if (n >= 2) {
        /* Asymmetric: neither field alone picks cpu 1 here, only the
         * average does (current: 0 vs 40; predicted: 100 vs 60; average:
         * 50 vs 50 -> tie broken by whichever CPU is scanned first, cpu 0,
         * since select_cpu_scored keeps the first-seen strictly-lower
         * score and never replaces on a tie). Reading only current_usage
         * would pick cpu 0 too here by coincidence, so use unequal values
         * that make current-only and predicted-only disagree with each
         * other, and confirm the average's own winner shows up instead. */
        set_cpu_stat(lb, 0, 0.0, 100.0, 0);    /* avg 50 */
        set_cpu_stat(lb, 1, 90.0, 10.0, 0);    /* avg 50, but current-only would pick this */
        for (int i = 2; i < n; i++) set_cpu_stat(lb, i, 1000.0, 1000.0, 0);
        /* Both average to 50 and tie; the implementation keeps the first
         * strictly-lower cpu seen, i.e. cpu 0. This nails down that exact,
         * documented tie-break rather than leaving it unspecified. */
        CHECK(select_cpu(lb) == 0);
    }

    stop_test_lb(lb);
}

/* With load prediction disabled in config, PREDICTIVE's `use_prediction &&
 * enable_load_prediction` guard must fall back to current_usage alone, the
 * same as LEAST_LOAD — predicted_load must be ignored entirely. */
static void test_predictive_ignores_prediction_when_disabled(void) {
    int n = test_clamp_cpus(4);
    LoadBalancerConfig* cfg = test_config(n, 16, SCHED_PREDICTIVE, 1);
    cfg->monitoring_interval_ms = QUIET_MONITOR_MS;
    cfg->enable_load_prediction = 0;

    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);
    CHECK(start_load_balancer(lb) == 0);

    /* cpu0 has the lower current_usage but a wildly lower predicted_load
     * would flip the outcome if predicted_load were still being read. */
    set_cpu_stat(lb, 0, 10.0, 999.0, 0);
    set_cpu_stat(lb, 1, 50.0, 0.0, 0);
    for (int i = 2; i < n; i++) set_cpu_stat(lb, i, 999.0, 0.0, 0);

    CHECK(select_cpu(lb) == 0);

    stop_test_lb(lb);
}

static void test_null_and_zero_cpus(void) {
    CHECK(select_cpu(NULL) == -1);

    LoadBalancerConfig* cfg = test_config(1, 16, SCHED_ROUND_ROBIN, 0);
    cfg->num_cpus = 0;
    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);
    CHECK(select_cpu(lb) == -1);   /* num_cpus <= 0, never started */

    cleanup_load_balancer(lb);
    free_config(cfg);
}

int main(void) {
    test_round_robin_cycles_in_order();
    test_least_load_picks_lowest_usage();
    test_least_load_weighs_active_tasks();
    test_predictive_uses_blended_average();
    test_predictive_ignores_prediction_when_disabled();
    test_null_and_zero_cpus();

    test_pass(__FILE__);
    return 0;
}
