#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static log_level_t g_level = LOG_INFO;
static int g_initialized = 0;

static const char *level_name(log_level_t level) {
    switch (level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        default:        return "?";
    }
}

static log_level_t level_from_string(const char *s) {
    if (!s) return LOG_INFO;
    if (strcasecmp(s, "debug") == 0) return LOG_DEBUG;
    if (strcasecmp(s, "info")  == 0) return LOG_INFO;
    if (strcasecmp(s, "warn")  == 0) return LOG_WARN;
    if (strcasecmp(s, "warning") == 0) return LOG_WARN;
    if (strcasecmp(s, "error") == 0) return LOG_ERROR;
    return LOG_INFO;
}

void log_init(void) {
    if (g_initialized) return;
    const char *env = getenv("PIXELGO_LOG_LEVEL");
    if (env && *env) g_level = level_from_string(env);
    g_initialized = 1;
}

void log_set_level(log_level_t level) {
    g_level = level;
    g_initialized = 1;
}

log_level_t log_get_level(void) {
    return g_level;
}

void log_msg(log_level_t level, const char *fmt, ...) {
    if (!g_initialized) log_init();
    if (level < g_level) return;

    /* timestamp: YYYY-MM-DD HH:MM:SS in local time */
    char ts[32];
    time_t now = time(NULL);
    struct tm tmbuf;
    localtime_r(&now, &tmbuf);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmbuf);

    /* we prefix with the pid, useful when agent forks run in parallel */
    fprintf(stderr, "%s [%-5s] (pid %d) ", ts, level_name(level), (int)getpid());

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
    fflush(stderr);
}
