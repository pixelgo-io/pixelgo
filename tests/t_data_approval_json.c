#include "test.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * The data-approval gateway (ai_loop.c) shows the human the ACTUAL request
 * about to be sent - model/system/messages/tools, pretty-printed as real
 * JSON - not a summary or a single block picked out of context. On "Send
 * trimmed", the edited JSON is parsed back and its "messages" array
 * WHOLESALE REPLACES the live conversation: deleting a whole turn from the
 * JSON removes it entirely, not just empties its text.
 *
 * ai_loop_run() itself needs a live agent + provider to exercise end to
 * end, so this test exercises the exact same cJSON operations ai_loop.c
 * performs, in isolation - the same logic verified manually against the
 * real code during development, now checked in so it can't regress
 * unnoticed.
 */

/* Mirrors ai_loop.c's preview construction. */
static char *build_preview(cJSON *messages) {
    cJSON *preview = cJSON_CreateObject();
    cJSON_AddStringToObject(preview, "model", "claude-sonnet-5");
    cJSON_AddStringToObject(preview, "system", "You are a helper.");
    cJSON_AddItemToObject(preview, "messages", cJSON_Duplicate(messages, 1));
    char *text = cJSON_Print(preview);
    cJSON_Delete(preview);
    return text;
}

/*
 * Mirrors ai_loop.c's parse-and-replace: on success, `messages`'s CONTENTS
 * are replaced in place (same array pointer, new children) - this is what
 * lets a deleted turn simply not come back, and what history_save() at the
 * end of the run persists. Returns 1 on success, 0 if the edited text
 * cannot be used (the caller must then DENY the request, never send it,
 * never silently keep the stale original).
 */
static int parse_and_replace(cJSON *messages, const char *edited_text) {
    cJSON *edited_root = cJSON_Parse(edited_text);
    cJSON *edited_messages = edited_root
        ? cJSON_DetachItemFromObjectCaseSensitive(edited_root, "messages")
        : NULL;

    int ok = cJSON_IsArray(edited_messages);
    if (ok) {
        while (cJSON_GetArraySize(messages) > 0)
            cJSON_DeleteItemFromArray(messages, 0);
        cJSON *item = edited_messages->child;
        while (item) {
            cJSON *next = item->next;
            cJSON_DetachItemViaPointer(edited_messages, item);
            cJSON_AddItemToArray(messages, item);
            item = next;
        }
    }
    if (edited_messages) cJSON_Delete(edited_messages);
    if (edited_root) cJSON_Delete(edited_root);
    return ok;
}

static cJSON *make_text_message(const char *role, const char *text) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON *content = cJSON_CreateArray();
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", text);
    cJSON_AddItemToArray(content, block);
    cJSON_AddItemToObject(m, "content", content);
    return m;
}

static void test_preview_contains_everything(void) {
    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, make_text_message("user", "hello"));
    cJSON_AddItemToArray(messages, make_text_message("assistant", "hi there"));

    char *preview = build_preview(messages);

    ok("preview contains the model", strstr(preview, "claude-sonnet-5") != NULL);
    ok("preview contains the system prompt", strstr(preview, "You are a helper.") != NULL);
    ok("preview contains the first message's text", strstr(preview, "hello") != NULL);
    ok("preview contains the second message's text", strstr(preview, "hi there") != NULL);
    ok("preview is pretty-printed (not compact - contains a newline)",
       strchr(preview, '\n') != NULL);

    cJSON_free(preview);
    cJSON_Delete(messages);
}

static void test_edit_replaces_text_in_place(void) {
    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, make_text_message("user", "original text"));

    const char *edited =
        "{\"messages\":[{\"role\":\"user\",\"content\":"
        "[{\"type\":\"text\",\"text\":\"edited text\"}]}]}";

    int result = parse_and_replace(messages, edited);
    ok("valid edit: parse_and_replace succeeds", result);
    eq_int("valid edit: message count unchanged (1)", cJSON_GetArraySize(messages), 1);

    char *final = cJSON_PrintUnformatted(messages);
    ok("valid edit: new text is present", strstr(final, "edited text") != NULL);
    ok("valid edit: old text is gone", strstr(final, "original text") == NULL);
    cJSON_free(final);

    cJSON_Delete(messages);
}

