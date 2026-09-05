#include "cpu_stats.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

CPUMonitor* init_cpu_monitor(LoadBalancerConfig* config) {
    CPUMonitor* monitor = malloc(sizeof(CPUMonitor));
    if (!monitor) return NULL;
    
    monitor->num_cpus = config->num_cpus;
    monitor->config = config;
    monitor->stats = malloc(sizeof(CPUStats) * monitor->num_cpus);
    
    if (!monitor->stats) {
        free(monitor);
        return NULL;
    }
    
    for (int i = 0; i < monitor->num_cpus; i++) {
        CPUStats* cpu = &monitor->stats[i];

        /* Every field must start at a known value: update_cpu_stats() subtracts
         * the previous jiffie counters from the current ones, so uninitialized
         * *_time fields would make the very first usage reading garbage. This
         * memset is also what gives has_baseline its correct starting value of
         * 0, which is what tells update_cpu_stats() that this core has no real
         * previous sample yet (see the comment at has_baseline's use there). */
        memset(cpu, 0, sizeof(*cpu));
        cpu->cpu_id = i;

        cpu->usage_history = calloc(config->load_history_size, sizeof(double));
        if (!cpu->usage_history) {
            for (int j = 0; j < i; j++) {
                free(monitor->stats[j].usage_history);
            }
            free(monitor->stats);
            free(monitor);
            return NULL;
        }
    }

    pthread_mutex_init(&monitor->lock, NULL);

    return monitor;
}

void update_cpu_stats(CPUMonitor* monitor) {
    FILE* fp = fopen("/proc/stat", "r");
    if (!fp) {
        log_message(LOG_ERROR, "Failed to open /proc/stat");
        return;
    }
    
    char line[256];
    // Skip first line (aggregate CPU stats)
    if (!fgets(line, sizeof(line), fp)) {
        log_message(LOG_ERROR, "Unexpected end of /proc/stat");
        fclose(fp);
        return;
    }

    pthread_mutex_lock(&monitor->lock);

    for (int i = 0; i < monitor->num_cpus; i++) {
        if (fgets(line, sizeof(line), fp)) {
            CPUStats* cpu = &monitor->stats[i];
            uint64_t user, nice, system, idle, iowait, irq, softirq, steal;

            /* Ran past the per-core lines (fewer cores than configured). */
            if (strncmp(line, "cpu", 3) != 0) break;

            if (sscanf(line, "cpu%*d %lu %lu %lu %lu %lu %lu %lu %lu",
                       &user, &nice, &system, &idle,
                       &iowait, &irq, &softirq, &steal) != 8) {
                log_message(LOG_WARNING, "Malformed /proc/stat line for CPU %d", i);
                continue;
            }

            /* The very first sample for a core has no real previous reading to
             * diff against: every cpu->*_time field is still the memset-zero
             * from init_cpu_monitor(), so prev_total/prev_idle would compute
             * to 0 and the "delta" below would actually be the full jiffie
             * counts since boot — a real number, but meaningless as "usage
             * over the sampling interval", and one that would otherwise get
             * baked into current_usage/usage_history as if it were a genuine
             * measurement. Just establish the baseline instead: store the raw
             * counters, mark it, and wait for the second call (which now has
             * a real previous sample) to produce the first actual usage
             * figure. */
            if (!cpu->has_baseline) {
                cpu->user_time = user;
                cpu->nice_time = nice;
                cpu->system_time = system;
                cpu->idle_time = idle;
                cpu->iowait_time = iowait;
                cpu->irq_time = irq;
                cpu->softirq_time = softirq;
                cpu->steal_time = steal;
                cpu->has_baseline = 1;
                continue;
            }

            uint64_t prev_idle = cpu->idle_time + cpu->iowait_time;
            uint64_t idle_time = idle + iowait;

            uint64_t prev_total = cpu->user_time + cpu->nice_time +
                                cpu->system_time + prev_idle +
                                cpu->irq_time + cpu->softirq_time +
                                cpu->steal_time;

            uint64_t total_time = user + nice + system + idle_time +
                                irq + softirq + steal;

            uint64_t total_delta = total_time - prev_total;
            uint64_t idle_delta = idle_time - prev_idle;

            /* No jiffies elapsed between samples (polled faster than the
             * kernel's tick — the first-reading case is already handled
             * above by the has_baseline check): keep the previous figure
             * rather than dividing by zero. */
            if (total_delta > 0) {
                cpu->current_usage =
                    100.0 * (1.0 - ((double)idle_delta / (double)total_delta));

                // Update history
                cpu->usage_history[cpu->history_index] = cpu->current_usage;
                cpu->history_index =
                    (cpu->history_index + 1) % monitor->config->load_history_size;
                if (cpu->history_count < monitor->config->load_history_size) {
                    cpu->history_count++;
                }
            }

            // Update raw stats
            cpu->user_time = user;
            cpu->nice_time = nice;
            cpu->system_time = system;
            cpu->idle_time = idle;
            cpu->iowait_time = iowait;
            cpu->irq_time = irq;
            cpu->softirq_time = softirq;
            cpu->steal_time = steal;

            if (monitor->config->enable_load_prediction) {
                cpu->predicted_load = predict_cpu_load(cpu);
            }
        }
    }

    pthread_mutex_unlock(&monitor->lock);

    fclose(fp);
}

