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
 * TRUNCATION: a long conversation exceeds the model's context limit and the API
 * errors out. When the history grows past HISTORY_MAX_MESSAGES, we trim the old
 * messages. The trimming is NOT naive - see history_trim().
 */

#define HISTORY_FILE "_history.json"

/* Past this many messages, we trim the oldest. A "message" is a turn (user or
   assistant), not a character - so 40 means ~20 exchanges. */
#define HISTORY_MAX_MESSAGES 40

/* Loads the agent's history from <dir>/_history.json.
   Returns a cJSON array (empty if the file does not exist or is invalid).
   The caller must free it with cJSON_Delete(). Never returns NULL. */
cJSON *history_load(const agent_t *agent);

/* Saves the history to <dir>/_history.json. Returns 1 on success. */
int history_save(const agent_t *agent, const cJSON *messages);

/* Deletes the history (starts a new conversation). Returns 1 on success. */
int history_reset(const agent_t *agent);

/*
 * Trims old messages if the history is too long, PRESERVING VALIDITY.
 *
 * Why we cannot cut just anywhere: the protocol requires an assistant message
 * with tool_use to be followed by the user message with the corresponding
 * tool_results. If we cut between them, the API rejects the request
 * ("tool_use without tool_result").
 *
 * So we cut only at a SAFE boundary: the first user message that does NOT start
 * with tool_result. This way we never break a tool_use/tool_result pair.
 *
 * Modifies messages in place. Returns how many messages were trimmed.
 */
int history_trim(cJSON *messages, int max_messages);

#endif
