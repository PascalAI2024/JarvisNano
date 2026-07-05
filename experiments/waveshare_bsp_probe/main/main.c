#include <stdint.h>
#include <stdio.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

static const char *TAG = "bsp_probe";

static lv_obj_t *s_touch_label;
static lv_obj_t *s_heap_label;
static lv_obj_t *s_touch_dot;
static lv_obj_t *s_progress_arc;
static uint32_t s_touch_count;

static int clamp_i32(int value, int min, int max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static void update_heap_label(lv_timer_t *timer)
{
    (void)timer;
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    lv_label_set_text_fmt(s_heap_label,
                          "heap: %u KB int / %u KB psram",
                          (unsigned)(internal / 1024),
                          (unsigned)(psram / 1024));
}

static void touch_event_cb(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point = { .x = 233, .y = 233 };

    if (indev) {
        lv_indev_get_point(indev, &point);
    }

    s_touch_count++;
    lv_label_set_text_fmt(s_touch_label,
                          "touch: %lu  x=%ld y=%ld",
                          (unsigned long)s_touch_count,
                          (long)point.x,
                          (long)point.y);

    lv_obj_set_pos(s_touch_dot,
                   clamp_i32(point.x - 8, 0, BSP_LCD_H_RES - 16),
                   clamp_i32(point.y - 8, 0, BSP_LCD_V_RES - 16));
    lv_arc_set_value(s_progress_arc, (int)((s_touch_count * 9U) % 100U));
}

static lv_obj_t *make_label(lv_obj_t *parent,
                            const char *text,
                            const lv_font_t *font,
                            lv_color_t color,
                            lv_align_t align,
                            int32_t x,
                            int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_align(label, align, x, y);
    return label;
}

static void build_probe_screen(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x070a0d), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    s_progress_arc = lv_arc_create(screen);
    lv_obj_set_size(s_progress_arc, 430, 430);
    lv_obj_center(s_progress_arc);
    lv_arc_set_range(s_progress_arc, 0, 100);
    lv_arc_set_value(s_progress_arc, 31);
    lv_obj_remove_style(s_progress_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_progress_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_progress_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_progress_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_progress_arc, lv_color_hex(0x25303a), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_progress_arc, lv_color_hex(0x2dd4bf), LV_PART_INDICATOR);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, 326, 244);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x2dd4bf), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);

    make_label(panel,
               "JarvisNano",
               &lv_font_montserrat_28,
               lv_color_hex(0xf8fafc),
               LV_ALIGN_TOP_MID,
               0,
               4);
    make_label(panel,
               "BSP Probe",
               &lv_font_montserrat_20,
               lv_color_hex(0x7dd3fc),
               LV_ALIGN_TOP_MID,
               0,
               42);
    make_label(panel,
               "display: waveshare BSP 3.0.1",
               &lv_font_montserrat_16,
               lv_color_hex(0xb8c7d1),
               LV_ALIGN_TOP_LEFT,
               4,
               92);
    make_label(panel,
               "touch: CST9217 via BSP",
               &lv_font_montserrat_16,
               lv_color_hex(0xb8c7d1),
               LV_ALIGN_TOP_LEFT,
               4,
               122);
    s_touch_label = make_label(panel,
                               "touch: waiting",
                               &lv_font_montserrat_16,
                               lv_color_hex(0xfacc15),
                               LV_ALIGN_TOP_LEFT,
                               4,
                               154);
    s_heap_label = make_label(panel,
                              "heap: measuring",
                              &lv_font_montserrat_16,
                              lv_color_hex(0x86efac),
                              LV_ALIGN_TOP_LEFT,
                              4,
                              184);

    s_touch_dot = lv_obj_create(screen);
    lv_obj_set_size(s_touch_dot, 16, 16);
    lv_obj_set_style_radius(s_touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_touch_dot, lv_color_hex(0xfacc15), 0);
    lv_obj_set_style_bg_opa(s_touch_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_touch_dot, 0, 0);
    lv_obj_set_pos(s_touch_dot, 225, 225);

    lv_obj_t *hit = lv_obj_create(screen);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(hit, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hit, touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(hit, touch_event_cb, LV_EVENT_RELEASED, NULL);

    lv_timer_create(update_heap_label, 1000, NULL);
    update_heap_label(NULL);
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting Waveshare BSP display/touch probe");
    init_nvs();

    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE(TAG, "bsp_display_start failed; staying alive for monitor logs");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    (void)bsp_display_backlight_on();

    bsp_display_lock(-1);
    build_probe_screen();
    bsp_display_unlock();

    ESP_LOGI(TAG, "probe UI started");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
