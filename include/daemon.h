#ifndef DAEMON_H
#define DAEMON_H

/*
 * Daemonization: running the server in the background, detached from the terminal.
 *
 * Without this, `pixelgo serve` dies when you close the terminal. With `--daemon`:
 *   - the process detaches (fork + setsid), so it no longer has a controlling terminal
 *   - it writes a PID file, so we can stop/query it later
 *   - it redirects stdout/stderr into a log file
 *
 * Commands:
 *   pixelgo serve --daemon [port]   starts in the background
 *   pixelgo serve stop              stops the daemon
 *   pixelgo serve status            says whether it is running and on what pid/port
 */

#define DAEMON_PID_FILE "pixelgo.pid"
#define DAEMON_LOG_FILE "pixelgo.log"

/* Detaches the current process (fork + setsid + I/O redirection) and writes the
   PID file. Returns 0 on success (in the detached process), -1 on error.
   The parent does NOT return - it exits immediately with exit(0). */
int daemon_start(int port);

/* Stops the running daemon (reads the PID file, sends SIGTERM).
   Returns 0 on success, -1 if it is not running or cannot be stopped. */
int daemon_stop(void);

/* Displays the daemon's status. Returns 1 if running, 0 otherwise. */
int daemon_status(void);

/* Deletes the PID file (called on clean exit). */
void daemon_cleanup_pidfile(void);

#endif
