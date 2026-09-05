#include "task_queue.h"
#include "logger.h"
#include <stdlib.h>
#include <time.h>

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
    /* Captured before the task becomes visible to a consumer: once unlocked
     * below, a waiting dequeue_task() can pop, run, and free this exact task
     * before this function's next line runs, so `task` itself is no longer
     * safe to dereference past the unlock. */
    int task_id = task->task_id;

    bucket->rear = (bucket->rear + 1) % queue->capacity;
    bucket->slots[bucket->rear] = task;
    bucket->count++;
    queue->size++;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);

    log_message(LOG_DEBUG, "Task %d enqueued at priority %d", task_id, p);
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

    pthread_mutex_lock(&queue->mutex);

    /* Pop through the locked helper rather than dequeue_task(): the mutex is
     * not recursive, so calling the public API from here would deadlock on
     * the lock this function already holds. Every popped task goes into a
     * local array instead of straight into free_task(): free_task() runs the
     * caller's args destructor, which is arbitrary, unbounded-duration code
     * (real cleanup work, I/O, logging — whatever the caller wrote) that has
     * no structural need for this queue's lock. A task is fully detached from
     * the buckets the instant pop_highest_priority_locked() returns it, so
     * running destructors here would block every other producer/consumer of
     * this queue for the entire sum of their durations, for no reason beyond
     * "this loop happened to still be under the lock". queue->size is the
     * exact number pop_highest_priority_locked() can still hand back, so it's
     * the right fixed size for the array — no realloc/relock needed. */
    int capacity = queue->size;
    Task** detached = capacity > 0 ? malloc(sizeof(Task*) * (size_t)capacity) : NULL;
    int dropped = 0;

    if (capacity > 0 && !detached) {
        /* Allocation failed: fall back to freeing under the lock rather than
         * leaking every queued task outright. This is the only path where
         * destructors still run with the mutex held — acceptable only because
         * it takes catastrophic OOM to reach it, at which point lock-hold
         * time is not the pressing problem. */
        Task* task;
        while ((task = pop_highest_priority_locked(queue)) != NULL) {
            task->status = STATUS_FAILED;
            free_task(task);
            dropped++;
        }
        pthread_cond_broadcast(&queue->not_full);
        pthread_mutex_unlock(&queue->mutex);
        return dropped;
    }

    Task* task;
    while ((task = pop_highest_priority_locked(queue)) != NULL) {
        detached[dropped++] = task;
    }

    /* Broadcast while the lock is still held, before running any destructor:
     * not_full means "there is room to enqueue again", which is already true
     * here — every bucket this loop could still drain from is now empty. A
     * blocked producer only needs that fact to make progress; it has no
     * reason to also wait for the detached tasks' destructors to finish, so
     * delaying the broadcast until after the loop below would only add
     * needless latency for waiters without buying correctness. */
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    /* free_task() runs each task's args destructor, now with the lock free.
     * The library owns args on every path, so there is nothing to free by
     * hand here — and no assumption that args is a single malloc'd block. */
    for (int i = 0; i < dropped; i++) {
        detached[i]->status = STATUS_FAILED;
        free_task(detached[i]);
    }
    free(detached);

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

/* ---------------------------------------------------------------------------
 * Per-core work-stealing queue
 * ------------------------------------------------------------------------ */

CoreQueue* init_core_queue(void) {
    CoreQueue* queue = calloc(1, sizeof(CoreQueue));
    if (!queue) return NULL;

    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
    queue->shutdown = 0;
    queue->pushed_total = 0;
    queue->stolen_total = 0;

    if (pthread_mutex_init(&queue->lock, NULL) != 0) {
        free(queue);
        return NULL;
    }
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&queue->lock);
        free(queue);
        return NULL;
    }

    return queue;
}

