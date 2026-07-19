#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include "workspace.h"
#include "flow.h"

/* Limit on orchestration steps (transitions between nodes), so it does not loop
   forever if the graph has a cycle that never closes (e.g. a condition that never
   becomes true). Analogous to AI_LOOP_MAX_ITERATIONS. */
#define ORCH_MAX_STEPS 50

/* Runs graph `g` over the agents in `ws`, starting from the entry node.
   `initial_task` is the task given to the first node (may be NULL).
   At each step:
     1. runs the current node's agent (supervised)
     2. reads its final output (from <agent_dir>/_output.txt)
     3. picks the next node with flow_next (applies conditional edges)
     4. writes the current output as the next node's input (_input.txt)
   It stops at END, at a dead end, on agent error, or at ORCH_MAX_STEPS.
   Returns 0 if it reached END cleanly, -1 otherwise. */
int orchestrator_run(workspace_t *ws, const flow_graph_t *g, const char *initial_task);

#endif
