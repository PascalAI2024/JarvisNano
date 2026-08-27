/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C — board setup.
 *
 * Board-manager owns shared I2C devices here. The CO5300 display is owned by
 * jarvis_board via the Waveshare BSP so QSPI is not double-initialized.
 * The 1.75C has no TCA9554 IO expander, so there is no io_expander factory.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_lcd_touch_cst9217.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "WS_AMOLED_1_75C_SETUP";

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
