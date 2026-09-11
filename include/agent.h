#ifndef AGENT_H
#define AGENT_H

#include <sys/types.h>

#define MAX_TOOLS 16
#define MAX_ALLOWLIST 16
#define MAX_STR 256
#define MAX_PATH_LEN 512

typedef enum {
    AGENT_TYPE_WORKER,   /* generic isolated process, fixed command */
    AGENT_TYPE_AI        /* agent driven by a model via API, prompt + tools */
} agent_type_t;

/* The LLM provider of an AI agent. Each agent uses EXACTLY one.
   The API key comes from the environment, depending on the provider (see provider.c). */
typedef enum {
    LLM_PROVIDER_ANTHROPIC,
    LLM_PROVIDER_OPENAI,
    LLM_PROVIDER_GEMINI,
    LLM_PROVIDER_UNKNOWN
} llm_provider_id_t;

typedef enum {
    AGENT_STOPPED,
    AGENT_RUNNING,
    AGENT_FAILED,
    AGENT_TIMEOUT        /* force-terminated because it exceeded the time limit */
} agent_status_t;

/*
 * Resource limits applied to the agent's process (via setrlimit in the child).
 * Zero = "impose no limit" for that field (uses the system default).
 * They are common to both agent types - hence, in the base structure.
 *
 * Extensibility: when we want a new limit (e.g. number of processes), we add a
 * field here + a line in apply_rlimits() in agent_run.c + parsing in
 * workspace.c. Nothing else changes.
 */
typedef struct {
    long cpu_seconds;    /* RLIMIT_CPU - max CPU time                        */
    long mem_bytes;      /* RLIMIT_AS  - max virtual memory                  */
    long fsize_bytes;    /* RLIMIT_FSIZE - max size of a written file        */
    long timeout_seconds;/* wall-clock: the parent kills the agent after this */
} resource_limits_t;

/* Config for a worker-type agent */
typedef struct {
    char command[MAX_PATH_LEN];
    char args[MAX_TOOLS][MAX_STR]; /* we reuse MAX_TOOLS as a simple "max number of arguments" */
    int  argc;
} worker_config_t;

/* Config for an AI-type agent */
typedef struct {
    llm_provider_id_t provider;  /* anthropic / openai / gemini */
    char model[MAX_STR];
    char system_prompt[2048];
    char tools[MAX_TOOLS][MAX_STR];
    int  tool_count;
    char run_command_allowlist[MAX_ALLOWLIST][MAX_STR];
    int  allowlist_count;

    /*
     * Tools that require the human to approve each call before it runs.
     * Empty = the agent runs unsupervised (the previous behavior).
     *
     * Set this per agent, per tool: a reviewer that only reads files needs no
     * approval, while a coder that writes files and runs commands probably
     * does. Typically "write_file,run_command".
     */
    char approval_tools[MAX_TOOLS][MAX_STR];
    int  approval_count;

    /* ===== DATA REQUEST APPROVAL GATEWAY (NEW) ===== */
    /*
     * approve_data_threshold_bytes: THREE states, not two -
     *   -1  -> not specified for this agent; inherits PIXELGO_DATA_THRESHOLD
     *          (the global default) at the point of the check. This is the
     *          default when no --data-threshold flag is given at all.
     *    0  -> explicitly disabled ("--data-threshold off"). Overrides the
     *          global default - this agent is never checked, even if a
     *          global default is set.
     *   >0  -> an explicit threshold in bytes for this agent, overriding
     *          the global default.
     *
     * The check runs before EVERY provider call for the rest of the run,
     * not just the first one that crosses the threshold - there is no
     * separate "per request" flag because that is already the only
     * behavior this gateway has.
     */
    long approve_data_threshold_bytes;
    /* ===== END DATA APPROVAL ===== */
} ai_config_t;

typedef struct {
    char id[MAX_STR];
    char dir[MAX_PATH_LEN];       /* its own directory, sandbox root */

    /*
     * The workspace's SHARED directory (workspaces/<ws>/shared).
     * All agents in a workspace see it - this way they can work on the SAME code.
     *
     * Why it is needed: without it, each agent has its own copy of the files. A
     * reviewer checking what a coder fixed would read its own stale copy and
     * report that nothing changed -> infinite loop.
     *
     * The sandbox allows access to dir OR to shared_dir, nothing else.
     */
    char shared_dir[MAX_PATH_LEN];
    agent_type_t type;

    union {
        worker_config_t worker;
        ai_config_t ai;
    } cfg;

    /* resource limits (common to both types) */
    resource_limits_t limits;

    /* runtime state */
    pid_t pid;
    agent_status_t status;
    int   exit_code;             /* the last exit code (informational)        */
    int   restart_count;         /* how many times it was restarted           */
} agent_t;

#endif
