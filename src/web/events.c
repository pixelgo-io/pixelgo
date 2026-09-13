#include "events.h"
#include "jobs.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

/* How long the agent waits for your decision before giving up and denying.
   Five minutes: long enough to read what it wants to do, short enough that a
   forgotten browser tab does not leave a process blocked forever. */
#define APPROVAL_TIMEOUT_MS  (5 * 60 * 1000)
#define APPROVAL_POLL_US     (200 * 1000)   /* check 5x per second */

static FILE *g_events = NULL;

/* The current node name - set once by ai_loop, so we do not pass it on every event. */
static char g_node[128] = "";

/* Timestamp in milliseconds since the journal started - the frontend uses it to
   know how long each step took. */
static long long g_start_ms = 0;

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Escapes a string for JSON (quotes, backslash, newline).
   Simple, but sufficient: our fields are node names and short messages. */
static void json_escape(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j < out_size - 2; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (j < out_size - 3) { out[j++] = '\\'; out[j++] = c; }
        } else if (c == '\n') {
            if (j < out_size - 3) { out[j++] = '\\'; out[j++] = 'n'; }
        } else if ((unsigned char)c < 0x20) {
            continue;   /* skip control characters */
        } else {
            out[j++] = c;
        }
    }
    out[j] = 0;
}

int events_open(const char *job_id) {
    if (g_events) return 1;

    /* The ABSOLUTE path: the child chdirs into the agent's sandbox, so a relative
       path "jobs/x.events" would write in the wrong place. jobs_events_path() gives
       us the absolute path (jobs_init resolved it at server startup). */
    char path[1024];
    if (!jobs_events_path(job_id, path, sizeof(path))) return 0;

    g_events = fopen(path, "a");
    if (!g_events) {
        LOG_W("events: cannot open the journal '%s'", path);
        return 0;
    }

    /* Unbuffered: we want the frontend to see the events AS SOON AS they happen,
       not when the buffer fills up. Otherwise the animation would come in bursts. */
    setvbuf(g_events, NULL, _IONBF, 0);

    g_start_ms = now_ms();
    return 1;
}

void events_close(void) {
    if (g_events) {
        fclose(g_events);
        g_events = NULL;
    }
}

/* Writes one event line (JSONL). */
static void emit(const char *fmt, ...) {
    if (!g_events) return;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_events, fmt, ap);
    va_end(ap);

    fputc('\n', g_events);
}

void event_graph_start(const char *entry_node, int total_nodes) {
    char e[128];
    json_escape(entry_node, e, sizeof(e));
    emit("{\"t\":\"graph_start\",\"ms\":%lld,\"entry\":\"%s\",\"total_nodes\":%d}",
         now_ms() - g_start_ms, e, total_nodes);
}

void event_node_start(const char *node, const char *provider, const char *model) {
    char n[128], p[64], m[128];
    json_escape(node, n, sizeof(n));
    json_escape(provider ? provider : "", p, sizeof(p));
    json_escape(model ? model : "", m, sizeof(m));
    emit("{\"t\":\"node_start\",\"ms\":%lld,\"node\":\"%s\",\"provider\":\"%s\",\"model\":\"%s\"}",
         now_ms() - g_start_ms, n, p, m);
}

void event_node_done(const char *node, long input_tokens, long output_tokens,
                     long cost_micro_usd, long duration_ms) {
    char n[128];
    json_escape(node, n, sizeof(n));
    emit("{\"t\":\"node_done\",\"ms\":%lld,\"node\":\"%s\","
         "\"input_tokens\":%ld,\"output_tokens\":%ld,\"cost_micro_usd\":%ld,\"duration_ms\":%ld}",
         now_ms() - g_start_ms, n, input_tokens, output_tokens,
         cost_micro_usd, duration_ms);
}

