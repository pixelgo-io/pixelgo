#ifndef SHUTDOWN_H
#define SHUTDOWN_H

/*
 * Coordinated shutdown.
 *
 * The problem. A running flow is a four-level tree of processes, and each level
 * puts the next one in a NEW process group:
 *
 *   pixelgo serve            the server, one fork per connection
 *     connection child       runs the handler, forks the job
 *       job child            ai_loop (chat) or orchestrator (flow)
 *         agent              setpgid(0,0)      - agent_run.c
 *           run_command      setpgid(0,0)      - exec_tools.c
 *
 * A signal sent to a process group stops at that group. Ctrl+C goes to the
 * terminal's foreground group only, which is the server and its children up to
 * the job - so the agents and their commands never hear it. That is not a
 * handler that ran too late, it is one that was never invoked: the observed
 * symptom was a `reviewer` agent that outlived the server by five minutes and
 * then logged a 300s approval timeout, because the thing that would have
 * answered it no longer existed.
 *
 * `pixelgo serve stop` has the same shape of problem from the other side: it
 * sends SIGTERM to the pid in the PID file, so only the server hears it.
 *
 * The rule. Each level stops the level below it, because only that level knows
 * its own children. So shutdown is not one signal, it is a relay:
 *
 *   server     -> SIGTERM to each running job pid (read from the job files in jobs/),
 *                 then SIGKILL whatever is still there at the deadline
 *   job child  -> unwinds: agent_wait notices the request and calls agent_stop,
 *                 which signals the agent's GROUP (SIGTERM, grace, SIGKILL)
 *   agent      -> run_command's child group is signalled the same way
 *
 * This file is the plumbing for that relay: one flag, and a list of sinks that
 * each layer registers so it can pass the request down to the children only it
 * can see.
 *
 * Everything reachable from a handler is async-signal-safe: no allocation, no
 * stdio, no locks. A sink may call kill() and set a sig_atomic_t flag, and
 * nothing else. Logging a shutdown belongs in the code that unwinds, not in the
 * handler.
 */

/* How long a drain waits before it stops asking and starts killing. */
#define SHUTDOWN_TIMEOUT_DEFAULT 10
#define SHUTDOWN_TIMEOUT_MAX     300

/* A sink receives the signal number that started the shutdown. It runs inside
   a signal handler, so see the rule above about what it may do. */
typedef void (*shutdown_sink_fn)(int sig);

/* Registers a sink. Idempotent: registering the same function twice is a no-op,
   which lets a layer register from a path it runs more than once (agent_start,
   for instance). Returns 1 on success, 0 if the table is full. */
int shutdown_register_sink(shutdown_sink_fn fn);

/*
 * Installs the handlers for SIGTERM, SIGINT and SIGHUP.
 *
 * interrupt_blocking_calls:
 *   1  - without SA_RESTART, so a blocking syscall fails with EINTR. The
 *        server needs this: accept() has to return in order for the loop to
 *        notice the flag and start draining.
 *   0  - with SA_RESTART, so syscalls resume. The job child needs this: it is
 *        sitting in waitpid() or read(), and an EINTR there would read as a
 *        failure of the agent rather than a shutdown.
 *
 * SIGHUP is included because a foreground `serve` gets it when the terminal
 * closes, and that should drain rather than kill.
 *
 * A SECOND signal is fatal: the handler restores the default disposition and
 * re-raises. The first one has already asked every sink to stop its children,
 * so pressing Ctrl+C twice does not leave more behind than having had no
 * handler would have - it just stops waiting for the drain.
 */
void shutdown_install_handlers(int interrupt_blocking_calls);

/* Has a shutdown been requested? Safe to call from anywhere. */
int shutdown_requested(void);

/* Requests the shutdown: sets the flag and runs the sinks. Exposed because the
   tests drive it directly, and because a caller that discovers it cannot go on
   can start the same relay without a signal. Async-signal-safe. */
void shutdown_request(int sig);

/*
 * The drain deadline, in seconds. PIXELGO_STOP_TIMEOUT overrides the default;
 * anything unparseable, zero or negative falls back to it, and the value is
 * clamped to SHUTDOWN_TIMEOUT_MAX.
 *
 * It is configurable because it has to fit inside whatever supervises pixelgo:
 * under systemd, a drain longer than TimeoutStopSec is a drain that gets
 * SIGKILLed halfway through, which is the thing we are trying to avoid.
 */
long shutdown_timeout_seconds(void);

#endif
