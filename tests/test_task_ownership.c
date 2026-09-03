/*
 * submit_task's ownership contract (README "Argument ownership"): it takes
 * ownership of `args` on entry, and the destructor runs exactly once on
 * every path — success, submit failure, and cancellation at shutdown. Each
 * scenario below uses its own atomic counter so "exactly once" is checked
 * per-path rather than just in aggregate.
 */

#include "test_common.h"
#include "load_balancer.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

static void count_destructor(void* args) {
    atomic_int* counter = (atomic_int*)args;
    atomic_fetch_add(counter, 1);
}

static void noop_fn(void* args) {
    (void)args;
}

/* Normal completion: destructor must fire exactly once, after the task ran. */
static void test_destructor_fires_once_on_success(void) {
    LoadBalancerConfig* cfg = test_config(test_clamp_cpus(2), 16, SCHED_ROUND_ROBIN, 1);
    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);
    CHECK(start_load_balancer(lb) == 0);

    atomic_int destroyed;
    atomic_init(&destroyed, 0);

    CHECK(submit_task(lb, noop_fn, &destroyed, count_destructor, PRIORITY_MEDIUM) == 0);

    wait_for_tasks_completion(lb);

    CHECK(atomic_load(&destroyed) == 1);
    CHECK(load_balancer_pending_tasks(lb) == 0);
    CHECK(load_balancer_active_tasks(lb) == 0);

    cleanup_load_balancer(lb);
    free_config(cfg);
}

/* lb == NULL: submit_task must destroy args synchronously and return -1,
 * per its documented "on return, success OR failure" contract — there is no
 * background thread here to eventually free it, so this has to happen
 * before submit_task returns. */
static void test_destructor_fires_once_on_null_lb(void) {
    atomic_int destroyed;
    atomic_init(&destroyed, 0);

    CHECK(submit_task(NULL, noop_fn, &destroyed, count_destructor, PRIORITY_MEDIUM) == -1);
    CHECK(atomic_load(&destroyed) == 1);
}

/* function == NULL is the other half of submit_task's `!lb || !function`
 * guard; same synchronous-destruction contract applies. */
static void test_destructor_fires_once_on_null_function(void) {
    LoadBalancerConfig* cfg = test_config(1, 4, SCHED_ROUND_ROBIN, 0);
    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);

    atomic_int destroyed;
    atomic_init(&destroyed, 0);

    CHECK(submit_task(lb, NULL, &destroyed, count_destructor, PRIORITY_MEDIUM) == -1);
    CHECK(atomic_load(&destroyed) == 1);

    cleanup_load_balancer(lb);
    free_config(cfg);
}

typedef struct {
    LoadBalancer* lb;
    atomic_int* destroyed;
    int rc;
} BlockedSubmitArg;

static void* blocked_submit_thread(void* arg) {
    BlockedSubmitArg* b = (BlockedSubmitArg*)arg;
    b->rc = submit_task(b->lb, noop_fn, b->destroyed, count_destructor, PRIORITY_MEDIUM);
    return NULL;
}

/*
 * Queue-full-then-shutdown rejection: a LoadBalancer that is never started
 * has no dispatcher draining its admission queue, so a second submit_task
 * against a capacity-1 queue blocks inside enqueue_task's cond_wait. Calling
 * stop_load_balancer() then wakes it via shutdown rather than by freeing a
 * slot, so enqueue_task returns -1 and submit_task must free_task (running
 * the destructor) before returning -1 itself. The first, already-queued
 * task gets its destructor run too, via cancel_pending_tasks()'s drain — a
 * second, independently-counted path to the same "exactly once" guarantee.
 */
static void test_destructor_fires_once_on_queue_full_then_shutdown(void) {
    LoadBalancerConfig* cfg = test_config(1, /*max_tasks=*/1, SCHED_ROUND_ROBIN, 0);
    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);
    /* Deliberately not started: nothing will ever dequeue the first task. */

    atomic_int destroyed_first;
    atomic_init(&destroyed_first, 0);
    atomic_int destroyed_second;
    atomic_init(&destroyed_second, 0);

    CHECK(submit_task(lb, noop_fn, &destroyed_first, count_destructor, PRIORITY_LOW) == 0);

    BlockedSubmitArg barg = { .lb = lb, .destroyed = &destroyed_second, .rc = -99 };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, blocked_submit_thread, &barg) == 0);

    /* Give the second submit_task time to actually reach the blocking
     * cond_wait before shutdown races it — see test_task_queue_capacity.c
     * for why this is the standard way to make "didn't actually block" a
     * visible failure. The outcome asserted below holds either way (see
     * the file-level comment), this just raises confidence the blocking
     * path itself was exercised. */
    usleep(150000);

    stop_load_balancer(lb);
    pthread_join(th, NULL);

    CHECK(barg.rc == -1);
    CHECK(atomic_load(&destroyed_second) == 1);
    CHECK(atomic_load(&destroyed_first) == 1);

    cleanup_load_balancer(lb);
    free_config(cfg);
}

/*
 * Cancelled at shutdown: tasks are submitted faster than a single worker can
 * run them (each sleeps briefly), so most are still sitting in the admission
 * queue or a CoreQueue when stop_load_balancer() is called immediately
 * after the submit loop. Every one of them must still have its destructor
 * run exactly once, whether it got to run or was cancelled.
 */
static void sleep_briefly(void* args) {
    (void)args;
    usleep(20000);
}

#define CANCEL_TASK_COUNT 30

static void test_destructor_fires_once_on_shutdown_cancel(void) {
    LoadBalancerConfig* cfg = test_config(1, CANCEL_TASK_COUNT + 5, SCHED_ROUND_ROBIN, 0);
    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);
    CHECK(start_load_balancer(lb) == 0);

    atomic_int destroyed;
    atomic_init(&destroyed, 0);

    for (int i = 0; i < CANCEL_TASK_COUNT; i++) {
        CHECK(submit_task(lb, sleep_briefly, &destroyed, count_destructor,
                           PRIORITY_MEDIUM) == 0);
    }

    /* No wait here on purpose: with one worker and 20ms/task, stopping
     * immediately after submission guarantees most of these are still
     * pending (queued or in a CoreQueue), which is exactly the path this
     * test exists to cover. */
    stop_load_balancer(lb);

    CHECK(atomic_load(&destroyed) == CANCEL_TASK_COUNT);
    CHECK(load_balancer_pending_tasks(lb) == 0);
    CHECK(load_balancer_active_tasks(lb) == 0);

    cleanup_load_balancer(lb);
    free_config(cfg);
}

int main(void) {
    test_destructor_fires_once_on_success();
    test_destructor_fires_once_on_null_lb();
    test_destructor_fires_once_on_null_function();
    test_destructor_fires_once_on_queue_full_then_shutdown();
    test_destructor_fires_once_on_shutdown_cancel();

    test_pass(__FILE__);
    return 0;
}
