#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include "task.h"

/* One FIFO ring per TaskPriority level. PRIORITY_LOW..PRIORITY_CRITICAL. */
#define TASK_PRIORITY_LEVELS 4

typedef struct {
    Task** slots;
    int front;
    int rear;
    int count;
} PriorityBucket;

/*
 * Multi-level queue: a task is served from the highest-priority non-empty
 * bucket, and within a bucket strictly in FIFO order. Both enqueue and dequeue
 * are O(TASK_PRIORITY_LEVELS), i.e. constant.
 *
 * `size` is the total across all buckets and is what `capacity` bounds.
 */
typedef struct {
    PriorityBucket buckets[TASK_PRIORITY_LEVELS];
    int capacity;
    int size;
    int shutdown;               /* set once; makes blocked waiters return */
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} TaskQueue;

TaskQueue* init_task_queue(int capacity);

/* Returns 0 on success, -1 if the queue is shutting down. Blocks while full. */
int enqueue_task(TaskQueue* queue, Task* task);

/* Blocks until a task is available. Returns NULL once the queue is shut down
 * and drained — that NULL is the consumer's signal to exit its loop. */
Task* dequeue_task(TaskQueue* queue);

/* Wakes every blocked producer and consumer so they can observe shutdown. */
void shutdown_task_queue(TaskQueue* queue);

/* Frees all still-queued tasks, running each one's args destructor. Returns
 * how many were dropped. */
int drain_task_queue(TaskQueue* queue);

/* Current number of queued tasks, read under the queue lock. */
int task_queue_size(TaskQueue* queue);

void cleanup_task_queue(TaskQueue* queue);

/* ---------------------------------------------------------------------------
 * Per-core work-stealing queue
 *
 * One of these belongs to each worker in the pool. The dispatcher (the only
 * producer) pushes at the tail; the owning worker (the only "normal" consumer)
 * pops at the head, so tasks run in the order they were assigned to this core.
 * An idle peer may steal from the *tail* instead — the opposite end from the
 * one the owner is draining, which keeps a stealing peer from fighting the
 * owner over the same node most of the time (the mutex still serializes the
 * rare case where both ends collide, but the window is a handful of pointer
 * writes).
 *
 * Unbounded and node-based rather than a capacity-bounded ring buffer: a
 * single core can end up holding more tasks than the global admission queue's
 * capacity ever had in flight at once, simply because tasks are consumed from
 * one core's queue and replaced by new dispatches over the life of a run. A
 * capacity here would just relocate the "queue full" failure mode into the
 * dispatcher for no benefit.
 * ------------------------------------------------------------------------ */

typedef struct CoreQueueNode {
    struct CoreQueueNode* next;
    struct CoreQueueNode* prev;
    Task* task;
} CoreQueueNode;

typedef struct {
    CoreQueueNode* head;   /* front: owner pops here */
    CoreQueueNode* tail;   /* back: dispatcher pushes here, thieves steal here */
    int count;
    int shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    long pushed_total;    /* tasks ever pushed to this queue */
    long stolen_total;    /* tasks a peer took from this queue via steal */
} CoreQueue;

CoreQueue* init_core_queue(void);

/* Push at the tail. Returns 0, or -1 if the queue is shut down (the caller
 * still owns `task` on -1). */
int core_queue_push(CoreQueue* queue, Task* task);

/* Pop from the head. Blocks up to timeout_ms for a task to appear if the
 * queue is empty and not yet shut down; a timeout and "shut down and empty"
 * both return NULL, deliberately indistinguishable to the caller — the worker
 * loop treats both as "nothing here right now, go look elsewhere or check
 * whether it's time to exit". */
Task* core_queue_pop_own(CoreQueue* queue, int timeout_ms);

/* Non-blocking: pops from the tail if the lock is free AND the queue holds
 * more than one task. The trylock keeps an idle thief from blocking on a busy
 * owner; the ">1" heuristic leaves the owner's last task alone so an idle
 * peer doesn't strip work out from under it the instant it arrives. Returns
 * NULL if neither condition holds. */
Task* core_queue_try_steal(CoreQueue* queue);

/* Current number of queued tasks, read under the queue lock. */
int core_queue_size(CoreQueue* queue);

/* Marks the queue shut down and wakes anyone blocked in pop_own. Does not
 * discard queued tasks — that is core_queue_drain's job. */
void core_queue_shutdown(CoreQueue* queue);

/* Frees all still-queued tasks, running each one's args destructor and
 * marking it STATUS_FAILED. Returns how many were dropped. */
int core_queue_drain(CoreQueue* queue);

void cleanup_core_queue(CoreQueue* queue);

#endif
