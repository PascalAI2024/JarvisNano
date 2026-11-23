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
#define QMI8658_REG_CTRL3       0x04   /* gyro: gFS bits 6:4, gODR bits 3:0 */
#define QMI8658_REG_CTRL7       0x08
#define QMI8658_REG_CTRL8       0x09   /* bit7: CTRL9 handshake on STATUSINT */
#define QMI8658_REG_CTRL9       0x0A   /* command register                 */
#define QMI8658_REG_CAL1_L      0x0B   /* WoM threshold, milli-g           */
#define QMI8658_REG_CAL1_H      0x0C   /* WoM pin select | initial | blank */
#define QMI8658_REG_STATUSINT   0x2D   /* bit7 CmdDone                     */
#define QMI8658_REG_REVISION    0x01
#define QMI8658_REG_RESET       0x60   /* 0xB0 = soft reset                */
#define QMI8658_RESET_CMD       0xB0
#define QMI8658_REG_AX_L        0x35   /* AX_L..AZ_H = 0x35..0x3A */
#define QMI8658_REG_TEMP_L      0x33   /* TEMP_L, TEMP_H: H + L/256 deg C */
#define QMI8658_REG_GX_L        0x3B   /* GX_L..GZ_H = 0x3B..0x40 */

#define QMI8658_WHOAMI_VALUE    0x05

/* CTRL1: ADDR_AI (auto-increment, bit6), little-endian (BE=0), oscillator on. */
#define QMI8658_CTRL1_CFG       0x40
/* CTRL2: aFS = 010 (±8 g) | aODR = 0110 (125 Hz Normal) = 0x26. */
#define QMI8658_CTRL2_CFG       0x26
/* CTRL7: aEN = 1 (accelerometer on), gyro off. */
#define QMI8658_CTRL7_CFG       0x01
/* CTRL1 with INT1 enabled (bit3) for Wake-on-Motion; CTRL2 for the low-power
 * 21 Hz accel ODR the engine runs at (aFS ±8 g | aODR 1101). CAL1_H: bit7
 * initial INT level 0, bit6 INT1, low six bits blanking samples. */
#define QMI8658_CTRL1_WOM       0x48
#define QMI8658_CTRL2_WOM       0x2D
#define QMI8658_CAL1_H_WOM      0x20
#define QMI8658_CMD_WRITE_WOM   0x08
#define QMI8658_CMD_ACK         0x00
#define QMI8658_STATUSINT_DONE  0x80
#define QMI8658_CTRL8_HS_STATUS 0x80   /* CmdDone in STATUSINT, not on INT1 */
#define IMU_CMD_TIMEOUT_US     100000LL
/* At CONFIG_FREERTOS_HZ=100 a 2 ms delay truncates to zero. Use a real
 * tick and a wall-clock deadline, including I2C time, for both handshake phases. */
#define IMU_CMD_GAP_TICKS      ((pdMS_TO_TICKS(2) > 0) ? pdMS_TO_TICKS(2) : 1)

/* ±8 g full scale -> 4096 LSB/g. */
#define QMI8658_ACC_LSB_PER_G   4096.0f
#define RAD_TO_DEG              57.2957795f

#define QMI8658_I2C_TIMEOUT_MS  100

/* Sampler cadence. CONFIG_FREERTOS_HZ=100 makes one tick 10 ms, so
 * pdMS_TO_TICKS(10) == 1 tick; clamp to >= 1 so a config change can never make
 * this a delay that never sleeps. 10 ms also clears the 125 Hz accel ODR period
 * (8 ms): every sample is a fresh conversion, exactly as the original burst.
 * In rest moods (WHISPER/DREAM) drop to 20 Hz (50 ms) with QMI8658 low-power
 * 21 Hz ODR (CTRL2 = 0x2D, same ±8 g full scale). */
#define IMU_SAMPLE_GAP_TICKS  ((pdMS_TO_TICKS(10) > 0) ? pdMS_TO_TICKS(10) : 1)
#define IMU_REST_GAP_TICKS    ((pdMS_TO_TICKS(50) > 0) ? pdMS_TO_TICKS(50) : 1)
#define QMI8658_CTRL2_REST    QMI8658_CTRL2_WOM

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
static volatile bool s_rest_req;
/* True when the task stack came from PSRAM via xTaskCreateWithCaps — such a
 * task MUST be torn down with vTaskDeleteWithCaps or its stack leaks. */
