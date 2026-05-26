/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "emote.h"

#include <string.h>
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_board_manager_includes.h"
#include "expression_emote.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gfx.h"
#include "display_arbiter.h"

static const char *TAG = "app_emote";

#define EMOTE_ASSETS_PARTITION "emote"

/* Reactive waveform face — defined in waveform.c (same component). Initialised
 * after the engine + assets are up so it can create its image object. */
esp_err_t emote_face_init(emote_handle_t emote);

static esp_lcd_panel_io_handle_t s_io_handle;
static esp_lcd_panel_handle_t s_panel_handle;
static int s_lcd_width;
static int s_lcd_height;
static emote_handle_t s_emote_handle;
static SemaphoreHandle_t s_snapshot_mx;
static uint16_t *s_snapshot_rgb565;
static size_t s_snapshot_bytes;
static uint64_t s_snapshot_frame_id;
static uint64_t s_snapshot_last_flush_ms;
static bool s_snapshot_valid;
static bool s_snapshot_alloc_logged;
static SemaphoreHandle_t s_flush_done;
static bool s_flush_sync_registered;
static uint32_t s_flush_timeouts;

#define EMOTE_FLUSH_WAIT_MS        80
#define EMOTE_FLUSH_TIMEOUT_LIMIT  3
#define EMOTE_FLUSH_STRIP_ROWS     12

static IRAM_ATTR bool emote_panel_io_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                             esp_lcd_panel_io_event_data_t *edata,
                                             void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;

    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    if (s_flush_done) {
        xSemaphoreGiveFromISR(s_flush_done, &high_task_woken);
    }

    return high_task_woken == pdTRUE;
}

static void emote_flush_sync_drain(void)
{
    if (!s_flush_sync_registered || !s_flush_done) {
        return;
    }
    while (xSemaphoreTake(s_flush_done, 0) == pdTRUE) {
    }
}

static void emote_flush_sync_wait(void)
{
    if (!s_flush_sync_registered || !s_flush_done) {
        return;
    }

    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(EMOTE_FLUSH_WAIT_MS)) == pdTRUE) {
        s_flush_timeouts = 0;
        return;
    }

    s_flush_timeouts++;
    ESP_LOGW(TAG, "display flush completion timeout (%u/%u)",
             (unsigned)s_flush_timeouts, (unsigned)EMOTE_FLUSH_TIMEOUT_LIMIT);
    if (s_flush_timeouts >= EMOTE_FLUSH_TIMEOUT_LIMIT) {
        s_flush_sync_registered = false;
        ESP_LOGW(TAG, "display flush sync disabled after repeated timeouts");
    }
}

static void emote_register_flush_sync(void)
{
    if (!s_io_handle || s_flush_sync_registered) {
        return;
    }
    if (!s_flush_done) {
        s_flush_done = xSemaphoreCreateBinary();
        if (!s_flush_done) {
            ESP_LOGW(TAG, "display flush sync semaphore alloc failed");
            return;
        }
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = emote_panel_io_done_cb,
    };
    esp_err_t err = esp_lcd_panel_io_register_event_callbacks(s_io_handle, &callbacks, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display flush sync callback registration failed: %s", esp_err_to_name(err));
        return;
    }

    s_flush_sync_registered = true;
    s_flush_timeouts = 0;
    ESP_LOGI(TAG, "display flush sync callback registered");
}

static bool emote_should_swap_color(
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    const dev_display_lcd_config_t *lcd_cfg
#else
    const void *lcd_cfg
#endif
)
{
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    if (lcd_cfg == NULL || lcd_cfg->sub_type == NULL) {
        return true;
    }

    if (strcmp(lcd_cfg->sub_type, "dsi") == 0 || strcmp(lcd_cfg->sub_type, "mipi_dsi") == 0 || strcmp(lcd_cfg->sub_type, "rgb") == 0) {
        return false;
    }

    return true;
#else
    (void)lcd_cfg;
    return true;
#endif
}

