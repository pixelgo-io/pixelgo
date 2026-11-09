#include "test.h"
#include "shutdown.h"
#include "jobs.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>

/*
 * Shutdown: does a stop request actually reach the bottom of the tree?
 *
 * The bug this suite is about. `serve` was stopped with Ctrl-C, the server
 * died, and a `reviewer` agent did not: five minutes later it logged a
 * 300-second approval timeout and exited. Nothing had gone wrong with the
 * agent - the signal never reached it. Each level of the tree puts the next one
 * in a NEW process group, and a signal sent to a group stops at that group.
 *
 * So the thing worth testing is not "is there a handler". A handler exists in
 * plenty of programs that leak processes. What has to hold is that a request
 * made at the top arrives at a GRANDCHILD in its own group, and that a timeout
 * long enough to be useless is not what ends the wait.
 *
 * Everything here uses `sleep` as the stand-in for an agent. No API key, no
 * network, nothing that depends on a model.
 */

/* --- helpers --------------------------------------------------------- */

/* Waits up to `ms` for the pid to disappear. Returns 1 if it did. */
static int gone_within(pid_t pid, int ms) {
    for (int i = 0; i < ms / 10; i++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) return 1;
        if (kill(pid, 0) != 0 && errno == ESRCH) return 1;
        usleep(10 * 1000);
    }
    return 0;
}

static int alive(pid_t pid) {
    return !(kill(pid, 0) != 0 && errno == ESRCH);
}

/* --- the flag and the sinks ------------------------------------------ */

static volatile sig_atomic_t g_sink_a_calls = 0;
static volatile sig_atomic_t g_sink_a_sig   = 0;
static volatile sig_atomic_t g_sink_b_calls = 0;

static void sink_a(int sig) { g_sink_a_calls++; g_sink_a_sig = (sig_atomic_t)sig; }
static void sink_b(int sig) { (void)sig; g_sink_b_calls++; }

static void test_flag_and_sinks(void) {
    section("the flag starts clear and the sinks run once each");

    ok("nothing requested yet", shutdown_requested() == 0);

    ok("sink registered", shutdown_register_sink(sink_a) == 1);
    ok("the same sink again is accepted and not duplicated",
       shutdown_register_sink(sink_a) == 1);
    ok("a second sink registered", shutdown_register_sink(sink_b) == 1);

    shutdown_request(SIGTERM);

    ok("the request is visible", shutdown_requested() == 1);
    eq_int("the first sink ran exactly once", (long)g_sink_a_calls, 1);
    eq_int("it was told which signal", (long)g_sink_a_sig, SIGTERM);
    eq_int("the second sink ran too", (long)g_sink_b_calls, 1);

    ok("a null sink is refused", shutdown_register_sink(NULL) == 0);
}

/*
 * The handler path, as opposed to calling shutdown_request() by hand.
 *
 * Done in a child because the flag cannot be cleared: this is a one-way switch
 * on purpose, and a test that flipped it in the parent would poison every test
 * after it. The child reports through its exit code.
 */
