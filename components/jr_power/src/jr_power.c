/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * See jr_power.h. Register access is verbatim from
 * firmware/components/jarvis_pmic/src/jarvis_pmic.c. No rail configuration;
 * the one write beyond telemetry is jr_power_off(), the PMIC's soft power-off.
 */
#include "jr_power/jr_power.h"

#include <stdatomic.h>
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

#define AXP2101_I2C_ADDR          0x34

/* PKEY IRQ lives in INTSTS2/INTEN2 (XPowersLib AXP2101 map): bit3 = short
 * press, bit2 = long press. On the 1.75C the old TCA9554 IRQ route is gone,
 * but the STATUS register still latches over plain I2C — so the power key is
 * readable by polling, no interrupt line needed. */
#define AXP2101_REG_INTEN2        0x41
#define AXP2101_REG_INTSTS2       0x49
#define AXP2101_PKEY_SHORT_BIT    (1u << 3)
#define AXP2101_PKEY_LONG_BIT     (1u << 2)
#define POWER_PKEY_POLL_MS        500
#define AXP2101_REG_STATUS1       0x00
#define AXP2101_REG_STATUS2       0x01
#define AXP2101_REG_ADC_CH_CTRL   0x30
#define AXP2101_REG_VBAT_H        0x34  /* ADC_DATA_RELUST0 */
#define AXP2101_REG_BAT_DET_CTRL  0x68  /* battery presence detection */
#define AXP2101_REG_BAT_PERCENT   0xA4
#define AXP2101_REG_COMMON_CFG    0x10  /* bit0: soft power-off (all rails)  */
#define AXP2101_REG_PWRON_CFG     0x27  /* bits1:0 ON hold: 0=128ms 1=512ms 2=1s 3=2s */
#define AXP2101_COMMON_PWROFF     0x01
#define AXP2101_ONLEVEL_MASK      0x03
#define AXP2101_ONLEVEL_1S        0x02

#define AXP2101_STATUS1_VBUS_GOOD   (1u << 5)
#define AXP2101_STATUS1_BAT_PRESENT (1u << 3)
#define AXP2101_ADC_CH_BATT_VOLT    (1u << 0)
#define AXP2101_BAT_DET_ENABLE      (1u << 0)

#define AXP2101_I2C_TIMEOUT_MS  100

/* USB/charging edges are visible product events; the official C-board example
 * samples at 1 Hz. This remains negligible I2C load while keeping feedback
 * under one second. */
#define POWER_SAMPLE_PERIOD_MS  1000
#define POWER_TASK_STACK        3072
#define POWER_TASK_PRIO         2

static _Atomic uint32_t s_pkey_short;
static _Atomic uint32_t s_pkey_long;

static const char *TAG = "jr_power";

static i2c_master_dev_handle_t s_dev;
static bool s_ready;
static TaskHandle_t s_task;
static volatile bool s_run;
/* True when the task stack came from PSRAM via xTaskCreateWithCaps — such a
 * task MUST be torn down with vTaskDeleteWithCaps or its stack leaks. */
static bool s_ext_stack;

static jr_power_t s_snap;
static int64_t    s_snap_us;
static SemaphoreHandle_t s_snap_lock;
static StaticSemaphore_t s_snap_lock_buf;

/* Constructor-created static mutex — ESP-IDF runs constructors single-threaded
 * before app_main(), so there is no creation race to bootstrap around. */
__attribute__((constructor))
static void power_lock_ctor(void)
{
    s_snap_lock = xSemaphoreCreateMutexStatic(&s_snap_lock_buf);
}

/* Burst read of `len` consecutive registers in ONE transaction; the AXP2101
 * auto-increments its register pointer on multi-byte reads.
 * NOTE: the new i2c_master API takes its timeout in MILLISECONDS
 * (xfer_timeout_ms), not ticks — wrapping it in pdMS_TO_TICKS() would silently
 * shrink 100 ms to 10 at CONFIG_FREERTOS_HZ=100. */
