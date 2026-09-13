#include "workspace.h"
#include "data_request_audit.h"
#include "agent_run.h"
#include "flow.h"
#include "orchestrator.h"
#include "provider.h"
#include "history.h"
#include "ai_loop.h"
#include "http_server.h"
#include "daemon.h"
#include "jobs.h"
#include "envfile.h"
#include "selfhost.h"
#include "log.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#define BASE_DIR "workspaces"

static void print_usage(void) {
    printf(
        "pixelgo " PIXELGO_VERSION "\n\n"
        "Usage: pixelgo <command> [options]\n\n"
        "  workspace create <name>\n"
        "  agent add worker <workspace> <agent_id> <command> [args...]\n"
        "  agent add ai     <workspace> <agent_id> <provider> <model> <system_prompt> <tool1,tool2,...> [cmd1,cmd2,...]\n"
        "                   provider: anthropic | openai | gemini\n"
        "                   tools:    read_file, write_file, list_dir, run_command, search_files\n"
        "                   last argument (optional): commands allowed for run_command, e.g. \"gcc,make\"\n"
        "                   --approve <tool1,tool2>: ask you before those tools run, e.g. \"write_file,run_command\"\n"
        "                   --data-threshold <size>: ask you before a request over this size is sent to the\n"
        "                                            provider, e.g. \"500KB\", \"2MB\", or \"off\" to explicitly\n"
        "                                            disable it for this agent. Default: inherits\n"
        "                                            PIXELGO_DATA_THRESHOLD if set, otherwise disabled.\n"
        "  agent run  <workspace> <agent_id> [task]        (single question)\n"
        "  agent chat <workspace> <agent_id> [--reset]    (conversation, remembers)\n"
        "  agent list <workspace>\n"
        "  flow run     <workspace> <file.flow> [task]\n"
        "  flow diagram <file.flow>\n"
        "\n"
        "  serve [port] [--daemon]                        (web interface, default 8080)\n"
        "  serve stop                                     (stop the daemon)\n"
        "  serve status                                   (daemon status)\n"
        "\n"
        "  keys                                           (which API keys are configured)\n"
        "  --version                                      (print the version)\n"
        "\n"
        "  selfhost init [--ws <name>] [--src <dir>] [--provider <p>] [--model <m>] [--force]\n"
        "                                                 (pixelgo develops itself; reviewer = human)\n"
    );
}

static int cmd_workspace_create(const char *name) {
    workspace_t ws;
    if (!workspace_create(&ws, name, BASE_DIR)) {
        fprintf(stderr, "Error creating workspace '%s'\n", name);
        return 1;
    }
    printf("Workspace '%s' created in %s/%s\n", name, BASE_DIR, name);
    return 0;
}

static int cmd_agent_add_worker(const char *ws_name, const char *agent_id,
                                 int argc, char **argv, int start_idx) {
    char ws_path[MAX_PATH_LEN];
    snprintf(ws_path, MAX_PATH_LEN, "%s/%s", BASE_DIR, ws_name);

    workspace_t ws;
    if (!workspace_load(&ws, ws_path)) {
        fprintf(stderr, "Workspace '%s' does not exist\n", ws_name);
        return 1;
    }

    agent_t agent = {0};
    snprintf(agent.id, MAX_STR, "%s", agent_id);
    agent.type = AGENT_TYPE_WORKER;
    snprintf(agent.cfg.worker.command, MAX_PATH_LEN, "%s", argv[start_idx]);

    agent.cfg.worker.argc = 0;
    for (int i = start_idx + 1; i < argc; i++) {
        snprintf(agent.cfg.worker.args[agent.cfg.worker.argc], MAX_STR, "%s", argv[i]);
        agent.cfg.worker.argc++;
    }

    if (!workspace_add_agent(&ws, &agent)) {
        fprintf(stderr, "Error adding agent\n");
        return 1;
    }
    printf("Worker agent '%s' added to workspace '%s', dir=%s\n", agent_id, ws_name, agent.dir);
    return 0;
}

/* Lists all existing workspaces (every subdirectory of BASE_DIR containing a
   workspace.conf). It was missing from the dispatch, though it was in the usage. */