void check_load_balance(CPUMonitor* monitor) {
    if (!monitor || monitor->num_cpus < 2) return;

    double highest = 0.0, lowest = 100.0;
    int busiest = 0, idlest = 0;

    pthread_mutex_lock(&monitor->lock);
    for (int i = 0; i < monitor->num_cpus; i++) {
        double usage = monitor->stats[i].current_usage;
        if (usage > highest) { highest = usage; busiest = i; }
        if (usage < lowest)  { lowest = usage;  idlest = i; }
    }
    pthread_mutex_unlock(&monitor->lock);

    /* One core saturated while another sits idle is the imbalance this whole
     * program exists to avoid, so it is worth a log line when it happens. */
    if (highest > monitor->config->high_load_threshold &&
        lowest < monitor->config->low_load_threshold) {
        log_message(LOG_WARNING,
                    "Load imbalance: CPU %d at %.1f%% (above %.1f) while CPU %d "
                    "at %.1f%% (below %.1f)",
                    busiest, highest, monitor->config->high_load_threshold,
                    idlest, lowest, monitor->config->low_load_threshold);
    }
}

void cpu_monitor_adjust_active_tasks(CPUMonitor* monitor, int cpu_id, int delta) {
    if (!monitor || cpu_id < 0 || cpu_id >= monitor->num_cpus) return;

    pthread_mutex_lock(&monitor->lock);
    monitor->stats[cpu_id].active_tasks += delta;
    if (monitor->stats[cpu_id].active_tasks < 0) {
        monitor->stats[cpu_id].active_tasks = 0;
    }
    pthread_mutex_unlock(&monitor->lock);
}

double predict_cpu_load(CPUStats* cpu) {
    /* Simple moving average over the samples we actually hold.
     * Note this averages history_count entries, not history_index: the latter
     * is a write cursor that wraps back to 0, so using it as a count would
     * shrink the window to nothing every time the ring wrapped. */
    double sum = 0.0;

    if (cpu->history_count == 0) {
        return cpu->current_usage;
    }

    for (int i = 0; i < cpu->history_count; i++) {
        sum += cpu->usage_history[i];
    }

    return sum / cpu->history_count;
}


void print_cpu_stats(CPUMonitor* monitor) {
    if (monitor == NULL || monitor->stats == NULL) {
        printf("CPUMonitor is not initialized.\n");
        return;
    }

    printf("CPU Usage Statistics:\n");
    printf("------------------------------------------------------------\n");

    pthread_mutex_lock(&monitor->lock);

    for (int i = 0; i < monitor->num_cpus; ++i) {
        CPUStats* cpu = &monitor->stats[i];
        
        printf("CPU ID: %d\n", cpu->cpu_id);
        printf("  Current Usage: %.2f%%\n", cpu->current_usage);
        printf("  User Time: %lu\n", cpu->user_time);
        printf("  Nice Time: %lu\n", cpu->nice_time);
        printf("  System Time: %lu\n", cpu->system_time);
        printf("  Idle Time: %lu\n", cpu->idle_time);
        printf("  IOWait Time: %lu\n", cpu->iowait_time);
        printf("  IRQ Time: %lu\n", cpu->irq_time);
        printf("  SoftIRQ Time: %lu\n", cpu->softirq_time);
        printf("  Steal Time: %lu\n", cpu->steal_time);
        printf("  Predicted Load: %.2f%%\n", cpu->predicted_load);
        printf("  Active Tasks: %d\n", cpu->active_tasks);
        
        if (cpu->usage_history != NULL && cpu->history_count > 0) {
            int hist_size = monitor->config->load_history_size;
            int shown = cpu->history_count < 5 ? cpu->history_count : 5;

            /* Walk backwards from the write cursor so these really are the
             * most recent samples, oldest of the five printed first. */
            printf("  Usage History (last %d samples): ", shown);
            for (int j = shown; j >= 1; --j) {
                int idx = (cpu->history_index - j + hist_size) % hist_size;
                printf("%.2f%% ", cpu->usage_history[idx]);
            }
            printf("\n");
        }

        printf("------------------------------------------------------------\n");
    }

    pthread_mutex_unlock(&monitor->lock);
}

void cleanup_cpu_monitor(CPUMonitor* monitor) {
    if (monitor == NULL) {
        return; // Nothing to clean up
    }

    if (monitor->stats != NULL) {
        for (int i = 0; i < monitor->num_cpus; ++i) {
            CPUStats* cpu = &monitor->stats[i];
            
            // Free usage history if allocated
            if (cpu->usage_history != NULL) {
                free(cpu->usage_history);
                cpu->usage_history = NULL;
            }
        }
        
        // Free the stats array
        free(monitor->stats);
        monitor->stats = NULL;
    }

    /* The monitor borrows the config; it does not own it. main() allocates it
     * and frees it via free_config(), so freeing it here would double-free. */
    monitor->config = NULL;
    monitor->num_cpus = 0;

    pthread_mutex_destroy(&monitor->lock);
    free(monitor);
}
