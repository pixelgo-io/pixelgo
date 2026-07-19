#include "orchestrator.h"
#include "agent_run.h"
#include "log.h"
#include "usage.h"
#include "provider.h"
#include "json_util.h"
#include "events.h"
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * Orchestrator with FAN-OUT / FAN-IN.
 *
 * Model: a "frontier" of nodes ready to run.
 *   - FAN-OUT: all nodes in the frontier run IN PARALLEL (one fork per node via
 *     agent_start), then we wait for all of them (synchronization barrier).
 *   - FAN-IN: a node with several predecessors enters the frontier only when ALL
 *     of its predecessors that are active in the current step have finished.
 *   - Conditional edges are evaluated on each finished node's output, so loops
 *     (reviewer -> coder when NEEDS_FIX) remain possible.
 *
 * Messages flow through files in the sandbox: each node writes _output.txt; the
 * next node receives _input.txt with the predecessors' outputs (labeled, if there
 * are several - this way a merger knows who produced what).
 */

#define MAX_FRONTIER FLOW_MAX_NODES

/* Current time in ms - so we can measure how long each node takes. */
static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int read_agent_output(const agent_t *a, char *out, int out_size) {
    char path[MAX_PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/_output.txt", a->dir);
    FILE *f = fopen(path, "r");
    if (!f) { out[0] = 0; return 0; }
    size_t n = fread(out, 1, out_size - 1, f);
    out[n] = 0;
    fclose(f);
    return 1;
}

/* Deletes the old output before running, so we do not read a stale result if the
   agent re-enters the loop and does not write a new one. */
static void clear_agent_output(const agent_t *a) {
    char path[MAX_PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/_output.txt", a->dir);
    remove(path);
}

static int write_agent_input(const agent_t *a, const char *msg) {
    char path[MAX_PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/_input.txt", a->dir);
    FILE *f = fopen(path, "w");
    if (!f) { LOG_E("orch: cannot write the input for '%s'", a->id); return 0; }
    fputs(msg ? msg : "", f);
    fclose(f);
    return 1;
}

static int in_list(char list[][MAX_STR], int count, const char *id) {
    for (int i = 0; i < count; i++)
        if (strcmp(list[i], id) == 0) return 1;
    return 0;
}

/* A node's input = the outputs of its predecessors. On fan-in (several
   predecessors) we label them, so the merger knows who said what. */
static void build_input(workspace_t *ws, const flow_graph_t *g,
                        const char *node, char *out, size_t out_size) {
    out[0] = 0;
    size_t off = 0;

    char preds[FLOW_MAX_NODES][MAX_STR];
    int npred = flow_predecessors(g, node, preds, FLOW_MAX_NODES);

    for (int i = 0; i < npred; i++) {
        agent_t *pa = workspace_find_agent(ws, preds[i]);
        if (!pa) continue;

        char pout[8192];
        if (!read_agent_output(pa, pout, sizeof(pout)) || pout[0] == 0) continue;

        int n;
        if (npred > 1)
            n = snprintf(out + off, out_size - off,
                         "--- Result from '%s' ---\n%s\n\n", preds[i], pout);
        else
            n = snprintf(out + off, out_size - off, "%s", pout);

        if (n < 0) break;
        off += (size_t)n;
        if (off >= out_size - 1) break;
    }
}

/* Reads an agent's usage from <dir>/_usage.json. Returns 1 if it exists. */
static int read_agent_usage(const agent_t *a, usage_t *out, char *model, size_t model_size,
                            llm_provider_id_t *provider) {
    char path[MAX_PATH_LEN + 24];
    snprintf(path, sizeof(path), "%s/_usage.json", a->dir);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);

    cJSON *o = cJSON_Parse(buf);
    if (!o) return 0;

    cJSON *in   = cJSON_GetObjectItemCaseSensitive(o, "input_tokens");
    cJSON *out_ = cJSON_GetObjectItemCaseSensitive(o, "output_tokens");
    cJSON *cost = cJSON_GetObjectItemCaseSensitive(o, "cost_micro_usd");

    out->input_tokens   = cJSON_IsNumber(in)   ? (long)in->valuedouble   : 0;
    out->output_tokens  = cJSON_IsNumber(out_) ? (long)out_->valuedouble : 0;
    out->cost_micro_usd = cJSON_IsNumber(cost) ? (long)cost->valuedouble : 0;

    if (model && model_size) json_get_string(o, "model", model, model_size);
    if (provider) {
        char p[32];
        if (json_get_string(o, "provider", p, sizeof(p)))
            *provider = provider_from_string(p);
    }
    cJSON_Delete(o);
    return 1;
}

/*
 * The graph's cost report. This is the demonstration of the central argument:
 * it shows what it REALLY cost (mixed models) versus what it would have cost if
 * the WHOLE graph had run on the most expensive model used.
 */
static void report_costs(workspace_t *ws, const flow_graph_t *g) {
    usage_t total = {0, 0, 0};
    int have_any = 0;

    /* the most expensive model used in the graph - the reference for comparison */
    char most_expensive_model[MAX_STR] = "";
    llm_provider_id_t most_expensive_provider = LLM_PROVIDER_UNKNOWN;
    long highest_rate = 0;

    LOG_I("--- cost pe agent ---");

    for (int i = 0; i < g->node_count; i++) {
        agent_t *a = workspace_find_agent(ws, g->nodes[i]);
        if (!a || a->type != AGENT_TYPE_AI) continue;

        usage_t u;
        char model[MAX_STR] = "";
        llm_provider_id_t prov = LLM_PROVIDER_UNKNOWN;
        if (!read_agent_usage(a, &u, model, sizeof(model), &prov)) continue;

        have_any = 1;
        usage_add(&total, &u);

        char cost[24];
        usage_format_cost(u.cost_micro_usd, cost, sizeof(cost));
        LOG_I("  %-16s %-10s %-22s in=%-7ld out=%-6ld %s",
              a->id, provider_to_string(prov), model,
              u.input_tokens, u.output_tokens, cost);

        /* we keep the model with the highest OUTPUT price (a proxy for "the expensive one") */
        long rate = usage_cost_micro(prov, model, 0, 1000000);   /* price per 1M output */
        if (rate > highest_rate) {
            highest_rate = rate;
            snprintf(most_expensive_model, sizeof(most_expensive_model), "%s", model);
            most_expensive_provider = prov;
        }
    }

    if (!have_any) return;

    char total_cost[24];
    usage_format_cost(total.cost_micro_usd, total_cost, sizeof(total_cost));

    LOG_I("--- total: in=%ld out=%ld cost=%s ---",
          total.input_tokens, total.output_tokens, total_cost);

    /* The comparison that matters: what would everything have cost on the expensive model? */
    if (most_expensive_model[0] && highest_rate > 0) {
        long alt = usage_cost_if_all_on(most_expensive_provider, most_expensive_model,
                                        total.input_tokens, total.output_tokens);
        if (alt > total.cost_micro_usd) {
            char alt_cost[24], saved_cost[24];
            usage_format_cost(alt, alt_cost, sizeof(alt_cost));
            usage_format_cost(alt - total.cost_micro_usd, saved_cost, sizeof(saved_cost));
            int pct = (int)(100 - (total.cost_micro_usd * 100) / alt);

            LOG_I("--- if the whole graph had run on %s: %s ---",
                  most_expensive_model, alt_cost);
            LOG_I("--- SAVINGS: %s (%d%%) ---", saved_cost, pct);
        }
    }
}

int orchestrator_run(workspace_t *ws, const flow_graph_t *g, const char *initial_task) {
    char frontier[MAX_FRONTIER][MAX_STR];
    int frontier_count = 0;
    char completed[MAX_FRONTIER][MAX_STR];
    int completed_count = 0;

    snprintf(frontier[frontier_count++], MAX_STR, "%s", g->entry);
    LOG_I("orch: starting the graph from '%s'", g->entry);

    /* The event journal: the frontend reads it to animate the graph in real time
       (the pixels traveling along edges, the active node pulsing). */
    event_graph_start(g->entry, g->node_count);
    long long graph_start = now_ms();

    int reached_end = 0;

    for (int step = 0; step < ORCH_MAX_STEPS; step++) {
        LOG_I("orch: step %d - running %d node(s) in parallel", step + 1, frontier_count);

        /* --- FAN-OUT: we start all nodes in the frontier simultaneously --- */
        agent_t *running[MAX_FRONTIER];
        int running_count = 0;

        for (int i = 0; i < frontier_count; i++) {
            agent_t *a = workspace_find_agent(ws, frontier[i]);
            if (!a) {
                LOG_E("orch: node '%s' has no corresponding agent", frontier[i]);
                for (int k = 0; k < running_count; k++) agent_stop(running[k]);
                return -1;
            }

            if (step > 0) {
                char input[16384];
                build_input(ws, g, frontier[i], input, sizeof(input));
                if (input[0]) write_agent_input(a, input);
            }
            clear_agent_output(a);

            const char *base_task = (step == 0) ? initial_task : NULL;
            LOG_I("orch: starting '%s'", a->id);

            event_node_start(a->id,
                a->type == AGENT_TYPE_AI ? provider_to_string(a->cfg.ai.provider) : "worker",
                a->type == AGENT_TYPE_AI ? a->cfg.ai.model : "");

            if (agent_start(a, base_task) != 0) {
                LOG_E("orch: starting node '%s' failed", a->id);
                for (int k = 0; k < running_count; k++) agent_stop(running[k]);
                return -1;
            }
            running[running_count++] = a;
        }

        /* --- BARRIER: we wait for ALL the started nodes --- */
        long long step_start = now_ms();
        int failed = 0;

        for (int i = 0; i < running_count; i++) {
            if (agent_wait(running[i]) != 0) {
                LOG_E("orch: node '%s' failed (status=%d)",
                      running[i]->id, running[i]->status);
                event_node_failed(running[i]->id, "the agent failed");
                failed = 1;
            } else {
                /* node finished successfully: we report the cost and duration */
                usage_t u = {0, 0, 0};
                read_agent_usage(running[i], &u, NULL, 0, NULL);
                event_node_done(running[i]->id, u.input_tokens, u.output_tokens,
                                u.cost_micro_usd, (long)(now_ms() - step_start));
            }
        }
        if (failed) {
            LOG_E("orch: at least one node failed, stopping the graph");
            event_graph_failed("a node failed");
            return -1;
        }

        completed_count = 0;
        for (int i = 0; i < frontier_count; i++)
            snprintf(completed[completed_count++], MAX_STR, "%s", frontier[i]);

        /* --- compute the next candidates (fan-out + conditions) --- */
        char candidates[MAX_FRONTIER][MAX_STR];
        int cand_count = 0;

        for (int i = 0; i < completed_count; i++) {
            agent_t *a = workspace_find_agent(ws, completed[i]);
            char output[8192];
            if (!read_agent_output(a, output, sizeof(output))) output[0] = 0;

            char targets[MAX_FRONTIER][MAX_STR];
            int nt = flow_next_all(g, completed[i], output, targets, MAX_FRONTIER);

            /*
             * Important diagnostic: if the node HAS edges, but NONE matched, it
             * means they are all conditional and the model wrote no expected
             * marker. The graph would stop SILENTLY - very hard to debug without
             * this message.
             */
            if (nt == 0) {
                int has_edges = 0, all_conditional = 1;
                char expected[256];
                expected[0] = 0;
                size_t eoff = 0;

                for (int e = 0; e < g->edge_count; e++) {
                    if (strcmp(g->edges[e].from, completed[i]) != 0) continue;
                    has_edges = 1;
                    if (!g->edges[e].condition[0]) { all_conditional = 0; continue; }

                    int n = snprintf(expected + eoff, sizeof(expected) - eoff,
                                     "%s%s", eoff ? ", " : "", g->edges[e].condition);
                    if (n > 0) eoff += (size_t)n;
                }

                if (has_edges && all_conditional) {
                    LOG_W("orch: node '%s' has ONLY conditional edges (%s), but "
                          "its output contains none of the markers.",
                          completed[i], expected);
                    LOG_W("orch: the graph stops here. Either the model forgot to write "
                          "the marker, or add an unconditional edge as a fallback.");
                    LOG_W("orch: output of '%s' was: %.200s",
                          completed[i], output[0] ? output : "(empty)");
                }
            }

            for (int j = 0; j < nt; j++) {
                /* This is where the pixel travels: a message goes from completed[i] to
                   targets[j]. We also send the size (the frontend can make the pixel
                   thicker if the message is larger) and the condition that activated
                   the edge. */
                const char *cond = "";
                for (int e = 0; e < g->edge_count; e++) {
                    if (strcmp(g->edges[e].from, completed[i]) == 0 &&
                        strcmp(g->edges[e].to, targets[j]) == 0 &&
                        g->edges[e].condition[0]) {
                        cond = g->edges[e].condition;
                        break;
                    }
                }
                event_edge_flow(completed[i], targets[j], (long)strlen(output), cond);

                if (strcmp(targets[j], FLOW_END) == 0) {
                    LOG_I("orch: '%s' reached END", completed[i]);
                    reached_end = 1;
                    continue;
                }
                if (!in_list(candidates, cand_count, targets[j]) && cand_count < MAX_FRONTIER)
                    snprintf(candidates[cand_count++], MAX_STR, "%s", targets[j]);
            }
        }

        /* --- FAN-IN: a candidate with several predecessors runs only when ALL of
           its predecessors have finished in the current step. --- */
        frontier_count = 0;
        for (int i = 0; i < cand_count; i++) {
            char preds[FLOW_MAX_NODES][MAX_STR];
            int npred = flow_predecessors(g, candidates[i], preds, FLOW_MAX_NODES);

            int ready = 1;
            if (npred > 1) {
                int done = 0;
                for (int p = 0; p < npred; p++)
                    if (in_list(completed, completed_count, preds[p])) done++;
                if (done < npred) {
                    LOG_I("orch: '%s' waiting for fan-in (%d/%d predecessors ready)",
                          candidates[i], done, npred);
                    ready = 0;
                }
            }
            if (ready && frontier_count < MAX_FRONTIER)
                snprintf(frontier[frontier_count++], MAX_STR, "%s", candidates[i]);
        }

        if (frontier_count == 0) {
            if (reached_end)
                LOG_I("orch: graph finished at END after %d steps", step + 1);
            else
                LOG_I("orch: no applicable transition, stopping (dead end)");

            /* the cost total, for the final event */
            usage_t grand = {0, 0, 0};
            for (int i = 0; i < g->node_count; i++) {
                agent_t *a = workspace_find_agent(ws, g->nodes[i]);
                if (!a) continue;
                usage_t u = {0, 0, 0};
                if (read_agent_usage(a, &u, NULL, 0, NULL)) usage_add(&grand, &u);
            }
            event_graph_done(grand.cost_micro_usd, (long)(now_ms() - graph_start));

            report_costs(ws, g);
            return 0;
        }
    }

    LOG_E("orch: reached the limit of %d steps (possibly a loop that does not close)",
          ORCH_MAX_STEPS);
    return -1;
}
