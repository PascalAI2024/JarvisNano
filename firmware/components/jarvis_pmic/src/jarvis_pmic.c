/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 */
#include "jarvis_pmic.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define AXP2101_I2C_ADDR          0x34
#define AXP2101_REG_STATUS1       0x00
#define AXP2101_REG_STATUS2       0x01
#define AXP2101_REG_ADC_CH_CTRL   0x30
#define AXP2101_REG_VBAT_H        0x34  /* ADC_DATA_RELUST0 */
#define AXP2101_REG_VBAT_L        0x35  /* ADC_DATA_RELUST1 */
#define AXP2101_REG_BAT_DET_CTRL  0x68  /* battery presence detection */
#define AXP2101_REG_BAT_PERCENT   0xA4

#define AXP2101_STATUS1_VBUS_GOOD   (1u << 5)
#define AXP2101_STATUS1_BAT_PRESENT (1u << 3)
#define AXP2101_ADC_CH_BATT_VOLT    (1u << 0)
#define AXP2101_BAT_DET_ENABLE      (1u << 0)

#define AXP2101_I2C_TIMEOUT_MS  100

static const char *TAG = "jarvis_pmic";

static i2c_master_dev_handle_t s_dev;   /* AXP2101 device handle on the shared bus */
static bool s_ready;

static esp_err_t pmic_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1,
                                       pdMS_TO_TICKS(AXP2101_I2C_TIMEOUT_MS));
}

static esp_err_t pmic_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf),
                               pdMS_TO_TICKS(AXP2101_I2C_TIMEOUT_MS));
}

static esp_err_t pmic_lazy_init(void)
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
        ESP_LOGW(TAG, "add AXP2101 @0x%02X failed: %s", AXP2101_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    /* Enable the battery-voltage ADC channel. This is read-only side data — it
     * does NOT alter the factory rail sequencing the panel/codecs rely on. */
    uint8_t ch = 0;
    if (pmic_read_reg(AXP2101_REG_ADC_CH_CTRL, &ch) == ESP_OK &&
        !(ch & AXP2101_ADC_CH_BATT_VOLT)) {
        pmic_write_reg(AXP2101_REG_ADC_CH_CTRL, ch | AXP2101_ADC_CH_BATT_VOLT);
    }

    /* Enable battery-presence detection. Without this STATUS1[3] can read 0 even
     * with a cell attached, leaving percent/voltage permanently unreported. Like
     * the ADC enable above this is a detection bit only — no power rail is touched.
     * (XPowersLib enableBattDetection(): BAT_DET_CTRL 0x68 bit0.) */
    uint8_t det = 0;
    if (pmic_read_reg(AXP2101_REG_BAT_DET_CTRL, &det) == ESP_OK &&
        !(det & AXP2101_BAT_DET_ENABLE)) {
        pmic_write_reg(AXP2101_REG_BAT_DET_CTRL, det | AXP2101_BAT_DET_ENABLE);
    }

    s_ready = true;
    ESP_LOGI(TAG, "AXP2101 fuel gauge online (0x%02X)", AXP2101_I2C_ADDR);
    return ESP_OK;
}

esp_err_t jarvis_pmic_read_battery(jarvis_battery_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->percent = 0xFF;

    esp_err_t err = pmic_lazy_init();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t s1 = 0, s2 = 0;
    err = pmic_read_reg(AXP2101_REG_STATUS1, &s1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STATUS1 read failed: %s", esp_err_to_name(err));
        return err;
    }
    pmic_read_reg(AXP2101_REG_STATUS2, &s2);

    out->usb_present = (s1 & AXP2101_STATUS1_VBUS_GOOD) != 0;
    out->present     = (s1 & AXP2101_STATUS1_BAT_PRESENT) != 0;
    out->charging    = ((s2 >> 5) & 0x07u) == 0x01u;

    if (out->present) {
        uint8_t pct = 0;
        if (pmic_read_reg(AXP2101_REG_BAT_PERCENT, &pct) == ESP_OK && pct <= 100) {
            out->percent = pct;
        }
        uint8_t vh = 0, vl = 0;
        if (pmic_read_reg(AXP2101_REG_VBAT_H, &vh) == ESP_OK &&
            pmic_read_reg(AXP2101_REG_VBAT_L, &vl) == ESP_OK) {
            out->millivolts = (uint16_t)(((vh & 0x1Fu) << 8) | vl);
        }
    }

    return ESP_OK;
}
