/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * jarvis_imu — read-only QMI8658 accelerometer telemetry for JarvisNano.
 *
 * The Waveshare ESP32-S3-Touch-AMOLED-1.75 carries a QMI8658 6-axis IMU on the
 * shared I2C bus (SDA15/SCL14, 400 kHz), alongside the AXP2101, codecs, touch
 * and RTC. It is NOT declared in board_devices.yaml — like jarvis_pmic this
 * component borrows the board-manager bus on demand and never stands up an
 * always-on poller (the bus is shared; reads happen only when asked).
 *
 * This first cut enables the accelerometer only (gyro left off to save power)
 * and reports raw + g-scaled axes plus a coarse orientation. The hardware tap /
 * wake-on-motion engines and the INT2->GPIO21 interrupt line (see
 * docs/reference/waveshare-amoled-175.md) are future work.
 *
 * Register facts: QMI8658A datasheet Rev A (docs/reference/vendor/datasheets).
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
    float    pitch_deg;     /* tilt fore/aft, atan2(ax, sqrt(ay^2+az^2))         */
    float    roll_deg;      /* tilt left/right, atan2(ay, az)                    */
    const char *orientation;/* coarse face: "face_up"/"face_down"/"tilted"/...   */
} jarvis_imu_t;

/**
 * @brief Read QMI8658 accelerometer telemetry on demand.
 *
 * Lazily acquires the shared I2C bus, probes the QMI8658 at 0x6B then 0x6A,
 * configures the accelerometer (±8 g, 125 Hz) on first success, and reads the
 * three axes. Thread-safe via the ESP-IDF i2c_master per-bus serialization.
 *
 * @param[out] out  filled on ESP_OK.
 * @return ESP_OK on success; an esp_err_t if the bus/device is unavailable.
 */
esp_err_t jarvis_imu_read(jarvis_imu_t *out);

#ifdef __cplusplus
}
#endif
