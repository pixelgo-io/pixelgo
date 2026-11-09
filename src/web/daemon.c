#include "daemon.h"
#include "shutdown.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Reads the pid and the port from the PID file. Returns 1 if the file exists. */
static int read_pidfile(pid_t *pid, int *port) {
    FILE *f = fopen(DAEMON_PID_FILE, "r");
    if (!f) return 0;

    int p = 0, prt = 0;
    int n = fscanf(f, "%d %d", &p, &prt);
    fclose(f);

    if (n < 1 || p <= 0) return 0;
    *pid = (pid_t)p;
    *port = prt;
    return 1;
}

static int write_pidfile(pid_t pid, int port) {
    FILE *f = fopen(DAEMON_PID_FILE, "w");
    if (!f) {
        LOG_E("daemon: cannot write '%s': %s", DAEMON_PID_FILE, strerror(errno));
        return 0;
    }
    fprintf(f, "%d %d\n", (int)pid, port);
    fclose(f);
    return 1;
}

void daemon_cleanup_pidfile(void) {
    remove(DAEMON_PID_FILE);
}

/* Checks whether the process with the given pid is really running. */
static int process_alive(pid_t pid) {
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0 || errno != ESRCH;
}

/* Refuses to start if another server already holds the PID file; clears it if
   the process it names is gone. Returns 0 when the caller may proceed. */
static int check_not_already_running(void) {
    pid_t old_pid = 0;
    int old_port = 0;

    if (read_pidfile(&old_pid, &old_port) && process_alive(old_pid)) {
        fprintf(stderr, "Server already running (pid %d, port %d).\n",
                (int)old_pid, old_port);
        fprintf(stderr, "Stop it first: pixelgo serve stop\n");
        return -1;
    }
    /* orphan PID file (dead process) - we delete it and continue */
    if (old_pid > 0) daemon_cleanup_pidfile();
    return 0;
}

int daemon_claim_pidfile(int port) {
    if (check_not_already_running() != 0) return -1;
    return write_pidfile(getpid(), port) ? 0 : -1;
}

int daemon_start(int port) {
    /* Already running? We do not start two daemons on the same PID file. */
    if (check_not_already_running() != 0) return -1;

    /* --- fork 1: the parent exits, the child continues --- */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid > 0) {
        /* parent: announces and exits, so the shell gets the prompt back */
        printf("Daemon started (pid %d) at http://127.0.0.1:%d\n", (int)pid, port);
        printf("  log:  %s\n", DAEMON_LOG_FILE);
        printf("  stop: pixelgo serve stop\n");
        exit(0);
    }

    /* --- child: becomes the daemon --- */

    /* A new session: we detach from the controlling terminal. Without this, a
       Ctrl+C in the parent terminal would kill the daemon too. */
    if (setsid() < 0) {
        perror("setsid");
        return -1;
    }

    /* The second fork: guarantees we cannot reacquire a controlling terminal
       (the process is no longer a session leader). */
    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    umask(0);

    /* We redirect I/O: stdin -> /dev/null, stdout+stderr -> the log file.
       We must close the inherited descriptors, otherwise the daemon would keep
       the terminal open. */
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        close(devnull);
    }

    int logfd = open(DAEMON_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd >= 0) {
        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        close(logfd);
    }

    /*
     * No signal handlers here.
     *
     * There used to be one, which deleted the PID file and called _exit(0). It
     * made `serve stop` look instant while leaving every job and agent running,
     * and it was overwritten a moment later by http_serve's own handlers
     * anyway. Shutdown now belongs to http_serve, which drains before it exits
     * and removes the PID file at the end of that. See shutdown.h.
     */
    if (!write_pidfile(getpid(), port)) return -1;

    LOG_I("daemon: started (pid %d, port %d)", (int)getpid(), port);
    return 0;   /* the caller continues with http_serve() */
}

int daemon_stop(void) {
    pid_t pid;
    int port;

    if (!read_pidfile(&pid, &port)) {
        fprintf(stderr, "Server is not running (%s not found).\n", DAEMON_PID_FILE);
        return -1;
    }
    if (!process_alive(pid)) {
        fprintf(stderr, "Server is not running (orphan PID file, removing it).\n");
        daemon_cleanup_pidfile();
        return -1;
    }

    if (kill(pid, SIGTERM) != 0) {
        fprintf(stderr, "Cannot stop the server (pid %d): %s\n",
                (int)pid, strerror(errno));
        return -1;
    }

    /*
     * We wait for the server's own drain, plus a margin.
     *
     * It must be longer than the drain, not shorter: the server spends up to
     * shutdown_timeout_seconds() asking its jobs to stop, and killing it
     * halfway through that would orphan exactly the processes the drain exists
     * to collect. The margin covers the final reap.
     */
    long grace = shutdown_timeout_seconds() + 2;
    printf("Stopping (up to %lds for the running jobs)...\n", grace);
    fflush(stdout);

    for (long i = 0; i < grace * 10; i++) {
        usleep(100 * 1000);
        if (!process_alive(pid)) {
            daemon_cleanup_pidfile();
            printf("Server stopped (pid %d).\n", (int)pid);
            return 0;
        }
    }

    /* It did not respond to SIGTERM - we force it. */
    fprintf(stderr, "Server not responding after %lds, sending SIGKILL...\n", grace);
    fprintf(stderr, "Jobs it had not finished draining may still be running; "
                    "check with: pixelgo serve status\n");
    kill(pid, SIGKILL);
    daemon_cleanup_pidfile();
    printf("Server force-stopped (pid %d).\n", (int)pid);
    return 0;
}

/*
 * "Server", not "Daemon": a foreground `serve` now writes the PID file too, so
 * this reports both. It had to - `serve stop` works by sending SIGTERM to the
 * pid in that file, and without one, half the ways of starting the server could
 * not be stopped by the command that exists for stopping it.
 *
 * What it still does not report is the work: a job listing is #13.
 */
int daemon_status(void) {
    pid_t pid;
    int port;

    if (!read_pidfile(&pid, &port)) {
        printf("Server: STOPPED\n");
        return 0;
    }
    if (!process_alive(pid)) {
        printf("Server: STOPPED (orphan PID file - process %d no longer exists)\n", (int)pid);
        return 0;
    }

    printf("Server: RUNNING\n");
    printf("  pid:  %d\n", (int)pid);
    printf("  url:  http://127.0.0.1:%d\n", port);
    printf("  log:  %s\n", DAEMON_LOG_FILE);
    return 1;
}
