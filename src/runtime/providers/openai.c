#include "provider_internal.h"
#include "json_util.h"
#include "usage.h"
#include "log.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * OpenAI adapter (Chat Completions API).
 *
 * Differences from the neutral (Anthropic-style) format:
 *   - system prompt = a message with role:"system" at the start of the list (not a
 *     separate field)
 *   - an assistant message with a tool call has "tool_calls":[{id,type:"function",
 *     function:{name, arguments}}] where arguments is a JSON STRING, not an object
 *   - a tool's result = a message with role:"tool", tool_call_id, content
 *   - tool definitions: [{type:"function", function:{name,description,parameters}}]
 *   - response: choices[0].message; if it has tool_calls -> finish_reason "tool_calls"
 *
 * We translate neutral messages -> OpenAI messages, neutral tools -> OpenAI tools,
 * then normalize the response back into neutral blocks (text + tool_use).
 */

/* Translates an array of neutral content-blocks into one/several OpenAI messages.
   One neutral message can become: an assistant message with text+tool_calls, OR
   several role:"tool" messages (one per tool_result). */
static void translate_message_to_openai(const cJSON *msg, cJSON *openai_messages) {
    const cJSON *role = cJSON_GetObjectItemCaseSensitive(msg, "role");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    const char *role_str = cJSON_IsString(role) ? role->valuestring : "user";

    if (!cJSON_IsArray(content)) return;

    /* collect: concatenated text, tool_use blocks, tool_result blocks */
    char text[8192]; text[0] = 0; size_t toff = 0;
    cJSON *tool_calls = cJSON_CreateArray();
    cJSON *tool_results = cJSON_CreateArray(); /* separate role:"tool" messages */

    const cJSON *block = NULL;
    cJSON_ArrayForEach(block, content) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
        if (!cJSON_IsString(type)) continue;

        if (strcmp(type->valuestring, "text") == 0) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(block, "text");
            if (cJSON_IsString(t) && t->valuestring) {
                int n = snprintf(text + toff, sizeof(text) - toff, "%s", t->valuestring);
                if (n > 0) toff += (size_t)n;
            }
        } else if (strcmp(type->valuestring, "tool_use") == 0) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(block, "id");
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(block, "name");
            const cJSON *input = cJSON_GetObjectItemCaseSensitive(block, "input");

            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id", cJSON_IsString(id) ? id->valuestring : "");
            cJSON_AddStringToObject(tc, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", cJSON_IsString(name) ? name->valuestring : "");
            /* arguments = the JSON STRING of the input object */
            char args[8192];
            if (input && json_serialize(input, args, sizeof(args)))
                cJSON_AddStringToObject(fn, "arguments", args);
            else
                cJSON_AddStringToObject(fn, "arguments", "{}");
            cJSON_AddItemToObject(tc, "function", fn);
            cJSON_AddItemToArray(tool_calls, tc);
        } else if (strcmp(type->valuestring, "tool_result") == 0) {
            const cJSON *tuid = cJSON_GetObjectItemCaseSensitive(block, "tool_use_id");
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(block, "content");
            cJSON *tmsg = cJSON_CreateObject();
            cJSON_AddStringToObject(tmsg, "role", "tool");
            cJSON_AddStringToObject(tmsg, "tool_call_id", cJSON_IsString(tuid) ? tuid->valuestring : "");
            cJSON_AddStringToObject(tmsg, "content", cJSON_IsString(c) ? c->valuestring : "");
            cJSON_AddItemToArray(tool_results, tmsg);
        }
    }

    int has_text = toff > 0;
    int has_calls = cJSON_GetArraySize(tool_calls) > 0;
    int has_results = cJSON_GetArraySize(tool_results) > 0;

    if (has_text || has_calls) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", role_str);
        if (has_text) cJSON_AddStringToObject(m, "content", text);
        else cJSON_AddNullToObject(m, "content");
        if (has_calls) cJSON_AddItemToObject(m, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        cJSON_AddItemToArray(openai_messages, m);
    }
    /* the role:"tool" messages go separately, after the assistant message */
    if (has_results) {
        cJSON *tr = NULL;
        cJSON_ArrayForEach(tr, tool_results)
            cJSON_AddItemToArray(openai_messages, cJSON_Duplicate(tr, 1));
    }

    cJSON_Delete(tool_calls);
    cJSON_Delete(tool_results);
}

/* Translates neutral tools -> OpenAI tools. */
static cJSON *translate_tools_to_openai(const cJSON *tools) {
    cJSON *arr = cJSON_CreateArray();
    const cJSON *t = NULL;
    cJSON_ArrayForEach(t, tools) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(t, "name");
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(t, "description");
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(t, "input_schema");

        cJSON *ot = cJSON_CreateObject();
        cJSON_AddStringToObject(ot, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", cJSON_IsString(name) ? name->valuestring : "");
        if (cJSON_IsString(desc))
            cJSON_AddStringToObject(fn, "description", desc->valuestring);
        if (schema)
            cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(schema, 1));
        cJSON_AddItemToObject(ot, "function", fn);
        cJSON_AddItemToArray(arr, ot);
    }
    return arr;
}