static esp_err_t pmic_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len,
                                       AXP2101_I2C_TIMEOUT_MS);
}

static esp_err_t pmic_read_reg(uint8_t reg, uint8_t *val)
{
    return pmic_read_regs(reg, val, 1);
}

static esp_err_t pmic_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), AXP2101_I2C_TIMEOUT_MS);
}

esp_err_t jr_power_off(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* First the way back: the PMIC's own power-on hold set to one second, so
     * a brush of the key in a bag does not boot it, a deliberate hold does.
     * The register lives in the PMIC's always-on domain and survives the
     * off state. Then the soft power-off: every rail, the ESP32 included. */
    uint8_t on = 0;
    esp_err_t err = pmic_read_reg(AXP2101_REG_PWRON_CFG, &on);
    if (err == ESP_OK) {
        on = (uint8_t)((on & ~AXP2101_ONLEVEL_MASK) | AXP2101_ONLEVEL_1S);
        err = pmic_write_reg(AXP2101_REG_PWRON_CFG, on);
    }
    if (err != ESP_OK) {
        return err;
    }
    uint8_t common = 0;
    err = pmic_read_reg(AXP2101_REG_COMMON_CFG, &common);
    if (err != ESP_OK) {
        return err;
    }
    return pmic_write_reg(AXP2101_REG_COMMON_CFG,
                          (uint8_t)(common | AXP2101_COMMON_PWROFF));
}

static esp_err_t power_bring_up(void)
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

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add AXP2101 @0x%02X failed: %s", AXP2101_I2C_ADDR,
                 esp_err_to_name(err));
        return err;
    }

    /* Enable the battery-voltage ADC channel. Read-only side data — this does
     * NOT alter the factory rail sequencing the panel/codecs rely on. */
    uint8_t ch = 0;
    if (pmic_read_reg(AXP2101_REG_ADC_CH_CTRL, &ch) == ESP_OK &&
        !(ch & AXP2101_ADC_CH_BATT_VOLT)) {
        pmic_write_reg(AXP2101_REG_ADC_CH_CTRL, ch | AXP2101_ADC_CH_BATT_VOLT);
    }

    /* Enable battery-presence detection. Without this STATUS1[3] can read 0 even
     * with a cell attached, leaving percent/voltage permanently unreported. Like
     * the ADC enable this is a detection bit only — no power rail is touched.
     * (XPowersLib enableBattDetection(): BAT_DET_CTRL 0x68 bit0.) */
    uint8_t det = 0;
    if (pmic_read_reg(AXP2101_REG_BAT_DET_CTRL, &det) == ESP_OK &&
        !(det & AXP2101_BAT_DET_ENABLE)) {
        pmic_write_reg(AXP2101_REG_BAT_DET_CTRL, det | AXP2101_BAT_DET_ENABLE);
    }

    /* Arm PKEY short/long IRQ latching and clear anything stale. */
    uint8_t inten2 = 0;
    if (pmic_read_reg(AXP2101_REG_INTEN2, &inten2) == ESP_OK) {
        (void)pmic_write_reg(AXP2101_REG_INTEN2,
                             inten2 | AXP2101_PKEY_SHORT_BIT |
                             AXP2101_PKEY_LONG_BIT);
    }
    (void)pmic_write_reg(AXP2101_REG_INTSTS2, 0xFF);

    s_ready = true;
    ESP_LOGI(TAG, "AXP2101 fuel gauge online (0x%02X)", AXP2101_I2C_ADDR);
    return ESP_OK;
}

