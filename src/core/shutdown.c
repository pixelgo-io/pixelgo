#include "shutdown.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SHUTDOWN_MAX_SINKS 8

static shutdown_sink_fn g_sinks[SHUTDOWN_MAX_SINKS];
static volatile sig_atomic_t g_sink_count = 0;
static volatile sig_atomic_t g_requested = 0;

int shutdown_register_sink(shutdown_sink_fn fn) {
    if (!fn) return 0;

    int n = (int)g_sink_count;
    for (int i = 0; i < n; i++)
        if (g_sinks[i] == fn) return 1;          /* already registered */

    if (n >= SHUTDOWN_MAX_SINKS) return 0;

    /*
     * The slot is written before the count is raised. A handler firing between
     * the two statements reads the old count and runs one sink fewer, which is
     * a sink that had not finished being registered; it never reads a slot that
     * is half written.
     */
    g_sinks[n] = fn;
    g_sink_count = (sig_atomic_t)(n + 1);
    return 1;
}

int shutdown_requested(void) {
    return g_requested != 0;
}

void shutdown_request(int sig) {
    g_requested = 1;

    int n = (int)g_sink_count;
    for (int i = 0; i < n; i++)
        if (g_sinks[i]) g_sinks[i](sig);
}

static void on_signal(int sig) {
    int saved_errno = errno;   /* the handler must not clobber errno */

    /*
     * A second signal means stop asking.
     *
     * The first one starts a drain, which is the right default but is not
     * instant: it can be waiting on a model call that has nothing to do with
     * signals. Someone pressing Ctrl+C twice is telling us they are done
     * waiting, and a program that refuses to die is worse than one that leaves
     * a mess. We put the default disposition back and re-raise, so the second
     * signal does what it would have done without us.
     *
     * The first signal has already asked every sink to stop its children, so
     * this is not the same as having had no handler at all.
     */
    int second = g_requested;

    /*
     * The sinks run on the second signal too, and escalate when they do: the
     * first pass asks children to stop, this one kills them. Without it, a
     * second Ctrl+C killed this process in the middle of a grace period and
     * left behind exactly the children that had ignored the first SIGTERM.
     */
    shutdown_request(sig);

    if (second) {
        signal(sig, SIG_DFL);
        raise(sig);
    }
    errno = saved_errno;
}

void shutdown_install_handlers(int interrupt_blocking_calls) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = interrupt_blocking_calls ? 0 : SA_RESTART;

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

long shutdown_timeout_seconds(void) {
    const char *s = getenv("PIXELGO_STOP_TIMEOUT");
    if (!s || !*s) return SHUTDOWN_TIMEOUT_DEFAULT;

    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || v <= 0) return SHUTDOWN_TIMEOUT_DEFAULT;
    if (v > SHUTDOWN_TIMEOUT_MAX) return SHUTDOWN_TIMEOUT_MAX;
    return v;
}