/* Diagnostic: which API keys are available and from where. We do NOT display the
   keys in clear - only whether they are set plus a fragment, so you can check
   without accidentally exposing them. */
static int cmd_keys(void) {
    const char *loaded = envfile_loaded_path();
    if (loaded && loaded[0])
        printf("Config file: %s\n\n", loaded);
    else
        printf("Config file: (none found - using only environment variables)\n\n");

    const char *keys[] = {"ANTHROPIC_API_KEY", "OPENAI_API_KEY", "GEMINI_API_KEY"};
    const char *provs[] = {"anthropic", "openai", "gemini"};

    for (int i = 0; i < 3; i++) {
        const char *v = getenv(keys[i]);
        if (v && v[0]) {
            size_t len = strlen(v);
            /* we show only the first/last characters - enough to check it is the
               right key, without exposing it in full */
            if (len > 12)
                printf("  %-10s OK      %.6s...%.4s  (%zu chars)\n",
                       provs[i], v, v + len - 4, len);
            else
                printf("  %-10s OK      (%zu chars - looks too short?)\n", provs[i], len);
        } else {
            printf("  %-10s MISSING (set %s)\n", provs[i], keys[i]);
        }
    }

    printf("\nTo add keys, create a .env file in the current directory:\n");
    printf("  ANTHROPIC_API_KEY=sk-ant-...\n");
    printf("  OPENAI_API_KEY=sk-...\n");
    printf("  GEMINI_API_KEY=AIza...\n");
    printf("\nThen: chmod 600 .env\n");
    return 0;
}

static int cmd_workspace_list(void) {
    DIR *d = opendir(BASE_DIR);
    if (!d) {
        printf("No workspaces (directory '%s' does not exist yet)\n", BASE_DIR);
        return 0;
    }

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        char conf[MAX_PATH_LEN * 2];
        snprintf(conf, sizeof(conf), "%s/%s/workspace.conf", BASE_DIR, e->d_name);
        struct stat st;
        if (stat(conf, &st) != 0) continue;  /* not a valid workspace */

        /* we load it so we can also display the number of agents */
        char ws_path[MAX_PATH_LEN];
        snprintf(ws_path, sizeof(ws_path), "%s/%s", BASE_DIR, e->d_name);
        workspace_t ws;
        if (workspace_load(&ws, ws_path))
            printf("  - %-20s %d agent(s)   dir=%s\n", ws.name, ws.agent_count, ws_path);
        else
            printf("  - %-20s (config invalid)\n", e->d_name);
        count++;
    }
    closedir(d);

    if (count == 0) printf("No workspaces created yet.\n");
    else printf("\nTotal: %d workspace(s)\n", count);
    return 0;
}

