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

#ifdef __cplusplus
}
#endif
