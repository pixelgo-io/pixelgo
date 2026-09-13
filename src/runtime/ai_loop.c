#include "ai_loop.h"
#include "provider.h"
#include "tools.h"
#include "tool_defs.h"
#include "json_util.h"
#include "history.h"
#include "usage.h"
#include "events.h"
#include "log.h"
#include "data_request_audit.h"
#include "http_server.h"   /* for HTTP_MAX_BODY_CEILING - see the edited_text
                               buffer sizing in the data-approval gateway below */
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The complete AI runtime.
 *
 * The conversation history is kept as a cJSON "messages" array, built
 * incrementally and serialized on each turn. This way escaping is always correct
 * (tasks with quotes, multi-line tool output, etc.) and the format follows the
 * Anthropic Messages API protocol exactly:
 *
 *   user     -> { role:"user", content:[{type:"text", text:...}] }
 *   assistant-> the model's response, sent back in full (content[] with text
 *               and/or tool_use blocks) - required in order to keep the tool_use_ids
 *   user     -> { role:"user", content:[{type:"tool_result", tool_use_id:...,
 *               content:...}] } - the results of the requested tools
 *
 * The loop continues as long as stop_reason == "tool_use". It stops at
 * "end_turn", on error, or when the iteration limit is reached.
 */

/*
 * Asks the human to approve a tool call before it runs.
 *
 * Only reached for tools listed in the agent's approval_tools. Reading stdin
 * requires a terminal, so when there is none (the agent runs as a job from the
 * web server, or output is piped) we DENY rather than silently allowing: an
 * agent configured to need approval must never act unsupervised just because
 * nobody could be asked.
 *
 * Returns 1 to approve, 0 to deny.
 */
/*
 * Terminal ownership while asking, kept in file scope so a signal handler can
 * put it back. If the agent dies while holding the foreground (Ctrl+C at the
 * prompt), the terminal would be assigned to a process group that no longer
 * exists - the shell never gets it back and the session appears frozen.
 */
static volatile sig_atomic_t g_tty_taken = 0;
static pid_t g_tty_owner = -1;

static void restore_terminal(void) {
    if (g_tty_taken && g_tty_owner != -1) {
        signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(STDIN_FILENO, g_tty_owner);
        g_tty_taken = 0;
    }
}

static void approval_signal_handler(int sig) {
    restore_terminal();
    /* Re-raise with the default action so the exit status is honest: the agent
       really was interrupted, and the orchestrator should see that. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static int ask_approval(const char *agent_id, const char *tool_name,
                        const char *summary) {
    /*
     * Two channels, and the ORDER matters:
     *   1. a web job  -> ask in the browser
     *   2. a terminal -> ask on stdin
     *
     * The web channel comes first because it is the more specific signal: it
     * exists only when the server started this agent, and then the person is
     * watching the graph, not the terminal. Checking isatty() first would send
     * the question to whatever console launched the server, where nobody is
     * looking - the browser would just sit there.
     *
     * If neither is available we DENY: an agent configured to need approval
     * must never act unsupervised just because nobody could be asked.
     */
    if (approval_channel_available())
        return approval_wait(agent_id, tool_name, summary);

    if (!isatty(STDIN_FILENO)) {
        LOG_W("approval: no terminal and no web channel, denying '%s' for agent '%s'",
              tool_name, agent_id);
        return 0;
    }

    /*
     * The agent runs in its own process group (agent_start calls setpgid so a
     * timeout can kill the whole subtree). A process that is not in the
     * terminal's FOREGROUND group cannot read stdin: the kernel stops it with
     * SIGTTIN, and the prompt would hang forever after being printed.
     *
     * So we claim the foreground for the duration of the question, then hand it
     * back. SIGTTOU must be ignored around tcsetpgrp, otherwise the very act of
     * claiming it suspends us.
     */
    pid_t my_pgrp = getpgrp();
    pid_t old_pgrp = tcgetpgrp(STDIN_FILENO);
    struct sigaction ign, old_ttou;
    struct sigaction sa, old_int, old_term, old_quit, old_hup;

    if (old_pgrp != -1 && old_pgrp != my_pgrp) {
        memset(&ign, 0, sizeof(ign));
        ign.sa_handler = SIG_IGN;
        sigaction(SIGTTOU, &ign, &old_ttou);
        if (tcsetpgrp(STDIN_FILENO, my_pgrp) == 0) {
            g_tty_taken = 1;
            g_tty_owner = old_pgrp;
        }
    }

    /* With the terminal in hand, Ctrl+C now reaches US, not the shell. Catch it
       so we hand the terminal back before dying. */
    if (g_tty_taken) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = approval_signal_handler;
        sigaction(SIGINT,  &sa, &old_int);
        sigaction(SIGTERM, &sa, &old_term);
        sigaction(SIGQUIT, &sa, &old_quit);
        sigaction(SIGHUP,  &sa, &old_hup);
    }

    printf("\n");
    printf("  ┌─ approval needed ─────────────────────────────\n");
    printf("  │ agent: %s\n", agent_id);
    printf("  │ tool:  %s\n", tool_name);
    printf("  │ %s\n", summary);
    printf("  └───────────────────────────────────────────────\n");
    printf("  allow? [y/N] ");
    fflush(stdout);

    char line[32];
    int yes = 0;
    if (fgets(line, sizeof(line), stdin)) {
        yes = (line[0] == 'y' || line[0] == 'Y');
        printf("  %s\n\n", yes ? "-> allowed" : "-> denied");
    } else {
        printf("\n");   /* EOF (Ctrl+D) = deny */
    }
    fflush(stdout);

    if (g_tty_taken) {
        sigaction(SIGINT,  &old_int,  NULL);
        sigaction(SIGTERM, &old_term, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        sigaction(SIGHUP,  &old_hup,  NULL);
        restore_terminal();
        sigaction(SIGTTOU, &old_ttou, NULL);
    }
    return yes;
}


