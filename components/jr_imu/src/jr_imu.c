/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * See jr_imu.h for the threading rationale. Register-level code is carried over
 * verbatim from firmware/components/jarvis_imu/src/jarvis_imu.c, including the
 * two hardware-earned gotchas called out inline below — do not "simplify" them.
 */
#include "jr_imu/jr_imu.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps / vTaskDeleteWithCaps */
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

/* Sampler cadence. CONFIG_FREERTOS_HZ=100 makes one tick 10 ms, so
 * pdMS_TO_TICKS(10) == 1 tick; clamp to >= 1 so a config change can never make
 * this a delay that never sleeps. 10 ms also clears the 125 Hz accel ODR period
 * (8 ms): every sample is a fresh conversion, exactly as the original burst. */
#define IMU_SAMPLE_GAP_TICKS  ((pdMS_TO_TICKS(10) > 0) ? pdMS_TO_TICKS(10) : 1)

/* Sentinel (0x8000) retries after enable — see the read path. */
#define IMU_SENTINEL_RETRIES  3

/* Motion window. FIVE samples 10 ms apart == the 40 ms window the shipped
 * thresholds below were calibrated against. Changing this invalidates them. */
#define MOTION_SAMPLES        5
#define MOTION_MOVING_MG      50.0f
#define MOTION_SHAKE_MG       350.0f

#define IMU_TASK_STACK        3072
#define IMU_TASK_PRIO         3

static const char *TAG = "jr_imu";

static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static bool    s_ready;
static TaskHandle_t s_task;
static volatile bool s_run;
/* True when the task stack came from PSRAM via xTaskCreateWithCaps — such a
 * task MUST be torn down with vTaskDeleteWithCaps or its stack leaks. */
static bool s_ext_stack;

/* Published snapshot + its guard. The sampler holds the mutex only long enough
 * to memcpy the struct, so jr_imu_read() never meaningfully blocks. */
static jr_imu_t s_snap;
static int64_t  s_snap_us;
static SemaphoreHandle_t s_snap_lock;
static StaticSemaphore_t s_snap_lock_buf;

/* Created from a C constructor: ESP-IDF runs these single-threaded during
 * startup, before app_main() and before any task that could call in exists, so
 * there is no mutex-creation race to bootstrap around. Static: no heap. */
__attribute__((constructor))
static void imu_lock_ctor(void)
{
    s_snap_lock = xSemaphoreCreateMutexStatic(&s_snap_lock_buf);
}

/* NOTE: the new i2c_master API takes its timeout in MILLISECONDS
 * (xfer_timeout_ms), not ticks — wrapping it in pdMS_TO_TICKS() would silently
 * shrink 100 ms to 10 at CONFIG_FREERTOS_HZ=100. */
static esp_err_t imu_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1,
                                       QMI8658_I2C_TIMEOUT_MS);
}

static esp_err_t imu_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), QMI8658_I2C_TIMEOUT_MS);
}

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
    if (imu_read_reg(QMI8658_REG_WHOAMI, &who) == ESP_OK &&
        who == QMI8658_WHOAMI_VALUE) {
        s_addr = addr;
        return true;
    }
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

static esp_err_t imu_bring_up(void)
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

    /* Accelerometer only: ±8 g, 125 Hz. Gyro stays off (power). A failed CTRL
     * write means the device is NOT configured — unwind rather than latch
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
    vTaskDelay(pdMS_TO_TICKS(20));   /* let the first conversion settle */

    s_ready = true;
    ESP_LOGI(TAG, "QMI8658 online @0x%02X (accel ±8g/125Hz)", s_addr);
    return ESP_OK;
}

/* Read AX_L..AZ_H (0x35..0x3A) in ONE 6-byte transaction. CTRL1.ADDR_AI
 * auto-increments within the transaction, so the axis byte pairs are coherent;
 * separate 1-byte reads could pair L/H from different 125 Hz conversions
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
     * lying screen-up reads gz ~= -1 g (confirmed on hardware 2026-06-11).
     * Hence face_up == gz negative. Pick the dominant axis and name the face. */
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

