#include "agent_run.h"
#include "ai_loop.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/types.h>

/* Applies the resource limits in the child process (after fork, before exec/loop).
   Only fields > 0 are enforced; 0 = keep the system default. */
static void apply_rlimits(const resource_limits_t *lim) {
    if (!lim) return;
    struct rlimit rl;

    if (lim->cpu_seconds > 0) {
        rl.rlim_cur = rl.rlim_max = (rlim_t)lim->cpu_seconds;
        if (setrlimit(RLIMIT_CPU, &rl) != 0)
            LOG_W("rlimit: RLIMIT_CPU failed: %s", strerror(errno));
    }
    if (lim->mem_bytes > 0) {
        rl.rlim_cur = rl.rlim_max = (rlim_t)lim->mem_bytes;
        if (setrlimit(RLIMIT_AS, &rl) != 0)
            LOG_W("rlimit: RLIMIT_AS failed: %s", strerror(errno));
    }
    if (lim->fsize_bytes > 0) {
        rl.rlim_cur = rl.rlim_max = (rlim_t)lim->fsize_bytes;
        if (setrlimit(RLIMIT_FSIZE, &rl) != 0)
            LOG_W("rlimit: RLIMIT_FSIZE failed: %s", strerror(errno));
    }
}

int agent_start(agent_t *agent, const char *task) {
    /* Critical: flush ANYTHING in the stdio buffer BEFORE fork(), otherwise the
       unflushed content is duplicated in the child. */
    fflush(stdout);
    fflush(stderr);

    LOG_I("agent_start: starting '%s' (type=%s, dir=%s)", agent->id,
          agent->type == AGENT_TYPE_WORKER ? "worker" : "ai", agent->dir);

    pid_t pid = fork();
    if (pid < 0) {
        LOG_E("agent_start: fork failed: %s", strerror(errno));
        agent->status = AGENT_FAILED;
        return -1;
    }

    if (pid == 0) {
        /* --- child process --- */
        /* Its own process group: lets us kill the whole subtree (the agent + any
           children it has) with a single kill(-pgid). */
        setpgid(0, 0);

        /*
         * BUG FIXED: the web server installs a SIGCHLD handler (a zombie reaper).
         * The agents are its GRANDCHILDREN, so they inherit the handler through
         * fork. When run_command forks, the inherited handler REAPS the child
         * before waitpid gets to it -> "No child processes" -> we report exit code
         * -1, even though the command worked perfectly.
         *
         * Symptom: gcc compiled cleanly, but the agent received "Exit code: -1
         * (command failed)" and spun in a loop.
         *
         * Fixed: we reset SIGCHLD to the default behavior. The agent reaps its own
         * children, with waitpid.
         */
        signal(SIGCHLD, SIG_DFL);

        /* We make the paths ABSOLUTE before the chdir. Otherwise, any code using
           agent->dir after the chdir (the tools' sandbox, history) would get a
           relative path that no longer resolves from the new cwd. */
        char abs_dir[MAX_PATH_LEN];
        if (realpath(agent->dir, abs_dir))
            snprintf(agent->dir, MAX_PATH_LEN, "%s", abs_dir);

        char abs_shared[MAX_PATH_LEN];
        if (agent->shared_dir[0] && realpath(agent->shared_dir, abs_shared))
            snprintf(agent->shared_dir, MAX_PATH_LEN, "%s", abs_shared);

        /*
         * The WORKING directory is the SHARED one (that is where the code all the
         * agents work on lives). The agent's private files (_history.json,
         * _output.txt) are written through absolute paths into agent->dir, so they
         * do not mix.
         *
         * If shared does not exist (an old workspace), we stay in our own dir.
         */
        const char *workdir = agent->shared_dir[0] ? agent->shared_dir : agent->dir;

        if (chdir(workdir) != 0) {
            LOG_E("agent_start(child): chdir '%s' failed: %s", workdir, strerror(errno));
            _exit(127);
        }

        apply_rlimits(&agent->limits);

        if (agent->type == AGENT_TYPE_WORKER) {
            char *argv[MAX_TOOLS + 2];
            argv[0] = agent->cfg.worker.command;
            int i;
            for (i = 0; i < agent->cfg.worker.argc; i++)
                argv[i + 1] = agent->cfg.worker.args[i];
            argv[i + 1] = NULL;

            execv(agent->cfg.worker.command, argv);
            LOG_E("agent_start(child): execv '%s' failed: %s",
                  agent->cfg.worker.command, strerror(errno));
            _exit(127);
        } else {
            const char *t = task ? task : "Default test task - replace with a real task";
            int rc = ai_loop_run(agent, t);
            fflush(stdout);
            fflush(stderr);
            _exit(rc == 0 ? 0 : 1);
        }
    }

    /* --- parent process --- */
    setpgid(pid, pid); /* here too, to avoid a race with the child */
    agent->pid = pid;
    agent->status = AGENT_RUNNING;
    return 0;
}

/* Sends a signal to the agent's whole process group. */
static void signal_group(pid_t pid, int sig) {
    if (pid > 0) kill(-pid, sig);
}

