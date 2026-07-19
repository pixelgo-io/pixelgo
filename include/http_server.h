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
#define HTTP_MAX_BODY 65536

typedef struct {
    char method[8];               /* "GET" | "POST"                     */
    char path[HTTP_MAX_PATH];     /* "/api/agents" (without query string) */
    char query[HTTP_MAX_PATH];    /* what comes after '?', may be empty */
    char body[HTTP_MAX_BODY];     /* the request body (JSON on POST)    */
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

/* Helpers for handlers: set the response.
   http_res_json copies the given text (heap). http_res_static does not copy. */
void http_res_json(http_res_t *res, int status, const char *json);
void http_res_static(http_res_t *res, int status, const char *content_type,
                     const char *body, size_t len);
void http_res_error(http_res_t *res, int status, const char *message);

/* Extracts a parameter's value from the query string ("a=1&b=2").
   Returns 1 if it was found. */
int http_query_get(const char *query, const char *key, char *out, size_t out_size);

#endif
