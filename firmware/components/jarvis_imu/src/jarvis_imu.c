/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 */
#include "jarvis_imu.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define QMI8658_ADDR_PRIMARY    0x6B   /* SA0 = 0 (board default) */
#define QMI8658_ADDR_SECONDARY  0x6A   /* SA0 = 1                 */

#define QMI8658_REG_WHOAMI      0x00   /* -> 0x05 */
#define QMI8658_REG_CTRL1       0x02
#define QMI8658_REG_CTRL2       0x03
#define QMI8658_REG_CTRL7       0x08
#define QMI8658_REG_AX_L        0x35   /* AX_L..AZ_H = 0x35..0x3A */

#define QMI8658_WHOAMI_VALUE    0x05

/* CTRL1: ADDR_AI (auto-increment, bit6), little-endian (BE=0), oscillator on. */
#define QMI8658_CTRL1_CFG       0x40
/* CTRL2: aFS = 010 (±8 g) | aODR = 0110 (125 Hz Normal) = 0x26. */
#define QMI8658_CTRL2_CFG       0x26
/* CTRL7: aEN = 1 (accelerometer on), gyro off. */
#define QMI8658_CTRL7_CFG       0x01

/* ±8 g full scale -> 4096 LSB/g. */
#define QMI8658_ACC_LSB_PER_G   4096.0f
#define RAD_TO_DEG              57.2957795f

#define QMI8658_I2C_TIMEOUT_MS  100

static const char *TAG = "jarvis_imu";

static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static bool s_ready;

static esp_err_t imu_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1,
                                       pdMS_TO_TICKS(QMI8658_I2C_TIMEOUT_MS));
}

static esp_err_t imu_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf),
                               pdMS_TO_TICKS(QMI8658_I2C_TIMEOUT_MS));
}

/* Add the device handle for one candidate address and check WHO_AM_I. */
static bool imu_probe_addr(i2c_master_bus_handle_t bus, uint8_t addr)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        return false;
    }
    uint8_t who = 0;
    if (imu_read_reg(QMI8658_REG_WHOAMI, &who) == ESP_OK && who == QMI8658_WHOAMI_VALUE) {
        s_addr = addr;
        return true;
    }
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

static esp_err_t imu_lazy_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = esp_board_manager_get_periph_handle("i2c_master", (void **)&bus);
    if (err != ESP_OK || bus == NULL) {
        ESP_LOGW(TAG, "i2c_master bus handle unavailable: %s", esp_err_to_name(err));
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_STATE;
    }

    if (!imu_probe_addr(bus, QMI8658_ADDR_PRIMARY) &&
        !imu_probe_addr(bus, QMI8658_ADDR_SECONDARY)) {
        ESP_LOGW(TAG, "QMI8658 not found at 0x%02X/0x%02X",
                 QMI8658_ADDR_PRIMARY, QMI8658_ADDR_SECONDARY);
        return ESP_ERR_NOT_FOUND;
    }

    /* Enable the accelerometer only: ±8 g, 125 Hz. Gyro stays off (power). */
    imu_write_reg(QMI8658_REG_CTRL1, QMI8658_CTRL1_CFG);
    imu_write_reg(QMI8658_REG_CTRL2, QMI8658_CTRL2_CFG);
    imu_write_reg(QMI8658_REG_CTRL7, QMI8658_CTRL7_CFG);
    /* Let the first conversion settle before the caller reads it. */
    vTaskDelay(pdMS_TO_TICKS(20));

    s_ready = true;
    ESP_LOGI(TAG, "QMI8658 online @0x%02X (accel ±8g/125Hz)", s_addr);
    return ESP_OK;
}

static int16_t imu_read_axis(uint8_t reg_l)
{
    uint8_t lo = 0, hi = 0;
    imu_read_reg(reg_l, &lo);
    imu_read_reg(reg_l + 1, &hi);
    return (int16_t)(((uint16_t)hi << 8) | lo);
}

static const char *orientation_from_g(float gx, float gy, float gz)
{
    /* Board mounting: the QMI8658 Z+ axis points away from the AMOLED front, so
     * lying screen-up reads gz ~= -1 g (confirmed on hardware 2026-06-11). Hence
     * face_up == gz negative. Pick the dominant axis and name the face. */
    float ax = fabsf(gx), ay = fabsf(gy), az = fabsf(gz);
    if (az >= ax && az >= ay) {
        if (az < 0.6f) return "tilted";
        return gz < 0 ? "face_up" : "face_down";
    }
    if (ay >= ax) {
        return gy > 0 ? "portrait_up" : "portrait_down";
    }
    return gx > 0 ? "landscape_right" : "landscape_left";
}

esp_err_t jarvis_imu_read(jarvis_imu_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    esp_err_t err = imu_lazy_init();
    if (err != ESP_OK) {
        return err;
    }

    out->present  = true;
    out->i2c_addr = s_addr;

    /* The QMI8658 outputs 0x8000 (INT16_MIN) on every axis until the first
     * conversion after the accelerometer is enabled is ready. Retry briefly so
     * the first /api/imu call after boot returns a real sample, not the sentinel. */
    for (int tries = 0; tries < 8; tries++) {
        out->ax = imu_read_axis(QMI8658_REG_AX_L);
        out->ay = imu_read_axis(QMI8658_REG_AX_L + 2);
        out->az = imu_read_axis(QMI8658_REG_AX_L + 4);
        if (!(out->ax == INT16_MIN && out->ay == INT16_MIN && out->az == INT16_MIN)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    out->gx = (float)out->ax / QMI8658_ACC_LSB_PER_G;
    out->gy = (float)out->ay / QMI8658_ACC_LSB_PER_G;
    out->gz = (float)out->az / QMI8658_ACC_LSB_PER_G;

    out->pitch_deg = atan2f(out->gx, sqrtf(out->gy * out->gy + out->gz * out->gz)) * RAD_TO_DEG;
    out->roll_deg  = atan2f(out->gy, out->gz) * RAD_TO_DEG;
    out->orientation = orientation_from_g(out->gx, out->gy, out->gz);

    return ESP_OK;
}
