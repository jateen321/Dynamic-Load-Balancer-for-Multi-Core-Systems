/*
 * Graceful shutdown: stop_load_balancer() must leave nothing in flight,
 * must be safe to call more than once on the same LoadBalancer (sequential
 * calls, not concurrent — that guarantee is documented, not implied), and
 * a handful of NULL/never-started edge cases must not crash.
 */

#include "test_common.h"
#include "load_balancer.h"

#include <stdatomic.h>

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
    test_null_lb_is_safe();

    test_pass(__FILE__);
    return 0;
}
