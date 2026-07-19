#include "workspace.h"
#include "provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* helpers defined below (escaping for arguments containing ';') */
static void write_escaped(FILE *f, const char *s);
static void write_escaped_multiline(FILE *f, const char *s);
static void unescape_multiline(char *s);
static int split_escaped(const char *str, char sep, char out[][MAX_STR], int max_items);

/* NOTE: we use our own simple config format (key=value on lines, sections in
   [workspace]/[agent]). When we move to production, we can easily swap in a real
   TOML library (tomlc99) - the rest of the code does not change, only
   workspace_load/save. */

static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) == 0) return 1;
    if (errno == EEXIST) return 1;
    perror("mkdir");
    return 0;
}

int workspace_create(workspace_t *ws, const char *name, const char *base_dir) {
    memset(ws, 0, sizeof(*ws));
    snprintf(ws->name, MAX_STR, "%s", name);
    snprintf(ws->root_dir, MAX_PATH_LEN, "%s/%s", base_dir, name);

    if (!ensure_dir(base_dir)) return 0;
    if (!ensure_dir(ws->root_dir)) return 0;

    char agents_dir[MAX_PATH_LEN + 16];
    snprintf(agents_dir, sizeof(agents_dir), "%s/agents", ws->root_dir);
    if (!ensure_dir(agents_dir)) return 0;

    /* The SHARED directory: all agents in the workspace see it. This is where you
       put the code they work on together (otherwise each would have its own copy,
       and a reviewer would not see what a coder fixed). */
    char shared_dir[MAX_PATH_LEN + 16];
    snprintf(shared_dir, sizeof(shared_dir), "%s/shared", ws->root_dir);
    if (!ensure_dir(shared_dir)) return 0;

    return workspace_save(ws);
}

int workspace_add_agent(workspace_t *ws, agent_t *agent) {
    if (ws->agent_count >= MAX_AGENTS) return 0;

    /* the agent's directory is always relative to the workspace: agents/<id> */
    char full_dir[MAX_PATH_LEN + MAX_STR + 16];
    snprintf(full_dir, sizeof(full_dir), "%s/agents/%s", ws->root_dir, agent->id);
    /* if the full path does not fit in the agent->dir field, we refuse (otherwise
       we would silently truncate the sandbox path - dangerous). */
    if (strlen(full_dir) >= MAX_PATH_LEN) {
        fprintf(stderr, "Error: agent directory path is too long\n");
        return 0;
    }
    if (!ensure_dir(full_dir)) return 0;

    /* the length was already checked < MAX_PATH_LEN above; the copy is safe */
    memcpy(agent->dir, full_dir, strlen(full_dir) + 1);

    /* All agents in a workspace share the same shared directory. */
    char sh[MAX_PATH_LEN + 16];
    snprintf(sh, sizeof(sh), "%s/shared", ws->root_dir);
    if (strlen(sh) < MAX_PATH_LEN)
        memcpy(agent->shared_dir, sh, strlen(sh) + 1);
    agent->status = AGENT_STOPPED;
    agent->pid = 0;

    ws->agents[ws->agent_count++] = *agent;
    return workspace_save(ws);
}

agent_t *workspace_find_agent(workspace_t *ws, const char *id) {
    for (int i = 0; i < ws->agent_count; i++) {
        if (strcmp(ws->agents[i].id, id) == 0) return &ws->agents[i];
    }
    return NULL;
}

