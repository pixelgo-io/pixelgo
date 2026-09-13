#ifndef PROVIDER_INTERNAL_H
#define PROVIDER_INTERNAL_H

#include "provider.h"
#include "cJSON.h"

/*
 * The internal interface between the provider dispatcher (provider.c) and the
 * concrete adapters (anthropic.c, openai.c, gemini.c). Not exposed to the rest
 * of the project.
 */

/* A generic HTTP request (POST JSON). The adapters build the provider-specific
   headers and body, then call http_post_json. */
typedef struct {
    const char *url;
    struct curl_slist *headers;   /* owned by the caller; freed by the caller  */
    const char *body;             /* the serialized JSON body                  */
    long timeout_sec;
} http_request_t;

typedef struct {
    long http_code;
    int  curl_ok;                 /* 1 if the transport succeeded (regardless of status) */
    char *data;                   /* buffer allocated by the caller (fixed size) */
    size_t size;                  /* bytes received                            */
    char curl_error[256];
} http_response_t;

/* Performs a JSON POST. data must be pre-allocated (PROVIDER_RESPONSE_MAX).
   Returns 0 if the transport succeeded (check http_code separately), -1 otherwise. */
int http_post_json(const http_request_t *req, http_response_t *resp);

/*
 * Each adapter implements this signature. It receives the agent's config and the
 * conversation in the NEUTRAL format, and returns a NORMALIZED response.
 * api_key has already been fetched from the environment by the dispatcher.
 */
typedef int (*provider_adapter_fn)(const agent_t *agent, const char *api_key,
                                   const cJSON *messages, const cJSON *tools,
                                   provider_response_t *out);

/* The concrete adapters. */
int anthropic_call(const agent_t *agent, const char *api_key,
                   const cJSON *messages, const cJSON *tools,
                   provider_response_t *out);
int openai_call(const agent_t *agent, const char *api_key,
                const cJSON *messages, const cJSON *tools,
                provider_response_t *out);
int gemini_call(const agent_t *agent, const char *api_key,
                const cJSON *messages, const cJSON *tools,
                provider_response_t *out);

/* Common helper: retry with exponential backoff around an adapter call.
   The adapters do NOT implement retry themselves; they get it from the dispatcher. */
int provider_should_retry(long http_code, int curl_ok);

/*
 * Resolves a stored cache_mode_t against PIXELGO_CACHE_MODE, exactly like the
 * PIXELGO_DATA_THRESHOLD / --data-threshold global default: CACHE_MODE_UNSET
 * means "no --cache flag was given for this agent", so it inherits the
 * environment variable if set, falling back to CACHE_MODE_AUTO if the
 * variable is unset or unrecognized. Any other stored value (AUTO/ON/OFF,
 * from an explicit --cache) is returned unchanged - an explicit per-agent
 * choice always wins over the global default, never the other way round.
 */
cache_mode_t provider_resolve_cache_mode(cache_mode_t stored);

/*
 * Decides whether THIS request should be marked cacheable, given an ALREADY
 * RESOLVED cache_mode_t (see provider_resolve_cache_mode above - callers must
 * resolve CACHE_MODE_UNSET before calling this) and two signals available
 * before any call is made:
 *
 *   tool_count      - how many tools the agent is configured with. An agent
 *                     with tools very rarely stops at one request (the first
 *                     reply is usually a tool_use, not the final answer), so
 *                     the prefix it just sent is likely to be resent (and
 *                     therefore worth caching) on the very next turn.
 *   message_count   - the size of the `messages` array about to be sent,
 *                     BEFORE this call. If it is already more than 1, this is
 *                     either the Nth turn of a tool loop, or an `agent chat`
 *                     continuing from persisted history - either way, a
 *                     cacheable prefix already exists from earlier calls.
 *
 * A tool-less agent's very first, one-shot request (tool_count == 0 AND
 * message_count <= 1) is the one case pixelgo can be SURE will never see a
 * second request: nothing will ever read back what caching it would write, so
 * CACHE_MODE_AUTO deliberately does not cache it - marking it would only pay
 * the write surcharge for a read that can't happen.
 *
 * Pure function (no I/O), so it is unit-tested directly without a network call.
 */
int provider_cache_decision(cache_mode_t mode, int tool_count, int message_count);

#endif
