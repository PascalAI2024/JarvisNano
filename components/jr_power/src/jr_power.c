/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * See jr_power.h. Register access is verbatim from
 * firmware/components/jarvis_pmic/src/jarvis_pmic.c. No charge/rail configuration;
 * existing detection, PKEY IRQ setup/ack and explicit soft power-off writes
 * remain. The fixed charge diagnostic adds reads only.
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
#define AXP2101_REG_STATUS1          0x00
#define AXP2101_REG_STATUS2          0x01
#define AXP2101_REG_COMMON_CFG       0x10  /* bit0: soft power-off (all rails)  */
#define AXP2101_REG_CHARGE_GAUGE_WDT 0x18  /* bit3=gauge_en, bit1=cell_chg_en   */
#define AXP2101_REG_PWRON_CFG        0x27  /* bits1:0 ON hold: 0=128ms 1=512ms 2=1s 3=2s */
#define AXP2101_REG_ADC_CH_CTRL      0x30
#define AXP2101_REG_VBAT_H           0x34  /* ADC_DATA_RELUST0 (0x34..0x3D)     */
#define AXP2101_ADC_DATA_FIRST       0x34  /* VBAT H5L8, TS H6L8, VBUS, VSYS, Tdie */
#define AXP2101_REG_CC_SETTING       0x62  /* bits[4:0] ICC charge current      */
#define AXP2101_REG_CV_SETTING       0x64  /* bits[2:0] CV: 1=4.0V, 3=4.2V      */
#define AXP2101_REG_THERMAL_THRES    0x65  /* bits[1:0]: 0=60C 1=80C 2=100C 3=120C */
#define AXP2101_REG_BAT_DET_CTRL     0x68  /* battery presence detection        */
#define AXP2101_REG_BAT_PERCENT      0xA4
#define AXP2101_COMMON_PWROFF        0x01
#define AXP2101_ONLEVEL_MASK         0x03
#define AXP2101_ONLEVEL_1S           0x02

#define AXP2101_STATUS1_VBUS_GOOD    (1u << 5)
#define AXP2101_STATUS1_BAT_PRESENT  (1u << 3)
#define AXP2101_GAUGE_EN_BIT         (1u << 3)
#define AXP2101_CELL_CHG_EN_BIT      (1u << 1)
#define AXP2101_ADC_CH_BATT_VOLT     (1u << 0)
#define AXP2101_ADC_CH_TS            (1u << 1)
#define AXP2101_ADC_CH_VBUS          (1u << 2)
#define AXP2101_ADC_CH_VSYS          (1u << 3)
#define AXP2101_ADC_CH_TDIE          (1u << 4)
/* Continuous ADC mask: VBAT|VBUS|VSYS|TDIE = 0x1D (TS bit1 MUST remain 0!). */
#define AXP2101_ADC_CH_CONTINUOUS    (AXP2101_ADC_CH_BATT_VOLT | \
                                      AXP2101_ADC_CH_VBUS | \
                                      AXP2101_ADC_CH_VSYS | \
                                      AXP2101_ADC_CH_TDIE)
#define AXP2101_BAT_DET_ENABLE       (1u << 0)
#define AXP2101_ICC_FALLBACK_CODE    8u    /* 200 mA safe ceiling if read fails */
#define AXP2101_ICC_MIN_THROTTLE     4u    /* 100 mA floor for thermal step-down */
#define AXP2101_DIE_TEMP_INVALID     ((int8_t)-128)

#define AXP2101_I2C_TIMEOUT_MS  100
/* At most ONE diagnostic burst after PKEY/battery per pass. Do not add seven
 * 100 ms timeouts to the power-key polling interval on a broken bus. */
#define POWER_CHARGE_TIMEOUT_MS 20
#define POWER_CHARGE_PERIOD_MS  5000

/* USB/charging edges are visible product events; the official C-board example
 * samples at 1 Hz. This remains negligible I2C load while keeping feedback
 * under one second. */
