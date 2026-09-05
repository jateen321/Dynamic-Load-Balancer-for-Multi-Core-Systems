/*
 * Graceful shutdown: stop_load_balancer() must leave nothing in flight,
 * must be safe to call more than once on the same LoadBalancer (sequential
 * calls, not concurrent — that guarantee is documented, not implied), and
 * a handful of NULL/never-started edge cases must not crash.
 */

#include "test_common.h"
#include "load_balancer.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static void count_fn(void* args) {
    atomic_fetch_add((atomic_int*)args, 1);
}

static void count_destructor(void* args) {
    atomic_fetch_add((atomic_int*)args, 1);
}

#define TASK_COUNT 20

/* Run a full batch of tasks to completion, then call stop_load_balancer()
 * three times in a row on the same LoadBalancer (once explicitly, twice
 * more implicitly via cleanup_load_balancer(), which always calls it first)
 * and confirm none of those calls crash, hang, or change the "nothing left"
 * counts. */
static void test_stop_is_idempotent_after_normal_completion(void) {
    LoadBalancerConfig* cfg = test_config(test_clamp_cpus(4), 32, SCHED_PREDICTIVE, 1);
    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);
    CHECK(start_load_balancer(lb) == 0);

    atomic_int ran;
    atomic_init(&ran, 0);

    for (int i = 0; i < TASK_COUNT; i++) {
        CHECK(submit_task(lb, count_fn, &ran, NULL, PRIORITY_MEDIUM) == 0);
    }

    wait_for_tasks_completion(lb);
    CHECK(atomic_load(&ran) == TASK_COUNT);
    CHECK(load_balancer_pending_tasks(lb) == 0);
    CHECK(load_balancer_active_tasks(lb) == 0);

    stop_load_balancer(lb);   /* call #1: the real transition to stopped */
    CHECK(load_balancer_pending_tasks(lb) == 0);
    CHECK(load_balancer_active_tasks(lb) == 0);

    stop_load_balancer(lb);   /* call #2: must be a safe no-op, not a hang/crash */
    CHECK(load_balancer_pending_tasks(lb) == 0);
    CHECK(load_balancer_active_tasks(lb) == 0);
    CHECK(atomic_load(&ran) == TASK_COUNT);   /* nothing re-ran */

    /* A task submitted after shutdown must be rejected (the admission queue
     * was already shut down) rather than silently accepted and never run. */
    atomic_int post_shutdown_destroyed;
    atomic_init(&post_shutdown_destroyed, 0);
    CHECK(submit_task(lb, count_fn, &post_shutdown_destroyed, count_destructor,
                       PRIORITY_MEDIUM) == -1);
    CHECK(atomic_load(&post_shutdown_destroyed) == 1);

    cleanup_load_balancer(lb);   /* call #3, internally: must also be safe */
    free_config(cfg);
}

/* Never started, then stopped: stop_load_balancer() must tolerate a
 * LoadBalancer that never had workers, a dispatcher, or a monitor. */
static void test_stop_on_never_started(void) {
    LoadBalancerConfig* cfg = test_config(1, 4, SCHED_ROUND_ROBIN, 0);
    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);

    stop_load_balancer(lb);
    stop_load_balancer(lb);   /* still idempotent even in this state */
    CHECK(load_balancer_pending_tasks(lb) == 0);
    CHECK(load_balancer_active_tasks(lb) == 0);

    cleanup_load_balancer(lb);
    free_config(cfg);
}

/* A LoadBalancer that was actually started and then stopped must reject a
 * second start_load_balancer() call outright rather than silently leaking
 * the old worker pool and coming back up inert (the task queue's `shutdown`
 * flag is permanent, so a "restarted" dispatcher would see it shut down and
 * exit immediately without running anything). The rejected restart must also
 * leave the object exactly as safely-stopped as it already was — no crash,
 * and no tasks pending/active despite the failed attempt to bring it back up. */
static void test_restart_after_stop_is_rejected(void) {
    LoadBalancerConfig* cfg = test_config(test_clamp_cpus(2), 8, SCHED_ROUND_ROBIN, 0);
    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);
    CHECK(start_load_balancer(lb) == 0);

    stop_load_balancer(lb);
    CHECK(load_balancer_pending_tasks(lb) == 0);
    CHECK(load_balancer_active_tasks(lb) == 0);

    CHECK(start_load_balancer(lb) == -1);
    CHECK(load_balancer_pending_tasks(lb) == 0);
    CHECK(load_balancer_active_tasks(lb) == 0);

    /* The rejected restart must not have left anything half-started that a
     * subsequent stop/cleanup would trip over. */
    stop_load_balancer(lb);
    CHECK(load_balancer_pending_tasks(lb) == 0);
    CHECK(load_balancer_active_tasks(lb) == 0);

    cleanup_load_balancer(lb);
    free_config(cfg);
}

static void on_complete_count(const Task* task, void* user_data) {
    (void)task;
    atomic_fetch_add((atomic_int*)user_data, 1);
}

/*
 * Basic-case regression: hook fires for every task by the time
 * wait_for_tasks_completion() returns. Runs many small trials, each with a
 * fresh LoadBalancer, because the specific ordering bug this guards against
 * (see test_hook_never_observed_incomplete_by_a_tight_poller below for the
 * surgical version) is a narrow race that a single trial won't reliably hit
 * — this test exists to document the contract and catch a gross regression,
 * not as the primary proof.
 */
