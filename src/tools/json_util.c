#include "json_util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

cJSON *json_parse(const char *text) {
    if (!text) return NULL;
    return cJSON_Parse(text);
}

int json_serialize(const cJSON *node, char *out, size_t out_size) {
    if (!node || !out || out_size == 0) return 0;
    out[0] = 0;

    /* cJSON_PrintUnformatted allocates dynamically; we then copy into the caller's
       fixed buffer and free. Compact (no whitespace) - we send it to the API. */
    char *printed = cJSON_PrintUnformatted(node);
    if (!printed) return 0;

    size_t len = strlen(printed);
    if (len >= out_size) {
        /* it does not fit - truncation would produce invalid JSON, so we signal failure */
        free(printed);
        return 0;
    }
    memcpy(out, printed, len + 1);
    free(printed);
    return 1;
}

int json_get_string(const cJSON *obj, const char *key, char *out, size_t out_size) {
    if (!obj || !key || !out || out_size == 0) return 0;
    out[0] = 0;

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) return 0;

    snprintf(out, out_size, "%s", item->valuestring);
    return 1;
}

int json_extract_string(const char *json, const char *key, char *out, int out_size) {
    if (!json || !key || !out || out_size <= 0) return 0;

    cJSON *root = cJSON_Parse(json);
    if (!root) return 0;

    int ok = json_get_string(root, key, out, (size_t)out_size);
    cJSON_Delete(root);
    return ok;
}
