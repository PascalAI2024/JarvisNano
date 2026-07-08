/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_net/src/jr_net.c — Wi-Fi station + NVS "app" config adapter.
 *
 * Harvested from v4's esp-claw wifi_manager (station init + GOT_IP -> PS_NONE +
 * exp-backoff reconnect) and app_config (NVS namespace "app", key table). Kept
 * deliberately small: station-only, auto-reconnect, credentials from NVS. The
 * provisioning AP is out of scope for the v5 device bring-up (creds land in NVS
 * directly). Nothing here is hardcoded — empty NVS => no connect, logged.
 */
#include "jr_net/jr_net.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"

static const char *TAG = "jr_net";

#define JR_NET_NS            "app"
#define JR_NET_CONNECTED_BIT BIT0
#define JR_NET_FAIL_BIT      BIT1
#define JR_NET_MAX_RETRY     8

static EventGroupHandle_t s_wifi_events;
static esp_netif_t       *s_sta_netif;
static volatile bool      s_connected;
static int                s_retry;
static bool               s_inited;

esp_err_t jr_cfg_get_str(const char *key, char *buf, size_t cap)
{
    if (key == NULL || buf == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(JR_NET_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;   /* namespace absent == not provisioned */
    }
    size_t len = cap;
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry < JR_NET_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "sta disconnected; retry %d/%d", s_retry, JR_NET_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "sta connect failed after %d retries", JR_NET_MAX_RETRY);
            xEventGroupSetBits(s_wifi_events, JR_NET_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_connected = true;
        /* Modem-sleep OFF: required for reliable WSS audio + HTTP snapshot
         * throughput (v4 wifi_manager GOT_IP note). */
        esp_wifi_set_ps(WIFI_PS_NONE);
        xEventGroupSetBits(s_wifi_events, JR_NET_CONNECTED_BIT);
    }
}

esp_err_t jr_net_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;   /* INVALID_STATE == already created; tolerate */
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_inited = true;
    ESP_LOGI(TAG, "wifi init OK (station mode)");
    return ESP_OK;
}

esp_err_t jr_net_wifi_connect(void)
{
    if (!s_inited) {
        esp_err_t e = jr_net_init();
        if (e != ESP_OK) {
            return e;
        }
    }

    char ssid[64] = {0};
    char pass[128] = {0};
    esp_err_t err = jr_cfg_get_str("wifi_ssid", ssid, sizeof ssid);
    if (err != ESP_OK || ssid[0] == '\0') {
        ESP_LOGW(TAG, "no wifi_ssid in NVS 'app' — skipping connect (provision via NVS)");
        return ESP_ERR_NOT_FOUND;
    }
    (void)jr_cfg_get_str("wifi_password", pass, sizeof pass);

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_retry = 0;
    xEventGroupClearBits(s_wifi_events, JR_NET_CONNECTED_BIT | JR_NET_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());   /* STA_START -> esp_wifi_connect() */

    ESP_LOGI(TAG, "connecting to SSID (len=%u) ...", (unsigned)strlen(ssid));
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, JR_NET_CONNECTED_BIT | JR_NET_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & JR_NET_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "wifi connect timed out / failed");
    return ESP_FAIL;
}

bool jr_net_is_connected(void)
{
    return s_connected;
}
