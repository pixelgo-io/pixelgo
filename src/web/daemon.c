#include "daemon.h"
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

/* Termination handler: we delete the PID file so it does not stay orphaned. */
static void on_terminate(int sig) {
    (void)sig;
    daemon_cleanup_pidfile();
    _exit(0);
}

/* Checks whether the process with the given pid is really running. */
static int process_alive(pid_t pid) {
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0 || errno != ESRCH;
}

int daemon_start(int port) {
    /* Already running? We do not start two daemons on the same PID file. */
    pid_t old_pid;
    int old_port;
    if (read_pidfile(&old_pid, &old_port) && process_alive(old_pid)) {
        fprintf(stderr, "Daemon already running (pid %d, port %d).\n",
                (int)old_pid, old_port);
        fprintf(stderr, "Stop it first: pixelgo serve stop\n");
        return -1;
    }
    /* orphan PID file (dead process) - we delete it and continue */
    if (old_pid > 0) daemon_cleanup_pidfile();

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

    /* On SIGTERM/SIGINT: we delete the PID file and exit cleanly. */
    signal(SIGTERM, on_terminate);
    signal(SIGINT, on_terminate);

    if (!write_pidfile(getpid(), port)) return -1;

    LOG_I("daemon: started (pid %d, port %d)", (int)getpid(), port);
    return 0;   /* the caller continues with http_serve() */
}

int daemon_stop(void) {
    pid_t pid;
    int port;

    if (!read_pidfile(&pid, &port)) {
        fprintf(stderr, "Daemon is not running (%s not found).\n", DAEMON_PID_FILE);
        return -1;
    }
    if (!process_alive(pid)) {
        fprintf(stderr, "Daemon is not running (orphan PID file, removing it).\n");
        daemon_cleanup_pidfile();
        return -1;
    }

    if (kill(pid, SIGTERM) != 0) {
        fprintf(stderr, "Cannot stop daemon (pid %d): %s\n",
                (int)pid, strerror(errno));
        return -1;
    }

    /* We wait up to 5s for it to die cleanly. */
    for (int i = 0; i < 50; i++) {
        usleep(100 * 1000);
        if (!process_alive(pid)) {
            daemon_cleanup_pidfile();
            printf("Daemon stopped (pid %d).\n", (int)pid);
            return 0;
        }
    }

    /* It did not respond to SIGTERM - we force it. */
    fprintf(stderr, "Daemon not responding, sending SIGKILL...\n");
    kill(pid, SIGKILL);
    daemon_cleanup_pidfile();
    printf("Daemon force-stopped (pid %d).\n", (int)pid);
    return 0;
}

int daemon_status(void) {
    pid_t pid;
    int port;

    if (!read_pidfile(&pid, &port)) {
        printf("Daemon: STOPPED\n");
        return 0;
    }
    if (!process_alive(pid)) {
        printf("Daemon: STOPPED (orphan PID file - process %d no longer exists)\n", (int)pid);
        return 0;
    }

    printf("Daemon: RUNNING\n");
    printf("  pid:  %d\n", (int)pid);
    printf("  url:  http://127.0.0.1:%d\n", port);
    printf("  log:  %s\n", DAEMON_LOG_FILE);
    return 1;
}
