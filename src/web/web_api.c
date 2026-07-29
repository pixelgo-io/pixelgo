#include "http_server.h"
#include "workspace.h"
#include "agent_run.h"
#include "ai_loop.h"
#include "history.h"
#include "provider.h"
#include "tools.h"
#include "flow.h"
#include "orchestrator.h"
#include "jobs.h"
#include "events.h"
#include "json_util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/*
 * The web API. Each route calls EXACTLY the same functions as the CLI - we do
 * not duplicate logic. The web is just another "front-end" over the same core.
 *
 * Routes:
 *   GET  /                       -> the frontend (HTML)
 *   GET  /api/workspaces         -> the list of workspaces
 *   POST /api/workspaces         -> creates a workspace    {name}
 *   GET  /api/agents?ws=X        -> the agents in a workspace
 *   POST /api/agents             -> creates an agent       {ws, id, type, ...}
 *   POST /api/chat               -> sends a message        {ws, agent, message}
 *   GET  /api/history?ws=X&agent=Y -> the conversation history
 *   POST /api/history/reset      -> clears the history     {ws, agent}
 *   GET  /api/flows              -> the .flow files in examples/
 *   POST /api/flow/run           -> runs a graph           {ws, flow, task}
 *   POST /api/flow/diagram       -> Mermaid diagram        {flow}
 *   POST /api/flows              -> saves a graph          {name, content}
 */

#define BASE_DIR "workspaces"

extern const char *WEB_INDEX_HTML;   /* in web_assets.c */

/* --- helper-e --- */

/* Gets a string field from the request's JSON body. Returns 1 on success. */
static int body_get_string(const http_req_t *req, const char *key,
                           char *out, size_t out_size) {
    out[0] = 0;
    cJSON *root = cJSON_Parse(req->body);
    if (!root) return 0;
    int ok = json_get_string(root, key, out, out_size);
    cJSON_Delete(root);
    return ok;
}

/* Loads a workspace by name. Returns 1 on success. */
static int load_ws(const char *name, workspace_t *ws) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", BASE_DIR, name);
    return workspace_load(ws, path);
}

/* --- routes: workspaces --- */

static void route_workspaces_list(http_res_t *res) {
    cJSON *arr = cJSON_CreateArray();

    DIR *d = opendir(BASE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;

            char conf[MAX_PATH_LEN * 2];
            snprintf(conf, sizeof(conf), "%s/%s/workspace.conf", BASE_DIR, e->d_name);
            struct stat st;
            if (stat(conf, &st) != 0) continue;

            workspace_t ws;
            if (!load_ws(e->d_name, &ws)) continue;

            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", ws.name);
            cJSON_AddNumberToObject(o, "agent_count", ws.agent_count);
            cJSON_AddItemToArray(arr, o);
        }
        closedir(d);
    }

    char out[16384];
    if (!json_serialize(arr, out, sizeof(out)))
        snprintf(out, sizeof(out), "[]");
    cJSON_Delete(arr);
    http_res_json(res, 200, out);
}

static void route_workspace_create(const http_req_t *req, http_res_t *res) {
    char name[MAX_STR];
    if (!body_get_string(req, "name", name, sizeof(name)) || name[0] == 0) {
        http_res_error(res, 400, "missing the 'name' field");
        return;
    }
    /* safe name: no '/' or '..' (would escape BASE_DIR) */
    if (strchr(name, '/') || strstr(name, "..")) {
        http_res_error(res, 400, "invalid workspace name");
        return;
    }

    workspace_t ws;
    if (!workspace_create(&ws, name, BASE_DIR)) {
        http_res_error(res, 500, "could not create the workspace");
        return;
    }
    http_res_json(res, 201, "{\"ok\":true}");
}

/* --- routes: agents --- */

