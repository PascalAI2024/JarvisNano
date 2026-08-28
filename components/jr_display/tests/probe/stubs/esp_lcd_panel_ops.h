#pragma once
#include "esp_err.h"
typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x1, int y1, int x2, int y2, const void *data);
