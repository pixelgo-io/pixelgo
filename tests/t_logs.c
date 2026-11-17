#include "test.h"
#include "web_api.h"
#include "http_server.h"
#include "daemon.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/*
 * GET /api/logs.
 *
 * The route exists for `pixelgo-ops logs`, which runs outside the VM and reads
 * the engine's log over the channel. What is tested here is mostly the shapes
 * of log file that break a naive tail: an empty one, one with no trailing
 * newline, one line larger than any budget, and a tail large enough that
 * returning it whole would exceed the route's own contract cap and come back as
 * a 500 - the failure /api/history has today.
 *
 * Called through web_handler, so the route table, the method matching and the
 * response cap are all in the path.
 */

#define LOGS_CAP 131072      /* the route's declared maximum, from the table */

static http_res_t call(const char *path, const char *query) {
    http_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.method, sizeof(req.method), "GET");
    snprintf(req.path, sizeof(req.path), "%s", path);
    if (query) snprintf(req.query, sizeof(req.query), "%s", query);
    char *b = strdup("");
    req.body = b;
    req.body_len = 0;

    http_res_t res;
    memset(&res, 0, sizeof(res));
    web_handler(&req, &res);
    free(b);
    return res;
}

static void res_free(http_res_t *res) {
    if (res->body_is_heap && res->body) free(res->body);
    res->body = NULL;
}

static void write_log(const char *content) {
    FILE *f = fopen(DAEMON_LOG_FILE, "wb");
    if (!f) return;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

/* Writes n lines, each "[INFO ] line <i>: " plus `pad` filler characters. */
static void write_lines(int n, int pad) {
    FILE *f = fopen(DAEMON_LOG_FILE, "wb");
    if (!f) return;
    char *filler = malloc((size_t)pad + 1);
    if (!filler) { fclose(f); return; }
    memset(filler, 'y', (size_t)pad);
    filler[pad] = 0;
    for (int i = 0; i < n; i++) fprintf(f, "[INFO ] line %d: %s\n", i, filler);
    free(filler);
    fclose(f);
}

/* Parses the response, asserting the declared shape, and hands back the object.
   The caller deletes it. Returns NULL if the shape is wrong. */
static cJSON *parsed(const char *what, http_res_t *res) {
    cJSON *j = res->body ? cJSON_Parse(res->body) : NULL;
    if (!j) { ok(what, 0); return NULL; }

    cJSON *lines = cJSON_GetObjectItem(j, "lines");
    cJSON *trunc = cJSON_GetObjectItem(j, "truncated");
    int shape = cJSON_IsArray(lines) && cJSON_IsBool(trunc);
    ok(what, shape);
    if (!shape) { cJSON_Delete(j); return NULL; }
    return j;
}

static int line_count(cJSON *j) {
    return cJSON_GetArraySize(cJSON_GetObjectItem(j, "lines"));
}

static int is_truncated(cJSON *j) {
    return cJSON_IsTrue(cJSON_GetObjectItem(j, "truncated"));
}

static const char *line_at(cJSON *j, int i) {
    cJSON *it = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "lines"), i);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

/* --- the ordinary case ----------------------------------------------- */

static void test_default_tail(void) {
    section("the default is the last 200 lines, oldest first");

    write_lines(500, 20);

    http_res_t r = call("/api/logs", NULL);
    eq_int("status 200", r.status, 200);

    cJSON *j = parsed("the shape is {lines:[], truncated:bool}", &r);
    if (j) {
        eq_int("200 lines", line_count(j), 200);
        ok("truncated, because 300 lines are above them", is_truncated(j));

        /* Oldest first: line 300 of 0..499 is the first of the last 200. */
        const char *first = line_at(j, 0);
        const char *last  = line_at(j, line_count(j) - 1);
        ok("the first entry is the oldest of the tail",
           first && strstr(first, "line 300:") != NULL);
        ok("the last entry is the newest line in the file",
           last && strstr(last, "line 499:") != NULL);
        cJSON_Delete(j);
    }
    res_free(&r);
}

