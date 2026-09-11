#ifndef DATA_REQUEST_AUDIT_H
#define DATA_REQUEST_AUDIT_H

#include <stddef.h>
#include "agent.h"

/*
 * Data Request Approval Gateway.
 *
 * Before a request is sent to an LLM provider, ai_loop can check its size
 * against a per-agent threshold (agent->cfg.ai.approve_data_threshold_bytes).
 * If the payload is over the threshold, the human is asked to approve it -
 * reusing the SAME approval channel already used for tool calls
 * (ask_approval() in ai_loop.c, which in turn uses approval_wait()/the CLI
 * prompt from events.c). No new cross-process mechanism is introduced: the
 * existing one already solves the fork/no-shared-memory problem correctly.
 *
 * This module's own job is narrow:
 *   1. build a human-readable summary of the payload/cost, and
 *   2. append every decision to a persistent audit log on disk.
 */

/* Builds a one-line summary such as:
 *   "1.2 MB payload (~245,000 tokens), estimated cost $0.87 to
 *    anthropic/claude-sonnet-5"
 * `out` must be at least 256 bytes. */
void data_audit_build_summary(llm_provider_id_t provider, const char *model,
                              size_t payload_bytes, long tokens_estimated,
                              long cost_micro_usd,
                              char *out, size_t out_size);

/* Rough token estimate from raw byte length of prompt + history.
 * ~4 bytes/token is the same rule of thumb used throughout the industry for a
 * quick warning-level estimate; it is not meant to match the provider's own
 * tokenizer exactly. */
long data_audit_estimate_tokens(size_t payload_bytes);

/*
 * Parses a --data-threshold specification: a raw byte count, a size with a
 * KB/MB/GB suffix (case-insensitive, e.g. "500KB", "2MB"), or the literal
 * "off" (case-insensitive) meaning explicitly disabled. Used identically by
 * the CLI flag, workspace.conf's `data_approve_threshold` key, and
 * PIXELGO_DATA_THRESHOLD - one parser, so all three accept exactly the same
 * spellings.
 *
 * On success, *out_bytes is 0 for "off" or a positive byte count, and the
 * function returns 1. Returns 0 (and leaves *out_bytes untouched) if `str`
 * is NULL, empty, or not parseable as either form.
 *
 * An unrecognized unit suffix is treated as raw bytes rather than rejected,
 * matching the original behavior of this parser before it was factored out.
 */
int data_threshold_parse(const char *str, long *out_bytes);

/*
 * The GLOBAL default threshold, from PIXELGO_DATA_THRESHOLD - used for any
 * agent whose own threshold is unset (see ai_config_t.approve_data_threshold_bytes
 * in agent.h for the three-state semantics this resolves). Read fresh from
 * getenv() each call, same convention as the other PIXELGO_* runtime knobs
 * (PIXELGO_COMMAND_TIMEOUT, PIXELGO_MAX_ITERATIONS, PIXELGO_HTTP_MAX_BODY).
 * Returns 0 (disabled) if the variable is unset or cannot be parsed.
 */
long data_threshold_global(void);

/*
 * Appends one line to ~/.local/share/pixelgo/data_audit.log recording the
 * decision. Best-effort: a logging failure never blocks the agent.
 *
 * decision: "approved" or "denied"
 */
void data_audit_log_decision(const char *workspace_id, const char *agent_id,
                             size_t payload_bytes, long tokens_estimated,
                             long cost_micro_usd, const char *decision);

#endif /* DATA_REQUEST_AUDIT_H */
