/*
 * Keeping the conversation inside the model's window.
 *
 * The failure this replaces: history_trim() counted MESSAGES and ran once,
 * before the loop. A message is not a unit of anything - a user turn is 50
 * bytes and a tool result is up to TOOL_RESULT_MAX - and the loop then added
 * up to AI_LOOP_MAX_ITERATIONS turns with nothing looking again. A long run
 * died with a 400 and lost everything it had done.
 *
 * The cases below are the ones that decide whether the replacement works:
 * the worst case being reachable at all, the guarantee that a tool_use is
 * never separated from its tool_result, and the rule that nothing is ever
 * removed silently.
 */

#include "test.h"
#include "history.h"
#include "agent.h"
#include "tools.h"
#include "ai_loop.h"
#include "provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static char g_dir[256];

static void workdir(void) {
    snprintf(g_dir, sizeof(g_dir), "/tmp/pixelgo-hc-%d", (int)getpid());
    mkdir(g_dir, 0755);
}

static void workdir_clean(void) {
    char p[512];
    snprintf(p, sizeof(p), "%s/_history_archive.jsonl", g_dir);
    remove(p);
    rmdir(g_dir);
}

static agent_t mk_agent(void) {
    agent_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.id,  sizeof(a.id),  "tester");
    snprintf(a.dir, sizeof(a.dir), "%s", g_dir);
    return a;
}

/* --- building a conversation --- */

static void add_user_text(cJSON *msgs, const char *text) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON *c = cJSON_CreateArray();
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "type", "text");
    cJSON_AddStringToObject(b, "text", text);
    cJSON_AddItemToArray(c, b);
    cJSON_AddItemToObject(m, "content", c);
    cJSON_AddItemToArray(msgs, m);
}

static void add_tool_use(cJSON *msgs, const char *id, const char *name,
                         const char *path_arg) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "assistant");
    cJSON *c = cJSON_CreateArray();
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "type", "tool_use");
    cJSON_AddStringToObject(b, "id", id);
    cJSON_AddStringToObject(b, "name", name);
    cJSON *in = cJSON_CreateObject();
    cJSON_AddStringToObject(in, "path", path_arg);
    cJSON_AddItemToObject(b, "input", in);
    cJSON_AddItemToArray(c, b);
    cJSON_AddItemToObject(m, "content", c);
    cJSON_AddItemToArray(msgs, m);
}

static void add_tool_result(cJSON *msgs, const char *id, size_t bytes) {
    char *body = malloc(bytes + 1);
    for (size_t i = 0; i < bytes; i++) body[i] = (i % 64 == 63) ? '\n' : 'x';
    body[bytes] = 0;

    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON *c = cJSON_CreateArray();
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "type", "tool_result");
    cJSON_AddStringToObject(b, "tool_use_id", id);
    cJSON_AddStringToObject(b, "content", body);
    cJSON_AddItemToArray(c, b);
    cJSON_AddItemToObject(m, "content", c);
    cJSON_AddItemToArray(msgs, m);
    free(body);
}

/* The worst case the limits allow: every iteration calls a tool and gets a
   full-sized result back. */
static cJSON *worst_case_history(int iterations, size_t result_bytes) {
    cJSON *msgs = cJSON_CreateArray();
    add_user_text(msgs, "the task the human asked for");
    for (int i = 0; i < iterations; i++) {
        char id[32], path[64];
        snprintf(id, sizeof(id), "call_%d", i);
        snprintf(path, sizeof(path), "src/file_%d.c", i);
        add_tool_use(msgs, id, "read_file", path);
        add_tool_result(msgs, id, result_bytes);
    }
    return msgs;
}

/* --- checks over the result --- */

/* Every tool_use id must have a tool_result, and the reverse. The API
   rejects a history where that is not true, so this is the one thing
   compaction must never break. */
static int pairs_intact(cJSON *msgs) {
    char uses[2048][32];  int nuses = 0;
    char rslts[2048][32]; int nrslts = 0;

    cJSON *m = NULL;
    cJSON_ArrayForEach(m, msgs) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (!cJSON_IsArray(c)) continue;
        cJSON *b = NULL;
        cJSON_ArrayForEach(b, c) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(b, "type");
            if (!cJSON_IsString(t)) continue;
            if (strcmp(t->valuestring, "tool_use") == 0 && nuses < 2048) {
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(b, "id");
                if (cJSON_IsString(id))
                    snprintf(uses[nuses++], 32, "%s", id->valuestring);
            }
            if (strcmp(t->valuestring, "tool_result") == 0 && nrslts < 2048) {
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(b, "tool_use_id");
                if (cJSON_IsString(id))
                    snprintf(rslts[nrslts++], 32, "%s", id->valuestring);
            }
        }
    }
    if (nuses != nrslts) return 0;
    for (int i = 0; i < nuses; i++) {
        int found = 0;
        for (int j = 0; j < nrslts; j++)
            if (strcmp(uses[i], rslts[j]) == 0) { found = 1; break; }
        if (!found) return 0;
    }
    return 1;
}

