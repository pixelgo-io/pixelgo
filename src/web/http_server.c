#include "http_server.h"
#include "jobs.h"
#include "daemon.h"
#include "shutdown.h"
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
#include <time.h>
#include <poll.h>
#include <sys/un.h>
#include <sys/stat.h>
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

/* How many tracked children are still alive. */
static int live_children(void) {
    int n = 0;
    for (int i = 0; i < MAX_TRACKED_CHILDREN; i++)
        if (g_child_pids[i] != 0) n++;
    return n;
}

#define MAX_DRAINED_JOBS 128

/*
 * The drain.
 *
 * Runs after the accept loop has stopped, in the server process, with signals
 * already delivered - so there is nothing async-signal-safe about it and it can
 * log, sleep and read files.
 *
 * The order matters. The listening socket is already closed, so no new work can
 * arrive. Then the jobs are asked to stop, because they are the ones holding
 * agents; the agents are in their own process groups and only their job knows
 * their pids, so the server cannot shortcut past them (shutdown.h explains why
 * each level has to stop the one below it). Only when the deadline passes does
 * anything get killed.
 *
 * Returns the number of processes that had to be killed - zero means everything
 * stopped when it was asked to.
 */
static int drain_children(void) {
    long timeout = shutdown_timeout_seconds();

    pid_t jobs[MAX_DRAINED_JOBS];
    int job_count = jobs_list_running(jobs, MAX_DRAINED_JOBS);

    LOG_I("http: draining - %d connection child(ren), %d running job(s), "
          "deadline %lds", live_children(), job_count, timeout);

    /*
     * Ask the jobs to stop. A job's own handler (jobs.c) turns this into
     * agent_stop for each agent it started, which signals the agent's GROUP -
     * that is what reaches a `gcc` or a `make` under the agent.
     *
     * The connection children are left alone: each one is serving a single
     * request and exits by itself, and one of them is the job's parent, whose
     * waitpid we would rather not interrupt.
     */
    for (int i = 0; i < job_count; i++) {
        if (kill(jobs[i], SIGTERM) == 0)
            LOG_I("http: asked job pid %d to stop", (int)jobs[i]);
    }

    /* Wait, reaping as they go. SIGCHLD is still installed, so most of the
       reaping happens in the handler; this loop is what notices the end of it. */
    time_t deadline = time(NULL) + timeout;
    for (;;) {
        reap_children(0);

        int remaining = 0;
        for (int i = 0; i < job_count; i++)
            if (jobs[i] > 0 && !(kill(jobs[i], 0) != 0 && errno == ESRCH)) remaining++;

        if (remaining == 0 && live_children() == 0) {
            LOG_I("http: drained cleanly");
            return 0;
        }
        if (time(NULL) >= deadline) {
            LOG_W("http: %d job(s) and %d connection child(ren) still alive at "
                  "the %lds deadline", remaining, live_children(), timeout);
            break;
        }
        struct timespec ts = { 0, 100 * 1000 * 1000 };   /* 100ms */
        nanosleep(&ts, NULL);
    }

    /* The deadline passed: stop asking. */
    int killed = 0;
    for (int i = 0; i < job_count; i++) {
        if (jobs[i] > 0 && kill(jobs[i], SIGKILL) == 0) {
            LOG_W("http: killed job pid %d", (int)jobs[i]);
            killed++;
        }
    }
    for (int i = 0; i < MAX_TRACKED_CHILDREN; i++) {
        pid_t pid = (pid_t)g_child_pids[i];
        if (pid == 0) continue;
        kill(pid, SIGKILL);
        killed++;
    }

    /* Give the kernel a moment to deliver, then reap so nothing is left as a
       zombie for init to adopt. */
    struct timespec ts = { 0, 200 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    reap_children(0);
    return killed;
}

/*
 * The listening sockets.
 *
 * HTTP does not care what kind of stream socket it runs over, so the TCP and
 * unix listeners differ only in how they are set up: everything after accept()
 * - reading the request, routing it, the fork per connection - is the same code
 * for both. That is the whole reason this is worth doing rather than writing a
 * second protocol for pixelgo-ops to speak.
 */
static int listen_tcp(int port) {
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
    return srv;
}

/* Is something already listening on this socket file? */
static int unix_socket_in_use(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);

    int in_use = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return in_use;
}

