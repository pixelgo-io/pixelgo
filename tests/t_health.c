#include "test.h"
#include "web_api.h"
#include "http_server.h"
#include "shutdown.h"
#include "jobs.h"
#include "version.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

/*
 * GET /healthz and GET /readyz.
 *
 * These two exist for pixelgo-ops, which runs outside the VM and has to decide
 * three things before it does anything: is the engine there, does its API
 * contract match the one ops was built against, and will it accept work. Each
 * of those has a wrong answer that is worse than no answer, so what is tested
 * here is mostly the wrong answers.
 *
 * The routes are called through web_handler, the same dispatch a real request
 * goes through, rather than by calling the handlers directly - that way the
 * route table, the method matching and the response caps are all in the path.
 */

static http_res_t call(const char *method, const char *path) {
    http_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.method, sizeof(req.method), "%s", method);
    snprintf(req.path, sizeof(req.path), "%s", path);
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

/* Parses the body as JSON, failing the named check if it is not. */
static cJSON *body_json(const char *what, http_res_t *res) {
    cJSON *j = res->body ? cJSON_Parse(res->body) : NULL;
    ok(what, j != NULL);
    return j;
}

/* --- healthz --------------------------------------------------------- */

static void test_healthz(void) {
    section("healthz answers with the version and the contract");

    http_res_t r = call("GET", "/healthz");
    eq_int("status 200", r.status, 200);

    cJSON *j = body_json("the body is JSON", &r);
    if (j) {
        cJSON *status   = cJSON_GetObjectItem(j, "status");
        cJSON *version  = cJSON_GetObjectItem(j, "version");
        cJSON *contract = cJSON_GetObjectItem(j, "contract");

        ok("status is a string", cJSON_IsString(status));
        if (cJSON_IsString(status))
            eq_str("status is \"ok\"", status->valuestring, "ok");

        ok("version is a string", cJSON_IsString(version));
        if (cJSON_IsString(version))
            eq_str("version is the one the binary was built with",
                   version->valuestring, PIXELGO_VERSION);

        /*
         * The contract number is the whole point of the route for ops: it
         * serves its own copy of the UI and refuses to run against an engine
         * whose contract it does not speak. A number that does not match the
         * header would let a mismatched pair start and fail later, on a field
         * that moved.
         */
        ok("contract is a number", cJSON_IsNumber(contract));
        if (cJSON_IsNumber(contract))
            eq_int("contract matches WEB_API_CONTRACT_VERSION",
                   (long)contract->valuedouble, WEB_API_CONTRACT_VERSION);

        cJSON_Delete(j);
    }
    res_free(&r);
}

/*
 * Health must not depend on anything that can be broken, which is why the route
 * reads no state. The check for that is behavioural: break what readiness
 * depends on, and health must still answer.
 */
static void test_healthz_survives_a_broken_jobs_dir(void) {
    section("healthz answers even when the server cannot work");

    if (rename("jobs", "jobs-moved-by-test") != 0) {
        ok("jobs/ moved aside for the test", 0);
        return;
    }

    http_res_t r = call("GET", "/healthz");
    eq_int("still 200", r.status, 200);

    cJSON *j = body_json("still JSON", &r);
    if (j) {
        cJSON *status = cJSON_GetObjectItem(j, "status");
        ok("still \"ok\" - this route is about the process, not the work",
           cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0);
        cJSON_Delete(j);
    }
    res_free(&r);

    rename("jobs-moved-by-test", "jobs");
}

static void test_healthz_rejects_other_methods(void) {
    section("healthz is GET only");

    http_res_t r = call("POST", "/healthz");
    eq_int("POST is 405, not 200 and not 404", r.status, 405);
    res_free(&r);
}

/* --- readyz ---------------------------------------------------------- */

static void test_readyz_ready(void) {
    section("readyz reports ready on a working server");

    http_res_t r = call("GET", "/readyz");
    eq_int("status 200", r.status, 200);

    cJSON *j = body_json("the body is JSON", &r);
    if (j) {
        cJSON *ready  = cJSON_GetObjectItem(j, "ready");
        cJSON *reason = cJSON_GetObjectItem(j, "reason");

        ok("ready is a boolean", cJSON_IsBool(ready));
        ok("and it is true", cJSON_IsTrue(ready));
        ok("reason is a string", cJSON_IsString(reason));
        if (cJSON_IsString(reason))
            eq_str("and it is empty when ready", reason->valuestring, "");
        cJSON_Delete(j);
    }
    res_free(&r);
}

/*
 * The branch that earns the route its place.
 *
 * A jobs directory that cannot be written to is a server that is healthy,
 * reachable, and unable to do the only thing it exists for: every chat and
 * every flow run begins by writing a file there. Without this, ops would submit
 * work and watch it fail, with nothing to have warned it.
 */
