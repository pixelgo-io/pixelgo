#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <stddef.h>
#include "cJSON.h"

/*
 * json_util - a thin layer over cJSON, in the project's style.
 *
 * Why a wrapper and not cJSON directly everywhere:
 *   - a single place where we include cJSON (easy to swap the library if we want)
 *   - helpers with safe semantics for the fixed buffers used throughout the code
 *     (snprintf-style: always NUL-terminated, always report whether they succeeded)
 *   - it fully replaces the old jsonmini.c (extraction via strstr), which did not
 *     understand nested objects, escaping, or arrays - exactly what we need for
 *     the real Anthropic API response (content[] with text/tool_use blocks).
 */

/* Parses a JSON string. Returns NULL on a syntax error.
   The caller MUST free the result with cJSON_Delete(). */
cJSON *json_parse(const char *text);

/* Serializes a cJSON object into the given buffer (compact, no spaces).
   Returns 1 on success (it fit), 0 if it was truncated/errored.
   out is always NUL-terminated if out_size > 0. */
int json_serialize(const cJSON *node, char *out, size_t out_size);

/* Gets the value of a string field from an object. Copies into out (NUL-terminated).
   Returns 1 if the key exists and is a string, 0 otherwise. */
int json_get_string(const cJSON *obj, const char *key, char *out, size_t out_size);

/* Compatibility with the old jsonmini API: extracts a string field from JSON
   given as text. Now implemented correctly via cJSON (nested-safe, correct
   unescaping). Kept so we do not break existing callers during the migration.
   Returns 1 on success, 0 otherwise. */
int json_extract_string(const char *json, const char *key, char *out, int out_size);

#endif