/*
 * The unix-socket listener (PIXELGO_UNIX_SOCKET).
 *
 * This is the channel pixelgo-ops uses. It matters that it is a FILE: ops can
 * talk to a real pixelgo over it with no network, no VM and no hypervisor in
 * the way, so the two can be developed against each other today rather than
 * after the VM work lands. The vsock listener, once it exists, is the same code
 * again with a different address family.
 *
 * Access control is the filesystem. The socket is created 0600, so only the
 * user running pixelgo can connect - which is the same trust boundary as the
 * loopback bind, expressed in a way that does not involve the network at all.
 */
static int listen_unix(const char *path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    /* sun_path is a fixed-size field, so an over-long path is a configuration
       error rather than something to truncate silently into the wrong socket. */
    if (strlen(path) >= sizeof(addr.sun_path)) {
        LOG_E("http: PIXELGO_UNIX_SOCKET is too long (%zu chars, max %zu)",
              strlen(path), sizeof(addr.sun_path) - 1);
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    /*
     * A socket file left over from a crash looks exactly like a live one, so we
     * ask before removing it: if something answers, that is another pixelgo and
     * we must not take its socket away. If nothing answers, the file is stale.
     */
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            LOG_E("http: '%s' exists and is not a socket, refusing to replace it",
                  path);
            return -1;
        }
        if (unix_socket_in_use(path)) {
            LOG_E("http: '%s' is already in use by another server", path);
            return -1;
        }
        LOG_W("http: removing the stale socket '%s'", path);
        if (unlink(path) != 0) {
            LOG_E("http: cannot remove '%s': %s", path, strerror(errno));
            return -1;
        }
    }

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        LOG_E("http: unix socket() failed: %s", strerror(errno));
        return -1;
    }

    /* The umask decides the mode at bind time; the chmod afterwards is for the
       umasks that would have allowed more. Both, because the window between
       them is the only time the socket is reachable by anyone else. */
    mode_t old_umask = umask(0177);
    int rc = bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_umask);

    if (rc < 0) {
        LOG_E("http: bind on '%s' failed: %s", path, strerror(errno));
        close(srv);
        return -1;
    }
    if (chmod(path, 0600) != 0)
        LOG_W("http: cannot set the mode of '%s': %s", path, strerror(errno));

    if (listen(srv, 16) < 0) {
        LOG_E("http: listen on '%s' failed: %s", path, strerror(errno));
        close(srv);
        unlink(path);
        return -1;
    }
    return srv;
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

    /*
     * Shutdown handlers, WITHOUT SA_RESTART - the 1 says so.
     *
     * This is the one place in the program that needs the interruption: the
     * server spends its life parked in accept(), and with SA_RESTART that call
     * would resume after the signal and the loop would never get to look at the
     * flag. Note that the SIGCHLD handler above keeps SA_RESTART, so the two
     * dispositions differ on purpose and for different reasons.
     *
     * Installing them here also replaces what daemon_start() used to do, which
     * was to delete the PID file and _exit(0) immediately. That was a shutdown
     * only in the sense that the server stopped.
     */
    shutdown_install_handlers(1);

    int srv = listen_tcp(port);
    if (srv < 0) return -1;

    /* The optional second door, for pixelgo-ops. Absent by default: a local
       install gets exactly what it got before. */
    const char *sock_path = getenv("PIXELGO_UNIX_SOCKET");
    int usrv = -1;
    if (sock_path && sock_path[0]) {
        usrv = listen_unix(sock_path);
        if (usrv < 0) { close(srv); return -1; }
    }

    LOG_I("http: server started at http://127.0.0.1:%d", port);
    printf("\n  Web interface: http://127.0.0.1:%d\n", port);
    if (usrv >= 0) {
        LOG_I("http: also listening on the unix socket '%s'", sock_path);
        printf("  API socket:    %s\n", sock_path);
    }
    printf("  Ctrl+C to stop.\n\n");
    fflush(stdout);

    while (!shutdown_requested()) {
        /*
         * Two listeners, one loop. poll() rather than two accept() calls,
         * because a blocking accept on the first socket would starve the
         * second: nobody could reach the API over the socket while no browser
         * happened to connect.
         *
         * No timeout. The shutdown handler is installed without SA_RESTART, so
         * a signal makes this return EINTR, which is how the loop gets to look
         * at the flag; a timeout here would only add wakeups that do nothing.
         */
        struct pollfd pfd[2];
        int n = 0;
        pfd[n].fd = srv;  pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        if (usrv >= 0) { pfd[n].fd = usrv; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }

        if (poll(pfd, (nfds_t)n, -1) < 0) {
            if (errno == EINTR) continue;
            LOG_W("http: poll failed: %s", strerror(errno));
            continue;
        }

        for (int i = 0; i < n && !shutdown_requested(); i++) {
            if (!(pfd[i].revents & POLLIN)) continue;

            int listener = pfd[i].fd;
            int on_unix = (listener == usrv);

            /* The address is read and dropped: nothing downstream uses the peer
               address, and over a unix socket there is not one to speak of. */
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) {
                /* A shutdown signal is what this EINTR usually is: the handler
                   is installed without SA_RESTART precisely so this lands here.
                   The loop condition sorts the two cases out. */
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                LOG_W("http: accept failed: %s", strerror(errno));
                continue;
            }

            /* The signal can also arrive between accept() returning and the
               fork. Closing the connection unserved is better than starting
               work we are about to abandon; the browser sees a dropped
               connection, which is what it would have seen a moment later. */
            if (shutdown_requested()) {
                close(fd);
                break;
            }

            /*
             * CONCURRENCY: one fork per connection. Each client is served in a
             * separate process, so a slow request no longer blocks the others,
             * and a handler that crashes does not take the server with it.
             *
             * Why fork and not threads: the core uses global state and fork for
             * the agents; processes give us isolation for free, without having
             * to make everything thread-safe. For a local control panel, the
             * cost of a fork per request is negligible.
             */
            pid_t pid = fork();

            if (pid == 0) {
                /* --- child: serves the request and exits --- */
                close(srv);                    /* not ours any more */
                if (usrv >= 0) close(usrv);

                http_req_t req;
                http_res_t res;
                memset(&res, 0, sizeof(res));

                if (read_request(fd, &req)) {
                    LOG_D("http: %s %s%s", req.method, req.path,
                          on_unix ? " (socket)" : "");

                    /*
                     * Before routing: is this request allowed to reach a
                     * handler at all? See http_origin_allowed().
                     *
                     * Not over the unix socket. That check defends against a
                     * BROWSER being talked into reaching the API - a rebound
                     * hostname, a form on another page - and it works by
                     * recognising our own authority in Host and Origin. A unix
                     * socket has no authority to recognise and no browser can
                     * open one; the caller is a process that already holds
                     * 0600 access to the socket file, which is the real check.
                     * Applying the rule here would mean guessing what Host a
                     * non-browser client ought to have sent.
                     */
                    if (!on_unix && !http_origin_allowed(&req, port)) {
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

            /* --- parent: closes the client socket, back to the poll --- */
            close(fd);
        }
    }

    /* Stop listening first, so nothing new arrives while we drain. */
    close(srv);
    if (usrv >= 0) {
        close(usrv);
        /* The socket file outlives the socket, and a leftover one is
           indistinguishable from a live server to whoever looks next. */
        if (sock_path && unlink(sock_path) != 0 && errno != ENOENT)
            LOG_W("http: cannot remove '%s': %s", sock_path, strerror(errno));
    }

    LOG_I("http: shutting down");
    printf("\n  Stopping - waiting for the running jobs...\n");
    fflush(stdout);

    int killed = drain_children();

    /* The PID file is ours for as long as we are the server, in both foreground
       and daemon mode; this is the one exit where we can still remove it. */
    daemon_cleanup_pidfile();

    if (killed > 0)
        printf("  Stopped (%d process(es) had to be killed).\n\n", killed);
    else
        printf("  Stopped.\n\n");
    fflush(stdout);

    return 0;
}
