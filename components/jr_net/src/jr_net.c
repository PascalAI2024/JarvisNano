/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_net/src/jr_net.c — bounded Wi-Fi + typed NVS configuration adapter.
 */
#include "jr_net/jr_net.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char *TAG = "jr_net";

#define JR_NET_NS                 "app"
#define JR_NET_CONNECTED_BIT      BIT0
#define JR_NET_FAIL_BIT           BIT1
#define JR_NET_SCAN_DONE_BIT      BIT2
#define JR_NET_MAX_RETRY          8U
#define JR_NET_LOCK_TIMEOUT_MS    2000U
#define JR_NET_AP_PASSWORD_LEN    (JR_NET_AP_PASSWORD_CAP - 1U)
#define JR_PAIR_TOKEN_BYTES       32U
#define JR_PAIR_HASH_KEY          "pair_token_h"
#define JR_PROVISION_PASS_KEY     "prov_ap_pass"
#define JR_LEGACY_GEMINI_KEY      "gemini_api_key"

typedef struct {
    const char *key;
    size_t capacity;
    bool secret;
} cfg_field_desc_t;

static const cfg_field_desc_t s_cfg_fields[JR_CFG_FIELD_COUNT] = {
    [JR_CFG_AGENT_NAME]     = {"agent_name",     JR_CFG_AGENT_NAME_CAP, false},
    [JR_CFG_WIFI_SSID]      = {"wifi_ssid",      JR_CFG_WIFI_SSID_CAP, false},
    [JR_CFG_WIFI_PASSWORD]  = {"wifi_password",  JR_CFG_WIFI_PASSWORD_CAP, true},
    [JR_CFG_LLM_API_KEY]    = {"llm_api_key",    JR_CFG_LLM_API_KEY_CAP, true},
    [JR_CFG_JARVIS_MCP_URL] = {"jarvis_mcp_url", JR_CFG_MCP_URL_CAP, false},
    [JR_CFG_JARVIS_MCP_KEY] = {"jarvis_mcp_key", JR_CFG_MCP_KEY_CAP, true},
    /* Pairing tokens are write-only and stored as SHA-256, not plaintext. */
    [JR_CFG_PAIRING_TOKEN]  = {"pairing_token",  JR_CFG_PAIRING_TOKEN_CAP, true},
};

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_wifi_lock;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;

static volatile bool s_inited;
static volatile bool s_wifi_started;
static volatile bool s_connected;
static volatile bool s_connect_requested;
static volatile bool s_provisioning;
static volatile uint8_t s_retry;
static volatile uint16_t s_last_disconnect_reason;
static volatile esp_err_t s_last_error;
static volatile wifi_ps_type_t s_requested_ps = WIFI_PS_NONE;
static volatile wifi_ps_type_t s_applied_ps = WIFI_PS_NONE;
static char s_ap_ssid[JR_CFG_WIFI_SSID_CAP];

static bool field_valid(jr_cfg_field_t field)
{
    return field >= JR_CFG_AGENT_NAME && field < JR_CFG_FIELD_COUNT;
}

static void secure_zero(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len-- > 0U) {
        *p++ = 0U;
    }
}

static esp_err_t copy_string(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0U || src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = strnlen(src, cap);
    if (len >= cap) {
        dst[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, src, len + 1U);
    return ESP_OK;
}

static bool has_control_or_space(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        if (iscntrl(*p) || isspace(*p)) {
            return true;
        }
    }
    return false;
}

static bool has_control(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        if (iscntrl(*p)) {
            return true;
        }
    }
    return false;
}

const char *jr_cfg_field_name(jr_cfg_field_t field)
{
    return field_valid(field) ? s_cfg_fields[field].key : NULL;
}

bool jr_cfg_field_is_secret(jr_cfg_field_t field)
{
    return field_valid(field) && s_cfg_fields[field].secret;
}