static int count_user_text(cJSON *msgs) {
    int n = 0;
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, msgs) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (!cJSON_IsArray(c)) continue;
        cJSON *b = NULL;
        cJSON_ArrayForEach(b, c) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(b, "type");
            if (cJSON_IsString(t) && strcmp(t->valuestring, "text") == 0) n++;
        }
    }
    return n;
}

static int serialized_has(cJSON *msgs, const char *needle) {
    char *s = cJSON_PrintUnformatted(msgs);
    if (!s) return 0;
    int found = strstr(s, needle) != NULL;
    free(s);
    return found;
}

static long archive_lines(void) {
    char p[512];
    snprintf(p, sizeof(p), "%s/_history_archive.jsonl", g_dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    long n = 0; int c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

int main(void) {
    workdir();
    agent_t agent = mk_agent();

    section("the ratio is measured, not guessed");
    ok("before any response, the pessimistic default is used",
       history_bytes_per_token() == HISTORY_BYTES_PER_TOKEN_DEFAULT);
    history_calibrate(300000, 100000);
    ok("after a response, the measured value is used",
       history_bytes_per_token() > 2.9 && history_bytes_per_token() < 3.1);
    history_calibrate(10, 100000);
    ok("a request too small to learn from is ignored",
       history_bytes_per_token() > 2.9);
    history_calibrate(300000, 0);
    ok("a response that reported no tokens is ignored",
       history_bytes_per_token() > 2.9);
    history_calibrate(300000, 10);
    ok("a ratio no tokenizer could produce is ignored",
       history_bytes_per_token() > 2.9);

    section("the budget");
    unsetenv("PIXELGO_HISTORY_BUDGET");
    ok("the default applies when nothing is set",
       history_budget_tokens() == HISTORY_BUDGET_TOKENS_DEFAULT);
    setenv("PIXELGO_HISTORY_BUDGET", "50000", 1);
    ok("a plain number is honoured", history_budget_tokens() == 50000);
    setenv("PIXELGO_HISTORY_BUDGET", "nonsense", 1);
    ok("garbage falls back to the default rather than to zero",
       history_budget_tokens() == HISTORY_BUDGET_TOKENS_DEFAULT);
    unsetenv("PIXELGO_HISTORY_BUDGET");

    section("a small conversation is left completely alone");
    {
        cJSON *msgs = worst_case_history(3, 1000);
        char *before = cJSON_PrintUnformatted(msgs);
        int changed = history_compact(&agent, msgs, 0);
        char *after = cJSON_PrintUnformatted(msgs);
        ok("nothing was changed", changed == 0);
        ok("byte for byte identical", before && after && strcmp(before, after) == 0);
        free(before); free(after);
        cJSON_Delete(msgs);
    }

    section("the worst case the limits allow: 40 iterations x 32 KB");
    {
        setenv("PIXELGO_HISTORY_BUDGET", "20000", 1);
        cJSON *msgs = worst_case_history(AI_LOOP_MAX_ITERATIONS, TOOL_RESULT_MAX);
        long before = history_estimate_tokens(msgs);

        ok("the history really is over budget before compacting",
           before > 20000);
        ok("compaction reports that it acted", history_compact(&agent, msgs, 0) == 1);

        long after = history_estimate_tokens(msgs);
        ok("it ends under the low-water mark",
           after <= 20000 * HISTORY_LOW_WATER_PCT / 100);
        ok("a second call on the compacted history does nothing",
           history_compact(&agent, msgs, 0) == 0);
        ok("no tool_use was left without its tool_result", pairs_intact(msgs));
        ok("the human's message is still there", count_user_text(msgs) >= 1);
        ok("what went is marked, not silently gone", serialized_has(msgs, "omitted"));
        ok("and it was written to the archive", archive_lines() > 0);

        cJSON_Delete(msgs);
        unsetenv("PIXELGO_HISTORY_BUDGET");
    }

    section("supersession: the same call twice keeps only the newest result");
    {
        setenv("PIXELGO_HISTORY_BUDGET", "4000", 1);
        cJSON *msgs = cJSON_CreateArray();
        add_user_text(msgs, "fix the bug in calc.c");
        /* read, write, read again - the first read describes a file that no
           longer exists in that form */
        add_tool_use(msgs, "a1", "read_file", "src/calc.c");
        add_tool_result(msgs, "a1", 9000);
        add_tool_use(msgs, "a2", "write_file", "src/calc.c");
        add_tool_result(msgs, "a2", 100);
        add_tool_use(msgs, "a3", "read_file", "src/calc.c");
        add_tool_result(msgs, "a3", 9000);

        history_compact(&agent, msgs, 0);

        ok("the superseded result says why it went",
           serialized_has(msgs, "the same call was made again later"));
        ok("the pairing survived", pairs_intact(msgs));

        /* the newest result must still be whole: it is the current state */
        char *s = cJSON_PrintUnformatted(msgs);
        const char *last = s ? strstr(s, "\"a3\"") : NULL;
        ok("the newest result was not the one removed",
           last && !strstr(last, "the same call was made again later"));
        free(s);
        cJSON_Delete(msgs);
        unsetenv("PIXELGO_HISTORY_BUDGET");
    }

    section("truncation keeps the head and the tail, where the meaning is");
    {
        setenv("PIXELGO_HISTORY_BUDGET", "3000", 1);
        cJSON *msgs = cJSON_CreateArray();
        add_user_text(msgs, "build it");

        /* a build log: the command at the top, the verdict at the bottom */
        char *big = malloc(60000);
        int n = snprintf(big, 60000, "gcc -Wall -c src/calc.c\n");
        while (n < 59000) n += snprintf(big + n, 60000 - n, "compiling object %d\n", n);
        snprintf(big + n, 60000 - n, "\nerror: the thing that actually failed\n");

        add_tool_use(msgs, "b1", "run_command", "make");
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON *c = cJSON_CreateArray();
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "type", "tool_result");
        cJSON_AddStringToObject(b, "tool_use_id", "b1");
        cJSON_AddStringToObject(b, "content", big);
        cJSON_AddItemToArray(c, b);
        cJSON_AddItemToObject(m, "content", c);
        cJSON_AddItemToArray(msgs, m);
        free(big);

        history_compact(&agent, msgs, 0);

        ok("the command at the start survived",
           serialized_has(msgs, "gcc -Wall -c src/calc.c"));
        ok("the error at the end survived - that is where the verdict lives",
           serialized_has(msgs, "the thing that actually failed"));
        ok("and it says the middle went", serialized_has(msgs, "omitted from the middle"));

        cJSON_Delete(msgs);
        unsetenv("PIXELGO_HISTORY_BUDGET");
    }

    section("a length rejection is recognised, whichever API words it");
    ok("Anthropic",  provider_error_is_too_long("prompt is too long: 1234 tokens > 1000 maximum"));
    ok("OpenAI",     provider_error_is_too_long("This model's maximum context length is 128000 tokens"));
    ok("OpenAI code",provider_error_is_too_long("context_length_exceeded"));
    ok("Gemini",     provider_error_is_too_long("The input token count exceeds the limit"));
    ok("matched case-insensitively",
       provider_error_is_too_long("PROMPT IS TOO LONG"));
    ok("an unrelated error is not mistaken for one",
       !provider_error_is_too_long("invalid api key"));
    ok("a network error is not one either",
       !provider_error_is_too_long("network: connection reset by peer"));
    ok("NULL does not crash", !provider_error_is_too_long(NULL));
    ok("an empty message does not crash", !provider_error_is_too_long(""));

    section("aggressive mode, used after a provider has already refused");
    {
        setenv("PIXELGO_HISTORY_BUDGET", "20000", 1);
        cJSON *msgs = worst_case_history(AI_LOOP_MAX_ITERATIONS, TOOL_RESULT_MAX);
        history_compact(&agent, msgs, 1);
        long after = history_estimate_tokens(msgs);
        ok("it cuts well below the ordinary low-water mark",
           after <= 20000 / 4);
        ok("the pairing still survived", pairs_intact(msgs));
        ok("the human's message is still there", count_user_text(msgs) >= 1);
        cJSON_Delete(msgs);
        unsetenv("PIXELGO_HISTORY_BUDGET");
    }

    section("nothing crashes on degenerate input");
    {
        cJSON *empty = cJSON_CreateArray();
        ok("an empty history", history_compact(&agent, empty, 0) == 0);
        cJSON_Delete(empty);
        ok("a NULL history", history_compact(&agent, NULL, 0) == 0);
        ok("a NULL agent", history_compact(NULL, cJSON_CreateArray(), 0) == 0);
    }

    workdir_clean();
    return t_report("history_compact");
}