static int cmd_agent_add_ai(const char *ws_name, const char *agent_id,
                             const char *provider_str, const char *model,
                             const char *prompt, const char *tools_csv,
                             const char *allowlist_csv,
                             const char *approval_csv,
                             const char *data_threshold_str) {
    char ws_path[MAX_PATH_LEN];
    snprintf(ws_path, MAX_PATH_LEN, "%s/%s", BASE_DIR, ws_name);

    /* We validate the provider up front - a typo here would only fail at run time. */
    llm_provider_id_t pid = provider_from_string(provider_str);
    if (pid == LLM_PROVIDER_UNKNOWN) {
        fprintf(stderr, "Unknown provider: '%s' (valid: anthropic, openai, gemini)\n",
                provider_str);
        return 1;
    }

    workspace_t ws;
    if (!workspace_load(&ws, ws_path)) {
        fprintf(stderr, "Workspace '%s' does not exist\n", ws_name);
        return 1;
    }

    agent_t agent = {0};
    snprintf(agent.id, MAX_STR, "%s", agent_id);
    agent.type = AGENT_TYPE_AI;
    agent.cfg.ai.provider = pid;
    snprintf(agent.cfg.ai.model, MAX_STR, "%s", model);
    snprintf(agent.cfg.ai.system_prompt, sizeof(agent.cfg.ai.system_prompt), "%s", prompt);

    /* simple split on ',' */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", tools_csv);
    char *tok = strtok(buf, ",");
    agent.cfg.ai.tool_count = 0;
    while (tok && agent.cfg.ai.tool_count < MAX_TOOLS) {
        snprintf(agent.cfg.ai.tools[agent.cfg.ai.tool_count], MAX_STR, "%s", tok);
        agent.cfg.ai.tool_count++;
        tok = strtok(NULL, ",");
    }

    /* Allowlist for run_command (optional). Without it, run_command refuses
       EVERYTHING (fail-closed), so if the agent gets the run_command tool but no
       allowed command, we warn - otherwise it would look like "it does not work". */
    agent.cfg.ai.allowlist_count = 0;
    if (allowlist_csv && allowlist_csv[0]) {
        char abuf[1024];
        snprintf(abuf, sizeof(abuf), "%s", allowlist_csv);
        char *atok = strtok(abuf, ",");
        while (atok && agent.cfg.ai.allowlist_count < MAX_ALLOWLIST) {
            snprintf(agent.cfg.ai.run_command_allowlist[agent.cfg.ai.allowlist_count],
                     MAX_STR, "%s", atok);
            agent.cfg.ai.allowlist_count++;
            atok = strtok(NULL, ",");
        }
    }

    agent.cfg.ai.approval_count = 0;
    if (approval_csv && approval_csv[0]) {
        char pbuf[1024];
        snprintf(pbuf, sizeof(pbuf), "%s", approval_csv);
        char *ptok = strtok(pbuf, ",");
        while (ptok && agent.cfg.ai.approval_count < MAX_TOOLS) {
            snprintf(agent.cfg.ai.approval_tools[agent.cfg.ai.approval_count],
                     MAX_STR, "%s", ptok);
            agent.cfg.ai.approval_count++;
            ptok = strtok(NULL, ",");
        }
    }

    /*
     * Data request approval threshold, e.g. "500KB", "2MB", or "off".
     * -1 (not 0) is the "no flag given" default, so this agent inherits
     * PIXELGO_DATA_THRESHOLD (the global default) at check time instead of
     * being silently disabled - see agent.h for the full three-state
     * semantics (-1 / 0 / >0).
     */
    agent.cfg.ai.approve_data_threshold_bytes = -1;
    if (data_threshold_str && data_threshold_str[0]) {
        long bytes = 0;
        if (data_threshold_parse(data_threshold_str, &bytes)) {
            agent.cfg.ai.approve_data_threshold_bytes = bytes;
        } else {
            fprintf(stderr,
                    "Warning: could not parse --data-threshold '%s', ignoring - this "
                    "agent will fall back to PIXELGO_DATA_THRESHOLD if set "
                    "(expected e.g. \"500KB\", \"2MB\", or \"off\")\n", data_threshold_str);
        }
    }

    int has_run_command = 0;
    for (int i = 0; i < agent.cfg.ai.tool_count; i++)
        if (strcmp(agent.cfg.ai.tools[i], "run_command") == 0) has_run_command = 1;
    if (has_run_command && agent.cfg.ai.allowlist_count == 0)
        fprintf(stderr, "Warning: agent '%s' has the 'run_command' tool but "
                        "the allowlist is empty -> it will not be able to run any command.\n"
                        "  Pass the allowed commands as the last argument, e.g. \"gcc,make\"\n",
                agent_id);

    if (!workspace_add_agent(&ws, &agent)) {
        fprintf(stderr, "Error adding agent\n");
        return 1;
    }
    printf("AI agent '%s' added to workspace '%s', dir=%s, tools=%s\n",
           agent_id, ws_name, agent.dir, tools_csv);
    if (agent.cfg.ai.approval_count > 0)
        printf("  requires your approval for: %s\n", approval_csv);
    if (agent.cfg.ai.approve_data_threshold_bytes > 0)
        printf("  requires your approval for requests over %ld bytes\n",
               agent.cfg.ai.approve_data_threshold_bytes);
    else if (agent.cfg.ai.approve_data_threshold_bytes == 0)
        printf("  data-threshold: explicitly off (ignores PIXELGO_DATA_THRESHOLD if set)\n");
    return 0;
}