void event_node_failed(const char *node, const char *error) {
    char n[128], e[256];
    json_escape(node, n, sizeof(n));
    json_escape(error ? error : "", e, sizeof(e));
    emit("{\"t\":\"node_failed\",\"ms\":%lld,\"node\":\"%s\",\"error\":\"%s\"}",
         now_ms() - g_start_ms, n, e);
}

void event_edge_flow(const char *from, const char *to, long bytes,
                     const char *condition) {
    char f[128], t[128], c[128];
    json_escape(from, f, sizeof(f));
    json_escape(to, t, sizeof(t));
    json_escape(condition ? condition : "", c, sizeof(c));
    emit("{\"t\":\"edge_flow\",\"ms\":%lld,\"from\":\"%s\",\"to\":\"%s\","
         "\"bytes\":%ld,\"condition\":\"%s\"}",
         now_ms() - g_start_ms, f, t, bytes, c);
}

void event_graph_done(long total_cost_micro_usd, long total_ms) {
    emit("{\"t\":\"graph_done\",\"ms\":%lld,\"total_cost_micro_usd\":%ld,\"total_ms\":%ld}",
         now_ms() - g_start_ms, total_cost_micro_usd, total_ms);
}

void event_graph_failed(const char *error) {
    char e[256];
    json_escape(error ? error : "", e, sizeof(e));
    emit("{\"t\":\"graph_failed\",\"ms\":%lld,\"error\":\"%s\"}",
         now_ms() - g_start_ms, e);
}

/* --- events from inside the agent --- */

/*
 * ai_loop runs in the AGENT's process (forked from the job, so a grandchild of
 * the server). The journal is not open there. The server puts the path in
 * PIXELGO_EVENTS_FILE before the fork, and the agent reads it from here.
 *
 * If the variable is missing (a CLI run, without the web), we effectively write
 * to /dev/null - the event functions become no-ops. This way we do not have to
 * put checks everywhere.
 */
/*
 * Human-in-the-loop approval for agents running as a web job.
 *
 * The decision file sits next to the journal: jobs/<id>.events becomes
 * jobs/<id>.approval. Deriving it means the agent needs no extra configuration -
 * it already knows the journal path from PIXELGO_EVENTS_FILE.
 */
static int approval_file_path(char *out, size_t out_size) {
    const char *ev = getenv("PIXELGO_EVENTS_FILE");
    if (!ev || !ev[0]) return 0;

    const char *dot = strrchr(ev, '.');
    size_t base_len = dot ? (size_t)(dot - ev) : strlen(ev);
    if (base_len + 10 >= out_size) return 0;

    memcpy(out, ev, base_len);
    out[base_len] = 0;
    strncat(out, ".approval", out_size - base_len - 1);
    return 1;
}

int approval_channel_available(void) {
    char path[1024];
    return approval_file_path(path, sizeof(path));
}

void event_approval_needed(const char *node, const char *tool, const char *input) {
    char n[128], t[64], i[512];
    json_escape(node && node[0] ? node : g_node, n, sizeof(n));
    json_escape(tool ? tool : "", t, sizeof(t));
    json_escape(input ? input : "", i, sizeof(i));
    emit("{\"t\":\"approval_needed\",\"ms\":%lld,\"node\":\"%s\",\"tool\":\"%s\",\"input\":\"%s\"}",
         now_ms() - g_start_ms, n, t, i);
}

void event_approval_done(const char *node, const char *tool, int allowed) {
    char n[128], t[64];
    json_escape(node && node[0] ? node : g_node, n, sizeof(n));
    json_escape(tool ? tool : "", t, sizeof(t));
    emit("{\"t\":\"approval_done\",\"ms\":%lld,\"node\":\"%s\",\"tool\":\"%s\",\"allowed\":%d}",
         now_ms() - g_start_ms, n, t, allowed);
}

/*
 * Blocks until the human decides in the browser, or until the wait times out.
 *
 * Polling a file is crude but correct here: the server and the agent are
 * different processes, so there is no shared memory to signal through, and the
 * journal already established the file-based convention.
 *
 * On timeout we DENY. An agent configured to need approval must never proceed
 * unsupervised just because nobody was watching - e.g. the browser was closed.
 */
