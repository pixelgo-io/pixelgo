#include "test.h"
#include "agent_run.h"
#include "agent.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

/*
 * Does an agent's exit status survive the web server's SIGCHLD handler?
 *
 * The server forks per request and must not leave zombies, so it installs a
 * handler. The bug was that the handler reaped indiscriminately:
 *
 *     while (waitpid(-1, NULL, WNOHANG) > 0) { }
 *
 * waitpid(-1, ...) matches EVERY child, so whenever the handler fired first it
 * consumed an agent's exit status and threw it away. The orchestrator's own
 * waitpid then failed with ECHILD, agent_wait read that as AGENT_FAILED, and
 * the graph stopped on a node that had actually succeeded - while the agent
 * process carried on writing files.
 *
 * Short tool calls almost never lost the race. A multi-minute `docker compose
 * build` lost it every time.
 *
 * The fix is that the server reaps only the pids it forked itself. This test
 * reproduces both handlers side by side: the greedy one must destroy the
 * status, the targeted one must leave it alone. Asserting only the second would
 * pass equally well against a handler that was never installed at all, and
 * would not notice the bug coming back.
 *
 * Worker agents throughout - a plain command, no API key, no network.
 */

/* --- the two handlers, as the server had them before and after --- */

static void reaper_greedy(int sig) {
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
    errno = saved;
}

/*
 * The targeted handler reaps only pids it was told about. Nothing registers a
 * pid in this test, which is exactly the point: it mirrors the fixed server,
 * where agent pids are never tracked and so the handler never touches them.
 */
#define MAX_TRACKED 64
static volatile sig_atomic_t g_tracked[MAX_TRACKED];

static void reaper_targeted(int sig) {
    (void)sig;
    int saved = errno;
    for (int i = 0; i < MAX_TRACKED; i++) {
        pid_t pid = (pid_t)g_tracked[i];
        if (pid == 0) continue;
        if (waitpid(pid, NULL, WNOHANG) == pid) g_tracked[i] = 0;
    }
    errno = saved;
}

static void install(void (*fn)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

static void uninstall(void) { signal(SIGCHLD, SIG_DFL); }

/* --- helpers --- */

static agent_t make_worker(const char *script) {
    agent_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.id, sizeof(a.id), "probe");
    snprintf(a.dir, sizeof(a.dir), "/tmp");
    a.type = AGENT_TYPE_WORKER;
    snprintf(a.cfg.worker.command, sizeof(a.cfg.worker.command), "/bin/sh");
    snprintf(a.cfg.worker.args[0], MAX_STR, "-c");
    snprintf(a.cfg.worker.args[1], MAX_STR, "%s", script);
    a.cfg.worker.argc = 2;
    a.limits.timeout_seconds = 10;
    return a;
}

static int run(const char *script, agent_status_t *status_out) {
    agent_t a = make_worker(script);
    if (agent_start(&a, NULL) != 0) {
        *status_out = AGENT_FAILED;
        return -1;
    }
    int rc = agent_wait(&a);
    *status_out = a.status;
    return rc;
}

/* The race is timing-dependent, so one round proves nothing either way. */
static int count_correct(int rounds) {
    int correct = 0;
    for (int i = 0; i < rounds; i++) {
        agent_status_t st;
        if (run("sleep 0.2; exit 0", &st) == 0 && st == AGENT_STOPPED) correct++;
    }
    return correct;
}

int main(void) {
    const int ROUNDS = 12;
    char msg[200];

    section("baseline: no handler at all");
    uninstall();

    agent_status_t st;
    ok("a successful agent reports success", run("exit 0", &st) == 0 && st == AGENT_STOPPED);
    ok("a failing agent reports failure",    run("exit 3", &st) != 0 && st == AGENT_FAILED);

    /*
     * This section is the regression itself. If a future change reintroduces a
     * waitpid(-1, ...) handler, the targeted section below would still pass -
     * it is this one that shows the mechanism is real.
     */
    section("a greedy waitpid(-1) handler destroys the status (the old bug)");
    install(reaper_greedy);

    int correct = count_correct(ROUNDS);
    snprintf(msg, sizeof(msg),
             "%d/%d successful agents survive a greedy reaper (expected: fewer than all)",
             correct, ROUNDS);
    ok(msg, correct < ROUNDS);

    section("a targeted handler leaves agent statuses alone (the fix)");
    install(reaper_targeted);

    correct = count_correct(ROUNDS);
    snprintf(msg, sizeof(msg), "%d/%d successful agents reported correctly", correct, ROUNDS);
    ok(msg, correct == ROUNDS);

    /* The other direction matters just as much: a fix that reported everything
       as success would satisfy the check above and be worse than the bug. */
    ok("a genuinely failing agent is still reported as failed",
       run("sleep 0.2; exit 3", &st) != 0 && st == AGENT_FAILED);

    ok("an agent killed by a signal is reported as failed",
       run("sleep 0.2; kill -9 $$", &st) != 0 && st == AGENT_FAILED);

    uninstall();
    return t_report("reaping");
}
