#ifndef AGENT_RUN_H
#define AGENT_RUN_H

#include "agent.h"

/* The task given to an AI agent. May be NULL -> a default task is used.
   Ignored for a worker. Kept here so it can be passed from the CLI later. */

/* Starts the agent (fork + exec for worker, fork + ai_loop for ai). Non-blocking.
   Applies the resource limits (rlimit) in the child process before exec/loop.
   Returns 0 on success, -1 on startup error. */
int agent_start(agent_t *agent, const char *task);

/* Waits for the agent to finish, updates status. If limits.timeout_seconds > 0,
   applies a wall-clock timeout: on expiry, sends SIGTERM (then SIGKILL if it does
   not die) and sets status = AGENT_TIMEOUT.
   Returns 0 if it ended cleanly, -1 if it was force-stopped/errored. */
int agent_wait(agent_t *agent);

/* Controlled termination: SIGTERM, grace period, then SIGKILL. Does cleanup
   (reap) so no zombie processes remain. Safe to call even if already stopped. */
int agent_stop(agent_t *agent);

/* Runs the agent supervised: start + wait, and if it fails (not on timeout, not
   on exit code 0) restarts it for up to max_restarts attempts.
   Returns 0 if it eventually succeeded, -1 otherwise. */
int agent_run_supervised(agent_t *agent, const char *task, int max_restarts);

#endif
