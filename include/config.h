#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

typedef struct {
    int max_tasks;
    int monitoring_interval_ms;
    double high_load_threshold;
    double low_load_threshold;
    int load_history_size;
    int enable_load_prediction;
    int enable_detailed_logging;
    char* log_file_path;
    int num_cpus;
} LoadBalancerConfig;

// Initialize with default configuration
LoadBalancerConfig* init_default_config(void);

// Load configuration from file
LoadBalancerConfig* load_config(const char* config_path);

// Free configuration
void free_config(LoadBalancerConfig* config);

#endif