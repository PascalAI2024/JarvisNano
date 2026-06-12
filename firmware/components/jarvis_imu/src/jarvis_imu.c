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
#include "freertos/semphr.h"
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

/* Gap between burst samples / sentinel retries. CONFIG_FREERTOS_HZ=100 makes
 * one tick 10 ms, so pdMS_TO_TICKS(5) rounds down to 0 — a "delay" that never
 * sleeps. Clamp to >= 1 tick so the pacing is real. 10 ms also clears the
 * 125 Hz accel ODR period (8 ms): every paced sample is a fresh conversion. */
#define IMU_SAMPLE_GAP_TICKS  ((pdMS_TO_TICKS(10) > 0) ? pdMS_TO_TICKS(10) : 1)

/* Sentinel (0x8000) retries after enable. Bounded so the single httpd task is
 * never parked long: worst case 3 * 1 tick on the first call after init only. */
#define IMU_SENTINEL_RETRIES  3

static const char *TAG = "jarvis_imu";

static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static bool s_ready;

/* Guards lazy init (probe + CTRL config + s_dev/s_ready publication). Created
 * from a C constructor, which ESP-IDF runs single-threaded during startup —
 * before app_main() and before any task that could call jarvis_imu_read()
 * exists — so there is no mutex-creation race to bootstrap around. Statically
 * allocated: no heap, cannot fail. */
static SemaphoreHandle_t s_init_lock;
static StaticSemaphore_t s_init_lock_buf;

__attribute__((constructor))
static void imu_init_lock_ctor(void)
{
    s_init_lock = xSemaphoreCreateMutexStatic(&s_init_lock_buf);
}

/* NOTE: the new i2c_master API takes its timeout in MILLISECONDS
 * (xfer_timeout_ms), not ticks — wrapping it in pdMS_TO_TICKS() would
 * silently shrink 100 ms to 10 at CONFIG_FREERTOS_HZ=100. */
static esp_err_t imu_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1,
                                       QMI8658_I2C_TIMEOUT_MS);
}

static esp_err_t imu_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf),
                               QMI8658_I2C_TIMEOUT_MS);
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

static esp_err_t imu_lazy_init_locked(void)
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

    /* Enable the accelerometer only: ±8 g, 125 Hz. Gyro stays off (power).
     * A failed CTRL write means the device is NOT configured — propagate the
     * error and unwind so the next call re-probes, instead of latching
     * s_ready over a half-initialized sensor. */
    err = imu_write_reg(QMI8658_REG_CTRL1, QMI8658_CTRL1_CFG);
    if (err == ESP_OK) {
        err = imu_write_reg(QMI8658_REG_CTRL2, QMI8658_CTRL2_CFG);
    }
    if (err == ESP_OK) {
        err = imu_write_reg(QMI8658_REG_CTRL7, QMI8658_CTRL7_CFG);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 CTRL config failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    /* Let the first conversion settle before the caller reads it. */
    vTaskDelay(pdMS_TO_TICKS(20));

    s_ready = true;
    ESP_LOGI(TAG, "QMI8658 online @0x%02X (accel ±8g/125Hz)", s_addr);
    return ESP_OK;
}

static esp_err_t imu_lazy_init(void)
{
    if (s_init_lock == NULL) {
        /* Constructor did not run — should be impossible; stay safe, not racy. */
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_init_lock, portMAX_DELAY);
    esp_err_t err = imu_lazy_init_locked();
    xSemaphoreGive(s_init_lock);
    return err;
}

/* Read AX_L..AZ_H (0x35..0x3A) in ONE 6-byte I2C transaction. CTRL1.ADDR_AI
 * (bit 6, set in QMI8658_CTRL1_CFG) auto-increments the register address
 * within the transaction, so the axis pair bytes are coherent — separate
 * 1-byte reads could pair L/H bytes from different 125 Hz conversions
 * (torn 16-bit values). */