int approval_wait(const char *node, const char *tool, const char *input) {
    char path[1024];
    if (!approval_file_path(path, sizeof(path))) return 0;

    /* Clear any stale decision from a previous call before announcing. */
    remove(path);

    event_approval_needed(node, tool, input);

    long waited_ms = 0;
    while (waited_ms < APPROVAL_TIMEOUT_MS) {
        FILE *f = fopen(path, "r");
        if (f) {
            char verdict[16] = {0};
            if (!fgets(verdict, sizeof(verdict), f)) verdict[0] = 0;
            fclose(f);
            remove(path);

            int allowed = (verdict[0] == 'a');   /* "allow" vs "deny" */
            event_approval_done(node, tool, allowed);
            LOG_I("approval: '%s' %s by the human", tool, allowed ? "allowed" : "denied");
            return allowed;
        }
        usleep(APPROVAL_POLL_US);
        waited_ms += APPROVAL_POLL_US / 1000;
    }

    LOG_W("approval: no decision for '%s' within %ds, denying",
          tool, APPROVAL_TIMEOUT_MS / 1000);
    event_approval_done(node, tool, 0);
    return 0;
}

/*
 * Editable approval: same file-based convention as approval_file_path
 * above (jobs/<id>.events -> jobs/<id>.data_edit for the decision), plus a
 * second side file (jobs/<id>.data_edit_src) that carries the text being
 * offered for editing. The text is NOT put through json_escape()/emit()
 * into the journal: it can be arbitrarily large (that is the whole point
 * of this feature), and journal lines are meant to stay small - an
 * oversized event line is exactly the kind of thing that stalled the event
 * route before it was fixed to return bounded batches.
 */
static int data_edit_file_path(char *out, size_t out_size) {
    const char *ev = getenv("PIXELGO_EVENTS_FILE");
    if (!ev || !ev[0]) return 0;

    const char *dot = strrchr(ev, '.');
    size_t base_len = dot ? (size_t)(dot - ev) : strlen(ev);
    if (base_len + 11 >= out_size) return 0;

    memcpy(out, ev, base_len);
    out[base_len] = 0;
    strncat(out, ".data_edit", out_size - base_len - 1);
    return 1;
}

static int data_edit_source_path(char *out, size_t out_size) {
    const char *ev = getenv("PIXELGO_EVENTS_FILE");
    if (!ev || !ev[0]) return 0;

    const char *dot = strrchr(ev, '.');
    size_t base_len = dot ? (size_t)(dot - ev) : strlen(ev);
    if (base_len + 15 >= out_size) return 0;

    memcpy(out, ev, base_len);
    out[base_len] = 0;
    strncat(out, ".data_edit_src", out_size - base_len - 1);
    return 1;
}

int data_edit_channel_available(void) {
    char path[1024];
    return data_edit_file_path(path, sizeof(path));
}

/* A small, separate event: announces that there is a large piece of text to
   review, WITHOUT the text itself (see the comment above). The frontend
   fetches the actual text from GET /api/jobs/<id>/data-edit-source. */
static void event_data_edit_needed(const char *node, const char *tool,
                                   size_t text_len) {
    char n[128], t[64];
    json_escape(node && node[0] ? node : g_node, n, sizeof(n));
    json_escape(tool ? tool : "", t, sizeof(t));
    emit("{\"t\":\"data_edit_needed\",\"ms\":%lld,\"node\":\"%s\",\"tool\":\"%s\",\"text_len\":%zu}",
         now_ms() - g_start_ms, n, t, text_len);
}

