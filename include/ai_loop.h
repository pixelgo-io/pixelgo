#ifndef AI_LOOP_H
#define AI_LOOP_H

#include "agent.h"
#include <stddef.h>

/*
 * Maximum number of iterations (assistant turn -> tool -> assistant ...) before
 * we stop the loop, so it does not run forever if the model gets stuck
 * requesting tools without finishing.
 *
 * 20 is enough for editing code, and far too few for anything involving
 * containers: bringing up a Docker stack costs several turns just to inspect
 * the environment before the first file is written, and every failed build
 * costs two more. Hitting the ceiling looks exactly like a crash from the
 * outside - the agent stops with no final response and the graph reports a
 * failed node - so the limit is worth raising and worth being able to change
 * without a rebuild.
 *
 * Override with PIXELGO_MAX_ITERATIONS.
 */
#define AI_LOOP_MAX_ITERATIONS 40

/* Reads the effective limit, honouring PIXELGO_MAX_ITERATIONS. */
int ai_loop_max_iterations(void);

/*
 * Runs the full loop of an AI agent for a given task/message.
 *
 * persist_history:
 *   0 = isolated conversation (one-shot). The history is NOT saved and NOT
 *       loaded. This is how nodes in a graph run: each start is clean.
 *   1 = continuous conversation (chat). The history is loaded from _history.json
 *       at the start and saved at the end, so the model remembers the previous
 *       turns.
 *
 * Returns 0 on success, -1 on error.
 */
int ai_loop_run_ex(agent_t *agent, const char *task, int persist_history);

/*
 * Like ai_loop_run_ex, but also returns the final response in `out` (if out != NULL).
 * Needed for the web API: there we cannot read from stdout, we need the text in
 * order to send it in the HTTP response.
 * out_size: the buffer size. The response is always NUL-terminated.
 */
int ai_loop_run_capture(agent_t *agent, const char *task, int persist_history,
                        char *out, size_t out_size);

/* The classic variant (one-shot, without persistent history). Kept for existing
   callers (orchestrator, agent_run). Equivalent to ai_loop_run_ex(a, t, 0). */
int ai_loop_run(agent_t *agent, const char *task);

#endif
