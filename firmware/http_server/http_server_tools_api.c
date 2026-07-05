/*
 * SPDX-FileCopyrightText: 2026 JarvisRobot
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <string.h>

static bool configured_value(const char *value)
{
    return value && value[0] != '\0';
}

static esp_err_t tools_status_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    app_config_t config = {0};
    esp_err_t err = ctx->services.load_config(&config);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load config");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "gemini_configured", configured_value(config.llm_api_key));
    cJSON_AddBoolToObject(root, "jarvis_mcp_configured",
                          configured_value(config.jarvis_mcp_url) &&
                          configured_value(config.jarvis_mcp_key));
    cJSON_AddBoolToObject(root, "jarvis_mcp_url_configured",
                          configured_value(config.jarvis_mcp_url));
    cJSON_AddBoolToObject(root, "jarvis_mcp_key_configured",
                          configured_value(config.jarvis_mcp_key));
    cJSON_AddBoolToObject(root, "pairing_token_configured",
                          configured_value(config.pairing_token));
    http_server_json_add_string(root, "firmware_mcp", "disabled");
    http_server_json_add_string(root, "tool_bridge", "gemini_live_jarvismcp");
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_tools_routes(httpd_handle_t server)
{
    const httpd_uri_t handler = {
        .uri = "/api/tools/status",
        .method = HTTP_GET,
        .handler = tools_status_handler,
    };
    return httpd_register_uri_handler(server, &handler);
}
