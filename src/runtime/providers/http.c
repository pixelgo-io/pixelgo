#include "provider_internal.h"
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_TIMEOUT_SEC 120L

/* --- common HTTP helper (curl), reused by all the adapters --- */

struct curl_buffer {
    char *data;
    size_t size;
    size_t cap;
};

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct curl_buffer *buf = (struct curl_buffer *)userp;
    size_t copy = total;
    if (buf->size + copy > buf->cap) copy = buf->cap - buf->size;
    if (copy > 0) {
        memcpy(buf->data + buf->size, contents, copy);
        buf->size += copy;
        buf->data[buf->size] = 0;
    }
    return total;
}

int http_post_json(const http_request_t *req, http_response_t *resp) {
    resp->http_code = 0;
    resp->curl_ok = 0;
    resp->size = 0;
    resp->curl_error[0] = 0;
    if (resp->data) resp->data[0] = 0;

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(resp->curl_error, sizeof(resp->curl_error), "curl init failed");
        return -1;
    }

    struct curl_buffer buf;
    buf.data = resp->data;
    buf.size = 0;
    buf.cap  = PROVIDER_RESPONSE_MAX - 1;

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req->headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, req->timeout_sec > 0 ? req->timeout_sec : DEFAULT_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->http_code);
    resp->size = buf.size;

    if (res != CURLE_OK) {
        snprintf(resp->curl_error, sizeof(resp->curl_error), "%s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return -1;
    }
    resp->curl_ok = 1;
    curl_easy_cleanup(curl);
    return 0;
}

