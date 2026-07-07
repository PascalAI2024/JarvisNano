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
#include "expression_emote.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gfx.h"
#include "display_arbiter.h"
#include "jarvis_board.h"

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
/* Last time a snapshot consumer was seen (ms tick, 0 = never). Written by the
 * httpd task, read lock-free by the render task per strip — a 32-bit aligned
 * store/load is atomic on Xtensa, and unsigned-subtract compare is wrap-safe. */
static volatile uint32_t s_snapshot_consumer_ms;
static SemaphoreHandle_t s_flush_done;
static bool s_flush_sync_registered;
static volatile bool s_flush_handoff_grace;
static uint32_t s_flush_timeouts;

#define EMOTE_FLUSH_WAIT_MS        80
#define EMOTE_FLUSH_HANDOFF_WAIT_MS 200
#define EMOTE_FLUSH_TIMEOUT_LIMIT  3
#define EMOTE_FLUSH_STRIP_ROWS     12
/* P2.6: how long after the last /api/display/snapshot* read the mirror keeps
 * recording flushes before idling off. */
#define EMOTE_SNAPSHOT_CONSUMER_IDLE_MS 10000

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

    bool handoff_grace = s_flush_handoff_grace;
    s_flush_handoff_grace = false;
    uint32_t wait_ms = handoff_grace ? EMOTE_FLUSH_HANDOFF_WAIT_MS : EMOTE_FLUSH_WAIT_MS;
    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
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

