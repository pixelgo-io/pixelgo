#include "http_server.h"
#include "log.h"
#include "data_request_audit.h"   /* for the shared data_threshold_parse() -
                                     see http_max_body() below: KB/MB/GB/off
                                     parsing lives there once, not duplicated
                                     here as a second strtol()-only parser */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp - in <strings.h> on macOS, a target platform */
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

/* --- response helpers --- */

void http_res_json(http_res_t *res, int status, const char *json) {
    size_t len = strlen(json);
    res->body = malloc(len + 1);
    if (!res->body) { res->status = 500; res->body_len = 0; return; }
    memcpy(res->body, json, len + 1);
    res->status = status;
    res->content_type = "application/json; charset=utf-8";
    res->body_len = len;
    res->body_is_heap = 1;
}

void http_res_static(http_res_t *res, int status, const char *content_type,
                     const char *body, size_t len) {
    res->status = status;
    res->content_type = content_type;
    res->body = (char *)body;      /* we do not copy - it is static */
    res->body_len = len;
    res->body_is_heap = 0;
}

void http_res_error(http_res_t *res, int status, const char *message) {
    /*
     * Escape everything JSON requires inside a string: the quote, the
     * backslash, and every control character below 0x20. Only escaping the
     * first two (as this used to) produced invalid JSON as soon as a message
     * carried a newline - and error messages can include caller-supplied
     * text. The longest escape is 6 bytes (\u00XX), so the loop stops while
     * there is still room for one more plus the terminator: a long message
     * is truncated, never broken mid-escape.
     */
    char escaped[1024];
    size_t j = 0;
    for (size_t i = 0; message[i] && j + 7 <= sizeof(escaped); i++) {
        unsigned char c = (unsigned char)message[i];
        if (c == '"' || c == '\\') {
            escaped[j++] = '\\';
            escaped[j++] = (char)c;
        } else if (c == '\n') {
            escaped[j++] = '\\'; escaped[j++] = 'n';
        } else if (c == '\r') {
            escaped[j++] = '\\'; escaped[j++] = 'r';
        } else if (c == '\t') {
            escaped[j++] = '\\'; escaped[j++] = 't';
        } else if (c < 0x20) {
            j += (size_t)snprintf(escaped + j, sizeof(escaped) - j, "\\u%04x", c);
        } else {
            escaped[j++] = (char)c;
        }
    }
    escaped[j] = 0;

    char json[1200];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", escaped);
    http_res_json(res, status, json);
}

