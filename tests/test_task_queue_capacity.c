/*
 * TaskQueue capacity: enqueue_task must block a producer while the queue is
 * at capacity, wake it once room appears, and also wake it (with a failure
 * return) on shutdown rather than leaving it parked forever.
 */

#include "test_common.h"
#include "task_queue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

static void noop(void* args) {
    (void)args;
}

static Task* make_task(TaskPriority priority) {
    Task* t = create_task(noop, NULL, NULL, priority);
    CHECK(t != NULL);
    return t;
}

typedef struct {
    TaskQueue* queue;
    Task* task;
    int rc;                 /* enqueue_task's return, valid only after join */
    atomic_int returned;    /* flipped just after enqueue_task returns */
} ProducerArg;

static void* producer_thread(void* arg) {
    ProducerArg* p = (ProducerArg*)arg;
    p->rc = enqueue_task(p->queue, p->task);
    atomic_store(&p->returned, 1);
    return NULL;
}

/* Blocks producers at capacity and unblocks them once the consumer makes
 * room — the "queue full" half of the contract. */
static void test_blocks_then_unblocks_on_room(void) {
    TaskQueue* q = init_task_queue(2);
    CHECK(q != NULL);

    CHECK(enqueue_task(q, make_task(PRIORITY_LOW)) == 0);
    CHECK(enqueue_task(q, make_task(PRIORITY_LOW)) == 0);
    CHECK(task_queue_size(q) == 2);

    ProducerArg p = { .queue = q, .task = make_task(PRIORITY_LOW), .rc = -99 };
    atomic_init(&p.returned, 0);

    pthread_t th;
    CHECK(pthread_create(&th, NULL, producer_thread, &p) == 0);

    /* Give the producer ample time to reach pthread_cond_wait. There is no
     * observable "now blocked" event to wait on directly, so a generous
     * sleep is the standard way to make this failure mode (a producer that
     * doesn't actually block) visible instead of racing past it. */
    usleep(150000);
    CHECK(atomic_load(&p.returned) == 0);   /* still blocked: queue was full */

    Task* drained = dequeue_task(q);         /* frees up one slot */
    CHECK(drained != NULL);
    free_task(drained);

    pthread_join(th, NULL);
    CHECK(atomic_load(&p.returned) == 1);
    CHECK(p.rc == 0);
    CHECK(task_queue_size(q) == 2);   /* the other original task + the producer's */

    /* Drain what's left; each task here has args_free == NULL so
     * drain_task_queue's free_task calls are pure no-op destructors. */
    drain_task_queue(q);
    cleanup_task_queue(q);
}

/* Blocks a producer at capacity, then shuts the queue down instead of
 * freeing a slot — enqueue_task must wake and fail rather than hang. */
static void test_shutdown_wakes_blocked_producer(void) {
    TaskQueue* q = init_task_queue(1);
    CHECK(q != NULL);
    CHECK(enqueue_task(q, make_task(PRIORITY_LOW)) == 0);

    ProducerArg p = { .queue = q, .task = make_task(PRIORITY_LOW), .rc = -99 };
    atomic_init(&p.returned, 0);

    pthread_t th;
    CHECK(pthread_create(&th, NULL, producer_thread, &p) == 0);

    usleep(150000);
    CHECK(atomic_load(&p.returned) == 0);   /* still blocked: queue was full */

    shutdown_task_queue(q);

    pthread_join(th, NULL);
    CHECK(atomic_load(&p.returned) == 1);
    CHECK(p.rc == -1);   /* rejected, not enqueued */

    /* enqueue_task's contract leaves a rejected task owned by the caller
     * (mirrors submit_task's own free_task on enqueue failure). */
    free_task(p.task);

    /* dequeue_task must also return NULL (not hang) once shut down and
     * drained, which is its documented signal for a consumer to exit. */
    Task* leftover = dequeue_task(q);
    CHECK(leftover != NULL);   /* the one task enqueued before shutdown */
    free_task(leftover);
    CHECK(dequeue_task(q) == NULL);

    cleanup_task_queue(q);
}

int main(void) {
    test_blocks_then_unblocks_on_room();
    test_shutdown_wakes_blocked_producer();

    test_pass(__FILE__);
    return 0;
}
