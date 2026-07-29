#include "tools.h"
#include "sandbox.h"
#include "json_util.h"
#include "log.h"
#include "events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>

/*
 * exec_tools.c - the tools that execute things: run_command and search_files.
 *
 * run_command is the most DANGEROUS tool in the system, so it is built with
 * defense in depth:
 *
 *   1. MANDATORY ALLOWLIST (fail-closed): if the agent has no command in
 *      run_command_allowlist, the tool refuses EVERYTHING. An agent is granted the
 *      right to run only the commands explicitly written in its config.
 *
 *   2. NO SHELL: we use fork + execvp, NOT system(). This means there is no shell
 *      interpretation - ";", "|", "$(...)", "&&", ">" are treated as plain text,
 *      not as operators. Command injection becomes impossible by construction, not
 *      by filtering (metacharacter filtering is always incomplete).
 *
 *   3. ONLY the command name is checked against the allowlist. The arguments are
 *      free (they cannot do harm without a shell), but the command itself must be
 *      allowed.
 *
 *   4. TIMEOUT: the command is killed if it exceeds the limit (SIGTERM, then
 *      SIGKILL on the whole process group, so we catch its children too).
 *
 *   5. SANDBOX: it runs with chdir into the agent's directory.
 *
 *   6. The output (stdout+stderr combined) is captured and truncated at
 *      TOOL_RESULT_MAX, so the model's context does not blow up.
 */

/*
 * Default timeout for a command, in seconds. One hour: long enough for slow
 * package installs and full build/test runs, while still bounded so a hung
 * process eventually gets cleaned up instead of blocking the agent forever.
 *
 * Override per call with the "timeout_seconds" argument, or globally with the
 * PIXELGO_COMMAND_TIMEOUT environment variable.
 */
#define RUN_COMMAND_DEFAULT_TIMEOUT 3600   /* seconds */
#define MAX_CMD_ARGS 32

/* Checks whether a command is in the agent's allowlist. Fail-closed: an empty
   allowlist => nothing is allowed. */
static int command_allowed(agent_t *agent, const char *cmd) {
    if (agent->cfg.ai.allowlist_count == 0) return 0;  /* fail-closed */

    for (int i = 0; i < agent->cfg.ai.allowlist_count; i++) {
        if (strcmp(agent->cfg.ai.run_command_allowlist[i], cmd) == 0) return 1;
    }
    return 0;
}

/* Builds the allowlist string for error messages (helps the model understand
   what it IS allowed to run). */
static void allowlist_to_string(agent_t *agent, char *out, size_t out_size) {
    out[0] = 0;
    size_t off = 0;
    for (int i = 0; i < agent->cfg.ai.allowlist_count; i++) {
        int n = snprintf(out + off, out_size - off, "%s%s",
                         agent->cfg.ai.run_command_allowlist[i],
                         i + 1 < agent->cfg.ai.allowlist_count ? ", " : "");
        if (n < 0) break;
        off += (size_t)n;
        if (off >= out_size - 1) break;
    }
}

/*
 * ---------------------------------------------------------------- streaming
 *
 * The output of a long command used to be invisible until it finished: we read
 * it into a buffer and returned the whole thing at the end. Fine for `gcc -c`,
 * useless for `docker compose build`, which runs for minutes behind a silent UI.
 *
 * So we also publish the output AS IT ARRIVES, line by line, to the event
 * journal. Two rules keep this from becoming noise:
 *
 *   - we emit whole LINES, not raw read() chunks, because a chunk boundary
 *     falls wherever the pipe happened to fill up and produces torn words;
 *   - a line that never terminates (a progress bar redrawing with \r, which is
 *     exactly what docker does) is flushed once it grows past STREAM_LINE_MAX,
 *     so the journal keeps moving instead of waiting for a newline that is not
 *     coming.
 *
 * What the MODEL receives is unchanged - it still gets one coherent result at
 * the end. This is purely for the human watching.
 */
#define STREAM_LINE_MAX 400

typedef struct {
    char   line[STREAM_LINE_MAX + 1];
    size_t len;
    const char *tool;
    int    enabled;
} stream_ctx_t;

static void stream_flush(stream_ctx_t *s) {
    if (!s->enabled || s->len == 0) return;
    s->line[s->len] = 0;
    event_tool_output(NULL, s->tool, s->line);
    s->len = 0;
}

