/*
 * update_cpu_stats()'s first-sample baseline handling. /proc/stat's jiffie
 * counters are cumulative since boot, so the first read for a core has no
 * valid previous sample to diff against (every cpu->*_time field is still
 * the memset-zero from init_cpu_monitor()). Without a baseline flag, that
 * first read would treat "since boot" as "since the previous sample" and
 * bake a meaningless usage figure into current_usage/usage_history. This
 * checks that the first call establishes a baseline only (current_usage and
 * history_count untouched), and the second call is the first to actually
 * produce a usage sample.
 */

#include "test_common.h"
#include "cpu_stats.h"

#include <unistd.h>

int main(void) {
    LoadBalancerConfig* cfg = test_config(4, 16, SCHED_LEAST_LOAD, 0);

    CPUMonitor* monitor = init_cpu_monitor(cfg);
    CHECK(monitor != NULL);

    /* First sample: every core should still be at its pre-sample default
     * (0.0 usage, no history) — nothing computed from a nonexistent
     * baseline. */
    update_cpu_stats(monitor);

    for (int i = 0; i < monitor->num_cpus; i++) {
        CPUStats* cpu = &monitor->stats[i];
        CHECK(cpu->current_usage == 0.0);
        CHECK(cpu->history_count == 0);
        CHECK(cpu->has_baseline == 1);
    }

    /* Let some real wall-clock time (and, on a live system, real jiffies)
     * pass before the second sample, per the existing "No jiffies elapsed
     * between samples" reasoning in update_cpu_stats(). We don't assert a
     * specific usage value (machine-load-dependent) — only that a sample
     * was actually recorded this time. */
    usleep(20000);

    update_cpu_stats(monitor);

    for (int i = 0; i < monitor->num_cpus; i++) {
        CPUStats* cpu = &monitor->stats[i];
        CHECK(cpu->has_baseline == 1);
        /* A core only gets a real history entry once total_delta > 0, i.e.
         * once /proc/stat actually reported a matching "cpuN" line for it
         * both times. That is expected on any real Linux box for every
         * configured core, but guard the assertion anyway rather than
         * assuming without checking. */
        CHECK(cpu->history_count == 1);
        CHECK(cpu->usage_history[0] == cpu->current_usage);
    }

    cleanup_cpu_monitor(monitor);
    free_config(cfg);

    test_pass(__FILE__);
    return 0;
}
