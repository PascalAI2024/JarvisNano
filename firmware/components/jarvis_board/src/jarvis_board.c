#include "jarvis_board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "jarvis_board";

#define JARVIS_LCD_H_RES 466
#define JARVIS_LCD_V_RES 466
#define JARVIS_LCD_BITS_PER_PIXEL 16
#define JARVIS_LCD_SPI_HOST SPI2_HOST
#define JARVIS_LCD_CS GPIO_NUM_12
#define JARVIS_LCD_PCLK GPIO_NUM_38
#define JARVIS_LCD_DATA0 GPIO_NUM_4
#define JARVIS_LCD_DATA1 GPIO_NUM_5
#define JARVIS_LCD_DATA2 GPIO_NUM_6
#define JARVIS_LCD_DATA3 GPIO_NUM_7
#define JARVIS_LCD_RST GPIO_NUM_39
#define JARVIS_LCD_TRANS_QUEUE_DEPTH 10

static const co5300_lcd_init_cmd_t s_lcd_init_cmds[] = {
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

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static bool s_display_ready;
static jarvis_board_display_t s_display;

static SemaphoreHandle_t jarvis_board_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    return s_lock;
}

esp_err_t jarvis_board_display_get(jarvis_board_display_t *out_display)
{
    ESP_RETURN_ON_FALSE(out_display, ESP_ERR_INVALID_ARG, TAG, "out_display is NULL");

    if (s_display_ready) {
        *out_display = s_display;
        return ESP_OK;
    }

    SemaphoreHandle_t lock = jarvis_board_lock();
    ESP_RETURN_ON_FALSE(lock, ESP_ERR_NO_MEM, TAG, "display lock alloc failed");
    xSemaphoreTake(lock, portMAX_DELAY);

    esp_err_t ret = ESP_OK;
    if (!s_display_ready) {
        esp_lcd_panel_handle_t panel = NULL;
        esp_lcd_panel_io_handle_t io = NULL;
        bool bus_initialized = false;

        const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(
            JARVIS_LCD_PCLK,
            JARVIS_LCD_DATA0,
            JARVIS_LCD_DATA1,
            JARVIS_LCD_DATA2,
            JARVIS_LCD_DATA3,
            JARVIS_LCD_H_RES * JARVIS_LCD_V_RES * JARVIS_LCD_BITS_PER_PIXEL / 8);
        ESP_LOGI(TAG, "Initialize SPI bus for CO5300 QSPI display");
        ESP_GOTO_ON_ERROR(spi_bus_initialize(JARVIS_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                          fail_display, TAG, "spi_bus_initialize failed");
        bus_initialized = true;

        esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(JARVIS_LCD_CS, NULL, NULL);
        io_config.trans_queue_depth = JARVIS_LCD_TRANS_QUEUE_DEPTH;
        ESP_GOTO_ON_ERROR(
            esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)JARVIS_LCD_SPI_HOST,
                                     &io_config, &io),
            fail_display, TAG, "esp_lcd_new_panel_io_spi failed");

        co5300_vendor_config_t vendor_config = {
            .init_cmds = s_lcd_init_cmds,
            .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = JARVIS_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = JARVIS_LCD_BITS_PER_PIXEL,
            .vendor_config = &vendor_config,
        };
        ESP_GOTO_ON_ERROR(esp_lcd_new_panel_co5300(io, &panel_config, &panel),
                          fail_display, TAG, "esp_lcd_new_panel_co5300 failed");
        ESP_GOTO_ON_FALSE(panel && io, ESP_ERR_INVALID_STATE, fail_display, TAG,
                          "CO5300 display returned NULL handles");
        ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(panel, 0x06, 0), fail_display, TAG,
                          "esp_lcd_panel_set_gap failed");
        ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel), fail_display, TAG,
                          "esp_lcd_panel_reset failed");
        ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel), fail_display, TAG,
                          "esp_lcd_panel_init failed");
        ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), fail_display, TAG,
                          "esp_lcd_panel_disp_on failed");
        ESP_GOTO_ON_ERROR(esp_lcd_panel_co5300_set_brightness(panel, 100), fail_display, TAG,
                          "esp_lcd_panel_co5300_set_brightness failed");

        jarvis_board_display_t display = {
            .panel = panel,
            .io = io,
            .width = JARVIS_LCD_H_RES,
            .height = JARVIS_LCD_V_RES,
            .swap_color_bytes = true,
        };
        memcpy(&s_display, &display, sizeof(s_display));
        s_display_ready = true;
        ESP_LOGI(TAG, "CO5300 QSPI display ready: %ux%u panel=%p io=%p",
                 (unsigned)s_display.width, (unsigned)s_display.height,
                 (void *)s_display.panel, (void *)s_display.io);

fail_display:
        if (ret != ESP_OK && !s_display_ready) {
            if (panel) {
                (void)esp_lcd_panel_del(panel);
            }
            if (io) {
                (void)esp_lcd_panel_io_del(io);
            }
            if (bus_initialized) {
                (void)spi_bus_free(JARVIS_LCD_SPI_HOST);
            }
        }
    }

    if (ret == ESP_OK) {
        *out_display = s_display;
    }

    xSemaphoreGive(lock);
    return ret;
}
