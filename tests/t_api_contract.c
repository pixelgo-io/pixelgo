#include "test.h"
#include "web_api.h"
#include "http_server.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/*
 * The HTTP API contract (docs/API.md) against the code (the route table in
 * web_api.c). Four things are checked:
 *
 *   1. No drift: the index in docs/API.md and the route table list exactly the
 *      same implemented routes, with the same content type and maximum size,
 *      and the same contract version. A route added on one side only fails
 *      here - that is the point of having a written contract at all.
 *   2. Exact routing: paths that are not routes are 404, even when they share
 *      a prefix with one (the old dispatcher used strstr() and served
 *      /api/jobs/<id>/events-anything as the events route).
 *   3. Error bodies are always valid JSON, whatever the message contains.
 *   4. Conformance: every route, called through the real handler against a
 *      real (temporary) workspace, answers with the declared status, content
 *      type and shape, within its declared maximum - and never a 3xx.
 *
 * Point 4's redirect rule came out of a probe: a proxy built from this
 * contract followed a 302 from a hostile backend and tried to fetch the URL it
 * named. The contract now forbids redirects; this test holds pixelgo to it.
 *
 * Not exercised here: the SUCCESS paths of POST /api/chat and
 * POST /api/flow/run, because they fork a real agent process. Their error
 * paths are covered; their 202 shape is a single snprintf in web_api.c.
 */

#define DOC_PATH "docs/API.md"
#define TMP_BASE "/tmp/pixelgo_test_api_contract"

/* ---------- helpers ---------- */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static const web_route_t *find_route(const char *method, const char *pattern) {
    size_t n;
    const web_route_t *r = web_routes(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(r[i].method, method) == 0 && strcmp(r[i].pattern, pattern) == 0)
            return &r[i];
    return NULL;
}

/* Calls the real handler. The caller frees with res_free(). */
static http_res_t call(const char *method, const char *path, const char *query,
                       const char *body) {
    http_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.method, sizeof(req.method), "%s", method);
    snprintf(req.path, sizeof(req.path), "%s", path);
    snprintf(req.query, sizeof(req.query), "%s", query ? query : "");
    char *b = strdup(body ? body : "");
    req.body = b;
    req.body_len = strlen(b);

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

static int is_json_type(const char *ct) {
    return ct && strncmp(ct, "application/json", 16) == 0;
}

/*
 * Checks one response against its route's contract: the status, the content
 * type (the route's type on success, JSON on error), the maximum size, and
 * that a JSON body parses. Returns the parsed JSON (caller deletes) or NULL.
 */
static cJSON *expect(const char *what, const char *method, const char *pattern,
                     http_res_t *res, int status) {
    const web_route_t *rt = find_route(method, pattern);
    char msg[256];

    snprintf(msg, sizeof(msg), "%s: status %d", what, status);
    eq_int(msg, res->status, status);

    /*
     * No route may redirect. A proxy that follows a 3xx from a hostile guest
     * would fetch whatever URL the guest names - an outbound request from the
     * host, made on the guest's behalf. Checked on every single response, not
     * as a separate case, because it must hold everywhere.
     */
    snprintf(msg, sizeof(msg), "%s: not a redirect", what);
    ok(msg, res->status < 300 || res->status >= 400);

    const char *want_ct = (status < 400 && rt) ? rt->content_type
                                               : "application/json; charset=utf-8";
    snprintf(msg, sizeof(msg), "%s: content type", what);
    eq_str(msg, res->content_type, want_ct);

    if (rt) {
        snprintf(msg, sizeof(msg), "%s: body within the contract maximum", what);
        ok(msg, res->body_len <= rt->max_bytes);
    }

    cJSON *j = NULL;
    if (is_json_type(res->content_type)) {
        j = cJSON_ParseWithLength(res->body ? res->body : "", res->body_len);
        snprintf(msg, sizeof(msg), "%s: body is valid JSON", what);
        ok(msg, j != NULL);
        if (j && status >= 400) {
            cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
            snprintf(msg, sizeof(msg), "%s: error body is {\"error\": string}", what);
            ok(msg, cJSON_IsString(e));
        }
    }
    return j;
}

static int has(const cJSON *o, const char *key, int (*is)(const cJSON *)) {
    return o && is(cJSON_GetObjectItemCaseSensitive(o, key));
}

