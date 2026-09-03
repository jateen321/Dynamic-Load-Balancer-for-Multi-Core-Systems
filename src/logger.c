#include "logger.h"
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <string.h>

static FILE* log_file = NULL;
static int log_file_owned = 0;   /* 0 when log_file is stderr: do not fclose it */
static int detailed_logging = 0;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

int init_logger(const char* file_path, int detailed) {
    int rc = 0;

    pthread_mutex_lock(&log_mutex);

    /* Re-init must not leak the previous stream. */
    if (log_file && log_file_owned) {
        fclose(log_file);
    }
    log_file = NULL;
    log_file_owned = 0;

    detailed_logging = detailed;

    if (file_path) {
        log_file = fopen(file_path, "a");
    }

    if (log_file) {
        log_file_owned = 1;
    } else {
        /* Fall back rather than going silent. */
        log_file = stderr;
        rc = -1;
    }

    pthread_mutex_unlock(&log_mutex);
    return rc;
}

void log_message(LogLevel level, const char* format, ...) {
    /* The NULL check belongs INSIDE the lock. Reading log_file outside it
     * races cleanup_logger()'s fclose/NULL store, and a thread that passed an
     * unlocked check could then write through a freed FILE*. */
    pthread_mutex_lock(&log_mutex);

    if (!log_file || (level == LOG_DEBUG && !detailed_logging)) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    /* ctime() returns a pointer to a single shared static buffer; two threads
     * logging at once would overwrite each other's timestamp. ctime_r() writes
     * into a caller-supplied buffer instead. */
    time_t now;
    time(&now);
    char date[32];
    if (ctime_r(&now, date)) {
        size_t len = strlen(date);
        if (len > 0 && date[len - 1] == '\n') date[len - 1] = '\0';
    } else {
        date[0] = '\0';
    }

    const char* level_str;
    switch (level) {
        case LOG_DEBUG:   level_str = "DEBUG"; break;
        case LOG_INFO:    level_str = "INFO"; break;
        case LOG_WARNING: level_str = "WARNING"; break;
        case LOG_ERROR:   level_str = "ERROR"; break;
        default:          level_str = "UNKNOWN";
    }

    fprintf(log_file, "[%s] [%s] ", date, level_str);

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);

    fprintf(log_file, "\n");
    fflush(log_file);

    pthread_mutex_unlock(&log_mutex);
}

void cleanup_logger(void) {
    pthread_mutex_lock(&log_mutex);

    if (log_file && log_file_owned) {
        fclose(log_file);
    }
    log_file = NULL;
    log_file_owned = 0;

    pthread_mutex_unlock(&log_mutex);
}