/*
 * A readable summary of a tool's arguments, for the live journal and the
 * approval prompt. The raw JSON would be unreadable; we want "src/calc.c" not
 * {"path":"src/calc.c"}.
 */
static void summarize_tool_input(const char *tool, const cJSON *input,
                                 char *out, size_t out_size) {
    out[0] = 0;
    if (!input) return;

    if (strcmp(tool, "run_command") == 0) {
        /* the command + its arguments, as a shell line */
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(input, "command");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(input, "args");

        size_t off = 0;
        if (cJSON_IsString(cmd)) {
            int n = snprintf(out, out_size, "%s", cmd->valuestring);
            if (n > 0) off = (size_t)n;
        }
        if (cJSON_IsArray(args)) {
            const cJSON *a = NULL;
            cJSON_ArrayForEach(a, args) {
                if (!cJSON_IsString(a) || off >= out_size - 2) break;
                int n = snprintf(out + off, out_size - off, " %s", a->valuestring);
                if (n < 0) break;
                off += (size_t)n;
            }
        }
        return;
    }

    /* for the rest: path / pattern, whichever is more relevant */
    const char *keys[] = { "path", "pattern", "command" };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(input, keys[k]);
        if (cJSON_IsString(v) && v->valuestring[0]) {
            snprintf(out, out_size, "%s", v->valuestring);
            return;
        }
    }
}

/* Builds the array of tool definitions allowed for the agent, as a cJSON node.
   We skip tools without a definition (typos/unknown) - the model never sees them. */
static cJSON *build_tools_array(agent_t *agent) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    for (int i = 0; i < agent->cfg.ai.tool_count; i++) {
        const char *def = tool_def_json(agent->cfg.ai.tools[i]);
        if (!def) {
            LOG_W("ai_loop: tool '%s' from config has no definition, skipping it",
                  agent->cfg.ai.tools[i]);
            continue;
        }
        cJSON *tool = cJSON_Parse(def);
        if (!tool) {
            LOG_W("ai_loop: definition of tool '%s' is invalid JSON, skipping it",
                  agent->cfg.ai.tools[i]);
            continue;
        }
        cJSON_AddItemToArray(arr, tool);
    }
    return arr;
}

/* Adds a simple user message (a single text block) to the history. */
static int history_add_user_text(cJSON *messages, const char *text) {
    cJSON *msg = cJSON_CreateObject();
    if (!msg) return 0;
    cJSON_AddStringToObject(msg, "role", "user");

    cJSON *content = cJSON_CreateArray();
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", text);
    cJSON_AddItemToArray(content, block);
    cJSON_AddItemToObject(msg, "content", content);

    cJSON_AddItemToArray(messages, msg);
    return 1;
}