/* Normalizes the OpenAI response (choices[0].message) back into neutral blocks. */
static int normalize_openai_response(const agent_t *agent, const char *respbuf,
                                    provider_response_t *out) {
    cJSON *resp = cJSON_Parse(respbuf);
    if (!resp) {
        snprintf(out->stop_reason, sizeof(out->stop_reason), "error");
        snprintf(out->error, sizeof(out->error), "openai: non-JSON response");
        return -1;
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(resp, "choices");
    cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice0 ? cJSON_GetObjectItemCaseSensitive(choice0, "message") : NULL;
    if (!message) {
        snprintf(out->stop_reason, sizeof(out->stop_reason), "error");
        snprintf(out->error, sizeof(out->error), "openai: response without choices[0].message");
        cJSON_Delete(resp);
        return -1;
    }

    cJSON *neutral = cJSON_CreateArray();

    /* text */
    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (cJSON_IsString(content) && content->valuestring && content->valuestring[0]) {
        cJSON *tb = cJSON_CreateObject();
        cJSON_AddStringToObject(tb, "type", "text");
        cJSON_AddStringToObject(tb, "text", content->valuestring);
        cJSON_AddItemToArray(neutral, tb);
    }

    /* tool_calls -> neutral tool_use (arguments string -> input object) */
    int has_tool = 0;
    cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    if (cJSON_IsArray(tool_calls)) {
        cJSON *tc = NULL;
        cJSON_ArrayForEach(tc, tool_calls) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(tc, "id");
            const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
            const cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
            const cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;

            cJSON *tub = cJSON_CreateObject();
            cJSON_AddStringToObject(tub, "type", "tool_use");
            cJSON_AddStringToObject(tub, "id", cJSON_IsString(id) ? id->valuestring : "");
            cJSON_AddStringToObject(tub, "name", cJSON_IsString(name) ? name->valuestring : "");
            /* arguments is a JSON string -> we parse it into an object for the neutral format */
            cJSON *input = NULL;
            if (cJSON_IsString(args) && args->valuestring)
                input = cJSON_Parse(args->valuestring);
            cJSON_AddItemToObject(tub, "input", input ? input : cJSON_CreateObject());
            cJSON_AddItemToArray(neutral, tub);
            has_tool = 1;
        }
    }

    out->content = neutral;
    snprintf(out->stop_reason, sizeof(out->stop_reason), has_tool ? "tool_use" : "end_turn");

    /* Usage: OpenAI -> usage.prompt_tokens / usage.completion_tokens */
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(resp, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON *in = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
        cJSON *o  = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
        out->usage.input_tokens  = cJSON_IsNumber(in) ? (long)in->valuedouble : 0;
        out->usage.output_tokens = cJSON_IsNumber(o)  ? (long)o->valuedouble  : 0;
        out->usage.cost_micro_usd = usage_cost_micro(
            LLM_PROVIDER_OPENAI, agent->cfg.ai.model,
            out->usage.input_tokens, out->usage.output_tokens);
    }

    out->ok = 1;
    cJSON_Delete(resp);
    return 0;
}

int openai_call(const agent_t *agent, const char *api_key,
                const cJSON *messages, const cJSON *tools,
                provider_response_t *out) {
    memset(out, 0, sizeof(*out));

    /* --- we build the OpenAI messages (system + the translation of the neutral ones) --- */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", agent->cfg.ai.model);

    cJSON *omsgs = cJSON_CreateArray();
    if (agent->cfg.ai.system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", agent->cfg.ai.system_prompt);
        cJSON_AddItemToArray(omsgs, sys);
    }
    const cJSON *m = NULL;
    cJSON_ArrayForEach(m, messages)
        translate_message_to_openai(m, omsgs);
    cJSON_AddItemToObject(root, "messages", omsgs);

    if (tools && cJSON_GetArraySize(tools) > 0)
        cJSON_AddItemToObject(root, "tools", translate_tools_to_openai(tools));

    static char body[PROVIDER_RESPONSE_MAX];
    if (!json_serialize(root, body, sizeof(body))) {
        cJSON_Delete(root);
        out->ok = 0;
        snprintf(out->stop_reason, sizeof(out->stop_reason), "error");
        snprintf(out->error, sizeof(out->error), "openai: failed to serialize body");
        return -1;
    }
    cJSON_Delete(root);

    /* --- headers --- */
    struct curl_slist *headers = NULL;
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "content-type: application/json");

    static char respbuf[PROVIDER_RESPONSE_MAX];
    http_request_t req = {
        .url = "https://api.openai.com/v1/chat/completions",
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
        snprintf(out->error, sizeof(out->error), "openai: network: %s", hr.curl_error);
        return -1;
    }
    if (hr.http_code != 200) {
        out->ok = 0;
        snprintf(out->stop_reason, sizeof(out->stop_reason),
                 provider_should_retry(hr.http_code, hr.curl_ok) ? "retry" : "error");
        snprintf(out->error, sizeof(out->error), "openai: HTTP %ld: %.200s",
                 hr.http_code, respbuf);
        return -1;
    }

    return normalize_openai_response(agent, respbuf, out);
}
