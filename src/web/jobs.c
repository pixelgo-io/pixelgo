#include "jobs.h"
#include "workspace.h"
#include "ai_loop.h"
#include "flow.h"
#include "orchestrator.h"
#include "tools.h"
#include "json_util.h"
#include "usage.h"
#include "events.h"
#include "provider.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define BASE_DIR "workspaces"

/* --- on-disk state helpers --- */

/*
 * The ABSOLUTE path to jobs/. Essential: the child chdir()s into the agent's
 * sandbox, so a relative path "jobs/x.json" would write into the agent's
 * directory, not the server's. We resolve it once, at init.
 */
static char g_jobs_dir[MAX_PATH_LEN] = "";

static void job_path(const char *id, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s.json", g_jobs_dir, id);
}

/* Generates a unique id (timestamp + pid + counter). */
static void generate_id(char out[JOB_ID_LEN]) {
    static int counter = 0;
    snprintf(out, JOB_ID_LEN, "%lx%x%x",
             (unsigned long)time(NULL), (unsigned)getpid(), (unsigned)(counter++));
}

/* Writes the job's state to disk (atomic: write temp + rename, so the parent does
   not read a half-written file). */
static int job_write(const job_t *job) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", job->id);
    cJSON_AddStringToObject(o, "status",
        job->status == JOB_RUNNING ? "running" :
        job->status == JOB_DONE    ? "done" : "failed");
    cJSON_AddNumberToObject(o, "pid", job->pid);
    cJSON_AddStringToObject(o, "kind", job->kind);
    cJSON_AddStringToObject(o, "result", job->result);
    cJSON_AddStringToObject(o, "error", job->error);

    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) return 0;

    char tmp[MAX_PATH_LEN + 64], final[MAX_PATH_LEN + 64];
    snprintf(tmp, sizeof(tmp), "%s/%s.tmp", g_jobs_dir, job->id);
    job_path(job->id, final, sizeof(final));

    FILE *f = fopen(tmp, "w");
    if (!f) { free(text); return 0; }
    fputs(text, f);
    fclose(f);
    free(text);

    /* rename is atomic on the same filesystem */
    if (rename(tmp, final) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
}

int jobs_init(void) {
    if (g_jobs_dir[0]) return 1;   /* already initialized */

    struct stat st;
    if (stat(JOBS_DIR, &st) != 0) {
        if (mkdir(JOBS_DIR, 0755) != 0 && errno != EEXIST) {
            LOG_E("jobs: cannot create directory '%s': %s", JOBS_DIR, strerror(errno));
            return 0;
        }
    }

    /* We resolve the absolute path NOW, while we are still in the server's
       startup directory. The child will chdir into the sandbox and would no
       longer find "jobs/" relatively. */
    if (!realpath(JOBS_DIR, g_jobs_dir)) {
        LOG_E("jobs: cannot resolve the absolute path of '%s'", JOBS_DIR);
        g_jobs_dir[0] = 0;
        return 0;
    }
    LOG_D("jobs: director de job-uri: %s", g_jobs_dir);
    return 1;
}