static void emote_on_owner_changed(display_arbiter_owner_t owner, void *user_ctx)
{
    (void)user_ctx;

    if (owner != DISPLAY_ARBITER_OWNER_EMOTE || !s_emote_handle) {
        return;
    }

    esp_err_t err = emote_notify_all_refresh(s_emote_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refresh after owner switch failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t emote_snapshot_ensure_buffer(void)
{
    if (s_lcd_width <= 0 || s_lcd_height <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_snapshot_mx) {
        s_snapshot_mx = xSemaphoreCreateMutex();
        if (!s_snapshot_mx) {
            return ESP_ERR_NO_MEM;
        }
    }

    size_t need = (size_t)s_lcd_width * (size_t)s_lcd_height * sizeof(uint16_t);
    if (s_snapshot_rgb565 && s_snapshot_bytes == need) {
        return ESP_OK;
    }

    uint16_t *buf = (uint16_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint16_t *)heap_caps_malloc(need, MALLOC_CAP_DEFAULT);
    }
    if (!buf) {
        if (!s_snapshot_alloc_logged) {
            ESP_LOGW(TAG, "display snapshot mirror alloc failed (%u bytes)", (unsigned)need);
            s_snapshot_alloc_logged = true;
        }
        return ESP_ERR_NO_MEM;
    }

    memset(buf, 0, need);

    if (xSemaphoreTake(s_snapshot_mx, portMAX_DELAY) == pdTRUE) {
        uint16_t *old = s_snapshot_rgb565;
        s_snapshot_rgb565 = buf;
        s_snapshot_bytes = need;
        s_snapshot_valid = false;
        xSemaphoreGive(s_snapshot_mx);
        if (old) {
            heap_caps_free(old);
        }
    } else {
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "display snapshot mirror ready %dx%d (%u bytes)",
             s_lcd_width, s_lcd_height, (unsigned)need);
    return ESP_OK;
}

static void emote_snapshot_record_flush(int x_start, int y_start, int x_end, int y_end,
                                        const void *data)
{
    if (!data || emote_snapshot_ensure_buffer() != ESP_OK || !s_snapshot_mx) {
        return;
    }

    if (x_start < 0) {
        x_start = 0;
    }
    if (y_start < 0) {
        y_start = 0;
    }
    if (x_end > s_lcd_width) {
        x_end = s_lcd_width;
    }
    if (y_end > s_lcd_height) {
        y_end = s_lcd_height;
    }
    if (x_end <= x_start || y_end <= y_start) {
        return;
    }

    const int w = x_end - x_start;
    const int h = y_end - y_start;
    const uint16_t *src = (const uint16_t *)data;

    if (xSemaphoreTake(s_snapshot_mx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    for (int row = 0; row < h; row++) {
        uint16_t *dst = s_snapshot_rgb565 + ((size_t)(y_start + row) * (size_t)s_lcd_width) + (size_t)x_start;
        memcpy(dst, src + ((size_t)row * (size_t)w), (size_t)w * sizeof(uint16_t));
    }
    s_snapshot_frame_id++;
    s_snapshot_last_flush_ms = (uint64_t)(esp_timer_get_time() / 1000);
    s_snapshot_valid = true;
    xSemaphoreGive(s_snapshot_mx);
}

static void emote_flush_callback(int x_start, int y_start, int x_end, int y_end,
                                 const void *data, emote_handle_t handle)
{
    if (!s_panel_handle || !display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        if (handle) {
            emote_notify_flush_finished(handle);
        }
        return;
    }

    emote_flush_sync_drain();
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle, x_start, y_start, x_end, y_end, data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_draw_bitmap failed: %s", esp_err_to_name(err));
    } else {
        emote_flush_sync_wait();
        emote_snapshot_record_flush(x_start, y_start, x_end, y_end, data);
    }

    if (handle) {
        emote_notify_flush_finished(handle);
    }
}

esp_err_t emote_display_snapshot_get_info(emote_display_snapshot_info_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "snapshot info is NULL");
    memset(out, 0, sizeof(*out));

    if (emote_snapshot_ensure_buffer() != ESP_OK || !s_snapshot_mx) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_snapshot_mx, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    out->width = (uint16_t)s_lcd_width;
    out->height = (uint16_t)s_lcd_height;
    out->bytes = s_snapshot_bytes;
    out->frame_id = s_snapshot_frame_id;
    out->last_flush_ms = s_snapshot_last_flush_ms;
    out->valid = s_snapshot_valid;
    xSemaphoreGive(s_snapshot_mx);

    return out->valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t emote_display_snapshot_copy_rgb565(void *dst,
                                             size_t dst_size,
                                             emote_display_snapshot_info_t *out)
{
    ESP_RETURN_ON_FALSE(dst != NULL, ESP_ERR_INVALID_ARG, TAG, "snapshot destination is NULL");

    emote_display_snapshot_info_t info = {0};
    esp_err_t err = emote_display_snapshot_get_info(&info);
    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_FALSE(dst_size >= info.bytes, ESP_ERR_INVALID_SIZE, TAG, "snapshot destination too small");

    if (xSemaphoreTake(s_snapshot_mx, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    memcpy(dst, s_snapshot_rgb565, info.bytes);
    xSemaphoreGive(s_snapshot_mx);

    if (out) {
        *out = info;
    }
    return ESP_OK;
}

static void emote_update_callback(gfx_disp_event_t event, const void *obj,
                                  emote_handle_t handle)
{
    if (!handle) {
        return;
    }

    gfx_obj_t *wait_obj = emote_get_obj_by_name(handle, EMT_DEF_ELEM_EMERG_DLG);
    if (wait_obj == obj && event == GFX_DISP_EVENT_ALL_FRAME_DONE) {
        ESP_LOGI(TAG, "Emergency dialog finished");
    }
}

static esp_err_t emote_load_board_display(void)
{
#if !CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    return ESP_ERR_NOT_SUPPORTED;
#else
    void *lcd_handle = NULL;
    void *lcd_config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config);
    if (err != ESP_OK) {
        return err;
    }

    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)lcd_config;

    ESP_RETURN_ON_FALSE(lcd_handles && lcd_cfg && lcd_handles->panel_handle,
                        ESP_ERR_INVALID_STATE, TAG, "display_lcd handle/config is NULL");

    s_panel_handle = lcd_handles->panel_handle;
    s_io_handle = lcd_handles->io_handle;
    s_lcd_width = lcd_cfg->lcd_width;
    s_lcd_height = lcd_cfg->lcd_height;
    emote_register_flush_sync();
    return ESP_OK;
#endif
}

static emote_config_t emote_get_default_config(void)
{
    void *lcd_config = NULL;
    bool swap = true;
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    if (esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config) == ESP_OK) {
        swap = emote_should_swap_color((const dev_display_lcd_config_t *)lcd_config);
    }
#endif

    emote_config_t config = {
        .flags = {
            .swap = swap,
            .double_buffer = true,
            .buff_dma = true,
        },
        .gfx_emote = {
            .h_res = s_lcd_width,
            .v_res = s_lcd_height,
            .fps = 30,
        },
        .buffers = {
            .buf_pixels = (size_t)s_lcd_width * EMOTE_FLUSH_STRIP_ROWS,
        },
        .task = {
            .task_priority = 3,
            .task_stack = 12 * 1024,
            .task_affinity = -1,
#ifdef CONFIG_SPIRAM_XIP_FROM_PSRAM
            .task_stack_in_ext = true,
#else
            .task_stack_in_ext = false,
#endif
        },
        .flush_cb = emote_flush_callback,
        .update_cb = emote_update_callback,
    };

    return config;
}

static esp_err_t emote_apply(const char *idle, const char *msg)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "emote handle is NULL");

    ESP_RETURN_ON_ERROR(emote_set_event_msg(s_emote_handle, EMOTE_MGR_EVT_SYS, msg), TAG, "set emote message failed");
    ESP_RETURN_ON_ERROR(emote_set_anim_emoji(s_emote_handle, idle), TAG, "set emote idle animation failed");

    if (display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        ESP_RETURN_ON_ERROR(emote_notify_all_refresh(s_emote_handle), TAG, "refresh emote display failed");
    }

    return ESP_OK;
}

