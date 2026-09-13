#include "provider.h"
#include "provider_internal.h"
#include "json_util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_TIMEOUT_SEC 120L
#define DEFAULT_MAX_RETRIES 3

/* --- the provider table (name + adapter + env key) --- */

typedef struct {
    llm_provider_id_t id;
    const char *name;
    const char *env_key;
    provider_adapter_fn fn;
} provider_entry_t;

static const provider_entry_t PROVIDERS[] = {
    { LLM_PROVIDER_ANTHROPIC, "anthropic", "ANTHROPIC_API_KEY", anthropic_call },
    { LLM_PROVIDER_OPENAI,    "openai",    "OPENAI_API_KEY",    openai_call    },
    { LLM_PROVIDER_GEMINI,    "gemini",    "GEMINI_API_KEY",    gemini_call    },
};
static const int PROVIDER_COUNT = (int)(sizeof(PROVIDERS) / sizeof(PROVIDERS[0]));

static const provider_entry_t *find_provider(llm_provider_id_t id) {
    for (int i = 0; i < PROVIDER_COUNT; i++)
        if (PROVIDERS[i].id == id) return &PROVIDERS[i];
    return NULL;
}

llm_provider_id_t provider_from_string(const char *s) {
    if (!s) return LLM_PROVIDER_UNKNOWN;
    for (int i = 0; i < PROVIDER_COUNT; i++)
        if (strcmp(PROVIDERS[i].name, s) == 0) return PROVIDERS[i].id;
    return LLM_PROVIDER_UNKNOWN;
}

const char *provider_to_string(llm_provider_id_t id) {
    const provider_entry_t *e = find_provider(id);
    return e ? e->name : "unknown";
}

const char *provider_env_key(llm_provider_id_t id) {
    const provider_entry_t *e = find_provider(id);
    return e ? e->env_key : NULL;
}

int provider_should_retry(long http_code, int curl_ok) {
    if (!curl_ok) return 1;                       /* network error -> retry    */
    if (http_code == 429 || http_code >= 500) return 1; /* rate limit / 5xx    */
    return 0;
}

cache_mode_t provider_resolve_cache_mode(cache_mode_t stored) {
    if (stored != CACHE_MODE_UNSET) return stored;   /* explicit --cache always wins */

    const char *env = getenv("PIXELGO_CACHE_MODE");
    if (env) {
        if (strcmp(env, "on")   == 0) return CACHE_MODE_ON;
        if (strcmp(env, "off")  == 0) return CACHE_MODE_OFF;
        if (strcmp(env, "auto") == 0) return CACHE_MODE_AUTO;
        LOG_W("provider: unrecognized PIXELGO_CACHE_MODE '%s' (expected "
              "\"auto\", \"on\", or \"off\"), falling back to \"auto\"", env);
    }
    return CACHE_MODE_AUTO;   /* no flag, no env var (or an unrecognized one) */
}

int provider_cache_decision(cache_mode_t mode, int tool_count, int message_count) {
    if (mode == CACHE_MODE_ON)  return 1;
    if (mode == CACHE_MODE_OFF) return 0;
    /* CACHE_MODE_AUTO (and, defensively, a stray CACHE_MODE_UNSET that
       reached here unresolved): cache unless we can already be sure no one
       will ever read back what we would write - see provider_internal.h. */
    return (tool_count > 0) || (message_count > 1);
}

/* --- public dispatcher with retry --- */

int provider_call(const agent_t *agent, const cJSON *messages, const cJSON *tools,
                  provider_response_t *out) {
    memset(out, 0, sizeof(*out));

    const provider_entry_t *entry = find_provider(agent->cfg.ai.provider);
    if (!entry) {
        out->ok = 0;
        snprintf(out->error, sizeof(out->error), "unknown provider (id=%d)",
                 (int)agent->cfg.ai.provider);
        LOG_E("provider: %s", out->error);
        return -1;
    }

    const char *api_key = getenv(entry->env_key);
    if (!api_key || !api_key[0]) {
        out->ok = 0;
        snprintf(out->error, sizeof(out->error), "%s is not set in the environment",
                 entry->env_key);
        LOG_E("provider: %s", out->error);
        return -1;
    }

    int max_retries = DEFAULT_MAX_RETRIES;
    for (int attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            int delay = 1 << (attempt - 1);
            if (delay > 8) delay = 8;
            LOG_W("provider[%s]: retry %d/%d after %ds",
                  entry->name, attempt, max_retries, delay);
            struct timespec ts = { delay, 0 };
            nanosleep(&ts, NULL);
        }

        /* the adapter fills out; it signals retryability through out->error +
           a stop_reason "error" plus the temporarily stored HTTP code. For
           simplicity, the adapters return 0 on real success, -1 otherwise, and
           set out->ok. The retry happens only if the adapter marks it so through
           the special field (see below). */
        int rc = entry->fn(agent, api_key, messages, tools, out);
        if (rc == 0 && out->ok) {
            LOG_D("provider[%s]: call succeeded", entry->name);
            return 0;
        }

        /* The adapter puts "retry" in out->stop_reason if the error is transient. */
        if (strcmp(out->stop_reason, "retry") != 0) {
            LOG_E("provider[%s]: non-retryable error: %.200s", entry->name, out->error);
            provider_response_free(out);
            out->ok = 0;
            return -1;
        }
        provider_response_free(out); /* we free the partial content before retrying */
    }

    LOG_E("provider[%s]: all retries failed", entry->name);
    out->ok = 0;
    snprintf(out->error, sizeof(out->error), "all retries failed");
    return -1;
}

void provider_response_free(provider_response_t *resp) {
    if (resp && resp->content) {
        cJSON_Delete(resp->content);
        resp->content = NULL;
    }
}
