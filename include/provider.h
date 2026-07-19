#ifndef PROVIDER_H
#define PROVIDER_H

#include "agent.h"
#include "cJSON.h"
#include "usage.h"

/*
 * Abstraction layer over the LLM providers (Anthropic, OpenAI, Gemini).
 *
 * ai_loop.c builds a conversation in an internal NEUTRAL format (messages +
 * tools, as cJSON nodes) and sends it through provider_call(). Each provider
 * adapter translates the neutral format to/from its specific API:
 *
 *   - different endpoint         (api.anthropic.com vs api.openai.com vs Google)
 *   - different authentication   (x-api-key vs Bearer vs ?key=)
 *   - different request format   (separate system vs system message vs systemInstruction)
 *   - different tool_call format in the response (content[] vs tool_calls[] vs functionCall)
 *
 * ai_loop does NOT know which provider is underneath - it speaks only the neutral
 * format, defined below.
 *
 * NEUTRAL FORMAT (identical to what we already used for Anthropic, chosen as the
 * "lingua franca"):
 *
 *   messages: an array of messages, each { role, content[] } where content[] is an
 *     array of blocks, each of type:
 *       { type:"text", text:"..." }
 *       { type:"tool_use", id:"...", name:"...", input:{...} }
 *       { type:"tool_result", tool_use_id:"...", content:"...", is_error?:bool }
 *
 *   tools: an array of { name, description, input_schema (JSON Schema) }
 *
 *   neutral response (provider_response_t): normalized by each adapter to:
 *       stop_reason: "tool_use" | "end_turn" | "error"
 *       content[]  : the same format as above (text + tool_use blocks)
 */

#define PROVIDER_RESPONSE_MAX 262144

/* Translates the textual provider name ("anthropic"/"openai"/"gemini") into the enum. */
llm_provider_id_t provider_from_string(const char *s);

/* The textual name of a provider (for config/log). */
const char *provider_to_string(llm_provider_id_t id);

/* The name of the environment variable holding the API key for the given provider. */
const char *provider_env_key(llm_provider_id_t id);

/* The result of a call, already NORMALIZED to the neutral format.
   content is a cJSON array owned by the caller (must be cJSON_Delete()d). */
typedef struct {
    int ok;                       /* 1 = success, 0 = error                    */
    char stop_reason[32];         /* "tool_use" | "end_turn" | "error"         */
    cJSON *content;               /* array of neutral blocks (text/tool_use)   */
    char error[512];              /* error message if ok==0                    */
    usage_t usage;                /* tokens consumed + cost (normalized)       */
} provider_response_t;

/*
 * Performs a call to the agent's provider.
 *   agent         - for provider, model, system_prompt (config)
 *   messages      - cJSON array of messages in the neutral format
 *   tools         - cJSON array of tool definitions in the neutral format ("[]" ok)
 *   out           - the normalized response (content must be freed by the caller)
 * Returns 0 on success (out->ok==1), -1 on error (out->ok==0, out->error set).
 */
int provider_call(const agent_t *agent, const cJSON *messages, const cJSON *tools,
                  provider_response_t *out);

/* Frees the resources owned by a provider_response_t (content). */
void provider_response_free(provider_response_t *resp);

#endif