/* Cached status state so emote_set_status_detail() can re-render the message
 * without the caller re-supplying the network state, and vice versa. */
static bool s_sta_connected;
static char s_ap_ssid[48];
static char s_status_detail[48];   /* e.g. the active LLM model name */
static bool s_voice_visual_active;

static esp_err_t emote_render_status(void)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "emote handle is NULL");

    const bool ap_present = (s_ap_ssid[0] != '\0');
    const bool has_detail = (s_status_detail[0] != '\0');
    const char *idle = s_sta_connected ? "neutral" : "offline";

    char msg[96];
    if (s_sta_connected && has_detail) {
        snprintf(msg, sizeof(msg), "Ready * %s", s_status_detail);
    } else if (s_sta_connected && ap_present) {
        snprintf(msg, sizeof(msg), "Online * AP: %s", s_ap_ssid);
    } else if (s_sta_connected) {
        snprintf(msg, sizeof(msg), "Wi-Fi connected");
    } else if (ap_present) {
        snprintf(msg, sizeof(msg), "Setup WiFi: %s", s_ap_ssid);
    } else {
        snprintf(msg, sizeof(msg), "Wi-Fi offline");
    }

    esp_err_t err = emote_apply(idle, msg);
    emote_face_set_state(EMOTE_FACE_IDLE);
    return err;
}

esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid)
{
    s_sta_connected = sta_connected;
    if (ap_ssid != NULL && ap_ssid[0] != '\0') {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ap_ssid);
    } else {
        s_ap_ssid[0] = '\0';
    }
    return emote_render_status();
}

esp_err_t emote_set_status_detail(const char *detail)
{
    if (detail != NULL && detail[0] != '\0') {
        snprintf(s_status_detail, sizeof(s_status_detail), "%s", detail);
    } else {
        s_status_detail[0] = '\0';
    }
    if (s_voice_visual_active) {
        return ESP_OK;
    }
    /* If the emote engine is up, reflect the new detail now; otherwise the
     * next emote_set_network_status() will pick it up. */
    if (s_emote_handle != NULL) {
        return emote_render_status();
    }
    return ESP_OK;
}

