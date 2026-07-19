/*
 * selfhost.c — the implementation of the 'pixelgo selfhost' command.
 *
 * See include/selfhost.h for the contract and the rationale. The module
 * assembles, from the existing bricks (workspace_create, workspace_add_agent,
 * flow), a workspace in which pixelgo develops itself, with the HUMAN as reviewer.
 */
#include "selfhost.h"
#include "workspace.h"
#include "agent.h"
#include "provider.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#define BASE_DIR "workspaces"

/* ------------------------------------------------------------------ */
/* File utilities                                                      */
/* ------------------------------------------------------------------ */

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    if (mkdir(path, 0755) == 0) return 1;
    LOG_E("selfhost: cannot create '%s': %s", path, strerror(errno));
    return 0;
}

/* Names we do NOT copy into shared/: build artifacts, git, workspaces, the
   binary, the pidfile, and the self-host directory itself (so it does not include
   itself). */
static int is_excluded(const char *name) {
    static const char *skip[] = {
        ".git", "workspaces", "pixelgo", "pixelgo.pid",
        ".", "..", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (strcmp(name, skip[i]) == 0) return 1;
    /* any object file */
    size_t n = strlen(name);
    if (n >= 2 && strcmp(name + n - 2, ".o") == 0) return 1;
    return 0;
}

/* Copies a regular file, preserving permissions (so .sh scripts stay executable). */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) { LOG_E("selfhost: cannot read '%s': %s", src, strerror(errno)); return 0; }
    FILE *out = fopen(dst, "wb");
    if (!out) { LOG_E("selfhost: cannot write '%s': %s", dst, strerror(errno)); fclose(in); return 0; }

    char buf[65536];
    size_t r;
    int ok = 1;
    while ((r = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, r, out) != r) { ok = 0; break; }
    }
    fclose(in);
    fclose(out);

    if (ok) {
        struct stat st;
        if (stat(src, &st) == 0) chmod(dst, st.st_mode & 0777);
    }
    return ok;
}

/* Recursive copy src -> dst, skipping the excluded names at the ROOT LEVEL.
   (We exclude only at the top level: a '.o' anywhere is skipped, but 'workspaces'
   as the name of a deep subdirectory is unlikely and not worth complicating.) */
static int copy_tree(const char *src, const char *dst, int top_level) {
    if (!ensure_dir(dst)) return 0;

    DIR *d = opendir(src);
    if (!d) { LOG_E("selfhost: cannot open '%s': %s", src, strerror(errno)); return 0; }

    struct dirent *e;
    int ok = 1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        /* at the root we apply the exclusion list; everywhere we exclude *.o */
        if (top_level) {
            if (is_excluded(e->d_name)) continue;
        } else {
            size_t n = strlen(e->d_name);
            if (n >= 2 && strcmp(e->d_name + n - 2, ".o") == 0) continue;
        }

        char sp[MAX_PATH_LEN], dp[MAX_PATH_LEN];
        snprintf(sp, sizeof(sp), "%s/%s", src, e->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, e->d_name);

        struct stat st;
        if (stat(sp, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (!copy_tree(sp, dp, 0)) { ok = 0; break; }
        } else if (S_ISREG(st.st_mode)) {
            if (!copy_file(sp, dp)) { ok = 0; break; }
        }
        /* symbolic links / other types: we ignore them intentionally */
    }
    closedir(d);
    return ok;
}

static int count_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char p[MAX_PATH_LEN];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) n += count_files(p);
        else n++;
    }
    closedir(d);
    return n;
}

/* ------------------------------------------------------------------ */
/* Agent definitions + the flow                                        */
/* ------------------------------------------------------------------ */

/* Architect prompt: read-only, produces a concrete plan. */
static const char *PROMPT_ARCHITECT =
    "You are an architect on the pixelgo project, a minimalist AI agent "
    "orchestrator written in C. The codebase is in the current directory "
    "(shared/). When given a task: (1) use list_dir and read_file to understand "
    "the code RELEVANT to the task, not all of it. (2) Write a CONCRETE and SHORT "
    "plan: which files change, what changes in each, what risks exist. Do not "
    "write code yourself. End with a numbered list of steps for the coder.";