/* ---------- 1. no drift between docs/API.md and the route table ---------- */

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '`') p++;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '`' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
}

static void test_no_drift(const char *doc) {
    section("docs/API.md and the route table list the same routes");

    ok("docs/API.md was read", doc != NULL);
    if (!doc) return;

    const char *v = strstr(doc, "**Contract version: ");
    eq_int("contract version in the document matches WEB_API_CONTRACT_VERSION",
           v ? atol(v + strlen("**Contract version: ")) : -1, WEB_API_CONTRACT_VERSION);

    const char *begin = strstr(doc, "<!-- routes:begin -->");
    const char *end = strstr(doc, "<!-- routes:end -->");
    ok("the index has its begin/end markers", begin && end && begin < end);
    if (!begin || !end || begin > end) return;

    size_t n;
    const web_route_t *routes = web_routes(&n);
    int *seen = calloc(n, sizeof(int));
    int doc_implemented = 0, doc_planned = 0, all_rows_match = 1, planned_absent = 1;

    const char *line = strchr(begin, '\n');
    while (line && line < end) {
        line++;
        const char *eol = strchr(line, '\n');
        if (!eol || eol > end) break;
        char row[512];
        size_t len = (size_t)(eol - line);
        if (len >= sizeof(row)) len = sizeof(row) - 1;
        memcpy(row, line, len);
        row[len] = 0;
        line = eol;

        if (row[0] != '|') continue;
        char *cols[6] = {0};
        int c = 0;
        char *save = NULL;
        for (char *tok = strtok_r(row + 1, "|", &save); tok && c < 6;
             tok = strtok_r(NULL, "|", &save)) {
            trim(tok);
            cols[c++] = tok;
        }
        if (c < 5) continue;
        if (strcmp(cols[0], "Method") == 0 || strncmp(cols[0], "---", 3) == 0) continue;

        const char *method = cols[0], *path = cols[1], *ct = cols[2], *status = cols[4];
        size_t max = (size_t)strtoul(cols[3], NULL, 10);

        const web_route_t *rt = NULL;
        size_t idx = 0;
        for (size_t i = 0; i < n; i++)
            if (strcmp(routes[i].method, method) == 0 && strcmp(routes[i].pattern, path) == 0) {
                rt = &routes[i]; idx = i;
            }

        if (strcmp(status, "planned") == 0) {
            doc_planned++;
            if (rt) {
                planned_absent = 0;
                printf("      %s %s is marked planned but exists in the code\n", method, path);
            }
            continue;
        }

        doc_implemented++;
        if (!rt) {
            all_rows_match = 0;
            printf("      %s %s is in docs/API.md but not in the route table\n", method, path);
            continue;
        }
        seen[idx] = 1;
        if (strcmp(rt->content_type, ct) != 0 || rt->max_bytes != max) {
            all_rows_match = 0;
            printf("      %s %s differs: doc says %s / %zu, code says %s / %zu\n",
                   method, path, ct, max, rt->content_type, rt->max_bytes);
        }
    }

    int all_code_routes_documented = 1;
    for (size_t i = 0; i < n; i++)
        if (!seen[i]) {
            all_code_routes_documented = 0;
            printf("      %s %s is in the route table but not in docs/API.md\n",
                   routes[i].method, routes[i].pattern);
        }
    free(seen);

    ok("every implemented route in the document exists in code, with the same type and maximum",
       all_rows_match);
    ok("every route in code is documented", all_code_routes_documented);
    eq_int("same number of implemented routes on both sides", doc_implemented, (long)n);
    ok("planned routes are not in code yet (mark them implemented when they are)",
       planned_absent);
    ok("the document defines the planned routes /healthz, /readyz, /api/logs",
       doc_planned == 3);
}

/* ---------- 2. exact routing ---------- */

static void expect_status(const char *what, const char *method, const char *path, int status) {
    http_res_t r = call(method, path, NULL, "{}");
    cJSON *j = expect(what, method, "", &r, status);
    cJSON_Delete(j);
    res_free(&r);
}

