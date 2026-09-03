/*
 * Concurrent producers: many threads pushing at once against one shared
 * queue/balancer, with the total that comes out the other end checked
 * against exactly what went in — no lost tasks, no duplicates. Two levels:
 * raw TaskQueue.enqueue_task (with a concurrent consumer, since capacity is
 * kept smaller than the total submitted so producers actually contend for
 * room) and submit_task against a live LoadBalancer end to end.
 */

#include "test_common.h"
#include "load_balancer.h"
#include "task_queue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

static void noop_fn(void* args) {
    (void)args;
}

/* ---------------------------------------------------------------------------
 * Level 1: enqueue_task/dequeue_task directly on a TaskQueue.
 * ------------------------------------------------------------------------ */

#define QPRODUCERS 8
#define QPER_PRODUCER 50
#define QTOTAL (QPRODUCERS * QPER_PRODUCER)
#define QCAPACITY 16   /* well under QTOTAL: producers must genuinely block */

typedef struct {
    TaskQueue* queue;
    int base_tag;
} QProducerArg;

static void* q_producer(void* arg) {
    QProducerArg* p = (QProducerArg*)arg;
    for (int i = 0; i < QPER_PRODUCER; i++) {
        int* tag = malloc(sizeof(int));
        CHECK(tag != NULL);
        *tag = p->base_tag + i;
        Task* t = create_task(noop_fn, tag, free, PRIORITY_MEDIUM);
        CHECK(t != NULL);
        CHECK(enqueue_task(p->queue, t) == 0);
    }
    return NULL;
}

typedef struct {
    TaskQueue* queue;
    atomic_int* seen;
    atomic_int* collected;
} QConsumerArg;

static void* q_consumer(void* arg) {
    QConsumerArg* c = (QConsumerArg*)arg;
    while (atomic_load(c->collected) < QTOTAL) {
        Task* t = dequeue_task(c->queue);
        if (!t) break;   /* would mean shutdown, which this test never triggers */
        int tag = *(int*)t->args;
        CHECK(tag >= 0 && tag < QTOTAL);
        int prior = atomic_exchange(&c->seen[tag], 1);
        CHECK(prior == 0);   /* never delivered before */
        atomic_fetch_add(c->collected, 1);
        free_task(t);
    }
    return NULL;
}

static void test_concurrent_enqueue_dequeue(void) {
    TaskQueue* q = init_task_queue(QCAPACITY);
    CHECK(q != NULL);

    atomic_int seen[QTOTAL];
    for (int i = 0; i < QTOTAL; i++) atomic_init(&seen[i], 0);
    atomic_int collected;
    atomic_init(&collected, 0);

    /*
     * Exactly one consumer, deliberately: with two, the loser of the very
     * last item can already be parked inside dequeue_task's cond_wait (it
     * checked `collected < QTOTAL` and found the queue empty a moment
     * before the winner's collected++ reached QTOTAL) with nothing left to
     * ever signal it — enqueue_task only signals on a push, and there are
     * no more pushes once every producer has finished. A single consumer
     * can't lose a race with itself, so it can't strand itself that way;
     * the producer/consumer *contention* this test cares about is on the
     * producer side (QPRODUCERS threads vs QCAPACITY < QTOTAL), which one
     * consumer still fully exercises.
     */
    QConsumerArg cons_arg = { .queue = q, .seen = seen, .collected = &collected };
    pthread_t consumer;
    CHECK(pthread_create(&consumer, NULL, q_consumer, &cons_arg) == 0);

    pthread_t producers[QPRODUCERS];
    QProducerArg prod_args[QPRODUCERS];
    for (int i = 0; i < QPRODUCERS; i++) {
        prod_args[i].queue = q;
        prod_args[i].base_tag = i * QPER_PRODUCER;
        CHECK(pthread_create(&producers[i], NULL, q_producer, &prod_args[i]) == 0);
    }

    for (int i = 0; i < QPRODUCERS; i++) pthread_join(producers[i], NULL);
    pthread_join(consumer, NULL);

    CHECK(atomic_load(&collected) == QTOTAL);
    for (int i = 0; i < QTOTAL; i++) {
        CHECK(atomic_load(&seen[i]) == 1);
    }
    CHECK(task_queue_size(q) == 0);

    cleanup_task_queue(q);
}

/* ---------------------------------------------------------------------------
 * Level 2: submit_task against a live, running LoadBalancer.
 * ------------------------------------------------------------------------ */

#define LPRODUCERS 4
#define LPER_PRODUCER 50
#define LTOTAL (LPRODUCERS * LPER_PRODUCER)

static void count_fn(void* args) {
    atomic_fetch_add((atomic_int*)args, 1);
}

typedef struct {
    LoadBalancer* lb;
    atomic_int* ran;
    int failures;   /* submit_task calls that returned nonzero; expect 0 */
} LProducerArg;

static void* l_producer(void* arg) {
    LProducerArg* p = (LProducerArg*)arg;
    for (int i = 0; i < LPER_PRODUCER; i++) {
        /* args is the shared `ran` counter itself, borrowed (args_free ==
         * NULL): many in-flight tasks safely sharing one non-owned pointer
         * is exactly the case NULL-as-destructor documents in task.h. */
        if (submit_task(p->lb, count_fn, p->ran, NULL, PRIORITY_MEDIUM) != 0) {
            p->failures++;
        }
    }
    return NULL;
}

static void test_concurrent_submit_task(void) {
    /* Capacity well under LTOTAL, and only a couple of cores, so producers
     * genuinely contend both for admission-queue room and for the
     * dispatcher/workers actually keeping up with them. */
    LoadBalancerConfig* cfg = test_config(test_clamp_cpus(2), 16, SCHED_LEAST_LOAD, 1);
    LoadBalancer* lb = init_load_balancer(cfg);
    CHECK(lb != NULL);
    CHECK(start_load_balancer(lb) == 0);

    atomic_int ran;
    atomic_init(&ran, 0);

    pthread_t producers[LPRODUCERS];
    LProducerArg args[LPRODUCERS];
    for (int i = 0; i < LPRODUCERS; i++) {
        args[i].lb = lb;
        args[i].ran = &ran;
        args[i].failures = 0;
        CHECK(pthread_create(&producers[i], NULL, l_producer, &args[i]) == 0);
    }
    for (int i = 0; i < LPRODUCERS; i++) {
        pthread_join(producers[i], NULL);
        CHECK(args[i].failures == 0);   /* shutdown never happens mid-test */
    }

    wait_for_tasks_completion(lb);

    CHECK(atomic_load(&ran) == LTOTAL);   /* every submitted task ran exactly once */
    CHECK(load_balancer_pending_tasks(lb) == 0);
    CHECK(load_balancer_active_tasks(lb) == 0);

    cleanup_load_balancer(lb);
    free_config(cfg);
}

int main(void) {
    test_concurrent_enqueue_dequeue();
    test_concurrent_submit_task();

    test_pass(__FILE__);
    return 0;
}