static int cmd_agent_run(const char *ws_name, const char *agent_id, const char *task) {
    char ws_path[MAX_PATH_LEN];
    snprintf(ws_path, MAX_PATH_LEN, "%s/%s", BASE_DIR, ws_name);

    workspace_t ws;
    if (!workspace_load(&ws, ws_path)) {
        fprintf(stderr, "Workspace '%s' does not exist\n", ws_name);
        return 1;
    }

    agent_t *agent = workspace_find_agent(&ws, agent_id);
    if (!agent) {
        fprintf(stderr, "Agent '%s' does not exist in workspace '%s'\n", agent_id, ws_name);
        return 1;
    }

    printf("Starting agent '%s' (type=%s)...\n", agent_id,
           agent->type == AGENT_TYPE_WORKER ? "worker" : "ai");

    /* Supervised run: workers are not restarted (max_restarts=0, a fixed command
       that fails will fail the same way); AI agents can be restarted once on a
       transient failure (e.g. a network outage caught after retries ran out). */
    int max_restarts = agent->type == AGENT_TYPE_AI ? 1 : 0;
    int rc = agent_run_supervised(agent, task, max_restarts);

    const char *st = "OK";
    if (agent->status == AGENT_TIMEOUT) st = "TIMEOUT";
    else if (agent->status != AGENT_STOPPED) st = "FAILED";
    printf("Agent '%s' finished, status=%s (exit=%d)\n", agent_id, st, agent->exit_code);
    return rc == 0 ? 0 : 1;
}

static int cmd_agent_list(const char *ws_name) {
    char ws_path[MAX_PATH_LEN];
    snprintf(ws_path, MAX_PATH_LEN, "%s/%s", BASE_DIR, ws_name);

    workspace_t ws;
    if (!workspace_load(&ws, ws_path)) {
        fprintf(stderr, "Workspace '%s' does not exist\n", ws_name);
        return 1;
    }

    printf("Workspace '%s' (%d agents):\n", ws.name, ws.agent_count);
    for (int i = 0; i < ws.agent_count; i++) {
        agent_t *a = &ws.agents[i];
        printf("  - %-20s type=%-8s dir=%s\n", a->id,
               a->type == AGENT_TYPE_WORKER ? "worker" : "ai", a->dir);
    }
    return 0;
}


/*
 * Interactive chat: a loop in the terminal where you converse with an agent.
 * Unlike 'agent run' (a single question, then the process dies), here the history
 * persists between messages - the model remembers what was discussed.
 *
 * The agent may or may not have tools:
 *   - without tools -> a pure conversational assistant
 *   - with tools    -> an assistant that can read files, run commands, etc.
 * No need for two mechanisms: it is the same agent, configured differently.
 *
 * We run IN THE CURRENT PROCESS (no fork): a chat is interactive, so it makes no
 * sense to isolate each message in a separate process. The sandbox applies at the
 * tool level anyway (every file operation goes through sandbox_resolve_path).
 */
static int cmd_agent_chat(const char *ws_name, const char *agent_id, int reset) {
    char ws_path[MAX_PATH_LEN];
    snprintf(ws_path, MAX_PATH_LEN, "%s/%s", BASE_DIR, ws_name);

    workspace_t ws;
    if (!workspace_load(&ws, ws_path)) {
        fprintf(stderr, "Workspace '%s' does not exist\n", ws_name);
        return 1;
    }

    agent_t *agent = workspace_find_agent(&ws, agent_id);
    if (!agent) {
        fprintf(stderr, "Agent '%s' does not exist in workspace '%s'\n", agent_id, ws_name);
        return 1;
    }
    if (agent->type != AGENT_TYPE_AI) {
        fprintf(stderr, "Agent '%s' is not an AI agent (chat requires an AI agent)\n", agent_id);
        return 1;
    }

    if (reset) {
        history_reset(agent);
        printf("(history cleared - new conversation)\n");
    }

    printf("Chat with '%s' (%s / %s)\n", agent->id,
           provider_to_string(agent->cfg.ai.provider), agent->cfg.ai.model);
    if (agent->cfg.ai.tool_count > 0) {
        printf("Tool-uri: ");
        for (int i = 0; i < agent->cfg.ai.tool_count; i++)
            printf("%s%s", agent->cfg.ai.tools[i],
                   i + 1 < agent->cfg.ai.tool_count ? ", " : "");
        printf("\n");
    }
    printf("Type your message. 'exit' or Ctrl+D to quit.\n\n");

    char line[8192];
    for (;;) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n(exit)\n");
            break;   /* Ctrl+D */
        }
        line[strcspn(line, "\n")] = 0;

        if (line[0] == 0) continue;                    /* empty line */
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;

        /* useful commands during the chat */
        if (strcmp(line, "/reset") == 0) {
            history_reset(agent);
            printf("(history cleared - new conversation)\n\n");
            continue;
        }

        /* persist_history=1 -> the model remembers the previous turns */
        if (ai_loop_run_ex(agent, line, 1) != 0)
            fprintf(stderr, "(error calling the model - see the log)\n");

        printf("\n");
    }
    return 0;
}


