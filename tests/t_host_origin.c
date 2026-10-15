/*
 * Who is allowed to reach a handler: the Host and Origin check.
 *
 * The attack this exists to stop needs no bug in the browser and nothing
 * installed on the machine. A page the user visits can make the browser send
 * requests to http://localhost:8080 on its behalf, and - if it points its own
 * hostname at 127.0.0.1 - read the answers as well.
 *
 * These tests drive http_origin_allowed() directly rather than through a
 * socket: it is a pure function of the request and the bound port, which is
 * exactly why it was split out of the accept loop.
 */

#include "test.h"
#include "http_server.h"
#include <string.h>
#include <stdlib.h>

#define PORT 8080

/* Builds a request. host/origin may be NULL for "the header was absent". */
static http_req_t mk(const char *method, const char *path,
                     const char *host, const char *origin) {
    http_req_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.method, sizeof(r.method), "%s", method);
    snprintf(r.path, sizeof(r.path), "%s", path);
    if (host)   snprintf(r.host,   sizeof(r.host),   "%s", host);
    if (origin) snprintf(r.origin, sizeof(r.origin), "%s", origin);
    return r;
}

static void allowed(const char *what, http_req_t r, int port) {
    ok(what, http_origin_allowed(&r, port) == 1);
}

static void refused(const char *what, http_req_t r, int port) {
    ok(what, http_origin_allowed(&r, port) == 0);
}

int main(void) {
    unsetenv("PIXELGO_ALLOWED_HOSTS");

    section("the embedded UI keeps working, in both of its forms");
    allowed("GET with Host: localhost:8080",
            mk("GET", "/", "localhost:8080", NULL), PORT);
    allowed("GET with Host: 127.0.0.1:8080",
            mk("GET", "/", "127.0.0.1:8080", NULL), PORT);
    allowed("POST from the page itself (localhost)",
            mk("POST", "/api/chat", "localhost:8080", "http://localhost:8080"), PORT);
    allowed("POST from the page itself (127.0.0.1)",
            mk("POST", "/api/chat", "127.0.0.1:8080", "http://127.0.0.1:8080"), PORT);
    allowed("Host is matched case-insensitively, as the RFC says",
            mk("GET", "/", "LocalHost:8080", NULL), PORT);

    section("a page on another site cannot drive the API");
    refused("GET with Host: evil.example:8080",
            mk("GET", "/", "evil.example:8080", NULL), PORT);
    refused("POST with Host: evil.example:8080",
            mk("POST", "/api/chat", "evil.example:8080", NULL), PORT);
    refused("a rebinding host that resolves to 127.0.0.1 still carries its own name",
            mk("GET", "/api/agents", "rebind.evil.example:8080", NULL), PORT);
    refused("POST with a good Host but Origin: http://evil.example",
            mk("POST", "/api/chat", "localhost:8080", "http://evil.example"), PORT);
    refused("POST with Origin: https://evil.example",
            mk("POST", "/api/chat", "localhost:8080", "https://evil.example"), PORT);
    refused("POST with Origin: null (a sandboxed iframe)",
            mk("POST", "/api/chat", "localhost:8080", "null"), PORT);
    refused("POST with a file:// Origin",
            mk("POST", "/api/chat", "localhost:8080", "file://"), PORT);
    refused("POST with a browser-extension Origin",
            mk("POST", "/api/chat", "localhost:8080", "chrome-extension://editor"), PORT);

    section("the port has to be ours too");
    refused("right name, wrong port",
            mk("GET", "/", "localhost:9999", NULL), PORT);
    refused("a bare name means port 80, and we are not on 80",
            mk("GET", "/", "localhost", NULL), PORT);
    allowed("a bare name is right when we ARE on port 80",
            mk("GET", "/", "localhost", NULL), 80);
    refused("Origin on the right host but the wrong port",
            mk("POST", "/api/chat", "localhost:8080", "http://localhost:9999"), PORT);
    refused("a port with trailing junk is not a port",
            mk("GET", "/", "localhost:8080x", NULL), PORT);

    section("a missing or malformed Host is refused, not assumed");
    refused("no Host header at all",
            mk("GET", "/", NULL, NULL), PORT);
    refused("an empty Host header",
            mk("GET", "/", "", NULL), PORT);
    refused("a Host that is only a colon and a port",
            mk("GET", "/", ":8080", NULL), PORT);
    refused("a Host that is a prefix of ours",
            mk("GET", "/", "localhos:8080", NULL), PORT);
    refused("a Host that is ours plus a suffix",
            mk("GET", "/", "localhost.evil.example:8080", NULL), PORT);

    section("curl and scripts keep working: no Origin is not a browser");
    allowed("GET with no Origin",
            mk("GET", "/api/agents", "127.0.0.1:8080", NULL), PORT);
    allowed("POST with no Origin",
            mk("POST", "/api/workspaces", "127.0.0.1:8080", NULL), PORT);
    allowed("DELETE with no Origin",
            mk("DELETE", "/api/agents", "127.0.0.1:8080", NULL), PORT);

    section("PIXELGO_ALLOWED_HOSTS, for a container mapping or a reverse proxy");
    setenv("PIXELGO_ALLOWED_HOSTS", "pixelgo.example.com", 1);
    allowed("the named host is accepted",
            mk("GET", "/", "pixelgo.example.com:8080", NULL), PORT);
    allowed("and as an Origin on a POST",
            mk("POST", "/api/chat", "pixelgo.example.com:8080",
               "https://pixelgo.example.com:8080"), PORT);
    refused("a host NOT in the list is still refused",
            mk("GET", "/", "other.example.com:8080", NULL), PORT);
    allowed("localhost still works alongside it",
            mk("GET", "/", "localhost:8080", NULL), PORT);

    setenv("PIXELGO_ALLOWED_HOSTS", "a.example , b.example,c.example", 1);
    allowed("a comma-and-space separated list: first entry",
            mk("GET", "/", "a.example:8080", NULL), PORT);
    allowed("a comma-and-space separated list: middle entry",
            mk("GET", "/", "b.example:8080", NULL), PORT);
    allowed("a comma-and-space separated list: last entry",
            mk("GET", "/", "c.example:8080", NULL), PORT);
    refused("an entry that is only a prefix of the list's is not a match",
            mk("GET", "/", "a.exampl:8080", NULL), PORT);

    setenv("PIXELGO_ALLOWED_HOSTS", "", 1);
    refused("an empty list grants nothing",
            mk("GET", "/", "other.example:8080", NULL), PORT);
    allowed("an empty list does not take localhost away either",
            mk("GET", "/", "localhost:8080", NULL), PORT);
    unsetenv("PIXELGO_ALLOWED_HOSTS");

    section("a NULL request is refused rather than crashing");
    ok("NULL", http_origin_allowed(NULL, PORT) == 0);

    return t_report("host_origin");
}