static void test_lines_parameter(void) {
    section("lines is honoured, clamped, and validated");

    write_lines(500, 20);

    http_res_t r = call("/api/logs", "lines=3");
    cJSON *j = parsed("lines=3 answers", &r);
    if (j) {
        eq_int("three lines", line_count(j), 3);
        const char *last = line_at(j, 2);
        ok("and they are the newest three",
           last && strstr(last, "line 499:") != NULL);
        cJSON_Delete(j);
    }
    res_free(&r);

    /* Above the declared maximum of 1000: clamped, not refused. The whole file
       is 500 lines, so the answer is the file and nothing is truncated. */
    r = call("/api/logs", "lines=99999");
    j = parsed("lines=99999 answers", &r);
    if (j) {
        eq_int("clamped to the file, which is shorter than the maximum",
               line_count(j), 500);
        ok("nothing was left out", !is_truncated(j));
        cJSON_Delete(j);
    }
    res_free(&r);

    /*
     * A value we cannot read is refused rather than replaced by the default. A
     * client asking for "last" or "1O0" wants something specific, and quietly
     * giving it 200 lines is the kind of help that hides a bug in the client.
     */
    const char *bad[] = { "lines=abc", "lines=0", "lines=-5", "lines=" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        r = call("/api/logs", bad[i]);
        char what[64];
        snprintf(what, sizeof(what), "%s is a 400", bad[i]);
        eq_int(what, r.status, 400);
        res_free(&r);
    }
}

/* --- the shapes that break a naive tail ------------------------------ */

static void test_no_log_file(void) {
    section("no log file is an empty answer, not an error");

    unlink(DAEMON_LOG_FILE);

    http_res_t r = call("/api/logs", NULL);
    eq_int("status 200 - a server without --daemon writes no log", r.status, 200);

    cJSON *j = parsed("the shape holds", &r);
    if (j) {
        eq_int("no lines", line_count(j), 0);
        ok("and nothing was truncated", !is_truncated(j));
        cJSON_Delete(j);
    }
    res_free(&r);
}

/*
 * An empty file has zero lines, not one empty line. The first version of this
 * route reported one: walking backwards from the end found the single
 * zero-length stretch between the start and the end and counted it. "Nothing
 * has been logged" and "the last thing logged was blank" are different claims.
 */
static void test_empty_log(void) {
    section("an empty log has no lines, not one empty line");

    write_log("");

    http_res_t r = call("/api/logs", NULL);
    cJSON *j = parsed("the shape holds", &r);
    if (j) {
        eq_int("no lines", line_count(j), 0);
        cJSON_Delete(j);
    }
    res_free(&r);
}

static void test_no_trailing_newline(void) {
    section("a file not ending in a newline still has its last line");

    write_log("[INFO ] first\n[WARN ] last line without a newline");

    http_res_t r = call("/api/logs", NULL);
    cJSON *j = parsed("the shape holds", &r);
    if (j) {
        eq_int("both lines", line_count(j), 2);
        const char *last = line_at(j, 1);
        eq_str("and the last one is whole",
               last ? last : "", "[WARN ] last line without a newline");
        cJSON_Delete(j);
    }
    res_free(&r);

    /* The mirror case: a trailing newline must not produce a final empty line. */
    write_log("[INFO ] one\n[INFO ] two\n");
    r = call("/api/logs", NULL);
    j = parsed("the shape holds", &r);
    if (j) {
        eq_int("a trailing newline adds no empty line", line_count(j), 2);
        cJSON_Delete(j);
    }
    res_free(&r);
}

/* --- the cap, which is the point of the byte budget ------------------ */

/*
 * The route's contract cap is enforced centrally by turning an over-size body
 * into a 500. No line count prevents that, so the implementation also has a
 * byte budget. This is the test that the budget is doing its job: ask for the
 * maximum number of lines from a file whose lines are long enough that 1000 of
 * them would be far over the cap.
 */
static void test_byte_budget_keeps_it_under_the_cap(void) {
    section("a large tail comes back truncated, never as a 500");

    write_lines(2000, 400);      /* ~820KB, far over the 128KB cap */

    http_res_t r = call("/api/logs", "lines=1000");
    eq_int("status 200, not 500", r.status, 200);
    ok("the body is under the route's contract cap", r.body_len < LOGS_CAP);

    cJSON *j = parsed("the shape holds", &r);
    if (j) {
        ok("fewer lines than asked for", line_count(j) < 1000);
        ok("some lines came back", line_count(j) > 0);
        ok("and it says so", is_truncated(j));

        /* Truncation drops the oldest, so the newest line must still be there:
           the end of the log is the whole reason for the route. */
        const char *last = line_at(j, line_count(j) - 1);
        ok("the newest line is kept",
           last && strstr(last, "line 1999:") != NULL);
        cJSON_Delete(j);
    }
    res_free(&r);
}

/*
 * One line can be larger than the whole budget - a logged tool result, a dumped
 * response body - and in a crash that line is usually the thing being looked
 * for. Returning an empty array with a flag would be the least useful honest
 * answer available, so its beginning comes back instead.
 */