int core_queue_push(CoreQueue* queue, Task* task) {
    if (!queue || !task) return -1;

    CoreQueueNode* node = malloc(sizeof(CoreQueueNode));
    if (!node) return -1;
    node->task = task;
    node->next = NULL;
    /* Captured before the task becomes visible to its owning worker: once
     * unlocked below, that worker can pop, run, and free this exact task
     * before this function's log call runs, so `task` itself is no longer
     * safe to dereference past the unlock. */
    int task_id = task->task_id;

    pthread_mutex_lock(&queue->lock);

    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->lock);
        free(node);
        return -1;
    }

    node->prev = queue->tail;
    if (queue->tail) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }
    queue->tail = node;
    queue->count++;
    queue->pushed_total++;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->lock);

    log_message(LOG_DEBUG, "Task %d pushed to core queue", task_id);
    return 0;
}

Task* core_queue_pop_own(CoreQueue* queue, int timeout_ms) {
    if (!queue) return NULL;

    pthread_mutex_lock(&queue->lock);

    /* Only wait if there is genuinely nothing to do right now. A negative or
     * zero timeout degenerates to a poll, which is fine: the caller decides
     * how patient to be. */
    if (queue->count == 0 && !queue->shutdown && timeout_ms > 0) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec += 1;
        }
        /* Ignoring the return value is deliberate: whether this wakes because
         * a task arrived, because it timed out, or spuriously, the code below
         * re-reads queue->head and acts on what is actually there. */
        pthread_cond_timedwait(&queue->not_empty, &queue->lock, &deadline);
    }

    Task* task = NULL;
    CoreQueueNode* node = queue->head;
    if (node) {
        task = node->task;
        queue->head = node->next;
        if (queue->head) {
            queue->head->prev = NULL;
        } else {
            queue->tail = NULL;
        }
        queue->count--;
        free(node);
    }

    pthread_mutex_unlock(&queue->lock);
    return task;
}

Task* core_queue_try_steal(CoreQueue* queue) {
    if (!queue) return NULL;

    if (pthread_mutex_trylock(&queue->lock) != 0) return NULL;

    Task* task = NULL;
    /* count > 1, not > 0: never take the owner's only remaining task. */
    if (queue->count > 1 && queue->tail) {
        CoreQueueNode* node = queue->tail;
        task = node->task;
        queue->tail = node->prev;
        if (queue->tail) {
            queue->tail->next = NULL;
        } else {
            queue->head = NULL;
        }
        queue->count--;
        queue->stolen_total++;
        free(node);
    }

    pthread_mutex_unlock(&queue->lock);

    if (task) {
        log_message(LOG_DEBUG, "Task %d stolen from core queue", task->task_id);
    }
    return task;
}

int core_queue_size(CoreQueue* queue) {
    if (!queue) return 0;

    pthread_mutex_lock(&queue->lock);
    int size = queue->count;
    pthread_mutex_unlock(&queue->lock);

    return size;
}

long core_queue_stolen_total(CoreQueue* queue) {
    if (!queue) return 0;

    pthread_mutex_lock(&queue->lock);
    long stolen = queue->stolen_total;
    pthread_mutex_unlock(&queue->lock);

    return stolen;
}

void core_queue_shutdown(CoreQueue* queue) {
    if (!queue) return;

    pthread_mutex_lock(&queue->lock);
    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->lock);
}

int core_queue_drain(CoreQueue* queue) {
    if (!queue) return 0;

    int dropped = 0;

    pthread_mutex_lock(&queue->lock);

    /* Detach the whole intrusive list in three assignments — pure pointer
     * bookkeeping, no destructor calls — then walk the detached list after
     * unlocking. free_task() runs the caller's args destructor, arbitrary and
     * possibly slow code that doesn't need this queue's lock: every node here
     * is already unreachable from queue->head/tail the moment it's detached,
     * so nothing else could observe it anyway. Running destructors under the
     * lock would block the owner's pop_own and any peer's try_steal for the
     * whole drain instead of just for this pointer-swap. */
    CoreQueueNode* local_head = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;

    pthread_mutex_unlock(&queue->lock);

    CoreQueueNode* node = local_head;
    while (node) {
        CoreQueueNode* next = node->next;
        node->task->status = STATUS_FAILED;
        free_task(node->task);   /* runs the task's args destructor */
        free(node);
        node = next;
        dropped++;
    }

    return dropped;
}

void cleanup_core_queue(CoreQueue* queue) {
    if (!queue) return;

    core_queue_drain(queue);

    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->not_empty);

    free(queue);
}
