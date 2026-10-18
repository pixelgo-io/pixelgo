#include "history.h"
#include "json_util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Builds the path to the agent's history file.
 *
 * CAREFUL (bug fixed): agent->dir is RELATIVE to the startup directory
 * ("workspaces/X/agents/Y"). But when the agent runs as a job from the web server,
 * the child chdir()s into its own sandbox - so the relative path no longer
 * resolves and writing the history failed.
 *
 * Resolution: if the agent's directory exists relative to the current cwd, we use
 * it (the CLI case). If it does NOT exist, it means we are ALREADY inside it (the
 * job case: we chdir'd there), so we use the file name directly.
 * This works in both situations, without changing the signature or the callers.
 */
static void history_path(const agent_t *agent, char *out, size_t out_size) {
    /* agent->dir is ABSOLUTE when running as an agent (we resolved it before the
       chdir), so the path works no matter where the cwd is. Important: the cwd is
       now the SHARED directory, not the agent's - so a relative path would write the
       history into shared/, mixed between agents. */
    struct stat st;
    if (agent->dir[0] && stat(agent->dir, &st) == 0 && S_ISDIR(st.st_mode))
        snprintf(out, out_size, "%s/%s", agent->dir, HISTORY_FILE);
    else
        snprintf(out, out_size, "%s", HISTORY_FILE);
}

cJSON *history_load(const agent_t *agent) {
    char path[MAX_PATH_LEN + 32];
    history_path(agent, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_D("history: '%s' does not exist, starting a new conversation", path);
        return cJSON_CreateArray();
    }

    /* we read the whole file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 4 * 1024 * 1024) {   /* sanity: max 4 MB */
        LOG_W("history: '%s' has an invalid size (%ld), ignoring it", path, size);
        fclose(f);
        return cJSON_CreateArray();
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return cJSON_CreateArray();
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = 0;
    fclose(f);

    cJSON *messages = cJSON_Parse(buf);
    free(buf);

    if (!messages || !cJSON_IsArray(messages)) {
        LOG_W("history: '%s' is not a valid JSON array, ignoring it", path);
        if (messages) cJSON_Delete(messages);
        return cJSON_CreateArray();
    }

    LOG_I("history: loaded %d messages from the previous conversation",
          cJSON_GetArraySize(messages));
    return messages;
}

int history_save(const agent_t *agent, const cJSON *messages) {
    char path[MAX_PATH_LEN + 32];
    history_path(agent, path, sizeof(path));

    char *text = cJSON_PrintUnformatted((cJSON *)messages);
    if (!text) {
        LOG_E("history: serializing the history failed");
        return 0;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        LOG_E("history: cannot write '%s'", path);
        free(text);
        return 0;
    }

    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    int close_err = fclose(f);
    free(text);

    if (written != len || close_err != 0) {
        LOG_E("history: partial write to '%s'", path);
        return 0;
    }

    LOG_D("history: saved %d messages", cJSON_GetArraySize((cJSON *)messages));
    return 1;
}

int history_reset(const agent_t *agent) {
    char path[MAX_PATH_LEN + 32];
    history_path(agent, path, sizeof(path));
    remove(path);   /* we do not care whether it exists or not */
    LOG_I("history: history cleared for agent '%s'", agent->id);
    return 1;
}

/* ================================================================== *
 *  Compaction  (see the long comment in history.h for the reasoning)
 * ================================================================== */

#include "data_request_audit.h"   /* data_threshold_parse: one spelling of
                                     sizes across every limit in the project */
#include <time.h>

/* --- the measured bytes-per-token ratio --- */

static double g_bytes_per_token = 0.0;   /* 0 = nothing measured yet */

void history_calibrate(size_t request_bytes, long input_tokens) {
    /* Refuse the implausible rather than poisoning the estimate: a tiny
       request, a provider that reported nothing, or a ratio outside what any
       tokenizer produces for text (UTF-8 makes 1 byte/token a hard floor;
       above 8 we are being told about a cached prefix, not this request). */
    if (request_bytes < 512 || input_tokens <= 0) return;

    double ratio = (double)request_bytes / (double)input_tokens;
    if (ratio < 1.0 || ratio > 8.0) return;

    g_bytes_per_token = ratio;
    LOG_D("history: bytes per token measured at %.2f (%zu bytes, %ld tokens)",
          ratio, request_bytes, input_tokens);
}

double history_bytes_per_token(void) {
    return g_bytes_per_token > 0.0 ? g_bytes_per_token
                                   : HISTORY_BYTES_PER_TOKEN_DEFAULT;
}

long history_estimate_tokens(const cJSON *messages) {
    if (!messages) return 0;
    char *text = cJSON_PrintUnformatted((cJSON *)messages);
    if (!text) return 0;
    size_t bytes = strlen(text);
    free(text);
    return (long)((double)bytes / history_bytes_per_token());
}

long history_budget_tokens(void) {
    const char *env = getenv("PIXELGO_HISTORY_BUDGET");
    if (env && env[0]) {
        long v = 0;
        if (data_threshold_parse(env, &v) && v > 0) return v;
        LOG_W("history: PIXELGO_HISTORY_BUDGET='%s' is not a size, using the default",
              env);
    }
    return HISTORY_BUDGET_TOKENS_DEFAULT;
}

/* --- the archive: nothing leaves without a copy on disk --- */

static void archive_path(const agent_t *agent, char *out, size_t out_size) {
    struct stat st;
    if (agent->dir[0] && stat(agent->dir, &st) == 0 && S_ISDIR(st.st_mode))
        snprintf(out, out_size, "%s/_history_archive.jsonl", agent->dir);
    else
        snprintf(out, out_size, "_history_archive.jsonl");
}

/* One line per removal. Append-only, and a failure here is logged but never
   stops the run: losing the audit trail is bad, losing the run is worse. */
static void archive_append(const agent_t *agent, const char *reason,
                           const char *tool, const char *content) {
    char path[MAX_PATH_LEN + 32];
    archive_path(agent, path, sizeof(path));

    cJSON *rec = cJSON_CreateObject();
    if (!rec) return;
    cJSON_AddNumberToObject(rec, "ts", (double)time(NULL));
    cJSON_AddStringToObject(rec, "reason", reason);
    if (tool) cJSON_AddStringToObject(rec, "tool", tool);
    if (content) cJSON_AddStringToObject(rec, "content", content);

    char *line = cJSON_PrintUnformatted(rec);
    cJSON_Delete(rec);
    if (!line) return;

    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    } else {
        LOG_W("history: cannot append to '%s'", path);
    }
    free(line);
}