static esp_err_t imu_read_axes(int16_t *ax, int16_t *ay, int16_t *az)
{
    uint8_t reg = QMI8658_REG_AX_L;
    uint8_t raw[6] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, raw, sizeof(raw),
                                                QMI8658_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    *ax = (int16_t)(((uint16_t)raw[1] << 8) | raw[0]);
    *ay = (int16_t)(((uint16_t)raw[3] << 8) | raw[2]);
    *az = (int16_t)(((uint16_t)raw[5] << 8) | raw[4]);
    return ESP_OK;
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
     * the first /api/imu call after boot returns a real sample, not the
     * sentinel. Retries are bounded (init already waited 20 ms and the ODR
     * period is 8 ms, so one retry normally suffices) to cap the time this
     * spends parked on the caller — typically the single httpd task. */
    err = imu_read_axes(&out->ax, &out->ay, &out->az);
    if (err != ESP_OK) {
        return err;
    }
    for (int tries = 0;
         tries < IMU_SENTINEL_RETRIES &&
         out->ax == INT16_MIN && out->ay == INT16_MIN && out->az == INT16_MIN;
         tries++) {
        vTaskDelay(IMU_SAMPLE_GAP_TICKS);
        err = imu_read_axes(&out->ax, &out->ay, &out->az);
        if (err != ESP_OK) {
            return err;
        }
    }

    out->gx = (float)out->ax / QMI8658_ACC_LSB_PER_G;
    out->gy = (float)out->ay / QMI8658_ACC_LSB_PER_G;
    out->gz = (float)out->az / QMI8658_ACC_LSB_PER_G;

    out->pitch_deg = atan2f(out->gx, sqrtf(out->gy * out->gy + out->gz * out->gz)) * RAD_TO_DEG;
    out->roll_deg  = atan2f(out->gy, out->gz) * RAD_TO_DEG;
    out->orientation = orientation_from_g(out->gx, out->gy, out->gz);

    /* Motion energy: std-dev of the accel-vector magnitude over a short
     * on-demand burst (~40 ms window: 5 samples one tick / 10 ms apart, the
     * first reusing the sample already read above). At rest this sits near 0;
     * handling pushes it up, a shake spikes it. This is a deliberate burst
     * inside the call, NOT a background poller — the shared I2C bus stays
     * quiet between requests. Delays happen only BETWEEN samples, keeping the
     * caller parked ~40 ms typical, ~70 ms worst case (first call after boot). */
    enum { MOTION_SAMPLES = 5 };
    float mag[MOTION_SAMPLES];
    float sum = 0.0f;
    int n = 0;
    mag[n] = sqrtf(out->gx * out->gx + out->gy * out->gy + out->gz * out->gz);
    sum += mag[n];
    n++;
    for (int i = 1; i < MOTION_SAMPLES; i++) {
        vTaskDelay(IMU_SAMPLE_GAP_TICKS);
        int16_t bx, by, bz;
        if (imu_read_axes(&bx, &by, &bz) != ESP_OK) {
            break;   /* keep what we have; stats fall back below */
        }
        float fx = (float)bx / QMI8658_ACC_LSB_PER_G;
        float fy = (float)by / QMI8658_ACC_LSB_PER_G;
        float fz = (float)bz / QMI8658_ACC_LSB_PER_G;
        mag[n] = sqrtf(fx * fx + fy * fy + fz * fz);
        sum += mag[n];
        n++;
    }
    if (n >= 2) {
        float mean = sum / (float)n;
        float var = 0.0f;
        for (int i = 0; i < n; i++) {
            float d = mag[i] - mean;
            var += d * d;
        }
        out->motion_mg = sqrtf(var / (float)n) * 1000.0f;   /* g -> milli-g */
    }
    /* With < 2 samples motion_mg stays 0 (memset above) — no motion claim. */
    out->moving = out->motion_mg > 50.0f;        /* handled / picked up */
    out->shake  = out->motion_mg > 350.0f;       /* deliberate shake    */

    return ESP_OK;
}
