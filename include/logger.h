#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR
} LogLevel;

/*
 * Returns 0 if the log file was opened, -1 if it could not be (or file_path
 * is NULL), in which case logging falls back to stderr rather than going
 * silent. Silence was the old behaviour and it hid every other diagnostic,
 * including the ones explaining why startup failed.
 */
int init_logger(const char* log_file, int detailed_logging);
void log_message(LogLevel level, const char* format, ...);
void cleanup_logger(void);

#endif