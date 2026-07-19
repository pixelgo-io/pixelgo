#include "flow.h"
#include "workspace.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* --- parsing helpers, in the style of workspace.c (no external dependencies) --- */

/* trims whitespace at the start/end, in-place; returns a pointer into the buffer */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = 0;
    return s;
}

static int node_exists(const flow_graph_t *g, const char *id) {
    for (int i = 0; i < g->node_count; i++)
        if (strcmp(g->nodes[i], id) == 0) return 1;
    return 0;
}

static int add_node(flow_graph_t *g, const char *id) {
    if (node_exists(g, id)) return 1; /* idempotent */
    if (g->node_count >= FLOW_MAX_NODES) {
        LOG_E("flow: too many nodes (max %d)", FLOW_MAX_NODES);
        return 0;
    }
    snprintf(g->nodes[g->node_count++], MAX_STR, "%s", id);
    return 1;
}

/* Parses a line "edge A -> B" or "edge A -> B when COND". */
static int parse_edge(flow_graph_t *g, char *rest, int lineno) {
    if (g->edge_count >= FLOW_MAX_EDGES) {
        LOG_E("flow: too many edges (max %d)", FLOW_MAX_EDGES);
        return 0;
    }

    /* we split off "when" if present */
    char *when = strstr(rest, " when ");
    char cond[MAX_STR] = {0};
    if (when) {
        *when = 0;
        char *c = trim(when + 6);
        snprintf(cond, sizeof(cond), "%s", c);
    }

    char *arrow = strstr(rest, "->");
    if (!arrow) {
        LOG_E("flow: line %d: 'edge' without '->'", lineno);
        return 0;
    }
    *arrow = 0;
    char *from = trim(rest);
    char *to   = trim(arrow + 2);

    if (from[0] == 0 || to[0] == 0) {
        LOG_E("flow: line %d: edge with an empty endpoint", lineno);
        return 0;
    }

    flow_edge_t *e = &g->edges[g->edge_count++];
    snprintf(e->from, MAX_STR, "%s", from);
    snprintf(e->to, MAX_STR, "%s", to);
    snprintf(e->condition, MAX_STR, "%s", cond);
    return 1;
}

int flow_load(flow_graph_t *g, const char *path) {
    memset(g, 0, sizeof(*g));

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_E("flow: cannot open '%s'", path);
        return 0;
    }

    char line[1024];
    int lineno = 0, ok = 1;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\n")] = 0;

        /* comments ('#') and empty lines */
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *s = trim(line);
        if (s[0] == 0) continue;

        if (strncmp(s, "entry", 5) == 0 && isspace((unsigned char)s[5])) {
            char *id = trim(s + 5);
            snprintf(g->entry, MAX_STR, "%s", id);
        } else if (strncmp(s, "node", 4) == 0 && isspace((unsigned char)s[4])) {
            char *id = trim(s + 4);
            if (!add_node(g, id)) { ok = 0; break; }
        } else if (strncmp(s, "edge", 4) == 0 && isspace((unsigned char)s[4])) {
            if (!parse_edge(g, trim(s + 4), lineno)) { ok = 0; break; }
        } else {
            LOG_E("flow: line %d: unknown syntax: '%s'", lineno, s);
            ok = 0;
            break;
        }
    }

    fclose(f);
    if (!ok) return 0;

    if (g->entry[0] == 0) {
        LOG_E("flow: missing the 'entry' directive");
        return 0;
    }
    LOG_I("flow: loaded '%s' (%d nodes, %d edges, entry=%s)",
          path, g->node_count, g->edge_count, g->entry);
    return 1;
}

int flow_validate(const flow_graph_t *g, const void *ws_ptr) {
    const workspace_t *ws = (const workspace_t *)ws_ptr;
    int ok = 1;

    if (!node_exists(g, g->entry)) {
        LOG_E("flow: entry '%s' is not a declared node", g->entry);
        ok = 0;
    }

    /* each node must be an existing agent in the workspace */
    for (int i = 0; i < g->node_count; i++) {
        int found = 0;
        for (int j = 0; j < ws->agent_count; j++)
            if (strcmp(ws->agents[j].id, g->nodes[i]) == 0) { found = 1; break; }
        if (!found) {
            LOG_E("flow: node '%s' does not match any agent in the workspace",
                  g->nodes[i]);
            ok = 0;
        }
    }

    /* edges: the source must be a node; the target must be a node or END */
    for (int i = 0; i < g->edge_count; i++) {
        const flow_edge_t *e = &g->edges[i];
        if (!node_exists(g, e->from)) {
            LOG_E("flow: edge with unknown source '%s'", e->from);
            ok = 0;
        }
        if (strcmp(e->to, FLOW_END) != 0 && !node_exists(g, e->to)) {
            LOG_E("flow: edge with unknown target '%s'", e->to);
            ok = 0;
        }
    }
    return ok;
}

