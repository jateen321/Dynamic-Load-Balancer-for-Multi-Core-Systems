/*
 * CoreQueue: FIFO from the owner's end, steal-refuses-at-<=1, steal-takes-
 * the-tail, and a concurrent push/pop/steal stress check for good measure
 * (this structure's whole reason to exist is being shared between an owner
 * and a thief without a global lock serializing everything, so it is worth
 * exercising under contention, not just single-threaded).
 */

#include "test_common.h"
#include "task_queue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

static void noop(void* args) {
    (void)args;
}

static Task* make_tagged_task(int tag) {
    int* payload = malloc(sizeof(int));
    CHECK(payload != NULL);
    *payload = tag;
    Task* t = create_task(noop, payload, free, PRIORITY_MEDIUM);
    CHECK(t != NULL);
    return t;
}

/* The owner pops from the head; the dispatcher (or, here, the test) pushes
 * at the tail. So push order must equal pop_own order. */
static void test_fifo_from_owner(void) {
    CoreQueue* q = init_core_queue();
    CHECK(q != NULL);

    for (int i = 0; i < 5; i++) {
        CHECK(core_queue_push(q, make_tagged_task(i)) == 0);
    }
    CHECK(core_queue_size(q) == 5);

    for (int i = 0; i < 5; i++) {
        /* timeout_ms == 0: purely a poll, there is always something queued
         * here so it must never actually block. */
        Task* t = core_queue_pop_own(q, 0);
        CHECK(t != NULL);
        CHECK(*(int*)t->args == i);
        free_task(t);
    }
    CHECK(core_queue_pop_own(q, 0) == NULL);

    cleanup_core_queue(q);
}

/* core_queue_try_steal must refuse to take the owner's only task, and once
 * there is more than one, must take from the tail (the end opposite the
 * owner's head), leaving the owner's next pop_own untouched. */
static void test_steal_refuses_single_takes_tail(void) {
    CoreQueue* q = init_core_queue();
    CHECK(q != NULL);

    Task* a = make_tagged_task(100);
    CHECK(core_queue_push(q, a) == 0);
    CHECK(core_queue_try_steal(q) == NULL);   /* count == 1: refused */

    Task* b = make_tagged_task(200);
    CHECK(core_queue_push(q, b) == 0);        /* count == 2 now */

    Task* stolen = core_queue_try_steal(q);
    CHECK(stolen != NULL);
    CHECK(*(int*)stolen->args == 200);        /* took the tail, i.e. `b` */
    free_task(stolen);

    CHECK(core_queue_size(q) == 1);
    CHECK(core_queue_try_steal(q) == NULL);   /* back down to 1: refused again */

    Task* remaining = core_queue_pop_own(q, 0);
    CHECK(remaining != NULL);
    CHECK(*(int*)remaining->args == 100);     /* `a`, untouched by the steal */
    free_task(remaining);

    cleanup_core_queue(q);
}

/* --------------------------------------------------------------------------
 * Concurrent stress: several pushers, one owner popping its own head, one
 * thief stealing from the tail, running at once. Every pushed tag must come
 * out exactly once between the owner and the thief combined — no task lost
 * to a race between pop_own and try_steal, none duplicated.
 * ------------------------------------------------------------------------ */

#define STRESS_PUSHERS 4
#define STRESS_PER_PUSHER 250
#define STRESS_TOTAL (STRESS_PUSHERS * STRESS_PER_PUSHER)

typedef struct {
    CoreQueue* queue;
    int base_tag;
} PusherArg;

static void* stress_pusher(void* arg) {
    PusherArg* p = (PusherArg*)arg;
    for (int i = 0; i < STRESS_PER_PUSHER; i++) {
        CHECK(core_queue_push(p->queue, make_tagged_task(p->base_tag + i)) == 0);
    }
    return NULL;
}

typedef struct {
    CoreQueue* queue;
    atomic_int* seen;        /* STRESS_TOTAL flags, one per tag */
    atomic_int* collected;   /* running total across owner + thief */
} DrainerArg;

/* Records one popped task: marks its tag seen (catching duplicates) and
 * bumps the shared collected count. Shared between the owner-pop loop and
 * the thief-steal loop below. */
static void record(atomic_int* seen, atomic_int* collected, Task* t) {
    int tag = *(int*)t->args;
    CHECK(tag >= 0 && tag < STRESS_TOTAL);
    /* exchange, not load-then-store: two drainers could otherwise both see
     * "0" for the same tag if this weren't atomic. */
    int prior = atomic_exchange(&seen[tag], 1);
    CHECK(prior == 0);   /* never popped before, from either end */
    atomic_fetch_add(collected, 1);
    free_task(t);
}

static void* stress_owner(void* arg) {
    DrainerArg* d = (DrainerArg*)arg;
    while (atomic_load(d->collected) < STRESS_TOTAL) {
        Task* t = core_queue_pop_own(d->queue, 5);
        if (t) record(d->seen, d->collected, t);
    }
    return NULL;
}

static void* stress_thief(void* arg) {
    DrainerArg* d = (DrainerArg*)arg;
    while (atomic_load(d->collected) < STRESS_TOTAL) {
        Task* t = core_queue_try_steal(d->queue);
        if (t) record(d->seen, d->collected, t);
    }
    return NULL;
}

static void test_concurrent_push_pop_steal(void) {
    CoreQueue* q = init_core_queue();
    CHECK(q != NULL);

    atomic_int seen[STRESS_TOTAL];
    for (int i = 0; i < STRESS_TOTAL; i++) atomic_init(&seen[i], 0);
    atomic_int collected;
    atomic_init(&collected, 0);

    pthread_t pushers[STRESS_PUSHERS];
    PusherArg pusher_args[STRESS_PUSHERS];
    for (int i = 0; i < STRESS_PUSHERS; i++) {
        pusher_args[i].queue = q;
        pusher_args[i].base_tag = i * STRESS_PER_PUSHER;
        CHECK(pthread_create(&pushers[i], NULL, stress_pusher, &pusher_args[i]) == 0);
    }

    DrainerArg drainer_arg = { .queue = q, .seen = seen, .collected = &collected };
    pthread_t owner, thief;
    CHECK(pthread_create(&owner, NULL, stress_owner, &drainer_arg) == 0);
    CHECK(pthread_create(&thief, NULL, stress_thief, &drainer_arg) == 0);

    for (int i = 0; i < STRESS_PUSHERS; i++) pthread_join(pushers[i], NULL);
    pthread_join(owner, NULL);
    pthread_join(thief, NULL);

    CHECK(atomic_load(&collected) == STRESS_TOTAL);
    for (int i = 0; i < STRESS_TOTAL; i++) {
        CHECK(atomic_load(&seen[i]) == 1);
    }
    CHECK(core_queue_size(q) == 0);

    cleanup_core_queue(q);
}

int main(void) {
    test_fifo_from_owner();
    test_steal_refuses_single_takes_tail();
    test_concurrent_push_pop_steal();

    test_pass(__FILE__);
    return 0;
}