static void event_data_edit_done(const char *node, const char *tool, int allowed) {
    char n[128], t[64];
    json_escape(node && node[0] ? node : g_node, n, sizeof(n));
    json_escape(tool ? tool : "", t, sizeof(t));
    emit("{\"t\":\"data_edit_done\",\"ms\":%lld,\"node\":\"%s\",\"tool\":\"%s\",\"allowed\":%d}",
         now_ms() - g_start_ms, n, t, allowed);
}

int data_edit_wait(const char *node, const char *tool,
                   const char *current_text,
                   char *out_text, size_t out_text_cap,
                   int *out_edited) {
    char decision_path[1024], source_path[1024];
    if (!data_edit_file_path(decision_path, sizeof(decision_path))) return 0;
    if (!data_edit_source_path(source_path, sizeof(source_path))) return 0;

    if (out_edited) *out_edited = 0;

    /* Clear any stale files from a previous call before announcing. */
    remove(decision_path);
    remove(source_path);

    size_t text_len = current_text ? strlen(current_text) : 0;

    FILE *src = fopen(source_path, "w");
    if (!src) {
        LOG_W("data_edit: could not write the source file, denying");
        return 0;
    }
    if (current_text) fwrite(current_text, 1, text_len, src);
    fclose(src);

    event_data_edit_needed(node, tool, text_len);

    long waited_ms = 0;
    while (waited_ms < APPROVAL_TIMEOUT_MS) {
        FILE *f = fopen(decision_path, "r");
        if (f) {
            /* The marker is "allow" (5 bytes) or "deny" (4 bytes). Reading
               exactly 5 bytes is deliberate: reading MORE would eat the
               first bytes of whatever follows along with the marker,
               corrupting it silently - which is exactly what an earlier
               version of this function did. */
            char marker[6] = {0};
            size_t got = fread(marker, 1, 5, f);
            marker[got] = 0;

            int allowed = (got >= 1 && marker[0] == 'a');

            if (allowed) {
                /*
                 * After "allow" comes exactly one mode byte:
                 *   'E' -> edited: the human's text follows, byte for byte.
                 *   'U' -> unchanged: nothing follows. The source text can
                 *          be arbitrarily large (served straight off disk,
                 *          no cap), but the human's browser can only POST
                 *          back up to HTTP_MAX_BODY - "unchanged" is how
                 *          they approve something too big to round-trip,
                 *          without the browser ever re-uploading it.
                 * A file that ends right after "allow" (no mode byte at
                 * all - e.g. hand-written during testing) is treated as
                 * unchanged rather than as a parse error: fail toward the
                 * cheaper, no-data-loss interpretation.
                 */
                char mode = 0;
                size_t mode_got = fread(&mode, 1, 1, f);
                int edited = (mode_got == 1 && mode == 'E');
                if (out_edited) *out_edited = edited;

                if (edited && out_text && out_text_cap > 0) {
                    /* The cursor sits right after the mode byte - the
                       edited text starts exactly here. */
                    long after_marker = ftell(f);
                    if (after_marker >= 0) {
                        fseek(f, 0, SEEK_END);
                        long total = ftell(f);
                        fseek(f, after_marker, SEEK_SET);
                        long remaining = total - after_marker;

                        if (remaining < 0) remaining = 0;
                        size_t to_read = (size_t)remaining;
                        if (to_read >= out_text_cap) {
                            to_read = out_text_cap - 1;
                            LOG_W("data_edit: edited text (%ld bytes) exceeds the "
                                  "%zu-byte cap, truncating - not corrupting: the "
                                  "extra bytes are dropped, never silently mixed in",
                                  remaining, out_text_cap);
                        }
                        size_t n = fread(out_text, 1, to_read, f);
                        out_text[n] = 0;
                    } else {
                        out_text[0] = 0;
                    }
                }
            }

            fclose(f);
            remove(decision_path);
            remove(source_path);

            event_data_edit_done(node, tool, allowed);
            LOG_I("data_edit: '%s' %s by the human%s", tool,
                  allowed ? "allowed" : "denied",
                  (allowed && out_edited && *out_edited) ? " (edited)" :
                  (allowed ? " (unchanged)" : ""));
            return allowed;
        }
        usleep(APPROVAL_POLL_US);
        waited_ms += APPROVAL_POLL_US / 1000;
    }

    remove(source_path);
    LOG_W("data_edit: no decision for '%s' within %ds, denying",
          tool, APPROVAL_TIMEOUT_MS / 1000);
    event_data_edit_done(node, tool, 0);
    return 0;
}

