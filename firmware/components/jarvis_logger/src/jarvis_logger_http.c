/*
 * jarvis_logger_http - GET /api/logs route for the persistent logger.
 *
 * Streams the tail of the active log file as text/plain so logs can be pulled
 * over Wi-Fi without USB or popping the SD card. Registered onto the running
 * http_server via jarvis_logger_register_http() (called from the http_server
 * route setup). Same trust boundary as /api/config (open on the local network).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "jarvis_logger.h"

#define JL_TAIL_DEFAULT   (16 * 1024)     /* 16 KB */
#define JL_TAIL_MAX       (256 * 1024)    /* 256 KB cap */

static esp_err_t jarvis_logs_get_handler(httpd_req_t *req)
{
    /* ---- parse optional ?tail=N ---- */
    long tail = JL_TAIL_DEFAULT;
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1 && qlen < 128) {
        char query[128];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[16];
            if (httpd_query_key_value(query, "tail", val, sizeof(val)) == ESP_OK) {
                long v = strtol(val, NULL, 10);
                if (v > 0) {
                    tail = v;
                }
            }
        }
    }
    if (tail > JL_TAIL_MAX) {
        tail = JL_TAIL_MAX;
    }

    const char *path = jarvis_logger_current_path();
    if (!path) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "logger not initialised\n");
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "log file unavailable\n");
    }

    /* ---- seek to max(0, size - tail) ---- */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        size = 0;
    }
    long start = size - tail;
    if (start < 0) {
        start = 0;
    }
    fseek(f, start, SEEK_SET);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    /* ---- stream the tail in chunks ---- */
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            /* abort the response on transport error */
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);

    /* terminate the chunked response */
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t jarvis_logger_register_http(void *server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    const httpd_uri_t logs_uri = {
        .uri     = "/api/logs",
        .method  = HTTP_GET,
        .handler = jarvis_logs_get_handler,
    };
    return httpd_register_uri_handler((httpd_handle_t)server, &logs_uri);
}
