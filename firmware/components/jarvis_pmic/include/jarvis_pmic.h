/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * jarvis_pmic — read-only AXP2101 battery telemetry for JarvisNano.
 *
 * The Waveshare ESP32-S3-Touch-AMOLED-1.75 declares the AXP2101 PMIC with
 * init_skip:true — the factory power-on defaults already bring up the display
 * and audio rails, and re-sequencing them is risky. This component therefore
 * NEVER touches rail configuration. It only:
 *   - borrows the board-manager I2C master bus (SDA15/SCL14, 400 kHz),
 *   - adds an extra device handle for the AXP2101 at 0x34,
 *   - reads the fuel-gauge / status / ADC registers on demand.
 *
 * Register facts are taken from XPowersLib (lewisxhe/XPowersLib, MIT):
 *   STATUS1 0x00  bit5 = VBUS good, bit3 = battery present
 *   STATUS2 0x01  bits[7:5]: 1 = charging, 2 = discharging
 *   ADC_CHANNEL_CTRL 0x30  bit0 = enable battery-voltage ADC
 *   ADC_DATA_RELUST0/1 0x34/0x35  battery voltage, H5L8 → mV
 *   BAT_PERCENT_DATA 0xA4  fuel-gauge percent (valid only with battery present)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     present;      /* battery physically connected (STATUS1 bit3)        */
    bool     charging;     /* PMU is actively charging (STATUS2 == 1)            */
    bool     usb_present;  /* VBUS good — USB plugged in (STATUS1 bit5)          */
    uint8_t  percent;      /* fuel-gauge 0..100, or 0xFF when unavailable        */
    uint16_t millivolts;   /* battery voltage in mV, 0 when no battery           */
} jarvis_battery_t;

/**
 * @brief Read AXP2101 battery telemetry on demand.
 *
 * Lazily acquires the shared I2C bus and AXP2101 device handle on first call.
 * Thread-safe: the ESP-IDF i2c_master driver serializes transactions per bus,
 * so this coexists with the touch / codec traffic on the same bus.
 *
 * @param[out] out  filled with the latest reading on ESP_OK.
 * @return ESP_OK on success; an esp_err_t if the bus/device is unavailable.
 */
esp_err_t jarvis_pmic_read_battery(jarvis_battery_t *out);

#ifdef __cplusplus
}
#endif