int workspace_save(workspace_t *ws) {
    char conf_path[MAX_PATH_LEN + 24];
    snprintf(conf_path, sizeof(conf_path), "%s/workspace.conf", ws->root_dir);

    FILE *f = fopen(conf_path, "w");
    if (!f) { perror("fopen"); return 0; }

    fprintf(f, "[workspace]\n");
    fprintf(f, "name=%s\n\n", ws->name);

    for (int i = 0; i < ws->agent_count; i++) {
        agent_t *a = &ws->agents[i];
        fprintf(f, "[agent]\n");
        fprintf(f, "id=%s\n", a->id);
        fprintf(f, "type=%s\n", a->type == AGENT_TYPE_WORKER ? "worker" : "ai");
        fprintf(f, "dir=%s\n", a->dir);

        if (a->type == AGENT_TYPE_WORKER) {
            fprintf(f, "command=%s\n", a->cfg.worker.command);
            fprintf(f, "args=");
            for (int j = 0; j < a->cfg.worker.argc; j++) {
                write_escaped(f, a->cfg.worker.args[j]);
                if (j + 1 < a->cfg.worker.argc) fputc(';', f);
            }
            fprintf(f, "\n");
        } else {
            fprintf(f, "provider=%s\n", provider_to_string(a->cfg.ai.provider));
            fprintf(f, "model=%s\n", a->cfg.ai.model);
            fprintf(f, "system_prompt=");
            write_escaped_multiline(f, a->cfg.ai.system_prompt);
            fprintf(f, "\n");
            fprintf(f, "tools=");
            for (int j = 0; j < a->cfg.ai.tool_count; j++) {
                fprintf(f, "%s%s", a->cfg.ai.tools[j], j + 1 < a->cfg.ai.tool_count ? "," : "");
            }
            fprintf(f, "\n");
            fprintf(f, "allowlist=");
            for (int j = 0; j < a->cfg.ai.allowlist_count; j++) {
                fprintf(f, "%s%s", a->cfg.ai.run_command_allowlist[j], j + 1 < a->cfg.ai.allowlist_count ? "," : "");
            }
            fprintf(f, "\n");
            /* Written only when set, so configs of unsupervised agents stay clean. */
            if (a->cfg.ai.approval_count > 0) {
                fprintf(f, "approval=");
                for (int j = 0; j < a->cfg.ai.approval_count; j++) {
                    fprintf(f, "%s%s", a->cfg.ai.approval_tools[j], j + 1 < a->cfg.ai.approval_count ? "," : "");
                }
                fprintf(f, "\n");
            }
        }

        /* Resource limits - common to both types. We write only the ones that are
           set (> 0), so the file stays clean for agents without limits. */
        if (a->limits.cpu_seconds > 0)
            fprintf(f, "limit_cpu_seconds=%ld\n", a->limits.cpu_seconds);
        if (a->limits.mem_bytes > 0)
            fprintf(f, "limit_mem_bytes=%ld\n", a->limits.mem_bytes);
        if (a->limits.fsize_bytes > 0)
            fprintf(f, "limit_fsize_bytes=%ld\n", a->limits.fsize_bytes);
        if (a->limits.timeout_seconds > 0)
            fprintf(f, "limit_timeout_seconds=%ld\n", a->limits.timeout_seconds);

        fprintf(f, "\n");
    }

    fclose(f);
    return 1;
}

/* simple split on a separator character, fills out[] up to max_items */
static int split(char *str, char sep, char out[][MAX_STR], int max_items) {
    int count = 0;
    char sepstr[2] = {sep, 0};
    char *saveptr = NULL;
    char *t = strtok_r(str, sepstr, &saveptr);
    while (t && count < max_items) {
        snprintf(out[count], MAX_STR, "%s", t);
        count++;
        t = strtok_r(NULL, sepstr, &saveptr);
    }
    return count;
}

/*
 * BUGFIX: the workers' arguments were saved/loaded with a naive split on ';'.
 * Any argument CONTAINING ';' (very common: `sh -c "sleep 2; echo hi"`) was torn
 * into pieces on reload, and the agent ran the wrong command.
 * Solution: we escape '\' and ';' on write, and unescape them on read. The format
 * stays compatible with arguments that do not contain ';'.
 */

/* Writes an argument with ';' and '\' escaped. */
/*
 * BUGFIX: the system_prompt is multi-line (detailed instructions for the agent),
 * but the config format is "key=value" on LINES. A newline in the value broke the
 * file: on reload only the first line was kept, and the agent lost exactly the
 * instructions that made it compile and verify.
 *
 * Solution: we escape the newline as "\n" (two characters: backslash + n) on
 * write, and restore it on read. Compatible with single-line prompts.
 */
static void write_escaped_multiline(FILE *f, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\\') {
            fputc('\\', f);
            fputc('\\', f);
        } else if (*p == '\n') {
            fputc('\\', f);
            fputc('n', f);
        } else {
            fputc(*p, f);
        }
    }
}

/* Restores the escaped newlines, in-place (the result is shorter or equal). */
static void unescape_multiline(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '\\' && *(r + 1) == 'n') {
            *w++ = '\n';
            r++;
        } else if (*r == '\\' && *(r + 1) == '\\') {
            *w++ = '\\';
            r++;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static void write_escaped(FILE *f, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == ';') fputc('\\', f);
        fputc(*p, f);
    }
}

/* A split that respects escaping: '\;' is not a separator, but a literal ';'. */
static int split_escaped(const char *str, char sep, char out[][MAX_STR], int max_items) {
    int count = 0;
    size_t oi = 0;
    if (max_items <= 0) return 0;
    out[0][0] = 0;

    for (const char *p = str; *p; p++) {
        if (*p == '\\' && *(p + 1)) {
            /* escaped character -> we take it literally */
            p++;
            if (oi < MAX_STR - 1) out[count][oi++] = *p;
            out[count][oi] = 0;
        } else if (*p == sep) {
            count++;
            if (count >= max_items) return count;
            oi = 0;
            out[count][0] = 0;
        } else {
            if (oi < MAX_STR - 1) out[count][oi++] = *p;
            out[count][oi] = 0;
        }
    }
    /* the last element exists only if we wrote something into it */
    if (out[count][0] != 0 || count > 0) count++;
    return count;
}

