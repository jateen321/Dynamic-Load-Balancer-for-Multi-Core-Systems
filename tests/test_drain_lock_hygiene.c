/*
 * Regression test for "destructor runs while holding the queue lock":
 * drain_task_queue() and core_queue_drain() must detach every queued task
 * from the queue's internal structure under the lock, then release it
 * BEFORE calling free_task() (which runs the caller's arbitrary args_free
 * destructor). Each destructor below takes a small deliberate delay so a
 * concurrent probe thread has a real window to observe the lock free
 * *between* destructor calls — not just before the first one starts or
 * after the last one finishes, which a broken (lock-held-throughout)
 * implementation could still slip past.
 *
 * The probe uses task_queue_size()/core_queue_size(), which each take the
 * same mutex the drain functions use, just briefly. If the fix holds, the
 * probe can complete that call at a point where some but not all
 * destructors have finished — proof the drain isn't one unbroken critical
 * section spanning every destructor call. Against the old, buggy code this
 * window never opens (the probe only ever succeeds once every destructor —
 * and the whole drain — is already done), so this test fails without the
 * fix, which is the point of it.
 */

#include "test_common.h"
#include "task_queue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#define TASK_COUNT 6
/* 25ms/task * 6 tasks = 150ms serialized. Long enough for a tight polling
 * probe on another thread to reliably land inside the window; short enough
 * that the test suite doesn't feel it. */
#define DESTRUCTOR_DELAY_US 25000

static void noop_fn(void* args) {
    (void)args;
}

/* Records completion into the counter passed as `args`, after a delay that
 * stands in for "real cleanup work" (freeing a complex structure, closing a
 * file descriptor, logging, etc. — whatever an actual caller might do). */
static void slow_destructor(void* args) {
    atomic_int* counter = (atomic_int*)args;
    usleep(DESTRUCTOR_DELAY_US);
    atomic_fetch_add(counter, 1);
}

/* ---------------------------------------------------------------------------
 * TaskQueue
 * ------------------------------------------------------------------------ */

typedef struct {
    TaskQueue* queue;
    int dropped;
} DrainTQArg;

static void* drain_tq_thread(void* arg) {
    DrainTQArg* d = (DrainTQArg*)arg;
    d->dropped = drain_task_queue(d->queue);
    return NULL;
}

typedef struct {
    TaskQueue* queue;
    atomic_int* done_count;
    atomic_int* saw_partial_window;
} ProbeTQArg;

static void* probe_tq_thread(void* arg) {
    ProbeTQArg* p = (ProbeTQArg*)arg;
    while (atomic_load(p->done_count) < TASK_COUNT) {
        /* Succeeds only when queue->mutex is free right now. */
        (void)task_queue_size(p->queue);
        int done_after = atomic_load(p->done_count);
        if (done_after > 0 && done_after < TASK_COUNT) {
            atomic_store(p->saw_partial_window, 1);
        }
    }
    return NULL;
}

static void test_drain_task_queue_releases_lock_between_destructors(void) {
    TaskQueue* q = init_task_queue(TASK_COUNT + 1);
    CHECK(q != NULL);

    atomic_int destroyed;
    atomic_init(&destroyed, 0);

    for (int i = 0; i < TASK_COUNT; i++) {
        Task* t = create_task(noop_fn, &destroyed, slow_destructor, PRIORITY_MEDIUM);
        CHECK(t != NULL);
        CHECK(enqueue_task(q, t) == 0);
    }
    CHECK(task_queue_size(q) == TASK_COUNT);

    atomic_int saw_partial_window;
    atomic_init(&saw_partial_window, 0);

    DrainTQArg drain_arg = { .queue = q, .dropped = -1 };
    ProbeTQArg probe_arg = { .queue = q, .done_count = &destroyed,
                              .saw_partial_window = &saw_partial_window };

    pthread_t drain_th, probe_th;
    CHECK(pthread_create(&probe_th, NULL, probe_tq_thread, &probe_arg) == 0);
    CHECK(pthread_create(&drain_th, NULL, drain_tq_thread, &drain_arg) == 0);

    pthread_join(drain_th, NULL);
    pthread_join(probe_th, NULL);

    CHECK(drain_arg.dropped == TASK_COUNT);
    CHECK(atomic_load(&destroyed) == TASK_COUNT);
    CHECK(atomic_load(&saw_partial_window) == 1);

    cleanup_task_queue(q);
}

/* ---------------------------------------------------------------------------
 * CoreQueue
 * ------------------------------------------------------------------------ */

typedef struct {
    CoreQueue* queue;
    int dropped;
} DrainCQArg;

static void* drain_cq_thread(void* arg) {
    DrainCQArg* d = (DrainCQArg*)arg;
    d->dropped = core_queue_drain(d->queue);
    return NULL;
}

typedef struct {
    CoreQueue* queue;
    atomic_int* done_count;
    atomic_int* saw_partial_window;
} ProbeCQArg;

static void* probe_cq_thread(void* arg) {
    ProbeCQArg* p = (ProbeCQArg*)arg;
    while (atomic_load(p->done_count) < TASK_COUNT) {
        (void)core_queue_size(p->queue);
        int done_after = atomic_load(p->done_count);
        if (done_after > 0 && done_after < TASK_COUNT) {
            atomic_store(p->saw_partial_window, 1);
        }
    }
    return NULL;
}

static void test_core_queue_drain_releases_lock_between_destructors(void) {
    CoreQueue* q = init_core_queue();
    CHECK(q != NULL);

    atomic_int destroyed;
    atomic_init(&destroyed, 0);

    for (int i = 0; i < TASK_COUNT; i++) {
        Task* t = create_task(noop_fn, &destroyed, slow_destructor, PRIORITY_MEDIUM);
        CHECK(t != NULL);
        CHECK(core_queue_push(q, t) == 0);
    }
    CHECK(core_queue_size(q) == TASK_COUNT);

    atomic_int saw_partial_window;
    atomic_init(&saw_partial_window, 0);

    DrainCQArg drain_arg = { .queue = q, .dropped = -1 };
    ProbeCQArg probe_arg = { .queue = q, .done_count = &destroyed,
                              .saw_partial_window = &saw_partial_window };

    pthread_t drain_th, probe_th;
    CHECK(pthread_create(&probe_th, NULL, probe_cq_thread, &probe_arg) == 0);
    CHECK(pthread_create(&drain_th, NULL, drain_cq_thread, &drain_arg) == 0);

    pthread_join(drain_th, NULL);
    pthread_join(probe_th, NULL);

    CHECK(drain_arg.dropped == TASK_COUNT);
    CHECK(atomic_load(&destroyed) == TASK_COUNT);
    CHECK(atomic_load(&saw_partial_window) == 1);

    cleanup_core_queue(q);
}

int main(void) {
    test_drain_task_queue_releases_lock_between_destructors();
    test_core_queue_drain_releases_lock_between_destructors();

    test_pass(__FILE__);
    return 0;
}