static void emote_on_owner_changed(display_arbiter_owner_t owner, void *user_ctx)
{
    (void)user_ctx;

    if (owner != DISPLAY_ARBITER_OWNER_EMOTE || !s_emote_handle) {
        return;
    }

    /* The Lua/UI owner (display_hal) registers its own on_color_trans_done on
     * the shared panel_io while it holds the display, then nulls it on destroy
     * — either way it clobbers emote's flush-sync callback (the serial log
     * shows "Callback on_color_trans_done was already set and now it was
     * overwritten!"). If we don't re-attach ours every time we regain the
     * display, emote_flush_sync_wait() blocks on a semaphore that never fires:
     * the render loop keeps ticking and the framebuffer keeps updating, but the
     * panel freezes on the last frame.
     *
     * Re-point the HW callback back at us DIRECTLY rather than via
     * emote_register_flush_sync() — its guard would no-op (s_flush_sync_registered
     * is still true; emote_draw() early-returns while LUA owns the display so no
     * timeouts accrued to flip it). Crucially we do NOT touch s_flush_sync_registered
     * or drain s_flush_done here: the render task may be concurrently inside
     * emote_flush_sync_wait() at the same priority, and mutating that shared state
     * would race it (a lost wakeup → spurious 80 ms stall). Writing the io
     * callback pointer is a single aligned store the flush ISR reads — safe. The
     * next flush signals normally. */
    if (s_io_handle && s_flush_sync_registered) {
        const esp_lcd_panel_io_callbacks_t callbacks = {
            .on_color_trans_done = emote_panel_io_done_cb,
        };
        esp_err_t cb_err = esp_lcd_panel_io_register_event_callbacks(s_io_handle, &callbacks, NULL);
        if (cb_err != ESP_OK) {
            ESP_LOGW(TAG, "re-attach flush callback after handback failed: %s", esp_err_to_name(cb_err));
        } else {
            s_flush_handoff_grace = true;
        }
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

void emote_display_snapshot_notify_consumer(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now == 0) {
        now = 1; /* 0 is reserved for "never seen a consumer" */
    }
    s_snapshot_consumer_ms = now;
}

static bool emote_snapshot_consumer_active(void)
{
    uint32_t last = s_snapshot_consumer_ms;
    if (last == 0) {
        return false;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return (uint32_t)(now - last) <= EMOTE_SNAPSHOT_CONSUMER_IDLE_MS;
}

static void emote_snapshot_record_flush(int x_start, int y_start, int x_end, int y_end,
                                        const void *data)
{
    /* P2.6: the mirror only records while someone is watching. With no
     * snapshot read in the last EMOTE_SNAPSHOT_CONSUMER_IDLE_MS this is a
     * lock-free u32 compare and early return — zero per-strip mutex takes,
     * zero memcpy (~13 MB/s of PSRAM write traffic reclaimed), and the
     * 434 KB mirror buffer is not even allocated until a consumer shows up. */
    if (!emote_snapshot_consumer_active()) {
        return;
    }

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

    /* Every read re-arms the mirror. The first request after an idle gap
     * serves whatever is in the buffer (one possibly stale frame — accepted
     * per P2.6); the next flush refreshes it. */
    emote_display_snapshot_notify_consumer();

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

    /* Once the buffer exists the snapshot is served as-is even before the
     * first armed flush lands (valid=false -> zeroed/blank frame): "respond
     * with whatever is available" (P2.6). The flush we just re-armed
     * refreshes it within one engine frame. */
    return ESP_OK;
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
    jarvis_board_display_t display = {0};
    ESP_RETURN_ON_ERROR(jarvis_board_display_get(&display), TAG, "jarvis board display init failed");
    ESP_RETURN_ON_FALSE(display.panel && display.io, ESP_ERR_INVALID_STATE, TAG,
                        "jarvis board display handles are NULL");

    s_panel_handle = display.panel;
    s_io_handle = display.io;
    s_lcd_width = display.width;
    s_lcd_height = display.height;
    emote_register_flush_sync();
    return ESP_OK;
}

static emote_config_t emote_get_default_config(void)
{
    bool swap = true;
    jarvis_board_display_t display = {0};
    if (jarvis_board_display_get(&display) == ESP_OK) {
        swap = display.swap_color_bytes;
    }

    emote_config_t config = {
        .flags = {
            .swap = swap,
            .double_buffer = true,
            .buff_dma = true,
        },
        .gfx_emote = {
            .h_res = s_lcd_width,
            .v_res = s_lcd_height,
            /* 24 fps: panel ceiling is ~23 fps (466*466*2 B/frame over 4-line QSPI @ 20 MHz pclk ≈ 10 MB/s) — 30 just rendered frames the panel can't take (P2.7) */
            .fps = 24,
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
static volatile bool s_voice_visual_active;
/* When set, a persistent alert overlay (emote_set_alert) owns the screen.
 * emote_render_status() and emote_set_status_detail() early-return so a late
 * status/detail write can't clobber the alert mid-session. Cleared only by
 * emote_clear_alert(). */
static volatile bool s_alert_active;

/* Ambient watch hooks are intentionally inert for now. The previous version
 * pushed EVT_BAT/EVT_IDLE while the gfx render task was decoding EAF frames,
 * and the board reproduced LoadProhibited panics in esp_emote_gfx. Keep the
 * API so touch_demo can compile; re-enable only through a serialized emote path. */
esp_err_t emote_set_battery_info(uint8_t pct, bool charging)
{
    (void)pct;
    (void)charging;
    return ESP_OK;
}

esp_err_t emote_refresh_ambient_watch(void)
{
    return ESP_OK;
}

static esp_err_t emote_render_status(void)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "emote handle is NULL");

    /* A latched alert — or an active voice session's reactive face — owns the
     * screen; don't let a status repaint clobber it. Mirrors the dual guard in
     * emote_set_status_detail() so emote_clear_alert()/network-status writes
     * can't repaint the idle face over a live waveform. */
    if (s_alert_active || s_voice_visual_active) {
        return ESP_OK;
    }

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
    if (s_voice_visual_active || s_alert_active) {
        return ESP_OK;
    }
    /* If the emote engine is up, reflect the new detail now; otherwise the
     * next emote_set_network_status() will pick it up. */
    if (s_emote_handle != NULL) {
        return emote_render_status();
    }
    return ESP_OK;
}

esp_err_t emote_set_alert(emote_alert_t kind, const char *line)
{
    (void)kind; /* "offline" is the only baked trouble face; reserved for future kinds */

    /* Latch FIRST so any concurrent status/detail repaint early-returns. */
    s_alert_active = true;

    /* If the engine isn't up yet, the flag is enough — the alert renders once
     * emote_start() brings the display online and a refresh runs. */
    if (s_emote_handle == NULL) {
        return ESP_OK;
    }

    const char *msg = (line && line[0] != '\0') ? line : "Hardware fault";
    /* Dark-visor "something's wrong" look + short strip line. Same pure state
     * writes as emote_apply(); we refresh only when emote owns the display
     * (the safe pattern emote_apply uses) so this never deadlocks the render
     * task from a caller context. */
    esp_err_t err = emote_set_anim_emoji(s_emote_handle, "offline");
    esp_err_t msg_err = emote_set_event_msg(s_emote_handle, EMOTE_MGR_EVT_SYS, msg);
    if (err == ESP_OK) {
        err = msg_err;
    }
    emote_face_set_state(EMOTE_FACE_OFF);
    if (display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        esp_err_t r = emote_notify_all_refresh(s_emote_handle);
        if (err == ESP_OK) {
            err = r;
        }
    }
    return err;
}

esp_err_t emote_clear_alert(void)
{
    if (!s_alert_active) {
        return ESP_OK;
    }
    s_alert_active = false;
    /* Restore the normal idle/status screen (neutral face when online,
     * offline-with-Wi-Fi-message when not). */
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