int workspace_load(workspace_t *ws, const char *root_dir) {
    memset(ws, 0, sizeof(*ws));
    snprintf(ws->root_dir, MAX_PATH_LEN, "%s", root_dir);

    char conf_path[MAX_PATH_LEN + 24];
    snprintf(conf_path, sizeof(conf_path), "%s/workspace.conf", root_dir);

    FILE *f = fopen(conf_path, "r");
    if (!f) { perror("fopen"); return 0; }

    char line[4096];
    agent_t *current = NULL;
    char section[32] = {0};

    while (fgets(line, sizeof(line), f)) {
        /* strip newline */
        line[strcspn(line, "\n")] = 0;
        if (line[0] == 0) continue;

        if (strcmp(line, "[workspace]") == 0) { strcpy(section, "workspace"); continue; }
        if (strcmp(line, "[agent]") == 0) {
            strcpy(section, "agent");
            current = &ws->agents[ws->agent_count++];
            memset(current, 0, sizeof(*current));
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;

        if (strcmp(section, "workspace") == 0) {
            if (strcmp(key, "name") == 0) snprintf(ws->name, MAX_STR, "%s", val);
        } else if (strcmp(section, "agent") == 0 && current) {
            if (strcmp(key, "id") == 0) snprintf(current->id, MAX_STR, "%s", val);
            else if (strcmp(key, "dir") == 0) snprintf(current->dir, MAX_PATH_LEN, "%s", val);
            else if (strcmp(key, "type") == 0)
                current->type = strcmp(val, "worker") == 0 ? AGENT_TYPE_WORKER : AGENT_TYPE_AI;
            else if (strcmp(key, "command") == 0)
                snprintf(current->cfg.worker.command, MAX_PATH_LEN, "%s", val);
            else if (strcmp(key, "args") == 0)
                current->cfg.worker.argc = split_escaped(val, ';', current->cfg.worker.args, MAX_TOOLS);
            else if (strcmp(key, "provider") == 0) {
                current->cfg.ai.provider = provider_from_string(val);
                if (current->cfg.ai.provider == LLM_PROVIDER_UNKNOWN)
                    fprintf(stderr, "Warning: unknown provider '%s' for agent '%s'\n",
                            val, current->id);
            }
            else if (strcmp(key, "model") == 0)
                snprintf(current->cfg.ai.model, MAX_STR, "%s", val);
            else if (strcmp(key, "system_prompt") == 0) {
                snprintf(current->cfg.ai.system_prompt,
                         sizeof(current->cfg.ai.system_prompt), "%s", val);
                unescape_multiline(current->cfg.ai.system_prompt);
            }
            else if (strcmp(key, "tools") == 0)
                current->cfg.ai.tool_count = split(val, ',', current->cfg.ai.tools, MAX_TOOLS);
            else if (strcmp(key, "allowlist") == 0)
                current->cfg.ai.allowlist_count = split(val, ',', current->cfg.ai.run_command_allowlist, MAX_ALLOWLIST);
            else if (strcmp(key, "approval") == 0)
                current->cfg.ai.approval_count = split(val, ',', current->cfg.ai.approval_tools, MAX_TOOLS);
            else if (strcmp(key, "limit_cpu_seconds") == 0)
                current->limits.cpu_seconds = strtol(val, NULL, 10);
            else if (strcmp(key, "limit_mem_bytes") == 0)
                current->limits.mem_bytes = strtol(val, NULL, 10);
            else if (strcmp(key, "limit_fsize_bytes") == 0)
                current->limits.fsize_bytes = strtol(val, NULL, 10);
            else if (strcmp(key, "limit_timeout_seconds") == 0)
                current->limits.timeout_seconds = strtol(val, NULL, 10);
        }
    }

    fclose(f);

    /*
     * BUGFIX: shared_dir is NOT saved in the config (it is derived from the
     * workspace root). We set it for ALL agents at the end of loading - more robust
     * than depending on the order of the keys in the file.
     *
     * Without this, agent->shared_dir stayed empty -> the agents did not see the
     * shared directory -> they worked on separate copies -> an infinite loop in
     * code review.
     */
    for (int i = 0; i < ws->agent_count; i++) {
        char sh[MAX_PATH_LEN + 16];
        int n = snprintf(sh, sizeof(sh), "%s/shared", ws->root_dir);
        if (n > 0 && (size_t)n < MAX_PATH_LEN)
            memcpy(ws->agents[i].shared_dir, sh, (size_t)n + 1);
        else
            ws->agents[i].shared_dir[0] = 0;
    }

    return 1;
}