static bool s_ext_stack;

/* Published snapshot + its guard. The sampler holds the mutex only long enough
 * to memcpy the struct, so jr_imu_read() never meaningfully blocks. */
static jr_imu_t s_snap;
static int64_t  s_snap_us;
static SemaphoreHandle_t s_snap_lock;
static StaticSemaphore_t s_snap_lock_buf;

/* The tilt: the accelerometer's gravity direction, low-passed, as two signed
 * 16-bit milli-g values (x low half, y high half) in ONE word the sampler
 * stores and any task loads, no mutex. Sensor axes (the same ones gx/gy carry,
 * Z already flipped at the read seam); the composition root maps them to the
 * glass. Gain 1/8 per 10 ms sample (about an 80 ms time constant) takes the
 * hand's tremor out and none of its lean out. The motion window above is not
 * touched. */
#define TILT_LP_DIV  8
static volatile uint32_t s_tilt_word;
static volatile bool     s_tilt_valid;

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

static esp_err_t imu_wait_cmd_done(bool expected)
{
    const int64_t deadline = esp_timer_get_time() + IMU_CMD_TIMEOUT_US;
    do {
        uint8_t st = 0;
        const esp_err_t err = imu_read_reg(QMI8658_REG_STATUSINT, &st);
        if (err != ESP_OK) {
            return err;
        }
        if (((st & QMI8658_STATUSINT_DONE) != 0) == expected) {
            return ESP_OK;
        }
        vTaskDelay(IMU_CMD_GAP_TICKS);
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

/* QMI8658A datasheet §5.10.4: completion AND successful ACK/clear are
 * required before claiming WoM is armed. A register write alone is no proof. */
static esp_err_t imu_ctrl9(uint8_t cmd)
{
    /* Keep command completion off INT1, which is reserved for motion wake. */
    esp_err_t err = imu_write_reg(QMI8658_REG_CTRL8, QMI8658_CTRL8_HS_STATUS);
    if (err == ESP_OK) {
        err = imu_write_reg(QMI8658_REG_CTRL9, cmd);
    }
    if (err != ESP_OK) {
        return err;
    }
    const esp_err_t completion_err = imu_wait_cmd_done(true);
    if (completion_err != ESP_OK) {
        return completion_err;
    }
    const esp_err_t ack_err = imu_write_reg(QMI8658_REG_CTRL9, QMI8658_CMD_ACK);
    if (ack_err != ESP_OK) {
        return ack_err;
    }
    return imu_wait_cmd_done(false);
}

/* Clear a Wake-on-Motion left armed by the previous deep sleep with a soft
 * reset — the one thing that needs no command handshake. Idempotent on a
 * fresh part; the normal configuration follows. */
static void imu_soft_reset(void)
{
    (void)imu_write_reg(QMI8658_REG_RESET, QMI8658_RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t rev = 0;
    if (imu_read_reg(QMI8658_REG_REVISION, &rev) == ESP_OK) {
        ESP_LOGI(TAG, "QMI8658 reset, revision 0x%02X", rev);
    }
}

esp_err_t jr_imu_arm_wake_on_motion(uint8_t threshold_mg)
{
    if (!s_ready || s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The sampler owns the bus while it runs; give it its last tick. */
    for (int i = 0; i < 20 && s_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (threshold_mg == 0U) {
        threshold_mg = 1U;
    }
    esp_err_t err = imu_write_reg(QMI8658_REG_CTRL7, 0x00);
    if (err == ESP_OK) {
        err = imu_write_reg(QMI8658_REG_CTRL2, QMI8658_CTRL2_WOM);
    }
    if (err == ESP_OK) {
        err = imu_write_reg(QMI8658_REG_CAL1_L, threshold_mg);
    }
    if (err == ESP_OK) {
        err = imu_write_reg(QMI8658_REG_CAL1_H, QMI8658_CAL1_H_WOM);
    }
    if (err == ESP_OK) {
        err = imu_ctrl9(QMI8658_CMD_WRITE_WOM);
    }
    if (err == ESP_OK) {
        err = imu_write_reg(QMI8658_REG_CTRL7, QMI8658_CTRL7_CFG);
    }
    if (err == ESP_OK) {
        err = imu_write_reg(QMI8658_REG_CTRL1, QMI8658_CTRL1_WOM);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "QMI8658 wake-on-motion armed: %u mg on INT1",
                 (unsigned)threshold_mg);
    } else {
        ESP_LOGW(TAG, "QMI8658 wake-on-motion failed: %s", esp_err_to_name(err));
    }
    return err;
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

    /* A boot out of deep sleep finds the Wake-on-Motion engine still armed
     * (it is what woke us); clear it before the data configuration. */
    imu_soft_reset();

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
#if defined(CONFIG_ESP_BOARD_ESP32S3_TOUCH_AMOLED_1_75C)
    /* The 1.75C mounts the QMI8658 with Z inverted relative to the original
     * board: a face-up device on the desk read gz > 0 and the whole gesture
     * stack (flip-to-mute, mood ladder, wake gating) believed it was face-down
     * from boot (measured on hardware 2026-08-27: privacy_paused=true, instant
     * DREAM while face-up). Negate at the single read seam so every consumer
     * inherits the original board's convention. */
    *az = (int16_t)-*az;
#endif
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
    int32_t  lp_x_q4 = 0, lp_y_q4 = 0;   /* tilt low-pass, milli-g * 16 */
    bool     lp_seeded = false;
    uint8_t  ctrl2_applied = QMI8658_CTRL2_CFG;
    bool     in_rest = false;

    while (s_run) {
        /* Reconcile QMI8658 ODR with the lock-free rest hint. In AWAKE/AMBIENT
         * keep 125 Hz Normal (0x26) / 100 Hz polling; in WHISPER/DREAM switch
         * to 21 Hz low-power (0x2D) / 20 Hz polling. Leaving rest restores
         * 125 Hz immediately before reading so shake/tilt stay crisp. */
        const bool want_rest = __atomic_load_n(&s_rest_req, __ATOMIC_ACQUIRE);
        const uint8_t want_ctrl2 = want_rest ? QMI8658_CTRL2_REST : QMI8658_CTRL2_CFG;
        if (want_ctrl2 != ctrl2_applied) {
            if (imu_write_reg(QMI8658_REG_CTRL2, want_ctrl2) == ESP_OK) {
                ctrl2_applied = want_ctrl2;
                in_rest = want_rest;
                filled = 0;
                head = 0;
                last_wake = xTaskGetTickCount();
            }
        } else {
            in_rest = want_rest;
        }

        int16_t ax = 0, ay = 0, az = 0;
        if (imu_read_axes(&ax, &ay, &az) != ESP_OK) {
            if (in_rest) {
                (void)ulTaskNotifyTake(pdTRUE, IMU_REST_GAP_TICKS);
                last_wake = xTaskGetTickCount();
            } else {
                vTaskDelayUntil(&last_wake, IMU_SAMPLE_GAP_TICKS);
            }
            continue;
        }

        /* The QMI8658 emits 0x8000 on every axis until the first conversion
         * after enable is ready. Skip the sentinel rather than publish it. */
        if (ax == INT16_MIN && ay == INT16_MIN && az == INT16_MIN) {
            if (in_rest) {
                (void)ulTaskNotifyTake(pdTRUE, IMU_REST_GAP_TICKS);
                last_wake = xTaskGetTickCount();
            } else {
                vTaskDelayUntil(&last_wake, IMU_SAMPLE_GAP_TICKS);
            }
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
        /* pitch_deg / roll_deg are computed lazily in jr_imu_read() so the
         * sampler does not run atan2f/sqrtf at 100 Hz. */
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

        {
            const int32_t rx_q4 = ((int32_t)ax * 125 / 512) * 16;   /* LSB -> mg -> q4 */
            const int32_t ry_q4 = ((int32_t)ay * 125 / 512) * 16;
            if (!lp_seeded) {
                lp_x_q4 = rx_q4;
                lp_y_q4 = ry_q4;
                lp_seeded = true;
            } else {
                lp_x_q4 += (rx_q4 - lp_x_q4) / TILT_LP_DIV;
                lp_y_q4 += (ry_q4 - lp_y_q4) / TILT_LP_DIV;
            }
            const uint32_t w = (uint32_t)(uint16_t)(int16_t)(lp_x_q4 / 16) |
                               ((uint32_t)(uint16_t)(int16_t)(lp_y_q4 / 16) << 16);
            __atomic_store_n(&s_tilt_word, w, __ATOMIC_RELEASE);
            __atomic_store_n(&s_tilt_valid, true, __ATOMIC_RELEASE);
        }

        if (xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_snap = s;
            s_snap_us = esp_timer_get_time();
            xSemaphoreGive(s_snap_lock);
        }

        if (in_rest) {
            (void)ulTaskNotifyTake(pdTRUE, IMU_REST_GAP_TICKS);
            last_wake = xTaskGetTickCount();
        } else {
            vTaskDelayUntil(&last_wake, IMU_SAMPLE_GAP_TICKS);
        }
    }

    /* Restore normal 125 Hz CTRL2 if we stopped while resting, unless the
     * caller immediately arms WoM (which writes CTRL2 itself). */
    if (ctrl2_applied != QMI8658_CTRL2_CFG) {
        (void)imu_write_reg(QMI8658_REG_CTRL2, QMI8658_CTRL2_CFG);
    }

    __atomic_store_n(&s_tilt_valid, false, __ATOMIC_RELEASE);   /* a stopped sampler has no tilt */
    const bool ext = s_ext_stack;
    __atomic_store_n(&s_task, (TaskHandle_t)NULL, __ATOMIC_RELEASE);
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
    TaskHandle_t t = __atomic_load_n(&s_task, __ATOMIC_ACQUIRE);
    if (t != NULL) {
        xTaskNotifyGive(t);
    }
}

void jr_imu_set_rest(bool resting)
{
    const bool prev = __atomic_exchange_n(&s_rest_req, resting, __ATOMIC_ACQ_REL);
    if (prev && !resting) {
        TaskHandle_t t = __atomic_load_n(&s_task, __ATOMIC_ACQUIRE);
        if (t != NULL) {
            xTaskNotifyGive(t);
        }
    }
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
    if (have) {
        out->pitch_deg = atan2f(out->gx, sqrtf(out->gy * out->gy + out->gz * out->gz)) * RAD_TO_DEG;
        out->roll_deg  = atan2f(out->gy, out->gz) * RAD_TO_DEG;
    }
    return have ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool jr_imu_tilt_read(int *gx_mg, int *gy_mg)
{
    if (!__atomic_load_n(&s_tilt_valid, __ATOMIC_ACQUIRE)) {
        return false;
    }
    const uint32_t w = __atomic_load_n(&s_tilt_word, __ATOMIC_ACQUIRE);
    *gx_mg = (int)(int16_t)(uint16_t)(w & 0xFFFFu);
    *gy_mg = (int)(int16_t)(uint16_t)(w >> 16);
    return true;
}

bool jr_imu_present(void)
{
    return s_ready;
}

/* ---- board probe (GET /api/diag/board) ---------------------------------- */

/* Probe gyro setting: gFS = 101 (+-512 dps -> 64 LSB/dps), gODR = 0110
 * (112 Hz). 20 samples 10 ms apart; the gyro needs ~100 ms to settle. */
#define PROBE_CTRL3_GYRO     0x56
#define PROBE_CTRL7_GYRO_EN  0x02
#define PROBE_GYRO_LSB_PER_DPS 64.0f
#define PROBE_GYRO_SAMPLES   20
#define PROBE_GYRO_SETTLE_MS 100

static void probe_gyro(jr_probe_imu_t *out)
{
    const uint8_t ctrl3_orig = out->ctrl3;
    const uint8_t ctrl7_orig = out->ctrl7;
    float sum[3] = {0};
    uint8_t n = 0;

    esp_err_t err = imu_write_reg(QMI8658_REG_CTRL3, PROBE_CTRL3_GYRO);
    if (err == ESP_OK) {
        err = imu_write_reg(QMI8658_REG_CTRL7,
                            (uint8_t)(ctrl7_orig | PROBE_CTRL7_GYRO_EN));
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(PROBE_GYRO_SETTLE_MS));
        for (int i = 0; i < PROBE_GYRO_SAMPLES; ++i) {
            uint8_t reg = QMI8658_REG_GX_L;
            uint8_t raw[6] = {0};
            if (i2c_master_transmit_receive(s_dev, &reg, 1, raw, sizeof(raw),
                                            QMI8658_I2C_TIMEOUT_MS) == ESP_OK) {
                int16_t v[3];
                for (int a = 0; a < 3; ++a) {
                    v[a] = (int16_t)(((uint16_t)raw[2 * a + 1] << 8) | raw[2 * a]);
                }
                if (!(v[0] == INT16_MIN && v[1] == INT16_MIN && v[2] == INT16_MIN)) {
#if defined(CONFIG_ESP_BOARD_ESP32S3_TOUCH_AMOLED_1_75C)
                    v[2] = (int16_t)-v[2];   /* same Z flip as imu_read_axes() */
#endif
                    for (int a = 0; a < 3; ++a) {
                        const float dps = (float)v[a] / PROBE_GYRO_LSB_PER_DPS;
                        if (n == 0 || dps < out->gmin[a]) { out->gmin[a] = dps; }
                        if (n == 0 || dps > out->gmax[a]) { out->gmax[a] = dps; }
                        sum[a] += dps;
                    }
                    n++;
                }
            }
            vTaskDelay(IMU_SAMPLE_GAP_TICKS);
        }
    }

    /* Always put it back: gyro off first, then its config. */
    (void)imu_write_reg(QMI8658_REG_CTRL7, ctrl7_orig);
    (void)imu_write_reg(QMI8658_REG_CTRL3, ctrl3_orig);
    (void)imu_read_reg(QMI8658_REG_CTRL3, &out->ctrl3_after);
    (void)imu_read_reg(QMI8658_REG_CTRL7, &out->ctrl7_after);
    out->restored = out->ctrl3_after == ctrl3_orig && out->ctrl7_after == ctrl7_orig;

    if (err != ESP_OK) {
        out->gyro_why = "gyro enable write failed";
        return;
    }
    if (n == 0) {
        out->gyro_why = "no valid gyro sample";
        return;
    }
    out->gyro_ran = true;
    out->gyro_n = n;
    for (int a = 0; a < 3; ++a) { out->gmean[a] = sum[a] / (float)n; }
}

void jr_imu_probe(bool gyro, jr_probe_imu_t *out)
{
    memset(out, 0, sizeof(*out));
    out->gyro_asked = gyro;
    if (!s_ready || s_dev == NULL) {
        out->why = "imu not present";
        return;
    }
    if (imu_read_reg(QMI8658_REG_WHOAMI, &out->who) != ESP_OK ||
        imu_read_reg(QMI8658_REG_REVISION, &out->rev) != ESP_OK ||
        imu_read_reg(QMI8658_REG_CTRL1, &out->ctrl1) != ESP_OK ||
        imu_read_reg(QMI8658_REG_CTRL2, &out->ctrl2) != ESP_OK ||
        imu_read_reg(QMI8658_REG_CTRL3, &out->ctrl3) != ESP_OK ||
        imu_read_reg(QMI8658_REG_CTRL7, &out->ctrl7) != ESP_OK) {
        out->why = "i2c read failed";
        return;
    }
    out->ok = true;

    uint8_t reg = QMI8658_REG_TEMP_L;
    uint8_t t[2] = {0};
    if (i2c_master_transmit_receive(s_dev, &reg, 1, t, sizeof(t),
                                    QMI8658_I2C_TIMEOUT_MS) == ESP_OK) {
        const int16_t raw = (int16_t)(((uint16_t)t[1] << 8) | t[0]);
        out->temp_ok = true;
        out->temp_tenths = ((int)raw * 10) / 256;   /* 1/256 degree per LSB */
    }

    /* Wake-on-Motion leaves the sampler stopped and INT1 enabled in CTRL1
     * (QMI8658_CTRL1_WOM); the data configuration never sets that bit. */
    out->wom = (out->ctrl1 & 0x08u) != 0;
    if (!gyro) {
        return;
    }
    if (out->wom) {
        out->gyro_why = "refused: wake-on-motion armed";
    } else if (s_task == NULL) {
        out->gyro_why = "refused: sampler stopped";
    } else {
        probe_gyro(out);
    }
}
