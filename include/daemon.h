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

/*
 * Writes the PID file for a server that is NOT detaching, and refuses if
 * another one already holds it. Returns 0 on success, -1 otherwise.
 *
 * A foreground `serve` needs this for one reason: `serve stop` works by sending
 * SIGTERM to the pid in that file. Without it, a server started without
 * --daemon could not be stopped by the command that exists for stopping it, so
 * the shutdown path would be there and unreachable for half of the ways people
 * start the server.
 */
int daemon_claim_pidfile(int port);

/*
 * Stops the running server: reads the PID file, sends SIGTERM, and waits for
 * the drain (shutdown_timeout_seconds() plus a margin) before resorting to
 * SIGKILL. Returns 0 on success, -1 if it is not running or cannot be stopped.
 */
int daemon_stop(void);

/* Displays the server's status. Returns 1 if running, 0 otherwise. */
int daemon_status(void);

/* Deletes the PID file (called on clean exit). */
void daemon_cleanup_pidfile(void);

#endif