static void route_agents_list(const http_req_t *req, http_res_t *res) {
    char wsname[MAX_STR];
    if (!http_query_get(req->query, "ws", wsname, sizeof(wsname))) {
        http_res_error(res, 400, "missing the 'ws' parameter");
        return;
    }

    workspace_t ws;
    if (!load_ws(wsname, &ws)) {
        http_res_error(res, 404, "no such workspace");
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < ws.agent_count; i++) {
        agent_t *a = &ws.agents[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", a->id);
        cJSON_AddStringToObject(o, "type",
            a->type == AGENT_TYPE_AI ? "ai" : "worker");

        if (a->type == AGENT_TYPE_AI) {
            cJSON_AddStringToObject(o, "provider", provider_to_string(a->cfg.ai.provider));
            cJSON_AddStringToObject(o, "model", a->cfg.ai.model);
            cJSON_AddStringToObject(o, "system_prompt", a->cfg.ai.system_prompt);

            cJSON *tools = cJSON_CreateArray();
            for (int j = 0; j < a->cfg.ai.tool_count; j++)
                cJSON_AddItemToArray(tools, cJSON_CreateString(a->cfg.ai.tools[j]));
            cJSON_AddItemToObject(o, "tools", tools);

            cJSON *allow = cJSON_CreateArray();
            for (int j = 0; j < a->cfg.ai.allowlist_count; j++)
                cJSON_AddItemToArray(allow, cJSON_CreateString(a->cfg.ai.run_command_allowlist[j]));
            cJSON_AddItemToObject(o, "allowlist", allow);

            cJSON *appr = cJSON_CreateArray();
            for (int j = 0; j < a->cfg.ai.approval_count; j++)
                cJSON_AddItemToArray(appr, cJSON_CreateString(a->cfg.ai.approval_tools[j]));
            cJSON_AddItemToObject(o, "approval", appr);
        } else {
            cJSON_AddStringToObject(o, "command", a->cfg.worker.command);
        }
        cJSON_AddItemToArray(arr, o);
    }

    char out[32768];
    if (!json_serialize(arr, out, sizeof(out)))
        snprintf(out, sizeof(out), "[]");
    cJSON_Delete(arr);
    http_res_json(res, 200, out);
}

static void route_agent_create(const http_req_t *req, http_res_t *res) {
    cJSON *root = cJSON_Parse(req->body);
    if (!root) { http_res_error(res, 400, "invalid JSON body"); return; }

    char wsname[MAX_STR], id[MAX_STR], type[16];
    if (!json_get_string(root, "ws", wsname, sizeof(wsname)) ||
        !json_get_string(root, "id", id, sizeof(id)) ||
        !json_get_string(root, "type", type, sizeof(type))) {
        cJSON_Delete(root);
        http_res_error(res, 400, "required fields: ws, id, type");
        return;
    }

    workspace_t ws;
    if (!load_ws(wsname, &ws)) {
        cJSON_Delete(root);
        http_res_error(res, 404, "no such workspace");
        return;
    }
    if (workspace_find_agent(&ws, id)) {
        cJSON_Delete(root);
        http_res_error(res, 400, "an agent with this id already exists");
        return;
    }

    agent_t agent;
    memset(&agent, 0, sizeof(agent));
    snprintf(agent.id, MAX_STR, "%s", id);

    if (strcmp(type, "ai") == 0) {
        agent.type = AGENT_TYPE_AI;

        char provider[32], model[MAX_STR];
        if (!json_get_string(root, "provider", provider, sizeof(provider)) ||
            !json_get_string(root, "model", model, sizeof(model))) {
            cJSON_Delete(root);
            http_res_error(res, 400, "an AI agent requires 'provider' and 'model'");
            return;
        }
        agent.cfg.ai.provider = provider_from_string(provider);
        if (agent.cfg.ai.provider == LLM_PROVIDER_UNKNOWN) {
            cJSON_Delete(root);
            http_res_error(res, 400, "unknown provider (anthropic|openai|gemini)");
            return;
        }
        snprintf(agent.cfg.ai.model, MAX_STR, "%s", model);
        json_get_string(root, "system_prompt", agent.cfg.ai.system_prompt,
                        sizeof(agent.cfg.ai.system_prompt));

        /* tools: array de stringuri */
        cJSON *tools = cJSON_GetObjectItemCaseSensitive(root, "tools");
        if (cJSON_IsArray(tools)) {
            cJSON *t = NULL;
            cJSON_ArrayForEach(t, tools) {
                if (!cJSON_IsString(t) || agent.cfg.ai.tool_count >= MAX_TOOLS) continue;
                snprintf(agent.cfg.ai.tools[agent.cfg.ai.tool_count], MAX_STR,
                         "%s", t->valuestring);
                agent.cfg.ai.tool_count++;
            }
        }
        /* allowlist for run_command */
        cJSON *allow = cJSON_GetObjectItemCaseSensitive(root, "allowlist");
        if (cJSON_IsArray(allow)) {
            cJSON *c = NULL;
            cJSON_ArrayForEach(c, allow) {
                if (!cJSON_IsString(c) || agent.cfg.ai.allowlist_count >= MAX_ALLOWLIST) continue;
                snprintf(agent.cfg.ai.run_command_allowlist[agent.cfg.ai.allowlist_count],
                         MAX_STR, "%s", c->valuestring);
                agent.cfg.ai.allowlist_count++;
            }
        }
        /* Tools the human must approve before they run. */
        cJSON *appr = cJSON_GetObjectItemCaseSensitive(root, "approval");
        if (cJSON_IsArray(appr)) {
            cJSON *c = NULL;
            cJSON_ArrayForEach(c, appr) {
                if (!cJSON_IsString(c) || agent.cfg.ai.approval_count >= MAX_TOOLS) continue;
                snprintf(agent.cfg.ai.approval_tools[agent.cfg.ai.approval_count],
                         MAX_STR, "%s", c->valuestring);
                agent.cfg.ai.approval_count++;
            }
        }
    } else if (strcmp(type, "worker") == 0) {
        agent.type = AGENT_TYPE_WORKER;
        if (!json_get_string(root, "command", agent.cfg.worker.command,
                             sizeof(agent.cfg.worker.command))) {
            cJSON_Delete(root);
            http_res_error(res, 400, "a worker agent requires 'command'");
            return;
        }
        cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
        if (cJSON_IsArray(args)) {
            cJSON *a = NULL;
            cJSON_ArrayForEach(a, args) {
                if (!cJSON_IsString(a) || agent.cfg.worker.argc >= MAX_TOOLS) continue;
                snprintf(agent.cfg.worker.args[agent.cfg.worker.argc], MAX_STR,
                         "%s", a->valuestring);
                agent.cfg.worker.argc++;
            }
        }
    } else {
        cJSON_Delete(root);
        http_res_error(res, 400, "type must be 'ai' or 'worker'");
        return;
    }
    cJSON_Delete(root);

    if (!workspace_add_agent(&ws, &agent)) {
        http_res_error(res, 500, "could not add the agent");
        return;
    }
    http_res_json(res, 201, "{\"ok\":true}");
}

/* --- routes: chat --- */

/*
 * Chat: we no longer run the agent in the server process. We start a JOB (fork),
 * which runs the agent isolated, and return a job_id immediately. The client
 * polls /api/jobs/<id>.
 *
 * Why it matters:
 *   - an agent that crashes no longer takes the server with it (isolation, as in the CLI)
 *   - the HTTP request responds in milliseconds, not after the 30s the model works
 */
static void route_chat(const http_req_t *req, http_res_t *res) {
    char wsname[MAX_STR], agent_id[MAX_STR], message[8192];

    cJSON *root = cJSON_Parse(req->body);
    if (!root) { http_res_error(res, 400, "invalid JSON body"); return; }
    int ok = json_get_string(root, "ws", wsname, sizeof(wsname)) &&
             json_get_string(root, "agent", agent_id, sizeof(agent_id)) &&
             json_get_string(root, "message", message, sizeof(message));
    cJSON_Delete(root);

    if (!ok) {
        http_res_error(res, 400, "required fields: ws, agent, message");
        return;
    }

    /* We validate BEFORE the fork, so we give clear errors immediately (not via the job). */
    workspace_t ws;
    if (!load_ws(wsname, &ws)) { http_res_error(res, 404, "no such workspace"); return; }
    agent_t *agent = workspace_find_agent(&ws, agent_id);
    if (!agent) { http_res_error(res, 404, "no such agent"); return; }
    if (agent->type != AGENT_TYPE_AI) {
        http_res_error(res, 400, "chat requires an AI agent");
        return;
    }

    char job_id[JOB_ID_LEN];
    if (!job_start_chat(wsname, agent_id, message, job_id)) {
        http_res_error(res, 500, "could not start the job");
        return;
    }

    char out[256];
    snprintf(out, sizeof(out), "{\"ok\":true,\"job_id\":\"%s\"}", job_id);
    http_res_json(res, 202, out);   /* 202 Accepted: started, not finished */
}

/* A job's state: /api/jobs/<id> */
static void route_job_get(const http_req_t *req, http_res_t *res) {
    /* extract the id from the path: /api/jobs/<id> */
    const char *id = req->path + strlen("/api/jobs/");
    if (!*id) { http_res_error(res, 400, "missing the job id"); return; }

    job_t job;
    if (!job_get(id, &job)) { http_res_error(res, 404, "no such job"); return; }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", job.id);
    cJSON_AddStringToObject(o, "kind", job.kind);
    cJSON_AddStringToObject(o, "status",
        job.status == JOB_RUNNING ? "running" :
        job.status == JOB_DONE    ? "done" : "failed");

    if (job.status == JOB_DONE) {
        if (strcmp(job.kind, "flow") == 0) {
            /* for flow, result is an object {nodes[], total_*} -> we unwrap it */
            cJSON *r = cJSON_Parse(job.result);
            if (r) {
                cJSON *nodes = cJSON_DetachItemFromObjectCaseSensitive(r, "nodes");
                cJSON_AddItemToObject(o, "outputs", nodes ? nodes : cJSON_CreateArray());

                cJSON *ti = cJSON_GetObjectItemCaseSensitive(r, "total_input_tokens");
                cJSON *to = cJSON_GetObjectItemCaseSensitive(r, "total_output_tokens");
                cJSON *tc = cJSON_GetObjectItemCaseSensitive(r, "total_cost_micro_usd");
                cJSON_AddNumberToObject(o, "total_input_tokens",  cJSON_IsNumber(ti) ? ti->valuedouble : 0);
                cJSON_AddNumberToObject(o, "total_output_tokens", cJSON_IsNumber(to) ? to->valuedouble : 0);
                cJSON_AddNumberToObject(o, "total_cost_micro_usd", cJSON_IsNumber(tc) ? tc->valuedouble : 0);
                cJSON_Delete(r);
            } else {
                cJSON_AddItemToObject(o, "outputs", cJSON_CreateArray());
            }
        } else {
            cJSON_AddStringToObject(o, "reply", job.result);
        }
    }
    if (job.status == JOB_FAILED)
        cJSON_AddStringToObject(o, "error", job.error);

    char out[JOB_RESULT_MAX + 2048];
    if (!json_serialize(o, out, sizeof(out)))
        snprintf(out, sizeof(out), "{\"status\":\"failed\",\"error\":\"response too large\"}");
    cJSON_Delete(o);
    http_res_json(res, 200, out);
}

static void route_history_get(const http_req_t *req, http_res_t *res) {
    char wsname[MAX_STR], agent_id[MAX_STR];
    if (!http_query_get(req->query, "ws", wsname, sizeof(wsname)) ||
        !http_query_get(req->query, "agent", agent_id, sizeof(agent_id))) {
        http_res_error(res, 400, "missing the 'ws' and 'agent' parameters");
        return;
    }

    workspace_t ws;
    if (!load_ws(wsname, &ws)) { http_res_error(res, 404, "no such workspace"); return; }
    agent_t *agent = workspace_find_agent(&ws, agent_id);
    if (!agent) { http_res_error(res, 404, "no such agent"); return; }

    cJSON *h = history_load(agent);
    char out[131072];
    if (!json_serialize(h, out, sizeof(out)))
        snprintf(out, sizeof(out), "[]");
    cJSON_Delete(h);
    http_res_json(res, 200, out);
}

static void route_history_reset(const http_req_t *req, http_res_t *res) {
    char wsname[MAX_STR], agent_id[MAX_STR];
    cJSON *root = cJSON_Parse(req->body);
    if (!root) { http_res_error(res, 400, "invalid JSON body"); return; }
    int ok = json_get_string(root, "ws", wsname, sizeof(wsname)) &&
             json_get_string(root, "agent", agent_id, sizeof(agent_id));
    cJSON_Delete(root);
    if (!ok) { http_res_error(res, 400, "required fields: ws, agent"); return; }

    workspace_t ws;
    if (!load_ws(wsname, &ws)) { http_res_error(res, 404, "no such workspace"); return; }
    agent_t *agent = workspace_find_agent(&ws, agent_id);
    if (!agent) { http_res_error(res, 404, "no such agent"); return; }

    history_reset(agent);
    http_res_json(res, 200, "{\"ok\":true}");
}

/* --- routes: graphs --- */

/* Scans a directory for .flow files and adds them to arr. */
static void scan_flows(const char *dir, cJSON *arr) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *ext = strrchr(e->d_name, '.');
        if (!ext || strcmp(ext, ".flow") != 0) continue;

        char rel[MAX_PATH_LEN];
        snprintf(rel, sizeof(rel), "%s/%s", dir, e->d_name);
        cJSON_AddItemToArray(arr, cJSON_CreateString(rel));
    }
    closedir(d);
}

