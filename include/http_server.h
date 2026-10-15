#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stddef.h>

/*
 * Minimal HTTP server, in pure C (POSIX sockets). No external dependencies.
 *
 * It is not a general-purpose server - it implements exactly what we need:
 *   - HTTP/1.1, GET and POST methods
 *   - JSON body on POST
 *   - a single client at a time (sequential)
 *
 * Why sequential and not threaded: a model call takes seconds, and the agents
 * already run isolated in processes. A server with fork per request would
 * complicate things needlessly (agents would become grandchildren, hard to
 * supervise). For a local control panel, sequential is enough and much easier to
 * reason about.
 */

#define HTTP_MAX_PATH 512

/*
 * Request body size: a configurable DEFAULT with a hard-coded CEILING.
 *
 * DEFAULT (64KB) is unchanged from before - anyone who does not opt in sees
 * identical behavior to the original fixed limit.
 *
 * CEILING (4MB) is the absolute maximum PIXELGO_HTTP_MAX_BODY (see
 * http_max_body() in http_server.c) can ever raise the effective limit to,
 * regardless of what is set in the environment. It is sized around the
 * largest context window current model providers accept (~1M tokens,
 * roughly 4MB of raw text at the industry's usual ~4-bytes-per-token rule
 * of thumb) - a request bigger than that could not be sent to the model
 * anyway, no matter how it got past this server.
 *
 * `body` is a pointer, not an embedded array, specifically so that raising
 * the ceiling never grows http_req_t itself: the struct is created as a
 * local (stack) variable per connection in http_serve's fork handler, and a
 * multi-megabyte struct sitting on a process's ~8MB default stack would
 * risk overflowing it on every single connection, not just large-body
 * ones. `body` instead points into a buffer whose lifetime is internal to
 * http_server.c (see read_request) - callers must not free it.
 */
#define HTTP_MAX_BODY_DEFAULT 65536
#define HTTP_MAX_BODY_CEILING (4 * 1024 * 1024)

/* Host and Origin header values. 256 is generous for the names we accept
   (localhost, 127.0.0.1, or whatever PIXELGO_ALLOWED_HOSTS names) and keeps
   http_req_t small enough to stay a stack local in http_serve's child. A
   longer value is truncated, which can only make the check fail - never pass. */
#define HTTP_MAX_HOST 256

typedef struct {
    char method[8];               /* "GET" | "POST"                     */
    char path[HTTP_MAX_PATH];     /* "/api/agents" (without query string) */
    char query[HTTP_MAX_PATH];    /* what comes after '?', may be empty */
    char host[HTTP_MAX_HOST];     /* Host header, "" if absent          */
    char origin[HTTP_MAX_HOST];   /* Origin header, "" if absent        */
    char *body;                   /* the request body (JSON on POST) -
                                      NOT owned by the caller, do not free */
    size_t body_len;
} http_req_t;

/* The response the handler builds. */
typedef struct {
    int status;                   /* 200, 400, 404, 500...              */
    const char *content_type;     /* "application/json", "text/html"... */
    char *body;                   /* buffer owned by the handler        */
    size_t body_len;
    int body_is_heap;             /* 1 if body must be free()d          */
} http_res_t;

/* A handler receives the request and fills in the response. */
typedef void (*http_handler_fn)(const http_req_t *req, http_res_t *res);

/* Starts the server on the given port. Infinite loop (blocks).
   Returns -1 only if it cannot open the socket. */
int http_serve(int port, http_handler_fn handler);

/*
 * Is this request allowed to reach a handler at all?
 *
 * A browser sends requests to localhost on behalf of ANY page the user has
 * open. Without this check two things work with no bug in the browser and
 * nothing installed on the machine:
 *
 *   - any page can POST to http://localhost:<port>/api/... and start a flow,
 *     create an agent with run_command, or approve a pending tool call. It
 *     does not need to read the answer for the damage to be done.
 *   - a site can make its own hostname resolve to 127.0.0.1 (DNS rebinding)
 *     and then read the answers too, because the browser treats that as
 *     same-origin with itself.
 *
 * So: Host must name us, which is what defeats rebinding - the rebinding
 * site's requests carry its own hostname. And for a state-changing method,
 * Origin must either be absent (curl, a script - not a browser acting for a
 * third party) or name us as well.
 *
 * The accepted names are localhost and 127.0.0.1, plus anything listed in
 * PIXELGO_ALLOWED_HOSTS (comma-separated, no port) - needed when PIXELGO_BIND
 * puts the server behind a container port mapping or a reverse proxy, where
 * the Host the client uses is not one we can guess.
 *
 * This is NOT authentication: it stops other web pages, not other local
 * processes. Returns 1 to allow, 0 to refuse with 403.
 */
int http_origin_allowed(const http_req_t *req, int port);

/* Helpers for handlers: set the response.
   http_res_json copies the given text (heap). http_res_static does not copy. */
void http_res_json(http_res_t *res, int status, const char *json);
void http_res_static(http_res_t *res, int status, const char *content_type,
                     const char *body, size_t len);
void http_res_error(http_res_t *res, int status, const char *message);

/* Extracts a parameter's value from the query string ("a=1&b=2").
   Returns 1 if it was found. */
int http_query_get(const char *query, const char *key, char *out, size_t out_size);

/*
 * The EFFECTIVE request body cap for this run: PIXELGO_HTTP_MAX_BODY from
 * the environment if set (a raw byte count or a KB/MB/GB size like "2MB" -
 * same format, same parser, as PIXELGO_DATA_THRESHOLD/--data-threshold -
 * clamped to [HTTP_MAX_BODY_DEFAULT, HTTP_MAX_BODY_CEILING]), otherwise
 * HTTP_MAX_BODY_DEFAULT. Read fresh from getenv() each call - same
 * convention as PIXELGO_COMMAND_TIMEOUT (exec_tools.c) and
 * PIXELGO_MAX_ITERATIONS (ai_loop.c) - not cached at startup. Exposed here
 * (not static in http_server.c) so routes that need to tell a client the
 * real limit - e.g. the data-edit dialog's byte counter - report the
 * actual configured value, not a stale guess.
 */
size_t http_max_body(void);

#endif
