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

#endif
