/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_power — AXP2101 battery telemetry for JarvisRobot v5.
 *
 * Ported 2026-07-18 from firmware/components/jarvis_pmic (Phase 1 of
 * docs/JARVISNANO_OS_PLAN.md). Register access is carried over verbatim.
 * As there, this component is READ-ONLY and NEVER touches rail configuration:
 * the board declares the AXP2101 with init_skip:true because the factory
 * power-on defaults already bring up the display and audio rails, and
 * re-sequencing them is risky. It only enables two detection bits (battery ADC
 * channel, battery-presence detect), neither of which is a power rail.
 *
 * Threading, as with jr_imu, is inverted relative to the original: a slow
 * sampler task owns the I2C traffic and jr_power_read() is a non-blocking
 * snapshot copy. The battery rim arc (PWR-06) is drawn every frame at 24 fps,
 * and I2C in the render path would be a frame-rate bug waiting to happen.
 * Battery state moves on a scale of minutes, so the sampler runs at 0.2 Hz —
 * ~5 transactions per 5 s, negligible on the shared bus.
 *
 * Register facts from XPowersLib (lewisxhe/XPowersLib, MIT):
 *   STATUS1 0x00  bit5 = VBUS good, bit3 = battery present
 *   STATUS2 0x01  bits[7:5]: 1 = charging, 2 = discharging
 *   ADC_CHANNEL_CTRL 0x30  bit0 = enable battery-voltage ADC
 *   ADC_DATA_RELUST0/1 0x34/0x35  battery voltage, H5L8 -> mV
 *   BAT_PERCENT_DATA 0xA4  fuel-gauge percent (valid only with battery present)
 *
 * NOT in scope here, deliberately (see docs/JARVISNANO_OS_PLAN.md, Phase 5):
 * rail gating for WHISPER/DREAM would require reversing the init_skip decision.
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
    uint32_t sample_seq;   /* increments per published sample; 0 = never sampled */
    uint32_t age_ms;       /* ms since this snapshot was published               */
} jr_power_t;

/**
 * @brief Start the background battery sampler.
 *
 * Lazily acquires the shared bus and AXP2101 handle, enables the battery ADC
 * channel and presence detection, then spawns a 0.2 Hz sampler. Safe to call
 * twice. Never fatal: on failure the device reports present=false.
 */
esp_err_t jr_power_start(void);

/** @brief Suspend sampling (Phase 5 power moods). */
void jr_power_stop(void);

/**
 * @brief Copy the most recent snapshot. NON-BLOCKING — safe from the render
 *        task or an httpd handler.
 *
 * @param[out] out  filled on ESP_OK; ESP_ERR_INVALID_STATE before the first
 *                  sample lands (the sampler takes one period to warm up).
 */
esp_err_t jr_power_read(jr_power_t *out);

#ifdef __cplusplus
}
#endif