/* Adds the assistant message received from the API (content[] sent back in full).
   We take ONLY the content array, under a message with role=assistant. */
static int history_add_assistant(cJSON *messages, const cJSON *content_array) {
    cJSON *msg = cJSON_CreateObject();
    if (!msg) return 0;
    cJSON_AddStringToObject(msg, "role", "assistant");

    cJSON *content_copy = cJSON_Duplicate(content_array, 1);
    if (!content_copy) { cJSON_Delete(msg); return 0; }
    cJSON_AddItemToObject(msg, "content", content_copy);

    cJSON_AddItemToArray(messages, msg);
    return 1;
}

/* Builds a tool_result block for a given tool_use and adds it to `results`
   (an array of content blocks that will become a user message's content). */
static void append_tool_result(cJSON *results, const char *tool_use_id,
                               const tool_result_t *tr) {
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "tool_result");
    cJSON_AddStringToObject(block, "tool_use_id", tool_use_id);
    cJSON_AddStringToObject(block, "content", tr->output);
    if (!tr->ok) cJSON_AddBoolToObject(block, "is_error", 1);
    cJSON_AddItemToArray(results, block);
}

/* Extracts and logs the final text (the type=text blocks) from a content[]. */
static void print_final_text(agent_t *agent, const cJSON *content_array,
                             char *capture, size_t capture_size) {
    char text[TOOL_RESULT_MAX];
    text[0] = 0;
    size_t off = 0;

    const cJSON *block = NULL;
    cJSON_ArrayForEach(block, content_array) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(block, "text");
            if (cJSON_IsString(t) && t->valuestring) {
                int n = snprintf(text + off, sizeof(text) - off, "%s", t->valuestring);
                if (n > 0) off += (size_t)n;
                if (off >= sizeof(text) - 1) break;
            }
        }
    }
    LOG_I("ai_loop: agent=%s finished. Final response: %s", agent->id, text);

    /* If the caller wants the response back (web API), we copy it. Otherwise we
       print it to stdout (CLI). */
    if (capture && capture_size > 0)
        snprintf(capture, capture_size, "%s", text);
    else {
        printf("%s\n", text);
        fflush(stdout);
    }

    /* The live journal: what the agent "says". */
    event_agent_says(agent->id, text);

    /* We persist the final response to <agent_dir>/_output.txt, so the orchestrator
       can read it and pass it to the next node in the graph. The agent already runs
       with chdir into its own dir, so a relative path is enough. */
    char outpath[MAX_PATH_LEN + 24];
    snprintf(outpath, sizeof(outpath), "%s/_output.txt", agent->dir);
    FILE *of = fopen(outpath, "w");
    if (of) {
        fputs(text, of);
        fclose(of);
    } else {
        LOG_W("ai_loop: could not write _output.txt for agent=%s", agent->id);
    }
}