static void test_handler_sets_the_flag(void) {
    section("a signal sets the flag instead of killing the process");

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        shutdown_install_handlers(0);
        if (shutdown_requested()) _exit(2);   /* must still be clear */
        raise(SIGTERM);                       /* would be fatal by default */
        _exit(shutdown_requested() ? 0 : 3);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    ok("the process survived SIGTERM", WIFEXITED(status));
    eq_int("and saw the request", WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
}

/* --- the part that the Ctrl-C bug was about -------------------------- */

/*
 * A sink reaches a grandchild in its own process group.
 *
 * The shape of the real tree: this process starts a child, the child calls
 * setpgid(0,0) - as agent_run.c does - and starts a child of its own, which
 * inherits that new group. A signal to the group must take both. A signal to
 * the pid alone must not, which is the half the old code got wrong, and is
 * asserted here so that a fix which only signals pids cannot pass.
 */
static volatile sig_atomic_t g_group_leader = 0;

static void sink_kill_group(int sig) {
    (void)sig;
    pid_t pid = (pid_t)g_group_leader;
    if (pid > 0) kill(-pid, SIGTERM);
}

static void test_group_vs_pid(void) {
    section("a group leader's children die with it, a bare pid's do not");

    /* The pipe is how the child tells us its own child's pid. */
    int pipefd[2];
    if (pipe(pipefd) != 0) { ok("pipe", 0); return; }

    fflush(stdout);
    pid_t leader = fork();
    if (leader == 0) {
        close(pipefd[0]);
        setpgid(0, 0);                  /* its own group, like an agent */

        pid_t grand = fork();
        if (grand == 0) {               /* like a run_command child */
            execlp("sleep", "sleep", "30", (char *)NULL);
            _exit(127);
        }
        if (write(pipefd[1], &grand, sizeof(grand)) != (ssize_t)sizeof(grand)) _exit(1);
        close(pipefd[1]);

        /* SIGTERM is left at its default here: the point is the delivery, not
           what the agent does with it. */
        for (;;) pause();
    }
    close(pipefd[1]);
    setpgid(leader, leader);            /* the parent side, against the race */

    pid_t grand = 0;
    ssize_t n = read(pipefd[0], &grand, sizeof(grand));
    close(pipefd[0]);
    ok("the grandchild reported its pid", n == (ssize_t)sizeof(grand) && grand > 0);
    if (grand <= 0) { kill(leader, SIGKILL); waitpid(leader, NULL, 0); return; }

    /* First the wrong way: the pid only. */
    kill(leader, SIGTERM);
    ok("the leader is gone", gone_within(leader, 2000));
    ok("but its child survived a signal sent to the pid alone", alive(grand));

    /* Now the way a sink does it: the whole group. The leader is already dead,
       so the group id is all that is left of it - which is exactly the state a
       drain can find itself in. */
    g_group_leader = (sig_atomic_t)leader;
    sink_kill_group(SIGTERM);
    ok("signalling the group reaches the child", gone_within(grand, 2000));

    if (alive(grand)) { kill(grand, SIGKILL); waitpid(grand, NULL, 0); }
}

/*
 * The second signal.
 *
 * Someone pressing Ctrl+C twice is done waiting, so the second one has to be
 * fatal - a program that will not die is worse than one that leaves a mess. But
 * it must not be fatal silently: the sinks run again first, and they escalate
 * when they do, because this process is about to die and there is no later in
 * which to kill the children it asked nicely a second ago.
 *
 * Both halves are asserted. The child reports each sink call through a pipe and
 * then dies of the signal, and the parent reads the count from the exit status.
 */
static int g_sink_pipe = -1;

static void sink_reports(int sig) {
    char c = (char)sig;
    ssize_t n = write(g_sink_pipe, &c, 1);   /* write() is safe in a handler */
    (void)n;
}

static void test_second_signal_is_fatal(void) {
    section("the second signal runs the sinks again and then kills");

    int pipefd[2];
    if (pipe(pipefd) != 0) { ok("pipe", 0); return; }

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        g_sink_pipe = pipefd[1];
        shutdown_register_sink(sink_reports);
        shutdown_install_handlers(0);

        raise(SIGTERM);     /* first: drains, does not kill */
        raise(SIGTERM);     /* second: must not come back from here */
        _exit(9);           /* reached only if the second one was swallowed */
    }
    close(pipefd[1]);

    int status = 0;
    waitpid(pid, &status, 0);

    char buf[4] = {0};
    ssize_t n = read(pipefd[0], buf, sizeof(buf));
    close(pipefd[0]);

    eq_int("the sink ran twice, not once", (long)n, 2);
    ok("the process died of the signal rather than returning",
       WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    ok("so it never reached the line after it", !WIFEXITED(status));
}

/* --- the deadline ---------------------------------------------------- */

static void test_timeout_config(void) {
    section("the drain deadline is configurable and clamped");

    unsetenv("PIXELGO_STOP_TIMEOUT");
    eq_int("unset falls back to the default",
           shutdown_timeout_seconds(), SHUTDOWN_TIMEOUT_DEFAULT);

    setenv("PIXELGO_STOP_TIMEOUT", "45", 1);
    eq_int("a number is honoured", shutdown_timeout_seconds(), 45);

    setenv("PIXELGO_STOP_TIMEOUT", "", 1);
    eq_int("empty falls back", shutdown_timeout_seconds(), SHUTDOWN_TIMEOUT_DEFAULT);

    setenv("PIXELGO_STOP_TIMEOUT", "soon", 1);
    eq_int("nonsense falls back", shutdown_timeout_seconds(), SHUTDOWN_TIMEOUT_DEFAULT);

    setenv("PIXELGO_STOP_TIMEOUT", "0", 1);
    eq_int("zero falls back, it would skip the drain entirely",
           shutdown_timeout_seconds(), SHUTDOWN_TIMEOUT_DEFAULT);

    setenv("PIXELGO_STOP_TIMEOUT", "-5", 1);
    eq_int("negative falls back", shutdown_timeout_seconds(), SHUTDOWN_TIMEOUT_DEFAULT);

    setenv("PIXELGO_STOP_TIMEOUT", "99999", 1);
    eq_int("an absurd value is clamped",
           shutdown_timeout_seconds(), SHUTDOWN_TIMEOUT_MAX);

    unsetenv("PIXELGO_STOP_TIMEOUT");
}

/* --- finding the jobs to drain --------------------------------------- */

/*
 * jobs_list_running() is how the server finds its grandchildren, so what it
 * reports decides what gets signalled. Three cases matter and all three are
 * on disk at once: a live running job (signal it), a running job whose process
 * is gone (do not - its pid may belong to something else by now), and a
 * finished job (do not).
 */
static void write_job_file(const char *dir, const char *id,
                           const char *status, pid_t pid, const char *result) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.json", dir, id);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\"id\":\"%s\",\"status\":\"%s\",\"pid\":%d,\"kind\":\"flow\","
               "\"result\":\"%s\",\"error\":\"\"}",
            id, status, (int)pid, result);
    fclose(f);
}

