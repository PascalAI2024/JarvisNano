/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Waveshare ESP32-S3-Touch-AMOLED-1.75 — board setup.
 *
 * Phase 1: display (CO5300 QSPI) + touch (CST9217 I2C). Audio (ES8311+ES7210)
 * and PMIC active management are phase 2.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_io_expander_tca9554.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "WS_AMOLED_1_75_SETUP";

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle,
                                      const uint16_t dev_addr,
                                      esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_handle, dev_addr, handle_ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_io_expander_new_i2c_tca9554 failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* CO5300 init sequence — vendor magic numbers from Waveshare's official BSP
 * (waveshareteam/Waveshare-ESP32-components/bsp/esp32_s3_touch_amoled_1_75).
 * Page command 0xFE selects register banks; 0x2A/0x2B set the column/row
 * windows for the 466x466 visible area (note the +6 column offset, applied
 * via esp_lcd_panel_set_gap below). */
static const co5300_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

static const co5300_vendor_config_t vendor_config = {
    .init_cmds = lcd_init_cmds,
    .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
    .flags = {
        .use_qspi_interface = 1,
    },
};

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));
    panel_dev_cfg.vendor_config = (void *)&vendor_config;

    esp_err_t ret = esp_lcd_new_panel_co5300(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_co5300 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Column gap: visible area starts at col 6 (vendor BSP applies the same offset). */
    esp_lcd_panel_set_gap(*ret_panel, 0x06, 0);
    return ESP_OK;
}

/* CST9217 is interrupt-driven — chip sleeps when idle and won't ACK polled I2C
 * reads. The board manager passes our callback into esp_lcd_touch_config_t. */
static IRAM_ATTR void cst9217_interrupt_cb(esp_lcd_touch_handle_t tp)
{
    SemaphoreHandle_t sem = (tp != NULL) ? (SemaphoreHandle_t)tp->config.user_data : NULL;
    if (sem == NULL) {
        return;
    }
    BaseType_t higher_prio_woken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &higher_prio_woken);
    if (higher_prio_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    static SemaphoreHandle_t s_cst9217_int_sem = NULL;
    esp_lcd_touch_config_t touch_cfg = {0};
    memcpy(&touch_cfg, touch_dev_config, sizeof(esp_lcd_touch_config_t));

    if (s_cst9217_int_sem == NULL) {
        s_cst9217_int_sem = xSemaphoreCreateBinary();
    }
    touch_cfg.interrupt_callback = cst9217_interrupt_cb;
    touch_cfg.user_data = (void *)s_cst9217_int_sem;

    esp_err_t ret = esp_lcd_touch_new_i2c_cst9217(io, &touch_cfg, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_cst9217 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
