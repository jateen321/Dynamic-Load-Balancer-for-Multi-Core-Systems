#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

LoadBalancerConfig* init_default_config(void) {
    LoadBalancerConfig* config = malloc(sizeof(LoadBalancerConfig));
    if (!config) return NULL;
    
    config->max_tasks = 10;
    config->monitoring_interval_ms = 100;
    config->high_load_threshold = 80.0;
    config->low_load_threshold = 20.0;
    config->load_history_size = 10;
    config->enable_load_prediction = 1;
    config->enable_detailed_logging = 1;
    config->log_file_path = strdup("./cpu_balancer.log");
    /* Default to every online core; main() overrides from argv. */
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    config->num_cpus = (online > 0) ? (int)online : 1;

    if (!config->log_file_path) {
        free(config);
        return NULL;
    }

    return config;
}

/* Not yet implemented: currently ignores the path and returns the defaults.
 * Kept so callers have a stable entry point once file parsing lands. */
LoadBalancerConfig* load_config(const char* config_path) {
    (void)config_path;
    return init_default_config();
}

void free_config(LoadBalancerConfig* config) {
    if (config) {
        free(config->log_file_path);
        free(config);
    }
}