int http_query_get(const char *query, const char *key, char *out, size_t out_size) {
    if (!query || !key || !out || out_size == 0) return 0;
    out[0] = 0;

    size_t keylen = strlen(key);
    const char *p = query;

    while (*p) {
        /* we look for "key=" at the start of a parameter */
        if (strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
            const char *val = p + keylen + 1;
            const char *end = strchr(val, '&');
            size_t len = end ? (size_t)(end - val) : strlen(val);
            if (len >= out_size) len = out_size - 1;
            memcpy(out, val, len);
            out[len] = 0;
            return 1;
        }
        /* skip to the next parameter */
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

/* --- parsing the request --- */

/*
 * Copies one header's value into out, trimmed of surrounding spaces.
 *
 * `name` carries its own leading "\r\n" so the search can never match the
 * request line - only a real header. The match is case-insensitive because
 * header names are, and the search is bounded by hdr_end so a value in the
 * BODY that happens to look like a header is never picked up.
 *
 * out is always NUL-terminated, and is left empty when the header is absent.
 */
static void header_value(const char *raw, const char *hdr_end,
                         const char *name, char *out, size_t out_size) {
    out[0] = 0;

    const char *p = strcasestr(raw, name);
    if (!p || p >= hdr_end) return;

    p = strchr(p, ':');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    const char *end = strstr(p, "\r\n");
    if (!end || end > hdr_end) end = hdr_end;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;

    size_t len = (size_t)(end - p);
    if (len >= out_size) len = out_size - 1;   /* truncation can only fail the
                                                  check, never pass it */
    memcpy(out, p, len);
    out[len] = 0;
}

/* Parses the request line and the headers; determines Content-Length.
   Returns 1 on success. */
static int parse_request(const char *raw, size_t raw_len, http_req_t *req,
                         size_t *header_len, size_t *content_length) {
    memset(req, 0, sizeof(*req));
    *content_length = 0;

    /* find the end of the headers */
    const char *hdr_end = strstr(raw, "\r\n\r\n");
    if (!hdr_end) return 0;   /* incomplete header - we read more */
    *header_len = (size_t)(hdr_end - raw) + 4;

    /* line 1: METHOD /path HTTP/1.1 */
    const char *sp1 = strchr(raw, ' ');
    if (!sp1) return 0;
    size_t mlen = (size_t)(sp1 - raw);
    if (mlen >= sizeof(req->method)) return 0;
    memcpy(req->method, raw, mlen);
    req->method[mlen] = 0;

    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return 0;
    size_t plen = (size_t)(sp2 - (sp1 + 1));
    if (plen >= HTTP_MAX_PATH) plen = HTTP_MAX_PATH - 1;

    char full_path[HTTP_MAX_PATH];
    memcpy(full_path, sp1 + 1, plen);
    full_path[plen] = 0;

    /* split the path from the query string */
    char *q = strchr(full_path, '?');
    if (q) {
        *q = 0;
        snprintf(req->query, HTTP_MAX_PATH, "%s", q + 1);
    }
    snprintf(req->path, HTTP_MAX_PATH, "%s", full_path);

    /* Content-Length (case-insensitive, a simple search) */
    const char *cl = strcasestr(raw, "\r\ncontent-length:");
    if (cl && cl < hdr_end) {
        cl = strchr(cl, ':');
        if (cl) *content_length = (size_t)strtoul(cl + 1, NULL, 10);
    }

    header_value(raw, hdr_end, "\r\nhost:",   req->host,   sizeof(req->host));
    header_value(raw, hdr_end, "\r\norigin:", req->origin, sizeof(req->origin));

    (void)raw_len;
    return 1;
}

/* --- who is allowed to reach a handler (see http_server.h) --- */

/* Is this hostname one of ours? localhost and 127.0.0.1 always; anything in
   PIXELGO_ALLOWED_HOSTS as well, for a container mapping or a reverse proxy,
   where the Host the client uses is not one we could guess. */
static int name_is_ours(const char *name, size_t len) {
    if (len == 0) return 0;

    /* both are nine characters, which is a happy accident */
    if (len == 9 && (strncasecmp(name, "localhost", 9) == 0 ||
                     strncasecmp(name, "127.0.0.1", 9) == 0))
        return 1;

    const char *list = getenv("PIXELGO_ALLOWED_HOSTS");
    if (!list) return 0;

    while (*list) {
        while (*list == ',' || *list == ' ') list++;
        const char *end = list;
        while (*end && *end != ',' && *end != ' ') end++;

        size_t elen = (size_t)(end - list);
        if (elen == len && strncasecmp(list, name, len) == 0) return 1;
        list = end;
    }
    return 0;
}

/* "name" or "name:port" - ours, and on the port we actually bound?
   A bare name means port 80, so it only passes when that is where we are. */
static int authority_is_ours(const char *authority, int port) {
    if (!authority || !authority[0]) return 0;

    const char *colon = strrchr(authority, ':');
    size_t nlen;

    if (colon) {
        nlen = (size_t)(colon - authority);
        char *endp = NULL;
        long p = strtol(colon + 1, &endp, 10);
        if (!endp || *endp != 0 || p != (long)port) return 0;
    } else {
        nlen = strlen(authority);
        if (port != 80) return 0;
    }
    return name_is_ours(authority, nlen);
}

int http_origin_allowed(const http_req_t *req, int port) {
    if (!req) return 0;

    /* Host must name us. This is what defeats DNS rebinding: the rebinding
       site's requests carry its own hostname, not ours. A request with no
       Host at all is refused too - HTTP/1.1 requires one, and every browser
       and every version of curl sends it. */
    if (!authority_is_ours(req->host, port)) return 0;

    /* A safe method with a correct Host is fine. A third-party page can still
       cause a GET, but no GET route changes anything, and the page cannot read
       the answer: we grant no CORS. */
    if (strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0)
        return 1;

    /* State-changing. No Origin means the caller is not a browser acting for
       some page - curl, a script, our own tests. Allowed, and it has already
       passed the Host check. */
    if (req->origin[0] == 0) return 1;

    /* There is an Origin, so a browser is telling us which page is asking.
       It has to be us. Anything carrying a scheme we do not serve - "null",
       a local file, a browser extension - is refused. */
    const char *o = req->origin;
    if      (strncasecmp(o, "http://",  7) == 0) o += 7;
    else if (strncasecmp(o, "https://", 8) == 0) o += 8;
    else return 0;

    return authority_is_ours(o, port);
}

/*
 * PIXELGO_HTTP_MAX_BODY, read fresh each call (see the doc comment in
 * http_server.h for why this is not cached at startup).
 *
 * Accepts exactly what PIXELGO_DATA_THRESHOLD and --data-threshold accept -
 * a raw byte count, or a size with a KB/MB/GB suffix (case-insensitive,
 * e.g. "2MB") - via the SAME shared parser (data_threshold_parse(), in
 * data_request_audit.c). Before this, HTTP_MAX_BODY only took raw bytes
 * ("2097152") while the data-threshold settings took friendly sizes
 * ("2MB") - same unit (bytes), same project, two different formats for no
 * real reason. One parser now backs all three.
 *
 * "off" is also accepted by that parser (it means 0 bytes), but 0 is not a
 * meaningful body-size cap - the server always needs SOME buffer. Rather
 * than special-casing "off" as an error here, it is simplest to let it
 * fall straight into the "too small" clamp below: 0 bytes is obviously
 * below HTTP_MAX_BODY_DEFAULT, so it gets raised to the default exactly
 * like any other too-small value would, with the same warning explaining
 * why.
 *
 * A malformed or out-of-range value falls back to the default rather than
 * failing the request - a misconfigured limit should degrade to "the old
 * behavior", not take the server down.
 */
size_t http_max_body(void) {
    const char *env = getenv("PIXELGO_HTTP_MAX_BODY");
    if (!env || !env[0]) return HTTP_MAX_BODY_DEFAULT;

    long v = 0;
    if (!data_threshold_parse(env, &v)) {
        LOG_W("PIXELGO_HTTP_MAX_BODY='%s' could not be parsed (expected e.g. "
              "\"2097152\", \"2MB\", or \"500KB\"), using the %d-byte default",
              env, HTTP_MAX_BODY_DEFAULT);
        return HTTP_MAX_BODY_DEFAULT;
    }
    if ((size_t)v > HTTP_MAX_BODY_CEILING) {
        LOG_W("PIXELGO_HTTP_MAX_BODY='%s' (%ld bytes) exceeds the %d-byte "
              "ceiling (around the largest context window current model "
              "providers accept) - clamping to it", env, v, HTTP_MAX_BODY_CEILING);
        return HTTP_MAX_BODY_CEILING;
    }
    if ((size_t)v < HTTP_MAX_BODY_DEFAULT) {
        /* Letting it go lower than the original fixed limit would silently
           reject requests every other route here already relies on being
           able to send (workspace/agent configs, etc.) - raise-only. This
           is also where "off" (parsed as 0 bytes, see the comment above)
           lands, with the same message as any other too-small value. */
        LOG_W("PIXELGO_HTTP_MAX_BODY='%s' (%ld bytes) is below the %d-byte "
              "default - using the default instead", env, v, HTTP_MAX_BODY_DEFAULT);
        return HTTP_MAX_BODY_DEFAULT;
    }
    return (size_t)v;
}

/* Reads a complete request from the socket (header + body). Returns 1 on success. */
static int read_request(int fd, http_req_t *req) {
    /*
     * Sized to the hard CEILING, once, as STATIC (not stack) memory - large
     * but harmless, since static/BSS storage does not compete with a
     * process's stack the way a local variable of the same size would.
     * req->body below points straight into this buffer instead of being
     * copied into a separately allocated one: one less allocation, and
     * nothing to free afterward (each forked child handles exactly one
     * request and exits - see http_serve - so there is no reuse to worry
     * about between requests).
     */
    static char buf[HTTP_MAX_BODY_CEILING + 8192];

    size_t max_body = http_max_body();
    size_t buf_limit = max_body + 8192;   /* header allowance, same as before */
    if (buf_limit > sizeof(buf)) buf_limit = sizeof(buf);   /* defensive */

    size_t total = 0;
    size_t header_len = 0, content_length = 0;
    int have_header = 0;

    for (;;) {
        if (total >= buf_limit - 1) break;   /* too large */

        ssize_t n = recv(fd, buf + total, buf_limit - 1 - total, 0);
        if (n <= 0) return 0;                  /* client closed / error */
        total += (size_t)n;
        buf[total] = 0;

        if (!have_header) {
            if (parse_request(buf, total, req, &header_len, &content_length))
                have_header = 1;
            else
                continue;   /* incomplete header, we read more */
        }

        /* we have the header; do we still need the whole body? */
        if (total >= header_len + content_length) break;
    }

    if (!have_header) return 0;

    req->body = buf + header_len;   /* alias into the static buffer, no copy */
    if (content_length > 0) {
        if (content_length > max_body) content_length = max_body;
        size_t avail = total - header_len;
        size_t copy = content_length < avail ? content_length : avail;
        req->body[copy] = 0;
        req->body_len = copy;
    } else {
        req->body[0] = 0;
        req->body_len = 0;
    }
    return 1;
}

/* --- sending the response --- */

static const char *status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

static void send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

/*
 * Content-Security-Policy: the net under the UI's escaping.
 *
 * The page loads nothing from anywhere - one inline <script>, no inline
 * on*="..." handlers, no external stylesheet, font or image - so everything
 * can be denied by default. What each directive buys:
 *
 *   default-src 'none'   nothing loads unless named below
 *   connect-src 'self'   fetch/XHR only back to us. This is the important
 *                        one: a string that did slip past escaping could
 *                        still not send what it read anywhere.
 *   img-src 'self' data: the graph's inline SVG and data: URIs
 *   base-uri / form-action 'none'   no base-tag hijack, no form posted away
 *   frame-ancestors 'none'          no clickjacking
 *
 * script-src is 'unsafe-inline' because the page's own script is inline and
 * has no nonce yet. That does not stop injected inline script from running -
 * escaping is what does - but with connect-src above, such a script has
 * nowhere to send anything. Giving the script a per-response nonce, which
 * would block injected script outright, is a separate change.
 */
#define CSP_HEADER \
    "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; " \
    "img-src 'self' data:; connect-src 'self'; base-uri 'none'; " \
    "form-action 'none'; frame-ancestors 'none'"

static void send_response(int fd, const http_res_t *res) {
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Security-Policy: %s\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "\r\n",
        res->status, status_text(res->status),
        res->content_type ? res->content_type : "text/plain",
        res->body_len,
        CSP_HEADER);

    if (n > 0) send_all(fd, header, (size_t)n);
    if (res->body && res->body_len > 0) send_all(fd, res->body, res->body_len);
}

/* --- the server loop --- */

/*
 * Child reaper.
 *
 * The server forks for each request, and those processes must not be left as
 * zombies. But the reaper must collect ONLY those: a job's orchestrator runs in
 * a descendant of this process and does its own waitpid() on the agents it
 * started.
 *
 * The bug this guards against: reaping with waitpid(-1, NULL, ...) matches
 * EVERY child, so whenever the handler fired first it consumed an agent's exit
 * status and threw it away. The orchestrator's waitpid then failed with ECHILD,
 * agent_wait read that as AGENT_FAILED, and the graph stopped on a node that
 * had actually succeeded - while the agent process carried on writing files.
 * Short tool calls almost never lost the race; a multi-minute `docker compose
 * build` lost it every time.
 *
 * So we track the pids we fork and reap them individually. The table is fixed
 * and touched from a signal handler, so everything here stays
 * async-signal-safe: no allocation, no stdio, only sig_atomic_t slots.
 */
#define MAX_TRACKED_CHILDREN 256

static volatile sig_atomic_t g_child_pids[MAX_TRACKED_CHILDREN];

static void track_child(pid_t pid) {
    for (int i = 0; i < MAX_TRACKED_CHILDREN; i++) {
        if (g_child_pids[i] == 0) { g_child_pids[i] = (sig_atomic_t)pid; return; }
    }
    /*
     * The table is full: reap this one synchronously rather than leaking a
     * zombie. Blocking here is acceptable because it only happens under a flood
     * of concurrent requests, and only for one of them.
     */
    LOG_W("http: child table full, reaping pid %d synchronously", (int)pid);
    waitpid(pid, NULL, 0);
}

static void reap_children(int sig) {
    (void)sig;
    int saved_errno = errno;   /* the handler must not clobber errno */

    for (int i = 0; i < MAX_TRACKED_CHILDREN; i++) {
        pid_t pid = (pid_t)g_child_pids[i];
        if (pid == 0) continue;
        if (waitpid(pid, NULL, WNOHANG) == pid) g_child_pids[i] = 0;
    }

    errno = saved_errno;
}

int http_serve(int port, http_handler_fn handler) {
    /* We do not want to die if a client abruptly closes the connection. */
    signal(SIGPIPE, SIG_IGN);

    /* We reap the finished children (served requests + agent jobs). */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        LOG_E("http: socket() failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;

    /*
     * Bind to 127.0.0.1 by default: the web UI has NO AUTHENTICATION, so it
     * must not be reachable from the network.
     *
     * PIXELGO_BIND=0.0.0.0 exists for containers, where 127.0.0.1 means the
     * container's own loopback and a published port would never reach us.
     * Only set it when something else controls access - publish the port as
     * "127.0.0.1:8080:8080", or put an authenticating reverse proxy in front.
     */
    const char *bind_addr = getenv("PIXELGO_BIND");
    if (bind_addr && bind_addr[0]) {
        if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
            LOG_E("http: PIXELGO_BIND='%s' is not a valid IPv4 address", bind_addr);
            close(srv);
            return -1;
        }
        if (strcmp(bind_addr, "127.0.0.1") != 0) {
            LOG_W("http: listening on %s - the web UI has no authentication. "
                  "Make sure access is restricted.", bind_addr);
        }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* localhost ONLY */
    }

    addr.sin_port = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_E("http: bind on port %d failed: %s", port, strerror(errno));
        close(srv);
        return -1;
    }
    if (listen(srv, 16) < 0) {
        LOG_E("http: listen failed: %s", strerror(errno));
        close(srv);
        return -1;
    }

    LOG_I("http: server started at http://127.0.0.1:%d", port);
    printf("\n  Web interface: http://127.0.0.1:%d\n", port);
    printf("  Ctrl+C to stop.\n\n");
    fflush(stdout);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int fd = accept(srv, (struct sockaddr *)&cli, &clilen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            LOG_W("http: accept failed: %s", strerror(errno));
            continue;
        }

        /*
         * CONCURRENCY: one fork per connection. Each client is served in a
         * separate process, so a slow request no longer blocks the others, and a
         * handler that crashes does not take the server with it.
         *
         * Why fork and not threads: the core uses global state and fork for the
         * agents; processes give us isolation for free, without having to make
         * everything thread-safe. For a local control panel, the cost of a fork
         * per request is negligible.
         */
        pid_t pid = fork();

        if (pid == 0) {
            /* --- child: serves the request and exits --- */
            close(srv);           /* the child does not need the listening socket */

            http_req_t req;
            http_res_t res;
            memset(&res, 0, sizeof(res));

            if (read_request(fd, &req)) {
                LOG_D("http: %s %s", req.method, req.path);

                /* Before routing: is this request allowed to reach a handler
                   at all? See http_origin_allowed() in http_server.h. */
                if (!http_origin_allowed(&req, port)) {
                    LOG_W("http: refused %s %s (Host='%s' Origin='%s')",
                          req.method, req.path, req.host, req.origin);
                    http_res_error(&res, 403, "forbidden: Host or Origin is not this server");
                } else {
                    handler(&req, &res);
                    if (res.status == 0)
                        http_res_error(&res, 500, "the handler did not set the response");
                }
                send_response(fd, &res);
                if (res.body_is_heap && res.body) free(res.body);
            }
            close(fd);
            _exit(0);
        }

        if (pid < 0)
            LOG_W("http: fork failed, the request is ignored: %s", strerror(errno));
        else
            track_child(pid);   /* so the reaper knows this one is ours */

        /* --- parent: closes the client socket and waits for the next one --- */
        close(fd);
    }

    close(srv);
    return 0;
}