static void emote_cleanup(void)
{
    if (s_emote_handle) {
        emote_deinit(s_emote_handle);
        s_emote_handle = NULL;
    }
    display_arbiter_set_owner_changed_callback(NULL, NULL);
}

static esp_err_t emote_init_internal(void)
{
    emote_data_t data = {
        .type = EMOTE_SOURCE_PARTITION,
        .source = {
            .partition_label = EMOTE_ASSETS_PARTITION,
        },
        .flags = {
#ifdef CONFIG_SPIRAM_XIP_FROM_PSRAM
            .mmap_enable = false,
#else
            .mmap_enable = true,
#endif
        },
    };

    if (s_emote_handle) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(emote_load_board_display(), TAG, "Failed to get board display handles");

    emote_config_t config = emote_get_default_config();
    ESP_RETURN_ON_ERROR(display_arbiter_set_owner_changed_callback(emote_on_owner_changed, NULL), TAG, "register display owner callback failed");
    s_emote_handle = emote_init(&config);
    if (!s_emote_handle || !emote_is_initialized(s_emote_handle)) {
        emote_cleanup();
        return ESP_FAIL;
    }

    esp_err_t err = emote_mount_and_load_assets(s_emote_handle, &data);
    if (err != ESP_OK) {
        emote_cleanup();
        return err;
    }

    /* Bring up the reactive waveform face (best-effort: a failure here must not
     * stop the static emote idle screen from working). */
    esp_err_t face_err = emote_face_init(s_emote_handle);
    if (face_err != ESP_OK) {
        ESP_LOGW(TAG, "waveform face init failed: %s (idle face still works)", esp_err_to_name(face_err));
    }

    return emote_set_network_status(false, NULL);
}

esp_err_t emote_start(void)
{
    esp_err_t err = emote_init_internal();
    if (err != ESP_OK) {
        emote_cleanup();
    }
    return err;
}

/* ---- Voice state helpers -------------------------------------------------- */
/* The reactive "Siri-style" waveform face is the PRIMARY visual during a voice
 * session: each helper drives emote_face_set_state(), which shows the baked
 * reactive anim object and hides the eye (emote_set_anim_visible(false)). The
 * short status label is still posted via EMOTE_MGR_EVT_SYS so the text strip
 * under the face stays informative. On voice-idle we turn the face OFF, which
 * restores the eye + the Wi-Fi/model status idle screen (emote_render_status).
 *
 * Best-effort + graceful fallback: if the reactive face assets failed to load
 * (emote_face_init found none), emote_face_set_state is a no-op on the object
 * and the eye anim set here keeps the device usable. So we ALSO set a matching
 * eye emoji as a fallback — when the reactive face is active it hides the eye,
 * when it isn't the eye shows. One code path, both behaviors. */

/* NOTE on ordering: emote_apply() -> emote_set_anim_emoji() re-shows the eye
 * (gfx_obj_set_visible(true), emote_op.c:289). emote_face_set_state() hides the
 * eye (emote_set_anim_visible(false)). So the face call MUST come AFTER apply,
 * or the eye would be re-shown on top of the waveform. We set the eye emoji
 * first (the graceful fallback if reactive assets are absent), then hand the
 * display to the reactive face. */

esp_err_t emote_set_connecting(void)
{
    s_voice_visual_active = true;
    emote_face_set_state(EMOTE_FACE_THINKING);
    return ESP_OK;
}

esp_err_t emote_set_listening(void)
{
    s_voice_visual_active = true;
    emote_face_set_state(EMOTE_FACE_LISTENING);
    return ESP_OK;
}

esp_err_t emote_set_thinking(void)
{
    s_voice_visual_active = true;
    emote_face_set_state(EMOTE_FACE_THINKING);
    return ESP_OK;
}

esp_err_t emote_set_speaking(void)
{
    s_voice_visual_active = true;
    /* emote_apply() acquires the emote lock, which the render task holds mid-flush.
     * Calling it from the voice session task deadlocks when the render task is busy.
     * The waveform face is the primary visual during speaking; emote_face_set_state()
     * is now a pure state write — the rwave_task applies visibility on its next tick. */
    emote_face_set_state(EMOTE_FACE_SPEAKING);
    return ESP_OK;
}

esp_err_t emote_set_voice_idle(void)
{
    s_voice_visual_active = false;
    /* Stay on the waveform IDLE breathing face after a session ends — the
     * device is a voice assistant, the reactive face IS its identity. The eye
     * + Wi-Fi status overlay still renders behind whenever the face is OFF
     * (no asset, or set via /api/display/face) for diagnostic use. */
    emote_face_set_state(EMOTE_FACE_IDLE);
    return ESP_OK;
}