static void route_flows_list(http_res_t *res) {
    cJSON *arr = cJSON_CreateArray();

    /* demo/ first: those are the graphs that work out of the box (they have ready agents) */
    scan_flows("demo", arr);
    scan_flows("examples", arr);

    char out[8192];
    if (!json_serialize(arr, out, sizeof(out))) snprintf(out, sizeof(out), "[]");
    cJSON_Delete(arr);
    http_res_json(res, 200, out);
}

/* Checks a graph path: only from examples/, no traversal. */
/*
 * Allowed paths for graphs: only from our graph directories (examples/ and
 * demo/), no traversal. The rest is refused - an arbitrary graph from disk could
 * reference agents from other workspaces.
 */
static int flow_path_ok(const char *p) {
    if (!p || !p[0]) return 0;
    if (strstr(p, "..")) return 0;

    if (strncmp(p, "examples/", 9) == 0) return 1;
    if (strncmp(p, "demo/", 5) == 0) return 1;
    return 0;
}

/*
 * Saves a graph written in the UI: POST /api/flows  {name, content}
 *
 * The name is confined to demo/ (the same directories we list), and we validate
 * the graph BEFORE writing: a file that fails to parse would show up in the list
 * and only break when someone tries to run it.
 */
static void route_flow_save(const http_req_t *req, http_res_t *res) {
    char name[MAX_PATH_LEN];
    if (!body_get_string(req, "name", name, sizeof(name))) {
        http_res_error(res, 400, "missing the 'name' field");
        return;
    }

    cJSON *body = cJSON_Parse(req->body);
    if (!body) { http_res_error(res, 400, "invalid JSON body"); return; }
    cJSON *c = cJSON_GetObjectItemCaseSensitive(body, "content");
    if (!cJSON_IsString(c) || !c->valuestring[0]) {
        cJSON_Delete(body);
        http_res_error(res, 400, "missing the 'content' field");
        return;
    }

    /* A bare name lands in demo/; a full path must still be one of ours.
       The name is bounded well below MAX_PATH_LEN, so the prefix always fits. */
    char path[MAX_PATH_LEN + 16];
    if (strlen(name) > MAX_PATH_LEN - 16) {
        http_res_error(res, 400, "graph name too long");
        return;
    }
    if (strchr(name, '/')) {
        snprintf(path, sizeof(path), "%s", name);
    } else {
        snprintf(path, sizeof(path), "demo/%s", name);
    }
    size_t plen = strlen(path);
    if (plen < 5 || strcmp(path + plen - 5, ".flow") != 0) {
        if (plen + 6 < sizeof(path)) strcat(path, ".flow");
    }
    if (!flow_path_ok(path)) {
        cJSON_Delete(body);
        http_res_error(res, 400, "invalid graph name");
        return;
    }

    /* Validate by writing to a temporary file and parsing it, so a broken graph
       never replaces a working one. */
    char tmp[MAX_PATH_LEN + 32];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        cJSON_Delete(body);
        http_res_error(res, 500, "cannot write the graph");
        return;
    }
    fputs(c->valuestring, f);
    fclose(f);
    cJSON_Delete(body);

    flow_graph_t g;
    if (!flow_load(&g, tmp)) {
        remove(tmp);
        http_res_error(res, 400, "the graph has invalid syntax (see the log)");
        return;
    }

    if (rename(tmp, path) != 0) {
        remove(tmp);
        http_res_error(res, 500, "cannot save the graph");
        return;
    }

    LOG_I("flow: saved '%s' (%d nodes, %d edges)", path, g.node_count, g.edge_count);

    char out[MAX_PATH_LEN + 64];
    snprintf(out, sizeof(out), "{\"saved\":\"%s\"}", path);
    http_res_json(res, 200, out);
}

