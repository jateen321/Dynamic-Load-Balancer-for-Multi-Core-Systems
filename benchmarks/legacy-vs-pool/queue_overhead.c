#include "task_queue.h"
#include "task.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

int main(void) {
    long n = 100000;
    CoreQueue* q = init_core_queue();
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (long i = 0; i < n; i++) {
        Task* t = create_task(NULL, NULL, NULL, PRIORITY_MEDIUM);
        core_queue_push(q, t);
        Task* got = core_queue_pop_own(q, 0);
        free_task(got);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
    printf("%ld push+pop cycles: %.2f ms total, %.4f ms/cycle\n", n, ms, ms / n);
    cleanup_core_queue(q);
    return 0;
}
