#include "provider_internal.h"
#include "json_util.h"
#include "usage.h"
#include "log.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Google Gemini adapter (generateContent API).
 *
 * The most different format of the three:
 *   - "contents" instead of "messages"; each has "parts":[...] instead of content[]
 *   - the roles: "user" and "model" (NOT "assistant"); there is no "system" role -
 *     the system prompt goes into the separate "systemInstruction" field
 *   - tool call: a part with {functionCall:{name, args}} - args is an OBJECT (like ours)
 *   - tool result: a part with {functionResponse:{name, response:{...}}} - it is
 *     identified by NAME, not by id (Gemini has no tool_use_id!)
 *   - tool definitions: "tools":[{functionDeclarations:[{name,description,parameters}]}]
 *   - the API key goes in the URL (?key=...), not in a header
 *
 * The id problem: our neutral format uses tool_use_id (like Anthropic and
 * OpenAI). Gemini links the result to the tool by NAME. Solution: when translating
 * to Gemini, we map tool_use_id -> name (we keep a small table from the previous
 * messages); when normalizing the response, we generate a synthetic id from the
 * name, so ai_loop works uniformly.
 */

#define MAX_TOOLCALL_MAP 32

typedef struct {
    char id[MAX_STR];
    char name[MAX_STR];
} toolcall_map_t;

/* Looks up the tool's name by id, in the table built from the previous messages. */
static const char *lookup_tool_name(const toolcall_map_t *map, int count, const char *id) {
    for (int i = 0; i < count; i++)
        if (strcmp(map[i].id, id) == 0) return map[i].name;
    return "";
}

/* Translates the neutral messages -> Gemini "contents". Builds the id->name table
   along the way, so it can resolve the tool_results (which in Gemini need names). */
static cJSON *translate_messages_to_gemini(const cJSON *messages) {
    cJSON *contents = cJSON_CreateArray();

    toolcall_map_t map[MAX_TOOLCALL_MAP];
    int map_count = 0;

    const cJSON *msg = NULL;
    cJSON_ArrayForEach(msg, messages) {
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(msg, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (!cJSON_IsArray(content)) continue;

        const char *role_str = cJSON_IsString(role) ? role->valuestring : "user";
        /* Gemini: "assistant" -> "model" */
        const char *grole = (strcmp(role_str, "assistant") == 0) ? "model" : "user";

        cJSON *parts = cJSON_CreateArray();

        const cJSON *block = NULL;
        cJSON_ArrayForEach(block, content) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
            if (!cJSON_IsString(type)) continue;

            if (strcmp(type->valuestring, "text") == 0) {
                const cJSON *t = cJSON_GetObjectItemCaseSensitive(block, "text");
                if (cJSON_IsString(t) && t->valuestring && t->valuestring[0]) {
                    cJSON *p = cJSON_CreateObject();
                    cJSON_AddStringToObject(p, "text", t->valuestring);
                    cJSON_AddItemToArray(parts, p);
                }
            } else if (strcmp(type->valuestring, "tool_use") == 0) {
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(block, "id");
                const cJSON *name = cJSON_GetObjectItemCaseSensitive(block, "name");
                const cJSON *input = cJSON_GetObjectItemCaseSensitive(block, "input");
                const char *nm = cJSON_IsString(name) ? name->valuestring : "";

                /* we memorize id->name for the tool_results that will follow */
                if (map_count < MAX_TOOLCALL_MAP && cJSON_IsString(id)) {
                    snprintf(map[map_count].id, MAX_STR, "%s", id->valuestring);
                    snprintf(map[map_count].name, MAX_STR, "%s", nm);
                    map_count++;
                }

                cJSON *p = cJSON_CreateObject();
                cJSON *fc = cJSON_CreateObject();
                cJSON_AddStringToObject(fc, "name", nm);
                cJSON_AddItemToObject(fc, "args",
                    input ? cJSON_Duplicate(input, 1) : cJSON_CreateObject());
                cJSON_AddItemToObject(p, "functionCall", fc);
                cJSON_AddItemToArray(parts, p);
            } else if (strcmp(type->valuestring, "tool_result") == 0) {
                const cJSON *tuid = cJSON_GetObjectItemCaseSensitive(block, "tool_use_id");
                const cJSON *c = cJSON_GetObjectItemCaseSensitive(block, "content");
                const char *nm = cJSON_IsString(tuid)
                    ? lookup_tool_name(map, map_count, tuid->valuestring) : "";

                cJSON *p = cJSON_CreateObject();
                cJSON *fr = cJSON_CreateObject();
                cJSON_AddStringToObject(fr, "name", nm);
                /* response must be an object; we wrap the text */
                cJSON *rwrap = cJSON_CreateObject();
                cJSON_AddStringToObject(rwrap, "result",
                    cJSON_IsString(c) ? c->valuestring : "");
                cJSON_AddItemToObject(fr, "response", rwrap);
                cJSON_AddItemToObject(p, "functionResponse", fr);
                cJSON_AddItemToArray(parts, p);
            }
        }

        if (cJSON_GetArraySize(parts) > 0) {
            cJSON *ct = cJSON_CreateObject();
            cJSON_AddStringToObject(ct, "role", grole);
            cJSON_AddItemToObject(ct, "parts", parts);
            cJSON_AddItemToArray(contents, ct);
        } else {
            cJSON_Delete(parts);
        }
    }
    return contents;
}

