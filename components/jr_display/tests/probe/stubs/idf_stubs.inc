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
static const void *s_stub_anim_source;
static unsigned s_stub_bound_frees;
static size_t s_stub_heap_bytes;
static struct { void *ptr; size_t bytes; } s_stub_heap[128];
void *heap_caps_malloc(size_t s, unsigned c)
{
    (void)c;
    void *ptr = malloc(s);
    if (ptr) {
        for (size_t i = 0; i < sizeof s_stub_heap / sizeof s_stub_heap[0]; ++i) {
            if (!s_stub_heap[i].ptr) {
                s_stub_heap[i].ptr = ptr;
                s_stub_heap[i].bytes = s;
                s_stub_heap_bytes += s;
                break;
            }
        }
    }
    return ptr;
}
void *heap_caps_calloc(size_t n, size_t s, unsigned c) { (void)c; return calloc(n, s); }
void heap_caps_free(void *p)
{
    if (p && p == s_stub_anim_source) { s_stub_bound_frees++; }
    for (size_t i = 0; i < sizeof s_stub_heap / sizeof s_stub_heap[0]; ++i) {
        if (p && s_stub_heap[i].ptr == p) {
            s_stub_heap_bytes -= s_stub_heap[i].bytes;
            s_stub_heap[i].ptr = NULL;
            break;
        }
    }
    free(p);
}
size_t heap_caps_get_free_size(unsigned c) { (void)c; return 0; }
static int64_t s_fake_us;
int64_t esp_timer_get_time(void) { return s_fake_us; }
static esp_err_t s_stub_spiffs_error;
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *c) { (void)c; return s_stub_spiffs_error; }
esp_err_t esp_spiffs_info(const char *l, size_t *t, size_t *u) { (void)l; *t = *u = 0; return ESP_OK; }
static esp_lcd_panel_io_callbacks_t s_stub_panel_callbacks;
static void *s_stub_panel_callback_ctx;
esp_err_t esp_lcd_panel_io_register_event_callbacks(
    esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t *cb, void *ctx)
{ (void)io; s_stub_panel_callbacks = *cb; s_stub_panel_callback_ctx = ctx; return ESP_OK; }
static esp_err_t s_stub_draw_error;
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int a, int b,
                                    int c, int d, const void *px)
{ (void)p; (void)a; (void)b; (void)c; (void)d; (void)px; return s_stub_draw_error; }
void taskENTER_CRITICAL(portMUX_TYPE *m) { (void)m; }
void taskEXIT_CRITICAL(portMUX_TYPE *m) { (void)m; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *n, uint32_t s,
                                   void *a, UBaseType_t p, TaskHandle_t *o, BaseType_t c)
{ (void)fn; (void)n; (void)s; (void)a; (void)p; (void)o; (void)c; return pdPASS; }
static unsigned s_stub_notifies;
void xTaskNotifyGive(TaskHandle_t t) { (void)t; s_stub_notifies++; }
static void (*s_stub_notify_take_hook)(void);
uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t)
{ (void)c; (void)t; if (s_stub_notify_take_hook) { s_stub_notify_take_hook(); } return 0; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelay(TickType_t t) { (void)t; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 0; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
static bool s_stub_engine_ready;
static void (*s_stub_disp_add_hook)(gfx_disp_t *, const gfx_disp_config_t *);
gfx_handle_t gfx_emote_init(const gfx_core_config_t *c) { (void)c; return s_stub_engine_ready ? (gfx_handle_t)1 : NULL; }
gfx_disp_t *gfx_disp_add(gfx_handle_t h, const gfx_disp_config_t *c)
{
    (void)h;
    gfx_disp_t *disp = s_stub_engine_ready ? (gfx_disp_t *)2 : NULL;
    if (disp && s_stub_disp_add_hook) { s_stub_disp_add_hook(disp, c); }
    return disp;
}
static void *s_stub_disp_user_data;
void *gfx_disp_get_user_data(gfx_disp_t *d) { (void)d; return s_stub_disp_user_data; }
static unsigned s_stub_flush_ready_calls;
esp_err_t gfx_disp_flush_ready(gfx_disp_t *d, bool ok) { (void)d; (void)ok; s_stub_flush_ready_calls++; return ESP_OK; }
static unsigned s_stub_refreshes;
void gfx_disp_refresh_all(gfx_disp_t *d) { (void)d; s_stub_refreshes++; }
static unsigned s_stub_lock_failures;
static void (*s_stub_lock_hook)(void);
esp_err_t gfx_emote_lock(gfx_handle_t h)
{
    (void)h;
    if (s_stub_lock_hook) { s_stub_lock_hook(); }
    if (s_stub_lock_failures) { s_stub_lock_failures--; return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t gfx_emote_unlock(gfx_handle_t h) { (void)h; return ESP_OK; }
uint32_t g_stub_render_fps;
esp_err_t gfx_emote_set_fps(gfx_handle_t h, uint32_t fps) { (void)h; g_stub_render_fps = fps; return ESP_OK; }
gfx_obj_t *gfx_anim_create(gfx_disp_t *d) { (void)d; return s_stub_engine_ready ? (gfx_obj_t *)3 : NULL; }
esp_err_t gfx_obj_align(gfx_obj_t *o, gfx_align_t a, int x, int y)
{ (void)o; (void)a; (void)x; (void)y; return ESP_OK; }
static bool s_stub_anim_visible;
esp_err_t gfx_obj_set_visible(gfx_obj_t *o, bool v) { (void)o; s_stub_anim_visible = v; return ESP_OK; }
static esp_err_t s_stub_set_src_error;
esp_err_t gfx_anim_set_src(gfx_obj_t *o, const void *d, size_t s)
{
    (void)o; (void)s;
    /* Vendor failure preserves the previous parser/source, even stopped. */
    if (s_stub_set_src_error != ESP_OK) { return s_stub_set_src_error; }
    s_stub_anim_source = d;
    return ESP_OK;
}
esp_err_t gfx_anim_set_segment(gfx_obj_t *o, uint32_t a, uint32_t b, uint32_t f, bool r)
{ (void)o; (void)a; (void)b; (void)f; (void)r; return ESP_OK; }
static unsigned s_stub_anim_starts;
static unsigned s_stub_anim_stops;
esp_err_t gfx_anim_start(gfx_obj_t *o) { (void)o; s_stub_anim_starts++; return ESP_OK; }
esp_err_t gfx_anim_stop(gfx_obj_t *o) { (void)o; s_stub_anim_stops++; return ESP_OK; }
uint32_t gfx_timer_get_actual_fps(gfx_handle_t h) { (void)h; return 0; }
/* The dial decode: the header a test sets, and a frame whose pixel i is
 * (uint16_t)(i ^ 0x5a5a) so a blit can be checked row by row. */
static eaf_header_t s_stub_eaf_head = { 8, 466, 466 };
static int s_stub_eaf_decodes;
static esp_err_t s_stub_eaf_decode_error;
esp_err_t eaf_init(const uint8_t *d, size_t n, eaf_format_handle_t *h)
{ (void)d; (void)n; *h = (eaf_format_handle_t)1; return ESP_OK; }
esp_err_t eaf_deinit(eaf_format_handle_t h) { (void)h; return ESP_OK; }
eaf_format_type_t eaf_get_frame_info(eaf_format_handle_t h, int i, eaf_header_t *o)
{ (void)h; (void)i; *o = s_stub_eaf_head; return EAF_FORMAT_VALID; }
void eaf_free_header(eaf_header_t *h) { (void)h; }
esp_err_t eaf_frame_decode(eaf_format_handle_t h, int i, uint8_t *buf, size_t n, bool swap)
{
    (void)h; (void)i; (void)swap;
    if (s_stub_eaf_decode_error != ESP_OK) { return s_stub_eaf_decode_error; }
    uint16_t *px = (uint16_t *)buf;
    for (size_t k = 0; k < n / sizeof(uint16_t); ++k) {
        px[k] = (uint16_t)(k ^ 0x5a5au);
    }
    s_stub_eaf_decodes++;
    return ESP_OK;
}
static esp_err_t s_stub_board_get_error;
static int64_t s_stub_board_init_us;
static jarvis_board_display_t s_stub_board_display;
esp_err_t jarvis_board_display_get(jarvis_board_display_t *o)
{
    s_fake_us += s_stub_board_init_us;
    if (s_stub_board_get_error != ESP_OK) { return s_stub_board_get_error; }
    *o = s_stub_board_display;
    return ESP_OK;
}
static uint8_t s_stub_panel_brightness;
esp_err_t jarvis_board_set_brightness(uint8_t p) { s_stub_panel_brightness = p; return ESP_OK; }

#endif /* JR_DISPLAY_TEST_IDF_STUBS_INC */
