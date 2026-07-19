#include "history.h"
#include "json_util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Builds the path to the agent's history file.
 *
 * CAREFUL (bug fixed): agent->dir is RELATIVE to the startup directory
 * ("workspaces/X/agents/Y"). But when the agent runs as a job from the web server,
 * the child chdir()s into its own sandbox - so the relative path no longer
 * resolves and writing the history failed.
 *
 * Resolution: if the agent's directory exists relative to the current cwd, we use
 * it (the CLI case). If it does NOT exist, it means we are ALREADY inside it (the
 * job case: we chdir'd there), so we use the file name directly.
 * This works in both situations, without changing the signature or the callers.
 */
static void history_path(const agent_t *agent, char *out, size_t out_size) {
    /* agent->dir is ABSOLUTE when running as an agent (we resolved it before the
       chdir), so the path works no matter where the cwd is. Important: the cwd is
       now the SHARED directory, not the agent's - so a relative path would write the
       history into shared/, mixed between agents. */
    struct stat st;
    if (agent->dir[0] && stat(agent->dir, &st) == 0 && S_ISDIR(st.st_mode))
        snprintf(out, out_size, "%s/%s", agent->dir, HISTORY_FILE);
    else
        snprintf(out, out_size, "%s", HISTORY_FILE);
}

cJSON *history_load(const agent_t *agent) {
    char path[MAX_PATH_LEN + 32];
    history_path(agent, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_D("history: '%s' does not exist, starting a new conversation", path);
        return cJSON_CreateArray();
    }

    /* we read the whole file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 4 * 1024 * 1024) {   /* sanity: max 4 MB */
        LOG_W("history: '%s' has an invalid size (%ld), ignoring it", path, size);
        fclose(f);
        return cJSON_CreateArray();
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return cJSON_CreateArray();
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = 0;
    fclose(f);

    cJSON *messages = cJSON_Parse(buf);
    free(buf);

    if (!messages || !cJSON_IsArray(messages)) {
        LOG_W("history: '%s' is not a valid JSON array, ignoring it", path);
        if (messages) cJSON_Delete(messages);
        return cJSON_CreateArray();
    }

    LOG_I("history: loaded %d messages from the previous conversation",
          cJSON_GetArraySize(messages));
    return messages;
}

int history_save(const agent_t *agent, const cJSON *messages) {
    char path[MAX_PATH_LEN + 32];
    history_path(agent, path, sizeof(path));

    char *text = cJSON_PrintUnformatted((cJSON *)messages);
    if (!text) {
        LOG_E("history: serializing the history failed");
        return 0;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        LOG_E("history: cannot write '%s'", path);
        free(text);
        return 0;
    }

    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    int close_err = fclose(f);
    free(text);

    if (written != len || close_err != 0) {
        LOG_E("history: partial write to '%s'", path);
        return 0;
    }

    LOG_D("history: saved %d messages", cJSON_GetArraySize((cJSON *)messages));
    return 1;
}

int history_reset(const agent_t *agent) {
    char path[MAX_PATH_LEN + 32];
    history_path(agent, path, sizeof(path));
    remove(path);   /* we do not care whether it exists or not */
    LOG_I("history: history cleared for agent '%s'", agent->id);
    return 1;
}

/* True if the message is a 'user' one starting with a tool_result block.
   Such a message can NOT be the first in the history: it would be orphaned,
   without the assistant message with the tool_use that requested it, and the API
   would reject the request. */
static int is_tool_result_message(const cJSON *msg) {
    const cJSON *role = cJSON_GetObjectItemCaseSensitive(msg, "role");
    if (!cJSON_IsString(role) || strcmp(role->valuestring, "user") != 0) return 0;

    const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (!cJSON_IsArray(content)) return 0;

    const cJSON *first = cJSON_GetArrayItem((cJSON *)content, 0);
    if (!first) return 0;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(first, "type");
    return cJSON_IsString(type) && strcmp(type->valuestring, "tool_result") == 0;
}

int history_trim(cJSON *messages, int max_messages) {
    int count = cJSON_GetArraySize(messages);
    if (count <= max_messages) return 0;

    /* How many we want to trim, as a first approximation. */
    int to_cut = count - max_messages;

    /*
     * We look for a SAFE trim boundary, starting from to_cut and moving forward.
     * A boundary is safe if the message that will become FIRST is not an orphan
     * tool_result (i.e. not a response to a tool_use we just trimmed).
     *
     * We move forward (not backward) so we do not trim less than needed -
     * otherwise we could exceed the context limit.
     */
    int cut = to_cut;
    while (cut < count) {
        const cJSON *first_kept = cJSON_GetArrayItem(messages, cut);
        if (!first_kept) break;
        if (!is_tool_result_message(first_kept)) break;   /* a safe boundary */
        cut++;   /* an orphan tool_result would remain -> we trim this message too */
    }

    if (cut >= count) {
        /* Pathological case: the whole history is tool_results. We clear it entirely
           (better a new conversation than an invalid one). */
        LOG_W("history: no safe trim boundary found, clearing the history");
        cut = count;
    }

    /* We trim the first `cut` messages. We always delete index 0, `cut` times. */
    for (int i = 0; i < cut; i++)
        cJSON_DeleteItemFromArray(messages, 0);

    LOG_I("history: trimmed %d old messages (history exceeded %d)",
          cut, max_messages);
    return cut;
}