static void test_deleting_a_whole_turn_removes_it_entirely(void) {
    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, make_text_message("user", "keep me"));
    cJSON_AddItemToArray(messages, make_text_message("user", "junk - delete me"));
    cJSON_AddItemToArray(messages, make_text_message("assistant", "also keep me"));
    eq_int("setup: 3 messages before editing", cJSON_GetArraySize(messages), 3);

    /* The human deleted the middle ("junk") message entirely - not emptied
       its text, removed the whole object from the array. */
    const char *edited =
        "{\"messages\":["
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"keep me\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"also keep me\"}]}"
        "]}";

    int result = parse_and_replace(messages, edited);
    ok("delete-a-turn: parse_and_replace succeeds", result);
    eq_int("delete-a-turn: array shrinks from 3 to 2 (whole message gone, not emptied)",
           cJSON_GetArraySize(messages), 2);

    char *final = cJSON_PrintUnformatted(messages);
    ok("delete-a-turn: the junk text is gone entirely", strstr(final, "junk") == NULL);
    ok("delete-a-turn: the kept messages survive", strstr(final, "keep me") != NULL);
    cJSON_free(final);

    cJSON_Delete(messages);
}

static void test_malformed_json_fails_closed(void) {
    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, make_text_message("user", "original, must survive untouched"));

    int result = parse_and_replace(messages, "{ this is not valid json at all");
    ok("malformed JSON: parse_and_replace reports failure (caller must deny)", !result);
    eq_int("malformed JSON: messages array left untouched", cJSON_GetArraySize(messages), 1);

    char *final = cJSON_PrintUnformatted(messages);
    ok("malformed JSON: original content is intact", strstr(final, "must survive untouched") != NULL);
    cJSON_free(final);

    cJSON_Delete(messages);
}

static void test_valid_json_missing_messages_field_fails_closed(void) {
    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, make_text_message("user", "original, must survive untouched"));

    /* Valid JSON, but no "messages" key at all - e.g. the human deleted the
       whole array by mistake while editing braces. */
    int result = parse_and_replace(messages, "{\"model\":\"claude-sonnet-5\"}");
    ok("no \"messages\" field: parse_and_replace reports failure", !result);
    eq_int("no \"messages\" field: messages array left untouched", cJSON_GetArraySize(messages), 1);

    cJSON_Delete(messages);
}

static void test_messages_field_wrong_type_fails_closed(void) {
    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, make_text_message("user", "original"));

    /* "messages" present but not an array (e.g. accidentally typed as a string). */
    int result = parse_and_replace(messages, "{\"messages\":\"oops\"}");
    ok("\"messages\" is a string, not an array: fails rather than guessing", !result);

    cJSON_Delete(messages);
}

static void test_edit_can_shrink_to_empty(void) {
    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, make_text_message("user", "hello"));

    /* An empty "messages" array is syntactically valid JSON with the right
       shape - parse_and_replace should accept it (whether ai_loop then
       wants to actually send an empty conversation is a separate policy
       question, not this function's job to second-guess). */
    int result = parse_and_replace(messages, "{\"messages\":[]}");
    ok("empty messages array: still accepted as valid", result);
    eq_int("empty messages array: result is genuinely empty", cJSON_GetArraySize(messages), 0);

    cJSON_Delete(messages);
}

int main(void) {
    section("data-approval preview: shows the real request");
    test_preview_contains_everything();

    section("data-approval edit: text changes splice in correctly");
    test_edit_replaces_text_in_place();

    section("data-approval edit: deleting a whole turn removes it entirely");
    test_deleting_a_whole_turn_removes_it_entirely();

    section("data-approval edit: invalid input fails closed, never corrupts or guesses");
    test_malformed_json_fails_closed();
    test_valid_json_missing_messages_field_fails_closed();
    test_messages_field_wrong_type_fails_closed();

    section("data-approval edit: edge cases");
    test_edit_can_shrink_to_empty();

    return t_report("data_approval_json");
}