#define POWER_SAMPLE_PERIOD_MS  1000
#define POWER_TASK_STACK        3072
#define POWER_TASK_PRIO         2

static _Atomic uint32_t s_pkey_short;
static _Atomic uint32_t s_pkey_long;
static _Atomic uint32_t s_pmic_i2c_errors;
static _Atomic bool     s_care_enabled_atomic;

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
static int8_t     s_adc_die_c = AXP2101_DIE_TEMP_INVALID;
static uint16_t   s_adc_vbus_mv;
static uint16_t   s_adc_vsys_mv;

/* Care & thermal control state (serialized by s_care_i2c_lock; published to
 * s_care_pub under s_snap_lock so readers never block on I2C). */
static bool     s_care_enabled;
static bool     s_care_cv_pending;
static bool     s_care_holding;
static bool     s_thermal_throttle;
static bool     s_thermal_pause;
static bool     s_active_chg_en = true;
static uint8_t  s_factory_icc_code = AXP2101_ICC_FALLBACK_CODE;
static uint8_t  s_active_icc_code  = AXP2101_ICC_FALLBACK_CODE;
static uint16_t s_active_cv_mv     = 4200u;

static jr_power_care_status_t s_care_pub = {
    .cv_mv          = 4200u,
    .icc_ma         = 200u,
    .factory_icc_ma = 200u,
    .pmic_die_c     = AXP2101_DIE_TEMP_INVALID,
};

static jr_power_charge_t *s_charge_snap;
static jr_power_charge_t *s_charge_work; /* sampler-owned; no I2C under cache lock */
static bool s_charge_active, s_charge_pending;
static size_t s_charge_next_burst;
static uint64_t s_charge_next_start_ms;
static SemaphoreHandle_t s_snap_lock;
static StaticSemaphore_t s_snap_lock_buf;
static SemaphoreHandle_t s_care_i2c_lock;
static StaticSemaphore_t s_care_i2c_lock_buf;

/* Constructor-created static mutexes — ESP-IDF runs constructors single-threaded
 * before app_main(), so there is no creation race to bootstrap around.
 * Lock order: s_care_i2c_lock -> s_snap_lock (never reversed; no I2C under
 * s_snap_lock). */
__attribute__((constructor))
static void power_lock_ctor(void)
{
    s_snap_lock = xSemaphoreCreateMutexStatic(&s_snap_lock_buf);
    s_care_i2c_lock = xSemaphoreCreateMutexStatic(&s_care_i2c_lock_buf);
}

/* Allocate the two charge-diagnostic structs (~1,360 B total) once in PSRAM
 * rather than pinning them in internal .dram0.bss. Safe fallback to general
 * 8-bit heap if PSRAM is unavailable. */
static void power_charge_ensure_alloc(void)
{
    if (s_charge_snap != NULL && s_charge_work != NULL) {
        return;
    }
    jr_power_charge_t *snap = s_charge_snap;
    jr_power_charge_t *work = s_charge_work;
    if (snap == NULL) {
        snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (snap == NULL) {
            snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_8BIT);
        }
        if (snap != NULL) {
            jr_power_charge_reset(snap);
        }
    }
    if (work == NULL) {
        work = heap_caps_malloc(sizeof(*work), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (work == NULL) {
            work = heap_caps_malloc(sizeof(*work), MALLOC_CAP_8BIT);
        }
        if (work != NULL) {
            jr_power_charge_reset(work);
        }
    }
    if (s_snap_lock != NULL &&
        xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_charge_snap = snap;
        s_charge_work = work;
        xSemaphoreGive(s_snap_lock);
    } else {
        s_charge_snap = snap;
        s_charge_work = work;
    }
}

static uint32_t adc14(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x3Fu) << 8) | p[1];
}

static uint16_t pmic_icc_code_to_ma(uint8_t code)
{
    code &= 0x1Fu;
    if (code <= 8u) {
        return (uint16_t)(25u * code);
    }
    if (code <= 21u) {
        return (uint16_t)(200u + 100u * (code - 8u));
    }
    return 0u;
}

