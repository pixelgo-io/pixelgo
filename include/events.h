#ifndef EVENTS_H
#define EVENTS_H

#include "agent.h"

/*
 * Event journal for visualizing the run in real time.
 *
 * The problem: until now, a job reported only "running" / "done" / "failed".
 * You saw NOTHING along the way - the graph was a black box until the end.
 *
 * The solution: the orchestrator writes events as they happen (node started,
 * node finished, message passed on an edge), and the frontend reads them by
 * polling and animates the graph: pixels travel along edges, the active node
 * pulses.
 *
 * The journal is a JSONL file (one event per line) in jobs/<id>.events.
 * Why JSONL and not one big JSON: writing is APPEND, so we do not have to
 * re-read/rewrite the whole file on each event, and a reader can consume the
 * lines as they appear (without waiting for it to close).
 *
 * Why a file and not memory: the orchestrator runs in a CHILD process (job),
 * separate from the server. Memory is not shared after fork.
 */

typedef enum {
    EV_GRAPH_START,     /* the graph started                                */
    EV_NODE_START,      /* a node started running                           */
    EV_NODE_DONE,       /* a node finished (with cost and tokens)           */
    EV_NODE_FAILED,     /* a node failed                                    */
    EV_EDGE_FLOW,       /* a message passes on an edge (the pixel travels)  */
    EV_GRAPH_DONE,      /* the graph finished                               */
    EV_GRAPH_FAILED,

    /* --- what happens INSIDE an agent (for the live journal) --- */
    EV_AGENT_THINKING,  /* the agent sent a request to the model            */
    EV_TOOL_CALL,       /* the agent requests a tool (read_file, run_command...) */
    EV_TOOL_RESULT,     /* the tool's result                                */
    EV_AGENT_SAYS,      /* the agent's text response                        */

    /* --- human-in-the-loop approval --- */
    EV_APPROVAL_NEEDED, /* the agent is blocked, waiting for your decision   */
    EV_APPROVAL_DONE    /* you decided; carries allowed=1/0                  */
} event_type_t;

/* Opens the journal for the given job. Called by the child, at the start of the
   run. The path is ABSOLUTE (the child chdirs into sandboxes, so a relative one
   would not work). Returns 1 on success. */
int events_open(const char *job_id);

/* Closes the journal. */
void events_close(void);

/* --- writing the events --- */

void event_graph_start(const char *entry_node, int total_nodes);

void event_node_start(const char *node, const char *provider, const char *model);

/* duration_ms: how long the node took. cost_micro: how much it cost. */
void event_node_done(const char *node, long input_tokens, long output_tokens,
                     long cost_micro_usd, long duration_ms);

void event_node_failed(const char *node, const char *error);

/* A message passes from `from` to `to`. bytes = the message size (the frontend
   can make the pixel thicker if the message is larger). `condition` is the marker
   that activated the conditional edge (or "" if unconditional). */
void event_edge_flow(const char *from, const char *to, long bytes,
                     const char *condition);

void event_graph_done(long total_cost_micro_usd, long total_ms);
void event_graph_failed(const char *error);

/* --- events from inside the agent (the live journal below the graph) --- */

/* The agent sent a request to the model (the Nth iteration of the loop). */
void event_agent_thinking(const char *node, int iteration);

/* The agent requests a tool. `input` is a short summary of the arguments
   (e.g. "path=src/calc.c" or "gcc -Wall -c src/calc.c"). */
void event_tool_call(const char *node, const char *tool, const char *input);

/* The tool's result. ok=1 success, 0 error. `summary` = the first lines. */
void event_tool_result(const char *node, const char *tool, int ok,
                       const char *summary, long bytes);

/* The agent's text response (what it "says"). */
void event_agent_says(const char *node, const char *text);

/*
 * Human-in-the-loop approval, for agents running as a web job.
 *
 * The agent cannot ask on stdin here: it runs in a grandchild process with no
 * terminal. Instead it announces what it wants to do, then blocks until you
 * decide in the browser.
 *
 * The decision travels back through a file (jobs/<id>.approval) because, after
 * fork, the server and the agent share no memory. The server writes it from the
 * API handler; the agent polls for it.
 *
 * Returns 1 if allowed, 0 if denied or if no decision arrived in time.
 */
void event_approval_needed(const char *node, const char *tool, const char *input);
void event_approval_done(const char *node, const char *tool, int allowed);
int  approval_wait(const char *node, const char *tool, const char *input);

/* True when this process can ask through the web UI at all (i.e. it was
   started as a job and knows where the approval file lives). */
int  approval_channel_available(void);

/*
 * IMPORTANT: ai_loop runs in the AGENT's process (a grandchild of the server),
 * which does not have the journal open. The server passes it the path through
 * the PIXELGO_EVENTS_FILE environment variable, and events_open_from_env()
 * reads it. If the variable is missing (CLI run), events are silently ignored.
 */
int events_open_from_env(void);

/* The current node name - we set it once, so we do not pass it on every call. */
void events_set_node(const char *node);

#endif