esp_err_t jr_cfg_validate(jr_cfg_field_t field, const char *value)
{
    if (!field_valid(field) || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t cap = s_cfg_fields[field].capacity;
    const size_t len = strnlen(value, cap);
    if (len >= cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (len == 0U) {
        return ESP_OK; /* Empty explicitly clears any supported field. */
    }

    switch (field) {
    case JR_CFG_AGENT_NAME:
        if (has_control(value) || value[0] == ' ' || value[len - 1U] == ' ') {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case JR_CFG_WIFI_SSID:
        if (has_control(value)) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case JR_CFG_WIFI_PASSWORD:
        if (len < 8U || len > 63U || has_control(value)) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case JR_CFG_LLM_API_KEY:
    case JR_CFG_JARVIS_MCP_KEY:
        if (has_control_or_space(value)) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case JR_CFG_JARVIS_MCP_URL: {
        const char *host = NULL;
        if (strncmp(value, "https://", 8U) == 0) {
            host = value + 8U;
        } else if (strncmp(value, "http://", 7U) == 0) {
            host = value + 7U;
        }
        if (host == NULL || *host == '\0' || has_control_or_space(value)) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    }
    case JR_CFG_PAIRING_TOKEN:
        if (len < 32U || has_control_or_space(value)) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t cfg_get_from_handle(nvs_handle_t handle, const char *key,
                                     char *buf, size_t cap)
{
    if (key == NULL || buf == NULL || cap == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    size_t len = cap;
    return nvs_get_str(handle, key, buf, &len);
}

esp_err_t jr_cfg_get_str(const char *key, char *buf, size_t cap)
{
    if (key == NULL || buf == NULL || cap == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    nvs_handle_t handle;
    esp_err_t err = nvs_open(JR_NET_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = cfg_get_from_handle(handle, key, buf, cap);
    nvs_close(handle);
    return err;
}

static esp_err_t cfg_hash_present(nvs_handle_t handle, bool *present)
{
    char hash[JR_CFG_PAIRING_TOKEN_CAP] = {0};
    esp_err_t err = cfg_get_from_handle(handle, JR_PAIR_HASH_KEY,
                                        hash, sizeof hash);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* A plaintext token may exist on devices upgraded from v4. It counts as
         * configured and is migrated by pairing_token_ensure(). */
        err = cfg_get_from_handle(handle, "pairing_token", hash, sizeof hash);
    }
    if (err == ESP_OK) {
        *present = hash[0] != '\0';
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *present = false;
    }
    secure_zero(hash, sizeof hash);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t jr_cfg_get(jr_cfg_field_t field, char *buf, size_t cap,
                     jr_cfg_view_t view)
{
    if (!field_valid(field) || buf == NULL || cap == 0U ||
        (view != JR_CFG_VIEW_INTERNAL && view != JR_CFG_VIEW_MASKED)) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(JR_NET_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    if (field == JR_CFG_PAIRING_TOKEN) {
        bool present = false;
        err = cfg_hash_present(handle, &present);
        if (err == ESP_OK && present && view == JR_CFG_VIEW_MASKED) {
            err = copy_string(buf, cap, JR_CFG_SECRET_MASK);
        } else if (err == ESP_OK && present) {
            /* New tokens deliberately cannot be recovered from NVS. */
            err = ESP_ERR_NOT_SUPPORTED;
        } else if (err == ESP_OK) {
            err = ESP_ERR_NVS_NOT_FOUND;
        }
        nvs_close(handle);
        return err;
    }

    if (view == JR_CFG_VIEW_MASKED && s_cfg_fields[field].secret) {
        size_t len = 0U;
        err = nvs_get_str(handle, s_cfg_fields[field].key, NULL, &len);
        if (err == ESP_OK && len > 1U) {
            err = copy_string(buf, cap, JR_CFG_SECRET_MASK);
        }
    } else {
        err = cfg_get_from_handle(handle, s_cfg_fields[field].key, buf, cap);
    }

    if (field == JR_CFG_LLM_API_KEY && err == ESP_ERR_NVS_NOT_FOUND) {
        if (view == JR_CFG_VIEW_MASKED) {
            size_t len = 0U;
            err = nvs_get_str(handle, JR_LEGACY_GEMINI_KEY, NULL, &len);
            if (err == ESP_OK && len > 1U) {
                err = copy_string(buf, cap, JR_CFG_SECRET_MASK);
            }
        } else {
            err = cfg_get_from_handle(handle, JR_LEGACY_GEMINI_KEY, buf, cap);
        }
    }

    nvs_close(handle);
    return err;
}

static const char *cfg_value(const jr_net_config_t *config, jr_cfg_field_t field)
{
    switch (field) {
    case JR_CFG_AGENT_NAME:     return config->agent_name;
    case JR_CFG_WIFI_SSID:      return config->wifi_ssid;
    case JR_CFG_WIFI_PASSWORD:  return config->wifi_password;
    case JR_CFG_LLM_API_KEY:    return config->llm_api_key;
    case JR_CFG_JARVIS_MCP_URL: return config->jarvis_mcp_url;
    case JR_CFG_JARVIS_MCP_KEY: return config->jarvis_mcp_key;
    case JR_CFG_PAIRING_TOKEN:  return config->pairing_token;
    default:                    return NULL;
    }
}

static char *cfg_value_mut(jr_net_config_t *config, jr_cfg_field_t field,
                           size_t *capacity)
{
    *capacity = s_cfg_fields[field].capacity;
    switch (field) {
    case JR_CFG_AGENT_NAME:     return config->agent_name;
    case JR_CFG_WIFI_SSID:      return config->wifi_ssid;
    case JR_CFG_WIFI_PASSWORD:  return config->wifi_password;
    case JR_CFG_LLM_API_KEY:    return config->llm_api_key;
    case JR_CFG_JARVIS_MCP_URL: return config->jarvis_mcp_url;
    case JR_CFG_JARVIS_MCP_KEY: return config->jarvis_mcp_key;
    case JR_CFG_PAIRING_TOKEN:  return config->pairing_token;
    default:                    return NULL;
    }
}

esp_err_t jr_cfg_load(jr_net_config_t *out, jr_cfg_view_t view)
{
    if (out == NULL || (view != JR_CFG_VIEW_INTERNAL && view != JR_CFG_VIEW_MASKED)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    for (int i = JR_CFG_AGENT_NAME; i < JR_CFG_FIELD_COUNT; ++i) {
        const jr_cfg_field_t field = (jr_cfg_field_t)i;
        size_t cap = 0U;
        char *value = cfg_value_mut(out, field, &cap);
        esp_err_t err = jr_cfg_get(field, value, cap, view);
        if (err == ESP_ERR_NVS_NOT_FOUND ||
            (field == JR_CFG_PAIRING_TOKEN && err == ESP_ERR_NOT_SUPPORTED &&
             view == JR_CFG_VIEW_INTERNAL)) {
            value[0] = '\0';
            continue;
        }
        if (err != ESP_OK) {
            secure_zero(out, sizeof *out);
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t cfg_set_or_erase(nvs_handle_t handle, const char *key,
                                  const char *value)
{
    if (value[0] == '\0') {
        esp_err_t err = nvs_erase_key(handle, key);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    return nvs_set_str(handle, key, value);
}

static void bytes_to_hex(const uint8_t *bytes, size_t count, char *hex)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        hex[i * 2U] = digits[bytes[i] >> 4U];
        hex[i * 2U + 1U] = digits[bytes[i] & 0x0FU];
    }
    hex[count * 2U] = '\0';
}

static esp_err_t token_hash(const char *token, char hash_hex[JR_CFG_PAIRING_TOKEN_CAP])
{
    uint8_t digest[32];
    /* mbedTLS 3.x (IDF 5.5): the one-shot lost its _ret suffix. */
    if (mbedtls_sha256((const unsigned char *)token, strlen(token), digest, 0) != 0) {
        return ESP_FAIL;
    }
    bytes_to_hex(digest, sizeof digest, hash_hex);
    secure_zero(digest, sizeof digest);
    return ESP_OK;
}

static esp_err_t cfg_set_pairing_hash(nvs_handle_t handle, const char *token)
{
    esp_err_t err;
    if (token[0] == '\0') {
        err = cfg_set_or_erase(handle, JR_PAIR_HASH_KEY, "");
    } else {
        char hash[JR_CFG_PAIRING_TOKEN_CAP];
        err = token_hash(token, hash);
        if (err == ESP_OK) {
            err = nvs_set_str(handle, JR_PAIR_HASH_KEY, hash);
        }
        secure_zero(hash, sizeof hash);
    }
    if (err == ESP_OK) {
        /* Remove any pre-v5 plaintext copy as part of the same commit. */
        esp_err_t erase_err = nvs_erase_key(handle, "pairing_token");
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
        }
    }
    return err;
}

esp_err_t jr_cfg_apply(const jr_net_config_t *config, uint32_t field_mask)
{
    if (config == NULL || (field_mask & ~JR_CFG_F_ALL) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = JR_CFG_AGENT_NAME; i < JR_CFG_FIELD_COUNT; ++i) {
        const jr_cfg_field_t field = (jr_cfg_field_t)i;
        if ((field_mask & (1U << i)) == 0U) {
            continue;
        }
        esp_err_t err = jr_cfg_validate(field, cfg_value(config, field));
        if (err != ESP_OK) {
            return err;
        }
    }
    if (field_mask == 0U) {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(JR_NET_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = JR_CFG_AGENT_NAME; i < JR_CFG_FIELD_COUNT && err == ESP_OK; ++i) {
        const jr_cfg_field_t field = (jr_cfg_field_t)i;
        if ((field_mask & (1U << i)) == 0U) {
            continue;
        }
        const char *value = cfg_value(config, field);
        if (field == JR_CFG_PAIRING_TOKEN) {
            err = cfg_set_pairing_hash(handle, value);
        } else {
            err = cfg_set_or_erase(handle, s_cfg_fields[field].key, value);
            if (err == ESP_OK && field == JR_CFG_LLM_API_KEY) {
                esp_err_t erase_err = nvs_erase_key(handle, JR_LEGACY_GEMINI_KEY);
                if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
                    err = erase_err;
                }
            }
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t jr_cfg_set(jr_cfg_field_t field, const char *value)
{
    if (!field_valid(field) || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    jr_net_config_t config = {0};
    size_t cap = 0U;
    char *slot = cfg_value_mut(&config, field, &cap);
    esp_err_t err = copy_string(slot, cap, value);
    if (err == ESP_OK) {
        err = jr_cfg_apply(&config, 1U << field);
    }
    secure_zero(&config, sizeof config);
    return err;
}

static esp_err_t cfg_set_internal_string(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(JR_NET_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = cfg_set_or_erase(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t jr_net_pairing_token_ensure(char *token, size_t cap, bool *created)
{
    if ((token != NULL && cap < JR_CFG_PAIRING_TOKEN_CAP) ||
        (token == NULL && cap != 0U)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (token != NULL) {
        token[0] = '\0';
    }
    if (created != NULL) {
        *created = false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(JR_NET_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char stored_hash[JR_CFG_PAIRING_TOKEN_CAP] = {0};
    err = cfg_get_from_handle(handle, JR_PAIR_HASH_KEY, stored_hash,
                              sizeof stored_hash);
    if (err == ESP_OK && strlen(stored_hash) == 64U) {
        nvs_close(handle);
        secure_zero(stored_hash, sizeof stored_hash);
        return ESP_OK; /* Existing hashed tokens are intentionally unrecoverable. */
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        secure_zero(stored_hash, sizeof stored_hash);
        return err;
    }

    char plaintext[JR_CFG_PAIRING_TOKEN_CAP] = {0};
    err = cfg_get_from_handle(handle, "pairing_token", plaintext, sizeof plaintext);
    bool migrated = err == ESP_OK && jr_cfg_validate(JR_CFG_PAIRING_TOKEN, plaintext) == ESP_OK;
    if (!migrated) {
        uint8_t random[JR_PAIR_TOKEN_BYTES];
        esp_fill_random(random, sizeof random);
        bytes_to_hex(random, sizeof random, plaintext);
        secure_zero(random, sizeof random);
    }

    char hash[JR_CFG_PAIRING_TOKEN_CAP];
    err = token_hash(plaintext, hash);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, JR_PAIR_HASH_KEY, hash);
    }
    if (err == ESP_OK) {
        esp_err_t erase_err = nvs_erase_key(handle, "pairing_token");
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err == ESP_OK && token != NULL) {
        err = copy_string(token, cap, plaintext);
    }
    if (err == ESP_OK && created != NULL) {
        *created = true;
    }
    nvs_close(handle);
    secure_zero(stored_hash, sizeof stored_hash);
    secure_zero(hash, sizeof hash);
    secure_zero(plaintext, sizeof plaintext);
    return err;
}

esp_err_t jr_net_pairing_token_verify(const char *candidate, bool *matches)
{
    if (candidate == NULL || matches == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *matches = false;
    if (jr_cfg_validate(JR_CFG_PAIRING_TOKEN, candidate) != ESP_OK) {
        return ESP_OK;
    }

    char expected[JR_CFG_PAIRING_TOKEN_CAP] = {0};
    esp_err_t err = jr_cfg_get_str(JR_PAIR_HASH_KEY, expected, sizeof expected);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Upgrade a pre-v5 plaintext token in place before comparing. */
        char legacy[JR_CFG_PAIRING_TOKEN_CAP] = {0};
        err = jr_cfg_get_str("pairing_token", legacy, sizeof legacy);
        if (err == ESP_OK &&
            jr_cfg_validate(JR_CFG_PAIRING_TOKEN, legacy) == ESP_OK) {
            err = token_hash(legacy, expected);
            if (err == ESP_OK) {
                err = jr_cfg_set(JR_CFG_PAIRING_TOKEN, legacy);
            }
        }
        secure_zero(legacy, sizeof legacy);
    }
    if (err != ESP_OK || strlen(expected) != 64U) {
        secure_zero(expected, sizeof expected);
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    char actual[JR_CFG_PAIRING_TOKEN_CAP];
    err = token_hash(candidate, actual);
    if (err == ESP_OK) {
        unsigned diff = 0U;
        for (size_t i = 0; i < 64U; ++i) {
            diff |= (unsigned)((uint8_t)expected[i] ^ (uint8_t)actual[i]);
        }
        *matches = diff == 0U;
    }
    secure_zero(expected, sizeof expected);
    secure_zero(actual, sizeof actual);
    return err;
}

static esp_err_t wifi_lock_take(void)
{
    if (s_wifi_lock == NULL ||
        xSemaphoreTake(s_wifi_lock, pdMS_TO_TICKS(JR_NET_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void wifi_lock_give(void)
{
    xSemaphoreGive(s_wifi_lock);
}

static void set_last_error(esp_err_t err)
{
    s_last_error = err;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_wifi_events, JR_NET_SCAN_DONE_BIT);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_wifi_started = true;
        if (esp_wifi_set_ps(s_requested_ps) == ESP_OK) {
            s_applied_ps = s_requested_ps;
        }
        if (s_connect_requested) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                set_last_error(err);
                xEventGroupSetBits(s_wifi_events, JR_NET_FAIL_BIT);
            }
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        s_wifi_started = false;
        s_connected = false;
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        s_last_disconnect_reason = event != NULL ? event->reason : 0U;
        s_applied_ps = (wifi_ps_type_t)-1;
        if (esp_wifi_set_ps(s_requested_ps) == ESP_OK) {
            s_applied_ps = s_requested_ps;
        }
        xEventGroupClearBits(s_wifi_events, JR_NET_CONNECTED_BIT);
        if (s_connect_requested && s_retry < JR_NET_MAX_RETRY) {
            ++s_retry;
            ESP_LOGW(TAG, "sta disconnected (reason=%u); retry %u/%u",
                     (unsigned)s_last_disconnect_reason, (unsigned)s_retry,
                     (unsigned)JR_NET_MAX_RETRY);
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                set_last_error(err);
                xEventGroupSetBits(s_wifi_events, JR_NET_FAIL_BIT);
            }
        } else if (s_connect_requested) {
            s_connect_requested = false;
            set_last_error(ESP_FAIL);
            xEventGroupSetBits(s_wifi_events, JR_NET_FAIL_BIT);
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        s_wifi_started = true;
        s_provisioning = true;
        if (esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) {
            s_applied_ps = WIFI_PS_NONE;
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        s_provisioning = false;
        if (esp_wifi_set_ps(s_requested_ps) == ESP_OK) {
            s_applied_ps = s_requested_ps;
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        s_retry = 0U;
        s_connected = true;
        set_last_error(ESP_OK);
        const wifi_ps_type_t ps =
            s_provisioning ? WIFI_PS_NONE : s_requested_ps;
        if (esp_wifi_set_ps(ps) == ESP_OK) {
            s_applied_ps = ps;
        }
        xEventGroupClearBits(s_wifi_events, JR_NET_FAIL_BIT);
        xEventGroupSetBits(s_wifi_events, JR_NET_CONNECTED_BIT);
        if (event != NULL) {
            ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        }
        /* A provisioning AP is a recovery surface, not a permanent second
         * network. Tear it down as soon as the station has proved connectivity. */
        if (s_provisioning && esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
            s_provisioning = false;
            if (esp_wifi_set_ps(s_requested_ps) == ESP_OK) {
                s_applied_ps = s_requested_ps;
            }
            s_ap_ssid[0] = '\0';
        }
    }
}

static esp_err_t set_unique_hostname(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }
    char hostname[32];
    snprintf(hostname, sizeof hostname, "jarvisnano-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    err = esp_netif_set_hostname(s_sta_netif, hostname);
    if (err == ESP_OK) {
        err = esp_netif_set_hostname(s_ap_netif, hostname);
    }
    return err;
}

esp_err_t jr_net_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_wifi_events = xEventGroupCreate();
    s_wifi_lock = xSemaphoreCreateMutex();
    if (s_wifi_events == NULL || s_wifi_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_handler);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_ip_handler);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err != ESP_OK) {
        set_last_error(err);
        return err;
    }

    esp_err_t hostname_err = set_unique_hostname();
    if (hostname_err != ESP_OK) {
        ESP_LOGW(TAG, "hostname setup failed: %s", esp_err_to_name(hostname_err));
    }
    s_inited = true;
    set_last_error(ESP_OK);
    ESP_LOGI(TAG, "wifi initialized (bounded STA + protected APSTA fallback)");
    return ESP_OK;
}

static esp_err_t wifi_start_locked(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        /* esp_wifi_start() completes driver start before returning; the event
         * handler remains the source of truth for subsequent stop events. */
        s_wifi_started = true;
    }
    return err;
}

esp_err_t jr_net_wifi_connect_start(void)
{
    esp_err_t err = jr_net_init();
    if (err != ESP_OK) {
        return err;
    }

    jr_net_config_t config;
    err = jr_cfg_load(&config, JR_CFG_VIEW_INTERNAL);
    if (err != ESP_OK) {
        return err;
    }
    if (config.wifi_ssid[0] == '\0') {
        secure_zero(&config, sizeof config);
        return ESP_ERR_NOT_FOUND;
    }
    err = jr_cfg_validate(JR_CFG_WIFI_SSID, config.wifi_ssid);
    if (err == ESP_OK) {
        err = jr_cfg_validate(JR_CFG_WIFI_PASSWORD, config.wifi_password);
    }
    if (err != ESP_OK) {
        secure_zero(&config, sizeof config);
        return err;
    }

    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, config.wifi_ssid, strlen(config.wifi_ssid));
    memcpy(wifi.sta.password, config.wifi_password, strlen(config.wifi_password));
    wifi.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi.sta.threshold.authmode = config.wifi_password[0] == '\0'
                                      ? WIFI_AUTH_OPEN
                                      : WIFI_AUTH_WPA2_PSK;
    wifi.sta.pmf_cfg.capable = true;
    wifi.sta.pmf_cfg.required = false;
    const size_t ssid_len = strlen(config.wifi_ssid);
    secure_zero(&config, sizeof config);

    err = wifi_lock_take();
    if (err != ESP_OK) {
        secure_zero(&wifi, sizeof wifi);
        return err;
    }

    const bool was_started = s_wifi_started;
    const bool was_connected = s_connected;
    s_connect_requested = false;
    s_retry = 0U;
    s_connected = false;
    xEventGroupClearBits(s_wifi_events, JR_NET_CONNECTED_BIT | JR_NET_FAIL_BIT);

    wifi_mode_t mode = s_provisioning ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    err = esp_wifi_set_mode(mode);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi);
    }
    if (err == ESP_OK && was_connected) {
        err = esp_wifi_disconnect();
    }
    if (err == ESP_OK) {
        s_connect_requested = true;
        err = wifi_start_locked();
    }
    if (err == ESP_OK && was_started && !was_connected) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        s_connect_requested = false;
        set_last_error(err);
    }
    wifi_lock_give();
    secure_zero(&wifi, sizeof wifi);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "station join started (SSID len=%u)", (unsigned)ssid_len);
    }
    return err;
}

esp_err_t jr_net_wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_inited || s_wifi_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_connected) {
        return ESP_OK;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, JR_NET_CONNECTED_BIT | JR_NET_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & JR_NET_CONNECTED_BIT) != 0U && s_connected) {
        return ESP_OK;
    }
    if ((bits & JR_NET_FAIL_BIT) != 0U) {
        return s_last_error == ESP_OK ? ESP_FAIL : s_last_error;
    }
    set_last_error(ESP_ERR_TIMEOUT);
    return ESP_ERR_TIMEOUT;
}

esp_err_t jr_net_wifi_connect_bounded(uint32_t timeout_ms)
{
    esp_err_t err = jr_net_wifi_connect_start();
    if (err == ESP_OK) {
        err = jr_net_wifi_wait_connected(timeout_ms);
    }
    if (err == ESP_OK) {
        if (s_provisioning) {
            (void)jr_net_provisioning_stop();
        }
        return ESP_OK;
    }

    esp_err_t fallback_err = jr_net_provisioning_start();
    if (fallback_err != ESP_OK) {
        ESP_LOGE(TAG, "station join failed and provisioning AP could not start: %s",
                 esp_err_to_name(fallback_err));
        return fallback_err;
    }
    ESP_LOGW(TAG, "station unavailable (%s); provisioning APSTA is active",
             esp_err_to_name(err));
    return err;
}

esp_err_t jr_net_wifi_connect(void)
{
    return jr_net_wifi_connect_bounded(JR_NET_DEFAULT_TIMEOUT_MS);
}

bool jr_net_is_connected(void)
{
    return s_connected;
}

static esp_err_t provision_credentials(char ssid[JR_CFG_WIFI_SSID_CAP],
                                       char password[JR_NET_AP_PASSWORD_CAP])
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(ssid, JR_CFG_WIFI_SSID_CAP, "JarvisNano-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    err = jr_cfg_get_str(JR_PROVISION_PASS_KEY, password, JR_NET_AP_PASSWORD_CAP);
    if (err == ESP_OK && strlen(password) == JR_NET_AP_PASSWORD_LEN) {
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    static const char alphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    uint8_t random[JR_NET_AP_PASSWORD_LEN];
    esp_fill_random(random, sizeof random);
    for (size_t i = 0; i < sizeof random; ++i) {
        password[i] = alphabet[random[i] % (sizeof alphabet - 1U)];
    }
    password[sizeof random] = '\0';
    secure_zero(random, sizeof random);
    return cfg_set_internal_string(JR_PROVISION_PASS_KEY, password);
}

esp_err_t jr_net_provisioning_start(void)
{
    esp_err_t err = jr_net_init();
    if (err != ESP_OK) {
        return err;
    }

    char ssid[JR_CFG_WIFI_SSID_CAP] = {0};
    char password[JR_NET_AP_PASSWORD_CAP] = {0};
    err = provision_credentials(ssid, password);
    if (err != ESP_OK) {
        secure_zero(password, sizeof password);
        return err;
    }

    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, ssid, strlen(ssid));
    ap.ap.ssid_len = strlen(ssid);
    memcpy(ap.ap.password, password, strlen(password));
    ap.ap.channel = 1U;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4U;
    ap.ap.pmf_cfg.capable = true;
    ap.ap.pmf_cfg.required = false;
    secure_zero(password, sizeof password);

    err = wifi_lock_take();
    if (err != ESP_OK) {
        secure_zero(&ap, sizeof ap);
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    if (err == ESP_OK) {
        err = wifi_start_locked();
    }
    if (err == ESP_OK) {
        s_provisioning = true;
        (void)copy_string(s_ap_ssid, sizeof s_ap_ssid, ssid);
        set_last_error(ESP_OK);
    } else {
        set_last_error(err);
    }
    wifi_lock_give();
    secure_zero(&ap, sizeof ap);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "provisioning AP started: %s (WPA2 password on local display)",
                 ssid);
    }
    return err;
}

esp_err_t jr_net_provisioning_stop(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = wifi_lock_take();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        s_provisioning = false;
        s_ap_ssid[0] = '\0';
    }
    wifi_lock_give();
    return err;
}

esp_err_t jr_net_provisioning_info(jr_net_provisioning_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    esp_err_t err = provision_credentials(out->ssid, out->password);
    if (err != ESP_OK) {
        secure_zero(out, sizeof *out);
        return err;
    }
    if (s_ap_netif != NULL) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
            snprintf(out->ip, sizeof out->ip, IPSTR, IP2STR(&ip.ip));
        }
    }
    return ESP_OK;
}

static jr_net_auth_t map_auth(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:         return JR_NET_AUTH_OPEN;
    case WIFI_AUTH_WEP:          return JR_NET_AUTH_WEP;
    case WIFI_AUTH_WPA_PSK:      return JR_NET_AUTH_WPA_PSK;
    case WIFI_AUTH_WPA2_PSK:     return JR_NET_AUTH_WPA2_PSK;
    case WIFI_AUTH_WPA_WPA2_PSK: return JR_NET_AUTH_WPA_WPA2_PSK;
    case WIFI_AUTH_WPA3_PSK:     return JR_NET_AUTH_WPA3_PSK;
    case WIFI_AUTH_WPA2_WPA3_PSK:return JR_NET_AUTH_WPA2_WPA3_PSK;
    default:                     return JR_NET_AUTH_OTHER;
    }
}

esp_err_t jr_net_wifi_scan(jr_net_ap_record_t *records, size_t capacity,
                           size_t *count, uint32_t timeout_ms)
{
    if (count == NULL || (capacity > 0U && records == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0U;
    if (capacity > JR_NET_SCAN_MAX_RESULTS) {
        capacity = JR_NET_SCAN_MAX_RESULTS;
    }

    esp_err_t err = jr_net_init();
    if (err != ESP_OK) {
        return err;
    }
    err = wifi_lock_take();
    if (err != ESP_OK) {
        return err;
    }
    err = wifi_start_locked();
    if (err != ESP_OK) {
        wifi_lock_give();
        return err;
    }

    wifi_scan_config_t scan = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 40U, .max = 120U},
    };
    xEventGroupClearBits(s_wifi_events, JR_NET_SCAN_DONE_BIT);
    err = esp_wifi_scan_start(&scan, false);
    if (err == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, JR_NET_SCAN_DONE_BIT, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(timeout_ms));
        if ((bits & JR_NET_SCAN_DONE_BIT) == 0U) {
            (void)esp_wifi_scan_stop();
            err = ESP_ERR_TIMEOUT;
        }
    }

    if (err == ESP_OK) {
        uint16_t fetch = (uint16_t)capacity;
        wifi_ap_record_t found[JR_NET_SCAN_MAX_RESULTS];
        memset(found, 0, sizeof found);
        if (fetch > 0U) {
            err = esp_wifi_scan_get_ap_records(&fetch, found);
        } else {
            uint16_t available = 0U;
            err = esp_wifi_scan_get_ap_num(&available);
        }
        if (err == ESP_OK) {
            for (uint16_t i = 0U; i < fetch; ++i) {
                const size_t ssid_len = strnlen((const char *)found[i].ssid, 32U);
                memcpy(records[i].ssid, found[i].ssid, ssid_len);
                records[i].ssid[ssid_len] = '\0';
                records[i].rssi = found[i].rssi;
                records[i].channel = found[i].primary;
                records[i].auth = map_auth(found[i].authmode);
            }
            *count = fetch;
        }
        secure_zero(found, sizeof found);
    }
    set_last_error(err);
    wifi_lock_give();
    return err;
}

esp_err_t jr_net_get_status(jr_net_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    out->mode = JR_NET_MODE_OFF;
    out->rssi = -127;
    out->wifi_started = s_wifi_started;
    out->sta_connected = s_connected;
    out->provisioning_active = s_provisioning;
    out->retry_count = s_retry;
    out->last_disconnect_reason = s_last_disconnect_reason;
    out->last_error = s_last_error;
    (void)copy_string(out->ap_ssid, sizeof out->ap_ssid, s_ap_ssid);

    if (!s_inited) {
        return ESP_OK;
    }
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        switch (mode) {
        case WIFI_MODE_STA:   out->mode = JR_NET_MODE_STA; break;
        case WIFI_MODE_AP:    out->mode = JR_NET_MODE_AP; break;
        case WIFI_MODE_APSTA: out->mode = JR_NET_MODE_APSTA; break;
        default:              out->mode = JR_NET_MODE_OFF; break;
        }
    }
    if (s_sta_netif != NULL && s_connected) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            snprintf(out->sta_ip, sizeof out->sta_ip, IPSTR, IP2STR(&ip.ip));
        }
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
            out->channel = ap.primary;
        }
    }
    if (s_ap_netif != NULL && s_provisioning) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
            snprintf(out->ap_ip, sizeof out->ap_ip, IPSTR, IP2STR(&ip.ip));
        }
    }
    return ESP_OK;
}

esp_err_t jr_net_set_power_save(bool enabled)
{
    const wifi_ps_type_t requested =
        enabled && !s_provisioning ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE;
    if (s_requested_ps == requested && s_applied_ps == requested) {
        return ESP_OK;
    }
    s_requested_ps = requested;
    if (!s_wifi_started) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_ps(requested);
    if (err == ESP_OK) {
        s_applied_ps = requested;
        ESP_LOGI(TAG, "wifi power save: %s",
                 requested == WIFI_PS_NONE ? "realtime" : "min-modem");
    }
    return err;
}

bool jr_net_power_save_active(void)
{
    return s_wifi_started && s_applied_ps != WIFI_PS_NONE;
}

esp_err_t jr_net_mdns_start(const char *hostname)
{
    if (hostname == NULL || hostname[0] == '\0' || has_control_or_space(hostname)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void jr_net_mdns_stop(void)
{
    /* Deliberate no-op until espressif/mdns is a pinned project dependency. */
}
