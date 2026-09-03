#ifndef TEST_COMMON_H
#define TEST_COMMON_H

/*
 * Shared by every test-*.c binary. No test framework, matching the rest of
 * this project's no-dependencies-beyond-pthread/m/rt policy: each test file
 * is its own executable with a plain main() and hand-rolled checks.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * CMake's built-in Release flags append -DNDEBUG (this project's own
 * CMAKE_C_FLAGS_RELEASE, layered on top of the add_compile_options() in
 * CMakeLists.txt), which would silently turn a plain assert() into a no-op
 * for the default `cmake --build build` CTest is documented to run. CHECK
 * is a small assert-alike that cannot be compiled away: on failure it prints
 * the file/line/expression and exits non-zero, which is exactly what makes
 * CTest report the binary as FAILED.
 */
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            fflush(stderr); \
            exit(1); \
        } \
    } while (0)

static inline void test_pass(const char* file) {
    printf("PASS %s\n", file);
}

/* Clamp a desired core count to what this machine actually has online, so
 * the same test file behaves on a 1-core CI runner and a 4-core workstation
 * without either failing pthread_create (asking for a CPU affinity mask bit
 * that doesn't exist) or degenerating everywhere to n==1. */
static inline int test_clamp_cpus(int wanted) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    int max_cpus = (online > 0) ? (int)online : 1;
    return (wanted < max_cpus) ? wanted : max_cpus;
}

/*
 * A LoadBalancerConfig tuned for fast, quiet, deterministic tests:
 *  - log_file_path "/dev/null" and detailed logging off, so a test run
 *    doesn't scatter *.log files around the tree or pay for formatting
 *    debug lines it never inspects.
 *  - num_cpus clamped to what's actually online (see test_clamp_cpus).
 * Caller may further override fields (e.g. monitoring_interval_ms) on the
 * returned pointer before passing it to init_load_balancer().
 */
static inline LoadBalancerConfig* test_config(int num_cpus, int max_tasks,
                                               SchedulingPolicy policy,
                                               int enable_work_stealing) {
    LoadBalancerConfig* cfg = init_default_config();
    CHECK(cfg != NULL);

    free(cfg->log_file_path);
    cfg->log_file_path = strdup("/dev/null");
    CHECK(cfg->log_file_path != NULL);

    cfg->enable_detailed_logging = 0;
    cfg->monitoring_interval_ms = 20;
    cfg->num_cpus = test_clamp_cpus(num_cpus);
    cfg->max_tasks = max_tasks;
    cfg->scheduling_policy = policy;
    cfg->enable_work_stealing = enable_work_stealing;

    return cfg;
}

#endif
