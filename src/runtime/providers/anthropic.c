#include "provider_internal.h"
#include "json_util.h"
#include "log.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Anthropic adapter (Messages API).
 *
 * The project's neutral format was chosen to be identical to Anthropic's, so the
 * translation is almost 1:1:
 *   - system_prompt -> a separate "system" field
 *   - neutral messages -> exactly "messages" (content[] with text/tool_use/tool_result)
 *   - neutral tools -> exactly "tools" (name/description/input_schema)
 * The response already has content[] with text/tool_use blocks and stop_reason ->
 * we copy them straight into the neutral format.
 */

int anthropic_call(const agent_t *agent, const char *api_key,
                   const cJSON *messages, const cJSON *tools,
                   provider_response_t *out) {
    memset(out, 0, sizeof(*out));

    /* --- we build the request body --- */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", agent->cfg.ai.model);
    cJSON_AddNumberToObject(root, "max_tokens", 4096);
    if (agent->cfg.ai.system_prompt[0])
        cJSON_AddStringToObject(root, "system", agent->cfg.ai.system_prompt);
    cJSON_AddItemToObject(root, "messages", cJSON_Duplicate(messages, 1));
    if (tools && cJSON_GetArraySize(tools) > 0)
        cJSON_AddItemToObject(root, "tools", cJSON_Duplicate(tools, 1));

    static char body[PROVIDER_RESPONSE_MAX];
    if (!json_serialize(root, body, sizeof(body))) {
        cJSON_Delete(root);
        out->ok = 0;
        snprintf(out->error, sizeof(out->error), "anthropic: failed to serialize body");
        return -1;
    }
    cJSON_Delete(root);

    /* --- headers --- */
    struct curl_slist *headers = NULL;
    char auth[512];
    snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    headers = curl_slist_append(headers, "content-type: application/json");

    static char respbuf[PROVIDER_RESPONSE_MAX];
    http_request_t req = {
        .url = "https://api.anthropic.com/v1/messages",
        .headers = headers,
        .body = body,
        .timeout_sec = 120
    };
    http_response_t hr = { .data = respbuf };

    int transport = http_post_json(&req, &hr);
    curl_slist_free_all(headers);

    if (transport != 0) {
        out->ok = 0;
        snprintf(out->stop_reason, sizeof(out->stop_reason), "retry");
        snprintf(out->error, sizeof(out->error), "anthropic: network: %s", hr.curl_error);
        return -1;
    }
    if (hr.http_code != 200) {
        out->ok = 0;
        if (provider_should_retry(hr.http_code, hr.curl_ok))
            snprintf(out->stop_reason, sizeof(out->stop_reason), "retry");
        else
            snprintf(out->stop_reason, sizeof(out->stop_reason), "error");
        snprintf(out->error, sizeof(out->error), "anthropic: HTTP %ld: %.200s",
                 hr.http_code, respbuf);
        return -1;
    }

    /* --- we normalize the response (already in the neutral format) --- */
    cJSON *resp = cJSON_Parse(respbuf);
    if (!resp) {
        out->ok = 0;
        snprintf(out->stop_reason, sizeof(out->stop_reason), "error");
        snprintf(out->error, sizeof(out->error), "anthropic: non-JSON response");
        return -1;
    }

    cJSON *content = cJSON_GetObjectItemCaseSensitive(resp, "content");
    cJSON *stop = cJSON_GetObjectItemCaseSensitive(resp, "stop_reason");
    if (!cJSON_IsArray(content)) {
        out->ok = 0;
        snprintf(out->stop_reason, sizeof(out->stop_reason), "error");
        snprintf(out->error, sizeof(out->error), "anthropic: response without content[]");
        cJSON_Delete(resp);
        return -1;
    }

    out->content = cJSON_Duplicate(content, 1);
    /* stop_reason Anthropic: tool_use / end_turn / max_tokens / stop_sequence */
    const char *sr = cJSON_IsString(stop) ? stop->valuestring : "end_turn";
    if (strcmp(sr, "tool_use") == 0)
        snprintf(out->stop_reason, sizeof(out->stop_reason), "tool_use");
    else
        snprintf(out->stop_reason, sizeof(out->stop_reason), "end_turn");

    /* Token usage: Anthropic -> usage.input_tokens / usage.output_tokens */
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(resp, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON *in  = cJSON_GetObjectItemCaseSensitive(usage, "input_tokens");
        cJSON *o   = cJSON_GetObjectItemCaseSensitive(usage, "output_tokens");
        out->usage.input_tokens  = cJSON_IsNumber(in) ? (long)in->valuedouble : 0;
        out->usage.output_tokens = cJSON_IsNumber(o)  ? (long)o->valuedouble  : 0;
        out->usage.cost_micro_usd = usage_cost_micro(
            LLM_PROVIDER_ANTHROPIC, agent->cfg.ai.model,
            out->usage.input_tokens, out->usage.output_tokens);
    }

    out->ok = 1;

    cJSON_Delete(resp);
    return 0;
}