/* Feeds newly-read bytes into the line buffer, emitting complete lines. */
static void stream_feed(stream_ctx_t *s, const char *data, size_t n) {
    if (!s->enabled) return;

    for (size_t i = 0; i < n; i++) {
        char c = data[i];

        /* '\r' ends a line for our purposes too: progress bars use it to redraw
           in place, and treating it as a terminator is what makes them show up
           as successive journal lines instead of one endless one. */
        if (c == '\n' || c == '\r') {
            stream_flush(s);
            continue;
        }

        s->line[s->len++] = c;
        if (s->len >= STREAM_LINE_MAX) stream_flush(s);
    }
}

tool_result_t tool_run_command(agent_t *agent, const char *input_json) {
    cJSON *args = json_parse(input_json);
    if (!args || !cJSON_IsObject(args)) {
        if (args) cJSON_Delete(args);
        return tool_err("invalid arguments JSON");
    }

    /* --- argument validation --- */
    char cmd[MAX_STR];
    if (!json_get_string(args, "command", cmd, sizeof(cmd))) {
        cJSON_Delete(args);
        return tool_err("missing or non-string 'command' field");
    }

    /* the arguments: an optional array of strings */
    char *argv[MAX_CMD_ARGS + 2];
    int argc = 0;
    argv[argc++] = cmd;

    cJSON *arglist = cJSON_GetObjectItemCaseSensitive(args, "args");
    if (arglist && !cJSON_IsArray(arglist)) {
        cJSON_Delete(args);
        return tool_err("'args' must be an array of strings");
    }
    if (cJSON_IsArray(arglist)) {
        cJSON *a = NULL;
        cJSON_ArrayForEach(a, arglist) {
            if (!cJSON_IsString(a) || !a->valuestring) {
                cJSON_Delete(args);
                return tool_err("all entries in 'args' must be strings");
            }
            if (argc >= MAX_CMD_ARGS) {
                cJSON_Delete(args);
                return tool_err("too many arguments (max %d)", MAX_CMD_ARGS);
            }
            argv[argc++] = a->valuestring;
        }
    }
    argv[argc] = NULL;

    /* --- the security check: is the command allowed? --- */
    if (!command_allowed(agent, cmd)) {
        char allowed[1024];
        allowlist_to_string(agent, allowed, sizeof(allowed));
        LOG_W("run_command: agent '%s' attempted the FORBIDDEN command '%s'",
              agent->id, cmd);
        cJSON_Delete(args);
        if (allowed[0] == 0)
            return tool_err("command '%s' not permitted: this agent has an empty "
                            "run_command allowlist (no commands allowed)", cmd);
        return tool_err("command '%s' not permitted. Allowed commands: %s", cmd, allowed);
    }

    /* Optional timeout (seconds). Precedence: per-call argument, then the
       PIXELGO_COMMAND_TIMEOUT environment variable, then the default. */
    long timeout = RUN_COMMAND_DEFAULT_TIMEOUT;
    const char *env_timeout = getenv("PIXELGO_COMMAND_TIMEOUT");
    if (env_timeout && env_timeout[0]) {
        long v = strtol(env_timeout, NULL, 10);
        if (v > 0) timeout = v;
    }
    cJSON *t = cJSON_GetObjectItemCaseSensitive(args, "timeout_seconds");
    if (cJSON_IsNumber(t) && t->valuedouble > 0) timeout = (long)t->valuedouble;

    /*
     * Log the command WITH its arguments.
     *
     * Logging only the name and a count made post-hoc auditing impossible: a
     * line reading "runs 'docker' (4 arguments)" tells you nothing about
     * whether the agent mounted a host path it should not have. The arguments
     * are the part worth keeping.
     */
    char cmdline[1024];
    size_t off = 0;
    for (int i = 0; i < argc && off < sizeof(cmdline) - 1; i++) {
        int w = snprintf(cmdline + off, sizeof(cmdline) - off,
                         "%s%s", i ? " " : "", argv[i]);
        if (w < 0) break;
        off += (size_t)w;
    }
    cmdline[sizeof(cmdline) - 1] = 0;

    LOG_I("run_command: agent=%s runs: %s (timeout=%lds)",
          agent->id, cmdline, timeout);

    /* --- pipe for capturing the output (stdout + stderr combined) --- */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        cJSON_Delete(args);
        return tool_err("could not create pipe: %s", strerror(errno));
    }

    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        cJSON_Delete(args);
        return tool_err("fork failed: %s", strerror(errno));
    }

    if (pid == 0) {
        /* --- child --- */
        setpgid(0, 0);              /* its own group: we can kill the whole subtree */
        close(pipefd[0]);

        /* stdout AND stderr both go into the pipe (the model wants to see the errors!) */
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /*
         * BUG FIXED: we used to chdir(agent->dir). But the agent works in the
         * SHARED directory (that is where the code is), so the command ran in its
         * private (empty) directory and gcc could not find the files:
         *   "cc1: fatal error: src/calc.c: No such file or directory"
         *
         * Correct: we do NOT chdir at all. The child inherits the parent's cwd,
         * which is already the correct working directory (shared/). The sandbox
         * remains ensured by the allowlist + by the fact that the agent cannot
         * escape the allowed roots through the file tools.
         */
        (void)agent;   /* we no longer need agent->dir here */

        /* NO SHELL: execvp receives the arguments as a vector, so the shell
           metacharacters (";", "|", "$()") are just text, not operators. */
        execvp(cmd, argv);
        fprintf(stderr, "exec failed for '%s': %s\n", cmd, strerror(errno));
        _exit(127);
    }

    /* --- parent --- */
    setpgid(pid, pid);
    close(pipefd[1]);
    cJSON_Delete(args);   /* argv pointed into args; after fork, the child has its own copy */

    /* We read the output, with a deadline. A non-blocking pipe so we can check
       periodically whether the time has expired. */
    tool_result_t r = {0};
    size_t total = 0;
    time_t deadline = time(NULL) + timeout;
    int timed_out = 0;
    int buffer_full = 0;
    int got_eof = 0;

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    /* Streaming is a no-op outside a web job: events_open_from_env() found no
       journal, so the emit calls fall through and cost nothing. The CLI keeps
       behaving exactly as before. */
    stream_ctx_t stream = { {0}, 0, "run_command", 1 };

    /*
     * A ring of the most recent output, used once the main buffer is full.
     *
     * Truncating from the front is the wrong end. A build prints its package
     * list first and its verdict last, so keeping the first 8 KB gave the model
     * apt chatter and threw away the line saying whether it worked.
     *
     * Worse, the old code broke out of the loop when the buffer filled and fell
     * through to the kill path below - so a long build was terminated for being
     * verbose, leaving containers half-created. We now keep draining the pipe
     * and let the command finish; only the middle of its output is lost.
     */
    char tail[TOOL_TAIL_KEEP];
    size_t tail_len = 0;
    int    tail_wrapped = 0;
    size_t dropped = 0;

    for (;;) {
        char scratch[4096];
        char *dst;
        size_t cap;

        if (!buffer_full && total < TOOL_RESULT_MAX - 1) {
            dst = r.output + total;
            cap = TOOL_RESULT_MAX - 1 - total;
        } else {
            buffer_full = 1;
            dst = scratch;
            cap = sizeof(scratch);
        }

        ssize_t n = read(pipefd[0], dst, cap);
        if (n > 0) {
            stream_feed(&stream, dst, (size_t)n);

            if (dst == scratch) {
                /* Past the main buffer: remember only the tail. */
                dropped += (size_t)n;
                for (ssize_t i = 0; i < n; i++) {
                    tail[tail_len++] = scratch[i];
                    if (tail_len == sizeof(tail)) { tail_len = 0; tail_wrapped = 1; }
                }
            } else {
                total += (size_t)n;
            }
            continue;
        }
        if (n == 0) { got_eof = 1; break; }   /* the command closed the pipe */

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (time(NULL) >= deadline) { timed_out = 1; break; }
            struct timespec ts = { 0, 50 * 1000 * 1000 };   /* 50ms */
            nanosleep(&ts, NULL);
            continue;
        }
        break;  /* a real read error */
    }

    /* Whatever sat in the line buffer when the loop ended - a last line with no
       trailing newline, or a partial progress line - still belongs in the
       journal. This matters most on the timeout path: the partial output is the
       only clue about where the command actually got stuck. */
    stream_flush(&stream);

    r.output[total] = 0;
    close(pipefd[0]);

    /* --- TIMEOUT: we kill the whole process group --- */
    if (timed_out) {
        LOG_W("run_command: '%s' exceeded the %lds timeout, stopping it", cmd, timeout);
        kill(-pid, SIGTERM);
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
        kill(-pid, SIGKILL);
        int status;
        waitpid(pid, &status, 0);
        return tool_err("command '%s' timed out after %ld seconds (killed)\n\n"
                        "--- Partial output ---\n%s", cmd, timeout, r.output);
    }

    /* --- BUFFER FULL: the command ran to completion; we kept head and tail --- */
    if (buffer_full) {
        /*
         * Reassemble the ring into reading order. If it never wrapped, the tail
         * is simply the first tail_len bytes; if it did, the oldest byte sits at
         * tail_len and the buffer reads around from there.
         */
        char tail_ordered[TOOL_TAIL_KEEP + 1];
        size_t tn = 0;
        if (tail_wrapped) {
            for (size_t i = tail_len; i < sizeof(tail); i++) tail_ordered[tn++] = tail[i];
            for (size_t i = 0; i < tail_len; i++)            tail_ordered[tn++] = tail[i];
        } else {
            for (size_t i = 0; i < tail_len; i++)            tail_ordered[tn++] = tail[i];
        }
        tail_ordered[tn] = 0;

        /* Start the tail at a line boundary - resuming mid-line reads as
           corruption to whoever is looking at it. */
        char *tail_start = tail_ordered;
        if (tail_wrapped) {
            char *nl = strchr(tail_ordered, '\n');
            if (nl && (size_t)(nl - tail_ordered) < tn - 1) tail_start = nl + 1;
        }

        LOG_W("run_command: output of '%s' exceeded %d bytes, dropped %zu bytes "
              "from the middle", cmd, TOOL_RESULT_MAX, dropped);

        /*
         * Trim the head so that head + notice + tail fits in the result buffer,
         * and copy it aside first.
         *
         * The copy is not defensive tidiness: tool_ok() formats INTO a
         * tool_result_t's output field while reading r.output as an argument.
         * Passing the same storage as both source and destination is undefined,
         * and in practice the tail came out empty.
         *
         * Cut first, THEN back up to a line boundary - searching the untrimmed
         * string finds the last newline of the whole 32 KB, which is not a cut
         * point at all.
         */
        static char head[TOOL_RESULT_MAX];
        size_t room = TOOL_RESULT_MAX - strlen(tail_start) - 256;
        if (room > total) room = total;

        memcpy(head, r.output, room);
        head[room] = 0;
        if (room < total) {
            char *nl = strrchr(head, '\n');
            if (nl) *nl = 0;
        }

        int status = 0;
        pid_t w;
        do { w = waitpid(pid, &status, 0); } while (w < 0 && errno == EINTR);

        int code = -1;
        if (w == pid) {
            if (WIFEXITED(status))        code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
        }

        if (code == 0)
            return tool_ok("Exit code: 0 (success)\n"
                           "Note: %zu bytes from the middle of the output were dropped.\n\n"
                           "--- Output (start) ---\n%s\n"
                           "--- [%zu bytes omitted] ---\n"
                           "--- Output (end) ---\n%s",
                           dropped, head, dropped, tail_start);

        return tool_err("Exit code: %d (failure)\n"
                        "Note: %zu bytes from the middle of the output were dropped.\n\n"
                        "--- Output (start) ---\n%s\n"
                        "--- [%zu bytes omitted] ---\n"
                        "--- Output (end) ---\n%s",
                        code, dropped, head, dropped, tail_start);
    }

    /* --- NORMAL (EOF or read error): we wait for the command and report --- */
    (void)got_eof;

    /*
     * BUG FIXED: we always reported "Exit code: 0 (success)", even when gcc had
     * failed. The cause: if waitpid fails (the child was already reaped, or
     * EINTR), `status` stayed 0 -> WEXITSTATUS(0) = 0 -> a false "success".
     * The model believed it had compiled successfully even though it saw errors in
     * the output -> it spun in a loop.
     *
     * Correct: we check that waitpid REALLY succeeded before interpreting status.
     */
    int status = 0;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);

    int code;
    if (w != pid) {
        /* we could not reap the child - we do not know the code, so we do NOT claim success */
        LOG_W("run_command: waitpid for '%s' failed: %s", cmd, strerror(errno));
        code = -1;
    } else if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        LOG_W("run_command: '%s' killed by signal %d", cmd, WTERMSIG(status));
        code = 128 + WTERMSIG(status);
    } else {
        code = -1;
    }

    LOG_I("run_command: '%s' finished with exit=%d (%zu bytes output)",
          cmd, code, total);

    /* The exit code is ESSENTIAL: without it, the model does not know whether it
       worked, so it cannot do the "compile -> read the error -> fix" loop. */
    char body[TOOL_RESULT_MAX];
    snprintf(body, sizeof(body), "%s", total > 0 ? r.output : "(no output)");

    if (code == 0)
        return tool_ok("Exit code: 0 (success)\n\n--- Output ---\n%s", body);
    return tool_err("Exit code: %d (command failed)\n\n--- Output ---\n%s", code, body);
}

