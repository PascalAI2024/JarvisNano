/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_http/src/jr_http.c — authenticated v5 cockpit and control boundary.
 */
#include "jr_http/jr_http.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs.h"                 /* ESP_ERR_NVS_NOT_FOUND in config_get */
#include "jr_net/jr_net.h"

#define JR_HTTP_ACTION_QUEUE_DEPTH  8U
#define JR_HTTP_BODY_CAP            2048U
#define JR_HTTP_AUTH_HEADER         "X-JarvisNano-Token"
#define JR_HTTP_SERVER_STACK        8192U
#define JR_HTTP_ACTION_INTERNAL_STOP ((jr_http_action_kind_t)INT_MAX)

extern const unsigned char jr_dashboard_start[]
    asm("_binary_assets_index_html_start");

typedef struct {
    httpd_handle_t server;
    QueueHandle_t actions;
    jr_http_config_t config;
} jr_http_state_t;

static jr_http_state_t s_http;
static portMUX_TYPE s_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_poll_waiters;
typedef enum {
    JR_HTTP_LIFECYCLE_STOPPED = 0,
    JR_HTTP_LIFECYCLE_STARTING,
    JR_HTTP_LIFECYCLE_RUNNING,
    JR_HTTP_LIFECYCLE_STOPPING,
} jr_http_lifecycle_t;
static jr_http_lifecycle_t s_lifecycle;

_Static_assert(JR_HTTP_PAIRING_TOKEN_CAP == JR_CFG_PAIRING_TOKEN_CAP,
               "jr_http/jr_net pairing token capacity drift");

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "200 OK";
    case 202: return "202 Accepted";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 408: return "408 Request Timeout";
    case 413: return "413 Payload Too Large";
    case 422: return "422 Unprocessable Entity";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default:  return "500 Internal Server Error";
    }
}

static void set_api_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    /* Deliberately no Access-Control-Allow-Origin header. The cockpit and API
     * are same-origin; protected writes are never exposed through wildcard
     * CORS on a plaintext LAN service. */
}

static esp_err_t send_json(httpd_req_t *req, int status, const char *json)
{
    httpd_resp_set_status(req, status_text(status));
    set_api_headers(req);
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, int status,
                            const char *code, const char *message)
{
    char body[224];
    int n = snprintf(body, sizeof body,
                     "{\"ok\":false,\"error\":{\"code\":\"%s\","
                     "\"message\":\"%s\"}}", code, message);
    if (n < 0 || (size_t)n >= sizeof body) {
        return send_json(req, 500,
                         "{\"ok\":false,\"error\":{\"code\":"
                         "\"internal_error\",\"message\":\"Request failed\"}}");
    }
    return send_json(req, status, body);
}

static void secure_zero(void *ptr, size_t size)
{
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (size-- > 0U) {
        *p++ = 0U;
    }
}

static void secure_json_strings(cJSON *item)
{
    for (cJSON *it = item; it != NULL; it = it->next) {
        if (it->valuestring != NULL) {
            secure_zero(it->valuestring, strlen(it->valuestring));
        }
        if (it->child != NULL) {
            secure_json_strings(it->child);
        }
    }
}