int job_get(const char *id, job_t *out) {
    /* safe id: no '/' or '..' (would read files outside jobs/) */
    if (!id || !id[0] || strchr(id, '/') || strstr(id, "..")) return 0;

    if (!jobs_init()) return 0;

    char path[MAX_PATH_LEN + 64];
    job_path(id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[JOB_RESULT_MAX + 2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);

    cJSON *o = cJSON_Parse(buf);
    if (!o) return 0;

    memset(out, 0, sizeof(*out));
    json_get_string(o, "id", out->id, sizeof(out->id));
    json_get_string(o, "kind", out->kind, sizeof(out->kind));
    json_get_string(o, "result", out->result, sizeof(out->result));
    json_get_string(o, "error", out->error, sizeof(out->error));

    char status[16];
    json_get_string(o, "status", status, sizeof(status));
    out->status = strcmp(status, "done") == 0   ? JOB_DONE :
                  strcmp(status, "failed") == 0 ? JOB_FAILED : JOB_RUNNING;

    cJSON *pid = cJSON_GetObjectItemCaseSensitive(o, "pid");
    out->pid = cJSON_IsNumber(pid) ? (pid_t)pid->valuedouble : 0;
    cJSON_Delete(o);

    /*
     * If the file says RUNNING, we check that the process is really still alive.
     * A child that crashed (segfault, OOM-kill) did not get to write the result -
     * without this check, the job would stay "running" forever.
     */
    if (out->status == JOB_RUNNING && out->pid > 0) {
        if (kill(out->pid, 0) != 0 && errno == ESRCH) {
            out->status = JOB_FAILED;
            snprintf(out->error, sizeof(out->error),
                     "the job process terminated unexpectedly (crash?)");
            job_write(out);   /* persist the outcome */
            LOG_W("jobs: job %s died without writing a result", out->id);
        }
    }
    return 1;
}

/* --- starting jobs (fork) --- */

/* The common part: creates the job, marks it RUNNING, forks.
   Returns: 0 in the CHILD (which must do the work), 1 in the PARENT (success),
   -1 on error. */
static int job_fork(const char *kind, job_t *job, char out_id[JOB_ID_LEN]) {
    if (!jobs_init()) return -1;

    memset(job, 0, sizeof(*job));
    generate_id(job->id);
    snprintf(job->kind, sizeof(job->kind), "%s", kind);
    job->status = JOB_RUNNING;
    job->pid = 0;

    /* we write the initial state BEFORE the fork, so it exists as soon as the
       parent returns the id to the client */
    if (!job_write(job)) return -1;

    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        LOG_E("jobs: fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* --- CHILD: runs the job isolated --- */
        /* We reset SIGCHLD: we inherit the server's handler, which would steal our
           children (the agents, and further down run_command's processes). */
        signal(SIGCHLD, SIG_DFL);
        job->pid = getpid();
        return 0;
    }

    /* --- PARENT: we record the pid and return immediately --- */
    job->pid = pid;
    job_write(job);
    snprintf(out_id, JOB_ID_LEN, "%s", job->id);
    LOG_I("jobs: started job %s (%s), pid=%d", job->id, kind, (int)pid);
    return 1;
}

/* The child finishes the job: writes the result and exits. Does NOT return. */
static void job_finish_child(job_t *job, int ok, const char *result, const char *err) {
    job->status = ok ? JOB_DONE : JOB_FAILED;
    if (result) snprintf(job->result, sizeof(job->result), "%s", result);
    if (err)    snprintf(job->error, sizeof(job->error), "%s", err);
    job_write(job);

    fflush(stdout);
    fflush(stderr);
    _exit(ok ? 0 : 1);
}

int job_start_chat(const char *ws_name, const char *agent_id,
                   const char *message, char out_id[JOB_ID_LEN]) {
    job_t job;
    int r = job_fork("chat", &job, out_id);
    if (r < 0) return 0;
    if (r == 1) return 1;   /* parent: done */

    /* --- from here on it is the CHILD --- */

    char ws_path[MAX_PATH_LEN];
    snprintf(ws_path, sizeof(ws_path), "%s/%s", BASE_DIR, ws_name);

    workspace_t ws;
    if (!workspace_load(&ws, ws_path))
        job_finish_child(&job, 0, "", "no such workspace");

    agent_t *agent = workspace_find_agent(&ws, agent_id);
    if (!agent)
        job_finish_child(&job, 0, "", "no such agent");
    if (agent->type != AGENT_TYPE_AI)
        job_finish_child(&job, 0, "", "chat requires an AI agent");

    /* Sandbox: we enter the agent's directory, exactly as in the CLI (agent_start).
       We make the path ABSOLUTE first, so the tools' sandbox and history work
       correctly after chdir (agent->dir is relative otherwise). */
    char abs_dir[MAX_PATH_LEN];
    if (realpath(agent->dir, abs_dir))
        snprintf(agent->dir, MAX_PATH_LEN, "%s", abs_dir);

    char abs_shared[MAX_PATH_LEN];
    if (agent->shared_dir[0] && realpath(agent->shared_dir, abs_shared))
        snprintf(agent->shared_dir, MAX_PATH_LEN, "%s", abs_shared);

    /* We work in the SHARED directory (that is where the code is). The agent's
       private files are written through absolute paths into agent->dir. */
    const char *workdir = agent->shared_dir[0] ? agent->shared_dir : agent->dir;

    if (chdir(workdir) != 0)
        job_finish_child(&job, 0, "", "cannot enter the working directory");

    /* We are in the agent's sandbox, so the relative paths in ai_loop
       (_history.json, _output.txt) work correctly. The job state is written via
       g_jobs_dir, which is ABSOLUTE - so the chdir does not matter. */

    /* The journal doubles as the approval channel: without it, an agent that
       needs approval would have nowhere to ask and would deny itself. */
    char evpath[1024];
    if (jobs_events_path(job.id, evpath, sizeof(evpath)))
        setenv("PIXELGO_EVENTS_FILE", evpath, 1);

    static char reply[TOOL_RESULT_MAX];
    int rc = ai_loop_run_capture(agent, message, 1, reply, sizeof(reply));

    if (rc == 0)
        job_finish_child(&job, 1, reply, "");
    else
        job_finish_child(&job, 0, "",
            "the model call failed (check the API key and the log)");

    _exit(1);   /* not reached */
}

int job_start_flow(const char *ws_name, const char *flow_file,
                   const char *task, char out_id[JOB_ID_LEN]) {
    job_t job;
    int r = job_fork("flow", &job, out_id);
    if (r < 0) return 0;
    if (r == 1) return 1;

    /* --- CHILD --- */

    char ws_path[MAX_PATH_LEN];
    snprintf(ws_path, sizeof(ws_path), "%s/%s", BASE_DIR, ws_name);

    workspace_t ws;
    if (!workspace_load(&ws, ws_path))
        job_finish_child(&job, 0, "", "no such workspace");

    flow_graph_t g;
    if (!flow_load(&g, flow_file))
        job_finish_child(&job, 0, "", "could not load the graph");
    if (!flow_validate(&g, &ws))
        job_finish_child(&job, 0, "", "the graph does not match the workspace");

    /* We open the event journal: the orchestrator will write to it as it runs, and
       the frontend reads it to animate the graph in real time. */
    events_open(job.id);

    /*
     * The agents run in GRANDCHILD processes (orchestrator -> fork -> agent),
     * which do not inherit an open FILE*. We pass them the path through the
     * environment; ai_loop reads it with events_open_from_env() and writes to the
     * same journal (append, so several processes can write without clobbering).
     */
    char evpath[1024];
    if (jobs_events_path(job.id, evpath, sizeof(evpath)))
        setenv("PIXELGO_EVENTS_FILE", evpath, 1);

    int rc = orchestrator_run(&ws, &g, (task && task[0]) ? task : NULL);

    events_close();

    /* we collect the output AND the cost of each node */
    cJSON *outputs = cJSON_CreateArray();
    long total_in = 0, total_out = 0, total_cost = 0;

    for (int i = 0; i < g.node_count; i++) {
        agent_t *a = workspace_find_agent(&ws, g.nodes[i]);
        if (!a) continue;

        cJSON *no = cJSON_CreateObject();
        cJSON_AddStringToObject(no, "node", g.nodes[i]);

        char path[MAX_PATH_LEN + 16];
        snprintf(path, sizeof(path), "%s/_output.txt", a->dir);
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            fclose(f);
            cJSON_AddStringToObject(no, "output", buf);
        } else {
            cJSON_AddStringToObject(no, "output", "");
        }

        /* the node's cost, from _usage.json */
        snprintf(path, sizeof(path), "%s/_usage.json", a->dir);
        FILE *uf = fopen(path, "r");
        if (uf) {
            char ubuf[1024];
            size_t un = fread(ubuf, 1, sizeof(ubuf) - 1, uf);
            ubuf[un] = 0;
            fclose(uf);

            cJSON *u = cJSON_Parse(ubuf);
            if (u) {
                cJSON *in   = cJSON_GetObjectItemCaseSensitive(u, "input_tokens");
                cJSON *o    = cJSON_GetObjectItemCaseSensitive(u, "output_tokens");
                cJSON *cost = cJSON_GetObjectItemCaseSensitive(u, "cost_micro_usd");
                char model[MAX_STR] = "";
                json_get_string(u, "model", model, sizeof(model));

                long li = cJSON_IsNumber(in)   ? (long)in->valuedouble   : 0;
                long lo = cJSON_IsNumber(o)    ? (long)o->valuedouble    : 0;
                long lc = cJSON_IsNumber(cost) ? (long)cost->valuedouble : 0;

                cJSON_AddStringToObject(no, "model", model);
                cJSON_AddNumberToObject(no, "input_tokens", (double)li);
                cJSON_AddNumberToObject(no, "output_tokens", (double)lo);
                cJSON_AddNumberToObject(no, "cost_micro_usd", (double)lc);

                total_in += li; total_out += lo; total_cost += lc;
                cJSON_Delete(u);
            }
        }
        cJSON_AddItemToArray(outputs, no);
    }

    /* We package the outputs AND the total cost. */
    cJSON *wrapper = cJSON_CreateObject();
    cJSON_AddItemToObject(wrapper, "nodes", outputs);
    cJSON_AddNumberToObject(wrapper, "total_input_tokens", (double)total_in);
    cJSON_AddNumberToObject(wrapper, "total_output_tokens", (double)total_out);
    cJSON_AddNumberToObject(wrapper, "total_cost_micro_usd", (double)total_cost);

    static char result[JOB_RESULT_MAX];
    if (!json_serialize(wrapper, result, sizeof(result)))
        snprintf(result, sizeof(result), "{}");
    cJSON_Delete(wrapper);

    job_finish_child(&job, rc == 0, result,
                     rc == 0 ? "" : "the graph failed (see the log)");
    _exit(1);
}

int jobs_events_path(const char *job_id, char *out, size_t out_size) {
    if (!jobs_init()) return 0;
    if (!job_id || !job_id[0]) return 0;
    if (strchr(job_id, '/') || strstr(job_id, "..")) return 0;   /* safe id */

    snprintf(out, out_size, "%s/%s.events", g_jobs_dir, job_id);
    return 1;
}

int jobs_approval_path(const char *job_id, char *out, size_t out_size) {
    if (!jobs_init()) return 0;
    if (!job_id || !job_id[0]) return 0;
    if (strchr(job_id, '/') || strstr(job_id, "..")) return 0;   /* safe id */

    snprintf(out, out_size, "%s/%s.approval", g_jobs_dir, job_id);
    return 1;
}

int jobs_cleanup(int max_age_seconds) {
    if (!jobs_init()) return 0;

    DIR *d = opendir(g_jobs_dir);
    if (!d) return 0;

    time_t now = time(NULL);
    int removed = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        char path[MAX_PATH_LEN + 300];
        snprintf(path, sizeof(path), "%s/%s", g_jobs_dir, e->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (now - st.st_mtime > max_age_seconds) {
            remove(path);
            removed++;
        }
    }
    closedir(d);
    return removed;
}