static uint16_t pmic_cv_code_to_mv(uint8_t code)
{
    static const uint16_t k_volts[] = { 0u, 4000u, 4100u, 4200u, 4350u, 4400u };
    code &= 0x07u;
    if (code >= 1u && code <= 5u) {
        return k_volts[code];
    }
    return 4200u;
}

/* Burst read of `len` consecutive registers in ONE transaction; the AXP2101
 * auto-increments its register pointer on multi-byte reads.
 * NOTE: the new i2c_master API takes its timeout in MILLISECONDS
 * (xfer_timeout_ms), not ticks — wrapping it in pdMS_TO_TICKS() would silently
 * shrink 100 ms to 10 at CONFIG_FREERTOS_HZ=100. */
static esp_err_t pmic_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    const esp_err_t err = i2c_master_transmit_receive(
        s_dev, &reg, 1, buf, len, AXP2101_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        atomic_fetch_add(&s_pmic_i2c_errors, 1u);
    }
    return err;
}

static esp_err_t pmic_read_reg(uint8_t reg, uint8_t *val)
{
    return pmic_read_regs(reg, val, 1);
}

static esp_err_t pmic_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    const esp_err_t err = i2c_master_transmit(
        s_dev, buf, sizeof(buf), AXP2101_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        atomic_fetch_add(&s_pmic_i2c_errors, 1u);
    }
    return err;
}

/* RMW 0x18 bit1 (cell_chg_en), ALWAYS preserving 0xFD and forcing bit3
 * (gauge_en = 1). Caller holds s_care_i2c_lock. */
static esp_err_t pmic_write_chg_en(bool enable)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t cur = 0;
    esp_err_t err = pmic_read_reg(AXP2101_REG_CHARGE_GAUGE_WDT, &cur);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t want = (uint8_t)((cur & 0xFDu) | AXP2101_GAUGE_EN_BIT);
    if (enable) {
        want |= AXP2101_CELL_CHG_EN_BIT;
    }
    if (want == cur) {
        return ESP_OK;
    }
    return pmic_write_reg(AXP2101_REG_CHARGE_GAUGE_WDT, want);
}

/* RMW 0x64 bits[2:0] (CV target: 1=4.0V Desk Care, 3=4.2V Full), preserving
 * 0xF8. Caller holds s_care_i2c_lock. */
static esp_err_t pmic_write_cv(bool care_enable)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t cur = 0;
    esp_err_t err = pmic_read_reg(AXP2101_REG_CV_SETTING, &cur);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t code = care_enable ? 1u : 3u;
    const uint8_t want = (uint8_t)((cur & 0xF8u) | code);
    if (want == cur) {
        return ESP_OK;
    }
    return pmic_write_reg(AXP2101_REG_CV_SETTING, want);
}

/* RMW 0x62 bits[4:0] (ICC target), preserving 0xE0 and NEVER exceeding the
 * latched factory boot ceiling s_factory_icc_code. Caller holds s_care_i2c_lock. */
static esp_err_t pmic_write_icc(uint8_t code)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    code &= 0x1Fu;
    if (code > s_factory_icc_code) {
        code = s_factory_icc_code;
    }
    uint8_t cur = 0;
    esp_err_t err = pmic_read_reg(AXP2101_REG_CC_SETTING, &cur);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t want = (uint8_t)((cur & 0xE0u) | code);
    if (want == cur) {
        return ESP_OK;
    }
    return pmic_write_reg(AXP2101_REG_CC_SETTING, want);
}

/* Publish care/thermal fields into s_care_pub without holding s_snap_lock over
 * any I2C. Caller holds s_care_i2c_lock. */