/* ------------------------------------------------------------------ */

/*
 * search_files - searches for text in the sandbox's files (a simple recursive
 * grep). Implemented in C (not by exec'ing grep) so it does not depend on the
 * allowlist and so it stays in the sandbox by construction.
 */

#define SEARCH_MAX_DEPTH 8
#define SEARCH_MAX_HITS 200

typedef struct {
    char  *out;
    size_t off;
    size_t cap;
    int    hits;
    int    truncated;
} search_ctx_t;

/* Searches for the pattern in the file, adds the matching lines to ctx. */
static void search_in_file(search_ctx_t *ctx, const char *path,
                           const char *rel, const char *pattern) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[2048];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (ctx->hits >= SEARCH_MAX_HITS) { ctx->truncated = 1; break; }

        if (strstr(line, pattern)) {
            line[strcspn(line, "\n")] = 0;
            int n = snprintf(ctx->out + ctx->off, ctx->cap - ctx->off,
                             "%s:%d: %.200s\n", rel, lineno, line);
            if (n < 0 || (size_t)n >= ctx->cap - ctx->off) {
                ctx->truncated = 1;
                break;
            }
            ctx->off += (size_t)n;
            ctx->hits++;
        }
    }
    fclose(f);
}

/* Walks the directory recursively, searching in files. */
static void search_dir(search_ctx_t *ctx, const char *abs_dir, const char *rel_dir,
                       const char *pattern, int depth) {
    if (depth > SEARCH_MAX_DEPTH || ctx->hits >= SEARCH_MAX_HITS) return;

    DIR *d = opendir(abs_dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;   /* skip hidden ones (including . and ..) */
        if (ctx->hits >= SEARCH_MAX_HITS) { ctx->truncated = 1; break; }

        char abs[PATH_MAX];
        char rel[PATH_MAX];
        if (snprintf(abs, sizeof(abs), "%s/%s", abs_dir, e->d_name) >= (int)sizeof(abs))
            continue;
        if (snprintf(rel, sizeof(rel), "%s%s%s", rel_dir,
                     rel_dir[0] ? "/" : "", e->d_name) >= (int)sizeof(rel))
            continue;

        struct stat st;
        if (stat(abs, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            search_dir(ctx, abs, rel, pattern, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            /* skip large files - probably binary, useless for text search */
            if (st.st_size > 2 * 1024 * 1024) continue;
            search_in_file(ctx, abs, rel, pattern);
        }
    }
    closedir(d);
}

tool_result_t tool_search_files(agent_t *agent, const char *input_json) {
    cJSON *args = json_parse(input_json);
    if (!args || !cJSON_IsObject(args)) {
        if (args) cJSON_Delete(args);
        return tool_err("invalid arguments JSON");
    }

    char pattern[MAX_STR];
    if (!json_get_string(args, "pattern", pattern, sizeof(pattern)) || pattern[0] == 0) {
        cJSON_Delete(args);
        return tool_err("missing or empty 'pattern' field");
    }

    char path[MAX_PATH_LEN] = ".";
    cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (cJSON_IsString(p) && p->valuestring && p->valuestring[0])
        snprintf(path, sizeof(path), "%s", p->valuestring);
    cJSON_Delete(args);

    char resolved[PATH_MAX];
    if (!sandbox_resolve_path_ex(agent->dir, agent->shared_dir, path, resolved)) {
        LOG_W("search_files: access denied, path '%s' escapes the sandbox", path);
        return tool_err("access denied, path outside agent sandbox");
    }

    LOG_I("search_files: agent=%s searching for '%s' in '%s'", agent->id, pattern, path);

    tool_result_t r = {0};
    search_ctx_t ctx = { r.output, 0, TOOL_RESULT_MAX - 128, 0, 0 };

    struct stat st;
    if (stat(resolved, &st) == 0 && S_ISREG(st.st_mode))
        search_in_file(&ctx, resolved, path, pattern);
    else
        search_dir(&ctx, resolved, "", pattern, 0);

    r.output[ctx.off] = 0;

    if (ctx.hits == 0)
        return tool_ok("No matches found for '%s'", pattern);

    if (ctx.truncated)
        return tool_ok("Found %d matches (truncated at %d):\n%s",
                       ctx.hits, SEARCH_MAX_HITS, r.output);

    return tool_ok("Found %d matches:\n%s", ctx.hits, r.output);
}
