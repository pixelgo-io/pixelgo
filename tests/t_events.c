#include "test.h"
#include "jobs.h"
#include "http_server.h"
#include "json_util.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/*
 * Can the client always walk the whole event journal?
 *
 * The route serializes its response into a fixed buffer, and json_serialize
 * refuses to truncate, because truncated JSON is invalid JSON. So an oversized
 * response used to fall through to a fallback that returned an empty list with
 * `next` UNCHANGED. The client asked for the same range again, got empty again,
 * and the journal stopped advancing - silently, with no error anywhere.
 *
 * The visible consequence was worse than a missing log line: the approval
 * dialog is delivered through this same stream. An agent would block waiting
 * for a decision that the human was never shown, then time out and be denied.
 *
 * Streaming command output made this the normal case rather than a rare one -
 * a single `docker compose build` writes several hundred KB of events.
 *
 * The property that matters is not "batches are 400 long" but "polling from 0
 * eventually reaches the end". That is what this test asserts, by draining the
 * route exactly as the frontend does.
 */

extern void web_handler(const http_req_t *req, http_res_t *res);

/* Writes a journal with `count` events, each carrying a chunk of `chunk_len`
   characters, mimicking streamed docker output. */
static int write_journal(const char *path, int count, int chunk_len) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;

    char *chunk = malloc((size_t)chunk_len + 1);
    if (!chunk) { fclose(f); return 0; }
    memset(chunk, 'x', (size_t)chunk_len);
    chunk[chunk_len] = 0;

    for (int i = 0; i < count; i++) {
        fprintf(f, "{\"t\":\"tool_output\",\"ms\":%d,\"node\":\"devops\","
                   "\"tool\":\"run_command\",\"chunk\":\"%s\"}\n", i * 50, chunk);
    }
    free(chunk);
    fclose(f);
    return 1;
}

/* One poll, as the frontend makes it. Returns the number of events received,
   or -1 on a malformed response; *next_out receives the new cursor. */
static int poll_once(const char *job_id, int from, int *next_out) {
    http_req_t req;
    http_res_t res;
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));

    snprintf(req.method, sizeof(req.method), "GET");
    snprintf(req.path, HTTP_MAX_PATH, "/api/jobs/%s/events", job_id);
    snprintf(req.query, HTTP_MAX_PATH, "from=%d", from);

    web_handler(&req, &res);

    if (!res.body) return -1;

    cJSON *root = cJSON_Parse(res.body);
    if (res.body_is_heap) free(res.body);
    if (!root) return -1;

    cJSON *evs  = cJSON_GetObjectItemCaseSensitive(root, "events");
    cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "next");

    int n = cJSON_IsArray(evs) ? cJSON_GetArraySize(evs) : -1;
    *next_out = cJSON_IsNumber(next) ? (int)next->valuedouble : from;

    cJSON_Delete(root);
    return n;
}

/*
 * Drains the journal the way the frontend does, and reports how far it got.
 * `polls_out` receives the number of requests it took, so a fix that technically
 * works but needs thousands of round trips is still visible.
 */
static int drain(const char *job_id, int expected, int *polls_out) {
    int from = 0, polls = 0, total = 0;

    /* Generous ceiling: we are testing that it terminates, not how fast. */
    while (polls < 5000) {
        int prev = from;
        int n = poll_once(job_id, from, &from);
        polls++;

        if (n < 0) break;             /* malformed response */
        total += n;

        if (from <= prev) break;      /* cursor did not advance: stalled */
        if (total >= expected) break; /* got everything */
    }

    *polls_out = polls;
    return total;
}

int main(void) {
    /* jobs_init() resolves "jobs/" relative to the current directory, so we
       work inside a throwaway one rather than touching the real journal. */
    char dir[] = "/tmp/pixelgo_ev_XXXXXX";
    if (!mkdtemp(dir)) {
        printf("\n  could not create a temp dir\n\n");
        return 1;
    }
    if (chdir(dir) != 0) {
        printf("\n  could not enter the temp dir\n\n");
        return 1;
    }
    if (!jobs_init()) {
        printf("\n  jobs_init failed\n\n");
        return 1;
    }

    char path[1024];
    char msg[200];

    /* ------------------------------------------------------------------ */
    section("a small journal is delivered in one poll");

    snprintf(path, sizeof(path), "jobs/small.events");
    write_journal(path, 20, 40);

    int polls = 0;
    int got = drain("small", 20, &polls);
    snprintf(msg, sizeof(msg), "received %d/20 events", got);
    ok(msg, got == 20);

    /* ------------------------------------------------------------------ */
    section("a journal far larger than the response buffer still drains fully");

    /*
     * 4000 events at ~380 bytes is roughly 1.5 MB - about what a Docker build
     * produces once its output is streamed, and more than ten times the
     * response buffer. Before the fix this stalled on the first poll and never
     * moved again.
     */
    snprintf(path, sizeof(path), "jobs/big.events");
    write_journal(path, 4000, 300);

    polls = 0;
    got = drain("big", 4000, &polls);

    snprintf(msg, sizeof(msg), "received %d/4000 events (in %d polls)", got, polls);
    ok(msg, got == 4000);

    ok("it did not stall part-way through", got == 4000);

    snprintf(msg, sizeof(msg), "took %d polls, a sane number for 4000 events", polls);
    ok(msg, polls > 0 && polls < 100);

    /* ------------------------------------------------------------------ */
    section("individually oversized events do not stall the stream either");

    /* A single event close to the journal's line limit. Even if one of these
       cannot be delivered, the cursor must keep moving - losing an event is
       recoverable, freezing the UI is not. */
    snprintf(path, sizeof(path), "jobs/huge.events");
    write_journal(path, 60, 1900);

    polls = 0;
    int from = 0, moved = 0;
    for (int i = 0; i < 200; i++) {
        int prev = from;
        if (poll_once("huge", from, &from) < 0) break;
        polls++;
        if (from > prev) moved++;
        if (from >= 60) break;
    }
    snprintf(msg, sizeof(msg), "the cursor advanced on %d of %d polls", moved, polls);
    ok(msg, moved > 0);
    ok("the cursor reached the end of the journal", from >= 60);

    /* ------------------------------------------------------------------ */
    section("an empty or missing journal is not an error");

    int next = -1;
    ok("a missing journal returns an empty batch",
       poll_once("nonexistent", 0, &next) == 0 && next == 0);

    /* cleanup */
    if (chdir("/tmp") == 0) {
        snprintf(msg, sizeof(msg), "rm -rf %s", dir);
        if (system(msg) != 0) { /* best effort */ }
    }

    return t_report("event delivery");
}
