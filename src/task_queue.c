#include "task_queue.h"
#include "logger.h"
#include <stdlib.h>

TaskQueue* init_task_queue(int capacity) {
    if (capacity <= 0) return NULL;

    TaskQueue* queue = calloc(1, sizeof(TaskQueue));
    if (!queue) return NULL;

    /* Each bucket is sized for the whole capacity so that a bucket can never
     * overflow before the queue as a whole is full. */
    for (int p = 0; p < TASK_PRIORITY_LEVELS; p++) {
        queue->buckets[p].slots = malloc(sizeof(Task*) * capacity);
        if (!queue->buckets[p].slots) {
            for (int q = 0; q < p; q++) free(queue->buckets[q].slots);
            free(queue);
            return NULL;
        }
        queue->buckets[p].front = 0;
        queue->buckets[p].rear = -1;
        queue->buckets[p].count = 0;
    }

    queue->capacity = capacity;
    queue->size = 0;
    queue->shutdown = 0;

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);

    return queue;
}

/* Clamp an out-of-range priority rather than indexing off the end. */
static int clamp_priority(TaskPriority priority) {
    int p = (int)priority;
    if (p < 0) return 0;
    if (p >= TASK_PRIORITY_LEVELS) return TASK_PRIORITY_LEVELS - 1;
    return p;
}

/* Caller must hold queue->mutex. Returns NULL if every bucket is empty. */
static Task* pop_highest_priority_locked(TaskQueue* queue) {
    for (int p = TASK_PRIORITY_LEVELS - 1; p >= 0; p--) {
        PriorityBucket* bucket = &queue->buckets[p];
        if (bucket->count == 0) continue;

        Task* task = bucket->slots[bucket->front];
        bucket->front = (bucket->front + 1) % queue->capacity;
        bucket->count--;
        queue->size--;
        return task;
    }
    return NULL;
}

int enqueue_task(TaskQueue* queue, Task* task) {
    if (!queue || !task) return -1;

    pthread_mutex_lock(&queue->mutex);

    /* Predicate loop, not an if: pthread_cond_wait can wake spuriously, and
     * another producer may have refilled the queue before we were scheduled. */
    while (queue->size >= queue->capacity && !queue->shutdown) {
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }

    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    int p = clamp_priority(task->priority);
    PriorityBucket* bucket = &queue->buckets[p];

    bucket->rear = (bucket->rear + 1) % queue->capacity;
    bucket->slots[bucket->rear] = task;
    bucket->count++;
    queue->size++;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);

    log_message(LOG_DEBUG, "Task %d enqueued at priority %d", task->task_id, p);
    return 0;
}

Task* dequeue_task(TaskQueue* queue) {
    if (!queue) return NULL;

    pthread_mutex_lock(&queue->mutex);

    /* The `!shutdown` term is what lets the scheduler thread exit. Without it
     * the consumer parks in pthread_cond_wait forever and shutdown has to fall
     * back on a join timeout plus pthread_cancel. */
    while (queue->size == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    Task* task = pop_highest_priority_locked(queue);

    if (task) {
        pthread_cond_signal(&queue->not_full);
    }

    pthread_mutex_unlock(&queue->mutex);

    if (task) {
        log_message(LOG_DEBUG, "Task %d dequeued", task->task_id);
    }
    return task;   /* NULL means: shut down and drained */
}

void shutdown_task_queue(TaskQueue* queue) {
    if (!queue) return;

    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = 1;
    /* Broadcast, not signal: every blocked producer and consumer has to
     * re-check the predicate, and there may be more than one of each. */
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}

int drain_task_queue(TaskQueue* queue) {
    if (!queue) return 0;

    int dropped = 0;

    pthread_mutex_lock(&queue->mutex);

    /* Pop through the locked helper rather than dequeue_task(): the mutex is
     * not recursive, so calling the public API from here would deadlock on
     * the lock this function already holds. */
    Task* task;
    while ((task = pop_highest_priority_locked(queue)) != NULL) {
        task->status = STATUS_FAILED;
        /* free_task() runs each task's args destructor. The library owns args
         * on every path, so there is nothing to free by hand here — and no
         * assumption that args is a single malloc'd block. */
        free_task(task);
        dropped++;
    }

    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    return dropped;
}

int task_queue_size(TaskQueue* queue) {
    if (!queue) return 0;

    pthread_mutex_lock(&queue->mutex);
    int size = queue->size;
    pthread_mutex_unlock(&queue->mutex);

    return size;
}

void cleanup_task_queue(TaskQueue* queue) {
    if (queue == NULL) {
        return; // Nothing to clean up
    }

    drain_task_queue(queue);

    for (int p = 0; p < TASK_PRIORITY_LEVELS; p++) {
        free(queue->buckets[p].slots);
        queue->buckets[p].slots = NULL;
        queue->buckets[p].count = 0;
    }

    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);

    free(queue);
}
