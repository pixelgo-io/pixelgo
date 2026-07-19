#include "tools.h"
#include "sandbox.h"
#include "json_util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

/*
 * Argument validation: each tool parses input_json with cJSON and checks the
 * PRESENCE + TYPE of each expected field, returning standardized errors
 * (tool_err) if something is missing or has the wrong type. Nothing reaches the
 * disk without first going through sandbox_resolve_path.
 */

tool_result_t tool_read_file(agent_t *agent, const char *input_json) {
    cJSON *args = json_parse(input_json);
    if (!args || !cJSON_IsObject(args)) {
        if (args) cJSON_Delete(args);
        return tool_err("invalid arguments JSON");
    }

    char path[MAX_PATH_LEN];
    if (!json_get_string(args, "path", path, sizeof(path))) {
        cJSON_Delete(args);
        return tool_err("missing or non-string 'path' field");
    }
    cJSON_Delete(args);

    char resolved[PATH_MAX];
    if (!sandbox_resolve_path_ex(agent->dir, agent->shared_dir, path, resolved)) {
        LOG_W("read_file: access denied, path '%s' escapes the sandbox", path);
        return tool_err("access denied, path outside agent sandbox");
    }

    /* We explicitly refuse directories - fopen("r") on a directory can succeed
       on some systems and read garbage. */
    struct stat st;
    if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode))
        return tool_err("'%s' is a directory, not a file", path);

    FILE *f = fopen(resolved, "r");
    if (!f) return tool_err("could not open file '%s'", path);

    tool_result_t r = {0};
    size_t n = fread(r.output, 1, TOOL_RESULT_MAX - 1, f);
    r.output[n] = 0;
    int truncated = !feof(f);
    fclose(f);
    r.ok = 1;

    if (truncated)
        LOG_W("read_file: '%s' truncated at %d bytes", path, TOOL_RESULT_MAX - 1);
    return r;
}

/*
 * Creates the parent directories of a path, one level at a time, checking each
 * against the sandbox before creating it.
 *
 * Why this exists: sandbox_resolve_path refuses a path whose parent directory
 * does not exist, so writing "nginx/default.conf" into an empty workspace fails
 * even though the target is perfectly legal. Agents then waste turns inventing
 * workarounds - one of them tried running mkdir inside a throwaway container.
 *
 * Why it is safe: every level is resolved and checked individually, so a path
 * that escapes the sandbox is rejected before anything is created. The worst a
 * bad path can do is fail.
 *
 * Returns 1 if the parents exist (or were created), 0 otherwise.
 */
static int ensure_parent_dirs(agent_t *agent, const char *path) {
    /* Walk the path, stopping at each '/' to create that level. */
    char partial[MAX_PATH_LEN];
    size_t n = strlen(path);
    if (n >= sizeof(partial)) return 0;

    for (size_t i = 0; i < n; i++) {
        if (path[i] != '/' || i == 0) continue;

        memcpy(partial, path, i);
        partial[i] = 0;

        char resolved[PATH_MAX];
        if (!sandbox_resolve_path_ex(agent->dir, agent->shared_dir, partial, resolved))
            return 0;   /* outside the sandbox: refuse before creating anything */

        struct stat st;
        if (stat(resolved, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return 0;   /* a file is in the way */
            continue;                             /* already there */
        }

        if (mkdir(resolved, 0755) != 0 && errno != EEXIST) {
            LOG_W("write_file: cannot create directory '%s': %s", partial, strerror(errno));
            return 0;
        }
        LOG_I("write_file: created directory '%s'", partial);
    }
    return 1;
}

tool_result_t tool_write_file(agent_t *agent, const char *input_json) {
    cJSON *args = json_parse(input_json);
    if (!args || !cJSON_IsObject(args)) {
        if (args) cJSON_Delete(args);
        return tool_err("invalid arguments JSON");
    }

    char path[MAX_PATH_LEN];
    if (!json_get_string(args, "path", path, sizeof(path))) {
        cJSON_Delete(args);
        return tool_err("missing or non-string 'path' field");
    }

    /* content can be large - we take it as a pointer straight from the node, without copying. */
    cJSON *content_item = cJSON_GetObjectItemCaseSensitive(args, "content");
    if (!cJSON_IsString(content_item) || content_item->valuestring == NULL) {
        cJSON_Delete(args);
        return tool_err("missing or non-string 'content' field");
    }
    const char *content = content_item->valuestring;

    /* Create any missing parent directories first, so writing into a new
       subdirectory works without the agent having to run mkdir itself. Each
       level is sandbox-checked as it goes. */
    if (strchr(path, '/') && !ensure_parent_dirs(agent, path)) {
        cJSON_Delete(args);
        return tool_err("could not create the directories for '%s'", path);
    }

    char resolved[PATH_MAX];
    if (!sandbox_resolve_path_ex(agent->dir, agent->shared_dir, path, resolved)) {
        cJSON_Delete(args);
        LOG_W("write_file: access denied, path '%s' escapes the sandbox", path);
        return tool_err("access denied, path outside agent sandbox");
    }

    FILE *f = fopen(resolved, "w");
    if (!f) {
        cJSON_Delete(args);
        return tool_err("could not write file '%s'", path);
    }
    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    int close_err = fclose(f);
    cJSON_Delete(args);

    if (written != len || close_err != 0)
        return tool_err("partial write to '%s' (%zu/%zu bytes)", path, written, len);

    LOG_I("write_file: '%s' written (%zu bytes)", path, len);
    return tool_ok("File '%s' written successfully (%zu bytes)", path, len);
}

tool_result_t tool_list_dir(agent_t *agent, const char *input_json) {
    /* path is optional here (defaults to "."). We parse defensively anyway. */
    char path[MAX_PATH_LEN] = ".";
    if (input_json && input_json[0]) {
        cJSON *args = json_parse(input_json);
        if (args && cJSON_IsObject(args)) {
            cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
            if (cJSON_IsString(p) && p->valuestring && p->valuestring[0])
                snprintf(path, sizeof(path), "%s", p->valuestring);
            /* if "path" exists but is not a string, we ignore it and use the default */
        }
        if (args) cJSON_Delete(args);
    }

    char resolved[PATH_MAX];
    if (!sandbox_resolve_path_ex(agent->dir, agent->shared_dir, path, resolved)) {
        LOG_W("list_dir: access denied, path '%s' escapes the sandbox", path);
        return tool_err("access denied, path outside agent sandbox");
    }

    DIR *d = opendir(resolved);
    if (!d) return tool_err("could not list directory '%s'", path);

    tool_result_t r = {0};
    r.ok = 1;
    int offset = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        /* we mark directories with a trailing '/' for clarity */
        char full[PATH_MAX];
        int suffix = 0;
        if (snprintf(full, sizeof(full), "%s/%s", resolved, entry->d_name) < (int)sizeof(full)) {
            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) suffix = 1;
        }
        int n = snprintf(r.output + offset, TOOL_RESULT_MAX - offset,
                         "%s%s\n", entry->d_name, suffix ? "/" : "");
        if (n < 0) break;
        offset += n;
        if (offset >= TOOL_RESULT_MAX - 1) break;
    }
    closedir(d);

    if (offset == 0)
        snprintf(r.output, TOOL_RESULT_MAX, "(empty directory)");
    return r;
}