static void test_jobs_list_running(void) {
    section("only the running jobs that still exist are listed");

    char tmpl[] = "/tmp/px_shutdown_XXXXXX";
    char *base = mkdtemp(tmpl);
    if (!base) { ok("mkdtemp", 0); return; }
    if (chdir(base) != 0) { ok("chdir", 0); return; }

    if (mkdir("jobs", 0755) != 0 && errno != EEXIST) { ok("mkdir jobs", 0); return; }

    /* A real live process to stand in for a running job. */
    fflush(stdout);
    pid_t live = fork();
    if (live == 0) { execlp("sleep", "sleep", "30", (char *)NULL); _exit(127); }

    /* And a pid that is certainly not running: one we start and reap. */
    fflush(stdout);
    pid_t dead = fork();
    if (dead == 0) _exit(0);
    waitpid(dead, NULL, 0);

    write_job_file("jobs", "live",   "running", live, "");
    write_job_file("jobs", "stale",  "running", dead, "");
    write_job_file("jobs", "done",   "done",    live, "");

    /*
     * A finished job whose result text contains the literal bytes of a running
     * job. The scan reads a fixed-size header for speed, so it has to cut the
     * result field off before matching - otherwise an agent could put a job on
     * the kill list by printing the right string.
     */
    write_job_file("jobs", "liar", "failed", live,
                   "\\\"status\\\":\\\"running\\\",\\\"pid\\\":1");

    /* Files in jobs/ that are not job state, which the scan must skip. */
    FILE *ev = fopen("jobs/live.events", "w");
    if (ev) { fprintf(ev, "{\"type\":\"noise\"}\n"); fclose(ev); }

    pid_t found[16];
    int n = jobs_list_running(found, 16);

    eq_int("exactly one job is listed", n, 1);
    ok("and it is the live one", n == 1 && found[0] == live);

    kill(live, SIGKILL);
    waitpid(live, NULL, 0);

    /* With the process gone, the same file must no longer be reported. */
    n = jobs_list_running(found, 16);
    eq_int("once its process is gone, nothing is listed", n, 0);
}

int main(void) {
    printf("\n=== shutdown ===\n");

    /* Order matters: everything before test_flag_and_sinks runs with the flag
       still clear, and the flag is deliberately one-way. */
    test_handler_sets_the_flag();
    test_second_signal_is_fatal();
    test_timeout_config();
    test_group_vs_pid();
    test_jobs_list_running();
    test_flag_and_sinks();       /* sets the flag for good - keep it last */

    return t_report("shutdown");
}
