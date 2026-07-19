#ifndef LOG_H
#define LOG_H

/*
 * Simple, centralized, thread-agnostic logging system (the project is
 * single-threaded per process; forks inherit the current configuration).
 *
 * Levels: DEBUG < INFO < WARN < ERROR. Below the configured level, messages
 * are ignored. The default level is INFO; it can be raised/lowered via the
 * PIXELGO_LOG_LEVEL environment variable (debug|info|warn|error) or via
 * log_set_level().
 *
 * The default output is stderr (so it does not mix with the agents' "useful"
 * output on stdout). Format: timestamp [LEVEL] message.
 */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} log_level_t;

/* Initializes logging. Reads PIXELGO_LOG_LEVEL from the environment if present.
   Idempotent - it can be called several times without side effects. */
void log_init(void);

/* Manually sets the minimum level displayed. */
void log_set_level(log_level_t level);

/* The current level (useful to avoid building expensive messages for nothing). */
log_level_t log_get_level(void);

/* Logs a formatted message (printf-style) at the given level. */
void log_msg(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Convenience macros. */
#define LOG_D(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define LOG_I(...) log_msg(LOG_INFO,  __VA_ARGS__)
#define LOG_W(...) log_msg(LOG_WARN,  __VA_ARGS__)
#define LOG_E(...) log_msg(LOG_ERROR, __VA_ARGS__)

#endif
