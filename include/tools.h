#ifndef TOOLS_H
#define TOOLS_H

#include "agent.h"

/*
 * How much output a tool may return to the model.
 *
 * This is a context-window budget, not a disk limit: every byte here is resent
 * on every subsequent turn, so a large value spends tokens for the rest of the
 * agent's run. But too small is worse than expensive - a build that overflows
 * it gets cut short, and the model is asked to debug from a fragment.
 *
 * 32 KB is chosen against real output: `docker compose up --build` for a PHP
 * image runs to roughly 20-30 KB once apt is pulling packages. At 8 KB that
 * build was truncated every time, and the truncation landed in the middle of
 * the package list - the part that says whether it worked never arrived.
 */
#define TOOL_RESULT_MAX 32768

/* How much of the END of an oversized output to keep. A command that overflows
   the buffer usually says what went wrong in its last lines, so we keep the
   head (what it set out to do) and the tail (how it ended), dropping the
   middle. */
#define TOOL_TAIL_KEEP 4096

/* The result of executing a tool - always text, so it can be sent straight to the model */
typedef struct {
    int ok;
    char output[TOOL_RESULT_MAX];
} tool_result_t;

/* The signature of any tool implementation. input_json = the arguments (a
   serialized JSON object) sent by the model. */
typedef tool_result_t (*tool_fn_t)(agent_t *agent, const char *input_json);

/* An entry in the tool registry. Registration is table-driven, so adding a new
   tool means a single line in tool_registry[]. */
typedef struct {
    const char *name;
    tool_fn_t   fn;
} tool_entry_t;

/*
 * The single entry point for any tool requested by the model.
 * The ONLY place in the whole codebase where a tool actually executes.
 * It checks, in order:
 *   1. is the agent allowed to use tool_name? (agent->cfg.ai.tools[])
 *   2. does the tool exist in the registry?
 *   3. only then does it execute (each tool validates its own arguments).
 */
tool_result_t tool_dispatch(agent_t *agent, const char *tool_name, const char *input_json);

/* Returns 1 if this tool call must be approved by the human before it runs,
   i.e. the tool is listed in the agent's approval_tools. */
int tool_needs_approval(agent_t *agent, const char *tool_name);

/* checks whether the agent has the requested tool in its allowed tool list */
int agent_has_tool(agent_t *agent, const char *tool_name);

/* --- Helpers for the tool implementations (in tools_common.c) --- */

/* Builds a success result with formatted output (printf-style). */
tool_result_t tool_ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Builds a standardized error result (ok=0) with a formatted message. */
tool_result_t tool_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
