#ifndef HISTORY_H
#define HISTORY_H

#include "agent.h"
#include "cJSON.h"

/*
 * Persistent conversation history.
 *
 * Until now, the history (the message array) lived only in memory for the
 * duration of one ai_loop_run() and was lost on exit. That makes a CONVERSATION
 * impossible: each run started from scratch (amnesia).
 *
 * Now we save it to <agent_dir>/_history.json after each turn and reload it on
 * the next one. The file is in the agent's sandbox, so it is inspectable on disk.
 *
 * SIZE: a long conversation exceeds the model's context limit and the API
 * errors out. Keeping it inside the window is history_compact(), below.
 */

#define HISTORY_FILE "_history.json"

/* Loads the agent's history from <dir>/_history.json.
   Returns a cJSON array (empty if the file does not exist or is invalid).
   The caller must free it with cJSON_Delete(). Never returns NULL. */
cJSON *history_load(const agent_t *agent);

/* Saves the history to <dir>/_history.json. Returns 1 on success. */
int history_save(const agent_t *agent, const cJSON *messages);

/* Deletes the history (starts a new conversation). Returns 1 on success. */
int history_reset(const agent_t *agent);

/* ------------------------------------------------------------------ *
 *  Compaction
 *
 *  Counting messages is not counting anything: a user turn is 50 bytes and
 *  a tool_result is up to TOOL_RESULT_MAX. So the budget is in TOKENS, and
 *  the bytes-per-token ratio is MEASURED rather than guessed - every
 *  response tells us how many input tokens the provider counted for a
 *  request whose size we know (see history_calibrate).
 *
 *  Two rules shape the rest:
 *
 *  Check often, change rarely. Prompt caching reuses only an identical
 *  prefix, and a cache hit costs roughly a twentieth of a fresh read, so
 *  rewriting the middle of the history every iteration can cost more than
 *  the bytes it saves. Hence the two water marks: compaction triggers at
 *  HIGH and cuts all the way down to LOW, so a long run compacts a handful
 *  of times instead of once per turn.
 *
 *  Never remove anything silently. Whatever goes is replaced by a marker
 *  saying what it was, and appended to <agent_dir>/_history_archive.jsonl.
 *  A history with invisible holes is worse than a short one: absence reads
 *  to the model as "never happened", so it repeats work it already did.
 * ------------------------------------------------------------------ */

/* The default budget, in tokens. Well under any current model's window on
   purpose: reasoning across a nearly full context is measurably worse than
   across a fifth of one, so the limit that matters is a quality limit, not
   the API's. Override with PIXELGO_HISTORY_BUDGET (a token count, or a
   KB/MB size with the same spelling the other limits accept). */
#define HISTORY_BUDGET_TOKENS_DEFAULT 120000

/* Compact when the history passes HIGH, and cut until it is under LOW.
   The gap between them is the whole point - see "check often, change
   rarely" above. */
#define HISTORY_HIGH_WATER_PCT 70
#define HISTORY_LOW_WATER_PCT  40

/* Used until the first response has been seen. Deliberately pessimistic:
   JSON with its quotes, braces and escapes tokenizes at roughly 2.3 bytes
   per token, and under-estimating the ratio over-estimates the token count,
   which compacts a little early rather than a little late. */
#define HISTORY_BYTES_PER_TOKEN_DEFAULT 2.0

/* Teaches the estimator from a real response: `request_bytes` is the size of
   the serialized request that was sent, `input_tokens` what the provider
   said it counted. Ignored when either is implausible. In-process only -
   each agent run learns from its own first call, which is also the smallest
   one, so starting from the default costs little. */
void history_calibrate(size_t request_bytes, long input_tokens);

/* The ratio in force right now: measured if we have seen a response,
   HISTORY_BYTES_PER_TOKEN_DEFAULT otherwise. */
double history_bytes_per_token(void);

/* Estimated tokens for this conversation, from its serialized size. */
long history_estimate_tokens(const cJSON *messages);

/* The effective budget in tokens, honouring PIXELGO_HISTORY_BUDGET. */
long history_budget_tokens(void);

/*
 * Brings `messages` under the low-water mark if it is over the high one.
 *
 * Spends the budget on tool results rather than on turns, because they are
 * 80-95% of the bytes, in four steps from free and lossless to lossy:
 *
 *   1. supersession - the same tool called twice with the same arguments:
 *      keep the newest result only. The largest single win, costs nothing,
 *      and it is the only step that also improves CORRECTNESS: an old
 *      read_file of a file the agent has since written is not redundant,
 *      it is wrong, and feeding the model both is worse than neither.
 *   2. truncation - keep the head and the tail of an oversized result. Not
 *      arbitrary: for command output the error is at the end and the header
 *      at the start; the middle is progress.
 *   3. age - a result from thirty iterations ago gets a smaller cap than
 *      one from two.
 *   4. dropping whole turns at a safe boundary, as the last resort rather
 *      than the only tool - and never a message the human wrote.
 *
 * `aggressive` cuts to a far lower target; used when a provider has already
 * rejected the request for length, where the estimate was evidently wrong.
 *
 * User messages are never removed or truncated, in any step.
 *
 * Returns 1 if it changed anything.
 */
int history_compact(const agent_t *agent, cJSON *messages, int aggressive);


#endif
