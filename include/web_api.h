#ifndef WEB_API_H
#define WEB_API_H

#include <stddef.h>
#include "http_server.h"

/*
 * The HTTP API's route table: the single place, in code, where the routes are
 * defined. docs/API.md is the written contract; tests/t_api_contract.c checks
 * that the two list exactly the same routes, with the same content type and
 * maximum size, so neither can change without the other.
 *
 * Dispatch goes through this table and nothing else: a path that matches no
 * entry is a 404, a path that matches an entry for another method is a 405.
 * There is no prefix or substring matching.
 */

/* Version of the contract in docs/API.md. Bump it when a route is added,
   removed, or changes its request or response in a way a client can see. */
#define WEB_API_CONTRACT_VERSION 1

typedef void (*web_route_fn)(const http_req_t *req, http_res_t *res);

typedef struct {
    const char  *method;        /* "GET" | "POST"                                */
    const char  *pattern;       /* exact path; "{id}" stands for one job id      */
    const char  *content_type;  /* content type of a SUCCESSFUL response;
                                   errors are always application/json            */
    size_t       max_bytes;     /* hard cap on the response body, errors included */
    web_route_fn fn;
} web_route_t;

/* The route table and its length. */
const web_route_t *web_routes(size_t *count);

/* The request handler passed to http_serve(). */
void web_handler(const http_req_t *req, http_res_t *res);

#endif