static void route_flow_diagram(const http_req_t *req, http_res_t *res) {
    char flow[MAX_PATH_LEN];
    if (!body_get_string(req, "flow", flow, sizeof(flow))) {
        http_res_error(res, 400, "missing the 'flow' field");
        return;
    }
    if (!flow_path_ok(flow)) {
        http_res_error(res, 400, "invalid graph path (only examples/*.flow)");
        return;
    }

    flow_graph_t g;
    if (!flow_load(&g, flow)) {
        http_res_error(res, 400, "could not load the graph");
        return;
    }

    char mermaid[8192];
    if (!flow_to_mermaid(&g, mermaid, sizeof(mermaid))) {
        http_res_error(res, 500, "the diagram does not fit");
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mermaid", mermaid);
    char out[16384];
    json_serialize(o, out, sizeof(out));
    cJSON_Delete(o);
    http_res_json(res, 200, out);
}

/* Running a graph: this becomes a job too (it can take minutes). */
static void route_flow_run(const http_req_t *req, http_res_t *res) {
    char wsname[MAX_STR], flow[MAX_PATH_LEN], task[4096];
    cJSON *root = cJSON_Parse(req->body);
    if (!root) { http_res_error(res, 400, "invalid JSON body"); return; }
    int ok = json_get_string(root, "ws", wsname, sizeof(wsname)) &&
             json_get_string(root, "flow", flow, sizeof(flow));
    task[0] = 0;
    json_get_string(root, "task", task, sizeof(task));   /* optional */
    cJSON_Delete(root);

    if (!ok) { http_res_error(res, 400, "required fields: ws, flow"); return; }
    if (!flow_path_ok(flow)) {
        http_res_error(res, 400, "invalid graph path (only examples/*.flow)");
        return;
    }

    /* we validate before the fork, so we give clear errors immediately */
    workspace_t ws;
    if (!load_ws(wsname, &ws)) { http_res_error(res, 404, "no such workspace"); return; }

    flow_graph_t g;
    if (!flow_load(&g, flow)) { http_res_error(res, 400, "invalid graph"); return; }

    if (!flow_validate(&g, &ws)) {
        /* A USEFUL message: we say exactly which agents are missing, not just "see the
           log". The most frequent case: you picked a graph that requires different
           agents than the ones in the selected workspace. */
        char missing[512];
        missing[0] = 0;
        size_t off = 0;

        for (int i = 0; i < g.node_count; i++) {
            if (workspace_find_agent(&ws, g.nodes[i])) continue;

            int n = snprintf(missing + off, sizeof(missing) - off,
                             "%s%s", off ? ", " : "", g.nodes[i]);
            if (n < 0) break;
            off += (size_t)n;
            if (off >= sizeof(missing) - 1) break;
        }

        char msg[1024];
        if (missing[0])
            snprintf(msg, sizeof(msg),
                     "Workspace '%s' does not have the agents required by the graph: %s. "
                     "Pick another workspace, or create the agents with these names.",
                     wsname, missing);
        else
            snprintf(msg, sizeof(msg),
                     "The graph is not valid (missing entry or an edge to a nonexistent node).");

        http_res_error(res, 400, msg);
        return;
    }

    char job_id[JOB_ID_LEN];
    if (!job_start_flow(wsname, flow, task, job_id)) {
        http_res_error(res, 500, "could not start the job");
        return;
    }

    char out[256];
    snprintf(out, sizeof(out), "{\"ok\":true,\"job_id\":\"%s\"}", job_id);
    http_res_json(res, 202, out);
}

/*
 * The human's decision: POST /api/jobs/<id>/approve  {"allow": true}
 *
 * The agent is blocked in another process, polling jobs/<id>.approval. We just
 * write the verdict there; the agent picks it up and continues. Writing a file
 * is how the two processes talk - after fork they share no memory.
 */
static void route_job_approve(const http_req_t *req, http_res_t *res) {
    char id[JOB_ID_LEN];
    const char *p = req->path + strlen("/api/jobs/");
    const char *slash = strchr(p, '/');
    if (!slash) { http_res_error(res, 400, "invalid path"); return; }

    size_t len = (size_t)(slash - p);
    if (len == 0 || len >= sizeof(id)) { http_res_error(res, 400, "invalid id"); return; }
    memcpy(id, p, len);
    id[len] = 0;

    cJSON *body = cJSON_Parse(req->body);
    if (!body) { http_res_error(res, 400, "invalid JSON body"); return; }
    cJSON *allow = cJSON_GetObjectItemCaseSensitive(body, "allow");
    int allowed = cJSON_IsTrue(allow);
    cJSON_Delete(body);

    char path[1024];
    if (!jobs_approval_path(id, path, sizeof(path))) {
        http_res_error(res, 400, "invalid id"); return;
    }

    FILE *f = fopen(path, "w");
    if (!f) { http_res_error(res, 500, "cannot record the decision"); return; }
    fputs(allowed ? "allow" : "deny", f);
    fclose(f);

    LOG_I("approval: job %s -> %s", id, allowed ? "allowed" : "denied");
    http_res_json(res, 200, allowed ? "{\"allowed\":true}" : "{\"allowed\":false}");
}

/*
 * A job's events: /api/jobs/<id>/events?from=N
 * The frontend polls and receives only the NEW events (from index N), so it does
 * not retransmit the whole journal on each request.
 * Response: {"events":[...], "next":N}
 *
 * Events are returned in BOUNDED batches. This matters more than it looks: the
 * response is serialized into a fixed buffer, and json_serialize refuses to
 * truncate (truncated JSON is invalid JSON), so an oversized batch used to fall
 * through to an empty response with `next` unchanged. The client then asked for
 * the same range again, got empty again, and the UI silently stopped updating -
 * including the approval dialog, leaving the agent waiting for a decision the
 * human was never shown.
 *
 * A single `docker compose build` streams enough output to blow past any
 * reasonable buffer, so this is the normal case, not an edge case. Capping the
 * batch keeps every response well inside the buffer; the client simply comes
 * back for the rest, which it already does on every poll.
 */
#define EVENTS_MAX_BATCH      400      /* events per response  */
#define EVENTS_MAX_BODY     98304      /* 96 KB, well under the 128 KB buffer */

static void route_job_events(const http_req_t *req, http_res_t *res) {
    /* extract the id: /api/jobs/<id>/events */
    char id[JOB_ID_LEN];
    const char *p = req->path + strlen("/api/jobs/");
    const char *slash = strchr(p, '/');
    if (!slash) { http_res_error(res, 400, "invalid path"); return; }

    size_t len = (size_t)(slash - p);
    if (len == 0 || len >= sizeof(id)) { http_res_error(res, 400, "invalid id"); return; }
    memcpy(id, p, len);
    id[len] = 0;

    char from_s[16];
    int from = 0;
    if (http_query_get(req->query, "from", from_s, sizeof(from_s)))
        from = atoi(from_s);
    if (from < 0) from = 0;

    char path[1024];
    if (!jobs_events_path(id, path, sizeof(path))) {
        http_res_error(res, 400, "invalid id");
        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        /* the journal does not exist yet (the job just started) - not an error */
        http_res_json(res, 200, "{\"events\":[],\"next\":0}");
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    char line[2048];
    int index = 0;
    int sent = 0;
    size_t bytes = 0;

    while (fgets(line, sizeof(line), f)) {
        if (index < from) { index++; continue; }   /* already delivered */

        /* Stop at either cap. `index` is not advanced for what we do not send,
           so `next` points exactly at the first event the client still needs. */
        if (sent >= EVENTS_MAX_BATCH || bytes >= EVENTS_MAX_BODY) break;

        line[strcspn(line, "\n")] = 0;
        if (!line[0]) { index++; continue; }

        cJSON *ev = cJSON_Parse(line);
        if (ev) {
            cJSON_AddItemToArray(arr, ev);
            bytes += strlen(line) + 2;   /* the comma and quoting overhead */
            sent++;
        }
        index++;
    }
    fclose(f);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "events", arr);
    cJSON_AddNumberToObject(o, "next", index);

    char out[131072];
    if (!json_serialize(o, out, sizeof(out))) {
        /*
         * Should be unreachable now that batches are capped, but if it ever
         * happens we must not hand back `next` unchanged - that is the silent
         * stall this whole function is written to avoid. Advancing past one
         * event loses that event and keeps the stream moving, which is the
         * lesser harm, and the log says so plainly.
         */
        LOG_W("events: job %s batch did not fit at index %d, skipping one event",
              id, from);
        snprintf(out, sizeof(out), "{\"events\":[],\"next\":%d}", from + 1);
    }
    cJSON_Delete(o);
    http_res_json(res, 200, out);
}

/*
 * A graph's structure: /api/flow/graph  {flow}
 * The frontend needs nodes + edges in order to DRAW the graph (SVG) before the
 * run starts. Until now we sent only Mermaid (text), which cannot be animated.
 */
static void route_flow_graph(const http_req_t *req, http_res_t *res) {
    char flow[MAX_PATH_LEN];
    if (!body_get_string(req, "flow", flow, sizeof(flow))) {
        http_res_error(res, 400, "missing the 'flow' field");
        return;
    }
    if (!flow_path_ok(flow)) {
        http_res_error(res, 400, "invalid graph path (only examples/*.flow)");
        return;
    }

    flow_graph_t g;
    if (!flow_load(&g, flow)) {
        http_res_error(res, 400, "could not load the graph");
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "entry", g.entry);

    cJSON *nodes = cJSON_CreateArray();
    for (int i = 0; i < g.node_count; i++)
        cJSON_AddItemToArray(nodes, cJSON_CreateString(g.nodes[i]));
    cJSON_AddItemToObject(o, "nodes", nodes);

    cJSON *edges = cJSON_CreateArray();
    for (int i = 0; i < g.edge_count; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "from", g.edges[i].from);
        cJSON_AddStringToObject(e, "to", g.edges[i].to);
        cJSON_AddStringToObject(e, "condition", g.edges[i].condition);
        cJSON_AddItemToArray(edges, e);
    }
    cJSON_AddItemToObject(o, "edges", edges);

    char out[32768];
    if (!json_serialize(o, out, sizeof(out)))
        snprintf(out, sizeof(out), "{}");
    cJSON_Delete(o);
    http_res_json(res, 200, out);
}

