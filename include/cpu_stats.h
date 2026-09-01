#ifndef CPU_STATS_H
#define CPU_STATS_H

#include <stdint.h>
#include <pthread.h>
#include "config.h"

typedef struct {
    int cpu_id;
    double current_usage;
    double *usage_history;
    int history_index;   /* next slot to write (wraps) */
    int history_count;   /* samples actually held, saturates at load_history_size */
    uint64_t user_time;
    uint64_t nice_time;
    uint64_t system_time;
    uint64_t idle_time;
    uint64_t iowait_time;
    uint64_t irq_time;
    uint64_t softirq_time;
    uint64_t steal_time;
    double temperature;
    double predicted_load;
    int active_tasks;
} CPUStats;

typedef struct {
    CPUStats* stats;
    int num_cpus;
    LoadBalancerConfig* config;
    /* Guards every field of every CPUStats. The monitor thread writes them,
     * the scheduler thread reads them in find_best_cpu(). */
    pthread_mutex_t lock;
} CPUMonitor;

CPUMonitor* init_cpu_monitor(LoadBalancerConfig* config);
void update_cpu_stats(CPUMonitor* monitor);
double predict_cpu_load(CPUStats* cpu);
void print_cpu_stats(CPUMonitor* monitor);
void cleanup_cpu_monitor(CPUMonitor* monitor);

/* Adjust the in-flight task count for a core. delta is +1 or -1. */
void cpu_monitor_adjust_active_tasks(CPUMonitor* monitor, int cpu_id, int delta);

/* Logs a warning when one core is above high_load_threshold while another is
 * below low_load_threshold. */
void check_load_balance(CPUMonitor* monitor);

#endif