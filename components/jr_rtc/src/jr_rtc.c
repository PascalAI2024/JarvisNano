/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PCF85063A: CTRL1 @0x00, seconds @0x04 (bit7 = oscillator-stop).
 * NXP PCF85063A datasheet. Address 0x51.
 */
#include "jr_rtc/jr_rtc.h"

#include "driver/i2c_master.h"
#include "esp_board_manager.h"
#include "esp_log.h"

#define PCF85063_ADDR        0x51
#define PCF85063_REG_CTRL1   0x00
#define PCF85063_REG_SECONDS 0x04
#define PCF85063_OS_BIT      0x80
#define PCF85063_I2C_MS      80

static const char *TAG = "jr_rtc";
static i2c_master_dev_handle_t s_dev;
static bool s_present;

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0f));
}

static uint8_t bin_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static esp_err_t rtc_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, PCF85063_I2C_MS);
}

static esp_err_t rtc_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t pkt[8];
    if (len + 1U > sizeof(pkt)) {
        return ESP_ERR_INVALID_SIZE;
    }
    pkt[0] = reg;
    for (size_t i = 0; i < len; i++) {
        pkt[i + 1] = buf[i];
    }
    return i2c_master_transmit(s_dev, pkt, len + 1U, PCF85063_I2C_MS);
}

esp_err_t jr_rtc_start(void)
{
    if (s_present) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = esp_board_manager_get_periph_handle("i2c_master",
                                                        (void **)&bus);
    if (err != ESP_OK || bus == NULL) {
        ESP_LOGW(TAG, "i2c_master unavailable: %s", esp_err_to_name(err));
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PCF85063_ADDR,
        .scl_speed_hz    = 400000,
    };
    err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 @0x51 add failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t ctrl1 = 0;
    err = rtc_read(PCF85063_REG_CTRL1, &ctrl1, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 probe failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Force 24-hour mode (CTRL1 bit1 = 0) if a previous sketch left 12h on. */
    if (ctrl1 & 0x02u) {
        uint8_t cleared = (uint8_t)(ctrl1 & ~0x02u);
        (void)rtc_write(PCF85063_REG_CTRL1, &cleared, 1);
    }

    s_present = true;
    ESP_LOGI(TAG, "PCF85063 online (CTRL1=0x%02X)", ctrl1);
    return ESP_OK;
}

bool jr_rtc_present(void)
{
    return s_present;
}

esp_err_t jr_rtc_get(struct tm *out)
{
    if (!s_present || out == NULL || s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t raw[7];
    esp_err_t err = rtc_read(PCF85063_REG_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    if (raw[0] & PCF85063_OS_BIT) {
        return ESP_ERR_INVALID_STATE; /* oscillator stopped — never set */
    }
    out->tm_sec  = bcd_to_bin((uint8_t)(raw[0] & 0x7f));
    out->tm_min  = bcd_to_bin((uint8_t)(raw[1] & 0x7f));
    out->tm_hour = bcd_to_bin((uint8_t)(raw[2] & 0x3f));
    out->tm_mday = bcd_to_bin((uint8_t)(raw[3] & 0x3f));
    out->tm_wday = (int)(raw[4] & 0x07);
    out->tm_mon  = bcd_to_bin((uint8_t)(raw[5] & 0x1f)) - 1;
    out->tm_year = bcd_to_bin(raw[6]) + 100; /* 2000-based → 1900-based */
    out->tm_isdst = -1;
    if (out->tm_min > 59 || out->tm_hour > 23 || out->tm_mon < 0 ||
        out->tm_mon > 11) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t jr_rtc_set(const struct tm *in)
{
    if (!s_present || in == NULL || s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t raw[7] = {
        bin_to_bcd((uint8_t)in->tm_sec),
        bin_to_bcd((uint8_t)in->tm_min),
        bin_to_bcd((uint8_t)in->tm_hour),
        bin_to_bcd((uint8_t)in->tm_mday),
        (uint8_t)(in->tm_wday & 0x07),
        bin_to_bcd((uint8_t)(in->tm_mon + 1)),
        bin_to_bcd((uint8_t)(in->tm_year % 100)),
    };
    return rtc_write(PCF85063_REG_SECONDS, raw, sizeof(raw));
}