static void test_one_oversize_line(void) {
    section("a single line larger than the budget comes back cut, not dropped");

    size_t n = 300000;
    char *big = malloc(n + 32);
    if (!big) { ok("allocated the fixture", 0); return; }
    memcpy(big, "[ERROR] ", 8);
    memset(big + 8, 'X', n);
    big[8 + n] = '\n';
    big[9 + n] = 0;
    write_log(big);
    free(big);

    http_res_t r = call("/api/logs", NULL);
    eq_int("status 200", r.status, 200);
    ok("under the cap", r.body_len < LOGS_CAP);

    cJSON *j = parsed("the shape holds", &r);
    if (j) {
        eq_int("one line", line_count(j), 1);
        ok("and it says it was cut", is_truncated(j));

        const char *line = line_at(j, 0);
        ok("something came back", line && strlen(line) > 1000);
        /* From the beginning: the level and the start of the message are the
           informative part, and the payload that made it huge comes after. */
        ok("from the beginning of the line",
           line && strncmp(line, "[ERROR] ", 8) == 0);
        cJSON_Delete(j);
    }
    res_free(&r);
}

/*
 * Cutting an over-size line must not split a UTF-8 sequence. The bytes would
 * still be valid JSON after escaping, but the string would not be valid UTF-8,
 * and a strict parser on the client is entitled to reject the whole response
 * over one truncated character.
 */
static void test_oversize_cut_keeps_utf8_valid(void) {
    section("cutting a line does not split a UTF-8 sequence");

    /* Two-byte characters, so almost any byte-aligned cut lands mid-sequence. */
    size_t reps = 150000;
    char *big = malloc(reps * 2 + 32);
    if (!big) { ok("allocated the fixture", 0); return; }
    for (size_t i = 0; i < reps; i++) {
        big[i * 2]     = (char)0xC4;   /* U+0103, a two-byte character */
        big[i * 2 + 1] = (char)0x83;
    }
    big[reps * 2] = '\n';
    big[reps * 2 + 1] = 0;
    write_log(big);
    free(big);

    http_res_t r = call("/api/logs", NULL);
    cJSON *j = parsed("the response still parses as JSON", &r);
    if (j) {
        const char *line = line_at(j, 0);
        ok("a line came back", line != NULL);

        /* Walk it: every lead byte must be followed by the right number of
           continuation bytes, and the string must not end mid-sequence. */
        int valid = 1;
        for (const unsigned char *p = (const unsigned char *)line; p && *p; ) {
            int need;
            if      (*p < 0x80) need = 0;
            else if ((*p & 0xE0) == 0xC0) need = 1;
            else if ((*p & 0xF0) == 0xE0) need = 2;
            else if ((*p & 0xF8) == 0xF0) need = 3;
            else { valid = 0; break; }
            p++;
            for (int k = 0; k < need; k++) {
                if ((*p & 0xC0) != 0x80) { valid = 0; break; }
                p++;
            }
            if (!valid) break;
        }
        ok("and it is valid UTF-8 throughout", valid);
        cJSON_Delete(j);
    }
    res_free(&r);
}

/* --- the contract ---------------------------------------------------- */

static void test_route_table_and_method(void) {
    section("the route is declared, capped, and GET only");

    size_t n = 0;
    const web_route_t *routes = web_routes(&n);

    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(routes[i].pattern, "/api/logs") == 0) {
            found = 1;
            eq_str("it is GET", routes[i].method, "GET");
            eq_int("with the cap the document declares",
                   (long)routes[i].max_bytes, LOGS_CAP);
        }
    }
    ok("/api/logs is in the table", found);

    http_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.method, sizeof(req.method), "POST");
    snprintf(req.path, sizeof(req.path), "/api/logs");
    char *b = strdup("");
    req.body = b;
    http_res_t res;
    memset(&res, 0, sizeof(res));
    web_handler(&req, &res);
    free(b);
    eq_int("POST is 405", res.status, 405);
    res_free(&res);
}

int main(void) {
    printf("\n=== api/logs ===\n");

    test_default_tail();
    test_lines_parameter();
    test_route_table_and_method();
    test_no_trailing_newline();
    test_empty_log();
    test_byte_budget_keeps_it_under_the_cap();
    test_one_oversize_line();
    test_oversize_cut_keeps_utf8_valid();
    test_no_log_file();      /* removes the file, so it goes last */

    unlink(DAEMON_LOG_FILE);
    return t_report("api/logs");
}