static bool power_sample(jr_power_t *s)
{
    memset(s, 0, sizeof(*s));
    s->percent = 0xFF;

    uint8_t s1 = 0, s2 = 0;
    if (pmic_read_reg(AXP2101_REG_STATUS1, &s1) != ESP_OK) {
        return false;
    }
    pmic_read_reg(AXP2101_REG_STATUS2, &s2);

    s->usb_present = (s1 & AXP2101_STATUS1_VBUS_GOOD) != 0;
    s->present     = (s1 & AXP2101_STATUS1_BAT_PRESENT) != 0;
    s->charging    = ((s2 >> 5) & 0x07u) == 0x01u;

    if (s->present) {
        uint8_t pct = 0;
        if (pmic_read_reg(AXP2101_REG_BAT_PERCENT, &pct) == ESP_OK && pct <= 100) {
            s->percent = pct;
        }
        /* VBAT H/L (0x34/0x35) in ONE 2-byte transaction — two separate reads
         * can pair the high byte of one ADC sample with the low byte of the
         * next (torn reading). */
        uint8_t v[2] = { 0, 0 };
        if (pmic_read_regs(AXP2101_REG_VBAT_H, v, sizeof(v)) == ESP_OK) {
            s->millivolts = (uint16_t)(((v[0] & 0x1Fu) << 8) | v[1]);
        }
    }
    return true;
}

static void power_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0;

    uint32_t pass = 0;
    while (s_run) {
        /* PKEY every pass (a button must feel instant-ish); battery every
         * POWER_SAMPLE_PERIOD_MS as before. Write-1-to-clear consumes the
         * latch so each press counts once. */
        uint8_t sts2 = 0;
        if (pmic_read_reg(AXP2101_REG_INTSTS2, &sts2) == ESP_OK &&
            (sts2 & (AXP2101_PKEY_SHORT_BIT | AXP2101_PKEY_LONG_BIT)) != 0) {
            if (sts2 & AXP2101_PKEY_SHORT_BIT) {
                atomic_fetch_add(&s_pkey_short, 1u);
            }
            if (sts2 & AXP2101_PKEY_LONG_BIT) {
                atomic_fetch_add(&s_pkey_long, 1u);
            }
            (void)pmic_write_reg(AXP2101_REG_INTSTS2,
                                 sts2 & (AXP2101_PKEY_SHORT_BIT |
                                         AXP2101_PKEY_LONG_BIT));
        }
        if ((pass++ % (POWER_SAMPLE_PERIOD_MS / POWER_PKEY_POLL_MS)) == 0) {
            jr_power_t s;
            if (power_sample(&s)) {
                s.sample_seq = ++seq;
                if (xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                    s_snap = s;
                    s_snap_us = esp_timer_get_time();
                    xSemaphoreGive(s_snap_lock);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POWER_PKEY_POLL_MS));
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

esp_err_t jr_power_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    esp_err_t err = power_bring_up();
    if (err != ESP_OK) {
        return err;
    }
    s_run = true;
    /* PSRAM stack — internal RAM is the scarcest resource on this board and this
     * task never touches flash. Same guarded pattern as jr_tools/jr_imu, with an
     * internal-stack fallback. */
    BaseType_t ok = pdFAIL;
#if defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && defined(CONFIG_SPIRAM) && \
    CONFIG_SPIRAM
    ok = xTaskCreateWithCaps(power_task, "jr_power", POWER_TASK_STACK, NULL,
                             POWER_TASK_PRIO, &s_task,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ext_stack = (ok == pdPASS);
    if (ok != pdPASS) {
        s_task = NULL;
    }
#endif
    if (ok != pdPASS) {
        ok = xTaskCreate(power_task, "jr_power", POWER_TASK_STACK, NULL,
                         POWER_TASK_PRIO, &s_task);
    }
    if (ok != pdPASS) {
        s_run = false;
        ESP_LOGE(TAG, "sampler task creation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "battery sampler running (%d ms period)", POWER_SAMPLE_PERIOD_MS);
    return ESP_OK;
}

void jr_power_stop(void)
{
    s_run = false;
}

esp_err_t jr_power_read(jr_power_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->percent = 0xFF;
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

/* Consume latched power-key presses (counts since last call). Any task. */
void jr_power_pkey_take(uint32_t *out_short, uint32_t *out_long)
{
    uint32_t sp = atomic_exchange(&s_pkey_short, 0u);
    uint32_t lp = atomic_exchange(&s_pkey_long, 0u);
    if (out_short) { *out_short = sp; }
    if (out_long)  { *out_long  = lp; }
}
