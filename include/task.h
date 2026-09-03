#ifndef TASK_H
#define TASK_H

#include <pthread.h>
#include <time.h>

typedef enum {
    PRIORITY_LOW = 0,
    PRIORITY_MEDIUM = 1,
    PRIORITY_HIGH = 2,
    PRIORITY_CRITICAL = 3
} TaskPriority;

typedef enum {
    STATUS_PENDING,
    STATUS_RUNNING,
    STATUS_COMPLETED,
    STATUS_FAILED
} TaskStatus;

/*
 * Destroys a task's args. Invoked exactly once per Task, by the library,
 * whether or not the task function ever ran. Receives the pointer that was
 * handed to create_task()/submit_task(). Must tolerate NULL, like free().
 */
typedef void (*TaskArgsDestructor)(void* args);

typedef struct {
    int task_id;
    TaskPriority priority;
    void (*function)(void*);
    void* args;
    /* NULL means the library never frees args. That is the correct value for
     * a stack address, a string literal, an integer encoded in a pointer, or
     * anything whose lifetime the caller guarantees. It is a statement of
     * non-ownership, not a shorthand for free. */
    TaskArgsDestructor args_free;
    int assigned_cpu;
    TaskStatus status;
    struct timespec create_time;
    struct timespec start_time;
    struct timespec end_time;
    double cpu_usage;
    double memory_usage;
} Task;

/*
 * Takes ownership of `args`: the Task destroys it via `args_free` exactly
 * once. Returns NULL on allocation failure, in which case the CALLER still
 * owns args (there is no Task to own them).
 */
Task* create_task(void (*function)(void*), void* args,
                  TaskArgsDestructor args_free, TaskPriority priority);

/*
 * Runs the destructor if one is set, then clears args and args_free.
 * Idempotent, so a later free_task() is a no-op rather than a double free.
 * Not safe to call concurrently on the same Task from two threads.
 */
void task_release_args(Task* task);

/* Releases args (see above), then frees the Task itself. */
void free_task(Task* task);

#endif
