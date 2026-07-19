#include "http_server.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    /* We escape the quotes in the message so we do not break the JSON. */
    char escaped[1024];
    size_t j = 0;
    for (size_t i = 0; message[i] && j < sizeof(escaped) - 2; i++) {
        if (message[i] == '"' || message[i] == '\\') {
            if (j < sizeof(escaped) - 3) escaped[j++] = '\\';
        }
        escaped[j++] = message[i];
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
    (void)raw_len;
    return 1;
}

/* Reads a complete request from the socket (header + body). Returns 1 on success. */
static int read_request(int fd, http_req_t *req) {
    static char buf[HTTP_MAX_BODY + 8192];
    size_t total = 0;
    size_t header_len = 0, content_length = 0;
    int have_header = 0;

    for (;;) {
        if (total >= sizeof(buf) - 1) break;   /* too large */

        ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
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

    if (content_length > 0) {
        if (content_length > HTTP_MAX_BODY) content_length = HTTP_MAX_BODY;
        size_t avail = total - header_len;
        size_t copy = content_length < avail ? content_length : avail;
        memcpy(req->body, buf + header_len, copy);
        req->body[copy] = 0;
        req->body_len = copy;
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

static void send_response(int fd, const http_res_t *res) {
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n",
        res->status, status_text(res->status),
        res->content_type ? res->content_type : "text/plain",
        res->body_len);

    if (n > 0) send_all(fd, header, (size_t)n);
    if (res->body && res->body_len > 0) send_all(fd, res->body, res->body_len);
}

/* --- the server loop --- */

/*
 * Child reaper: the server forks for each request (concurrency) and for each
 * agent job. Without this, the finished processes would remain zombies and fill
 * up the process table.
 * waitpid(-1, WNOHANG) in a loop: reaps ALL finished children, without blocking.
 */
static void reap_children(int sig) {
    (void)sig;
    int saved_errno = errno;   /* the handler must not clobber errno */
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
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
                handler(&req, &res);
                if (res.status == 0)
                    http_res_error(&res, 500, "the handler did not set the response");
                send_response(fd, &res);
                if (res.body_is_heap && res.body) free(res.body);
            }
            close(fd);
            _exit(0);
        }

        if (pid < 0)
            LOG_W("http: fork failed, the request is ignored: %s", strerror(errno));

        /* --- parent: closes the client socket and waits for the next one --- */
        close(fd);
    }

    close(srv);
    return 0;
}
