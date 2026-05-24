/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#define TOUCH_DIAG_JSON_SIZE 1024

static const char *TAG = "http_touch_api";

#ifndef HTTPD_501_METHOD_NOT_ALLOWED
#define HTTPD_501_METHOD_NOT_ALLOWED 501
#endif

static void touch_send_json(httpd_req_t *req, const char *payload)
{
    if (!payload) {
        httpd_resp_send_500(req);
        return;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, payload);
}

static void touch_send_error_json(httpd_req_t *req, int status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddNumberToObject(root, "status", status);
    cJSON_AddStringToObject(root, "error", message ? message : "failed");

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        httpd_resp_send_500(req);
        return;
    }

    char status_text[48];
    snprintf(status_text, sizeof(status_text), "%d", status);
    httpd_resp_set_status(req, status_text);
    touch_send_json(req, payload);
    free(payload);
}

static esp_err_t touch_get_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    if (!ctx->services.touch_get_diagnostics) {
        touch_send_error_json(req, HTTPD_501_METHOD_NOT_ALLOWED, "Touch diagnostics unavailable");
        return ESP_OK;
    }

    char payload[TOUCH_DIAG_JSON_SIZE];
    esp_err_t err = ctx->services.touch_get_diagnostics(payload, sizeof(payload));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch diagnostics failed: %s", esp_err_to_name(err));
        touch_send_error_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to collect touch diagnostics");
        return ESP_OK;
    }

    touch_send_json(req, payload);
    return ESP_OK;
}

static esp_err_t touch_post_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    cJSON *root = NULL;

    if (!ctx->services.touch_run_scene) {
        touch_send_error_json(req, HTTPD_501_METHOD_NOT_ALLOWED, "Touch control unavailable");
        return ESP_OK;
    }

    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        touch_send_error_json(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
        return ESP_OK;
    }

    const char *scene = "showcase";
    cJSON *scene_item = cJSON_GetObjectItemCaseSensitive(root, "scene");
    cJSON *action_item = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (cJSON_IsString(scene_item)) {
        scene = scene_item->valuestring;
    } else if (cJSON_IsString(action_item)) {
        const char *action = action_item->valuestring;
        if (strcmp(action, "listen") == 0 || strcmp(action, "think") == 0 ||
            strcmp(action, "speak") == 0 || strcmp(action, "showcase") == 0) {
            scene = action;
        }
    }

    char scene_buf[16];
    snprintf(scene_buf, sizeof(scene_buf), "%s", scene);

    esp_err_t err = ctx->services.touch_run_scene(scene_buf);
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG) {
        touch_send_error_json(req, HTTPD_400_BAD_REQUEST, "Unsupported touch scene");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch scene failed: %s", esp_err_to_name(err));
        touch_send_error_json(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Touch scene failed");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "scene", scene_buf);
    return http_server_send_json_response(req, resp);
}

esp_err_t http_server_register_touch_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/touch", .method = HTTP_GET, .handler = touch_get_handler },
        { .uri = "/api/touch", .method = HTTP_POST, .handler = touch_post_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register touch route: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