/* --- walking the neutral format --- */

static int msg_is_role(const cJSON *msg, const char *role) {
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(msg, "role");
    return cJSON_IsString(r) && strcmp(r->valuestring, role) == 0;
}

static const char *block_type(const cJSON *block) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(block, "type");
    return cJSON_IsString(t) ? t->valuestring : "";
}

/* FNV-1a over the tool name and its arguments: enough to tell "the same call
   again" from "a different call", without keeping every argument string. */
static unsigned long long call_hash(const char *name, const cJSON *input) {
    unsigned long long h = 1469598103934665603ULL;
    for (const char *p = name; p && *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    char *args = input ? cJSON_PrintUnformatted((cJSON *)input) : NULL;
    for (const char *p = args; p && *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    if (args) free(args);
    return h;
}

#define HC_MAX_CALLS 512

typedef struct {
    char               id[64];
    char               name[64];
    unsigned long long hash;
} hc_call_t;

/* --- step 1: supersession --- */

/*
 * The same tool, called twice with the same arguments: only the newest
 * result is worth sending. Replaces the older ones with a marker.
 *
 * This is the step that pays for itself twice. An agent that reads a file,
 * writes it, and reads it again is carrying two versions of that file in its
 * context - and the older one is not merely wasted tokens, it describes a
 * state of the world that no longer exists.
 */
static int compact_supersede(const agent_t *agent, cJSON *messages) {
    hc_call_t calls[HC_MAX_CALLS];
    int ncalls = 0;
    int changed = 0;

    /* Pass 1: learn id -> (name, hash) from the assistant turns. */
    cJSON *msg = NULL;
    cJSON_ArrayForEach(msg, messages) {
        if (!msg_is_role(msg, "assistant")) continue;
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (!cJSON_IsArray(content)) continue;

        const cJSON *b = NULL;
        cJSON_ArrayForEach(b, content) {
            if (strcmp(block_type(b), "tool_use") != 0) continue;
            if (ncalls >= HC_MAX_CALLS) break;   /* beyond this, no supersession:
                                                    a missed saving, never a wrong
                                                    one */
            const cJSON *id   = cJSON_GetObjectItemCaseSensitive(b, "id");
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(b, "name");
            if (!cJSON_IsString(id) || !cJSON_IsString(name)) continue;

            snprintf(calls[ncalls].id,   sizeof(calls[ncalls].id),   "%s", id->valuestring);
            snprintf(calls[ncalls].name, sizeof(calls[ncalls].name), "%s", name->valuestring);
            calls[ncalls].hash = call_hash(name->valuestring,
                                           cJSON_GetObjectItemCaseSensitive(b, "input"));
            ncalls++;
        }
    }

    /* Pass 2: a result whose call is repeated LATER is superseded. */
    cJSON_ArrayForEach(msg, messages) {
        if (!msg_is_role(msg, "user")) continue;
        cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (!cJSON_IsArray(content)) continue;

        cJSON *b = NULL;
        cJSON_ArrayForEach(b, content) {
            if (strcmp(block_type(b), "tool_result") != 0) continue;

            const cJSON *uid = cJSON_GetObjectItemCaseSensitive(b, "tool_use_id");
            cJSON *body      = cJSON_GetObjectItemCaseSensitive(b, "content");
            if (!cJSON_IsString(uid) || !cJSON_IsString(body)) continue;
            if (strlen(body->valuestring) < 64) continue;   /* not worth a marker */

            /* find this call, then look for the same hash after it */
            int self = -1;
            for (int i = 0; i < ncalls; i++)
                if (strcmp(calls[i].id, uid->valuestring) == 0) { self = i; break; }
            if (self < 0) continue;

            int superseded = 0;
            for (int i = self + 1; i < ncalls; i++)
                if (calls[i].hash == calls[self].hash) { superseded = 1; break; }
            if (!superseded) continue;

            archive_append(agent, "superseded", calls[self].name, body->valuestring);

            char marker[256];
            snprintf(marker, sizeof(marker),
                     "[omitted: %.64s, %zu bytes - the same call was made again "
                     "later, and that result is still here]",
                     calls[self].name, strlen(body->valuestring));
            cJSON_SetValuestring(body, marker);
            changed = 1;
        }
    }
    return changed;
}

/* --- steps 2 and 3: truncation, tighter the older it is --- */

/* Keeps the first head_lines and the last tail_lines, with a marker between.
   For command output the error is at the end and the header at the start;
   the middle is progress. Returns a newly allocated string, or NULL if the
   text was already short enough. */
static char *head_tail(const char *text, size_t cap) {
    size_t len = strlen(text);
    if (len <= cap) return NULL;

    size_t head = cap * 2 / 3;
    size_t tail = cap - head;

    /* step back to a line boundary so we do not cut mid-line */
    while (head > 0 && text[head] != '\n') head--;
    size_t tail_start = len - tail;
    while (tail_start < len && text[tail_start] != '\n') tail_start++;

    char note[96];
    int nlen = snprintf(note, sizeof(note),
                        "\n[... %zu bytes omitted from the middle ...]\n",
                        tail_start - head);

    size_t out_len = head + (size_t)nlen + (len - tail_start);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;

    memcpy(out, text, head);
    memcpy(out + head, note, (size_t)nlen);
    memcpy(out + head + nlen, text + tail_start, len - tail_start);
    out[out_len] = 0;
    return out;
}

/*
 * Caps every tool result, with a smaller cap the further back it is.
 * `recent` is how many of the most recent messages keep the full cap.
 */
static int compact_truncate(const agent_t *agent, cJSON *messages,
                            size_t cap_recent, size_t cap_old, int recent) {
    int total = cJSON_GetArraySize(messages);
    int idx = 0, changed = 0;

    cJSON *msg = NULL;
    cJSON_ArrayForEach(msg, messages) {
        int age_rank = total - idx;   /* 1 = newest */
        idx++;
        if (!msg_is_role(msg, "user")) continue;

        cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (!cJSON_IsArray(content)) continue;

        size_t cap = (age_rank <= recent) ? cap_recent : cap_old;

        cJSON *b = NULL;
        cJSON_ArrayForEach(b, content) {
            if (strcmp(block_type(b), "tool_result") != 0) continue;
            cJSON *body = cJSON_GetObjectItemCaseSensitive(b, "content");
            if (!cJSON_IsString(body)) continue;

            char *cut = head_tail(body->valuestring, cap);
            if (!cut) continue;

            archive_append(agent, "truncated", NULL, body->valuestring);
            cJSON_SetValuestring(body, cut);
            free(cut);
            changed = 1;
        }
    }
    return changed;
}

/* --- step 4: drop whole turns, archiving them first --- */

/* Does this message carry a plain text block from the human? Those are the
   task and any follow-up, they are small, and they are the only thing in the
   history that cannot be reconstructed - so they are never dropped. */
static int is_human_turn(const cJSON *msg) {
    if (!msg_is_role(msg, "user")) return 0;
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (!cJSON_IsArray(c)) return 0;
    const cJSON *b = NULL;
    cJSON_ArrayForEach(b, c)
        if (strcmp(block_type(b), "text") == 0) return 1;
    return 0;
}

static int msg_has_tool_use(const cJSON *msg) {
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (!cJSON_IsArray(c)) return 0;
    const cJSON *b = NULL;
    cJSON_ArrayForEach(b, c)
        if (strcmp(block_type(b), "tool_use") == 0) return 1;
    return 0;
}

static int msg_is_tool_results(const cJSON *msg) {
    if (!msg_is_role(msg, "user")) return 0;
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (!cJSON_IsArray(c)) return 0;
    const cJSON *first = cJSON_GetArrayItem((cJSON *)c, 0);
    return first && strcmp(block_type(first), "tool_result") == 0;
}

/*
 * Drops whole turns, oldest first, until the array is down to `keep`.
 *
 * Two rules, both of which history_trim() cannot express, which is why this
 * does not simply call it:
 *
 *   - a message carrying the human's own text is never dropped. It is the
 *     task; without it the model is left guessing what it was asked, and it
 *     costs almost nothing to keep.
 *   - an assistant turn holding a tool_use leaves together with the
 *     tool_result that answers it. Taking one without the other produces a
 *     history the API rejects outright.
 */
static int compact_drop_turns(const agent_t *agent, cJSON *messages, int keep) {
    int count = cJSON_GetArraySize(messages);
    if (count <= keep) return 0;

    int want = count - keep;
    int removed = 0;

    /* Walk forward marking what goes; remove afterwards, back to front, so
       the indices stay valid while marking. */
    int *drop = calloc((size_t)count, sizeof(int));
    if (!drop) return 0;

    for (int i = 0; i < count && removed < want; i++) {
        cJSON *m = cJSON_GetArrayItem(messages, i);
        if (!m || drop[i]) continue;
        if (is_human_turn(m)) continue;

        drop[i] = 1;
        removed++;

        /* take the answering tool_result with it */
        if (msg_has_tool_use(m) && i + 1 < count) {
            cJSON *next = cJSON_GetArrayItem(messages, i + 1);
            if (next && msg_is_tool_results(next)) {
                drop[i + 1] = 1;
                removed++;
            }
        }
    }

    /* A tool_result left at the very front would be an orphan even if its
       tool_use is still there - it cannot be the first message. */
    for (int i = 0; i < count; i++) {
        if (drop[i]) continue;
        cJSON *m = cJSON_GetArrayItem(messages, i);
        if (m && msg_is_tool_results(m)) { drop[i] = 1; removed++; continue; }
        break;
    }

    int changed = 0;
    for (int i = count - 1; i >= 0; i--) {
        if (!drop[i]) continue;
        cJSON *m = cJSON_GetArrayItem(messages, i);
        char *text = m ? cJSON_PrintUnformatted(m) : NULL;
        if (text) {
            archive_append(agent, "dropped", NULL, text);
            free(text);
        }
        cJSON_DeleteItemFromArray(messages, i);
        changed = 1;
    }

    free(drop);
    if (changed)
        LOG_I("history: dropped %d old turns (the human's messages kept)", removed);
    return changed;
}

/* --- the orchestration --- */

int history_compact(const agent_t *agent, cJSON *messages, int aggressive) {
    if (!agent || !messages) return 0;

    long budget = history_budget_tokens();
    long high   = budget * HISTORY_HIGH_WATER_PCT / 100;
    long low    = budget * HISTORY_LOW_WATER_PCT  / 100;

    if (aggressive) {
        /* A provider has already refused this for length, so the estimate was
           wrong. Aim far lower rather than shaving at the edge. */
        high = 0;
        low  = budget / 4;
    }

    long now = history_estimate_tokens(messages);
    if (now <= high) return 0;

    LOG_I("history: %ld tokens estimated, over the %ld high-water mark - compacting to %ld",
          now, high, low);

    int changed = 0;

    if (compact_supersede(agent, messages)) changed = 1;
    now = history_estimate_tokens(messages);
    if (now <= low) goto done;

    if (compact_truncate(agent, messages, 8192, 2048, 6)) changed = 1;
    now = history_estimate_tokens(messages);
    if (now <= low) goto done;

    if (compact_truncate(agent, messages, 2048, 512, 4)) changed = 1;
    now = history_estimate_tokens(messages);
    if (now <= low) goto done;

    /* Last resort. Halve the turn count and look again; never below four,
       which is one exchange plus its tool round trip. */
    for (int keep = cJSON_GetArraySize(messages) / 2; keep >= 4; keep /= 2) {
        if (compact_drop_turns(agent, messages, keep)) changed = 1;
        now = history_estimate_tokens(messages);
        if (now <= low) break;
    }

done:
    LOG_I("history: compacted to about %ld tokens", now);
    return changed;
}