/* Translates neutral tools -> the Gemini format (functionDeclarations). */
static cJSON *translate_tools_to_gemini(const cJSON *tools) {
    cJSON *decls = cJSON_CreateArray();
    const cJSON *t = NULL;
    cJSON_ArrayForEach(t, tools) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(t, "name");
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(t, "description");
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(t, "input_schema");

        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "name", cJSON_IsString(name) ? name->valuestring : "");
        if (cJSON_IsString(desc))
            cJSON_AddStringToObject(d, "description", desc->valuestring);
        if (schema)
            cJSON_AddItemToObject(d, "parameters", cJSON_Duplicate(schema, 1));
        cJSON_AddItemToArray(decls, d);
    }

    /* "tools": [ { "functionDeclarations": [...] } ] */
    cJSON *wrapper = cJSON_CreateObject();
    cJSON_AddItemToObject(wrapper, "functionDeclarations", decls);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, wrapper);
    return arr;
}

/* Normalizes the Gemini response -> neutral blocks (text + tool_use). */
static int normalize_gemini_response(const agent_t *agent, const char *respbuf,
                                    provider_response_t *out) {
    cJSON *resp = cJSON_Parse(respbuf);
    if (!resp) {
        snprintf(out->stop_reason, sizeof(out->stop_reason), "error");
        snprintf(out->error, sizeof(out->error), "gemini: non-JSON response");
        return -1;
    }

    cJSON *cands = cJSON_GetObjectItemCaseSensitive(resp, "candidates");
    cJSON *c0 = cJSON_IsArray(cands) ? cJSON_GetArrayItem(cands, 0) : NULL;
    cJSON *cont = c0 ? cJSON_GetObjectItemCaseSensitive(c0, "content") : NULL;
    cJSON *parts = cont ? cJSON_GetObjectItemCaseSensitive(cont, "parts") : NULL;

    if (!cJSON_IsArray(parts)) {
        snprintf(out->stop_reason, sizeof(out->stop_reason), "error");
        snprintf(out->error, sizeof(out->error),
                 "gemini: response without candidates[0].content.parts");
        cJSON_Delete(resp);
        return -1;
    }

    cJSON *neutral = cJSON_CreateArray();
    int has_tool = 0;
    int synth_id = 0;

    cJSON *p = NULL;
    cJSON_ArrayForEach(p, parts) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(p, "text");
        cJSON *fc   = cJSON_GetObjectItemCaseSensitive(p, "functionCall");

        if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
            cJSON *tb = cJSON_CreateObject();
            cJSON_AddStringToObject(tb, "type", "text");
            cJSON_AddStringToObject(tb, "text", text->valuestring);
            cJSON_AddItemToArray(neutral, tb);
        }

        if (cJSON_IsObject(fc)) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(fc, "name");
            const cJSON *args = cJSON_GetObjectItemCaseSensitive(fc, "args");
            const char *nm = cJSON_IsString(name) ? name->valuestring : "";

            /* Gemini gives no tool call id -> we generate a synthetic one, stable
               within the turn, so ai_loop can link the tool_result to it. */
            char id[MAX_STR];
            snprintf(id, sizeof(id), "gm_%s_%d", nm, synth_id++);

            cJSON *tub = cJSON_CreateObject();
            cJSON_AddStringToObject(tub, "type", "tool_use");
            cJSON_AddStringToObject(tub, "id", id);
            cJSON_AddStringToObject(tub, "name", nm);
            cJSON_AddItemToObject(tub, "input",
                args ? cJSON_Duplicate(args, 1) : cJSON_CreateObject());
            cJSON_AddItemToArray(neutral, tub);
            has_tool = 1;
        }
    }

    out->content = neutral;
    snprintf(out->stop_reason, sizeof(out->stop_reason), has_tool ? "tool_use" : "end_turn");

    /* Usage: Gemini -> usageMetadata.promptTokenCount / candidatesTokenCount */
    cJSON *um = cJSON_GetObjectItemCaseSensitive(resp, "usageMetadata");
    if (cJSON_IsObject(um)) {
        cJSON *in = cJSON_GetObjectItemCaseSensitive(um, "promptTokenCount");
        cJSON *o  = cJSON_GetObjectItemCaseSensitive(um, "candidatesTokenCount");
        out->usage.input_tokens  = cJSON_IsNumber(in) ? (long)in->valuedouble : 0;
        out->usage.output_tokens = cJSON_IsNumber(o)  ? (long)o->valuedouble  : 0;
        out->usage.cost_micro_usd = usage_cost_micro(
            LLM_PROVIDER_GEMINI, agent->cfg.ai.model,
            out->usage.input_tokens, out->usage.output_tokens);
    }

    out->ok = 1;
    cJSON_Delete(resp);
    return 0;
}

