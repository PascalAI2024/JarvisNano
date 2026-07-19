/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_imu — QMI8658 accelerometer telemetry for JarvisRobot v5.
 *
 * Ported 2026-07-18 from firmware/components/jarvis_imu (Phase 1 of
 * docs/JARVISNANO_OS_PLAN.md). The register-level driver is carried over
 * verbatim — it is hardware-earned — but the THREADING MODEL is deliberately
 * inverted, and that is the whole point of the port:
 *
 *   OLD: jarvis_imu_read() did a 5-sample / 10 ms-apart motion burst INSIDE the
 *        call, parking the caller ~40 ms typical, ~70 ms worst case. Acceptable
 *        for a lone httpd task; fatal in the voice pump or the 24 fps render
 *        task, both of which now want IMU data.
 *   NEW: a low-priority sampler task owns the bus traffic and runs the SAME
 *        burst continuously (one sample per 10 ms tick, std-dev over the last 5
 *        = the identical 40 ms window). jr_imu_read() copies the published
 *        snapshot under a briefly-held mutex and returns in microseconds.
 *
 * Because the burst window is bit-for-bit the original, the hardware-calibrated
 * motion thresholds (moving > 50 mg, shake > 350 mg) remain valid.
 *
 * TRADE-OFF, stated plainly: the original component deliberately had NO
 * background poller, because the I2C bus is shared (SDA15/SCL14 @400 kHz with
 * AXP2101, codecs, touch, RTC). This reverses that decision, because gestures
 * (GEST-01..06) and tilt-driven UI cannot be built on on-demand sampling. The
 * cost is ~100 short transactions/sec, roughly 2% of bus time. jr_imu_stop()
 * exists so the Phase 5 power moods can silence it, and Phase 5 replaces the
 * poll entirely with the QMI8658's own No-Motion/Any-Motion/Tap engines on
 * INT2 -> GPIO21.
 *
 * Register facts: QMI8658A datasheet Rev A.
 *   WHO_AM_I 0x00 -> 0x05         CTRL1 0x02  CTRL2 0x03  CTRL7 0x08
 *   AX_L..AZ_H 0x35..0x3A (signed 16-bit, little-endian)
 *   I2C address: SA0=0 -> 0x6B (default), SA0=1 -> 0x6A
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     present;       /* QMI8658 answered WHO_AM_I == 0x05                 */
    uint8_t  i2c_addr;      /* the address that responded (0x6B or 0x6A)         */
    int16_t  ax, ay, az;    /* raw accelerometer counts                          */
    float    gx, gy, gz;    /* acceleration in g (±8 g range -> 4096 LSB/g)      */
    float    pitch_deg;     /* tilt fore/aft, atan2(gx, sqrt(gy^2+gz^2))         */
    float    roll_deg;      /* tilt left/right, atan2(gy, gz)                    */
    const char *orientation;/* coarse face: "face_up"/"face_down"/"tilted"/...   */
    float    motion_mg;     /* std-dev of |accel| over the last 40 ms, milli-g   */
    bool     moving;        /* motion_mg > 50 mg  (handled / picked up)          */
    bool     shake;         /* motion_mg > 350 mg (deliberate shake)             */
    uint32_t sample_seq;    /* increments per published sample; 0 = never sampled*/
    uint32_t age_ms;        /* ms since this snapshot was published              */
} jr_imu_t;

/**
 * @brief Start the background sampler.
 *
 * Probes the QMI8658 (0x6B then 0x6A), configures accel-only ±8 g @125 Hz, and
 * spawns the sampler task. Safe to call twice (second call is a no-op).
 *
 * @return ESP_OK if the sampler is running; ESP_ERR_NOT_FOUND if no QMI8658
 *         answered; another esp_err_t if the shared bus was unavailable.
 *         On failure the device simply reports present=false — never fatal.
 */
esp_err_t jr_imu_start(void);

/** @brief Suspend sampling and release the bus (Phase 5 power moods). */
void jr_imu_stop(void);

/**
 * @brief Copy the most recent snapshot. NON-BLOCKING — safe from the voice
 *        pump, the render task, or an httpd handler.
 *
 * @param[out] out  filled on ESP_OK. If the sampler has not produced a sample
 *                  yet, returns ESP_ERR_INVALID_STATE and zeroes *out.
 */
esp_err_t jr_imu_read(jr_imu_t *out);

/** @brief True once a QMI8658 has answered WHO_AM_I. */
bool jr_imu_present(void);

#ifdef __cplusplus
}
#endif