/* --- dispatcher --- */

void web_handler(const http_req_t *req, http_res_t *res) {
    int is_get  = strcmp(req->method, "GET") == 0;
    int is_post = strcmp(req->method, "POST") == 0;

    /* frontend */
    if (is_get && (strcmp(req->path, "/") == 0 || strcmp(req->path, "/index.html") == 0)) {
        http_res_static(res, 200, "text/html; charset=utf-8",
                        WEB_INDEX_HTML, strlen(WEB_INDEX_HTML));
        return;
    }

    /* API */
    if (strcmp(req->path, "/api/workspaces") == 0) {
        if (is_get)  { route_workspaces_list(res); return; }
        if (is_post) { route_workspace_create(req, res); return; }
        http_res_error(res, 405, "method not allowed");
        return;
    }
    if (strcmp(req->path, "/api/agents") == 0) {
        if (is_get)  { route_agents_list(req, res); return; }
        if (is_post) { route_agent_create(req, res); return; }
        http_res_error(res, 405, "method not allowed");
        return;
    }
    if (is_post && strcmp(req->path, "/api/chat") == 0) {
        route_chat(req, res); return;
    }
    if (is_get && strcmp(req->path, "/api/history") == 0) {
        route_history_get(req, res); return;
    }
    if (is_post && strcmp(req->path, "/api/history/reset") == 0) {
        route_history_reset(req, res); return;
    }
    if (is_get && strcmp(req->path, "/api/flows") == 0) {
        route_flows_list(res); return;
    }
    if (is_post && strcmp(req->path, "/api/flow/diagram") == 0) {
        route_flow_diagram(req, res); return;
    }
    if (is_post && strcmp(req->path, "/api/flow/run") == 0) {
        route_flow_run(req, res); return;
    }
    if (is_get && strncmp(req->path, "/api/jobs/", 10) == 0) {
        /* /api/jobs/<id>/events takes priority over /api/jobs/<id> */
        if (strstr(req->path, "/events")) { route_job_events(req, res); return; }
        route_job_get(req, res); return;
    }
    if (is_post && strcmp(req->path, "/api/flow/graph") == 0) {
        route_flow_graph(req, res); return;
    }
    if (is_post && strcmp(req->path, "/api/flows") == 0) {
        route_flow_save(req, res); return;
    }
    if (is_post && strncmp(req->path, "/api/jobs/", 10) == 0 &&
        strstr(req->path, "/approve")) {
        route_job_approve(req, res); return;
    }

    http_res_error(res, 404, "no such route");
}