/* Waits non-blockingly for termination; returns:
    1  = it finished (status placed in *wstatus)
    0  = still running
   -1  = error */
static int try_reap(pid_t pid, int *wstatus) {
    pid_t r = waitpid(pid, wstatus, WNOHANG);
    if (r == pid) return 1;
    if (r == 0)   return 0;
    return -1;
}

/*
 * Safety net for the terminal.
 *
 * An agent asking for approval takes the terminal foreground so it can read
 * stdin. It hands it back on exit and on signals, but a SIGKILL leaves no
 * chance to clean up - and then the terminal belongs to a dead process group,
 * so the shell never regains it and the session looks frozen.
 *
 * The parent always outlives the agent, so it reclaims the terminal here. This
 * is a no-op in the normal case, where the agent already gave it back.
 */
static void reclaim_terminal(void) {
    if (!isatty(STDIN_FILENO)) return;

    pid_t mine = getpgrp();
    if (tcgetpgrp(STDIN_FILENO) == mine) return;   /* already ours */

    void (*old)(int) = signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(STDIN_FILENO, mine);
    signal(SIGTTOU, old);
}

int agent_wait(agent_t *agent) {
    if (agent->pid <= 0) return -1;

    long timeout = agent->limits.timeout_seconds;

    if (timeout <= 0) {
        /* no timeout: simple blocking wait */
        int status;
        if (waitpid(agent->pid, &status, 0) < 0) {
            LOG_E("agent_wait: waitpid failed: %s", strerror(errno));
            agent->status = AGENT_FAILED;
            agent->pid = 0;
            reclaim_terminal();
            return -1;
        }
        reclaim_terminal();
        int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        agent->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        agent->status = ok ? AGENT_STOPPED : AGENT_FAILED;
        LOG_I("agent_wait: '%s' finished, exit=%d status=%s",
              agent->id, agent->exit_code, ok ? "OK" : "FAILED");
        agent->pid = 0;
        return ok ? 0 : -1;
    }

    /* with timeout: polling every 100ms until the deadline */
    time_t deadline = time(NULL) + timeout;
    int status;
    for (;;) {
        int r = try_reap(agent->pid, &status);
        if (r == 1) {
            reclaim_terminal();
            int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            agent->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            agent->status = ok ? AGENT_STOPPED : AGENT_FAILED;
            LOG_I("agent_wait: '%s' finished, exit=%d status=%s",
                  agent->id, agent->exit_code, ok ? "OK" : "FAILED");
            agent->pid = 0;
            return ok ? 0 : -1;
        }
        if (r < 0) {
            LOG_E("agent_wait: waitpid failed: %s", strerror(errno));
            agent->status = AGENT_FAILED;
            agent->pid = 0;
            reclaim_terminal();
            return -1;
        }
        if (time(NULL) >= deadline) {
            LOG_W("agent_wait: '%s' exceeded the %lds timeout, stopping it",
                  agent->id, timeout);
            agent_stop(agent);
            agent->status = AGENT_TIMEOUT;
            reclaim_terminal();
            return -1;
        }
        struct timespec ts = { 0, 100 * 1000 * 1000 }; /* 100ms */
        nanosleep(&ts, NULL);
    }
}

int agent_stop(agent_t *agent) {
    if (agent->pid <= 0) return 0;
    pid_t pid = agent->pid;

    LOG_I("agent_stop: controlled shutdown of '%s' (pid %d)", agent->id, (int)pid);

    /* 1. a polite request */
    signal_group(pid, SIGTERM);

    /* 2. grace period ~3s, checking whether it died */
    int status;
    for (int i = 0; i < 30; i++) {
        int r = try_reap(pid, &status);
        if (r == 1) { agent->pid = 0; LOG_I("agent_stop: '%s' stopped cleanly", agent->id); return 0; }
        if (r < 0)  { agent->pid = 0; return 0; }
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    /* 3. by force */
    LOG_W("agent_stop: '%s' did not respond to SIGTERM, sending SIGKILL", agent->id);
    signal_group(pid, SIGKILL);
    waitpid(pid, &status, 0); /* reap so no zombie remains */
    agent->pid = 0;
    return 0;
}

int agent_run_supervised(agent_t *agent, const char *task, int max_restarts) {
    agent->restart_count = 0;
    for (;;) {
        if (agent_start(agent, task) != 0) return -1;
        int rc = agent_wait(agent);
        if (rc == 0) return 0; /* success */

        /* We do not restart if it was stopped by timeout - it is probably structurally stuck. */
        if (agent->status == AGENT_TIMEOUT) {
            LOG_E("supervised: '%s' stopped by timeout, not restarting", agent->id);
            return -1;
        }
        if (agent->restart_count >= max_restarts) {
            LOG_E("supervised: '%s' failed, restart limit of %d reached",
                  agent->id, max_restarts);
            return -1;
        }
        agent->restart_count++;
        LOG_W("supervised: '%s' failed (exit=%d), restart %d/%d",
              agent->id, agent->exit_code, agent->restart_count, max_restarts);
    }
}
