/*
 * SPDX-FileCopyrightText: 2026 JarvisRobot
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GET  /api/gemini/live
 * POST /api/gemini/live {"action":"start"|"stop"|"text"|"end_input","text":"..."}
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "cap_gemini_live.h"
#include "esp_log.h"

static const char *TAG = "http_gemini_api";

/* Diag JSON is >1 KB once lifecycle, audio, AEC, and tool counters are present. */
#define GEMINI_DIAG_JSON_SIZE 4096

static const char *gemini_status_text(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 500: return "500 Internal Server Error";
    default: return "500 Internal Server Error";
    }
}

static esp_err_t gemini_send_json_string(httpd_req_t *req, char *payload)
{
    if (!payload) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);
    return err;
}

static esp_err_t gemini_send_error_json(httpd_req_t *req, int status, const char *message)
{
    if (status < 400 || status > 599) {
        status = 500;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddNumberToObject(root, "status", status);
    cJSON_AddStringToObject(root, "error", message ? message : "failed");
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_status(req, gemini_status_text(status));
    return gemini_send_json_string(req, payload);
}

static void gemini_add_action_result(cJSON *root, const char *action, esp_err_t err)
{
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddStringToObject(root, "action", action ? action : "");
    cJSON_AddStringToObject(root, "result", esp_err_to_name(err));
}

static void gemini_add_diag_object(cJSON *root)
{
    char *diag = calloc(1, GEMINI_DIAG_JSON_SIZE);
    if (!diag) {
        cJSON_AddStringToObject(root, "diagnostics_error", "ESP_ERR_NO_MEM");
        return;
    }

    esp_err_t err = cap_gemini_live_get_diagnostics_json(diag, GEMINI_DIAG_JSON_SIZE);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "diagnostics_error", esp_err_to_name(err));
        free(diag);
        return;
    }

    cJSON *diag_root = cJSON_Parse(diag);
    free(diag);
    if (!diag_root) {
        cJSON_AddStringToObject(root, "diagnostics_error", "ESP_ERR_INVALID_RESPONSE");
        return;
    }
    cJSON_AddItemToObject(root, "diagnostics", diag_root);
}

static esp_err_t gemini_live_get_handler(httpd_req_t *req)
{
    char *diag = calloc(1, GEMINI_DIAG_JSON_SIZE);
    if (!diag) {
        return gemini_send_error_json(req, 500, "diagnostics allocation failed");
    }

    esp_err_t err = cap_gemini_live_get_diagnostics_json(diag, GEMINI_DIAG_JSON_SIZE);
    if (err != ESP_OK) {
        free(diag);
        ESP_LOGW(TAG, "diagnostics unavailable: %s", esp_err_to_name(err));
        return gemini_send_error_json(req, 500, "Gemini diagnostics unavailable");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t send_err = httpd_resp_sendstr(req, diag);
    free(diag);
    return send_err;
}

static esp_err_t gemini_live_post_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return gemini_send_error_json(req, 400, "Invalid JSON body");
    }

    cJSON *action_item = cJSON_GetObjectItemCaseSensitive(body, "action");
    const char *action = cJSON_IsString(action_item) ? action_item->valuestring : "";
    char action_copy[24] = {0};
    strlcpy(action_copy, action, sizeof(action_copy));

    esp_err_t err = ESP_OK;
    if (strcmp(action_copy, "start") == 0) {
        err = cap_gemini_live_start();
    } else if (strcmp(action_copy, "stop") == 0) {
        err = cap_gemini_live_stop();
    } else if (strcmp(action_copy, "text") == 0) {
        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(body, "text");
        const char *text = cJSON_IsString(text_item) ? text_item->valuestring : NULL;
        err = text && text[0] ? cap_gemini_live_send_text(text) : ESP_ERR_INVALID_ARG;
    } else if (strcmp(action_copy, "end_input") == 0 ||
               strcmp(action_copy, "commit") == 0 ||
               strcmp(action_copy, "end") == 0) {
        err = cap_gemini_live_end_input();
    } else if (strcmp(action_copy, "status") == 0) {
        err = ESP_OK;
    } else {
        cJSON_Delete(body);
        return gemini_send_error_json(req, 400, "Unsupported Gemini action");
    }
    cJSON_Delete(body);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    gemini_add_action_result(root, action_copy, err);
    gemini_add_diag_object(root);
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_gemini_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/gemini/live", .method = HTTP_GET, .handler = gemini_live_get_handler },
        { .uri = "/api/gemini/live", .method = HTTP_POST, .handler = gemini_live_post_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register Gemini route: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
