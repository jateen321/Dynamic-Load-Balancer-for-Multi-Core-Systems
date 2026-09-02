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

#endif