static void power_care_publish_locked(void)
{
    if (s_snap_lock != NULL &&
        xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_care_pub.enabled          = s_care_enabled;
        s_care_pub.holding          = s_care_holding;
        s_care_pub.thermal_throttle = s_thermal_throttle;
        s_care_pub.thermal_pause    = s_thermal_pause;
        s_care_pub.cv_mv            = s_active_cv_mv;
        s_care_pub.icc_ma           = pmic_icc_code_to_ma(s_active_icc_code);
        s_care_pub.factory_icc_ma   = pmic_icc_code_to_ma(s_factory_icc_code);
        xSemaphoreGive(s_snap_lock);
    }
}

uint32_t jr_power_i2c_errors(void)
{
    return atomic_load(&s_pmic_i2c_errors);
}

esp_err_t jr_power_boot_status(uint8_t *on_src, uint8_t *off_src)
{
    if (s_dev == NULL || on_src == NULL || off_src == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* AXP2101 0x20 = power-on source, 0x21 = power-off source: what the PMIC
     * itself recorded for the last time the rails came up and went down. */
    uint8_t v[2] = {0};
    esp_err_t err = pmic_read_regs(0x20, v, sizeof v);
    if (err == ESP_OK) {
        *on_src = v[0];
        *off_src = v[1];
    }
    return err;
}

esp_err_t jr_power_off(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Fail-safe: ALWAYS restore 0x18 bit1 (cell_chg_en = 1) and bit3
     * (gauge_en = 1) before soft power-off so charging keeps working while
     * powered off (0x64 = 4.0 V remains active as HW cap if Desk Care is on). */
    const bool locked = (s_care_i2c_lock != NULL) &&
                        (xSemaphoreTake(s_care_i2c_lock, pdMS_TO_TICKS(100)) == pdTRUE);
    const esp_err_t chg_err = pmic_write_chg_en(true);
    if (chg_err == ESP_OK) {
        s_active_chg_en = true;
    } else {
        ESP_LOGW(TAG, "power_off: restore cell_chg_en failed: %s",
                 esp_err_to_name(chg_err));
    }
    if (locked) {
        xSemaphoreGive(s_care_i2c_lock);
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

    /* Enable continuous ADC channels 0x30 = 0x1D (VBAT|VBUS|VSYS|TDIE) while
     * explicitly keeping TS bit1 = 0 (no NTC on board). Touches ZERO rails. */
    uint8_t ch = 0;
    if (pmic_read_reg(AXP2101_REG_ADC_CH_CTRL, &ch) == ESP_OK) {
        const uint8_t want_ch = (uint8_t)((ch & (uint8_t)~AXP2101_ADC_CH_TS) |
                                          AXP2101_ADC_CH_CONTINUOUS);
        if (want_ch != ch) {
            (void)pmic_write_reg(AXP2101_REG_ADC_CH_CTRL, want_ch);
        }
    } else {
        (void)pmic_write_reg(AXP2101_REG_ADC_CH_CTRL, AXP2101_ADC_CH_CONTINUOUS);
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

    /* Latch factory 0x62 ICC ceiling (never exceed), set HW thermal foldback
     * 0x65 bits[1:0] = 1 (80 C, RMW preserving 0xFC), and sync CV/CHG_EN state.
     * Touches ZERO DCDC/LDO rail registers (0x80..0x9A). */
    const bool locked = (s_care_i2c_lock != NULL) &&
                        (xSemaphoreTake(s_care_i2c_lock, pdMS_TO_TICKS(100)) == pdTRUE);

    uint8_t cc_raw = 0;
    if (pmic_read_reg(AXP2101_REG_CC_SETTING, &cc_raw) == ESP_OK &&
        (cc_raw & 0x1Fu) > 0u && (cc_raw & 0x1Fu) <= 21u) {
        s_factory_icc_code = (uint8_t)(cc_raw & 0x1Fu);
    } else {
        s_factory_icc_code = AXP2101_ICC_FALLBACK_CODE;
    }
    s_active_icc_code = s_factory_icc_code;

    uint8_t th = 0;
    if (pmic_read_reg(AXP2101_REG_THERMAL_THRES, &th) == ESP_OK) {
        const uint8_t want_th = (uint8_t)((th & 0xFCu) | 0x01u); /* 80 C */
        if (want_th != th) {
            (void)pmic_write_reg(AXP2101_REG_THERMAL_THRES, want_th);
        }
    }

    if (s_care_cv_pending) {
        if (pmic_write_cv(s_care_enabled) == ESP_OK) {
            s_active_cv_mv = s_care_enabled ? 4000u : 4200u;
            s_care_cv_pending = false;
        }
    } else {
        uint8_t cv_raw = 0;
        if (pmic_read_reg(AXP2101_REG_CV_SETTING, &cv_raw) == ESP_OK) {
            s_active_cv_mv = pmic_cv_code_to_mv(cv_raw);
        }
    }

    uint8_t chg_ctrl = 0;
    if (pmic_read_reg(AXP2101_REG_CHARGE_GAUGE_WDT, &chg_ctrl) == ESP_OK) {
        s_active_chg_en = (chg_ctrl & AXP2101_CELL_CHG_EN_BIT) != 0u;
        if ((chg_ctrl & AXP2101_GAUGE_EN_BIT) == 0u) {
            (void)pmic_write_reg(AXP2101_REG_CHARGE_GAUGE_WDT,
                                 (uint8_t)(chg_ctrl | AXP2101_GAUGE_EN_BIT));
        }
    } else {
        s_active_chg_en = true;
    }

    power_care_publish_locked();
    if (locked) {
        xSemaphoreGive(s_care_i2c_lock);
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
    ESP_LOGI(TAG, "AXP2101 online (0x%02X, factory ICC=%u mA, CV=%u mV, thermal=80C)",
             AXP2101_I2C_ADDR, (unsigned)pmic_icc_code_to_ma(s_factory_icc_code),
             (unsigned)s_active_cv_mv);
    return ESP_OK;
}

static bool power_sample(jr_power_t *s, bool *out_adc_ok,
                         int8_t *out_die_c, uint16_t *out_vbus_mv,
                         uint16_t *out_vsys_mv)
{
    memset(s, 0, sizeof(*s));
    s->percent = 0xFF;
    *out_adc_ok = false;
    *out_die_c = AXP2101_DIE_TEMP_INVALID;
    *out_vbus_mv = 0u;
    *out_vsys_mv = 0u;

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
    }

    /* 10-byte burst 0x34..0x3D in ONE transaction: VBAT H5L8, TS (ignored),
     * VBUS H6L8, VSYS H6L8, Tdie H6L8 — prevents torn high/low byte reads
     * and adds zero extra I2C transactions over the old 2-byte VBAT read. */
    uint8_t d[10] = {0};
    if (pmic_read_regs(AXP2101_ADC_DATA_FIRST, d, sizeof(d)) == ESP_OK) {
        *out_adc_ok = true;
        if (s->present) {
            s->millivolts = (uint16_t)(((d[0] & 0x1Fu) << 8) | d[1]);
        }
        *out_vbus_mv = (uint16_t)adc14(&d[4]);
        *out_vsys_mv = (uint16_t)adc14(&d[6]);
        const uint32_t die_raw = adc14(&d[8]);
        if (die_raw != 0u) {
            /* XPowersLib: 22 + (7274 - raw) / 20 degrees C. */
            const int die_c = 22 + (7274 - (int)die_raw) / 20;
            if (die_c >= -40 && die_c <= 125) {
                *out_die_c = (int8_t)die_c;
            }
        }
    }
    return true;
}

static bool power_charge_publish(void)
{
    if (s_charge_snap == NULL || s_charge_work == NULL) { return false; }
    if (xSemaphoreTake(s_snap_lock, 0) != pdTRUE) { return false; }
    *s_charge_snap = *s_charge_work;
    xSemaphoreGive(s_snap_lock);
    s_charge_pending = false;
    return true;
}

static void power_charge_step(void)
{
    if (s_charge_snap == NULL || s_charge_work == NULL) { return; }
    /* Retry a contended publication without reading or waiting on the bus.
     * The stored completion/read times survive publication delay unchanged. */
    if (s_charge_pending && !power_charge_publish()) { return; }
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000u;
    if (!s_charge_active) {
        if (now_ms < s_charge_next_start_ms) { return; }
        jr_power_charge_begin(s_charge_work, now_ms);
        s_charge_active = true;
        s_charge_next_burst = 0;
        s_charge_next_start_ms = now_ms + POWER_CHARGE_PERIOD_MS;
    }
    const jr_power_charge_burst_t *burst =
        jr_power_charge_burst(s_charge_next_burst);
    uint8_t values[5] = {0}; /* largest fixed burst; no allocation/request input */
    const esp_err_t err = i2c_master_transmit_receive(s_dev, &burst->address, 1,
                         values, burst->count, POWER_CHARGE_TIMEOUT_MS);
    if (err != ESP_OK) {
        atomic_fetch_add(&s_pmic_i2c_errors, 1u);
    }
    now_ms = (uint64_t)esp_timer_get_time() / 1000u;
    for (size_t i = 0; i < burst->count; ++i) {
        jr_power_charge_record(s_charge_work, burst->first_index + i,
                               values[i], err, now_ms);
    }
    if (++s_charge_next_burst == JR_POWER_CHARGE_BURST_COUNT) {
        jr_power_charge_finish(s_charge_work, now_ms);
        s_charge_active = false;
        s_charge_pending = true;
        (void)power_charge_publish();
    }
}

static void power_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0;
    /* A stop may interrupt a batch. Start a fresh one on restart; never
     * publish that partial attempt as a completed register snapshot. */
    s_charge_active = s_charge_pending = false;
    s_charge_next_start_ms = 0;

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
            bool adc_ok = false;
            int8_t die_c = AXP2101_DIE_TEMP_INVALID;
            uint16_t vbus_mv = 0u;
            uint16_t vsys_mv = 0u;
            if (power_sample(&s, &adc_ok, &die_c, &vbus_mv, &vsys_mv)) {
                s.sample_seq = ++seq;
                if (xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                    s_snap = s;
                    s_snap_us = esp_timer_get_time();
                    if (adc_ok) {
                        s_adc_die_c = die_c;
                        s_adc_vbus_mv = vbus_mv;
                        s_adc_vsys_mv = vsys_mv;
                    }
                    xSemaphoreGive(s_snap_lock);
                }
            }
        }
        power_charge_step();
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
    power_charge_ensure_alloc();
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

esp_err_t jr_power_charge_read(jr_power_charge_t *out)
{
    if (!out) { return ESP_ERR_INVALID_ARG; }
    jr_power_charge_reset(out);
    if (!s_snap_lock) { return ESP_ERR_INVALID_STATE; }
    if (xSemaphoreTake(s_snap_lock, 0) != pdTRUE) { return ESP_ERR_TIMEOUT; }
    const bool have = s_charge_snap != NULL && s_charge_snap->complete;
    if (have) { *out = *s_charge_snap; }
    xSemaphoreGive(s_snap_lock);
    if (have) {
        jr_power_charge_age(out, (uint64_t)esp_timer_get_time() / 1000u);
    }
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

/* ---- Desk Battery-Care (80% cap) & Thermal Charge Throttling ------------ */

esp_err_t jr_power_set_battery_care(bool enable)
{
    if (s_care_i2c_lock == NULL ||
        xSemaphoreTake(s_care_i2c_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_ready || s_dev == NULL) {
        s_care_enabled = enable;
        s_care_cv_pending = true;
        if (!enable) {
            s_care_holding = false;
        }
        atomic_store(&s_care_enabled_atomic, enable);
        power_care_publish_locked();
        xSemaphoreGive(s_care_i2c_lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = pmic_write_cv(enable);
    if (err == ESP_OK) {
        const bool was_holding = s_care_holding;
        s_care_enabled = enable;
        s_care_cv_pending = false;
        s_active_cv_mv = enable ? 4000u : 4200u;
        atomic_store(&s_care_enabled_atomic, enable);
        if (!enable && was_holding) {
            s_care_holding = false;
            if (!s_thermal_pause) {
                const esp_err_t chg_err = pmic_write_chg_en(true);
                if (chg_err == ESP_OK) {
                    s_active_chg_en = true;
                } else {
                    err = chg_err;
                }
            }
        }
        power_care_publish_locked();
        ESP_LOGI(TAG, "battery care: %s (CV=%u mV)",
                 enable ? "DESK 80%" : "TRAVEL 100%", (unsigned)s_active_cv_mv);
    }

    xSemaphoreGive(s_care_i2c_lock);
    return err;
}

bool jr_power_get_battery_care(void)
{
    return atomic_load(&s_care_enabled_atomic);
}

void jr_power_care_tick(bool usb_present, int pct, int chip_temp_c)
{
    int8_t pmic_die_c = AXP2101_DIE_TEMP_INVALID;
    if (s_snap_lock != NULL &&
        xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        pmic_die_c = s_adc_die_c;
        xSemaphoreGive(s_snap_lock);
    }

    if (s_care_i2c_lock == NULL ||
        xSemaphoreTake(s_care_i2c_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    /* Dual-sensor thermal envelope: max(chip_temp_c, pmic_die_c if valid). */
    int hot_c = chip_temp_c;
    if (pmic_die_c != AXP2101_DIE_TEMP_INVALID && pmic_die_c > hot_c) {
        hot_c = (int)pmic_die_c;
    }

    /* Thermal hysteresis: pause >=65C (clear <60C), throttle >=55C (clear <50C). */
    if (hot_c >= 65) {
        s_thermal_pause = true;
    } else if (hot_c < 60) {
        s_thermal_pause = false;
    }
    if (hot_c >= 55) {
        s_thermal_throttle = true;
    } else if (hot_c < 50) {
        s_thermal_throttle = false;
    }

    /* Target 0x62 ICC code: step down to max(4, factory/2), NEVER > factory. */
    uint8_t target_icc = s_factory_icc_code;
    if (s_thermal_throttle) {
        const uint8_t half = (uint8_t)(s_factory_icc_code / 2u);
        target_icc = half > AXP2101_ICC_MIN_THROTTLE
                         ? half
                         : AXP2101_ICC_MIN_THROTTLE;
        if (target_icc > s_factory_icc_code) {
            target_icc = s_factory_icc_code;
        }
    }
    if (s_ready && s_dev != NULL && target_icc != s_active_icc_code) {
        if (pmic_write_icc(target_icc) == ESP_OK) {
            s_active_icc_code = target_icc;
            ESP_LOGI(TAG, "thermal ICC -> %u mA (hot=%dC, throttle=%d)",
                     (unsigned)pmic_icc_code_to_ma(target_icc),
                     hot_c, (int)s_thermal_throttle);
        }
    }

    /* Desk Care 75/80% hysteresis on USB; always clears off USB or care off. */
    if (s_care_enabled && usb_present) {
        if (pct >= 0 && pct <= 100) {
            if (pct >= 80) {
                s_care_holding = true;
            } else if (pct <= 75) {
                s_care_holding = false;
            }
        }
    } else {
        s_care_holding = false;
    }

    /* Desired 0x18 bit1 (cell_chg_en): 0 ONLY when usb && (holding || pause). */
    const bool want_chg_en = !(usb_present && (s_care_holding || s_thermal_pause));
    if (s_ready && s_dev != NULL && want_chg_en != s_active_chg_en) {
        if (pmic_write_chg_en(want_chg_en) == ESP_OK) {
            s_active_chg_en = want_chg_en;
            ESP_LOGI(TAG, "cell_chg_en -> %d (usb=%d pct=%d hold=%d tpause=%d)",
                     (int)want_chg_en, (int)usb_present, pct,
                     (int)s_care_holding, (int)s_thermal_pause);
        }
    }

    power_care_publish_locked();
    xSemaphoreGive(s_care_i2c_lock);
}

void jr_power_care_status(jr_power_care_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->pmic_die_c = AXP2101_DIE_TEMP_INVALID;
    if (s_snap_lock == NULL ||
        xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    *out = s_care_pub;
    out->pmic_die_c = s_adc_die_c;
    out->vbus_mv = s_adc_vbus_mv;
    out->vsys_mv = s_adc_vsys_mv;
    xSemaphoreGive(s_snap_lock);
}

/* ---- board probe (GET /api/diag/board) ---------------------------------- */

#define AXP2101_ADC_CH_PROBE_MASK 0x1F  /* VBAT, TS, VBUS, VSYS, die temp */
/* 100 ms was not enough: on 2026-11-10 VSYS and Tdie read raw 0 that soon
 * after their channels were enabled (reported as 0 mV and 385.7 C). */
#define PROBE_ADC_SETTLE_MS       500

void jr_power_probe(jr_probe_pmic_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_ready || s_dev == NULL) {
        out->why = "pmic not present";
        return;
    }
    uint8_t adc = 0, cc = 0;
    if (pmic_read_reg(AXP2101_REG_ADC_CH_CTRL, &adc) != ESP_OK) {
        out->why = "i2c read failed";
        return;
    }
    out->adc_before = adc;
    if (pmic_read_reg(AXP2101_REG_CC_SETTING, &cc) == ESP_OK) {
        out->chg_cc_raw = cc;
        const uint8_t code = cc & 31u;   /* same decode as charge.c */
        if (code <= 21) {
            out->chg_cc_known = true;
            out->chg_cc_ma = pmic_icc_code_to_ma(code);
        }
    }

    const uint8_t want = (uint8_t)(adc | AXP2101_ADC_CH_PROBE_MASK);
    bool wrote = false;
    if (want != adc) {
        if (pmic_write_reg(AXP2101_REG_ADC_CH_CTRL, want) != ESP_OK) {
            out->why = "adc enable write failed";
            return;
        }
        wrote = true;
        vTaskDelay(pdMS_TO_TICKS(PROBE_ADC_SETTLE_MS));
    }
    (void)pmic_read_reg(AXP2101_REG_ADC_CH_CTRL, &out->adc_during);

    uint8_t d[10] = {0};   /* 0x34..0x3D */
    const esp_err_t rerr = pmic_read_regs(AXP2101_ADC_DATA_FIRST, d, sizeof(d));

    if (wrote) {
        (void)pmic_write_reg(AXP2101_REG_ADC_CH_CTRL, adc);
    }
    (void)pmic_read_reg(AXP2101_REG_ADC_CH_CTRL, &out->adc_after);
    out->adc_restored = out->adc_after == adc;

    if (rerr != ESP_OK) {
        out->why = "adc data read failed";
        return;
    }
    out->ok = true;
    const uint8_t on = out->adc_during;
    out->vbat_ok = (on & 0x01u) != 0;
    out->vbat_mv = ((uint32_t)(d[0] & 0x1Fu) << 8) | d[1];   /* H5L8 */
    out->ts_ok   = (on & 0x02u) != 0;
    out->ts_raw  = adc14(&d[2]);
    out->vbus_ok = (on & 0x04u) != 0;
    out->vbus_mv = adc14(&d[4]);
    /* A raw 0 on VSYS or Tdie is a channel that has not converted yet, not a
     * rail at 0 mV or a die at 385 C: the board is running, so neither can be. */
    out->vsys_mv = adc14(&d[6]);
    out->vsys_ok = (on & 0x08u) != 0 && out->vsys_mv != 0;
    const uint32_t die_raw = adc14(&d[8]);
    out->die_ok  = (on & 0x10u) != 0 && die_raw != 0;
    /* XPowersLib: 22 + (7274 - raw) / 20 degrees C. */
    out->die_tenths = 220 + (7274 - (int)die_raw) / 2;
}
