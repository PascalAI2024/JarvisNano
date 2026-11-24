#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    uint16_t width;
    uint16_t height;
    bool swap_color_bytes;
} jarvis_board_display_t;

esp_err_t jarvis_board_display_get(jarvis_board_display_t *out_display);
esp_err_t jarvis_board_set_brightness(uint8_t percent);
/* Gfx-owner only. Transport acknowledgement; caller owns 120 ms settling. */
esp_err_t jarvis_board_display_set_sleep(bool sleep);
/* Gfx-owner only: CO5300 High-Brightness Mode (0x63=0xFF, 0x66 bit 1 HBM_EN). */
esp_err_t jarvis_board_set_hbm(bool on);
/* 60 Hz LCD_TE (GPIO13) rising-edge V-blank sync. Bounded wait; never hangs. */
bool jarvis_board_te_enabled(void);
bool jarvis_board_te_wait(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
