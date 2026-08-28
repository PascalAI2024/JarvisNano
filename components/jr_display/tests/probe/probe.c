/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Visual probe: renders presenter overlays on the host, for eyes-on-the-glass
 * verification WITHOUT a flash cycle. It #includes jr_display.c itself over
 * the stub headers in ./stubs — statics and all — sets the render-side state
 * directly, composes the real apply_*_overlay functions in 12-row strips
 * exactly as panel_flush does, and writes a PPM. The rendered frame is the
 * expected-render reference to hold a hardware capture against.
 *
 * Build + run (from this directory; no cmake, no IDF):
 *
 *   cc -std=gnu17 -O2 -Wno-unused-function \
 *      -I../../include -I../../../jr_ports/include -I./stubs -I../../src \
 *      probe.c ../../src/hud_render.c -o probe && ./probe
 *   sips -s format png probe_out.ppm --out probe_out.png   # macOS
 *
 * NOT part of any automated suite: the output is a picture, and the assert is
 * a human (or agent) looking at it. The exhaustive pins live in
 * ../test_hud_render.c; this exists because jr_display.c's composition has no
 * other executable host path. Edit main() to stage whatever state you need. */
#include "jr_display.c"

#include <stdio.h>
#include <stdlib.h>

/* ---- stub implementations for every extern jr_display.c references ---- */
const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }
void *heap_caps_malloc(size_t s, unsigned c) { (void)c; return malloc(s); }
void *heap_caps_calloc(size_t n, size_t s, unsigned c) { (void)c; return calloc(n, s); }
void heap_caps_free(void *p) { free(p); }
size_t heap_caps_get_free_size(unsigned c) { (void)c; return 0; }
static int64_t s_fake_us;
int64_t esp_timer_get_time(void) { return s_fake_us; }
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *c) { (void)c; return ESP_OK; }
esp_err_t esp_spiffs_info(const char *l, size_t *t, size_t *u) { (void)l; *t = *u = 0; return ESP_OK; }
esp_err_t esp_lcd_panel_io_register_event_callbacks(
    esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t *cb, void *ctx)
{ (void)io; (void)cb; (void)ctx; return ESP_OK; }
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int a, int b,
                                    int c, int d, const void *px)
{ (void)p; (void)a; (void)b; (void)c; (void)d; (void)px; return ESP_OK; }
void taskENTER_CRITICAL(portMUX_TYPE *m) { (void)m; }
void taskEXIT_CRITICAL(portMUX_TYPE *m) { (void)m; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *n, uint32_t s,
                                   void *a, UBaseType_t p, TaskHandle_t *o, BaseType_t c)
{ (void)fn; (void)n; (void)s; (void)a; (void)p; (void)o; (void)c; return pdPASS; }
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t) { (void)c; (void)t; return 0; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelay(TickType_t t) { (void)t; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 0; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
gfx_handle_t gfx_emote_init(const gfx_core_config_t *c) { (void)c; return NULL; }
gfx_disp_t *gfx_disp_add(gfx_handle_t h, const gfx_disp_config_t *c) { (void)h; (void)c; return NULL; }
void *gfx_disp_get_user_data(gfx_disp_t *d) { (void)d; return NULL; }
esp_err_t gfx_disp_flush_ready(gfx_disp_t *d, bool ok) { (void)d; (void)ok; return ESP_OK; }
void gfx_disp_refresh_all(gfx_disp_t *d) { (void)d; }
esp_err_t gfx_emote_lock(gfx_handle_t h) { (void)h; return ESP_OK; }
esp_err_t gfx_emote_unlock(gfx_handle_t h) { (void)h; return ESP_OK; }
gfx_obj_t *gfx_anim_create(gfx_disp_t *d) { (void)d; return NULL; }
esp_err_t gfx_obj_align(gfx_obj_t *o, gfx_align_t a, int x, int y)
{ (void)o; (void)a; (void)x; (void)y; return ESP_OK; }
esp_err_t gfx_obj_set_visible(gfx_obj_t *o, bool v) { (void)o; (void)v; return ESP_OK; }
esp_err_t gfx_anim_set_src(gfx_obj_t *o, const void *d, size_t s)
{ (void)o; (void)d; (void)s; return ESP_OK; }
esp_err_t gfx_anim_set_segment(gfx_obj_t *o, uint32_t a, uint32_t b, uint32_t f, bool r)
{ (void)o; (void)a; (void)b; (void)f; (void)r; return ESP_OK; }
esp_err_t gfx_anim_start(gfx_obj_t *o) { (void)o; return ESP_OK; }
esp_err_t gfx_anim_stop(gfx_obj_t *o) { (void)o; return ESP_OK; }
uint32_t gfx_timer_get_actual_fps(gfx_handle_t h) { (void)h; return 0; }
esp_err_t jarvis_board_display_get(jarvis_board_display_t *o) { (void)o; return ESP_OK; }
esp_err_t jarvis_board_set_brightness(uint8_t p) { (void)p; return ESP_OK; }

/* ---- fake face: concentric cyan rings on black, like the baked art ---- */
static int probe_isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static uint16_t face_px(int x, int y)
{
    const int dx = x - 232, dy = y - 232;
    const int r2 = dx * dx + dy * dy;
    if (r2 > 232 * 232) return 0;
    const int r = probe_isqrt(r2);
    int lum = 30;
    if ((r >= 60 && r <= 66) || (r >= 125 && r <= 134) ||
        (r >= 155 && r <= 179) || (r >= 200 && r <= 214)) lum = 180;
    return (uint16_t)(((0) << 11) | (((lum * 63) / 255) << 5) | ((lum * 31) / 255));
}

int main(void)
{
    s_display.board.width = 466;
    s_display.board.height = 466;
    s_display.board.swap_color_bytes = false;

    /* shade open, settled ease */
    s_display.shell_word = JR_DISPLAY_SHELL_SHADE;
    s_shade_ease = 256;

    uint16_t *fb = malloc(466u * 466u * sizeof(uint16_t));
    for (int y = 0; y < 466; y++)
        for (int x = 0; x < 466; x++)
            fb[y * 466 + x] = face_px(x, y);

    /* compose in 12-row strips exactly like the flush does */
    for (int y = 0; y < 466; y += 12) {
        int y2 = y + 12 > 466 ? 466 : y + 12;
        apply_shell_overlay(&s_display, 0, y, 466, y2, fb + (size_t)y * 466);
        apply_guide_overlay(&s_display, 0, y, 466, y2, fb + (size_t)y * 466);
    }

    FILE *f = fopen("probe_out.ppm", "wb");
    fprintf(f, "P6 466 466 255\n");
    for (int i = 0; i < 466 * 466; i++) {
        uint16_t v = fb[i];
        unsigned char rgb[3] = {
            (unsigned char)(((v >> 11) & 31) * 255 / 31),
            (unsigned char)(((v >> 5) & 63) * 255 / 63),
            (unsigned char)((v & 31) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote probe_out.ppm\n");
    return 0;
}
