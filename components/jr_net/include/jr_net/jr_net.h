/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_net — device networking and typed configuration boundary.
 *
 * ESP-IDF Wi-Fi/NVS types stop here.  Higher layers receive bounded strings,
 * explicit status, and operations that cannot wait forever.  Runtime secrets
 * live only in NVS namespace "app"; none are compiled into the image.
 */
#ifndef JR_NET_JR_NET_H
#define JR_NET_JR_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JR_CFG_WIFI_SSID_CAP       33U
#define JR_CFG_WIFI_PASSWORD_CAP   64U
#define JR_CFG_AGENT_NAME_CAP      64U
#define JR_CFG_LLM_API_KEY_CAP     320U
#define JR_CFG_MCP_URL_CAP         257U
#define JR_CFG_MCP_KEY_CAP         320U
#define JR_CFG_PAIRING_TOKEN_CAP   65U

#define JR_NET_IPV4_CAP            16U
#define JR_NET_AP_PASSWORD_CAP     17U
#define JR_NET_SCAN_MAX_RESULTS    32U
#define JR_NET_DEFAULT_TIMEOUT_MS  30000U

/* A value used by public/readback APIs in place of any configured secret. */
#define JR_CFG_SECRET_MASK         "<configured>"

typedef enum {
    JR_CFG_AGENT_NAME = 0,
    JR_CFG_WIFI_SSID,
    JR_CFG_WIFI_PASSWORD,
    JR_CFG_LLM_API_KEY,
    JR_CFG_JARVIS_MCP_URL,
    JR_CFG_JARVIS_MCP_KEY,
    JR_CFG_PAIRING_TOKEN,
    JR_CFG_FIELD_COUNT,
} jr_cfg_field_t;

enum {
    JR_CFG_F_AGENT_NAME      = 1U << JR_CFG_AGENT_NAME,
    JR_CFG_F_WIFI_SSID       = 1U << JR_CFG_WIFI_SSID,
    JR_CFG_F_WIFI_PASSWORD   = 1U << JR_CFG_WIFI_PASSWORD,
    JR_CFG_F_LLM_API_KEY     = 1U << JR_CFG_LLM_API_KEY,
    JR_CFG_F_JARVIS_MCP_URL  = 1U << JR_CFG_JARVIS_MCP_URL,
    JR_CFG_F_JARVIS_MCP_KEY  = 1U << JR_CFG_JARVIS_MCP_KEY,
    JR_CFG_F_PAIRING_TOKEN   = 1U << JR_CFG_PAIRING_TOKEN,
    JR_CFG_F_ALL             = (1U << JR_CFG_FIELD_COUNT) - 1U,
};

typedef enum {
    JR_CFG_VIEW_INTERNAL = 0, /* Returns secrets. Device-internal callers only. */
    JR_CFG_VIEW_MASKED,       /* Secret fields become JR_CFG_SECRET_MASK. */
} jr_cfg_view_t;

typedef struct {
    char agent_name[JR_CFG_AGENT_NAME_CAP];
    char wifi_ssid[JR_CFG_WIFI_SSID_CAP];
    char wifi_password[JR_CFG_WIFI_PASSWORD_CAP];
    char llm_api_key[JR_CFG_LLM_API_KEY_CAP];
    char jarvis_mcp_url[JR_CFG_MCP_URL_CAP];
    char jarvis_mcp_key[JR_CFG_MCP_KEY_CAP];
    char pairing_token[JR_CFG_PAIRING_TOKEN_CAP];
} jr_net_config_t;

/* Typed NVS access. Empty strings erase a field. jr_cfg_apply() validates all
 * selected fields before committing once, so HTTP can apply a partial patch
 * without accidentally blanking secrets omitted by the client. */
const char *jr_cfg_field_name(jr_cfg_field_t field);
bool jr_cfg_field_is_secret(jr_cfg_field_t field);
esp_err_t jr_cfg_validate(jr_cfg_field_t field, const char *value);
esp_err_t jr_cfg_get(jr_cfg_field_t field, char *buf, size_t cap,
                     jr_cfg_view_t view);
esp_err_t jr_cfg_set(jr_cfg_field_t field, const char *value);
esp_err_t jr_cfg_load(jr_net_config_t *out, jr_cfg_view_t view);
esp_err_t jr_cfg_apply(const jr_net_config_t *config, uint32_t field_mask);

/* Compatibility accessor for existing v5 callers. This may return a secret and
 * must never be wired directly to an HTTP response. Prefer the typed API. */
