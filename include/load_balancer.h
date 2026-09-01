#ifndef LOAD_BALANCER_H
#define LOAD_BALANCER_H

#include "config.h"
#include "cpu_stats.h"
#include "task_queue.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>


typedef struct {
    LoadBalancerConfig* config;
    CPUMonitor* cpu_monitor;
    TaskQueue* task_queue;
    pthread_t monitor_thread;
    pthread_t scheduler_thread;
    /* Written by stop_load_balancer(), read by the worker threads. Atomic
     * rather than plain int: concurrent access to a plain int is a data race,
     * and `volatile` orders nothing between threads. */
    atomic_int running;
    int threads_started;
} LoadBalancer;

LoadBalancer* init_load_balancer(LoadBalancerConfig* config);
int submit_task(LoadBalancer* lb, void (*function)(void*), void* args, TaskPriority priority);
void start_load_balancer(LoadBalancer* lb);
void stop_load_balancer(LoadBalancer* lb);
void* monitor_thread_func(void* arg);
void* scheduler_thread_func(void* arg);
int find_best_cpu(CPUMonitor* monitor);
void wait_for_tasks_completion(LoadBalancer* lb);
void cancel_pending_tasks(LoadBalancer* lb);
void cleanup_load_balancer(LoadBalancer* lb);

/* Number of task threads currently running. */
int load_balancer_active_tasks(LoadBalancer* lb);

#endif