/* Coder prompt: implements, compiles, stops when make is OK. */
static const char *PROMPT_CODER =
    "You are a developer on pixelgo (C, agent orchestrator). The code is in the "
    "current directory. You receive a plan from the architect. Implement it: "
    "read_file to see the current state, write_file to apply changes, run_command "
    "'make' to compile. Rules: keep the existing style; don't break what works; "
    "if 'make' produces errors, READ them and fix until it compiles cleanly. When "
    "make exits 0, STOP and briefly summarize what you changed, file by file. Do "
    "not try to run the binary or tests - the human (the reviewer) handles that.";

static const char *FLOW_TEXT =
    "# pixelgo develops pixelgo - the reviewer is the HUMAN.\n"
    "#\n"
    "# architect designs the plan, coder implements and compiles it, then we\n"
    "# stop. You read the summary in the web UI and decide whether to keep it.\n"
    "#\n"
    "# Run:  pixelgo flow run " SELFHOST_WS_DEFAULT " " SELFHOST_FLOW_NAME " \"your task\"\n"
    "\n"
    "entry architect\n"
    "\n"
    "node architect\n"
    "node coder\n"
    "\n"
    "edge architect -> coder\n"
    "edge coder     -> END\n";

/* Builds an AI agent in the structure, reusing the same fields as
   'agent add ai'. tools_csv and allow_csv are comma-separated lists. */