int events_open_from_env(void) {
    if (g_events) return 1;

    const char *path = getenv("PIXELGO_EVENTS_FILE");
    if (!path || !path[0]) return 0;   /* without the web -> no-op */

    g_events = fopen(path, "a");
    if (!g_events) return 0;

    setvbuf(g_events, NULL, _IONBF, 0);   /* unbuffered: we want it live */
    g_start_ms = now_ms();
    return 1;
}

/* The current node - set once by ai_loop, so we do not pass it on every event. */

void events_set_node(const char *node) {
    snprintf(g_node, sizeof(g_node), "%s", node ? node : "");
}

void event_agent_thinking(const char *node, int iteration) {
    char n[128];
    json_escape(node && node[0] ? node : g_node, n, sizeof(n));
    emit("{\"t\":\"agent_thinking\",\"ms\":%lld,\"node\":\"%s\",\"iteration\":%d}",
         now_ms() - g_start_ms, n, iteration);
}

void event_tool_call(const char *node, const char *tool, const char *input) {
    char n[128], t[64], i[512];
    json_escape(node && node[0] ? node : g_node, n, sizeof(n));
    json_escape(tool ? tool : "", t, sizeof(t));
    json_escape(input ? input : "", i, sizeof(i));
    emit("{\"t\":\"tool_call\",\"ms\":%lld,\"node\":\"%s\",\"tool\":\"%s\",\"input\":\"%s\"}",
         now_ms() - g_start_ms, n, t, i);
}

void event_tool_result(const char *node, const char *tool, int ok,
                       const char *summary, long bytes) {
    char n[128], t[64], s[512];
    json_escape(node && node[0] ? node : g_node, n, sizeof(n));
    json_escape(tool ? tool : "", t, sizeof(t));
    json_escape(summary ? summary : "", s, sizeof(s));
    emit("{\"t\":\"tool_result\",\"ms\":%lld,\"node\":\"%s\",\"tool\":\"%s\","
         "\"ok\":%s,\"summary\":\"%s\",\"bytes\":%ld}",
         now_ms() - g_start_ms, n, t, ok ? "true" : "false", s, bytes);
}

void event_agent_says(const char *node, const char *text) {
    char n[128], t[1024];
    json_escape(node && node[0] ? node : g_node, n, sizeof(n));
    json_escape(text ? text : "", t, sizeof(t));
    emit("{\"t\":\"agent_says\",\"ms\":%lld,\"node\":\"%s\",\"text\":\"%s\"}",
         now_ms() - g_start_ms, n, t);
}

/*
 * Live output from a running command.
 *
 * The chunk is capped well below the journal's other fields: these arrive many
 * times per command, and a single build line is rarely useful past a couple of
 * hundred characters. json_escape drops control characters, which also strips
 * the ANSI colour codes docker emits - welcome here, since the journal renders
 * as plain text.
 */
void event_tool_output(const char *node, const char *tool, const char *chunk) {
    if (!chunk || !chunk[0]) return;

    char n[128], t[64], c[512];
    json_escape(node && node[0] ? node : g_node, n, sizeof(n));
    json_escape(tool ? tool : "", t, sizeof(t));
    json_escape(chunk, c, sizeof(c));
    if (!c[0]) return;   /* the chunk was only control characters */

    emit("{\"t\":\"tool_output\",\"ms\":%lld,\"node\":\"%s\",\"tool\":\"%s\",\"chunk\":\"%s\"}",
         now_ms() - g_start_ms, n, t, c);
}