/* Starts the web interface. The server is in-process (no fork): it serves the
   frontend and the JSON API, calling exactly the same functions as the CLI. */
void web_handler(const http_req_t *req, http_res_t *res);   /* in web_api.c */

static int cmd_serve(int port, int as_daemon) {
    if (as_daemon) {
        /* daemon_start forks; the parent exits, the child continues here */
        if (daemon_start(port) != 0) return 1;
    } else {
        printf("Starting pixelgo web interface...\n");
    }

    /* We clean up old jobs (over 24h) at startup. */
    jobs_cleanup(24 * 3600);

    if (http_serve(port, web_handler) != 0) {
        fprintf(stderr, "Could not start the server on port %d\n", port);
        if (as_daemon) daemon_cleanup_pidfile();
        return 1;
    }
    return 0;
}

static int cmd_flow_run(const char *ws_name, const char *flow_file, const char *task) {
    char ws_path[MAX_PATH_LEN];
    snprintf(ws_path, MAX_PATH_LEN, "%s/%s", BASE_DIR, ws_name);

    workspace_t ws;
    if (!workspace_load(&ws, ws_path)) {
        fprintf(stderr, "Workspace '%s' does not exist\n", ws_name);
        return 1;
    }

    flow_graph_t g;
    if (!flow_load(&g, flow_file)) {
        fprintf(stderr, "Could not load graph '%s'\n", flow_file);
        return 1;
    }
    if (!flow_validate(&g, &ws)) {
        fprintf(stderr, "Graph '%s' is invalid against workspace '%s'\n",
                flow_file, ws_name);
        return 1;
    }

    printf("Running graph '%s' over workspace '%s' (entry=%s)...\n",
           flow_file, ws_name, g.entry);
    int rc = orchestrator_run(&ws, &g, task);
    printf("Graph finished, status=%s\n", rc == 0 ? "OK" : "FAILED");
    return rc == 0 ? 0 : 1;
}

static int cmd_flow_diagram(const char *flow_file) {
    flow_graph_t g;
    if (!flow_load(&g, flow_file)) {
        fprintf(stderr, "Could not load graph '%s'\n", flow_file);
        return 1;
    }
    char mermaid[8192];
    if (!flow_to_mermaid(&g, mermaid, sizeof(mermaid))) {
        fprintf(stderr, "Diagram did not fit in buffer\n");
        return 1;
    }
    printf("%s", mermaid);
    return 0;
}

/* pixelgo selfhost init [--ws <name>] [--src <dir>] [--provider <p>] [--model <m>] [--force] */
static int cmd_selfhost(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[2], "init") != 0) {
        fprintf(stderr, "Usage: pixelgo selfhost init [--ws <name>] [--src <dir>] "
                        "[--provider <p>] [--model <m>] [--force]\n");
        return 1;
    }

    selfhost_opts_t opts = {0};
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--ws") == 0 && i + 1 < argc)            opts.ws_name  = argv[++i];
        else if (strcmp(argv[i], "--src") == 0 && i + 1 < argc)      opts.src_dir  = argv[++i];
        else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) opts.provider = argv[++i];
        else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)    opts.model    = argv[++i];
        else if (strcmp(argv[i], "--force") == 0)                    opts.force    = 1;
        else {
            fprintf(stderr, "Unknown argument for selfhost init: '%s'\n", argv[i]);
            return 1;
        }
    }
    return selfhost_init(&opts);
}

