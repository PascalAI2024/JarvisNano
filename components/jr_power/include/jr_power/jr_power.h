/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_power — AXP2101 battery telemetry for JarvisRobot v5.
 *
 * Ported 2026-07-18 from firmware/components/jarvis_pmic (Phase 1 of
 * docs/JARVISNANO_OS_PLAN.md). Register access is carried over verbatim.
 * Telemetry and charge diagnostics NEVER touch charge or rail configuration:
 * the board declares the AXP2101 with init_skip:true because the factory
 * power-on defaults already bring up the display and audio rails, and
 * re-sequencing them is risky. It only enables two detection bits (battery ADC
 * channel, battery-presence detect), neither of which is a power rail. PKEY
 * IRQ setup/ack and explicit jr_power_off() retain their existing writes.
 *
 * Threading, as with jr_imu, is inverted relative to the original: a slow
 * sampler task owns the telemetry I2C traffic and readers copy the cache.
 * The battery rim arc (PWR-06) is drawn every frame at 24 fps,
 * and I2C in the render path would be a frame-rate bug waiting to happen.
 * Battery samples run at 1 Hz and PKEY polling at 500 ms. Fixed read-only
 * charge diagnostics start every 5 s, one bounded burst per sampler pass.
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
#include "jr_power/charge.h"

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

/* Consume latched physical power-key presses (counts since last call). The
 * AXP2101 latches PKEY short/long in INTSTS2 over I2C — no IRQ line needed
 * on the 1.75C (the old TCA9554 route is gone). Polled at 500 ms by the
 * sampler task; consumed by the composition root. Any-task safe. */
void jr_power_pkey_take(uint32_t *out_short, uint32_t *out_long);

/**
 * @brief Start the background battery sampler.
 *
 * Lazily acquires the shared bus and AXP2101 handle, enables the battery ADC
 * channel and presence detection, then spawns a 1 Hz battery sampler. Safe to call
 * twice. Never fatal: on failure the device reports present=false.
 */
esp_err_t jr_power_start(void);

/** @brief Suspend sampling (Phase 5 power moods). */
void jr_power_stop(void);

/**
 * @brief Copy the most recent battery snapshot, waiting at most 20 ms for
 *        its cache mutex. Performs no I2C.
 *
 * @param[out] out  filled on ESP_OK; ESP_ERR_INVALID_STATE before the first
 *                  sample lands (the sampler takes one period to warm up).
 */
esp_err_t jr_power_read(jr_power_t *out);

/**
 * Copy a completed fixed-register charge diagnostic. Try-lock only; performs
 * no I2C. ESP_ERR_INVALID_STATE until the first batch completes (about 3 s);
 * ESP_ERR_TIMEOUT if the cache is busy. All-failed batches still return ESP_OK:
 * inspect each register's attempted/read_ok/error/available and ages. Aged
 * values (>10 s since their actual read) are unavailable to the pure decoder.
 * Reads occur over span_ms; this is not an atomic hardware snapshot.
 */
esp_err_t jr_power_charge_read(jr_power_charge_t *out);

/**
 * @brief Power the whole device off through the PMIC: every rail, the ESP32
 *        included — the lowest state the board has, microamps from the cell.
 *
 * Sets the PMIC's power-on hold to one second first, so only a deliberate
 * hold of PWR brings it back (a cold boot; charging keeps working while
 * off). Does not return on success: the rails drop within milliseconds.
 * Callers say goodbye on the glass and settle storage before calling.
 */
esp_err_t jr_power_off(void);

#ifdef __cplusplus
}
#endif