static int ai_loop_run_internal(agent_t *agent, const char *task, int persist_history,
                                char *capture, size_t capture_size) {
    if (agent->type != AGENT_TYPE_AI) {
        LOG_E("ai_loop: agent '%s' is not an AI agent", agent->id);
        return -1;
    }

    LOG_I("ai_loop: starting AI agent '%s' (model=%s), task=%.120s",
          agent->id, agent->cfg.ai.model, task ? task : "(null)");

    /* The live journal: the server passed us the path via PIXELGO_EVENTS_FILE.
       If it is missing (CLI run), the events become no-ops. */
    events_open_from_env();
    events_set_node(agent->id);

    /* If _input.txt exists (a message from the previous node in the graph), we
       combine it with the base task. This way an agent receives both its fixed
       instruction and the previous agent's output. The file is in the current dir
       (the agent runs with chdir into its sandbox). */
    char effective_task[16384];
    effective_task[0] = 0;
    {
        char inpath[MAX_PATH_LEN + 24];
        snprintf(inpath, sizeof(inpath), "%s/_input.txt", agent->dir);
        FILE *inf = fopen(inpath, "r");
        if (inf) {
            char incoming[8192];
            size_t rn = fread(incoming, 1, sizeof(incoming) - 1, inf);
            incoming[rn] = 0;
            fclose(inf);
            if (task && task[0])
                snprintf(effective_task, sizeof(effective_task),
                         "%s\n\n--- Message from the previous agent ---\n%s",
                         task, incoming);
            else
                snprintf(effective_task, sizeof(effective_task), "%s", incoming);
            LOG_I("ai_loop: agent=%s received input from the previous node (%zu bytes)",
                  agent->id, rn);
        } else {
            snprintf(effective_task, sizeof(effective_task), "%s", task ? task : "");
        }
    }

    /* The conversation history.
       - persist_history=1 (chat): we load the previous turns from disk, so the
         model remembers what was discussed.
       - persist_history=0 (one-shot / graph node): we start clean. */
    cJSON *messages = persist_history ? history_load(agent) : cJSON_CreateArray();
    if (!messages) {
        LOG_E("ai_loop: history allocation failed");
        return -1;
    }

    /* We trim the old messages if the history has grown too much - otherwise we
       exceed the model's context limit and the API errors out. */
    if (persist_history)
        history_trim(messages, HISTORY_MAX_MESSAGES);

    history_add_user_text(messages, effective_task);

    /* Tool definitions - built once, kept as a cJSON node (the provider layer
       translates them into each API's format). */
    cJSON *tools = build_tools_array(agent);
    if (!tools) {
        LOG_E("ai_loop: building tool definitions failed");
        cJSON_Delete(messages);
        return -1;
    }

    int rc = -1;

    /* We accumulate usage over the whole loop (an agent can make several calls:
       tool_use -> result -> tool_use -> ... -> final response). */
    usage_t total_usage = {0};

    const int max_iterations = ai_loop_max_iterations();

    for (int iter = 0; iter < max_iterations; iter++) {
        LOG_I("ai_loop: agent=%s iteration=%d/%d -> call %s (%s)",
              agent->id, iter + 1, max_iterations,
              provider_to_string(agent->cfg.ai.provider), agent->cfg.ai.model);

        event_agent_thinking(agent->id, iter + 1);

        /*
         * Data Request Approval Gateway.
         *
         * Before the request goes out, check its size against the agent's
         * configured threshold (0 = feature disabled, the default).
         *
         * Shows the human the ACTUAL request about to go out - system
         * prompt, model, the full `messages` history (every turn, every
         * tool result, up to and including whatever just pushed this over
         * threshold), and any tool schemas - as real, pretty-printed JSON.
         * This is pixelgo's own internal ("neutral") request shape, not a
         * summary or a single block picked out of context: for Anthropic
         * agents it IS the exact body that gets sent (the neutral format
         * was deliberately chosen to match Anthropic's Messages API almost
         * field-for-field - see providers/anthropic.c). For OpenAI/Gemini
         * agents, the adapter translates this same content into that
         * provider's own wire shape (different field names, different
         * tool-call encoding) immediately before sending - what's shown
         * here is the same conversation, not yet translated, so the human
         * can review and trim it in one common format regardless of which
         * provider the agent uses.
         *
         * On "Send trimmed", the edited JSON is parsed back and its
         * "messages" array WHOLESALE REPLACES the live `messages` array -
         * not a single field patched in place. This is deliberate: the
         * whole point of showing the full conversation is letting the
         * human delete entire turns they don't want sent (an old tool
         * result that turned out to be noise, a redundant exchange), not
         * just trim text within one block. If the edited text fails to
         * parse as JSON, or has no "messages" array, the request is DENIED
         * outright rather than guessing what the human meant or silently
         * falling back to the untouched original - a request this
         * malformed cannot safely be sent to the provider either way, so
         * failing closed here costs nothing.
         *
         * The edit channel (data_edit_wait) needs a web job; when it is not
         * available (CLI, or a non-interactive job) this falls back to the
         * plain approve/deny channel already used for tool calls
         * (ask_approval) - same summary, no editing, but never silently
         * skips the check.
         *
         * approve_data_threshold_bytes is THREE-state (see agent.h): -1
         * means this agent didn't set one, so it inherits
         * PIXELGO_DATA_THRESHOLD (the global default) here, at the point of
         * the check - not cached at startup, same convention as the other
         * PIXELGO_* runtime knobs. 0 means explicitly off, which must WIN
         * over the global default (an agent that opted out stays opted
         * out), not fall through to it.
         */
        long effective_threshold = (agent->cfg.ai.approve_data_threshold_bytes >= 0)
            ? agent->cfg.ai.approve_data_threshold_bytes
            : data_threshold_global();

        if (effective_threshold > 0) {
            char *msg_json = cJSON_PrintUnformatted(messages);
            size_t payload_bytes = strlen(agent->cfg.ai.system_prompt) +
                                    (msg_json ? strlen(msg_json) : 0);
            if (msg_json) free(msg_json);

            if (payload_bytes > (size_t)effective_threshold) {
                long tokens_est = data_audit_estimate_tokens(payload_bytes);
                long cost_micro = usage_cost_micro(agent->cfg.ai.provider,
                                                   agent->cfg.ai.model,
                                                   tokens_est, 0);

                char summary[256];
                data_audit_build_summary(agent->cfg.ai.provider, agent->cfg.ai.model,
                                         payload_bytes, tokens_est, cost_micro,
                                         summary, sizeof(summary));

                LOG_I("ai_loop: agent=%s data request needs approval: %s",
                      agent->id, summary);

                /* Build the real request preview: same shape as the actual
                   Anthropic body (providers/anthropic.c), pretty-printed so
                   a human can read it, not the compact form used for
                   payload_bytes above. */
                cJSON *preview = cJSON_CreateObject();
                cJSON_AddStringToObject(preview, "model", agent->cfg.ai.model);
                if (agent->cfg.ai.system_prompt[0])
                    cJSON_AddStringToObject(preview, "system", agent->cfg.ai.system_prompt);
                cJSON_AddItemToObject(preview, "messages", cJSON_Duplicate(messages, 1));
                if (tools && cJSON_GetArraySize(tools) > 0)
                    cJSON_AddItemToObject(preview, "tools", cJSON_Duplicate(tools, 1));

                char *preview_text = cJSON_Print(preview);   /* pretty-printed, not compact */
                cJSON_Delete(preview);

                int allowed;
                int edited = 0;
                /* Sized to the HARD ceiling (not whatever PIXELGO_HTTP_MAX_BODY
                   is currently set to), so a runtime env change can never
                   make this smaller than what the network layer could
                   actually deliver - static, so it costs nothing on the
                   stack (see http_server.c's read_request for the same
                   reasoning applied to the HTTP layer itself). */
                static char edited_text[HTTP_MAX_BODY_CEILING + 1];
                edited_text[0] = 0;

                if (preview_text && data_edit_channel_available()) {
                    allowed = data_edit_wait(agent->id, "send request to AI",
                                             preview_text,
                                             edited_text, sizeof(edited_text),
                                             &edited);
                } else {
                    /* No editable channel (CLI, or preview build failed):
                       fall back to the plain approve/deny already used for
                       tools. */
                    allowed = ask_approval(agent->id, "send request to AI", summary);
                }
                if (preview_text) cJSON_free(preview_text);

                if (allowed && edited) {
                    /* Parse the edited JSON back and replace the live
                       `messages` array's CONTENTS in place (not the
                       pointer itself - it's the same array object
                       history_save() writes out at the end of the run, and
                       ai_loop keeps iterating on it). A human deleting a
                       whole turn from the JSON simply means that message
                       object is absent from the parsed array - nothing
                       special needed to "delete" it here, it's just not
                       re-added below. */
                    cJSON *edited_root = cJSON_Parse(edited_text);
                    cJSON *edited_messages = edited_root
                        ? cJSON_DetachItemFromObjectCaseSensitive(edited_root, "messages")
                        : NULL;

                    if (!cJSON_IsArray(edited_messages)) {
                        LOG_W("ai_loop: agent=%s edited request is not valid JSON "
                              "with a \"messages\" array - denying rather than "
                              "sending something malformed or silently ignoring "
                              "the edit", agent->id);
                        allowed = 0;
                    } else {
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
                    if (edited_messages) cJSON_Delete(edited_messages);  /* now empty either way */
                    if (edited_root) cJSON_Delete(edited_root);
                }
                /* allowed && !edited: "approve unchanged" - messages already
                   holds the right content, nothing to do. */

                data_audit_log_decision(NULL, agent->id, payload_bytes,
                                        tokens_est, cost_micro,
                                        allowed ? "approved" : "denied");

                if (!allowed) {
                    LOG_W("ai_loop: agent=%s data request denied, stopping",
                          agent->id);
                    break;
                }
            }
        }

        /* The call goes through the provider layer: ai_loop does not know whether
           Anthropic, OpenAI or Gemini is underneath. The response arrives already
           NORMALIZED in the neutral format (content[] with text/tool_use blocks). */
        provider_response_t presp;
        if (provider_call(agent, messages, tools, &presp) != 0) {
            LOG_E("ai_loop: provider call failed: %.300s", presp.error);
            provider_response_free(&presp);
            break;
        }

        /* Add up the tokens for this turn. */
        usage_add(&total_usage, &presp.usage);

        cJSON *content = presp.content;
        if (!cJSON_IsArray(content)) {
            LOG_E("ai_loop: normalized response without content[]");
            provider_response_free(&presp);
            break;
        }

        /* We save the full assistant response in the history (preserves tool_use_id). */
        history_add_assistant(messages, content);

        const char *stop = presp.stop_reason;

        if (strcmp(stop, "tool_use") != 0) {
            /* Final turn (end_turn). We display the text and persist _output.txt. */
            print_final_text(agent, content, capture, capture_size);
            provider_response_free(&presp);
            rc = 0;
            break;
        }

        /* The model requests one or more tools. We execute them all and gather the
           tool_results into a single user message (an API requirement). */
        cJSON *results = cJSON_CreateArray();
        int tool_calls = 0;

        cJSON *block = NULL;
        cJSON_ArrayForEach(block, content) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
            if (!cJSON_IsString(type) || strcmp(type->valuestring, "tool_use") != 0)
                continue;

            const cJSON *name = cJSON_GetObjectItemCaseSensitive(block, "name");
            const cJSON *id   = cJSON_GetObjectItemCaseSensitive(block, "id");
            const cJSON *input = cJSON_GetObjectItemCaseSensitive(block, "input");

            const char *tool_name = cJSON_IsString(name) ? name->valuestring : "";
            const char *tool_id   = cJSON_IsString(id)   ? id->valuestring   : "";

            /* We re-serialize the input (a nested object) as a JSON string for dispatch. */
            char input_json[TOOL_RESULT_MAX];
            if (!input || !json_serialize(input, input_json, sizeof(input_json)))
                snprintf(input_json, sizeof(input_json), "{}");

            LOG_I("ai_loop: agent=%s requests tool='%s' (id=%s)",
                  agent->id, tool_name, tool_id);

            /* A short summary of the arguments, for the live journal.
               We do not send the whole JSON - it would be unreadable. */
            char arg_summary[256];
            summarize_tool_input(tool_name, input, arg_summary, sizeof(arg_summary));
            event_tool_call(agent->id, tool_name, arg_summary);

            tool_result_t tr;
            if (tool_needs_approval(agent, tool_name) &&
                !ask_approval(agent->id, tool_name, arg_summary)) {
                /* Denied: we do not run the tool, but we must still return a
                   tool_result - the protocol requires one per tool_use, and
                   without it the next API call is rejected. Telling the model
                   it was denied also lets it try a different approach. */
                tr = tool_err("denied by the human reviewer");
                LOG_W("approval: agent=%s denied tool='%s'", agent->id, tool_name);
            } else {
                tr = tool_dispatch(agent, tool_name, input_json);
            }
            LOG_I("ai_loop: tool result '%s' ok=%d output=%.200s",
                  tool_name, tr.ok, tr.output);

            event_tool_result(agent->id, tool_name, tr.ok,
                              tr.output, (long)strlen(tr.output));

            append_tool_result(results, tool_id, &tr);
            tool_calls++;
        }

        if (tool_calls == 0) {
            /* stop_reason said tool_use but we found no blocks - an abnormal
               situation; we avoid an infinite loop. */
            LOG_W("ai_loop: stop_reason=tool_use but no tool_use block found");
            cJSON_Delete(results);
            provider_response_free(&presp);
            break;
        }

        /* We add the user message with all the tool_results to the history. */
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddItemToObject(user_msg, "content", results);
        cJSON_AddItemToArray(messages, user_msg);

        provider_response_free(&presp);
    }

    if (rc != 0)
        LOG_E("ai_loop: agent=%s stopped without a final response "
              "(provider error, or the %d-iteration limit was reached - "
              "raise it with PIXELGO_MAX_ITERATIONS)",
              agent->id, max_iterations);

    /* We report usage: how many tokens, how much it cost. This is the number that
       supports the argument "the cheap model does the volume, the expensive one only
       the reasoning". */
    {
        char cost[24];
        usage_format_cost(total_usage.cost_micro_usd, cost, sizeof(cost));
        if (total_usage.cache_write_tokens || total_usage.cache_read_tokens) {
            LOG_I("usage: agent=%s model=%s in=%ld out=%ld cache_write=%ld cache_read=%ld cost=%s",
                  agent->id, agent->cfg.ai.model,
                  total_usage.input_tokens, total_usage.output_tokens,
                  total_usage.cache_write_tokens, total_usage.cache_read_tokens, cost);
        } else {
            LOG_I("usage: agent=%s model=%s in=%ld out=%ld cost=%s",
                  agent->id, agent->cfg.ai.model,
                  total_usage.input_tokens, total_usage.output_tokens, cost);
        }

        /* We persist to <agent_dir>/_usage.json, so the orchestrator and the web can
           sum usage across the whole graph. */
        char upath[MAX_PATH_LEN + 24];
        snprintf(upath, sizeof(upath), "%s/_usage.json", agent->dir);
        FILE *uf = fopen(upath, "w");
        if (uf) {
            fprintf(uf,
                "{\"agent\":\"%s\",\"provider\":\"%s\",\"model\":\"%s\","
                "\"input_tokens\":%ld,\"output_tokens\":%ld,"
                "\"cache_write_tokens\":%ld,\"cache_read_tokens\":%ld,"
                "\"cost_micro_usd\":%ld}\n",
                agent->id, provider_to_string(agent->cfg.ai.provider),
                agent->cfg.ai.model,
                total_usage.input_tokens, total_usage.output_tokens,
                total_usage.cache_write_tokens, total_usage.cache_read_tokens,
                total_usage.cost_micro_usd);
            fclose(uf);
        }
    }

    /* We save the conversation so it is available on the next run. We save on
       error too: partial context is more useful than nothing, and the user can
       resume where they left off. */
    if (persist_history)
        history_save(agent, messages);

    cJSON_Delete(tools);
    cJSON_Delete(messages);
    return rc;
}

