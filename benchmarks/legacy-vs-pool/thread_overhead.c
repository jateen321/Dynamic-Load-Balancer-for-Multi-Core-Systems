#include <stdio.h>
#include <pthread.h>
#include <time.h>

static void* noop(void* arg) { (void)arg; return NULL; }

int main(void) {
    long n = 100000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (long i = 0; i < n; i++) {
        pthread_t t;
        pthread_create(&t, NULL, noop, NULL);
        pthread_join(t, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
    printf("%ld create+join cycles: %.2f ms total, %.4f ms/cycle\n", n, ms, ms / n);
    return 0;
}