int gemini_call(const agent_t *agent, const char *api_key,
                const cJSON *messages, const cJSON *tools,
                provider_response_t *out) {
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_CreateObject();

    /* system prompt -> systemInstruction (a separate field, with parts) */
    if (agent->cfg.ai.system_prompt[0]) {
        cJSON *si = cJSON_CreateObject();
        cJSON *sparts = cJSON_CreateArray();
        cJSON *sp = cJSON_CreateObject();
        cJSON_AddStringToObject(sp, "text", agent->cfg.ai.system_prompt);
        cJSON_AddItemToArray(sparts, sp);
        cJSON_AddItemToObject(si, "parts", sparts);
        cJSON_AddItemToObject(root, "systemInstruction", si);
    }

    cJSON_AddItemToObject(root, "contents", translate_messages_to_gemini(messages));

    if (tools && cJSON_GetArraySize(tools) > 0)
        cJSON_AddItemToObject(root, "tools", translate_tools_to_gemini(tools));

    static char body[PROVIDER_RESPONSE_MAX];
    if (!json_serialize(root, body, sizeof(body))) {
        cJSON_Delete(root);
        out->ok = 0;
        snprintf(out->stop_reason, sizeof(out->stop_reason), "error");
        snprintf(out->error, sizeof(out->error), "gemini: failed to serialize body");
        return -1;
    }
    cJSON_Delete(root);

    /* The URL contains the model AND the API key (Gemini uses no auth header) */
    char url[1024];
    snprintf(url, sizeof(url),
             "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
             agent->cfg.ai.model, api_key);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "content-type: application/json");

    static char respbuf[PROVIDER_RESPONSE_MAX];
    http_request_t req = {
        .url = url,
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
        snprintf(out->error, sizeof(out->error), "gemini: network: %s", hr.curl_error);
        return -1;
    }
    if (hr.http_code != 200) {
        out->ok = 0;
        snprintf(out->stop_reason, sizeof(out->stop_reason),
                 provider_should_retry(hr.http_code, hr.curl_ok) ? "retry" : "error");
        snprintf(out->error, sizeof(out->error), "gemini: HTTP %ld: %.200s",
                 hr.http_code, respbuf);
        return -1;
    }

    return normalize_gemini_response(agent, respbuf, out);
}
