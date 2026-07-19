#ifndef FLOW_H
#define FLOW_H

#include "agent.h"

/*
 * State graph for orchestrating agents (a "state graph" model with conditional
 * edges, as in current frameworks - LangGraph/Swarm).
 *
 *   nodes = agents (referenced by their workspace id)
 *   edges = transitions from one node to another, optionally conditioned on a
 *           text marker that the source agent puts in its final output
 *
 * The special node "END" (the FLOW_END constant) means the graph terminates.
 * Execution starts from the `entry` node.
 */

#define FLOW_MAX_NODES 32
#define FLOW_MAX_EDGES 64
#define FLOW_END "END"           /* sentinel node: stops the orchestration */

/* An edge: from `from` to `to`. If `condition[0]` != 0, the edge is chosen
   ONLY if the output of agent `from` contains that marker (substring).
   Unconditional edges (empty condition) are the "fallback" - they are chosen if
   no conditional one matched. */
typedef struct {
    char from[MAX_STR];
    char to[MAX_STR];
    char condition[MAX_STR];   /* empty = unconditional */
} flow_edge_t;

typedef struct {
    char entry[MAX_STR];                 /* id of the start node */
    char nodes[FLOW_MAX_NODES][MAX_STR]; /* agent ids */
    int  node_count;
    flow_edge_t edges[FLOW_MAX_EDGES];
    int  edge_count;
} flow_graph_t;

/* Loads a graph from a .flow file. Returns 1 on success, 0 on error
   (invalid syntax, missing file). Error messages go to the log. */
int flow_load(flow_graph_t *g, const char *path);

/* Validates the graph AGAINST a workspace: each node must be an existing agent,
   entry must be set and be a node, edge targets must be nodes or END, edge
   sources must be nodes.
   Returns 1 if valid, 0 otherwise (details in the log). */
struct workspace_t; /* fwd */
int flow_validate(const flow_graph_t *g, const void *ws);

/* Picks the next node after `from` produced `output`. Applies the rule:
   1. the first conditional edge whose condition is in the output wins;
   2. otherwise, the first unconditional edge from `from`.
   Writes the target id into out (may be FLOW_END). Returns 1 if a transition
   was found, 0 if `from` has no applicable edge (dead end). */
int flow_next(const flow_graph_t *g, const char *from, const char *output,
              char out[MAX_STR]);

/*
 * FAN-OUT: collects ALL applicable targets from `from` (not just the first).
 * Rule: the matching conditional edges + the unconditional edges.
 * If more than 1 target results, they run IN PARALLEL.
 * Writes the targets into out[] (max max_out). Returns the number of targets found.
 */
int flow_next_all(const flow_graph_t *g, const char *from, const char *output,
                  char out[][MAX_STR], int max_out);

/* The number of predecessors of a node (how many have an edge to it).
   A node with >1 predecessor is a FAN-IN point: it runs only after ALL of its
   predecessors have finished. */
int flow_predecessor_count(const flow_graph_t *g, const char *node);

/* Fills out[] with the ids of `node`'s predecessors. Returns how many. */
int flow_predecessors(const flow_graph_t *g, const char *node,
                      char out[][MAX_STR], int max_out);

/* Generates a Mermaid diagram (text) of the graph into the given buffer.
   Returns 1 on success, 0 if it did not fit. */
int flow_to_mermaid(const flow_graph_t *g, char *out, int out_size);

#endif
