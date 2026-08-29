/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * idf_stubs.inc — definitions for every extern jr_display.c references.
 *
 * Included (not linked) by both host executables that pull jr_display.c into
 * their own translation unit: probe/probe.c, which renders a frame to a PPM,
 * and test_shell.c, which asserts on shell state and geometry. Keeping one
 * copy means a new IDF call in jr_display.c breaks both at once rather than
 * letting the tests drift behind the probe.
 *
 * Every stub is the cheapest thing that satisfies the caller: nothing here is
 * a simulation. The one exception is esp_timer_get_time, which reads a
 * writable s_fake_us so a test can drive the presenter's easing clock.
 */
#ifndef JR_DISPLAY_TEST_IDF_STUBS_INC
#define JR_DISPLAY_TEST_IDF_STUBS_INC

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

#endif /* JR_DISPLAY_TEST_IDF_STUBS_INC */