static void test_hook_completes_before_wait_returns(void) {
    const int trials = 100;
    const int tasks_per_trial = 16;

    for (int t = 0; t < trials; t++) {
        LoadBalancerConfig* cfg = test_config(test_clamp_cpus(4), tasks_per_trial,
                                               SCHED_PREDICTIVE, 1);
        atomic_int completed;
        atomic_init(&completed, 0);
        cfg->on_task_complete = on_complete_count;
        cfg->on_task_complete_user_data = &completed;

        LoadBalancer* lb = init_load_balancer(cfg);
        CHECK(lb != NULL);
        CHECK(start_load_balancer(lb) == 0);

        for (int i = 0; i < tasks_per_trial; i++) {
            CHECK(submit_task(lb, count_fn, &completed, NULL, PRIORITY_MEDIUM) == 0);
        }
        /* count_fn itself also increments `completed`, so the hook and the
         * task function race each other on purpose within a single task —
         * what matters is that by the time wait_for_tasks_completion()
         * returns, EVERY task has been through both, i.e. the counter reads
         * exactly 2x tasks_per_trial, not something short of it. */

        wait_for_tasks_completion(lb);
        CHECK(atomic_load(&completed) == tasks_per_trial * 2);

        cleanup_load_balancer(lb);
        free_config(cfg);
    }
}

static atomic_int g_hook_started;
static atomic_int g_hook_done;
static atomic_int g_saw_incomplete_state_as_done;

/* Deliberately slow: turns whatever gap exists between "task no longer in
 * flight" and "hook has returned" from a handful of instructions into 50ms —
 * wide enough that a tight-spinning watcher thread (below) is certain to
 * sample inside it if the gap is on the wrong side of the fix. */
static void slow_hook(const Task* task, void* user_data) {
    (void)task; (void)user_data;
    atomic_store(&g_hook_started, 1);
    usleep(50000);
    atomic_store(&g_hook_done, 1);
}

typedef struct {
    LoadBalancer* lb;
} WatcherArgs;

/* Spins (no sleep — this needs finer granularity than
 * wait_for_tasks_completion()'s own 50ms poll interval) checking whether the
 * balancer ever reports "nothing pending or active" while this test's one
 * task's hook is known to still be running. That specific combination is
 * exactly the bug: a caller relying on "nothing active" to mean "safe to
 * read what the hook accumulated" would be reading stale/incomplete data. */
static void* watch_for_premature_zero(void* arg) {
    LoadBalancer* lb = ((WatcherArgs*)arg)->lb;

    while (!atomic_load(&g_hook_done)) {
        if (atomic_load(&g_hook_started)) {
            int active = load_balancer_active_tasks(lb);
            int pending = load_balancer_pending_tasks(lb);
            /* g_hook_done is checked again, AFTER the reads, not just before
             * this iteration started: hook_done only ever transitions
             * false->true once, so if it still reads false here, it was
             * ALSO false throughout the active/pending reads just taken —
             * making a 0/0 result unambiguous evidence of the bug. Without
             * this second check, a 0/0 reading could just as easily mean the
             * hook finished in the handful of instructions between this
             * loop's own top-of-iteration check and these two reads, which
             * is a correct outcome, not a violation — that TOCTOU produced
             * exactly this test's first, wrong, version of this function. */
            if (active == 0 && pending == 0 && !atomic_load(&g_hook_done)) {
                atomic_store(&g_saw_incomplete_state_as_done, 1);
                return NULL;
            }
        }
    }
    return NULL;
}

static void noop_fn(void* args) { (void)args; }

static void test_hook_never_observed_incomplete_by_a_tight_poller(void) {
    atomic_init(&g_hook_started, 0);
    atomic_init(&g_hook_done, 0);
    atomic_init(&g_saw_incomplete_state_as_done, 0);

    LoadBalancerConfig* cfg = test_config(1, 4, SCHED_ROUND_ROBIN, 0);
    cfg->on_task_complete = slow_hook;

    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);
    CHECK(start_load_balancer(lb) == 0);

    WatcherArgs wargs = { lb };
    pthread_t watcher;
    CHECK(pthread_create(&watcher, NULL, watch_for_premature_zero, &wargs) == 0);

    CHECK(submit_task(lb, noop_fn, NULL, NULL, PRIORITY_MEDIUM) == 0);
    wait_for_tasks_completion(lb);
    /* By now the one task has fully run and its hook has returned (that is
     * exactly what wait_for_tasks_completion() must guarantee), so the
     * watcher's own loop condition is also about to become false; join it
     * rather than detach so this function doesn't return with it still
     * live. */
    pthread_join(watcher, NULL);

    CHECK(atomic_load(&g_hook_done) == 1);
    CHECK(atomic_load(&g_saw_incomplete_state_as_done) == 0);

    cleanup_load_balancer(lb);
    free_config(cfg);
}

/* NULL must be tolerated everywhere the API takes a LoadBalancer*, matching
 * every other public entry point's own `if (!lb) return` guard. */
static void test_null_lb_is_safe(void) {
    stop_load_balancer(NULL);
    cleanup_load_balancer(NULL);
    cancel_pending_tasks(NULL);
    wait_for_tasks_completion(NULL);
    CHECK(load_balancer_active_tasks(NULL) == 0);
    CHECK(load_balancer_pending_tasks(NULL) == 0);
    CHECK(init_load_balancer(NULL) == NULL);
    CHECK(start_load_balancer(NULL) == -1);
}

int main(void) {
    test_stop_is_idempotent_after_normal_completion();
    test_stop_on_never_started();
    test_restart_after_stop_is_rejected();
    test_hook_completes_before_wait_returns();
    test_hook_never_observed_incomplete_by_a_tight_poller();
    test_null_lb_is_safe();

    test_pass(__FILE__);
    return 0;
}
