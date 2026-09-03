#include "task.h"
#include <stdlib.h>
#include <string.h>

static int next_task_id = 0;

Task* create_task(void (*function)(void*), void* args,
                  TaskArgsDestructor args_free, TaskPriority priority) {
    Task* task = malloc(sizeof(Task));
    if (!task) return NULL;   /* caller still owns args; see task.h */

    task->task_id = __atomic_fetch_add(&next_task_id, 1, __ATOMIC_SEQ_CST);
    task->function = function;
    task->args = args;
    task->args_free = args_free;
    task->priority = priority;
    task->status = STATUS_PENDING;
    task->assigned_cpu = -1;
    task->cpu_usage = 0.0;
    task->memory_usage = 0.0;

    clock_gettime(CLOCK_MONOTONIC, &task->create_time);

    return task;
}

void task_release_args(Task* task) {
    if (!task) return;

    /* Clear before invoking, so a destructor that somehow re-entered this
     * function on the same Task cannot recurse, and so a later free_task()
     * finds nothing left to release. */
    TaskArgsDestructor destroy = task->args_free;
    void* args = task->args;

    task->args_free = NULL;
    task->args = NULL;

    if (destroy) destroy(args);
}

void free_task(Task* task) {
    if (!task) return;

    task_release_args(task);
    free(task);
}