int flow_next(const flow_graph_t *g, const char *from, const char *output,
              char out[MAX_STR]) {
    const char *fallback = NULL;

    /* the first matching conditional edge wins; we also keep a fallback */
    for (int i = 0; i < g->edge_count; i++) {
        const flow_edge_t *e = &g->edges[i];
        if (strcmp(e->from, from) != 0) continue;

        if (e->condition[0] == 0) {
            if (!fallback) fallback = e->to; /* the first unconditional one */
            continue;
        }
        if (output && strstr(output, e->condition)) {
            snprintf(out, MAX_STR, "%s", e->to);
            return 1;
        }
    }

    if (fallback) {
        snprintf(out, MAX_STR, "%s", fallback);
        return 1;
    }
    return 0; /* no transition -> dead end */
}

int flow_next_all(const flow_graph_t *g, const char *from, const char *output,
                  char out[][MAX_STR], int max_out) {
    int count = 0;

    /* We collect ALL applicable targets (we do not stop at the first, like
       flow_next). This is the difference that makes fan-out possible: if a node has
       3 unconditional edges, all 3 targets run in parallel. */
    for (int i = 0; i < g->edge_count && count < max_out; i++) {
        const flow_edge_t *e = &g->edges[i];
        if (strcmp(e->from, from) != 0) continue;

        /* conditional edge: only if the marker is in the output */
        if (e->condition[0] && !(output && strstr(output, e->condition)))
            continue;

        /* we avoid duplicates (the same target on two edges) */
        int dup = 0;
        for (int j = 0; j < count; j++)
            if (strcmp(out[j], e->to) == 0) { dup = 1; break; }
        if (dup) continue;

        snprintf(out[count++], MAX_STR, "%s", e->to);
    }
    return count;
}

int flow_predecessor_count(const flow_graph_t *g, const char *node) {
    int count = 0;
    for (int i = 0; i < g->edge_count; i++) {
        if (strcmp(g->edges[i].to, node) != 0) continue;
        /* we do not count the same predecessor twice */
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (strcmp(g->edges[j].to, node) == 0 &&
                strcmp(g->edges[j].from, g->edges[i].from) == 0) { dup = 1; break; }
        if (!dup) count++;
    }
    return count;
}

int flow_predecessors(const flow_graph_t *g, const char *node,
                      char out[][MAX_STR], int max_out) {
    int count = 0;
    for (int i = 0; i < g->edge_count && count < max_out; i++) {
        if (strcmp(g->edges[i].to, node) != 0) continue;
        int dup = 0;
        for (int j = 0; j < count; j++)
            if (strcmp(out[j], g->edges[i].from) == 0) { dup = 1; break; }
        if (dup) continue;
        snprintf(out[count++], MAX_STR, "%s", g->edges[i].from);
    }
    return count;
}

int flow_to_mermaid(const flow_graph_t *g, char *out, int out_size) {
    int off = 0;
    int n = snprintf(out + off, out_size - off, "flowchart TD\n");
    if (n < 0 || n >= out_size - off) return 0;
    off += n;

    /* mark the entry node */
    n = snprintf(out + off, out_size - off, "  start([start]) --> %s\n", g->entry);
    if (n < 0 || n >= out_size - off) return 0;
    off += n;

    for (int i = 0; i < g->edge_count; i++) {
        const flow_edge_t *e = &g->edges[i];
        if (e->condition[0])
            n = snprintf(out + off, out_size - off, "  %s -->|%s| %s\n",
                         e->from, e->condition, e->to);
        else
            n = snprintf(out + off, out_size - off, "  %s --> %s\n", e->from, e->to);
        if (n < 0 || n >= out_size - off) return 0;
        off += n;
    }
    return 1;
}