int main(int argc, char **argv) {
    /* We load .env BEFORE anything else: the API keys and PIXELGO_LOG_LEVEL can
       come from there. It is done here, in the parent, so the environment is
       inherited by everything that follows: the daemon, the HTTP handlers, and the
       agents' jobs (fork). */
    envfile_load();

    log_init();
    if (argc < 2) { print_usage(); return 1; }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("pixelgo %s\n", PIXELGO_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "workspace") == 0 && argc >= 3) {
        if (strcmp(argv[2], "list") == 0) {
            return cmd_workspace_list();
        }
        if (argc >= 4 && strcmp(argv[2], "create") == 0) {
            return cmd_workspace_create(argv[3]);
        }
    }

    if (strcmp(argv[1], "agent") == 0 && argc >= 3) {
        if (strcmp(argv[2], "add") == 0 && argc >= 6) {
            if (strcmp(argv[3], "worker") == 0 && argc >= 7) {
                return cmd_agent_add_worker(argv[4], argv[5], argc, argv, 6);
            }
            if (strcmp(argv[3], "ai") == 0 && argc >= 10) {
                /* Optional trailing args: the run_command allowlist (positional,
                   kept for compatibility), --approve <tools> which makes the
                   agent ask before those tools run, and --data-threshold <size>
                   which makes it ask before sending an oversized request to the
                   provider (e.g. "500KB", "2MB"). */
                const char *allow = NULL;
                const char *approve = NULL;
                const char *data_threshold = NULL;
                for (int i = 10; i < argc; i++) {
                    if (strcmp(argv[i], "--approve") == 0 && i + 1 < argc) {
                        approve = argv[++i];
                    } else if (strcmp(argv[i], "--data-threshold") == 0 && i + 1 < argc) {
                        data_threshold = argv[++i];
                    } else if (!allow) {
                        allow = argv[i];
                    }
                }
                return cmd_agent_add_ai(argv[4], argv[5], argv[6], argv[7], argv[8],
                                        argv[9], allow, approve, data_threshold);
            }
        }
        if (strcmp(argv[2], "run") == 0 && argc >= 5) {
            const char *task = argc >= 6 ? argv[5] : NULL;
            return cmd_agent_run(argv[3], argv[4], task);
        }
        if (strcmp(argv[2], "chat") == 0 && argc >= 5) {
            int reset = (argc >= 6 && strcmp(argv[5], "--reset") == 0);
            return cmd_agent_chat(argv[3], argv[4], reset);
        }
        if (strcmp(argv[2], "list") == 0 && argc >= 4) {
            return cmd_agent_list(argv[3]);
        }
    }

    if (strcmp(argv[1], "keys") == 0) {
        return cmd_keys();
    }

    if (strcmp(argv[1], "selfhost") == 0) {
        return cmd_selfhost(argc, argv);
    }

    if (strcmp(argv[1], "serve") == 0) {
        /* daemon subcommands */
        if (argc >= 3 && strcmp(argv[2], "stop") == 0)
            return daemon_stop() == 0 ? 0 : 1;
        if (argc >= 3 && strcmp(argv[2], "status") == 0)
            return daemon_status() ? 0 : 1;

        /* serve [--daemon] [port] - the arguments can come in any order */
        int as_daemon = 0, port = 8080;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--daemon") == 0 || strcmp(argv[i], "-d") == 0)
                as_daemon = 1;
            else {
                int p = atoi(argv[i]);
                if (p > 0 && p <= 65535) port = p;
            }
        }
        return cmd_serve(port, as_daemon);
    }

    if (strcmp(argv[1], "flow") == 0 && argc >= 3) {
        if (strcmp(argv[2], "run") == 0 && argc >= 5) {
            const char *task = argc >= 6 ? argv[5] : NULL;
            return cmd_flow_run(argv[3], argv[4], task);
        }
        if (strcmp(argv[2], "diagram") == 0 && argc >= 4) {
            return cmd_flow_diagram(argv[3]);
        }
    }

    print_usage();
    return 1;
}