static bool ascii_contains_folded(const char *value, const char *needle)
{
    if (value == NULL || needle == NULL || *needle == '\0') {
        return false;
    }
    for (const char *start = value; *start != '\0'; ++start) {
        const char *a = start;
        const char *b = needle;
        while (*a != '\0' && *b != '\0') {
            unsigned char ac = (unsigned char)*a;
            unsigned char bc = (unsigned char)*b;
            if (ac >= 'A' && ac <= 'Z') ac = (unsigned char)(ac + ('a' - 'A'));
            if (bc >= 'A' && bc <= 'Z') bc = (unsigned char)(bc + ('a' - 'A'));
            if (ac != bc) break;
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static bool sensitive_json_name(const char *name)
{
    static const char *needles[] = {
        "password", "passphrase", "secret", "token", "authorization",
        "credential", "api_key", "apikey", "mcp_key", "llm_key",
        "gemini_key", "private_key",
    };
    for (size_t i = 0U; i < sizeof needles / sizeof needles[0]; ++i) {
        if (ascii_contains_folded(name, needles[i])) {
            return true;
        }
    }
    return false;
}

static cJSON *masked_json_item(const char *key)
{
    cJSON *holder = cJSON_CreateObject();
    cJSON *replacement = cJSON_CreateString(JR_CFG_SECRET_MASK);
    if (holder == NULL || replacement == NULL ||
        !cJSON_AddItemToObject(holder, key, replacement)) {
        cJSON_Delete(holder);
        cJSON_Delete(replacement);
        return NULL;
    }
    if (cJSON_DetachItemViaPointer(holder, replacement) != replacement) {
        cJSON_Delete(holder); /* also owns replacement when detach failed */
        return NULL;
    }
    cJSON_Delete(holder);
    return replacement; /* owns a duplicated key; exact-pointer replace is safe */
}

/* Telemetry providers are contractually secret-free. This second boundary is
 * deliberate defence in depth: a future diagnostic field named like a secret
 * is masked before the response leaves the device. */
static bool scrub_json_secrets(cJSON *parent)
{
    for (cJSON *item = parent->child; item != NULL;) {
        cJSON *next = item->next;
        if (sensitive_json_name(item->string)) {
            cJSON *replacement = masked_json_item(item->string);
            if (replacement == NULL) {
                return false;
            }
            if (item->valuestring != NULL) {
                secure_zero(item->valuestring, strlen(item->valuestring));
            }
            if (item->child != NULL) {
                secure_json_strings(item->child);
            }
            if (!cJSON_ReplaceItemViaPointer(parent, item, replacement)) {
                cJSON_Delete(replacement);
                return false;
            }
        } else if (item->child != NULL && !scrub_json_secrets(item)) {
            return false;
        }
        item = next;
    }
    return true;
}

static bool bounded_copy(char *dst, size_t capacity, const char *src)
{
    if (dst == NULL || capacity == 0U || src == NULL) {
        return false;
    }
    size_t len = strnlen(src, capacity);
    if (len >= capacity) {
        dst[0] = '\0';
        return false;
    }
    memcpy(dst, src, len + 1U);
    return true;
}

static esp_err_t parse_json_body(httpd_req_t *req, bool allow_empty,
                                 cJSON **out)
{
    *out = NULL;
    if (req->content_len == 0) {
        if (allow_empty) {
            *out = cJSON_CreateObject();
            return *out != NULL ? ESP_OK : ESP_ERR_NO_MEM;
        }
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)req->content_len > JR_HTTP_BODY_CAP) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t length = (size_t)req->content_len;
    char *body = malloc(length + 1U);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t received = 0U;
    while (received < length) {
        int n = httpd_req_recv(req, body + received, length - received);
        if (n <= 0) {
            secure_zero(body, length + 1U);
            free(body);
            return n == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        received += (size_t)n;
    }
    body[length] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(body, length + 1U, NULL, true);
    secure_zero(body, length + 1U);
    free(body);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    *out = root;
    return ESP_OK;
}

static esp_err_t send_body_error(httpd_req_t *req, esp_err_t err,
                                 const char *expected)
{
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_error(req, 413, "body_size", "JSON body exceeds 2048 bytes");
    }
    if (err == ESP_ERR_TIMEOUT) {
        return send_error(req, 408, "body_timeout", "JSON body timed out");
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, 503, "body_unavailable", "Device is busy");
    }
    return send_error(req, 400, "invalid_json", expected);
}

static esp_err_t require_auth(httpd_req_t *req)
{
    size_t length = httpd_req_get_hdr_value_len(req, JR_HTTP_AUTH_HEADER);
    if (length == 0U) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "JarvisNano");
        send_error(req, 401, "missing_token", "Pairing token required");
        return ESP_ERR_NOT_ALLOWED;
    }
    if (length >= JR_CFG_PAIRING_TOKEN_CAP) {
        send_error(req, 403, "invalid_token", "Pairing token rejected");
        return ESP_ERR_NOT_ALLOWED;
    }

    char token[JR_CFG_PAIRING_TOKEN_CAP] = {0};
    esp_err_t err = httpd_req_get_hdr_value_str(req, JR_HTTP_AUTH_HEADER,
                                                 token, sizeof token);
    bool matches = false;
    if (err == ESP_OK) {
        err = jr_net_pairing_token_verify(token, &matches);
    }
    secure_zero(token, sizeof token);
    if (err != ESP_OK) {
        send_error(req, 503, "auth_unavailable",
                   "Pairing has not been initialised");
        return err;
    }
    if (!matches) {
        send_error(req, 403, "invalid_token", "Pairing token rejected");
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

static bool action_space_available(void)
{
    return s_http.actions != NULL && uxQueueSpacesAvailable(s_http.actions) > 0U;
}

static esp_err_t enqueue_action(const jr_http_action_t *action)
{
    if (s_http.actions == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_http.actions, action, 0) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t dashboard_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'self'; style-src 'self' 'unsafe-inline'; "
        "script-src 'self' 'unsafe-inline'; connect-src 'self'; "
        "img-src 'none'; object-src 'none'; base-uri 'none'; "
        "form-action 'none'; frame-ancestors 'none'");
    return httpd_resp_sendstr(req, (const char *)jr_dashboard_start);
}