static void test_exact_routing(void) {
    section("routing is exact: no prefix or substring matches");

    expect_status("GET /api/jobs/abc/events-anything", "GET", "/api/jobs/abc/events-anything", 404);
    expect_status("GET /api/jobs/abc/eventsX", "GET", "/api/jobs/abc/eventsX", 404);
    expect_status("POST /api/jobs/abc/approveX", "POST", "/api/jobs/abc/approveX", 404);
    expect_status("POST /api/jobs/abc/data-editX", "POST", "/api/jobs/abc/data-editX", 404);
    expect_status("GET /api/jobs/abc/data-edit-source/x", "GET",
                  "/api/jobs/abc/data-edit-source/x", 404);
    expect_status("GET /api/jobs/abc/ (trailing slash)", "GET", "/api/jobs/abc/", 404);
    expect_status("GET /api/jobs/ (no id)", "GET", "/api/jobs/", 404);
    expect_status("GET /api/jobs/a.b (dot in the id)", "GET", "/api/jobs/a.b", 404);
    expect_status("GET /api/jobs/../x", "GET", "/api/jobs/../x", 404);
    expect_status("GET id longer than JOB_ID_LEN-1", "GET",
                  "/api/jobs/0123456789abcdef0123456789abcdef", 404);
    expect_status("GET /index.htmlx", "GET", "/index.htmlx", 404);
    expect_status("GET /api/workspaces/ (trailing slash)", "GET", "/api/workspaces/", 404);
    expect_status("GET /api/nope", "GET", "/api/nope", 404);

    section("a known path with the wrong method is 405, not 404");
    expect_status("GET /api/chat", "GET", "/api/chat", 405);
    expect_status("DELETE /api/workspaces", "DELETE", "/api/workspaces", 405);
    expect_status("POST /api/jobs/abc (GET-only route)", "POST", "/api/jobs/abc", 405);
    expect_status("GET /api/jobs/abc/approve (POST-only route)", "GET", "/api/jobs/abc/approve", 405);
}

/* ---------- 3. error bodies are always valid JSON ---------- */