esp_err_t jr_cfg_get_str(const char *key, char *buf, size_t cap);

/* Creates a 256-bit token when absent, returns it once for a physical onboarding
 * display, and persists only its SHA-256 hash. Existing hashed tokens cannot be
 * recovered; rotate one with jr_cfg_set() if the user loses it. Never log it. */
esp_err_t jr_net_pairing_token_ensure(char *token, size_t cap, bool *created);
esp_err_t jr_net_pairing_token_verify(const char *candidate, bool *matches);

typedef enum {
    JR_NET_MODE_OFF = 0,
    JR_NET_MODE_STA,
    JR_NET_MODE_AP,
    JR_NET_MODE_APSTA,
} jr_net_mode_t;

typedef struct {
    jr_net_mode_t mode;
    bool wifi_started;
    bool sta_connected;
    bool provisioning_active;
    char sta_ip[JR_NET_IPV4_CAP];
    char ap_ip[JR_NET_IPV4_CAP];
    char ap_ssid[JR_CFG_WIFI_SSID_CAP];
    int8_t rssi;
    uint8_t channel;
    uint8_t retry_count;
    uint16_t last_disconnect_reason;
    esp_err_t last_error;
} jr_net_status_t;

typedef struct {
    char ssid[JR_CFG_WIFI_SSID_CAP];
    char password[JR_NET_AP_PASSWORD_CAP];
    char ip[JR_NET_IPV4_CAP];
} jr_net_provisioning_info_t;

typedef enum {
    JR_NET_AUTH_OPEN = 0,
    JR_NET_AUTH_WEP,
    JR_NET_AUTH_WPA_PSK,
    JR_NET_AUTH_WPA2_PSK,
    JR_NET_AUTH_WPA_WPA2_PSK,
    JR_NET_AUTH_WPA3_PSK,
    JR_NET_AUTH_WPA2_WPA3_PSK,
    JR_NET_AUTH_OTHER,
} jr_net_auth_t;

typedef struct {
    char ssid[JR_CFG_WIFI_SSID_CAP];
    int8_t rssi;
    uint8_t channel;
    jr_net_auth_t auth;
} jr_net_ap_record_t;

/* Initializes esp_netif/event loop/Wi-Fi without starting an unbounded join.
 * NVS must already be initialized. Idempotent. */
esp_err_t jr_net_init(void);

/* Composable station join. Start returns immediately; wait has an explicit
 * deadline. The bounded convenience call starts the protected provisioning AP
 * when credentials are absent or the join fails. */
esp_err_t jr_net_wifi_connect_start(void);
esp_err_t jr_net_wifi_wait_connected(uint32_t timeout_ms);
esp_err_t jr_net_wifi_connect_bounded(uint32_t timeout_ms);

/* Legacy v5 entry point: bounded to JR_NET_DEFAULT_TIMEOUT_MS and now falls
 * back to APSTA provisioning instead of leaving a blank device unreachable. */
esp_err_t jr_net_wifi_connect(void);

bool jr_net_is_connected(void);
esp_err_t jr_net_get_status(jr_net_status_t *out);

/* Toggle station modem sleep without stopping Wi-Fi or changing credentials.
 * The requested policy survives reconnects; provisioning always forces full
 * radio availability. Use false for realtime voice, OTA, and operator mode. */
esp_err_t jr_net_set_power_save(bool enabled);

/* Provisioning uses a device-unique WPA2 AP and an independent random password
 * persisted in NVS. The password is only returned through the explicit local
 * display API below; status/readback never includes it. */
esp_err_t jr_net_provisioning_start(void);
esp_err_t jr_net_provisioning_stop(void);
esp_err_t jr_net_provisioning_info(jr_net_provisioning_info_t *out);

/* Active scan with a hard deadline. count receives the number copied (at most
 * capacity and JR_NET_SCAN_MAX_RESULTS). */
esp_err_t jr_net_wifi_scan(jr_net_ap_record_t *records, size_t capacity,
                           size_t *count, uint32_t timeout_ms);

/* ESP-IDF 5.5 no longer bundles the mdns component. This API returns
 * ESP_ERR_NOT_SUPPORTED until espressif/mdns is added to the project, keeping
 * the dependency decision explicit instead of making .local silently flaky. */
esp_err_t jr_net_mdns_start(const char *hostname);
void jr_net_mdns_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* JR_NET_JR_NET_H */
