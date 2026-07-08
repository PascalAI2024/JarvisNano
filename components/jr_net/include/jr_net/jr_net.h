/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_net — L0 network + config bring-up for the composition root.
 *
 * A thin device-side adapter over ESP-IDF Wi-Fi station + the NVS "app"
 * namespace. Credentials are runtime-only (NVS), NEVER hardcoded — the repo
 * ships empty defaults and the SSID / password / Gemini key are provisioned to
 * NVS on the device. This is the ONLY audio/voice-adjacent component that owns
 * the radio + persistence; the pure core never sees an esp_wifi/nvs type.
 *
 * NVS namespace: "app"   (keys: "wifi_ssid", "wifi_password", "llm_api_key", ...)
 */
#ifndef JR_NET_JR_NET_H
#define JR_NET_JR_NET_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up esp_netif + the default event loop + Wi-Fi in station mode. NVS must
 * already be initialised (main owns nvs_flash_init). Idempotent. */
esp_err_t jr_net_init(void);

/* Read wifi_ssid/wifi_password from NVS "app" and connect the station. Blocks up
 * to ~30 s for an IP. Returns ESP_OK once connected, ESP_ERR_NOT_FOUND if no SSID
 * is provisioned, or the underlying error. Auto-reconnect stays armed after. */
esp_err_t jr_net_wifi_connect(void);

/* True once the station holds an IP. */
bool jr_net_is_connected(void);

/* Read one string value from the NVS "app" namespace into buf (NUL-terminated).
 * Returns ESP_OK on hit, ESP_ERR_NVS_NOT_FOUND if absent. Never logs the value
 * (secrets: llm_api_key, wifi_password). */
esp_err_t jr_cfg_get_str(const char *key, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* JR_NET_JR_NET_H */