static void imu_task(void *arg)
{
    (void)arg;

    /* Rolling window of accel-vector magnitudes, newest at head. std-dev over
     * the whole window is motion_mg — identical maths to the original burst. */
    float    mag[MOTION_SAMPLES] = {0};
    int      filled = 0;
    int      head = 0;
    uint32_t seq = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (s_run) {
        int16_t ax = 0, ay = 0, az = 0;
        if (imu_read_axes(&ax, &ay, &az) != ESP_OK) {
            vTaskDelayUntil(&last_wake, IMU_SAMPLE_GAP_TICKS);
            continue;
        }

        /* The QMI8658 emits 0x8000 on every axis until the first conversion
         * after enable is ready. Skip the sentinel rather than publish it. */
        if (ax == INT16_MIN && ay == INT16_MIN && az == INT16_MIN) {
            vTaskDelayUntil(&last_wake, IMU_SAMPLE_GAP_TICKS);
            continue;
        }

        jr_imu_t s;
        memset(&s, 0, sizeof(s));
        s.present  = true;
        s.i2c_addr = s_addr;
        s.ax = ax; s.ay = ay; s.az = az;
        s.gx = (float)ax / QMI8658_ACC_LSB_PER_G;
        s.gy = (float)ay / QMI8658_ACC_LSB_PER_G;
        s.gz = (float)az / QMI8658_ACC_LSB_PER_G;
        s.pitch_deg = atan2f(s.gx, sqrtf(s.gy * s.gy + s.gz * s.gz)) * RAD_TO_DEG;
        s.roll_deg  = atan2f(s.gy, s.gz) * RAD_TO_DEG;
        s.orientation = orientation_from_g(s.gx, s.gy, s.gz);

        mag[head] = sqrtf(s.gx * s.gx + s.gy * s.gy + s.gz * s.gz);
        head = (head + 1) % MOTION_SAMPLES;
        if (filled < MOTION_SAMPLES) {
            filled++;
        }
        if (filled >= 2) {
            float sum = 0.0f;
            for (int i = 0; i < filled; i++) {
                sum += mag[i];
            }
            const float mean = sum / (float)filled;
            float var = 0.0f;
            for (int i = 0; i < filled; i++) {
                const float d = mag[i] - mean;
                var += d * d;
            }
            s.motion_mg = sqrtf(var / (float)filled) * 1000.0f;  /* g -> mg */
        }
        /* With < 2 samples motion_mg stays 0 (memset) — no motion claim. */
        s.moving = s.motion_mg > MOTION_MOVING_MG;
        s.shake  = s.motion_mg > MOTION_SHAKE_MG;
        s.sample_seq = ++seq;

        if (xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_snap = s;
            s_snap_us = esp_timer_get_time();
            xSemaphoreGive(s_snap_lock);
        }

        vTaskDelayUntil(&last_wake, IMU_SAMPLE_GAP_TICKS);
    }

    const bool ext = s_ext_stack;
    s_task = NULL;
    s_ext_stack = false;
    if (ext) {
        vTaskDeleteWithCaps(NULL);   /* frees the PSRAM stack too */
    } else {
        vTaskDelete(NULL);
    }
}

esp_err_t jr_imu_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    esp_err_t err = imu_bring_up();
    if (err != ESP_OK) {
        return err;
    }
    s_run = true;
    /* Put the sampler stack in PSRAM. Internal RAM is the scarcest resource on
     * this board — the Gemini TLS handshake fails once the largest contiguous
     * INTERNAL block drops under ~8 KB — and this task never touches flash, so
     * it is never running with the cache disabled. Same guarded pattern as
     * jr_tools, which has shipped a PSRAM stack here for a while. Falls back to
     * an internal stack if EXT_MEM task creation is unavailable or fails. */
    BaseType_t ok = pdFAIL;
#if defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && defined(CONFIG_SPIRAM) && \
    CONFIG_SPIRAM
    ok = xTaskCreateWithCaps(imu_task, "jr_imu", IMU_TASK_STACK, NULL,
                             IMU_TASK_PRIO, &s_task,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ext_stack = (ok == pdPASS);
    if (ok != pdPASS) {
        s_task = NULL;
    }
#endif
    if (ok != pdPASS) {
        ok = xTaskCreate(imu_task, "jr_imu", IMU_TASK_STACK, NULL,
                         IMU_TASK_PRIO, &s_task);
    }
    if (ok != pdPASS) {
        s_run = false;
        ESP_LOGE(TAG, "sampler task creation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sampler running (%d ms period, %d-sample motion window)",
             (int)(IMU_SAMPLE_GAP_TICKS * portTICK_PERIOD_MS), MOTION_SAMPLES);
    return ESP_OK;
}

void jr_imu_stop(void)
{
    s_run = false;   /* the task removes itself on its next wake */
}

esp_err_t jr_imu_read(jr_imu_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (s_snap_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool have = s_snap.sample_seq != 0;
    if (have) {
        *out = s_snap;
        const int64_t age = esp_timer_get_time() - s_snap_us;
        out->age_ms = (uint32_t)(age > 0 ? age / 1000 : 0);
    }
    xSemaphoreGive(s_snap_lock);
    return have ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool jr_imu_present(void)
{
    return s_ready;
}
