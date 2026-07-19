#include "tools.h"
#include <stdio.h>
#include <stdarg.h>

tool_result_t tool_ok(const char *fmt, ...) {
    tool_result_t r = {0};
    r.ok = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.output, TOOL_RESULT_MAX, fmt, ap);
    va_end(ap);
    return r;
}

tool_result_t tool_err(const char *fmt, ...) {
    tool_result_t r = {0};
    r.ok = 0;
    /* We leave room for the "Error: " prefix (7 characters) so we do not truncate the message. */
    char msg[TOOL_RESULT_MAX - 8];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    /* A uniform "Error:" prefix so the model recognizes failures consistently. */
    snprintf(r.output, TOOL_RESULT_MAX, "Error: %s", msg);
    return r;
}