/* The classic variant: one-shot, without persistent history, output to stdout. */
/*
 * The iteration ceiling, overridable at runtime.
 *
 * Clamped to a sane range: zero or negative would stop the agent before it did
 * anything, and an unbounded value turns a stuck model into an unbounded bill.
 */
int ai_loop_max_iterations(void) {
    const char *env = getenv("PIXELGO_MAX_ITERATIONS");
    if (!env || !env[0]) return AI_LOOP_MAX_ITERATIONS;

    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end == env || v < 1) {
        LOG_W("PIXELGO_MAX_ITERATIONS='%s' is not a positive number, using %d",
              env, AI_LOOP_MAX_ITERATIONS);
        return AI_LOOP_MAX_ITERATIONS;
    }
    if (v > 500) v = 500;
    return (int)v;
}

int ai_loop_run(agent_t *agent, const char *task) {
    return ai_loop_run_internal(agent, task, 0, NULL, 0);
}

/* With control over the history; output to stdout (CLI). */
int ai_loop_run_ex(agent_t *agent, const char *task, int persist_history) {
    return ai_loop_run_internal(agent, task, persist_history, NULL, 0);
}

/* With the response returned in a buffer (web API). */
int ai_loop_run_capture(agent_t *agent, const char *task, int persist_history,
                        char *out, size_t out_size) {
    if (out && out_size > 0) out[0] = 0;
    return ai_loop_run_internal(agent, task, persist_history, out, out_size);
}
