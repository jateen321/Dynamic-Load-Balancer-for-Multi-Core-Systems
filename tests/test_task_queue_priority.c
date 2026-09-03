/*
 * TaskQueue priority ordering and FIFO-within-a-level, driven directly
 * (init_task_queue/enqueue_task/dequeue_task) rather than through a whole
 * LoadBalancer: the multi-level-queue invariant is entirely a property of
 * this data structure, so there is no need to spin up worker threads to
 * exercise it.
 */

#include "test_common.h"
#include "task_queue.h"

#include <stdlib.h>

static void noop(void* args) {
    (void)args;
}

/* Each task's args is a malloc'd int holding the order it was enqueued in,
 * so after dequeuing we can check both *which* priority came out and, within
 * a priority level, that submission order was preserved. */
static Task* make_tagged_task(int tag, TaskPriority priority) {
    int* payload = malloc(sizeof(int));
    CHECK(payload != NULL);
    *payload = tag;
    Task* t = create_task(noop, payload, free, priority);
    CHECK(t != NULL);
    return t;
}

int main(void) {
    TaskQueue* q = init_task_queue(32);
    CHECK(q != NULL);

    /*
     * Enqueued out of both priority and time order on purpose. Expected
     * dequeue order is: all CRITICAL (FIFO among themselves), then all HIGH,
     * then all MEDIUM, then all LOW — each bucket internally FIFO regardless
     * of the interleaving below.
     */
    CHECK(enqueue_task(q, make_tagged_task(0, PRIORITY_LOW)) == 0);
    CHECK(enqueue_task(q, make_tagged_task(1, PRIORITY_MEDIUM)) == 0);
    CHECK(enqueue_task(q, make_tagged_task(2, PRIORITY_HIGH)) == 0);
    CHECK(enqueue_task(q, make_tagged_task(3, PRIORITY_LOW)) == 0);
    CHECK(enqueue_task(q, make_tagged_task(4, PRIORITY_CRITICAL)) == 0);
    CHECK(enqueue_task(q, make_tagged_task(5, PRIORITY_MEDIUM)) == 0);
    CHECK(enqueue_task(q, make_tagged_task(6, PRIORITY_HIGH)) == 0);
    CHECK(enqueue_task(q, make_tagged_task(7, PRIORITY_LOW)) == 0);

    int expected[] = {4, 2, 6, 1, 5, 0, 3, 7};
    int n = (int)(sizeof(expected) / sizeof(expected[0]));

    CHECK(task_queue_size(q) == n);

    for (int i = 0; i < n; i++) {
        Task* t = dequeue_task(q);
        CHECK(t != NULL);
        int tag = *(int*)t->args;
        CHECK(tag == expected[i]);
        free_task(t);   /* runs the `free` destructor on the tag */
    }

    CHECK(task_queue_size(q) == 0);

    /*
     * A single priority level on its own is a narrower, more direct check
     * of FIFO-within-a-bucket than the interleaved scenario above, whose
     * per-level order could in principle be right by coincidence of the
     * ring-buffer indices chosen.
     */
    for (int i = 0; i < 10; i++) {
        CHECK(enqueue_task(q, make_tagged_task(i, PRIORITY_MEDIUM)) == 0);
    }
    for (int i = 0; i < 10; i++) {
        Task* t = dequeue_task(q);
        CHECK(t != NULL);
        CHECK(*(int*)t->args == i);
        free_task(t);
    }

    cleanup_task_queue(q);

    test_pass(__FILE__);
    return 0;
}
