/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * POST /api/debug/crash
 *   Requires confirm=1 as a query parameter, or a body of either
 *   "confirm=1" (form style) or {"confirm": true} / {"confirm": 1} (JSON).
 *   Without confirmation → 400, no side effects.
 *
 * Deliberate crash hook (STABILITY_PLAN.md P0 crash-evidence chain):
 * logs a warning, answers the request, waits 100 ms so the warning can
 * drain to the SD/serial loggers, then calls abort() on the httpd task.
 * Used to prove end-to-end that a real panic produces a logged reset
 * reason on the next boot and a decodable coredump (once
 * CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH lands, P0.2).
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_debug";

#define DEBUG_CRASH_BODY_MAX 256

static bool debug_crash_body_confirms(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= DEBUG_CRASH_BODY_MAX) {
        return false;
    }

    char body[DEBUG_CRASH_BODY_MAX];
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) {
        return false;
    }
    body[got] = '\0';

    if (strstr(body, "confirm=1") != NULL) {
        return true;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return false;
    }
    cJSON *confirm = cJSON_GetObjectItem(root, "confirm");
    bool ok = cJSON_IsTrue(confirm) ||
              (cJSON_IsNumber(confirm) && confirm->valueint == 1) ||
              (cJSON_IsString(confirm) && confirm->valuestring &&
               strcmp(confirm->valuestring, "1") == 0);
    cJSON_Delete(root);
    return ok;
}

static esp_err_t debug_crash_handler(httpd_req_t *req)
{
    char confirm[8] = {0};
    bool confirmed =
        (http_server_query_get(req, "confirm", confirm, sizeof(confirm)) == ESP_OK &&
         strcmp(confirm, "1") == 0) ||
        debug_crash_body_confirms(req);

    if (!confirmed) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "confirm=1 required (query param or body)");
    }

    ESP_LOGW(TAG,
             "deliberate crash requested via /api/debug/crash — abort() in 100 ms "
             "(crash-evidence chain test)");

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", true);
        http_server_json_add_string(root, "message",
                                    "aborting in 100 ms — expect panic + reboot");
        http_server_send_json_response(req, root);
    }

    /* Let the warning reach the SD-log writer / serial console. */
    vTaskDelay(pdMS_TO_TICKS(100));
    abort();

    return ESP_OK; /* unreachable */
}

esp_err_t http_server_register_debug_routes(httpd_handle_t server)
{
    const httpd_uri_t handler = {
        .uri     = "/api/debug/crash",
        .method  = HTTP_POST,
        .handler = debug_crash_handler,
    };
    return httpd_register_uri_handler(server, &handler);
}
