#ifndef JOBS_H
#define JOBS_H

#include "agent.h"
#include <sys/types.h>
#include <stddef.h>

/*
 * Job system: ASYNCHRONOUS and ISOLATED running of agents.
 *
 * The problem it solves:
 *   The first version of the web server ran the agent IN THE SERVER PROCESS
 *   (ai_loop_run_capture directly in the handler). Two serious consequences:
 *     1. an agent that crashed took the server with it (the CLI does not have
 *        this problem, because there the agent runs in a fork)
 *     2. the HTTP request stayed blocked for tens of seconds while the model worked
 *
 * The solution:
 *   Each agent run becomes a JOB:
 *     - the server does fork(); the child runs the agent (isolated, with sandbox +
 *       rlimit, exactly as in the CLI) and writes the result to a file
 *     - the parent returns a job_id IMMEDIATELY (HTTP responds in milliseconds)
 *     - the client polls /api/jobs/<id> until the job is done
 *
 * Job state is kept on disk (jobs/<id>.json), NOT in memory. Why:
 *   - the child (a different process) must be able to write the result somewhere
 *     visible to the parent; memory is not shared after fork
 *   - jobs survive a server restart
 */

#define JOBS_DIR "jobs"
#define JOB_ID_LEN 32
#define JOB_RESULT_MAX 65536

typedef enum {
    JOB_RUNNING,
    JOB_DONE,
    JOB_FAILED
} job_status_t;

typedef struct {
    char id[JOB_ID_LEN];
    job_status_t status;
    pid_t pid;                    /* the child process running the job  */
    char kind[16];                /* "chat" | "flow"                    */
    char result[JOB_RESULT_MAX];  /* the agent's response / the output  */
    char error[512];
} job_t;

/* Initializes the job system (creates the directory). */
int jobs_init(void);

/*
 * Starts a chat job: fork + ai_loop in the child.
 * Returns 1 on success and writes the id into out_id. Does not block.
 */
int job_start_chat(const char *ws_name, const char *agent_id,
                   const char *message, char out_id[JOB_ID_LEN]);

/*
 * Starts a graph job: fork + orchestrator in the child.
 */
int job_start_flow(const char *ws_name, const char *flow_file,
                   const char *task, char out_id[JOB_ID_LEN]);

/* Reads a job's state from disk. Returns 1 if the job exists.
   If the child process has terminated but the file still says RUNNING (the child
   died abruptly), marks the job as FAILED. */
int job_get(const char *id, job_t *out);

/* Deletes old jobs (cleanup). Returns how many it deleted. */
int jobs_cleanup(int max_age_seconds);

/* The ABSOLUTE path to a job's event journal (jobs/<id>.events).
   Absolute because the job chdirs into sandboxes. Returns 1 on success. */
int jobs_events_path(const char *job_id, char *out, size_t out_size);

/* Where the human's approval decision is written (jobs/<id>.approval).
   The agent polls this file while it is blocked waiting for you. */
int jobs_approval_path(const char *job_id, char *out, size_t out_size);

#endif