static esp_err_t health_get(httpd_req_t *req)
{
    char body[256];
    jr_net_status_t net = {0};
    bool connected = jr_net_get_status(&net) == ESP_OK && net.sta_connected;
    uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    int n = snprintf(body, sizeof body,
        "{\"ok\":true,\"service\":\"jarvisnano\",\"version\":\"v5\","
        "\"uptime_ms\":%llu,\"wifi_connected\":%s}",
        (unsigned long long)uptime_ms, connected ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof body) {
        return send_error(req, 500, "encoding_failed", "Health unavailable");
    }
    return send_json(req, 200, body);
}

static const char *net_mode_name(jr_net_mode_t mode)
{
    switch (mode) {
    case JR_NET_MODE_OFF:   return "off";
    case JR_NET_MODE_STA:   return "sta";
    case JR_NET_MODE_AP:    return "ap";
    case JR_NET_MODE_APSTA: return "apsta";
    default:                return "unknown";
    }
}

static cJSON *read_telemetry_object(jr_http_telemetry_kind_t kind)
{
    if (s_http.config.telemetry == NULL) {
        return NULL;
    }
    char *json = malloc(JR_HTTP_TELEMETRY_JSON_CAP);
    if (json == NULL) {
        return NULL;
    }
    json[0] = '\0';
    size_t written = 0U;
    esp_err_t err = s_http.config.telemetry(s_http.config.telemetry_ctx,
                                             kind, json,
                                             JR_HTTP_TELEMETRY_JSON_CAP,
                                             &written);
    if (err != ESP_OK) {
        secure_zero(json, JR_HTTP_TELEMETRY_JSON_CAP);
        free(json);
        return NULL;
    }
    if (written == 0U) {
        written = strnlen(json, JR_HTTP_TELEMETRY_JSON_CAP);
    }
    if (written == 0U || written >= JR_HTTP_TELEMETRY_JSON_CAP) {
        secure_zero(json, JR_HTTP_TELEMETRY_JSON_CAP);
        free(json);
        return NULL;
    }
    json[written] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(json, written + 1U, NULL, true);
    secure_zero(json, JR_HTTP_TELEMETRY_JSON_CAP);
    free(json);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    if (!scrub_json_secrets(root)) {
        secure_json_strings(root);
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static esp_err_t send_cjson(httpd_req_t *req, int status, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        return send_error(req, 500, "encoding_failed", "Response unavailable");
    }
    esp_err_t err = send_json(req, status, json);
    cJSON_free(json);
    return err;
}

static esp_err_t status_get(httpd_req_t *req)
{
    jr_net_status_t net = {0};
    esp_err_t net_err = jr_net_get_status(&net);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_error(req, 500, "encoding_failed", "Status unavailable");
    }
    bool built = cJSON_AddBoolToObject(root, "ok", true) != NULL &&
                 cJSON_AddBoolToObject(root, "network_available",
                                       net_err == ESP_OK) != NULL;
    if (net_err == ESP_OK) {
        built = built &&
            cJSON_AddStringToObject(root, "wifi_mode", net_mode_name(net.mode)) != NULL &&
            cJSON_AddBoolToObject(root, "wifi_started", net.wifi_started) != NULL &&
            cJSON_AddBoolToObject(root, "wifi_connected", net.sta_connected) != NULL &&
            cJSON_AddBoolToObject(root, "provisioning_active",
                                  net.provisioning_active) != NULL &&
            cJSON_AddStringToObject(root, "ip", net.sta_ip) != NULL &&
            cJSON_AddStringToObject(root, "ap_ip", net.ap_ip) != NULL &&
            cJSON_AddStringToObject(root, "ap_ssid", net.ap_ssid) != NULL &&
            cJSON_AddNumberToObject(root, "rssi", net.rssi) != NULL &&
            cJSON_AddNumberToObject(root, "channel", net.channel) != NULL &&
            cJSON_AddNumberToObject(root, "retry_count", net.retry_count) != NULL &&
            cJSON_AddNumberToObject(root, "last_disconnect_reason",
                                    net.last_disconnect_reason) != NULL;
    }
    cJSON *runtime = read_telemetry_object(JR_HTTP_TELEMETRY_STATUS);
    built = built && cJSON_AddBoolToObject(root, "runtime_available",
                                           runtime != NULL) != NULL;
    if (runtime != NULL) {
        if (!cJSON_AddItemToObject(root, "runtime", runtime)) {
            cJSON_Delete(runtime);
            built = false;
        }
    }
    if (!built) {
        cJSON_Delete(root);
        return send_error(req, 500, "encoding_failed", "Status unavailable");
    }
    esp_err_t err = send_cjson(req, 200, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t telemetry_get(httpd_req_t *req,
                               jr_http_telemetry_kind_t kind)
{
    cJSON *root = read_telemetry_object(kind);
    if (root == NULL) {
        return send_error(req, 503, "telemetry_unavailable",
                          "Telemetry is not ready");
    }
    esp_err_t err = send_cjson(req, 200, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t gemini_get(httpd_req_t *req)
{
    return telemetry_get(req, JR_HTTP_TELEMETRY_GEMINI_LIVE);
}

static esp_err_t display_get(httpd_req_t *req)
{
    return telemetry_get(req, JR_HTTP_TELEMETRY_DISPLAY);
}

static esp_err_t touch_get(httpd_req_t *req)
{
    return telemetry_get(req, JR_HTTP_TELEMETRY_TOUCH);
}

static esp_err_t tools_get(httpd_req_t *req)
{
    return telemetry_get(req, JR_HTTP_TELEMETRY_TOOLS);
}

static esp_err_t config_get(httpd_req_t *req)
{
    jr_net_config_t config = {0};
    esp_err_t load_err = jr_cfg_load(&config, JR_CFG_VIEW_MASKED);
    if (load_err != ESP_OK && load_err != ESP_ERR_NVS_NOT_FOUND) {
        secure_zero(&config, sizeof config);
        return send_error(req, 503, "config_unavailable",
                          "Configuration is not ready");
    }

    cJSON *root = cJSON_CreateObject();
    bool built = root != NULL &&
        cJSON_AddBoolToObject(root, "ok", true) != NULL &&
        cJSON_AddStringToObject(root, "agent_name", config.agent_name) != NULL &&
        cJSON_AddStringToObject(root, "wifi_ssid", config.wifi_ssid) != NULL &&
        cJSON_AddStringToObject(root, "wifi_password",
            config.wifi_password[0] != '\0' ? JR_CFG_SECRET_MASK : "") != NULL &&
        cJSON_AddStringToObject(root, "llm_api_key",
            config.llm_api_key[0] != '\0' ? JR_CFG_SECRET_MASK : "") != NULL &&
        cJSON_AddStringToObject(root, "jarvis_mcp_url", config.jarvis_mcp_url) != NULL &&
        cJSON_AddStringToObject(root, "jarvis_mcp_key",
            config.jarvis_mcp_key[0] != '\0' ? JR_CFG_SECRET_MASK : "") != NULL &&
        cJSON_AddStringToObject(root, "pairing_token",
            config.pairing_token[0] != '\0' ? JR_CFG_SECRET_MASK : "") != NULL;
    secure_zero(&config, sizeof config);
    if (!built) {
        cJSON_Delete(root);
        return send_error(req, 500, "encoding_failed", "Config unavailable");
    }
    esp_err_t err = send_cjson(req, 200, root);
    cJSON_Delete(root);
    return err;
}

static bool config_slot(jr_net_config_t *config, jr_cfg_field_t field,
                        char **slot, size_t *capacity)
{
    switch (field) {
    case JR_CFG_AGENT_NAME:
        *slot = config->agent_name; *capacity = sizeof config->agent_name; return true;
    case JR_CFG_WIFI_SSID:
        *slot = config->wifi_ssid; *capacity = sizeof config->wifi_ssid; return true;
    case JR_CFG_WIFI_PASSWORD:
        *slot = config->wifi_password; *capacity = sizeof config->wifi_password; return true;
    case JR_CFG_LLM_API_KEY:
        *slot = config->llm_api_key; *capacity = sizeof config->llm_api_key; return true;
    case JR_CFG_JARVIS_MCP_URL:
        *slot = config->jarvis_mcp_url; *capacity = sizeof config->jarvis_mcp_url; return true;
    case JR_CFG_JARVIS_MCP_KEY:
        *slot = config->jarvis_mcp_key; *capacity = sizeof config->jarvis_mcp_key; return true;
    default:
        return false;
    }
}

static bool field_from_name(const char *name, jr_cfg_field_t *field)
{
    for (int i = JR_CFG_AGENT_NAME; i < JR_CFG_PAIRING_TOKEN; ++i) {
        if (strcmp(name, jr_cfg_field_name((jr_cfg_field_t)i)) == 0) {
            *field = (jr_cfg_field_t)i;
            return true;
        }
    }
    return false;
}

static esp_err_t parse_config_patch(cJSON *root, jr_net_config_t *config,
                                    uint32_t *field_mask)
{
    *field_mask = 0U;
    uint32_t seen_mask = 0U;
    for (cJSON *item = root->child; item != NULL; item = item->next) {
        jr_cfg_field_t field;
        if (item->string == NULL || !field_from_name(item->string, &field)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        uint32_t bit = 1U << field;
        if ((seen_mask & bit) != 0U) {
            return ESP_ERR_INVALID_ARG;
        }
        seen_mask |= bit;
        const char *value = NULL;
        if (cJSON_IsNull(item)) {
            value = ""; /* Explicit null clears; omission preserves. */
        } else if (cJSON_IsString(item) && item->valuestring != NULL) {
            value = item->valuestring;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
        if (jr_cfg_field_is_secret(field) &&
            strcmp(value, JR_CFG_SECRET_MASK) == 0) {
            continue; /* Round-tripping a masked read never overwrites a secret. */
        }
        char *slot = NULL;
        size_t capacity = 0U;
        if (!config_slot(config, field, &slot, &capacity) ||
            !bounded_copy(slot, capacity, value)) {
            return ESP_ERR_INVALID_SIZE;
        }
        esp_err_t err = jr_cfg_validate(field, slot);
        if (err != ESP_OK) {
            return err;
        }
        *field_mask |= bit;
    }
    return *field_mask != 0U ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *root = NULL;
    esp_err_t err = parse_json_body(req, false, &root);
    if (err != ESP_OK) {
        return send_body_error(req, err, "Expected a JSON object");
    }

    jr_net_config_t config = {0};
    uint32_t field_mask = 0U;
    err = parse_config_patch(root, &config, &field_mask);
    secure_json_strings(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        secure_zero(&config, sizeof config);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            return send_error(req, 400, "unknown_field",
                              "Config field is unknown or immutable");
        }
        if (err == ESP_ERR_INVALID_SIZE) {
            return send_error(req, 422, "empty_or_oversize_patch",
                              "Config patch is empty or too large");
        }
        return send_error(req, 422, "invalid_config",
                          "One or more config values are invalid");
    }
    if (!action_space_available()) {
        secure_zero(&config, sizeof config);
        return send_error(req, 503, "action_queue_full", "Device is busy");
    }

    err = jr_cfg_apply(&config, field_mask);
    secure_zero(&config, sizeof config);
    if (err != ESP_OK) {
        return send_error(req, 500, "config_write_failed",
                          "Configuration was not saved");
    }
    jr_http_action_t action = {
        .kind = JR_HTTP_ACTION_CONFIG_UPDATED,
        .data.config.field_mask = field_mask,
    };
    err = enqueue_action(&action);
    if (err != ESP_OK) {
        /* Persistence succeeded. Be exact about the rare notification failure
         * instead of falsely claiming the config write was rolled back. */
        return send_json(req, 202,
            "{\"ok\":true,\"saved\":true,\"queued\":false,"
            "\"restart_required\":true}");
    }
    return send_json(req, 200,
        "{\"ok\":true,\"saved\":true,\"queued\":true,"
        "\"runtime_reload_queued\":true}");
}

static esp_err_t say_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *root = NULL;
    esp_err_t err = parse_json_body(req, false, &root);
    if (err != ESP_OK) {
        return send_body_error(req, err, "Expected a JSON object with text");
    }
    cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    bool shape_ok = root->child == text && text != NULL && text->next == NULL &&
                    cJSON_IsString(text) && text->valuestring != NULL &&
                    text->valuestring[0] != '\0';
    jr_http_action_t action = {.kind = JR_HTTP_ACTION_GEMINI_SAY};
    bool copied = shape_ok && bounded_copy(action.data.say.text,
                                           sizeof action.data.say.text,
                                           text->valuestring);
    secure_json_strings(root);
    cJSON_Delete(root);
    if (!copied) {
        secure_zero(&action, sizeof action);
        return send_error(req, 422, "invalid_text",
                          "Text must be 1 to 199 bytes");
    }
    err = enqueue_action(&action);
    secure_zero(&action, sizeof action);
    if (err != ESP_OK) {
        return send_error(req, 503, "action_queue_full", "Device is busy");
    }
    return send_json(req, 202, "{\"ok\":true,\"queued\":true}");
}

static bool json_int_in_range(cJSON *item, int minimum, int maximum, int *out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < (double)minimum ||
        item->valuedouble > (double)maximum ||
        item->valuedouble != (double)item->valueint) {
        return false;
    }
    *out = item->valueint;
    return true;
}

static esp_err_t gain_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *root = NULL;
    esp_err_t err = parse_json_body(req, false, &root);
    if (err != ESP_OK) {
        return send_body_error(req, err, "Expected a JSON gain object");
    }
    jr_http_action_t action = {.kind = JR_HTTP_ACTION_AUDIO_GAIN};
    bool valid = true;
    for (cJSON *item = root->child; item != NULL && valid; item = item->next) {
        int value = 0;
        if (item->string != NULL && strcmp(item->string, "mic") == 0 &&
            (action.data.gain.field_mask & JR_HTTP_GAIN_MIC) == 0U &&
            json_int_in_range(item, 0, 37, &value)) {
            action.data.gain.field_mask |= JR_HTTP_GAIN_MIC;
            action.data.gain.mic_db = (int16_t)value;
        } else if (item->string != NULL && strcmp(item->string, "reference") == 0 &&
                   (action.data.gain.field_mask & JR_HTTP_GAIN_REFERENCE) == 0U &&
                   json_int_in_range(item, 0, 37, &value)) {
            action.data.gain.field_mask |= JR_HTTP_GAIN_REFERENCE;
            action.data.gain.reference_db = (int16_t)value;
        } else if (item->string != NULL && strcmp(item->string, "volume") == 0 &&
                   (action.data.gain.field_mask & JR_HTTP_GAIN_VOLUME) == 0U &&
                   json_int_in_range(item, 0, 100, &value)) {
            action.data.gain.field_mask |= JR_HTTP_GAIN_VOLUME;
            action.data.gain.volume = (int16_t)value;
        } else if (item->string != NULL && strcmp(item->string, "barge_enabled") == 0 &&
                   (action.data.gain.field_mask & JR_HTTP_GAIN_BARGE) == 0U &&
                   cJSON_IsBool(item)) {
            action.data.gain.field_mask |= JR_HTTP_GAIN_BARGE;
            action.data.gain.barge_enabled = cJSON_IsTrue(item);
        } else {
            valid = false;
        }
    }
    if (action.data.gain.field_mask == 0U) {
        valid = false;
    }
    secure_json_strings(root);
    cJSON_Delete(root);
    if (!valid) {
        secure_zero(&action, sizeof action);
        return send_error(req, 422, "invalid_gain",
                          "Use mic/reference 0-37, volume 0-100, or barge_enabled");
    }
    err = enqueue_action(&action);
    secure_zero(&action, sizeof action);
    if (err != ESP_OK) {
        return send_error(req, 503, "action_queue_full", "Device is busy");
    }
    return send_json(req, 202, "{\"ok\":true,\"queued\":true}");
}

static esp_err_t restart_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *root = NULL;
    esp_err_t err = parse_json_body(req, true, &root);
    if (err != ESP_OK) {
        return send_body_error(req, err, "Expected an empty JSON object");
    }
    bool empty = root->child == NULL;
    secure_json_strings(root);
    cJSON_Delete(root);
    if (!empty) {
        return send_error(req, 400, "unexpected_field",
                          "Restart body must be empty");
    }
    jr_http_action_t action = {.kind = JR_HTTP_ACTION_RESTART};
    err = enqueue_action(&action);
    if (err != ESP_OK) {
        return send_error(req, 503, "action_queue_full", "Device is busy");
    }
    return send_json(req, 202, "{\"ok\":true,\"queued\":true}");
}

static esp_err_t register_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/",                 .method = HTTP_GET,  .handler = dashboard_get},
        {.uri = "/api/health",       .method = HTTP_GET,  .handler = health_get},
        {.uri = "/api/status",       .method = HTTP_GET,  .handler = status_get},
        {.uri = "/api/config",       .method = HTTP_GET,  .handler = config_get},
        {.uri = "/api/gemini/live",  .method = HTTP_GET,  .handler = gemini_get},
        {.uri = "/api/display",      .method = HTTP_GET,  .handler = display_get},
        {.uri = "/api/touch",        .method = HTTP_GET,  .handler = touch_get},
        {.uri = "/api/tools/status", .method = HTTP_GET,  .handler = tools_get},
        {.uri = "/api/config",       .method = HTTP_POST, .handler = config_post},
        {.uri = "/api/restart",      .method = HTTP_POST, .handler = restart_post},
        {.uri = "/api/gemini/say",   .method = HTTP_POST, .handler = say_post},
        {.uri = "/api/audio/gain",   .method = HTTP_POST, .handler = gain_post},
    };
    for (size_t i = 0U; i < sizeof routes / sizeof routes[0]; ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t jr_http_start(const jr_http_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (;;) {
        portENTER_CRITICAL(&s_lifecycle_lock);
        if (s_lifecycle == JR_HTTP_LIFECYCLE_RUNNING) {
            portEXIT_CRITICAL(&s_lifecycle_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (s_lifecycle == JR_HTTP_LIFECYCLE_STOPPED) {
            s_lifecycle = JR_HTTP_LIFECYCLE_STARTING;
            s_poll_waiters = 0U;
            portEXIT_CRITICAL(&s_lifecycle_lock);
            break;
        }
        portEXIT_CRITICAL(&s_lifecycle_lock);
        vTaskDelay(1); /* Serialize against an in-flight start/stop. */
    }

    memset(&s_http, 0, sizeof s_http);
    s_http.config = *config;
    s_http.actions = xQueueCreate(JR_HTTP_ACTION_QUEUE_DEPTH,
                                  sizeof(jr_http_action_t));
    if (s_http.actions == NULL) {
        memset(&s_http, 0, sizeof s_http);
        portENTER_CRITICAL(&s_lifecycle_lock);
        s_lifecycle = JR_HTTP_LIFECYCLE_STOPPED;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.server_port = config->port == 0U
                                    ? JR_HTTP_DEFAULT_PORT : config->port;
    server_config.max_uri_handlers = 12U;
    server_config.stack_size = JR_HTTP_SERVER_STACK;
    server_config.lru_purge_enable = true;
    server_config.recv_wait_timeout = 5;
    server_config.send_wait_timeout = 5;
    esp_err_t err = httpd_start(&s_http.server, &server_config);
    if (err == ESP_OK) {
        err = register_routes(s_http.server);
    }
    if (err != ESP_OK) {
        if (s_http.server != NULL) {
            httpd_stop(s_http.server);
        }
        vQueueDelete(s_http.actions);
        memset(&s_http, 0, sizeof s_http);
        portENTER_CRITICAL(&s_lifecycle_lock);
        s_lifecycle = JR_HTTP_LIFECYCLE_STOPPED;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return err;
    }
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_lifecycle = JR_HTTP_LIFECYCLE_RUNNING;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return ESP_OK;
}

void jr_http_stop(void)
{
    httpd_handle_t server = NULL;
    QueueHandle_t actions = NULL;
    for (;;) {
        portENTER_CRITICAL(&s_lifecycle_lock);
        if (s_lifecycle == JR_HTTP_LIFECYCLE_STOPPED) {
            portEXIT_CRITICAL(&s_lifecycle_lock);
            return;
        }
        if (s_lifecycle == JR_HTTP_LIFECYCLE_RUNNING) {
            s_lifecycle = JR_HTTP_LIFECYCLE_STOPPING;
            server = s_http.server;
            actions = s_http.actions;
            portEXIT_CRITICAL(&s_lifecycle_lock);
            break;
        }
        portEXIT_CRITICAL(&s_lifecycle_lock);
        vTaskDelay(1); /* Wait for the other lifecycle transition to finish. */
    }

    /* Stop every producer before draining/waking the sole action consumer. */
    if (server != NULL) {
        (void)httpd_stop(server);
    }
    if (actions != NULL) {
        jr_http_action_t stop = {.kind = JR_HTTP_ACTION_INTERNAL_STOP};
        (void)xQueueSendToFront(actions, &stop, 0);
        for (;;) {
            portENTER_CRITICAL(&s_lifecycle_lock);
            bool idle = s_poll_waiters == 0U;
            portEXIT_CRITICAL(&s_lifecycle_lock);
            if (idle) {
                break;
            }
            vTaskDelay(1);
        }
    }

    portENTER_CRITICAL(&s_lifecycle_lock);
    memset(&s_http, 0, sizeof s_http);
    portEXIT_CRITICAL(&s_lifecycle_lock);
    if (actions != NULL) {
        vQueueDelete(actions);
    }
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_lifecycle = JR_HTTP_LIFECYCLE_STOPPED;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}

bool jr_http_is_running(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    bool running = s_lifecycle == JR_HTTP_LIFECYCLE_RUNNING;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return running;
}

esp_err_t jr_http_action_poll(jr_http_action_t *out, uint32_t timeout_ms)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lifecycle_lock);
    if (s_lifecycle != JR_HTTP_LIFECYCLE_RUNNING ||
        s_http.actions == NULL || s_poll_waiters != 0U) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    QueueHandle_t actions = s_http.actions;
    s_poll_waiters = 1U;
    portEXIT_CRITICAL(&s_lifecycle_lock);

    TickType_t ticks = timeout_ms == UINT32_MAX
                           ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    bool received = xQueueReceive(actions, out, ticks) == pdTRUE;
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_poll_waiters = 0U;
    bool stopped = s_lifecycle != JR_HTTP_LIFECYCLE_RUNNING;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    if (stopped || (received && out->kind == JR_HTTP_ACTION_INTERNAL_STOP)) {
        memset(out, 0, sizeof *out);
        return ESP_ERR_INVALID_STATE;
    }
    return received ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t jr_http_pairing_token_ensure(
    char token[JR_HTTP_PAIRING_TOKEN_CAP], size_t capacity, bool *created)
{
    return jr_net_pairing_token_ensure(token, capacity, created);
}