static void test_readyz_not_ready_without_a_writable_jobs_dir(void) {
    section("readyz is not ready when jobs cannot be recorded");

    /*
     * The directory is moved away rather than chmod'ed, for two reasons. A mode
     * does not stop uid 0, so a chmod-based test passes for the wrong reason
     * wherever the suite runs as root - which is most CI. And this is the more
     * realistic failure anyway: jobs.c resolves the directory once at startup
     * and caches the absolute path, so a directory that disappears under a
     * running server is exactly what the cached path then points at.
     */
    if (rename("jobs", "jobs-moved-by-test") != 0) {
        ok("jobs/ moved aside for the test", 0);
        return;
    }

    http_res_t r = call("GET", "/readyz");
    eq_int("still 200 - it answers, it does not error", r.status, 200);

    cJSON *j = body_json("the body is JSON", &r);
    if (j) {
        cJSON *ready  = cJSON_GetObjectItem(j, "ready");
        cJSON *reason = cJSON_GetObjectItem(j, "reason");

        ok("ready is false", cJSON_IsFalse(ready));
        ok("reason says what is wrong",
           cJSON_IsString(reason) && strstr(reason->valuestring, "jobs") != NULL);
        cJSON_Delete(j);
    }
    res_free(&r);

    rename("jobs-moved-by-test", "jobs");

    /* And it recovers: the state is read per request, not cached. */
    r = call("GET", "/readyz");
    j = body_json("JSON again", &r);
    if (j) {
        ok("ready again once the directory is writable",
           cJSON_IsTrue(cJSON_GetObjectItem(j, "ready")));
        cJSON_Delete(j);
    }
    res_free(&r);
}

/*
 * Draining, in a child process.
 *
 * In a child because the shutdown flag is one-way by design - see shutdown.h -
 * so setting it here would make every later test in this binary see a draining
 * server. The child reports through its exit code: 0 means it saw ready=false
 * with a reason mentioning the shutdown.
 *
 * Note what this does NOT claim. A real drain closes the listening sockets
 * first, so a connection opened afterwards is refused by the kernel and never
 * reaches this route; for ops, a refused connection is the real signal. This
 * branch is the narrower case where the signal reached the process before the
 * connection was forked, which is what a terminal Ctrl+C does to the group.
 */
static void test_readyz_reports_draining(void) {
    section("readyz reports draining when it can see it");

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        shutdown_request(SIGTERM);

        http_res_t r = call("GET", "/readyz");
        if (r.status != 200) _exit(2);

        cJSON *j = r.body ? cJSON_Parse(r.body) : NULL;
        if (!j) _exit(3);

        cJSON *ready  = cJSON_GetObjectItem(j, "ready");
        cJSON *reason = cJSON_GetObjectItem(j, "reason");

        int bad = 0;
        if (!cJSON_IsFalse(ready)) bad = 4;
        else if (!cJSON_IsString(reason) ||
                 strstr(reason->valuestring, "shutting down") == NULL) bad = 5;

        cJSON_Delete(j);
        res_free(&r);
        _exit(bad);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    ok("the child ran to completion", WIFEXITED(status));
    eq_int("a draining server reports ready=false and says so",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
}

/* --- the contract ---------------------------------------------------- */

/*
 * Both routes are in the table, which is what makes them reachable at all, and
 * both are capped. A route added to the code and not to the table is a 404; one
 * added without a cap is the only kind that can return an unbounded body.
 */
static void test_both_are_in_the_route_table(void) {
    section("both routes are declared in the table, with a cap");

    size_t n = 0;
    const web_route_t *routes = web_routes(&n);

    int found_health = 0, found_ready = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(routes[i].pattern, "/healthz") == 0) {
            found_health = 1;
            eq_str("healthz is GET", routes[i].method, "GET");
            ok("healthz has a cap", routes[i].max_bytes > 0);
        }
        if (strcmp(routes[i].pattern, "/readyz") == 0) {
            found_ready = 1;
            eq_str("readyz is GET", routes[i].method, "GET");
            ok("readyz has a cap", routes[i].max_bytes > 0);
        }
    }
    ok("/healthz is in the table", found_health);
    ok("/readyz is in the table", found_ready);
}

/* Neither route may be reachable by a path that merely looks like it: the
   dispatch is exact, and these two sit at the root where a prefix match would
   be easiest to get wrong. */
static void test_paths_are_exact(void) {
    section("no prefix or suffix reaches them");

    const char *nope[] = { "/healthz/", "/healthzz", "/readyz/x", "/api/healthz" };
    for (size_t i = 0; i < sizeof(nope) / sizeof(nope[0]); i++) {
        http_res_t r = call("GET", nope[i]);
        char what[128];
        snprintf(what, sizeof(what), "GET %s is 404", nope[i]);
        eq_int(what, r.status, 404);
        res_free(&r);
    }
}

int main(void) {
    printf("\n=== health ===\n");

    /* A jobs directory to be writable, and to take away. */
    if (mkdir("jobs", 0755) != 0 && errno != EEXIST) {
        printf("  cannot create jobs/\n");
        return 1;
    }

    test_healthz();
    test_healthz_rejects_other_methods();
    test_both_are_in_the_route_table();
    test_paths_are_exact();
    test_readyz_ready();
    test_readyz_not_ready_without_a_writable_jobs_dir();
    test_healthz_survives_a_broken_jobs_dir();
    test_readyz_reports_draining();   /* forks; the flag is one-way, so last */

    return t_report("health");
}