static void build_ai_agent(agent_t *a, const char *id, llm_provider_id_t pid,
                           const char *model, const char *prompt,
                           const char *tools_csv, const char *allow_csv) {
    memset(a, 0, sizeof(*a));
    snprintf(a->id, MAX_STR, "%s", id);
    a->type = AGENT_TYPE_AI;
    a->cfg.ai.provider = pid;
    snprintf(a->cfg.ai.model, MAX_STR, "%s", model);
    snprintf(a->cfg.ai.system_prompt, sizeof(a->cfg.ai.system_prompt), "%s", prompt);

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", tools_csv);
    char *tok = strtok(buf, ",");
    while (tok && a->cfg.ai.tool_count < MAX_TOOLS) {
        snprintf(a->cfg.ai.tools[a->cfg.ai.tool_count], MAX_STR, "%s", tok);
        a->cfg.ai.tool_count++;
        tok = strtok(NULL, ",");
    }

    if (allow_csv && allow_csv[0]) {
        char abuf[1024];
        snprintf(abuf, sizeof(abuf), "%s", allow_csv);
        tok = strtok(abuf, ",");
        while (tok && a->cfg.ai.allowlist_count < MAX_ALLOWLIST) {
            snprintf(a->cfg.ai.run_command_allowlist[a->cfg.ai.allowlist_count],
                     MAX_STR, "%s", tok);
            a->cfg.ai.allowlist_count++;
            tok = strtok(NULL, ",");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int selfhost_init(const selfhost_opts_t *opts) {
    const char *ws_name  = (opts && opts->ws_name)  ? opts->ws_name  : SELFHOST_WS_DEFAULT;
    const char *src_dir  = (opts && opts->src_dir)  ? opts->src_dir  : ".";
    const char *prov_str = (opts && opts->provider) ? opts->provider : "anthropic";
    int force            = opts ? opts->force : 0;

    llm_provider_id_t pid = provider_from_string(prov_str);
    if (pid == LLM_PROVIDER_UNKNOWN) {
        fprintf(stderr, "Unknown provider: '%s' (valid: anthropic, openai, gemini)\n", prov_str);
        return 1;
    }

    /* default model per provider if none was given */
    const char *model = (opts && opts->model) ? opts->model : NULL;
    if (!model) {
        switch (pid) {
            case LLM_PROVIDER_ANTHROPIC: model = "claude-sonnet-5"; break;
            case LLM_PROVIDER_OPENAI:    model = "gpt-4o";          break;
            case LLM_PROVIDER_GEMINI:    model = "gemini-1.5-pro";  break;
            default:                     model = "claude-sonnet-5"; break;
        }
    }

    char ws_path[MAX_PATH_LEN];
    snprintf(ws_path, MAX_PATH_LEN, "%s/%s", BASE_DIR, ws_name);

    /* If the workspace already exists: we refuse (non-force) or delete it (force). */
    struct stat st;
    if (stat(ws_path, &st) == 0) {
        if (!force) {
            fprintf(stderr,
                "Workspace '%s' already exists in %s.\n"
                "Use --force to rebuild it from scratch (DELETES its contents).\n",
                ws_name, ws_path);
            return 1;
        }
        char cmd[MAX_PATH_LEN + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", ws_path);
        if (system(cmd) != 0) {
            fprintf(stderr, "Could not delete existing workspace '%s'\n", ws_path);
            return 1;
        }
        printf("selfhost: old workspace deleted (--force).\n");
    }

    /* 1. create the workspace (also creates shared/) */
    workspace_t ws;
    if (!workspace_create(&ws, ws_name, BASE_DIR)) {
        fprintf(stderr, "Error creating workspace '%s'\n", ws_name);
        return 1;
    }
    printf("selfhost: workspace '%s' created.\n", ws_name);

    /* 2. copy the codebase into shared/ */
    char shared[MAX_PATH_LEN + 16];
    snprintf(shared, sizeof(shared), "%s/shared", ws.root_dir);
    printf("selfhost: copying codebase from '%s' into shared/ ...\n", src_dir);
    if (!copy_tree(src_dir, shared, 1)) {
        fprintf(stderr, "Error copying codebase into shared/\n");
        return 1;
    }
    printf("selfhost: %d files in shared/.\n", count_files(shared));

    /* 3. add the agents */
    agent_t architect, coder;
    build_ai_agent(&architect, "architect", pid, model, PROMPT_ARCHITECT,
                   "read_file,list_dir", NULL);
    build_ai_agent(&coder, "coder", pid, model, PROMPT_CODER,
                   "read_file,write_file,list_dir,run_command", "make,gcc,ls,cat");

    if (!workspace_add_agent(&ws, &architect)) {
        fprintf(stderr, "Error adding 'architect' agent\n");
        return 1;
    }
    if (!workspace_add_agent(&ws, &coder)) {
        fprintf(stderr, "Error adding 'coder' agent\n");
        return 1;
    }
    printf("selfhost: agents added: architect (read-only), coder (writes+make).\n");

    /* 4. write the flow in the workspace root */
    char flow_path[MAX_PATH_LEN + 32];
    snprintf(flow_path, sizeof(flow_path), "%s/%s", ws.root_dir, SELFHOST_FLOW_NAME);
    FILE *f = fopen(flow_path, "w");
    if (!f) {
        fprintf(stderr, "Cannot write flow '%s': %s\n", flow_path, strerror(errno));
        return 1;
    }
    fputs(FLOW_TEXT, f);
    fclose(f);
    printf("selfhost: flow written to %s.\n", flow_path);

    /* summary / next steps */
    printf(
        "\n"
        "== Done. pixelgo can now develop itself. ==\n"
        "\n"
        "You are the reviewer: the flow stops after coder and shows you the summary.\n"
        "\n"
        "Start the development instance on another port (leaves production on 8080 untouched):\n"
        "    pixelgo serve 8090\n"
        "    # open http://127.0.0.1:8090, pick '%s', load %s, type a task, Run\n"
        "\n"
        "Or from the CLI:\n"
        "    export ANTHROPIC_API_KEY=sk-...\n"
        "    pixelgo flow run %s %s/%s \"What you want to improve\"\n"
        "\n"
        "The code the agents work on: %s\n",
        ws_name, SELFHOST_FLOW_NAME,
        ws_name, ws_path, SELFHOST_FLOW_NAME,
        shared);

    return 0;
}