static void test_error_json(void) {
    section("error bodies are valid JSON whatever the message contains");

    const char *nasty = "line one\nline two\r\ttab \"quoted\" back\\slash \x01\x1f end";
    http_res_t r;
    memset(&r, 0, sizeof(r));
    http_res_error(&r, 400, nasty);
    cJSON *j = cJSON_ParseWithLength(r.body, r.body_len);
    ok("newline, CR, tab, quotes, backslash and control bytes: still valid JSON", j != NULL);
    cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL;
    eq_str("the message survives the round trip unchanged",
           cJSON_IsString(e) ? e->valuestring : NULL, nasty);
    cJSON_Delete(j);
    res_free(&r);

    /* A long message of the worst case (every byte a 6-byte \u00XX escape)
       must be truncated between escapes, never in the middle of one. */
    char long_msg[3000];
    memset(long_msg, 0x01, sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = 0;
    memset(&r, 0, sizeof(r));
    http_res_error(&r, 500, long_msg);
    j = cJSON_ParseWithLength(r.body, r.body_len);
    ok("a 3000-byte message of control bytes: truncated, still valid JSON", j != NULL);
    ok("an error body stays within ~1.2KB", r.body_len <= 1200);
    cJSON_Delete(j);
    res_free(&r);
}

/* ---------- 4. conformance, route by route ---------- */

static void setup_fixtures(void) {
    mkdir("examples", 0755);
    mkdir("demo", 0755);
    write_file("examples/t.flow", "entry a\nnode a\nedge a -> END\n");

    mkdir("jobs", 0755);
    write_file("jobs/abc123.json",
               "{\"id\":\"abc123\",\"status\":\"done\",\"pid\":0,\"kind\":\"chat\","
               "\"result\":\"hello\",\"error\":\"\"}");
    write_file("jobs/abc123.events", "{\"type\":\"a\"}\n{\"type\":\"b\"}\n");
    write_file("jobs/abc123.data_edit_src", "<b>not html</b>\nsecond line\n");

    /* One byte over the data-edit-source maximum. Sparse: costs no disk. */
    FILE *f = fopen("jobs/big1.data_edit_src", "wb");
    if (f) {
        fseek(f, (long)HTTP_MAX_BODY_CEILING, SEEK_SET);
        fputc('x', f);
        fclose(f);
    }
}

static void test_conformance(void) {
    http_res_t r;
    cJSON *j;

    section("GET / and /index.html");
    r = call("GET", "/", NULL, NULL);
    cJSON_Delete(expect("GET /", "GET", "/", &r, 200));
    ok("GET /: the page is not empty", r.body_len > 0);
    res_free(&r);
    r = call("GET", "/index.html", NULL, NULL);
    cJSON_Delete(expect("GET /index.html", "GET", "/index.html", &r, 200));
    res_free(&r);

    section("workspaces");
    r = call("POST", "/api/workspaces", NULL, "{\"name\":\"w\"}");
    j = expect("POST /api/workspaces", "POST", "/api/workspaces", &r, 201);
    ok("POST /api/workspaces: {\"ok\":true}", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok")));
    cJSON_Delete(j); res_free(&r);

    r = call("POST", "/api/workspaces", NULL, "{}");
    cJSON_Delete(expect("POST /api/workspaces without name", "POST", "/api/workspaces", &r, 400));
    res_free(&r);

    r = call("GET", "/api/workspaces", NULL, NULL);
    j = expect("GET /api/workspaces", "GET", "/api/workspaces", &r, 200);
    ok("GET /api/workspaces: array of {name, agent_count}",
       cJSON_IsArray(j) && cJSON_GetArraySize(j) == 1 &&
       has(cJSON_GetArrayItem(j, 0), "name", cJSON_IsString) &&
       has(cJSON_GetArrayItem(j, 0), "agent_count", cJSON_IsNumber));
    cJSON_Delete(j); res_free(&r);

    section("agents");
    r = call("POST", "/api/agents", NULL,
             "{\"ws\":\"w\",\"id\":\"a\",\"type\":\"ai\",\"provider\":\"anthropic\",\"model\":\"m\"}");
    cJSON_Delete(expect("POST /api/agents (ai)", "POST", "/api/agents", &r, 201));
    res_free(&r);
    r = call("POST", "/api/agents", NULL,
             "{\"ws\":\"w\",\"id\":\"wk\",\"type\":\"worker\",\"command\":\"echo\"}");
    cJSON_Delete(expect("POST /api/agents (worker)", "POST", "/api/agents", &r, 201));
    res_free(&r);
    r = call("POST", "/api/agents", NULL, "not json");
    cJSON_Delete(expect("POST /api/agents with an invalid body", "POST", "/api/agents", &r, 400));
    res_free(&r);

    r = call("GET", "/api/agents", "ws=w", NULL);
    j = expect("GET /api/agents", "GET", "/api/agents", &r, 200);
    int shapes_ok = cJSON_IsArray(j) && cJSON_GetArraySize(j) == 2;
    cJSON *ag;
    cJSON_ArrayForEach(ag, j) {
        if (!has(ag, "id", cJSON_IsString) || !has(ag, "type", cJSON_IsString)) shapes_ok = 0;
    }
    ok("GET /api/agents: two agents, each with id and type", shapes_ok);
    cJSON_Delete(j); res_free(&r);
    r = call("GET", "/api/agents", "", NULL);
    cJSON_Delete(expect("GET /api/agents without ws", "GET", "/api/agents", &r, 400));
    res_free(&r);
    r = call("GET", "/api/agents", "ws=nope", NULL);
    cJSON_Delete(expect("GET /api/agents for a missing workspace", "GET", "/api/agents", &r, 404));
    res_free(&r);

    section("chat (error paths only - success forks a real agent)");
    r = call("POST", "/api/chat", NULL, "{\"ws\":\"w\",\"agent\":\"wk\",\"message\":\"hi\"}");
    cJSON_Delete(expect("POST /api/chat to a worker agent", "POST", "/api/chat", &r, 400));
    res_free(&r);
    r = call("POST", "/api/chat", NULL, "{\"ws\":\"w\",\"agent\":\"nope\",\"message\":\"hi\"}");
    cJSON_Delete(expect("POST /api/chat to a missing agent", "POST", "/api/chat", &r, 404));
    res_free(&r);

    section("history");
    r = call("GET", "/api/history", "ws=w&agent=a", NULL);
    j = expect("GET /api/history", "GET", "/api/history", &r, 200);
    ok("GET /api/history: an array", cJSON_IsArray(j));
    cJSON_Delete(j); res_free(&r);
    r = call("GET", "/api/history", "ws=w", NULL);
    cJSON_Delete(expect("GET /api/history without agent", "GET", "/api/history", &r, 400));
    res_free(&r);
    r = call("POST", "/api/history/reset", NULL, "{\"ws\":\"w\",\"agent\":\"a\"}");
    cJSON_Delete(expect("POST /api/history/reset", "POST", "/api/history/reset", &r, 200));
    res_free(&r);
    r = call("POST", "/api/history/reset", NULL, "{\"ws\":\"w\",\"agent\":\"nope\"}");
    cJSON_Delete(expect("POST /api/history/reset for a missing agent", "POST",
                        "/api/history/reset", &r, 404));
    res_free(&r);

    section("limits");
    r = call("GET", "/api/limits", NULL, NULL);
    j = expect("GET /api/limits", "GET", "/api/limits", &r, 200);
    ok("GET /api/limits: {http_max_body: number}", has(j, "http_max_body", cJSON_IsNumber));
    cJSON_Delete(j); res_free(&r);

    section("flows");
    r = call("GET", "/api/flows", NULL, NULL);
    j = expect("GET /api/flows", "GET", "/api/flows", &r, 200);
    ok("GET /api/flows: array of strings, including examples/t.flow",
       cJSON_IsArray(j) && cJSON_GetArraySize(j) >= 1 && cJSON_IsString(cJSON_GetArrayItem(j, 0)));
    cJSON_Delete(j); res_free(&r);

    r = call("POST", "/api/flows", NULL, "{\"name\":\"saved\",\"content\":\"entry a\\nnode a\\nedge a -> END\\n\"}");
    j = expect("POST /api/flows", "POST", "/api/flows", &r, 200);
    ok("POST /api/flows: {saved: string}", has(j, "saved", cJSON_IsString));
    cJSON_Delete(j); res_free(&r);
    r = call("POST", "/api/flows", NULL, "{\"name\":\"x\"}");
    cJSON_Delete(expect("POST /api/flows without content", "POST", "/api/flows", &r, 400));
    res_free(&r);

    r = call("POST", "/api/flow/diagram", NULL, "{\"flow\":\"examples/t.flow\"}");
    j = expect("POST /api/flow/diagram", "POST", "/api/flow/diagram", &r, 200);
    ok("POST /api/flow/diagram: {mermaid: string}", has(j, "mermaid", cJSON_IsString));
    cJSON_Delete(j); res_free(&r);
    r = call("POST", "/api/flow/diagram", NULL, "{\"flow\":\"../etc/passwd\"}");
    cJSON_Delete(expect("POST /api/flow/diagram outside examples/", "POST",
                        "/api/flow/diagram", &r, 400));
    res_free(&r);

    r = call("POST", "/api/flow/graph", NULL, "{\"flow\":\"examples/t.flow\"}");
    j = expect("POST /api/flow/graph", "POST", "/api/flow/graph", &r, 200);
    ok("POST /api/flow/graph: {entry, nodes[], edges[]}",
       has(j, "entry", cJSON_IsString) && has(j, "nodes", cJSON_IsArray) &&
       has(j, "edges", cJSON_IsArray));
    cJSON_Delete(j); res_free(&r);
    r = call("POST", "/api/flow/graph", NULL, "{}");
    cJSON_Delete(expect("POST /api/flow/graph without flow", "POST", "/api/flow/graph", &r, 400));
    res_free(&r);

    section("flow run (error paths only - success forks a real agent)");
    r = call("POST", "/api/flow/run", NULL, "{\"ws\":\"nope\",\"flow\":\"examples/t.flow\"}");
    cJSON_Delete(expect("POST /api/flow/run in a missing workspace", "POST",
                        "/api/flow/run", &r, 404));
    res_free(&r);
    r = call("POST", "/api/flow/run", NULL, "{\"ws\":\"w\"}");
    cJSON_Delete(expect("POST /api/flow/run without flow", "POST", "/api/flow/run", &r, 400));
    res_free(&r);

    section("jobs");
    r = call("GET", "/api/jobs/abc123", NULL, NULL);
    j = expect("GET /api/jobs/{id}", "GET", "/api/jobs/{id}", &r, 200);
    ok("GET /api/jobs/{id}: {id, kind, status, reply} for a done chat job",
       has(j, "id", cJSON_IsString) && has(j, "kind", cJSON_IsString) &&
       has(j, "status", cJSON_IsString) && has(j, "reply", cJSON_IsString));
    cJSON_Delete(j); res_free(&r);
    r = call("GET", "/api/jobs/nope1", NULL, NULL);
    cJSON_Delete(expect("GET /api/jobs/{id} for a missing job", "GET", "/api/jobs/{id}", &r, 404));
    res_free(&r);

    r = call("GET", "/api/jobs/abc123/events", "from=0", NULL);
    j = expect("GET /api/jobs/{id}/events", "GET", "/api/jobs/{id}/events", &r, 200);
    cJSON *evs = cJSON_GetObjectItemCaseSensitive(j, "events");
    cJSON *next = cJSON_GetObjectItemCaseSensitive(j, "next");
    ok("GET /api/jobs/{id}/events: {events: [2 items], next: 2}",
       cJSON_IsArray(evs) && cJSON_GetArraySize(evs) == 2 &&
       cJSON_IsNumber(next) && next->valueint == 2);
    cJSON_Delete(j); res_free(&r);

    section("data-edit-source: text on success, JSON on error, bounded");
    r = call("GET", "/api/jobs/abc123/data-edit-source", NULL, NULL);
    cJSON_Delete(expect("GET data-edit-source", "GET", "/api/jobs/{id}/data-edit-source", &r, 200));
    ok("the text comes back byte for byte",
       r.body && r.body_len == strlen("<b>not html</b>\nsecond line\n") &&
       memcmp(r.body, "<b>not html</b>\nsecond line\n", r.body_len) == 0);
    res_free(&r);
    r = call("GET", "/api/jobs/nope1/data-edit-source", NULL, NULL);
    cJSON_Delete(expect("GET data-edit-source with nothing pending", "GET",
                        "/api/jobs/{id}/data-edit-source", &r, 404));
    res_free(&r);
    r = call("GET", "/api/jobs/big1/data-edit-source", NULL, NULL);
    j = expect("GET data-edit-source one byte over the maximum", "GET",
               "/api/jobs/{id}/data-edit-source", &r, 500);
    cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
    ok("the error says the request is too large",
       cJSON_IsString(e) && strstr(e->valuestring, "too large") != NULL);
    cJSON_Delete(j); res_free(&r);

    section("approvals");
    r = call("POST", "/api/jobs/abc123/approve", NULL, "{\"allow\":true}");
    j = expect("POST /api/jobs/{id}/approve", "POST", "/api/jobs/{id}/approve", &r, 200);
    ok("approve: {allowed: true}", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "allowed")));
    cJSON_Delete(j); res_free(&r);
    r = call("POST", "/api/jobs/abc123/approve", NULL, "not json");
    cJSON_Delete(expect("approve with an invalid body", "POST", "/api/jobs/{id}/approve", &r, 400));
    res_free(&r);

    r = call("POST", "/api/jobs/abc123/data-edit", NULL, "{\"allow\":false}");
    j = expect("POST /api/jobs/{id}/data-edit", "POST", "/api/jobs/{id}/data-edit", &r, 200);
    ok("data-edit: {allowed: false}", cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "allowed")));
    cJSON_Delete(j); res_free(&r);
    r = call("POST", "/api/jobs/abc123/data-edit", NULL, "{\"allow\":true,\"text\":5}");
    cJSON_Delete(expect("data-edit with a non-string text", "POST", "/api/jobs/{id}/data-edit", &r, 400));
    res_free(&r);
}

int main(void) {
    /* Read the contract from the repository root (where `make test` runs)
       before moving into the scratch directory the routes will write to. */
    char *doc = read_file(DOC_PATH);

    test_no_drift(doc);
    free(doc);

    test_error_json();

    char dir[] = TMP_BASE "_XXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) {
        printf("  cannot create a scratch directory\n");
        return 1;
    }
    setup_fixtures();

    test_exact_routing();
    test_conformance();

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (chdir("/") == 0 && system(cmd) != 0) { /* best effort */ }

    return t_report("api_contract");
}
