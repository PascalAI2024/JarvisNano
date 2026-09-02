/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Physical display presenter for the Waveshare ESP32-S3 Touch AMOLED 1.75.
 *
 * The application-facing callbacks only publish a packed intent word and wake
 * this task. This task is the sole application owner of esp_emote_gfx. The gfx
 * library's internal renderer owns frame decoding; the panel callback only
 * submits an asynchronous QSPI transfer, and the panel ISR releases the gfx
 * flush wait. Voice/audio tasks therefore never wait on flash, PSRAM, or LCD.
 */
#include "jr_display/jr_display.h"
#include <stdatomic.h>
#include "jr_display/hud_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gfx.h"
#include "jarvis_board.h"

static const char *TAG = "jr_display";

#define JR_DISPLAY_MOUNT_POINT       "/emote"
#define JR_DISPLAY_PARTITION         "emote_assets"
/* Live spatial/OTA exercise: 2,288 B free at 5,120. 4,608 retains ~1.77 KB
 * measured margin and returns 512 B of internal SRAM. */
#define JR_DISPLAY_TASK_STACK        4608
#define JR_DISPLAY_TASK_PRIORITY     3
#define JR_DISPLAY_TASK_CORE         0
/* Right-sized 2026-07-19 from measured high-water: peak use 1,784 B under a
 * full exercise (voice turns + transitions + snapshot), leaving 3,336 B margin
 * — 187% headroom. Was 12288. Internal RAM is the binding resource on this
 * board; see docs/JARVISNANO_OS_PLAN.md "Internal RAM budget". Re-measure via
 * /api/diag/tasks after any render change before trusting this number. */
#define JR_DISPLAY_RENDER_STACK      5120
#define JR_DISPLAY_RENDER_PRIORITY   4
#define JR_DISPLAY_RENDER_CORE       0
/* 24, not 30: the measured CO5300 QSPI ceiling is ~23 fps full-frame — v4
 * shipped 24 for exactly this reason (P2.7); 30 only renders frames the
 * panel cannot take (live counter confirmed 20 actual at fps=30). */
#define JR_DISPLAY_RENDER_FPS        24
/* 12-row internal-SRAM DMA strips — the live-safe memory configuration.
 * A 20-row hardware A/B left FPS unchanged at 12 while shrinking the largest
 * internal block from 32 KB to 12.8 KB, proving strip waits were not the
 * current bottleneck. PSRAM strips still require a failing bounce allocation. */
#define JR_DISPLAY_STRIP_ROWS        12
#define JR_DISPLAY_DRIVER_MS         33
#define JR_DISPLAY_AMP_BUCKETS       8U
#define JR_DISPLAY_DECAY_HOLD_MS     600U
#define JR_DISPLAY_ASSET_MAX         (2U * 1024U * 1024U)
#define JR_DISPLAY_SNAPSHOT_ACTIVE_MS 1500U
#define JR_DISPLAY_SNAPSHOT_WAIT_MS   750U

#define JR_DISPLAY_CMD_BLANK         0x00010000U
#define JR_DISPLAY_CMD_FACE_SHIFT    8U
#define JR_DISPLAY_CMD_FACE_MASK     0x0000ff00U
#define JR_DISPLAY_CMD_AMP_MASK      0x000000ffU

#define JR_DISPLAY_SHELL_SHADE       0x80000000U
#define JR_DISPLAY_SHELL_AGENT       0x40000000U
#define JR_DISPLAY_SHELL_STATE_SHIFT 8U
#define JR_DISPLAY_SHELL_STATE_MASK  0x00000f00U
#define JR_DISPLAY_SHELL_PROGRESS    0x000000ffU

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t total_frames;
    uint32_t last_frame;
} jr_display_clip_t;

typedef struct {
    TaskHandle_t task;
    jarvis_board_display_t board;
    gfx_handle_t gfx;
    gfx_disp_t *disp;
    gfx_obj_t *anim;

    /* Exact software mirror of panel submissions. It is allocated only after
     * a diagnostics consumer asks and is updated for 1.5 seconds at a time.
     * A private capture buffer is swapped with the immutable published frame
     * only after every accepted strip of a full frame has arrived. */
    SemaphoreHandle_t snapshot_lock;
    uint16_t *snapshot_rgb565;
    uint16_t *snapshot_capture_rgb565;
    size_t snapshot_bytes;
    volatile uint32_t snapshot_initialized;
    volatile uint32_t snapshot_consumer_ms;
    volatile uint32_t snapshot_refresh_requested;
    volatile uint32_t snapshot_frame_dropped;
    uint16_t snapshot_next_y;
    uint64_t snapshot_frame_id;
    uint64_t snapshot_last_flush_ms;
    bool snapshot_valid;
    volatile uint32_t test_pattern;
    volatile int32_t challenge_sector;
    volatile uint32_t challenge_progress;
    volatile uint32_t shell_word;

    /* Resident clip cache: every face's EAF stays in PSRAM after first load
     * (~3.9 MB total of ~7.4 MB free; v4 kept clips resident too). Reloading
     * ~1 MB from SPIFFS per face change dropped 1-2 frames (whole-payload
     * checksum under the render lock) and churned the PSRAM heap for hours. */
    jr_display_clip_t clips[JR_FACE_COUNT];
    const jr_display_clip_t *active;    /* cache entry bound to the gfx anim */
    jr_face_t loaded_face;
    volatile bool blanked;
    volatile int applied_bucket;
    int last_end;
    uint64_t decay_gate_ms;
    uint64_t apply_retry_gate_ms;   /* backoff after a failed apply_face */

    volatile uint32_t requested_word;
    volatile uint32_t started;
    volatile uint32_t init_state;
    volatile int32_t last_error;
    volatile uint32_t task_running;
    volatile uint32_t requested_face;
    volatile uint32_t applied_face;
    volatile uint32_t requested_amplitude;
    volatile uint32_t requests;
    volatile uint32_t state_changes;
    volatile uint32_t segment_sets;
    volatile uint32_t asset_load_failures;
    volatile uint32_t flush_submissions;
    volatile uint32_t flush_completions;
    volatile uint32_t flush_errors;
    volatile uint32_t actual_fps;
    volatile uint32_t current_asset_bytes;
    volatile uint32_t free_psram_bytes;
    volatile uint32_t task_stack_hwm;
} jr_display_ctx_t;

static jr_display_ctx_t s_display = {
    .requested_word = ((uint32_t)JR_FACE_IDLE << JR_DISPLAY_CMD_FACE_SHIFT),
    .init_state = JR_DISPLAY_INIT_STOPPED,
    .last_error = ESP_OK,
    .requested_face = JR_FACE_IDLE,
    .applied_face = JR_FACE_IDLE,
    .loaded_face = JR_FACE_IDLE,
    .applied_bucket = -1,
    .last_end = -1,
    .test_pattern = JR_DISPLAY_TEST_OFF,
    .challenge_sector = 0,
};
static portMUX_TYPE s_snapshot_init_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_surface_mux = portMUX_INITIALIZER_UNLOCKED;
static jr_display_surface_t s_surface;
static bool s_surface_active;

static inline void diag_inc(volatile uint32_t *value)
{
    __atomic_add_fetch(value, 1U, __ATOMIC_RELAXED);
}

static inline uint32_t diag_load(const volatile uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static inline void diag_store(volatile uint32_t *value, uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELAXED);
}

static inline uint32_t diag_exchange(volatile uint32_t *value, uint32_t next)
{
    return __atomic_exchange_n(value, next, __ATOMIC_ACQ_REL);
}

static void display_publish(uint32_t word)
{
    __atomic_store_n(&s_display.requested_word, word, __ATOMIC_RELEASE);
    diag_store(&s_display.requested_face,
               (word & JR_DISPLAY_CMD_FACE_MASK) >> JR_DISPLAY_CMD_FACE_SHIFT);
    diag_store(&s_display.requested_amplitude, word & JR_DISPLAY_CMD_AMP_MASK);
    diag_inc(&s_display.requests);

    TaskHandle_t task = __atomic_load_n(&s_display.task, __ATOMIC_ACQUIRE);
    if (task) {
        xTaskNotifyGive(task);
    }
}

static void display_blank_cb(void *ctx)
{
    (void)ctx;
    display_publish(JR_DISPLAY_CMD_BLANK |
                    ((uint32_t)JR_FACE_IDLE << JR_DISPLAY_CMD_FACE_SHIFT));
}

static void display_present_cb(void *ctx, jr_face_t face, uint8_t amplitude)
{
    (void)ctx;
    if (face < JR_FACE_IDLE || face >= JR_FACE_COUNT) {
        face = JR_FACE_ERROR;
    }
    display_publish(((uint32_t)face << JR_DISPLAY_CMD_FACE_SHIFT) | amplitude);
}

static const char *face_asset(jr_face_t face)
{
    switch (face) {
    case JR_FACE_IDLE:      return JR_DISPLAY_MOUNT_POINT "/rwave_idle.eaf";
    case JR_FACE_LISTENING: return JR_DISPLAY_MOUNT_POINT "/rwave_listen.eaf";
    case JR_FACE_THINKING:  return JR_DISPLAY_MOUNT_POINT "/rwave_think.eaf";
    case JR_FACE_SPEAKING:  return JR_DISPLAY_MOUNT_POINT "/rwave_speak.eaf";
    case JR_FACE_ERROR:     return JR_DISPLAY_MOUNT_POINT "/error.eaf";
    case JR_FACE_RESTING:   return JR_DISPLAY_MOUNT_POINT "/rwave_rest.eaf";
    case JR_FACE_MUTED:     return JR_DISPLAY_MOUNT_POINT "/rwave_muted.eaf";
    case JR_FACE_LINKING:   return JR_DISPLAY_MOUNT_POINT "/rwave_link.eaf";
    default:                return NULL;
    }
}

static uint32_t face_fps(jr_face_t face)
{
    switch (face) {
    case JR_FACE_ERROR:   return 8;
    /* The quiet faces are baked slow on purpose: one breath per 3 s loop at
     * rest, a 2 s orbit while linking. Fewer decoded frames per second is
     * also the cheapest render-cadence win the rest ladder gets for free. */
    case JR_FACE_RESTING: return 8;
    case JR_FACE_MUTED:   return 8;
    case JR_FACE_LINKING: return 12;
    default:              return 24;   /* engine + panel ceiling (P2.7) */
    }
}

static uint32_t eaf_frame_count(const uint8_t *data, size_t size)
{
    if (!data || size < 24 || data[0] != 0x89 ||
            (memcmp(data + 1, "EAF", 3) != 0 && memcmp(data + 1, "AAF", 3) != 0)) {
        return 0;
    }

    uint32_t total = 0;
    uint32_t payload_size = 0;
    memcpy(&total, data + 4, sizeof(total));
    memcpy(&payload_size, data + 12, sizeof(payload_size));
    if (total < 2 || total > 1024 ||
            16U + (size_t)total * 8U > size ||
            payload_size > size - 16U) {
        return 0;
    }
    return total;
}

static esp_err_t clip_load(jr_face_t face, jr_display_clip_t *out)
{
    const char *path = face_asset(face);
    if (!path || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "face asset missing: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ESP_FAIL;
    if (fseek(file, 0, SEEK_END) != 0) {
        goto done;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || (uint32_t)file_size > JR_DISPLAY_ASSET_MAX ||
            fseek(file, 0, SEEK_SET) != 0) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    out->size = (size_t)file_size;
    out->data = heap_caps_malloc(out->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out->data) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    if (fread(out->data, 1, out->size, file) != out->size || ferror(file)) {
        err = ESP_FAIL;
        goto done;
    }

    out->total_frames = eaf_frame_count(out->data, out->size);
    if (out->total_frames < 2) {
        err = ESP_ERR_INVALID_CRC;
        goto done;
    }
    out->last_frame = out->total_frames - 1U;
    err = ESP_OK;

done:
    fclose(file);
    if (err != ESP_OK) {
        heap_caps_free(out->data);
        memset(out, 0, sizeof(*out));
    }
    return err;
}

static esp_err_t snapshot_ensure_buffer(jr_display_ctx_t *ctx)
{
    if (__atomic_load_n(&ctx->snapshot_initialized, __ATOMIC_ACQUIRE) != 0U) {
        return ESP_OK;
    }
    if (ctx->board.width == 0 || ctx->board.height == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes = (size_t)ctx->board.width * (size_t)ctx->board.height *
                   sizeof(uint16_t);
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    uint16_t *published = heap_caps_calloc(
        1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *capture = heap_caps_calloc(
        1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (lock == NULL || published == NULL || capture == NULL) {
        if (lock != NULL) {
            vSemaphoreDelete(lock);
        }
        heap_caps_free(published);
        heap_caps_free(capture);
        return ESP_ERR_NO_MEM;
    }

    /* Allocate outside the critical section, then install exactly one complete
     * buffer pair. The initialized release-store is the only fast-path signal;
     * rotating frame pointers are never inspected without snapshot_lock. */
    bool installed = false;
    taskENTER_CRITICAL(&s_snapshot_init_mux);
    if (diag_load(&ctx->snapshot_initialized) == 0U) {
        ctx->snapshot_bytes = bytes;
        ctx->snapshot_lock = lock;
        ctx->snapshot_capture_rgb565 = capture;
        ctx->snapshot_rgb565 = published;
        __atomic_store_n(&ctx->snapshot_initialized, 1U, __ATOMIC_RELEASE);
        installed = true;
    }
    taskEXIT_CRITICAL(&s_snapshot_init_mux);
    if (installed) {
        ESP_LOGI(TAG, "display mirror armed: 2 x %u bytes PSRAM",
                 (unsigned)bytes);
    } else {
        vSemaphoreDelete(lock);
        heap_caps_free(published);
        heap_caps_free(capture);
    }
    return ESP_OK;
}

static uint16_t panel_order_color(const jr_display_ctx_t *ctx, uint16_t rgb565)
{
    return ctx->board.swap_color_bytes ? __builtin_bswap16(rgb565) : rgb565;
}

/* Eight forgiving angular sectors, clockwise from 12 o'clock. The 0.414
 * boundary is tan(22.5 deg), so cardinal and diagonal wedges match what a
 * human sees without atan2 in the per-pixel render path. */
static int challenge_sector_from_vector(int dx, int dy)
{
    int ax = abs(dx);
    int ay = abs(dy);
    if (ax * 1000 < ay * 414) {
        return dy < 0 ? 0 : 4;
    }
    if (ay * 1000 < ax * 414) {
        return dx > 0 ? 2 : 6;
    }
    if (dx > 0) {
        return dy < 0 ? 1 : 3;
    }
    return dy < 0 ? 7 : 5;
}

static uint16_t challenge_pixel(const jr_display_ctx_t *ctx, int x, int y)
{
    int dx = 2 * x - ((int)ctx->board.width - 1);
    int dy = 2 * y - ((int)ctx->board.height - 1);
    int radius2 = dx * dx + dy * dy;
    const int inner = 2 * 145;
    const int outer = 2 * 222;
    int expected = __atomic_load_n(&ctx->challenge_sector, __ATOMIC_RELAXED);
    uint32_t progress = diag_load(&ctx->challenge_progress);
    bool success = expected < 0 && progress >= 3U;
    uint16_t native = 0x0000;

    if (radius2 >= inner * inner && radius2 <= outer * outer) {
        int sector = challenge_sector_from_vector(dx, dy);
        native = success ? 0x07E0 : sector == expected ? 0x07FF : 0x0821;
        /* Thin white divider rays make the eight hit regions unambiguous. */
        int ax = abs(dx);
        int ay = abs(dy);
        if (ax < 3 || ay < 3 || abs(ax - ay) < 4) {
            native = success ? 0x07E0 : 0x7BEF;
        }
    }

    /* Reactor center plus three explicit round markers: cyan as rounds pass,
     * dark blue while waiting. The success state turns the whole core green. */
    if (radius2 < 24 * 24) {
        native = success ? 0x07E0 : 0x03EF;
    }
    for (int i = 0; i < 3; ++i) {
        int dot_dx = x - ((int)ctx->board.width / 2 - 24 + i * 24);
        int dot_dy = y - ((int)ctx->board.height / 2 + 42);
        if (dot_dx * dot_dx + dot_dy * dot_dy <= 6 * 6) {
            native = success || (uint32_t)i < progress ? 0x07E0 : 0x0861;
        }
    }
    return panel_order_color(ctx, native);
}

static uint16_t test_pattern_pixel(const jr_display_ctx_t *ctx,
                                   jr_display_test_pattern_t pattern,
                                   int x, int y)
{
    uint16_t native = 0;
    switch (pattern) {
    case JR_DISPLAY_TEST_COLOR_BARS: {
        static const uint16_t bars[] = {
            0xF800, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F,
        };
        size_t index = ((size_t)x * (sizeof bars / sizeof bars[0])) /
                       (ctx->board.width ? ctx->board.width : 1U);
        if (index >= sizeof bars / sizeof bars[0]) {
            index = sizeof bars / sizeof bars[0] - 1U;
        }
        native = bars[index];
        /* White cross + black center marker prove orientation and geometry. */
        if (x == (int)ctx->board.width / 2 || y == (int)ctx->board.height / 2) {
            native = 0xFFFF;
        }
        if (abs(x - (int)ctx->board.width / 2) < 5 &&
            abs(y - (int)ctx->board.height / 2) < 5) {
            native = 0x0000;
        }
        break;
    }
    case JR_DISPLAY_TEST_GRID: {
        bool major = (x % 64) < 3 || (y % 64) < 3;
        bool minor = (x % 16) == 0 || (y % 16) == 0;
        native = major ? 0x07FF : minor ? 0x39E7 : 0x0000;
        if (x < 4 || y < 4 || x >= (int)ctx->board.width - 4 ||
            y >= (int)ctx->board.height - 4) {
            native = 0xFFFF;
        }
        break;
    }
    case JR_DISPLAY_TEST_WHITE: native = 0xFFFF; break;
    case JR_DISPLAY_TEST_RED:   native = 0xF800; break;
    case JR_DISPLAY_TEST_GREEN: native = 0x07E0; break;
    case JR_DISPLAY_TEST_BLUE:  native = 0x001F; break;
    case JR_DISPLAY_TEST_TOUCH_CHALLENGE:
        return challenge_pixel(ctx, x, y);
    case JR_DISPLAY_TEST_OFF:
    default:                    native = 0x0000; break;
    }
    return panel_order_color(ctx, native);
}

/* ---- pushed canvas: a full-frame RGB565 image supplied over the network
 * (companion tool / JarvisMCP) that temporarily replaces the face, exactly
 * like a test pattern does. The buffer lives in PSRAM, is written ONLY by
 * jr_display_canvas_show (single copy, pre-converted to panel byte order),
 * and read ONLY by the render task; `s_canvas_until_ms` gives it a TTL so an
 * abandoned push can never permanently cover the face. The word is the
 * INTENT; the render task eases its own presentation level toward it (see
 * overlay_fade_tick), so arrivals and departures crossfade with the face
 * instead of popping. A show during the exit fade re-targets the SAME buffer
 * with the new pixels already in it — accepted: a low-level content swap is
 * barely visible and a second full-frame buffer is not worth it. ---- */
static uint16_t         *s_canvas;              /* PSRAM, 466*466 */
static volatile uint32_t s_canvas_until_ms;     /* 0 = inactive   */

/* TRANS: watch + canvas entrance/exit easing. RENDER-TASK-OWNED — written
 * only by overlay_fade_tick (once per frame, from panel_flush) and read only
 * by the appliers on the same task, so nothing here needs atomics. The
 * published intent words (s_canvas_until_ms, s_clock_word) remain the only
 * cross-task surface: they carry the INTENT (on/off, active/expired); these
 * carry the PRESENTATION, a linear progress that steps toward the intent's
 * target each frame. Stepping from wherever progress currently is — rather
 * than replaying a timestamped curve — makes a mid-fade reversal walk
 * straight back with no snap. The appliers consume smoothstep(progress):
 * ease-in-out is what turns the old single-frame pop into something that
 * reads as the device breathing. */
#define JR_DISPLAY_FADE_MS 400U

static int      s_clock_prog;        /* linear 0..256 toward the on/off bit  */
static int      s_clock_ease;        /* smoothstep(s_clock_prog), 0..256     */
static uint32_t s_clock_shown_word;  /* last on-word: the fade-out must hold
                                      * the last shown time — the off publish
                                      * may zero hh/mm and hands snapping to
                                      * 12:00 mid-fade would be a new pop.   */
static int      s_canvas_prog;
static int      s_canvas_ease;
/* Captions ride a QUICKER fade than the watch/canvas: they follow speech
 * rhythm, and 400 ms of entrance on a rolling caption reads as lag. Only the
 * on/off edges fade — text swaps while visible stay immediate, which is what
 * a live transcript wants. */
#define JR_DISPLAY_CAPTION_FADE_MS 250U
static int      s_caption_prog;
static int      s_caption_ease;
/* The modal ask rides the caption's quick rate — it follows conversation
 * rhythm too. s_ask_shown_n is the render-side latch of the arc count: the
 * fade-OUT keeps drawing the presentation s_choice_n no longer admits to,
 * exactly as the watch fade-out holds its shown time. */
static int      s_ask_prog;
static int      s_ask_ease;
static int      s_ask_shown_n;
/* The shade rides the quick rate too — it answers a gesture, and a veil that
 * lags its swipe feels broken, not graceful. */
static int      s_shade_prog;
static int      s_shade_ease;
static uint32_t s_fade_prev_ms;

static inline int fade_smoothstep(int p)     /* 0..256 -> 0..256, 3p^2-2p^3 */
{
    return (p * p * (768 - 2 * p)) >> 16;
}

esp_err_t jr_display_canvas_show(const uint16_t *rgb565, size_t width,
                                 size_t height, uint32_t ttl_ms)
{
    jr_display_ctx_t *ctx = &s_display;
    if (rgb565 == NULL || width != ctx->board.width ||
        height != ctx->board.height) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ttl_ms == 0U || ttl_ms > 300000U) {
        ttl_ms = 30000U;
    }
    const size_t px = (size_t)ctx->board.width * ctx->board.height;
    if (s_canvas == NULL) {
        s_canvas = heap_caps_malloc(px * sizeof(uint16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_canvas == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    /* convert once at push time so the render task can memcpy rows */
    if (ctx->board.swap_color_bytes) {
        for (size_t i = 0; i < px; ++i) {
            uint16_t v = rgb565[i];
            s_canvas[i] = (uint16_t)((v >> 8) | (v << 8));
        }
    } else {
        memcpy(s_canvas, rgb565, px * sizeof(uint16_t));
    }
    __atomic_store_n(&s_canvas_until_ms,
                     (uint32_t)(esp_timer_get_time() / 1000) + ttl_ms,
                     __ATOMIC_RELEASE);
    return ESP_OK;
}

void jr_display_canvas_clear(void)
{
    __atomic_store_n(&s_canvas_until_ms, 0U, __ATOMIC_RELEASE);
}

bool jr_display_canvas_active(void)
{
    uint32_t until = __atomic_load_n(&s_canvas_until_ms, __ATOMIC_ACQUIRE);
    return until != 0U &&
           (int32_t)((uint32_t)(esp_timer_get_time() / 1000) - until) < 0;
}

static void apply_canvas(jr_display_ctx_t *ctx, int x1, int y1,
                         int x2, int y2, uint16_t *pixels)
{
    /* Gate on the eased level, not on canvas_active(): after a clear or TTL
     * expiry the intent is off but the exit fade still needs the buffer —
     * which outlives the fade (only the next show ever rewrites it). */
    const int e = s_canvas_ease;
    if (pixels == NULL || s_canvas == NULL || e <= 0) {
        return;
    }
    const int width = x2 - x1;
    if (e >= 256) {                          /* settled: rows are a memcpy */
        for (int row = y1; row < y2; ++row) {
            memcpy(pixels + (size_t)(row - y1) * (size_t)width,
                   s_canvas + (size_t)row * ctx->board.width + (size_t)x1,
                   (size_t)width * sizeof(uint16_t));
        }
        return;
    }
    const int m = (e * 32) >> 8;
    if (m <= 0) {
        return;
    }
    const bool swap = ctx->board.swap_color_bytes;
    for (int row = y1; row < y2; ++row) {
        const uint16_t *src =
            s_canvas + (size_t)row * ctx->board.width + (size_t)x1;
        uint16_t *dst = pixels + (size_t)(row - y1) * (size_t)width;
        for (int col = 0; col < width; ++col) {
            dst[col] = hud_mix565(dst[col], src[col], m, swap);
        }
    }
}

static void apply_test_pattern(jr_display_ctx_t *ctx, int x1, int y1,
                               int x2, int y2, uint16_t *pixels)
{
    jr_display_test_pattern_t pattern =
        (jr_display_test_pattern_t)diag_load(&ctx->test_pattern);
    if (pattern == JR_DISPLAY_TEST_OFF || pixels == NULL) {
        return;
    }
    int width = x2 - x1;
    int height = y2 - y1;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            pixels[(size_t)row * (size_t)width + (size_t)col] =
                test_pattern_pixel(ctx, pattern, x1 + col, y1 + row);
        }
    }
}

static uint16_t panel_native(const jr_display_ctx_t *ctx, uint16_t pixel)
{
    return ctx->board.swap_color_bytes ? __builtin_bswap16(pixel) : pixel;
}

/* Compact 5x7 font for the companion card. Columns use bit 0 as the top row.
 * Lowercase is folded to uppercase; unsupported glyphs render as a space. */
static uint8_t surface_glyph_column(char ch, unsigned col)
{
    if (col >= 5U) return 0U;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    const uint8_t *g = NULL;
    static const uint8_t A[] = {0x7e,0x11,0x11,0x11,0x7e};
    static const uint8_t B[] = {0x7f,0x49,0x49,0x49,0x36};
    static const uint8_t C[] = {0x3e,0x41,0x41,0x41,0x22};
    static const uint8_t D[] = {0x7f,0x41,0x41,0x22,0x1c};
    static const uint8_t E[] = {0x7f,0x49,0x49,0x49,0x41};
    static const uint8_t F[] = {0x7f,0x09,0x09,0x09,0x01};
    static const uint8_t G[] = {0x3e,0x41,0x49,0x49,0x7a};
    static const uint8_t H[] = {0x7f,0x08,0x08,0x08,0x7f};
    static const uint8_t I[] = {0x00,0x41,0x7f,0x41,0x00};
    static const uint8_t J[] = {0x20,0x40,0x41,0x3f,0x01};
    static const uint8_t K[] = {0x7f,0x08,0x14,0x22,0x41};
    static const uint8_t L[] = {0x7f,0x40,0x40,0x40,0x40};
    static const uint8_t M[] = {0x7f,0x02,0x0c,0x02,0x7f};
    static const uint8_t N[] = {0x7f,0x04,0x08,0x10,0x7f};
    static const uint8_t O[] = {0x3e,0x41,0x41,0x41,0x3e};
    static const uint8_t P[] = {0x7f,0x09,0x09,0x09,0x06};
    static const uint8_t Q[] = {0x3e,0x41,0x51,0x21,0x5e};
    static const uint8_t R[] = {0x7f,0x09,0x19,0x29,0x46};
    static const uint8_t S[] = {0x46,0x49,0x49,0x49,0x31};
    static const uint8_t T[] = {0x01,0x01,0x7f,0x01,0x01};
    static const uint8_t U[] = {0x3f,0x40,0x40,0x40,0x3f};
    static const uint8_t V[] = {0x1f,0x20,0x40,0x20,0x1f};
    static const uint8_t W[] = {0x3f,0x40,0x38,0x40,0x3f};
    static const uint8_t X[] = {0x63,0x14,0x08,0x14,0x63};
    static const uint8_t Y[] = {0x07,0x08,0x70,0x08,0x07};
    static const uint8_t Z[] = {0x61,0x51,0x49,0x45,0x43};
    static const uint8_t N0[] = {0x3e,0x51,0x49,0x45,0x3e};
    static const uint8_t N1[] = {0x00,0x42,0x7f,0x40,0x00};
    static const uint8_t N2[] = {0x42,0x61,0x51,0x49,0x46};
    static const uint8_t N3[] = {0x21,0x41,0x45,0x4b,0x31};
    static const uint8_t N4[] = {0x18,0x14,0x12,0x7f,0x10};
    static const uint8_t N5[] = {0x27,0x45,0x45,0x45,0x39};
    static const uint8_t N6[] = {0x3c,0x4a,0x49,0x49,0x30};
    static const uint8_t N7[] = {0x01,0x71,0x09,0x05,0x03};
    static const uint8_t N8[] = {0x36,0x49,0x49,0x49,0x36};
    static const uint8_t N9[] = {0x06,0x49,0x49,0x29,0x1e};
    static const uint8_t DASH[] = {0x08,0x08,0x08,0x08,0x08};
    static const uint8_t DOT[] = {0x00,0x60,0x60,0x00,0x00};
    static const uint8_t COLON[] = {0x00,0x36,0x36,0x00,0x00};
    static const uint8_t SLASH[] = {0x20,0x10,0x08,0x04,0x02};
    static const uint8_t PERCENT[] = {0x13,0x0b,0x04,0x32,0x31};
    static const uint8_t QMARK[] = {0x02,0x01,0x51,0x09,0x06};
    static const uint8_t BANG[] = {0x00,0x00,0x5f,0x00,0x00};
    static const uint8_t PLUS[] = {0x08,0x08,0x3e,0x08,0x08};
    static const uint8_t COMMA[] = {0x00,0x60,0x20,0x00,0x00};
    static const uint8_t APOSTROPHE[] = {0x00,0x03,0x03,0x00,0x00};
    static const uint8_t LPAREN[] = {0x00,0x1c,0x22,0x41,0x00};
    static const uint8_t RPAREN[] = {0x00,0x41,0x22,0x1c,0x00};
    static const uint8_t AMP[] = {0x36,0x49,0x55,0x22,0x50};
    switch (ch) {
    case 'A': g=A; break; case 'B': g=B; break; case 'C': g=C; break;
    case 'D': g=D; break; case 'E': g=E; break; case 'F': g=F; break;
    case 'G': g=G; break; case 'H': g=H; break; case 'I': g=I; break;
    case 'J': g=J; break; case 'K': g=K; break; case 'L': g=L; break;
    case 'M': g=M; break; case 'N': g=N; break; case 'O': g=O; break;
    case 'P': g=P; break; case 'Q': g=Q; break; case 'R': g=R; break;
    case 'S': g=S; break; case 'T': g=T; break; case 'U': g=U; break;
    case 'V': g=V; break; case 'W': g=W; break; case 'X': g=X; break;
    case 'Y': g=Y; break; case 'Z': g=Z; break;
    case '0': g=N0; break; case '1': g=N1; break; case '2': g=N2; break;
    case '3': g=N3; break; case '4': g=N4; break; case '5': g=N5; break;
    case '6': g=N6; break; case '7': g=N7; break; case '8': g=N8; break;
    case '9': g=N9; break; case '-': case '_': g=DASH; break;
    case '.': g=DOT; break; case ':': g=COLON; break; case '/': g=SLASH; break;
    case '%': g=PERCENT; break;
    case '?': g=QMARK; break; case '!': g=BANG; break; case '+': g=PLUS; break;
    case ',': g=COMMA; break; case '\'': g=APOSTROPHE; break;
    case '(': g=LPAREN; break; case ')': g=RPAREN; break; case '&': g=AMP; break;
    default: return 0U;
    }
    return g[col];
}

static bool surface_text_pixel(const char *text, size_t max_chars,
                               int x, int y, int x0, int y0, int scale)
{
    if (text == NULL || scale <= 0 || x < x0 || y < y0 ||
        y >= y0 + 7 * scale) return false;
    int rel_x = x - x0;
    size_t index = (size_t)(rel_x / (6 * scale));
    if (index >= max_chars || text[index] == '\0') return false;
    int glyph_x = (rel_x % (6 * scale)) / scale;
    if (glyph_x >= 5) return false;
    int glyph_y = (y - y0) / scale;
    return (surface_glyph_column(text[index], (unsigned)glyph_x) &
            (1U << glyph_y)) != 0U;
}

static uint16_t surface_accent(jr_display_surface_kind_t kind)
{
    switch (kind) {
    case JR_DISPLAY_SURFACE_RESULT:  return 0x07e0;
    case JR_DISPLAY_SURFACE_CONSENT: return 0xfd20;
    case JR_DISPLAY_SURFACE_CHOICE:  return 0x07ff;
    case JR_DISPLAY_SURFACE_PROGRESS:return 0xa81f;
    case JR_DISPLAY_SURFACE_NOTICE:
    default:                         return 0x681f;
    }
}

static void surface_split_body(const char *body, char first[25], char second[25])
{
    size_t length = strnlen(body, JR_DISPLAY_SURFACE_BODY_CAP);
    size_t split = length > 24U ? 24U : length;
    /* Split exactly at the display boundary. Word-wrapping at an earlier
     * space could silently discard characters from a 48-character consent
     * body, which is unacceptable when the text itself is the authority. */
    memcpy(first, body, split);
    first[split] = '\0';
    size_t next = split;
    size_t remain = length - next;
    if (remain > 24U) remain = 24U;
    memcpy(second, body + next, remain);
    second[remain] = '\0';
}

static void apply_surface_overlay(jr_display_ctx_t *ctx, int x1, int y1,
                                  int x2, int y2, uint16_t *pixels)
{
    jr_display_surface_t surface;
    bool active;
    taskENTER_CRITICAL(&s_surface_mux);
    active = s_surface_active;
    surface = s_surface;
    taskEXIT_CRITICAL(&s_surface_mux);
    if (!active || pixels == NULL) return;

    char body_first[25] = {0};
    char body_second[25] = {0};
    surface_split_body(surface.body, body_first, body_second);
    uint16_t accent = surface_accent(surface.kind);
    int width = x2 - x1;
    for (int row = 0; row < y2 - y1; ++row) {
        int y = y1 + row;
        for (int col = 0; col < width; ++col) {
            int x = x1 + col;
            uint16_t *slot = &pixels[(size_t)row * (size_t)width + (size_t)col];
            uint16_t native = panel_native(ctx, *slot);
            /* A ROUND PLATE ON ROUND GLASS.
             *
             * This card was a rectangle spanning x 52..413, y 72..388. Its
             * corners sit at r~242 on a panel whose glass ends at r232.5, so
             * the bezel sliced all four of them off — and because everything
             * outside the rectangle was darkened, the card also flattened the
             * face, the orbit and the rim into a grey surround. On a circular
             * display a rectangle cannot be centred, only cropped.
             *
             * The plate is now the shell disc itself, bounded by the same
             * JR_DISPLAY_SHELL_R_MAX every other shell primitive obeys, with
             * the accent as a ring around its edge instead of four straight
             * borders. Every text row and both action buttons already fall
             * inside r214 (the farthest corner of a button is r~203), so no
             * content moved.
             *
             * Outside the plate is left ALONE rather than darkened: r215-222
             * is where the battery arc and the gold privacy ring live, and a
             * card has no business dimming the indicator that says the
             * microphone is off. */
            const int cdx = x - (int)(HUD_W / 2);
            const int cdy = y - (int)(HUD_H / 2);
            const int cr2 = cdx * cdx + cdy * cdy;
            const bool card = cr2 <= JR_DISPLAY_SHELL_R_MAX *
                                     JR_DISPLAY_SHELL_R_MAX;
            if (!card) {
                /* rim tenants keep their full brightness */
            } else {
                const bool border = cr2 >= (JR_DISPLAY_SHELL_R_MAX - 6) *
                                           (JR_DISPLAY_SHELL_R_MAX - 6);
                native = border ? accent : 0x0842;
                if (surface_text_pixel(surface.title, 24U, x, y, 88, 128, 2) ||
                    surface_text_pixel(body_first, 24U, x, y, 88, 184, 2) ||
                    surface_text_pixel(body_second, 24U, x, y, 88, 210, 2)) {
                    native = 0xffff;
                }
                uint8_t count = surface.action_count;
                if (count > JR_DISPLAY_SURFACE_ACTION_CAP)
                    count = JR_DISPLAY_SURFACE_ACTION_CAP;
                if (count > 0U && y >= 300 && y <= 354) {
                    const int left = 70, right = 396, gap = 8;
                    int button_w = (right - left - gap * ((int)count - 1)) /
                                   (int)count;
                    for (uint8_t i = 0; i < count; ++i) {
                        int bx0 = left + (int)i * (button_w + gap);
                        int bx1 = bx0 + button_w;
                        if (x >= bx0 && x <= bx1) {
                            bool edge = x <= bx0 + 2 || x >= bx1 - 2 ||
                                        y <= 302 || y >= 352;
                            native = edge ? accent : 0x1084;
                            size_t label_len = strnlen(
                                surface.action_labels[i],
                                JR_DISPLAY_SURFACE_LABEL_CAP);
                            int label_x = bx0 + (button_w - (int)label_len * 6) / 2;
                            if (surface_text_pixel(surface.action_labels[i],
                                    JR_DISPLAY_SURFACE_LABEL_CAP - 1U,
                                    x, y, label_x, 324, 1)) native = 0xffff;
                        }
                    }
                }
            }
            *slot = panel_order_color(ctx, native);
        }
    }
}

static uint16_t agent_native_color(jr_display_agent_state_t state)
{
    switch (state) {
    case JR_DISPLAY_AGENT_WAITING:   return 0xFD20; /* amber  */
    case JR_DISPLAY_AGENT_SUCCEEDED: return 0x07E0; /* green  */
    case JR_DISPLAY_AGENT_FAILED:    return 0xF800; /* red    */
    case JR_DISPLAY_AGENT_VERIFYING: return 0x681F; /* cobalt */
    case JR_DISPLAY_AGENT_WORKING:
    default:                         return 0xA81F; /* violet */
    }
}

/* Procedural round-native furniture composited over the baked-EAF face.
 *
 * Today this is STATE-03 only — the thinking orbital spinner from
 * docs/prototype/jarvisnano-os.html — drawn while the applied face is THINKING.
 * It is the first element of the JarvisNano OS design to render on glass, and
 * the seam the rest of it lands through (choice arcs, battery rim, listen
 * countdown), because procedural drawing is the only option that fits: a baked
 * 4-mood x 4-state clip matrix needs ~12-15 MiB against a 6.875 MiB partition.
 *
 * Runs INSIDE the flush, on the gfx-owned DMA strip, before the shell and
 * surface overlays so those still win the z-order. hud_overlay_thinking() is
 * stateless and integer-only, so this costs no allocation and no float work.
 *
 * Full-width strips only: hud_render works on a fixed HUD_W-wide frame, while
 * this callback carries an (x2-x1) stride. The engine emits full-width 12-row
 * strips today; if that ever changes, skipping is the correct failure mode. */
/* World-state pushed in by the composition root. Packed into one word so the
 * flush path reads it without a lock: battery in bits 0-7, charging in bit 8,
 * privacy mute in bit 9, and the two tilt offsets pre-resolved to signed
 * pixels in bits 16-31. Doing the float trigonometry at push time keeps it out
 * of the render path. */
static volatile uint32_t s_hud_env_word =
    0x000000FFu;   /* battery unknown, level, listening */
static volatile uint32_t s_hud_enabled = 1u;

/* ---- STATE-05 choice arcs -------------------------------------------------
 * The flush path reads these; the composition root writes them. Guarded by a
 * generation counter rather than a mutex: the flush runs at 16 fps on the
 * render task and must never block, and a torn read is bounded to one frame of
 * a stale arc, which is invisible. Labels are borrowed (see the header). */
static hud_choice_t     s_choices[HUD_CHOICE_MAX];
static volatile int     s_choice_n;
static volatile int     s_choice_selected = -1;

/* Ask text furniture (STATE-05/06/07). Everything below is computed ONCE in
 * jr_display_present_choices and only READ by the flush path, under the same
 * publish discipline as s_choices: every field is written BEFORE the
 * s_choice_n release-store, so a present/flush race is bounded to one frame of
 * stale text — never half-updated text.
 *
 * The display OWNS every byte it renders: the question is wrapped into
 * s_choice_q_line and the labels are copied into s_choice_label_buf (and
 * s_choices[i].label points at those copies). The caller's ask snapshot is
 * only read during the present call itself — a re-ask that memsets and
 * rewrites that snapshot while the render task is still flushing strips of
 * the OLD presentation can therefore not blank or tear the labels on glass.
 * s_choice_label_buf[i][24] is only ever written as NUL, so the buffers are
 * NUL-terminated inside 25 bytes at EVERY interleaving of a racing rewrite. */
static char             s_choice_q_line[2][25];    /* hud_wrap2(question, 24) */
static int              s_choice_q_x[2];           /* per-line text origin    */
static int              s_choice_q_y[2];
static char             s_choice_label_buf[HUD_CHOICE_MAX][25];
static int              s_choice_label_x[HUD_CHOICE_MAX];
static int              s_choice_label_y[HUD_CHOICE_MAX];
static int              s_choice_label_len[HUD_CHOICE_MAX];

/* STATE-04 caption chip. SAME DISCIPLINE as the choice statics: SINGLE-WRITER
 * — only the app task calls jr_display_caption_set/clear — and everything the
 * flush reads (wrapped lines, origins) is derived at set time and written
 * BEFORE the s_caption_on release-store, so a set/flush race is bounded to
 * one frame of stale caption text. The input is COPIED (96-char cap), so the
 * caller's buffer only has to survive the call itself. */
static char             s_caption_text[97];
static char             s_caption_line[2][39];     /* wrapped at 19 for scale 2 */
static int              s_caption_x[2];
static int              s_caption_y[2];
static volatile int     s_caption_on;

/* TRANS-05 ripple slot: ONLY the app task writes (a new tap replaces the old
 * — single slot, no queue); the flush only reads and age-gates, so expiry
 * needs no write at all. x/y land before the start_ms release-store; a torn
 * read across a replace is one frame of a misplaced ring, accepted under the
 * module's one-frame-stale discipline. start_ms == 0 means never fired. */
static volatile int      s_ripple_x;
static volatile int      s_ripple_y;
static volatile uint32_t s_ripple_start_ms;
static volatile uint8_t  s_ripple_kind;   /* hud_ripple_kind_t */
/* Hold-to-commit ring: 0 = not in flight. Single writer (the app task), read
 * by the render task, same discipline as the ripple slot. */
static volatile uint8_t  s_commit_pct;

/* TRANS-01 wake bloom slot: SAME single-slot discipline as the ripple — only
 * the app task writes (a re-fire restarts the bloom), the flush age-gates, so
 * expiry needs no write. 0 means never fired. */
static volatile uint32_t s_bloom_start_ms;

/* UI-01 clock state: (on << 16) | (hh << 8) | mm in ONE word, published with
 * a single release-store, so the flush can never read a torn time (an on flag
 * from one set with minutes from another). SINGLE-WRITER: the app task calls
 * jr_display_clock_set at ~1 Hz; the flush only reads. 0 at boot = off. */
static volatile uint32_t s_clock_word;

/* Advance both fades ONE step. Called from panel_flush only at the y==0
 * strip — the same frame-start definition snapshot_record_flush relies on —
 * so every strip of a frame composites at the SAME level and a fade can
 * never band across strip boundaries. State lives beside the canvas section
 * above (its first consumer); this reads the intent words published there
 * and at s_clock_word. */
static void brightness_slew(uint32_t dt_ms);   /* defined with the pump below */
static void sp_fade_tick(uint32_t now_ms, uint32_t dt_ms, int cstep);

static void overlay_fade_tick(void)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t dt = now - s_fade_prev_ms;
    s_fade_prev_ms = now;
    if (dt > 100u) {
        dt = 100u;                   /* clamp across stalls, like hud_tick */
    }
    brightness_slew(dt);
    int step = (int)((dt * 256u) / JR_DISPLAY_FADE_MS);
    if (step < 1) {
        step = 1;
    }

    const uint32_t cw = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
    if ((cw & (1u << 16)) != 0u) {
        s_clock_shown_word = cw;
        s_clock_prog += step;
        if (s_clock_prog > 256) {
            s_clock_prog = 256;
        }
    } else {
        s_clock_prog -= step;
        if (s_clock_prog < 0) {
            s_clock_prog = 0;
        }
    }
    s_clock_ease = fade_smoothstep(s_clock_prog);

    if (jr_display_canvas_active()) {
        s_canvas_prog += step;
        if (s_canvas_prog > 256) {
            s_canvas_prog = 256;
        }
    } else {
        s_canvas_prog -= step;
        if (s_canvas_prog < 0) {
            s_canvas_prog = 0;
        }
    }
    s_canvas_ease = fade_smoothstep(s_canvas_prog);

    int cstep = (int)((dt * 256u) / JR_DISPLAY_CAPTION_FADE_MS);
    if (cstep < 1) {
        cstep = 1;
    }
    if (__atomic_load_n(&s_caption_on, __ATOMIC_ACQUIRE) != 0) {
        s_caption_prog += cstep;
        if (s_caption_prog > 256) {
            s_caption_prog = 256;
        }
    } else {
        s_caption_prog -= cstep;
        if (s_caption_prog < 0) {
            s_caption_prog = 0;
        }
    }
    s_caption_ease = fade_smoothstep(s_caption_prog);

    const int ask_n = __atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE);
    if (ask_n > 0) {
        s_ask_shown_n = ask_n;
        s_ask_prog += cstep;
        if (s_ask_prog > 256) {
            s_ask_prog = 256;
        }
    } else {
        s_ask_prog -= cstep;
        if (s_ask_prog < 0) {
            s_ask_prog = 0;
        }
    }
    s_ask_ease = fade_smoothstep(s_ask_prog);

    sp_fade_tick(now, dt, cstep);
}

void jr_display_present_choices(const char *question,
                                const char *const *labels, int n)
{
    if (labels == NULL || n <= 0) {
        jr_display_dismiss_choices();
        return;
    }
    if (n > HUD_CHOICE_MAX) {
        n = HUD_CHOICE_MAX;
    }
    hud_choice_t tmp[HUD_CHOICE_MAX];
    memset(tmp, 0, sizeof tmp);
    for (int i = 0; i < n; ++i) {
        tmp[i].label = labels[i];          /* borrowed; see header contract */
    }
    hud_choice_layout(n, tmp);             /* fills a0/a1, leaves label alone */

    /* Label BYTES are copied and OWNED here (24-char cap): the caller's ask
     * snapshot may be memset and rewritten by the voice task on a re-ask
     * while the render task is still flushing strips of the OLD presentation,
     * so nothing rendered may dereference caller storage. From here on the
     * hud_choice_t labels point at the copies — hud_overlay_choices' NULL
     * occupancy semantics are preserved (unused slots stay NULL).
     * Anchors go at r=205, just inside the arcs; the anchor clamp keeps every
     * box fully on-glass. */
    for (int i = 0; i < HUD_CHOICE_MAX; ++i) {
        s_choice_label_buf[i][0] = '\0';
        s_choice_label_len[i] = 0;
        s_choice_label_x[i] = 0;
        s_choice_label_y[i] = 0;
        if (i >= n || tmp[i].label == NULL) {
            tmp[i].label = NULL;
            continue;
        }
        const int len = (int)strnlen(tmp[i].label, 24U);
        memcpy(s_choice_label_buf[i], tmp[i].label, (size_t)len);
        s_choice_label_buf[i][len] = '\0';
        tmp[i].label = s_choice_label_buf[i];  /* render from the copy */
        s_choice_label_len[i] = len;
        hud_choice_label_anchor(&tmp[i], 205, 6 * len, 7,
                                &s_choice_label_x[i], &s_choice_label_y[i]);
    }
    memcpy(s_choices, tmp, sizeof tmp);

    /* Question furniture: wrapped ONCE into owned lines — the borrowed
     * pointer is not retained at all. Lines render at scale 2 (12 px/char,
     * 14 px tall): one line centres at y=184, two stack at 170/198.
     * (466 - 12*len) is even, so the centred origin is exact and the line
     * ends at HUD_W - x0. */
    s_choice_q_line[0][0] = '\0';
    s_choice_q_line[1][0] = '\0';
    if (question != NULL) {
        hud_wrap2(question, 24, s_choice_q_line[0], s_choice_q_line[1]);
    }
    const int q1 = (int)strnlen(s_choice_q_line[0], 24U);
    const int q2 = (int)strnlen(s_choice_q_line[1], 24U);
    s_choice_q_x[0] = (HUD_W - 12 * q1) / 2;
    s_choice_q_x[1] = (HUD_W - 12 * q2) / 2;
    s_choice_q_y[0] = (q2 > 0) ? 170 : 184;
    s_choice_q_y[1] = 198;

    s_choice_selected = -1;
    __atomic_store_n(&s_choice_n, n, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "choices: presenting %d arcs%s", n,
             question != NULL ? " + question" : "");
}

void jr_display_dismiss_choices(void)
{
    if (__atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE) == 0) {
        return;                            /* idempotent */
    }
    /* Only the count clears. Question, labels, and the selected index all
     * STAY: the render task keeps drawing them through the exit fade — the
     * tapped arc staying lit as the presentation sinks IS the confirmation
     * beat. The hit test gates on s_choice_n, so a fading ask can never
     * answer a tap; the next present() rewrites everything before its own
     * release-store. */
    __atomic_store_n(&s_choice_n, 0, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "choices: dismissed");
}

/* A PINNED CAPTION SURVIVES EVERY OTHER WRITER. The chip is documented
 * single-writer and is not: 56 call sites across the voice task and the HTTP
 * handlers. Most of the time the last writer winning is fine — captions are
 * ephemeral — but "UPDATING - DO NOT UNPLUG" is a safety instruction, and a
 * live transcript word or "LISTENING" from the voice task could replace it
 * mid-flash. While pinned, ordinary set/clear calls are ignored; only unpin
 * (or a new pin) changes the glass. */
static _Atomic bool s_caption_pinned;

void jr_display_caption_pin(const char *text)
{
    atomic_store(&s_caption_pinned, false);
    jr_display_caption_set(text);
    atomic_store(&s_caption_pinned, true);
}

void jr_display_caption_unpin(void)
{
    atomic_store(&s_caption_pinned, false);
}

bool jr_display_caption_pinned(void)
{
    return atomic_load(&s_caption_pinned);
}

void jr_display_caption_set(const char *text)
{
    if (atomic_load(&s_caption_pinned)) {
        return;
    }
    if (text == NULL || text[0] == '\0') {
        jr_display_caption_clear();
        return;
    }
    /* Copy, wrap, and place BEFORE the release-store — the flush path only
     * ever reads finished furniture. Scale 2 (12 px/char, 14 px tall), wrapped
     * at 19 characters: one line sits at y=394, two stack at 374/402. An
     * earlier comment here described scale 1 at y=403 / 396+408, which the
     * code has not done since the caption was doubled for legibility.
     * (466 - 6*len) is even, so the centred origin is exact and each line
     * ends at HUD_W - x0. */
    const size_t len = strnlen(text, 96U);
    memcpy(s_caption_text, text, len);
    s_caption_text[len] = '\0';
    hud_wrap2(s_caption_text, 19, s_caption_line[0], s_caption_line[1]);
    const int c1 = (int)strnlen(s_caption_line[0], 19U);
    const int c2 = (int)strnlen(s_caption_line[1], 19U);
    s_caption_x[0] = (HUD_W - 12 * c1) / 2;
    s_caption_x[1] = (HUD_W - 12 * c2) / 2;
    s_caption_y[0] = (c2 > 0) ? 374 : 394;
    s_caption_y[1] = 402;
    __atomic_store_n(&s_caption_on, 1, __ATOMIC_RELEASE);
}

void jr_display_caption_clear(void)
{
    if (atomic_load(&s_caption_pinned)) {
        return;
    }
    if (__atomic_load_n(&s_caption_on, __ATOMIC_ACQUIRE) == 0) {
        return;                            /* idempotent */
    }
    /* Only the flag clears. The wrapped lines STAY: the render task keeps
     * showing them through the exit fade — zeroing here made the text vanish
     * a frame before its band, which read as the chip's contents being
     * snatched. The next set() rewrites them before its own release-store. */
    __atomic_store_n(&s_caption_on, 0, __ATOMIC_RELEASE);
}

static void ripple_arm(int x, int y, hud_ripple_kind_t kind)
{
    /* x/y and kind first, then the timestamp release-store that arms the slot
     * — the renderer acquires on that timestamp, so everything it will read
     * must already be published. */
    s_ripple_x = x;
    s_ripple_y = y;
    s_ripple_kind = (uint8_t)kind;
    __atomic_store_n(&s_ripple_start_ms,
                     (uint32_t)(esp_timer_get_time() / 1000),
                     __ATOMIC_RELEASE);
}

void jr_display_ripple(int x, int y)
{
    ripple_arm(x, y, HUD_RIPPLE_ACCEPT);
}

void jr_display_ripple_reject(int x, int y)
{
    ripple_arm(x, y, HUD_RIPPLE_REJECT);
}

void jr_display_ripple_neutral(int x, int y)
{
    ripple_arm(x, y, HUD_RIPPLE_NEUTRAL);
}

void jr_display_commit_ring(uint8_t pct)
{
    __atomic_store_n(&s_commit_pct, pct > 100U ? 100U : pct, __ATOMIC_RELEASE);
}

void jr_display_bloom(void)
{
    __atomic_store_n(&s_bloom_start_ms,
                     (uint32_t)(esp_timer_get_time() / 1000),
                     __ATOMIC_RELEASE);
}

void jr_display_clock_set(bool on, int hh, int mm, int ss)
{
    if (hh < 0 || hh > 23) {
        hh = 0;
    }
    if (mm < 0 || mm > 59) {
        mm = 0;
    }
    if (ss < 0 || ss > 59) {
        ss = 0;
    }
    /* seconds ride bits 17..22, above the on flag at 16 — still one word,
     * still a single release-store, still untearable. */
    const uint32_t word = (on ? (1u << 16) : 0u)
                        | ((uint32_t)ss << 17)
                        | ((uint32_t)hh << 8) | (uint32_t)mm;
    __atomic_store_n(&s_clock_word, word, __ATOMIC_RELEASE);
}

/* Brightness target, published by any task; APPLIED only on the render task.
 *
 * The CO5300 brightness command and the frame flush are two transactions on
 * the SAME QSPI device. Issuing brightness from a caller's task (the voice
 * task ran the mood ramp) races the render task mid-flush and breaks the bus
 * acquire/release pairing — observed on hardware as
 *   "bus_lock: spi_bus_lock_acquire_end: Cannot release a lock that hasn't
 *    been acquired" -> assert failed: spi_device_release_bus
 * with the crash landing inside panel_co5300_draw_bitmap. So callers only
 * store a target here; brightness_pump() applies it from panel_flush, where
 * the render task owns the bus and no DMA is in flight.
 *
 * The pump applies the SLEWED value, not the raw target: overlay_fade_tick
 * walks s_brightness_shown toward the target at full-scale-per-600-ms, so a
 * mood pop (100 -> 8 in one publish) becomes a glide, while a caller-side
 * ramp slower than the slew passes through untouched — this is a rate
 * LIMITER, not a second animation. One panel write per frame at most while
 * slewing (distinct values), and the unchanged-value short-circuit still
 * kills idle-rate rewrites. */
#define JR_DISPLAY_BRIGHT_SLEW_MS 600U
static uint8_t s_brightness_want = 100;   /* written by any task (atomic) */
static uint8_t s_brightness_have = 100;   /* render task only — no atomics */
static int     s_brightness_shown = 100;  /* render task only: slewed value */

esp_err_t jr_display_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    __atomic_store_n(&s_brightness_want, percent, __ATOMIC_RELEASE);
    return ESP_OK;
}

/* Once per frame, from overlay_fade_tick: walk the shown value toward the
 * published target at the slew rate. Render task only. */
static void brightness_slew(uint32_t dt_ms)
{
    const int want = __atomic_load_n(&s_brightness_want, __ATOMIC_ACQUIRE);
    int step = (int)((dt_ms * 100u) / JR_DISPLAY_BRIGHT_SLEW_MS);
    if (step < 1) {
        step = 1;
    }
    if (want > s_brightness_shown) {
        s_brightness_shown += step;
        if (s_brightness_shown > want) {
            s_brightness_shown = want;
        }
    } else if (want < s_brightness_shown) {
        s_brightness_shown -= step;
        if (s_brightness_shown < want) {
            s_brightness_shown = want;
        }
    }
}

/* Called at the top of panel_flush, on the render task, between strips. */
static void brightness_pump(void)
{
    uint8_t want = (uint8_t)s_brightness_shown;   /* slewed, render task */
    if (want == s_brightness_have) {
        return;   /* also kills the 8.9 writes/sec of identical values */
    }
    if (jarvis_board_set_brightness(want) == ESP_OK) {
        s_brightness_have = want;
    }
}

/* ===== SPATIAL SHELL: state, content, easing =============================
 *
 * Four horizontal spaces and two overlays, held in ONE 32-bit word so the app
 * task can publish navigation with a single compare-exchange and the render
 * task can sample it with a single load. No lock, no queue, no allocation:
 *
 *   bits  1:0   current space          (jr_display_space_t)
 *   bits  3:2   space we came from     (only meaningful for one transition)
 *   bits  5:4   overlay                (jr_display_overlay_t)
 *   bit   6     travel direction       (1 = toward higher space index)
 *   bits 15:8   change serial          (wraps; render task edge-detects it)
 *
 * The serial is what makes the slide correct without a shared clock. The app
 * task never touches animation state; it bumps the serial, and the render
 * task notices the edge on its next frame and restarts the ease from zero.
 * Two swipes inside one frame therefore cost one slide from the first origin
 * to the last destination, which is what a fast flick should look like.
 *
 * Every string the shell can draw lives in a fixed-size static array with a
 * hard cap, written once by the app task and read by the render task. Text is
 * furniture, not truth: a torn read shows one frame of stale characters and
 * never a runaway length, because the caps bound the draw regardless.
 */

/* THREE bits per space, not two. The old layout gave the space field 0x3 —
 * four values — which was exactly enough while the ring held four screens and
 * silently truncated the moment it held more: with seven, screens 4/5/6 folded
 * onto 0/1/2, so sliding past MOTION appeared to wrap early and DESK, TOOLS
 * and SETTINGS were unreachable. Nothing warned; the field just dropped the
 * high bit.
 *
 * Widened to 3 bits (8 screens) and every downstream field moved with it. If
 * the ring ever needs a ninth screen this must widen again — the guard below
 * turns that into a build failure instead of another silent truncation. */
#define NAV_SPACE_MASK    0x7u
#define NAV_PREV_SHIFT    3
#define NAV_OVL_SHIFT     6
#define NAV_OVL_MASK      0xC0u
#define NAV_FORWARD_BIT   0x100u
#define NAV_SERIAL_SHIFT  9

_Static_assert((uint32_t)JR_DISPLAY_SPACE_COUNT <= NAV_SPACE_MASK + 1u,
               "JR_DISPLAY_SPACE_COUNT exceeds the nav word's space field — "
               "widen NAV_SPACE_MASK and shift NAV_PREV/OVL/FORWARD/SERIAL");

#define SP_LABEL_CAP      13   /* 12 glyphs at scale 2 = 144 px, fits r<=96 */
#define SP_COL_MAX        11   /* detail column: 10 glyphs at scale 2       */
#define SP_ROWS_MAX       9   /* STATUS: battery .. update, nine facts      */
#define SP_VAL_MAX       16   /* value column: the IP row is 15 glyphs      */

static volatile uint32_t s_nav_word;

static char s_space_head[JR_DISPLAY_SPACE_COUNT][SP_LABEL_CAP];
static char s_space_note[JR_DISPLAY_SPACE_COUNT][SP_LABEL_CAP];
static char s_desk_task[SP_LABEL_CAP];

static volatile uint32_t s_jarvis_word;      /* turns | linked<<16          */
static volatile uint32_t s_jarvis_secs;
static volatile uint32_t s_desk_word;        /* progress | state<<8         */
static volatile uint32_t s_status_word;      /* vol                         */

/* WEATHER is a whole struct, not a word, so it is double-buffered: the setter
 * fills the slot the render task is NOT reading and then publishes the slot
 * index with one release-store. A frame can read stale weather, never a
 * half-written one. Fetches arrive at most every ten minutes, so the two
 * slots never race in practice either. */
static jr_display_weather_t s_weather[2];
static volatile uint32_t    s_weather_slot;

/* ACTIVITY: the last three things Jarvis did, newest at index 0, plus WHEN
 * each happened (esp_timer ms at the push). The count is the gate: rows land
 * before it is released, so a racing frame shows the previous list at worst.
 * Single writer, the app task. */
#define ACT_MAX      3
#define ACT_KIND_CAP 9    /* 8 glyphs */
#define ACT_SUM_CAP  25   /* 24 glyphs */
static char     s_act_kind[ACT_MAX][ACT_KIND_CAP];
static char     s_act_sum[ACT_MAX][ACT_SUM_CAP];
static uint32_t s_act_ms[ACT_MAX];
static volatile uint32_t s_act_count;
static volatile uint32_t s_power_word = 0xFFu; /* pct | mv<<8 | usb/charge */
/* STATUS links: bit0 wifi, bit1 session open, bits3:2 tools (0 none,
 * 1 starting, 2 ready), bit4 desk live, bit5 radio saving, bits15:8 -dBm,
 * bits23:16 die temperature + 40 (0 = no reading), bits27:24 CPU MHz / 20. */
#define SP_CHIP_HOT_C  70
static volatile uint32_t s_links_word;
static char s_links_ip[16];
/* 0 running, 1 panel-off requested, 2 panel is off (render task only). */
static volatile uint32_t s_panel_off;
/* OTA status, packed like every other shell word so the updater can publish
 * from its own task with one release-store and the render task can sample it
 * with one load:
 *
 *   bits  3:0   jr_display_ota_state_t
 *   bits 15:8   percent 0..100
 *   bits 19:16  active slot   (0, 1, or 0xF = unknown)
 *   bits 23:20  target slot   (0, 1, or 0xF = none staged)
 *   bit  24     preflight verdict, 1 = ready
 *
 * No strings: ESP-IDF OTA slots are indices, so rendering them as digits
 * costs no storage and cannot truncate. */
#define OTA_SLOT_NONE 0xFu

static volatile uint32_t s_ota_word = ((uint32_t)OTA_SLOT_NONE << 16) |
                                      ((uint32_t)OTA_SLOT_NONE << 20);

/* Composed once per presented frame, then read by every strip in that frame.
 * Composing per strip would redo the same integer formatting eleven times. */
static char s_detail_head[SP_LABEL_CAP];
static char s_detail_label[SP_ROWS_MAX][SP_COL_MAX];
static char s_detail_value[SP_ROWS_MAX][SP_VAL_MAX];
static int  s_detail_rows;
static char s_shade_vol[SP_COL_MAX];
static char s_shade_light[SP_COL_MAX];

/* Angles are 1/256 turn, matching the LUT convention hud_render uses for the
 * glass: 0 is 3 o'clock, 64 is 6 o'clock, 192 is 12 o'clock, increasing
 * clockwise. */
#define SP_A_TOP       192

/* WEATHER, composed once per frame from the live slot: every string the
 * focal object prints and every angle its arcs sweep. The renderer reads
 * these, never the struct, so eleven strips agree on one age and one mark. */
#define SP_WX_A0     96    /* the gauge opens at 7:30 ...                     */
#define SP_WX_SWEEP  192   /* ... and closes 270 degrees later at 4:30, so the
                            * bottom stays empty above the headline           */
#define SP_WX_LO_F   40    /* the fixed range the arc IS: a Fort Lauderdale   */
#define SP_WX_HI_F   100   /* year lives inside 40..100 F                     */
#define SP_WX_STALE_MIN 30 /* beyond this the accent loses its colour         */
#define SP_WX_AGE_MIN   2  /* below this the age is not worth a line          */
static char s_wx_head[SP_LABEL_CAP];  /* "OVERCAST 83" / "NO WEATHER"         */
static char s_wx_age[10];             /* "12M AGO" / "STALE 45M" / ""         */
static char s_wx_temp[6];             /* "83" / "-12" / ""                    */
static char s_wx_hilo[10];            /* "H86 L76" / ""                       */
static bool s_wx_valid;
static bool s_wx_stale;
static jr_display_sky_t s_wx_sky;
static int  s_wx_mark_a;              /* 1/256 turn, the current temperature  */
static int  s_wx_band_a0;             /* lo..hi as an arc on the same scale   */
static int  s_wx_band_sweep;
static int  s_wx_rain_sweep;          /* 0..256, from 12 o'clock               */

/* ACTIVITY, composed once per frame: one row per entry, "KIND SUMMARY", cut
 * to the row's width with the tree's own shortening mark. */
#define SP_ACT_ROW_GLYPHS 24
#define SP_ACT_ROW_CAP    (SP_ACT_ROW_GLYPHS + 1)
static char    s_act_row[ACT_MAX][SP_ACT_ROW_CAP];
static uint8_t s_act_row_klen[ACT_MAX];   /* where the kind ends, for colour */
static int     s_act_rows;

static uint8_t  s_space_serial_seen;
static uint8_t  s_space_from;
static uint8_t  s_space_to;
static int8_t   s_space_dir = 1;
static int      s_space_prog = 256;
static int      s_space_ease = 256;
static int      s_detail_prog;
static int      s_detail_ease;
static int      s_space_veil;
static bool     s_space_on;
static uint8_t  s_detail_space;
static uint32_t s_space_hold_ms;

/* The orbit mark's angle, in 1/4096 turn (Q4 on the 256-unit circle), plus
 * the residue of the last re-spacing. When DESK appears or disappears every
 * slot moves, and the mark for the screen you are standing on would jump with
 * them; instead the difference is banked as an offset that decays over one
 * slide, so the mark eases to its new slot the way it eases between screens. */
static int      s_orbit_a16;
static int      s_orbit_off16;
static int      s_orbit_n_seen;

/* Bounded copy with a guaranteed terminator. The public setters are the only
 * writers, so an over-long caller string is truncated here once instead of
 * being trusted anywhere downstream. */
static void sp_copy(char *dst, const char *src)
{
    size_t i = 0;
    if (src != NULL) {
        for (; i + 1U < (size_t)SP_LABEL_CAP && src[i] != '\0'; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

/* Integer-only formatting into a caller buffer with an explicit cap. The
 * presenter has no room for snprintf here: it drags in float formatting and
 * several hundred bytes of stack, and this task shares a 6 KiB stack with the
 * strip machinery. Each helper returns the new length so calls chain. */
static int sp_str(char *dst, int len, int cap, const char *src)
{
    while (src != NULL && *src != '\0' && len + 1 < cap) {
        dst[len++] = *src++;
    }
    dst[len] = '\0';
    return len;
}

static int sp_num(char *dst, int len, int cap, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < (int)sizeof tmp);
    while (n > 0 && len + 1 < cap) {
        dst[len++] = tmp[--n];
    }
    dst[len] = '\0';
    return len;
}

static int sp_pct(char *dst, int len, int cap, uint32_t v)
{
    return sp_str(dst, sp_num(dst, len, cap, v), cap, "%");
}

/* Signed, for temperatures: the font has a dash, and a winter morning has a
 * minus sign. */
static int sp_int(char *dst, int len, int cap, int v)
{
    if (v < 0) {
        len = sp_str(dst, len, cap, "-");
        v = -v;
    }
    return sp_num(dst, len, cap, (uint32_t)v);
}

/* Battery volts as the gauge reports them: "4.02V". Two decimals because the
 * AXP2101 resolves millivolts and a tenth hides a whole hour of discharge. */
static int sp_volts(char *dst, int len, int cap, uint32_t mv)
{
    len = sp_num(dst, len, cap, mv / 1000u);
    len = sp_str(dst, len, cap, ".");
    const uint32_t cs = (mv % 1000u) / 10u;
    if (cs < 10u) {
        len = sp_str(dst, len, cap, "0");
    }
    len = sp_num(dst, len, cap, cs);
    return sp_str(dst, len, cap, "V");
}

/* How long the device has been up, in the two largest units that matter:
 * "12M", "6H 10M", "2D 14H". Days are what an owner reads as "stable". */
static int sp_uptime(char *dst, int len, int cap, uint32_t secs)
{
    const uint32_t mins = secs / 60u;
    if (mins < 60u) {
        len = sp_num(dst, len, cap, mins);
        return sp_str(dst, len, cap, "M");
    }
    const uint32_t hours = mins / 60u;
    if (hours < 24u) {
        len = sp_num(dst, len, cap, hours);
        len = sp_str(dst, len, cap, "H ");
        len = sp_num(dst, len, cap, mins % 60u);
        return sp_str(dst, len, cap, "M");
    }
    len = sp_num(dst, len, cap, hours / 24u);
    len = sp_str(dst, len, cap, "D ");
    len = sp_num(dst, len, cap, hours % 24u);
    return sp_str(dst, len, cap, "H");
}

/* The one word for where the power comes from. Charging outranks the cable,
 * a full cell on the cable is FULL, and a device with no gauge on USB is
 * still honestly ON USB. */
static const char *sp_power_word(uint32_t w)
{
    const uint32_t pct = w & 0xFFu;
    if ((w & (1u << 25)) != 0u) {
        return "CHARGING";
    }
    if ((w & (1u << 24)) != 0u) {
        return pct == 100u ? "FULL" : "ON USB";
    }
    return "ON CELL";
}

/* Wi-Fi bars from RSSI, the thresholds every phone uses: 0 means no link. */
static int sp_wifi_bars(uint32_t lk)
{
    if ((lk & 1u) == 0u) {
        return 0;
    }
    const int dbm = -(int)((lk >> 8) & 0xFFu);
    return dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 : 1;
}

/* An age in minutes as the shortest honest unit: "12M", "3H", "2D". Shared by
 * WEATHER (how old the fetch is) and ACTIVITY (how long ago it happened), so
 * the two screens cannot disagree about what a minute is. */
static int sp_age(char *dst, int len, int cap, uint32_t age_min)
{
    if (age_min < 60u) {
        return sp_str(dst, sp_num(dst, len, cap, age_min), cap, "M");
    }
    if (age_min < 48u * 60u) {
        return sp_str(dst, sp_num(dst, len, cap, age_min / 60u), cap, "H");
    }
    return sp_str(dst, sp_num(dst, len, cap, age_min / (24u * 60u)), cap, "D");
}

static inline int sp_len(const char *s, int cap);

/* Milliseconds since an esp_timer stamp, in minutes. Unsigned subtraction so
 * a stamp taken before the 49-day wrap still reads correctly after it. */
static uint32_t sp_age_min(uint32_t stamp_ms)
{
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return (now_ms - stamp_ms) / 60000u;
}

/* Elapsed session time as M:SS up to an hour, then H:MM. */
static int sp_clock_str(char *dst, int cap, uint32_t secs)
{
    const uint32_t big = secs >= 3600u ? secs / 3600u : secs / 60u;
    const uint32_t small = secs >= 3600u ? (secs / 60u) % 60u : secs % 60u;
    int len = sp_num(dst, 0, cap, big);
    len = sp_str(dst, len, cap, ":");
    if (small < 10u) {
        len = sp_str(dst, len, cap, "0");
    }
    return sp_num(dst, len, cap, small);
}

/* Privacy comes from the one place that already owns it: the HUD env word
 * jr_display_set_hud_env publishes, bit 9. The shell never keeps a second
 * copy, so the ring and the shell can never disagree about the mic. */
static bool sp_privacy_muted(void)
{
    return (diag_load(&s_hud_env_word) & (1u << 9)) != 0U;
}

/* DESK IS ON THE RING ONLY WHILE SOMETHING LIVES THERE. The gate is the one
 * bit main.c already publishes for the agent rim — an agent link, a pairing
 * claim or an operator lease all set it — so the screen and the rim segments
 * cannot disagree about whether there is a job. With the bit clear the ring
 * steps over DESK in both directions, the orbit draws one mark fewer, and a
 * DESK that goes dark under the owner's feet moves them on rather than
 * leaving them on a screen the ring no longer admits. A screen that says
 * "STANDBY" in a progress ring is the useless screen the owner named. */
static bool sp_desk_live(void)
{
    return (diag_load(&s_display.shell_word) & JR_DISPLAY_SHELL_AGENT) != 0U;
}

/* How many screens the ring shows right now, and which slot a space holds on
 * the orbit. A hidden DESK that is nonetheless current (an explicit nav_set)
 * keeps its full-ring slot, so the mark has somewhere honest to sit. */
static int sp_ring_n(bool desk_live)
{
    return desk_live ? (int)JR_DISPLAY_SPACE_COUNT
                     : (int)JR_DISPLAY_SPACE_COUNT - 1;
}

static int sp_orbit_angle(int space, bool desk_live)
{
    int n = sp_ring_n(desk_live);
    int idx = space;
    if (!desk_live) {
        if (space == (int)JR_DISPLAY_SPACE_DESK) {
            n = (int)JR_DISPLAY_SPACE_COUNT;
        } else if (space > (int)JR_DISPLAY_SPACE_DESK) {
            idx = space - 1;
        }
    }
    return SP_A_TOP + (idx * 256) / n;
}

/* The shade opens from either door: the nav overlay (swipe down through the
 * new API) or the legacy JR_DISPLAY_SHELL_SHADE bit that existing callers of
 * jr_display_set_shell_state already drive. One visual, two sources. */
static bool sp_shade_open(void)
{
    if ((diag_load(&s_display.shell_word) & JR_DISPLAY_SHELL_SHADE) != 0U) {
        return true;
    }
    const uint32_t nav = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    return ((nav & NAV_OVL_MASK) >> NAV_OVL_SHIFT) ==
           (uint32_t)JR_DISPLAY_OVERLAY_SHADE;
}

static const char *sp_agent_name(jr_display_agent_state_t st)
{
    switch (st) {
    case JR_DISPLAY_AGENT_WORKING:   return "WORKING";
    case JR_DISPLAY_AGENT_VERIFYING: return "VERIFY";
    case JR_DISPLAY_AGENT_WAITING:   return "WAITING";
    case JR_DISPLAY_AGENT_SUCCEEDED: return "DONE";
    case JR_DISPLAY_AGENT_FAILED:    return "FAILED";
    default:                         return "IDLE";
    }
}
/* Names stay inside SP_COL_MAX-1 (10 glyphs), so a state change can never
 * truncate the detail sheet's value column. */
static const char *sp_ota_name(jr_display_ota_state_t state)
{
    switch (state) {
    case JR_DISPLAY_OTA_PREFLIGHT:  return "CHECKING";
    case JR_DISPLAY_OTA_BLOCKED:    return "BLOCKED";
    case JR_DISPLAY_OTA_RECEIVING:  return "WRITING";
    case JR_DISPLAY_OTA_PROBATION:  return "PROBATION";
    case JR_DISPLAY_OTA_VALID:      return "VALID";
    case JR_DISPLAY_OTA_FAILED:     return "FAILED";
    case JR_DISPLAY_OTA_ROLLED_BACK:return "ROLLBACK";
    default:                        return "IDLE";
    }
}

/* IDLE and VALID are the healthy resting states: no ring, so a device with
 * nothing to say keeps a quiet dial. Everything else rings. */
static bool sp_ota_ringing(jr_display_ota_state_t state)
{
    return state != JR_DISPLAY_OTA_IDLE && state != JR_DISPLAY_OTA_VALID;
}

/* Arc fill 0..100. Only RECEIVING is a real measurement; PREFLIGHT is armed
 * but has written nothing, and the rest are conditions rather than progress,
 * so they fill the ring completely instead of implying movement. */
static int sp_ota_fill(jr_display_ota_state_t state, int percent)
{
    switch (state) {
    case JR_DISPLAY_OTA_RECEIVING:
        return percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    case JR_DISPLAY_OTA_PREFLIGHT:
        return 0;
    default:
        return 100;
    }
}

/* Per-space detail: the answer to "what is actually going on in here", not a
 * repeat of the ambient telemetry the face already shows. Every row is a
 * short left label and a right-aligned value, so the sheet scans as a column
 * of facts rather than a paragraph. */
static void sp_compose_detail(int space)
{
    s_detail_rows = 0;
    switch (space) {
    case JR_DISPLAY_SPACE_JARVIS: {
        const uint32_t w = __atomic_load_n(&s_jarvis_word, __ATOMIC_ACQUIRE);
        sp_copy(s_detail_head, "SESSION");
        sp_str(s_detail_label[0], 0, SP_COL_MAX, "LINK");
        sp_str(s_detail_value[0], 0, SP_COL_MAX,
               (w & (1u << 16)) != 0u ? "UP" : "DOWN");
        sp_str(s_detail_label[1], 0, SP_COL_MAX, "TURNS");
        sp_num(s_detail_value[1], 0, SP_COL_MAX, w & 0xFFFFu);
        sp_str(s_detail_label[2], 0, SP_COL_MAX, "ELAPSED");
        sp_clock_str(s_detail_value[2], SP_COL_MAX, diag_load(&s_jarvis_secs));
        sp_str(s_detail_label[3], 0, SP_COL_MAX, "MIC");
        sp_str(s_detail_value[3], 0, SP_COL_MAX,
               sp_privacy_muted() ? "MUTED" : "LIVE");
        s_detail_rows = 4;
        break;
    }
    case JR_DISPLAY_SPACE_DESK: {
        const uint32_t w = __atomic_load_n(&s_desk_word, __ATOMIC_ACQUIRE);
        /* THE TASK IS THE HEAD. It used to be a JOB row in the 10-glyph
         * value column, which re-cut the 12-glyph title main.c had already
         * shortened — and cut off the "." mark that says it was shortened.
         * A truncation of a truncation, with the evidence removed. The head
         * holds 12 glyphs, exactly what title_shorten() produces, so the
         * whole marked title lands intact and the sheet is about the job. */
        sp_copy(s_detail_head, s_desk_task[0] != '\0' ? s_desk_task : "TASK");
        sp_str(s_detail_label[0], 0, SP_COL_MAX, "STATE");
        sp_str(s_detail_value[0], 0, SP_COL_MAX,
               sp_agent_name((jr_display_agent_state_t)((w >> 8) & 0xFu)));
        sp_str(s_detail_label[1], 0, SP_COL_MAX, "DONE");
        sp_pct(s_detail_value[1], 0, SP_COL_MAX, w & 0xFFu);
        s_detail_rows = 2;
        break;
    }
    case JR_DISPLAY_SPACE_WEATHER: {
        const jr_display_weather_t *w =
            &s_weather[__atomic_load_n(&s_weather_slot, __ATOMIC_ACQUIRE) & 1u];
        sp_copy(s_detail_head, "WEATHER");
        if (!w->valid) {
            /* One honest row. Five rows of "--" would be five placeholders
             * shaped like readings. */
            sp_str(s_detail_label[0], 0, SP_COL_MAX, "DATA");
            sp_str(s_detail_value[0], 0, SP_COL_MAX, "NONE");
            s_detail_rows = 1;
            break;
        }
        const uint32_t age_min = sp_age_min(w->fetched_ms);
        sp_str(s_detail_label[0], 0, SP_COL_MAX, "FEELS");
        sp_int(s_detail_value[0], 0, SP_COL_MAX, w->feels_f);
        sp_str(s_detail_label[1], 0, SP_COL_MAX, "RAIN");
        sp_pct(s_detail_value[1], 0, SP_COL_MAX, w->rain_pct);
        sp_str(s_detail_label[2], 0, SP_COL_MAX, "HUMIDITY");
        sp_pct(s_detail_value[2], 0, SP_COL_MAX, w->humidity_pct);
        sp_str(s_detail_label[3], 0, SP_COL_MAX, "WIND");
        sp_str(s_detail_value[3], sp_num(s_detail_value[3], 0, SP_COL_MAX,
                                         w->wind_mph),
               SP_COL_MAX, " MPH");
        sp_str(s_detail_label[4], 0, SP_COL_MAX, "AGE");
        if (age_min == 0u) {
            sp_str(s_detail_value[4], 0, SP_COL_MAX, "NOW");
        } else {
            int len = sp_age(s_detail_value[4], 0, SP_COL_MAX, age_min);
            if (age_min > SP_WX_STALE_MIN) {
                (void)sp_str(s_detail_value[4], len, SP_COL_MAX, " STALE");
            }
        }
        s_detail_rows = 5;
        break;
    }
    case JR_DISPLAY_SPACE_ACTIVITY: {
        /* THE FOCAL SAYS WHAT, THE SHEET SAYS WHEN. Repeating the three
         * summaries here would re-cut 24-glyph text to a 10-glyph column —
         * the truncation-of-a-truncation the DESK sheet was cured of — and
         * a sheet that duplicates its own screen is the clutter this tree
         * keeps deleting. The push time is measured, not invented. */
        uint32_t n = __atomic_load_n(&s_act_count, __ATOMIC_ACQUIRE);
        if (n > (uint32_t)ACT_MAX) {
            n = (uint32_t)ACT_MAX;
        }
        sp_copy(s_detail_head, "ACTIVITY");
        if (n == 0u) {
            sp_str(s_detail_label[0], 0, SP_COL_MAX, "LOG");
            sp_str(s_detail_value[0], 0, SP_COL_MAX, "EMPTY");
            s_detail_rows = 1;
            break;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t age_min = sp_age_min(s_act_ms[i]);
            sp_str(s_detail_label[i], 0, SP_COL_MAX, s_act_kind[i]);
            if (age_min == 0u) {
                sp_str(s_detail_value[i], 0, SP_COL_MAX, "JUST NOW");
            } else {
                (void)sp_str(s_detail_value[i],
                             sp_age(s_detail_value[i], 0, SP_COL_MAX, age_min),
                             SP_COL_MAX, " AGO");
            }
        }
        s_detail_rows = (int)n;
        break;
    }
    /* WATCH AND POWER OWN THEIR OWN SHEETS.
     *
     * Both used to fall through to the default below, so tapping the clock or
     * the battery opened a sheet headed SETTINGS listing privacy, link, update
     * and slot rows. The screen you touched was not the screen you got, and
     * the heading said so in the wrong direction — it named SETTINGS while you
     * were standing on WATCH. A sheet is the context of ONE space; borrowing
     * another space's context is worse than showing nothing. */
    case JR_DISPLAY_SPACE_WATCH: {
        const uint32_t w = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
        const bool synced = (w & (1u << 16)) != 0u;
        const uint32_t hh = (w >> 8) & 0xFFu;
        const uint32_t mm = w & 0xFFu;
        sp_copy(s_detail_head, "TIME");
        sp_str(s_detail_label[0], 0, SP_COL_MAX, "CLOCK");
        sp_str(s_detail_value[0], 0, SP_COL_MAX, synced ? "SYNCED" : "NO SYNC");
        sp_str(s_detail_label[1], 0, SP_COL_MAX, "NOW");
        if (synced) {
            /* Built digit by digit rather than through sp_clock_str, which
             * switches to minutes:seconds below 3600 s and would render the
             * midnight hour as though it were a duration. */
            int len = 0;
            if (hh < 10u) {
                len = sp_str(s_detail_value[1], len, SP_COL_MAX, "0");
            }
            len = sp_num(s_detail_value[1], len, SP_COL_MAX, hh);
            len = sp_str(s_detail_value[1], len, SP_COL_MAX, ":");
            if (mm < 10u) {
                len = sp_str(s_detail_value[1], len, SP_COL_MAX, "0");
            }
            (void)sp_num(s_detail_value[1], len, SP_COL_MAX, mm);
        } else {
            sp_str(s_detail_value[1], 0, SP_COL_MAX, "--:--");
        }
        sp_str(s_detail_label[2], 0, SP_COL_MAX, "MIC");
        sp_str(s_detail_value[2], 0, SP_COL_MAX,
               sp_privacy_muted() ? "MUTED" : "LIVE");
        s_detail_rows = 3;
        break;
    }
    case JR_DISPLAY_SPACE_STATUS: {
        /* THE DEVICE IN NINE FACTS. The owner's brief, verbatim in spirit:
         * "it should show data connections, battery, etc", with zero junk.
         * So every row is a live reading of something outside the glass —
         * the cell, the charger, the Wi-Fi, this device's address, the
         * session socket, the tools bridge, the companion, the radio's
         * power mode, and the firmware update. Nothing here is a mood, and
         * nothing here is printed elsewhere on the ring: the clock and the
         * microphone live on WATCH and on the gold rim. Two former rows
         * (VOLTS, SLOT) folded into their neighbours; UPTIME moved to the
         * closed face's headline, where it is the quiet a healthy device
         * says. */
        const uint32_t w = __atomic_load_n(&s_power_word, __ATOMIC_ACQUIRE);
        const uint32_t lk = __atomic_load_n(&s_links_word, __ATOMIC_ACQUIRE);
        const uint32_t percent = w & 0xFFu;
        const uint32_t millivolts = (w >> 8) & 0xFFFFu;
        sp_copy(s_detail_head, "STATUS");
        sp_str(s_detail_label[0], 0, SP_COL_MAX, "BATTERY");
        if (percent <= 100U) {
            int len = sp_pct(s_detail_value[0], 0, SP_COL_MAX, percent);
            if (millivolts >= 1000U) {
                len = sp_str(s_detail_value[0], len, SP_COL_MAX, " ");
                (void)sp_volts(s_detail_value[0], len, SP_COL_MAX, millivolts);
            }
        } else {
            sp_str(s_detail_value[0], 0, SP_COL_MAX, "NONE");
        }
        sp_str(s_detail_label[1], 0, SP_COL_MAX, "POWER");
        sp_str(s_detail_value[1], 0, SP_COL_MAX, sp_power_word(w));
        /* WIFI is a word and the number: the word is what the owner reads,
         * the dBm is what they quote when moving the device to a better
         * spot. The tiers are the ones the closed face's bars use. */
        sp_str(s_detail_label[2], 0, SP_COL_MAX, "WIFI");
        {
            const int bars = sp_wifi_bars(lk);
            if (bars == 0) {
                sp_str(s_detail_value[2], 0, SP_COL_MAX, "DOWN");
            } else {
                int len = sp_str(s_detail_value[2], 0, SP_COL_MAX,
                                 bars >= 3 ? "GOOD " : bars == 2 ? "FAIR "
                                                                 : "WEAK ");
                (void)sp_int(s_detail_value[2], len, SP_COL_MAX,
                             -(int)((lk >> 8) & 0xFFu));
            }
        }
        /* The address is the one row wider than the column: 15 glyphs,
         * right-aligned, which still clears a two-glyph label by 6 px. It is
         * what every operator tool on the LAN needs first. */
        sp_str(s_detail_label[3], 0, SP_COL_MAX, "IP");
        sp_str(s_detail_value[3], 0, SP_VAL_MAX,
               s_links_ip[0] != '\0' ? s_links_ip : "NONE");
        /* The session socket opens on demand and closes at rest, so a
         * closed socket is STANDBY, not an alarm. */
        sp_str(s_detail_label[4], 0, SP_COL_MAX, "LINK");
        sp_str(s_detail_value[4], 0, SP_COL_MAX,
               (lk & (1u << 1)) != 0u ? "OPEN" : "STANDBY");
        sp_str(s_detail_label[5], 0, SP_COL_MAX, "TOOLS");
        {
            const uint32_t tools = (lk >> 2) & 3u;
            sp_str(s_detail_value[5], 0, SP_COL_MAX,
                   tools == 2u ? "READY" : tools == 1u ? "STARTING" : "NO KEY");
        }
        /* The die thermometer. DESK left this sheet: the ring itself admits
         * DESK only while a companion is live, so the row said nothing the
         * ring did not. "Is it hot" deserves a number. */
        sp_str(s_detail_label[6], 0, SP_COL_MAX, "CHIP");
        {
            const uint32_t t = (lk >> 16) & 0xFFu;
            if (t == 0u) {
                sp_str(s_detail_value[6], 0, SP_COL_MAX, "NONE");
            } else {
                int len = sp_int(s_detail_value[6], 0, SP_COL_MAX, (int)t - 40);
                (void)sp_str(s_detail_value[6], len, SP_COL_MAX, "C");
            }
        }
        /* The chip's power mode, as far as this firmware drives one: the
         * radio sleeps between beacons while the device rests and runs
         * realtime for voice, updates and a companion. The CPU does not
         * scale yet — see PLAN.md — so this row is the whole truth. */
        /* The gear and the radio in one row: "240 LIVE" while anything is
         * happening, "160 SAVE" at rest on the cell. */
        sp_str(s_detail_label[7], 0, SP_COL_MAX, "CPU");
        {
            const uint32_t mhz = ((lk >> 24) & 0xFu) * 20u;
            int len = mhz != 0u ? sp_num(s_detail_value[7], 0, SP_COL_MAX, mhz)
                                : sp_str(s_detail_value[7], 0, SP_COL_MAX, "--");
            len = sp_str(s_detail_value[7], len, SP_COL_MAX, " ");
            (void)sp_str(s_detail_value[7], len, SP_COL_MAX,
                         (lk & (1u << 5)) != 0u ? "SAVE" : "LIVE");
        }
        /* At rest the UPDATE row reports READINESS rather than a state noun,
         * so STATUS answers "could this take an update right now" without an
         * update having to be in flight. */
        const uint32_t ota = __atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE);
        const jr_display_ota_state_t ota_state =
            (jr_display_ota_state_t)(ota & 0xFu);
        sp_str(s_detail_label[8], 0, SP_COL_MAX, "UPDATE");
        if (ota_state == JR_DISPLAY_OTA_RECEIVING) {
            sp_pct(s_detail_value[8], 0, SP_COL_MAX, (ota >> 8) & 0xFFu);
        } else if (ota_state == JR_DISPLAY_OTA_IDLE) {
            sp_str(s_detail_value[8], 0, SP_COL_MAX,
                   (ota & (1u << 24)) != 0u ? "READY" : "HOLD");
        } else {
            sp_str(s_detail_value[8], 0, SP_COL_MAX, sp_ota_name(ota_state));
        }
        s_detail_rows = 9;
        break;
    }
    default:
        /* Unreachable: every space in jr_display_space_t is named above. This
         * is deliberately EMPTY rather than a copy of some other screen's
         * sheet — when a new space is added, showing nothing is an obvious
         * gap, whereas silently inheriting another screen's sheet is a lie
         * that looks like a feature. That is exactly how WATCH and POWER went
         * wrong, back when a SETTINGS sheet sat here. */
        s_detail_head[0] = '\0';
        s_detail_rows = 0;
        break;
    }
}

/* The temperature's place on the gauge. The arc IS the day: a fixed 40..100 F
 * range opening at 7:30 and closing at 4:30, so lo, hi and now read as
 * positions before anyone reads a numeral. Out-of-range weather pins to the
 * end it exceeds rather than leaving the dial. */
static int sp_wx_angle(int f)
{
    if (f < SP_WX_LO_F) {
        f = SP_WX_LO_F;
    }
    if (f > SP_WX_HI_F) {
        f = SP_WX_HI_F;
    }
    return SP_WX_A0 + ((f - SP_WX_LO_F) * SP_WX_SWEEP) / (SP_WX_HI_F - SP_WX_LO_F);
}

/* WEATHER for this frame. Everything numeric is gated on valid: an unfetched
 * or failed weather prints NO number anywhere — not a zero, not a dash pair
 * shaped like one — and the headline says why. */
static void sp_compose_weather(void)
{
    const jr_display_weather_t *w =
        &s_weather[__atomic_load_n(&s_weather_slot, __ATOMIC_ACQUIRE) & 1u];
    s_wx_valid = w->valid;
    s_wx_sky = w->sky;
    s_wx_age[0] = '\0';
    s_wx_temp[0] = '\0';
    s_wx_hilo[0] = '\0';
    s_wx_stale = false;
    s_wx_mark_a = -1;
    s_wx_band_a0 = 0;
    s_wx_band_sweep = 0;
    s_wx_rain_sweep = 0;
    if (!w->valid) {
        sp_str(s_wx_head, 0, SP_LABEL_CAP, "NO WEATHER");
        return;
    }

    /* Headline: the word, then the number if both fit twelve glyphs. When a
     * long condition leaves no room the number is dropped whole rather than
     * cut mid-digit — it is still the largest thing on the screen. */
    int len = sp_str(s_wx_head, 0, SP_LABEL_CAP, w->condition);
    char tail[6];
    int tlen = 0;
    if (len > 0) {
        tlen = sp_str(tail, 0, (int)sizeof tail, " ");
    }
    tlen = sp_int(tail, tlen, (int)sizeof tail, w->temp_f);
    if (len + tlen <= SP_LABEL_CAP - 1) {
        (void)sp_str(s_wx_head, len, SP_LABEL_CAP, tail);
    }

    (void)sp_int(s_wx_temp, 0, (int)sizeof s_wx_temp, w->temp_f);
    len = sp_str(s_wx_hilo, 0, (int)sizeof s_wx_hilo, "H");
    len = sp_int(s_wx_hilo, len, (int)sizeof s_wx_hilo, w->hi_f);
    len = sp_str(s_wx_hilo, len, (int)sizeof s_wx_hilo, " L");
    (void)sp_int(s_wx_hilo, len, (int)sizeof s_wx_hilo, w->lo_f);

    const uint32_t age_min = sp_age_min(w->fetched_ms);
    s_wx_stale = age_min > SP_WX_STALE_MIN;
    if (s_wx_stale) {
        /* "STALE 49D" is the worst case: fetched_ms is a uint32 of
         * milliseconds, so no age past 49 days can be expressed, and nine
         * glyphs is exactly what fits inside the rain ring. */
        len = sp_str(s_wx_age, 0, (int)sizeof s_wx_age, "STALE ");
        (void)sp_age(s_wx_age, len, (int)sizeof s_wx_age, age_min);
    } else if (age_min >= SP_WX_AGE_MIN) {
        len = sp_age(s_wx_age, 0, (int)sizeof s_wx_age, age_min);
        (void)sp_str(s_wx_age, len, (int)sizeof s_wx_age, " AGO");
    }

    int lo = w->lo_f, hi = w->hi_f;
    if (lo > hi) {
        const int t = lo;
        lo = hi;
        hi = t;
    }
    s_wx_band_a0 = sp_wx_angle(lo);
    s_wx_band_sweep = sp_wx_angle(hi) - s_wx_band_a0;
    s_wx_mark_a = sp_wx_angle(w->temp_f);
    s_wx_rain_sweep = ((int)(w->rain_pct > 100u ? 100u : w->rain_pct) * 256) / 100;
}

/* ACTIVITY rows for this frame: "KIND SUMMARY" in the row's 24 glyphs. A
 * summary that does not fit is cut and its last glyph spent on a "." — the
 * same mark title_shorten() leaves on a DESK title, so a reader knows the
 * sentence went on. */
static void sp_compose_activity(void)
{
    uint32_t n = __atomic_load_n(&s_act_count, __ATOMIC_ACQUIRE);
    if (n > (uint32_t)ACT_MAX) {
        n = (uint32_t)ACT_MAX;
    }
    s_act_rows = (int)n;
    for (uint32_t i = 0; i < n; ++i) {
        char *row = s_act_row[i];
        int len = sp_str(row, 0, SP_ACT_ROW_CAP, s_act_kind[i]);
        s_act_row_klen[i] = (uint8_t)len;
        const int slen = sp_len(s_act_sum[i], ACT_SUM_CAP - 1);
        if (slen == 0) {
            continue;
        }
        if (len > 0) {
            len = sp_str(row, len, SP_ACT_ROW_CAP, " ");
        }
        const int room = SP_ACT_ROW_GLYPHS - len;
        if (slen <= room) {
            (void)sp_str(row, len, SP_ACT_ROW_CAP, s_act_sum[i]);
        } else {
            for (int k = 0; k + 1 < room; ++k) {
                row[len++] = s_act_sum[i][k];
            }
            row[len++] = '.';
            row[len] = '\0';
        }
    }
}

/* One composition pass per frame: the shade readouts, the two live-data
 * screens, and the detail sheet if it is showing. */
static void sp_compose(void)
{
    sp_compose_weather();
    sp_compose_activity();
    const uint32_t w = __atomic_load_n(&s_status_word, __ATOMIC_ACQUIRE);
    const uint32_t vol = (w & 0xFFu) > 100u ? 100u : (w & 0xFFu);
    uint32_t brt = (uint32_t)__atomic_load_n(&s_brightness_want, __ATOMIC_ACQUIRE);
    if (brt > 100u) {
        brt = 100u;
    }

    /* BOTH NUMBERS MUST SURVIVE AT 100. The detail column stores 10 glyphs:
     * "L VOL 100%" is exactly 10 and survived; "R LIGHT 100%" is 12 and read
     * "R LIGHT 10" — one of the two levels the shade exists to show, silently
     * wrong rather than absent. LGT keeps both columns at exactly 10 in the
     * worst case; a host assertion pins that case. */
    int len = sp_str(s_shade_vol, 0, SP_COL_MAX, "L VOL ");
    (void)sp_pct(s_shade_vol, len, SP_COL_MAX, vol);
    len = sp_str(s_shade_light, 0, SP_COL_MAX, "R LGT ");
    (void)sp_pct(s_shade_light, len, SP_COL_MAX, brt);

    if (s_detail_ease > 0) {
        sp_compose_detail(s_detail_space);
    }
}

/* Advance every shell ease. Called once per frame from overlay_fade_tick, on
 * the render task, with the same cstep the rest of the presenter fades on so
 * the shell and the existing overlays stay visually in step. */
static void sp_fade_tick(uint32_t now_ms, uint32_t dt_ms, int cstep)
{
    const uint32_t nav = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    const uint8_t serial = (uint8_t)(nav >> NAV_SERIAL_SHIFT);
    const uint8_t space = (uint8_t)(nav & NAV_SPACE_MASK);
    const uint8_t overlay = (uint8_t)((nav & NAV_OVL_MASK) >> NAV_OVL_SHIFT);

    if (serial != s_space_serial_seen) {
        s_space_serial_seen = serial;
        s_space_from = (uint8_t)((nav >> NAV_PREV_SHIFT) & NAV_SPACE_MASK);
        s_space_to = space;
        s_space_dir = (nav & NAV_FORWARD_BIT) != 0U ? 1 : -1;
        s_space_prog = 0;
        s_space_hold_ms = now_ms + (uint32_t)JR_DISPLAY_SPACE_HOLD_MS;
    } else if (s_space_to != space) {
        s_space_to = space;          /* serial wrap: snap rather than lie */
        s_space_from = space;
    }

    int sstep = (int)((dt_ms * 256u) / (uint32_t)JR_DISPLAY_SPACE_MS);
    if (sstep < 1) {
        sstep = 1;
    }
    s_space_prog += sstep;
    if (s_space_prog > 256) {
        s_space_prog = 256;
    }
    s_space_ease = fade_smoothstep(s_space_prog);

    /* The orbit mark: where the slide puts it this frame, on the ring as it
     * is NOW, minus whatever re-spacing residue is still easing out. Computed
     * once here, not per strip, so every strip draws the same mark. */
    {
        const bool live = sp_desk_live();
        const int n = sp_ring_n(live);
        const int a_from = sp_orbit_angle(s_space_from, live) * 16;
        const int a_to = sp_orbit_angle(s_space_to, live) * 16;
        /* Signed shortest way round: a wrap is one step forward, not n-1
         * steps back. */
        const int d = (((a_to - a_from) + 2048) & 4095) - 2048;
        const int target = a_from + (d * s_space_ease) / 256;
        if (n != s_orbit_n_seen) {
            if (s_orbit_n_seen != 0) {
                s_orbit_off16 += (((target - s_orbit_a16) + 2048) & 4095) - 2048;
            }
            s_orbit_n_seen = n;
        }
        if (s_orbit_off16 != 0) {
            const bool neg = s_orbit_off16 < 0;
            int mag = neg ? -s_orbit_off16 : s_orbit_off16;
            int step = (mag * sstep) / 256;
            if (step < 1) {
                step = 1;
            }
            mag = mag > step ? mag - step : 0;
            s_orbit_off16 = neg ? -mag : mag;
        }
        s_orbit_a16 = target - s_orbit_off16;
    }

    /* The sheet keeps the space it was opened on for the whole of its own
     * fade-out, so dismissing it never flashes another space's facts. */
    if (overlay == (uint8_t)JR_DISPLAY_OVERLAY_DETAIL) {
        s_detail_space = space;
        s_detail_prog += cstep;
        if (s_detail_prog > 256) {
            s_detail_prog = 256;
        }
    } else {
        s_detail_prog -= cstep;
        if (s_detail_prog < 0) {
            s_detail_prog = 0;
        }
    }
    s_detail_ease = fade_smoothstep(s_detail_prog);

    if (sp_shade_open()) {
        s_shade_prog += cstep;
        if (s_shade_prog > 256) {
            s_shade_prog = 256;
        }
    } else {
        s_shade_prog -= cstep;
        if (s_shade_prog < 0) {
            s_shade_prog = 0;
        }
    }
    s_shade_ease = fade_smoothstep(s_shade_prog);

    /* The backdrop veil is the deepest any live element asks for. Because a
     * slide out of JARVIS still carries the outgoing space's weight, going
     * home fades the veil away with the slide instead of cutting it a frame
     * early. */
    int veil = 0;
    if (s_space_to != (uint8_t)JR_DISPLAY_SPACE_JARVIS) {
        veil = s_space_ease;
    }
    if (s_space_from != (uint8_t)JR_DISPLAY_SPACE_JARVIS &&
        256 - s_space_ease > veil) {
        veil = 256 - s_space_ease;
    }
    if (s_detail_ease > veil) {
        veil = s_detail_ease;
    }
    if (s_shade_ease > veil) {
        veil = s_shade_ease;
    }
    s_space_veil = veil;

    /* The orbital indicator outlives the slide by a short hold, so a swipe
     * leaves a readable trace of where you landed and then goes away and
     * JARVIS is a clean face again. */
    s_space_on = veil > 0 || s_space_prog < 256 ||
                 (int32_t)(now_ms - s_space_hold_ms) < 0;
    if (s_space_on) {
        sp_compose();
    }
}

bool jr_display_choices_active(void)
{
    return __atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE) > 0;
}

void jr_display_set_choice_selected(int index)
{
    s_choice_selected = index;
}

int jr_display_choice_hit(int x, int y)
{
    const int n = __atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE);
    if (n <= 0) {
        return -1;
    }
    int idx = -1;
    return hud_choice_hit(s_choices, n, x, y, &idx) ? idx : -1;
}

void jr_display_set_hud_enabled(bool enabled)
{
    __atomic_store_n(&s_hud_enabled, enabled ? 1u : 0u, __ATOMIC_RELAXED);
}

bool jr_display_hud_enabled(void)
{
    return __atomic_load_n(&s_hud_enabled, __ATOMIC_RELAXED) != 0u;
}

void jr_display_set_hud_env(uint8_t batt_pct, bool charging,
                            bool privacy_muted,
                            float roll_deg, float pitch_deg)
{
    int8_t ox = 0, oy = 0;
    hud_tilt_offset(roll_deg, pitch_deg, &ox, &oy);
    const uint32_t word = (uint32_t)batt_pct
                        | (charging ? (1u << 8) : 0u)
                        | (privacy_muted ? (1u << 9) : 0u)
                        | ((uint32_t)(uint8_t)ox << 16)
                        | ((uint32_t)(uint8_t)oy << 24);
    __atomic_store_n(&s_hud_env_word, word, __ATOMIC_RELAXED);
}

static uint8_t hud_face_of(jr_face_t f)
{
    switch (f) {
    case JR_FACE_IDLE:      return HUD_FACE_IDLE;
    case JR_FACE_LISTENING: return HUD_FACE_LISTEN;
    case JR_FACE_THINKING:  return HUD_FACE_THINK;
    case JR_FACE_SPEAKING:  return HUD_FACE_SPEAK;
    case JR_FACE_ERROR:     return HUD_FACE_ERROR;
    case JR_FACE_RESTING:   return HUD_FACE_IDLE;
    case JR_FACE_MUTED:     return HUD_FACE_IDLE;
    case JR_FACE_LINKING:   return HUD_FACE_THINK;
    default:                return HUD_FACE_IDLE;
    }
}

/* Full-strip backdrop dim shared by the ask overlay (STATE-07) and the clock
 * (UI-01). hud_dim565 folds the panel_native -> darken (>>2 per channel) ->
 * panel_order_color chain into one masked shift (proven equal for all 65536
 * values in the host suite) — two per-pixel byte swaps over 217k px/frame
 * cost measurable frame rate. Unswitched on the hoisted byte-order flag so
 * the inline folds to one constant path. */
static void dim_strip(jr_display_ctx_t *ctx, int y1, int y2, uint16_t *pixels)
{
    const bool swap = ctx->board.swap_color_bytes;
    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        if (swap) {
            for (int x = 0; x < HUD_W; ++x) {
                row[x] = hud_dim565(row[x], true);
            }
        } else {
            for (int x = 0; x < HUD_W; ++x) {
                row[x] = hud_dim565(row[x], false);
            }
        }
    }
}

/* Variable-strength companion to dim_strip for the watch fade: k 1..31 eases
 * toward the full quarter dim via hud_fade565. ~3x dim_strip's per-pixel cost
 * (the variable multiply cannot use the split-field fold), paid only for the
 * ~JR_DISPLAY_FADE_MS of a transition — the settled watch stays on dim_strip.
 * Unswitched on the hoisted byte-order flag for the same reason dim_strip is. */
static void fade_strip(jr_display_ctx_t *ctx, int y1, int y2, uint16_t *pixels,
                       int k)
{
    const bool swap = ctx->board.swap_color_bytes;
    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        if (swap) {
            for (int x = 0; x < HUD_W; ++x) {
                row[x] = hud_fade565(row[x], true, k);
            }
        } else {
            for (int x = 0; x < HUD_W; ++x) {
                row[x] = hud_fade565(row[x], false, k);
            }
        }
    }
}

/* STATE-07: the modal ask presentation, run per flushed strip only while a
 * question is on screen or easing off it (a modal state measured in seconds,
 * so a few fps of cost is acceptable). The whole strip dims — the same transform
 * apply_surface_overlay uses for its backdrop — then the question text and the
 * per-arc labels draw over the dimmed face; hud_overlay_choices follows in the
 * caller so the arcs stay bright ON TOP. Reads only the s_choice_* statics
 * published by jr_display_present_choices — nothing is derived here. Runs on
 * the render task; full-width strips guaranteed by the caller. */
static void apply_ask_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                              uint16_t *pixels, int cn, int e)
{
    /* e = s_ask_ease (0..256): dim depth and text brightness ride it
     * together, so the whole modal surfaces and sinks as one presentation
     * instead of stamping. e==256 is the settled path — dim_strip fold,
     * full-brightness text. */
    const int k = (e * 32) >> 8;
    if (k <= 0) {
        return;
    }
    const int c = e > 255 ? 255 : e;
    const int sel = s_choice_selected;
    /* Hoisted per call: the byte order never changes mid-strip. */
    const uint16_t px_white = panel_order_color(
        ctx, (uint16_t)(((c & 0xF8) << 8) | ((c & 0xFC) << 3) | (c >> 3)));
    const uint16_t px_cyan = panel_order_color(
        ctx, (uint16_t)((((63 * c / 255) & 0x3F) << 5) | (31 * c / 255)));
    if (k >= 32) {
        dim_strip(ctx, y1, y2, pixels);
    } else {
        fade_strip(ctx, y1, y2, pixels, k);
    }
    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;

        /* Question: up to two scale-2 lines, centred, white. The centred
         * origin is exact (see present), so the line ends at HUD_W - x0 and
         * no length is re-derived per strip. */
        for (int l = 0; l < 2; ++l) {
            const char *text = s_choice_q_line[l];
            const int ty = s_choice_q_y[l];
            if (text[0] == '\0' || y < ty || y >= ty + 14) {
                continue;
            }
            const int tx = s_choice_q_x[l];
            for (int x = tx; x < HUD_W - tx; ++x) {
                if (surface_text_pixel(text, 24U, x, y, tx, ty, 2)) {
                    row[x] = px_white;
                }
            }
        }

        /* Labels: scale 1 at the cached anchors (r=205, inside the arcs).
         * The selected answer in white, the rest cyan — mirroring the arcs. */
        for (int i = 0; i < cn; ++i) {
            const char *lab = s_choices[i].label;
            const int cap = s_choice_label_len[i];
            const int ly = s_choice_label_y[i];
            if (lab == NULL || cap <= 0 || y < ly || y >= ly + 7) {
                continue;
            }
            /* Belt and braces under the one-frame-stale discipline: lab
             * points at the display-owned copy (always NUL-terminated inside
             * its 25 bytes), and clamping the sweep to the LIVE string means
             * a stale longer cached len can never walk a shorter replacement
             * past its NUL — any future regression is torn text for a frame,
             * never an out-of-bounds read. */
            const int len = (int)strnlen(lab, (size_t)cap);
            if (len <= 0) {
                continue;
            }
            const int lx = s_choice_label_x[i];
            const uint16_t color = (i == sel) ? px_white : px_cyan;
            for (int x = lx; x < lx + 6 * len; ++x) {
                if (surface_text_pixel(lab, (size_t)len, x, y, lx, ly, 1)) {
                    row[x] = color;
                }
            }
        }
    }
}

/* STATE-04: the caption chip — a round-safe lower band with scale-2 text.
 * Scale 1 was technically legible in screenshots but unreadable on the
 * physical 1.75-inch panel. Nineteen characters per line fit the glass chord
 * at this radius; the band eases with the existing caption transition. */
#define JR_CAPTION_Y0 360
#define JR_CAPTION_Y1 430          /* inclusive */

static void apply_caption_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                                  uint16_t *pixels)
{
    int ys = y1 > JR_CAPTION_Y0 ? y1 : JR_CAPTION_Y0;
    int ye = (y2 - 1) < JR_CAPTION_Y1 ? (y2 - 1) : JR_CAPTION_Y1;
    if (ys > ye) {
        return;                    /* strip misses the band: one compare */
    }
    /* Caption entrance/exit rides s_caption_ease: the band's dim deepens and
     * the text brightens together, so the chip surfaces out of the face
     * instead of stamping onto it. Settled (e==256) takes the exact
     * pre-fade path — hud_dim565 fold, pure white text. */
    const int e = s_caption_ease;
    const int k = (e * 32) >> 8;
    if (k <= 0) {
        return;
    }
    const bool swap = ctx->board.swap_color_bytes;
    const int c = e > 255 ? 255 : e;
    const uint16_t px_text = panel_order_color(
        ctx, (uint16_t)(((c & 0xF8) << 8) | ((c & 0xFC) << 3) | (c >> 3)));
    for (int y = ys; y <= ye; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        const int half = hud_glass_chord(y);
        int xlo = 233 - half;
        int xhi = 233 + half;
        if (xlo < 0)         xlo = 0;
        if (xhi > HUD_W - 1) xhi = HUD_W - 1;
        if (k >= 32) {
            if (swap) {
                for (int x = xlo; x <= xhi; ++x) {
                    row[x] = hud_dim565(row[x], true);
                }
            } else {
                for (int x = xlo; x <= xhi; ++x) {
                    row[x] = hud_dim565(row[x], false);
                }
            }
        } else {
            for (int x = xlo; x <= xhi; ++x) {
                row[x] = hud_fade565(row[x], swap, k);
            }
        }
        for (int l = 0; l < 2; ++l) {
            const char *text = s_caption_line[l];
            const int ty = s_caption_y[l];
            if (text[0] == '\0' || y < ty || y >= ty + 14) {
                continue;
            }
            const int tx = s_caption_x[l];
            for (int x = tx; x < HUD_W - tx; ++x) {
                if (surface_text_pixel(text, 19U, x, y, tx, ty, 2)) {
                    row[x] = px_text;
                }
            }
        }
    }
}

/* Explicit Watch: the baked face is the dial (bezel ticks at r200-214), with
 * hud_overlay_clock's hands and hub riding s_clock_ease. The old ask-style
 * full-frame dim transformed 217k pixels and dropped Watch from 15-16 to
 * 11 FPS; the already-dark face needs no second dim. Choice asks outrank it,
 * and it renders under the caption. Time comes from the shown-word latch so
 * fade-out retains the prior time. Render task only. */
/* Defined with the shell primitives further down; the watch's disc-bounded
 * clear needs it up here. */
static int sp_isqrt(int v);

static void apply_clock_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                                uint16_t *pixels)
{
    const int e = s_clock_ease;
    if (e <= 0) {
        return;
    }
    /* THE SHEET OUTRANKS THE FACE.
     *
     * Tapping the focal object opens that space's detail sheet, but on WATCH
     * the clear below then painted over it, so the sheet opened INVISIBLY:
     * the tap was accepted, the overlay state changed, the caption said
     * "DETAIL - DOWN TO CLOSE", and the glass still showed only the hands.
     * Measured as 966 -> 944 lit pixels across the tap, against 10741 -> 2312
     * for the same tap on POWER.
     *
     * The clock keeps its clean dial by clearing, so it cannot also be the
     * bottom layer of a stack. It steps aside for the one surface the owner
     * explicitly asked for, exactly as the agent rim steps aside for an ask.
     *
     * BOTH shell surfaces, not just the sheet. The first version of this guard
     * tested only s_detail_ease, which left the CONTROL SHADE in the same
     * hole — and the shade is the worse place to lose: every one of its
     * primitives is folded through sp_clip into the same r<=214 disc this
     * function clears, so on WATCH the owner got a black dial, the caption
     * "CONTROLS - UP TO CLOSE", and an invisible-but-LIVE volume arc and
     * privacy button. jr_display_hit still routed taps to them, so a blind
     * tap in the lower disc could flip the microphone with nothing on screen
     * to warn of it. A control you cannot see but can still hit is worse than
     * one that is merely missing. */
    if (s_detail_ease > 0 || s_shade_ease > 0) {
        return;
    }
    /* THE WATCH REPLACES THE FACE — they are never on the glass together.
     * The hands used to be drawn straight over the baked arc-reactor art, so
     * two unrelated HUDs shared the same pixels and read as one cluttered
     * image ("the arc reactor and watch should never be on screen at the same
     * time").
     *
     * Cleared with a memset rather than the per-pixel dim that was tried
     * before: that transformed 217k pixels and cost the Watch 15-16 -> 11 fps.
     * A strip clear is ~11 KB of straight-line writes, and on an AMOLED black
     * costs no backlight either. The caption is drawn AFTER this, so status
     * text survives; the orbit indicator does not, which is the deliberate
     * trade for a clean dial.
     *
     * BUT THE CLEAR STOPS AT THE SHELL RADIUS. It used to blank the whole row,
     * which also erased the two tenants that live OUTSIDE the shell and are
     * supposed to be permanent: the battery arc at r215-220 and the gold
     * privacy ring at r221-222. The cost of that was worst exactly where it
     * mattered — the watch is the screen you glance at, and it was the one
     * screen that could not tell you the microphone was muted.
     *
     * r214 is not a new number: JR_DISPLAY_SHELL_R_MAX is already the contract
     * boundary between shell territory and the persistent rim, and the hands
     * only reach r190, so the dial still clears completely. Rows that miss the
     * disc entirely are skipped rather than cleared. */
    for (int y = y1; y < y2; ++y) {
        const int dy = y - HUD_H / 2;
        const int d2 = JR_DISPLAY_SHELL_R_MAX * JR_DISPLAY_SHELL_R_MAX
                       - dy * dy;
        if (d2 <= 0) {
            continue;
        }
        const int half = sp_isqrt(d2);
        int xlo = HUD_W / 2 - half;
        int xhi = HUD_W / 2 + half;
        if (xlo < 0) {
            xlo = 0;
        }
        if (xhi > HUD_W - 1) {
            xhi = HUD_W - 1;
        }
        memset(pixels + (size_t)(y - y1) * HUD_W + xlo, 0,
               (size_t)(xhi - xlo + 1) * sizeof(uint16_t));
    }

    const uint32_t w = s_clock_shown_word;
    hud_overlay_clock(pixels, y1, y2 - y1, ctx->board.swap_color_bytes,
                      (int)((w >> 8) & 0xFFu), (int)(w & 0xFFu),
                      (int)((w >> 17) & 0x3Fu),
                      e > 255 ? 255 : e);
}

/* ===== SPATIAL SHELL: geometry and drawing ===============================
 *
 * Strip-local, allocation-free, integer-only. Every primitive takes the strip
 * row it is writing and returns immediately if the shape does not reach that
 * row, so a shape costs one multiply on the rows it misses.
 *
 * THE CLIP IS THE CONTRACT. sp_clip() folds every x interval into the
 * JR_DISPLAY_SHELL_R_MAX circle before a single pixel is written. That is why
 * the shell provably cannot touch the battery rim, the privacy ring, or the
 * choice arcs: not one renderer below is trusted to stay in its lane, they
 * are all bounded by one function.
 */

#define SP_CX          (HUD_W / 2)
#define SP_CY          (HUD_H / 2)
#define SP_R_SHELL     JR_DISPLAY_SHELL_R_MAX
#define SP_SLIDE_PX    150            /* how far a space travels on a swipe */

/* SP_A_TOP (12 o'clock) is defined with the compose-side state above: the
 * orbit slot arithmetic needs it before the drawing section begins. */

/* The free band is r185-194, measured: baked art sits at r184 and r195. The
 * rail used to span 184-196 and overwrote both edges every frame. The inner
 * radius of sp_annulus_row is inclusive of pixels whose true radius floors to
 * one less (185 painted r184), so the inner edges sit one unit inside. */
#define SP_ORB_TRACK_IN  186
#define SP_ORB_TRACK_OUT 194
#define SP_ORB_R         190
#define SP_ORB_MARK_IN   187
#define SP_ORB_MARK_OUT  194

#define SP_FOCAL_IN    76
#define SP_FOCAL_OUT   96
/* The firmware-update ring, on every space. The inner edge clears every focal
 * object (r<=104) and the headline's worst-case glyph corner (12 glyphs at
 * scale 2, centred on SP_LABEL_Y, reach r131.5); the outer edge stays well
 * inside JR_DISPLAY_SAFE_R. */
#define SP_OTA_IN      140
#define SP_OTA_OUT     154
#define SP_FOCAL_HIT   116
#define SP_FOCAL_TEXT_Y 219
#define SP_LABEL_Y     330

#define SP_SHEET_Y0    150
#define SP_SHEET_Y1    358   /* two rows shy of the caption band at 360  */
#define SP_SHEET_HEAD_Y 162
/* Nine 14 px rows at a 20 px pitch run 184..357, ending at SP_SHEET_Y1.
 * The sheet was six rows at 26 px until STATUS gathered link, mic and
 * uptime under the battery and the update; the pitch shrank rather than
 * the glyphs, and the band grew down to the caption's edge rather than
 * clipping a row. Both text columns' worst corner (x 128/338 at y 357) is
 * r163, inside JR_DISPLAY_SAFE_R; test_detail_sheet_rows_stay_inside pins
 * it. */
#define SP_SHEET_ROW_Y 184
#define SP_SHEET_ROW_DY 20
#define SP_SHEET_LEFT  128
#define SP_SHEET_RIGHT 338

/* The minimal voice shade: one volume arc above one privacy action. Display
 * brightness stays in Settings; Home stays on the global double-tap. */
#define SP_VOL_IN      130
#define SP_VOL_OUT     144
#define SP_ARC_A0      136
#define SP_ARC_A1      248
#define SP_ARC_SPAN    112
#define SP_VOL_CAP_R   158
#define SP_VOL_TEXT_Y  176
#define SP_LIGHT_TEXT_Y 196
#define SP_HINT_Y      214
#define SP_BTN_CY      292
#define SP_BTN_R       62
#define SP_BTN_PRIV_CX SP_CX

#define SP_ARC_HIT_DY  10
#define SP_VOL_HIT_IN  124
#define SP_VOL_HIT_OUT 160

#define SP_C_CYAN      0x07FF
#define SP_C_CYAN_DIM  0x0330
#define SP_C_INK       0xF79E   /* cool white: never pure, never clinical */
#define SP_C_GREY      0x8C71
#define SP_C_AMBER     0xFD20
#define SP_C_RED       0xF800   /* low battery: the rim's red, not amber */
#define SP_C_GOLD      0xFEA0
#define SP_C_GOLD_DIM  0x6300
#define SP_C_TRACK     0x2124
#define SP_C_PLATE     0x1082
#define SP_C_VIOLET    0xA81F
/* WEATHER's sky accents. Every one is a hue the glass already speaks or a
 * dimming of one — and none is gold: CLEAR is amber, because gold means
 * muted everywhere on this glass and a sunny day must not read as a closed
 * microphone. */
#define SP_C_FOG       0x03EF   /* cyan at half: the sky is there, softened */
#define SP_C_RAIN      0x049F   /* blue-cyan: also the rain-chance ring     */
#define SP_C_SNOW      SP_C_INK

/* WEATHER geometry. The gauge rides the standard focal ring; the current
 * temperature is a mark that stands proud of it on both sides, the way the
 * lit petal did, so it reads at arm's length. The rain ring is a thin inner
 * band, and the disc inside it holds three lines of text whose worst-case
 * corners (nine glyphs at scale 2, "STALE 45M" / "H104 L-12") reach r65.5. */
#define SP_WX_MARK_IN   72
#define SP_WX_MARK_OUT  100
#define SP_WX_MARK_HALF 3      /* +-3/256 turn, about four degrees each way */
#define SP_WX_RAIN_IN   66
#define SP_WX_RAIN_OUT  70
#define SP_WX_AGE_Y     196    /* scale 2, rows 196..209                     */
#define SP_WX_HILO_Y    250    /* scale 2, rows 250..263                     */

/* ACTIVITY geometry: three rows at a 36 px pitch about the centre, each 24
 * glyphs at scale 2 centred on the axis (x 89..374). The worst corner is
 * r150 — inside JR_DISPLAY_SAFE_R, outside every focal ring, and crossed by
 * the update ring only while an update is actually in flight. */
#define SP_ACT_X0      (SP_CX - (6 * 2 * SP_ACT_ROW_GLYPHS) / 2)
#define SP_ACT_PITCH   36
#define SP_ACT_Y0      (SP_CY - SP_ACT_PITCH - 7)

/* Q15 sine, quarter wave, mirrored. 130 bytes of rodata buys every arc in
 * this layer with no float, no libm, and no atan anywhere. */
static const int16_t s_sp_sin[65] = {
        0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
     6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767,
};

static int sp_sin(int a)
{
    a &= 255;
    if (a <= 64) {
        return s_sp_sin[a];
    }
    if (a <= 128) {
        return s_sp_sin[128 - a];
    }
    if (a <= 192) {
        return -s_sp_sin[a - 128];
    }
    return -s_sp_sin[256 - a];
}

static inline int sp_cos(int a)
{
    return sp_sin(a + 64);
}

/* Integer square root by the classic restoring shift-and-subtract. The seed
 * is 4^8, which covers every radius the shell can ask about (214^2 = 45796). */
static int sp_isqrt(int v)
{
    if (v <= 0) {
        return 0;
    }
    int rem = v, root = 0, bit = 1 << 16;
    while (bit > rem) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (rem >= root + bit) {
            rem -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

/* THE round safe-area guard. Folds [*xlo, *xhi] into both the panel and the
 * shell circle for this row; false means the row has nothing to draw. */
static bool sp_clip(int y, int *xlo, int *xhi)
{
    const int dy = y - SP_CY;
    const int d2 = SP_R_SHELL * SP_R_SHELL - dy * dy;
    if (d2 <= 0) {
        return false;
    }
    const int half = sp_isqrt(d2);
    if (*xlo < SP_CX - half) {
        *xlo = SP_CX - half;
    }
    if (*xhi > SP_CX + half) {
        *xhi = SP_CX + half;
    }
    if (*xlo < 0) {
        *xlo = 0;
    }
    if (*xhi > HUD_W - 1) {
        *xhi = HUD_W - 1;
    }
    return *xlo <= *xhi;
}

/* An angular span held as two edge normals, so testing a pixel is two
 * multiplies and two compares — no atan2, no division, no float. Spans are
 * always measured about the panel axis. */
typedef struct {
    int c0, s0, c1, s1;
} sp_span_t;

static void sp_span_set(sp_span_t *sp, int a0, int a1)
{
    sp->c0 = sp_cos(a0);
    sp->s0 = sp_sin(a0);
    sp->c1 = sp_cos(a1);
    sp->s1 = sp_sin(a1);
}

static inline bool sp_in_span(const sp_span_t *sp, int dx, int dy)
{
    return (sp->c0 * dy - sp->s0 * dx) >= 0 && (dx * sp->s1 - dy * sp->c1) >= 0;
}

/* Build the spans for an arc that may exceed a half turn.
 *
 * sp_in_span is the INTERSECTION of two half-planes, so one span cannot
 * express more than 180 degrees. Past that the intersection starts shrinking
 * as the requested sweep grows, and at a full turn the two edges coincide and
 * the fill collapses to a ray. A battery at 79% was measured painting a 70 deg
 * wedge parked at the lower right instead of a nearly-full ring: predicted
 * 360 - pct*3.6 degrees starting at pct*3.6 - 180, and the panel agreed on
 * both the length and the position.
 *
 * DESK and the OTA ring had each open-coded this chunking; POWER was written
 * without it, which is the whole bug. One helper now, so the next ring cannot
 * forget. Chunking at 96 units keeps every span well inside a half turn, and
 * three of them cover a full 256-unit circle.
 *
 * Returns the number of spans written. `sweep` is in 256ths of a turn. */
#define SP_ARC_SEG_MAX 3

static int sp_arc_segments(sp_span_t *seg, int nmax, int start, int sweep)
{
    int n = 0;
    while (sweep > 0 && n < nmax) {
        const int step = sweep > 96 ? 96 : sweep;
        sp_span_set(&seg[n++], start, start + step);
        start += step;
        sweep -= step;
    }
    return n;
}

/* One primitive covers ring, arc, disc and button: rin <= 0 means filled,
 * span == NULL means the whole turn. */
static void sp_annulus_row(uint16_t *row, int y, int cx, int cy, int rin,
                           int rout, const sp_span_t *span, uint16_t px)
{
    const int dy = y - cy;
    const int out = rout * rout - dy * dy;
    if (out <= 0) {
        return;
    }
    const int xo = sp_isqrt(out);
    const int in2 = rin > 0 ? rin * rin - dy * dy : -1;
    const int xi = in2 > 0 ? sp_isqrt(in2) : -1;
    /* The wedge test used `y - SP_CY` here while the annulus itself is placed
     * about `cy` (= SP_CY + oy during the vertical slide). One vector, two
     * origins: the span sheared by exactly the slide offset, so every focal
     * arc's endpoints walked round the ring mid-transition. Same origin now. */
    for (int side = 0; side < 2; ++side) {
        int lo, hi;
        if (xi < 0) {
            if (side != 0) {
                break;
            }
            lo = cx - xo;
            hi = cx + xo;
        } else if (side == 0) {
            lo = cx - xo;
            hi = cx - xi;
        } else {
            lo = cx + xi;
            hi = cx + xo;
        }
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        if (span == NULL) {
            for (int x = lo; x <= hi; ++x) {
                row[x] = px;
            }
        } else {
            for (int x = lo; x <= hi; ++x) {
                if (sp_in_span(span, x - cx, dy)) {
                    row[x] = px;
                }
            }
        }
    }
}

static void sp_dot_row(uint16_t *row, int y, int cx, int cy, int n, uint16_t px)
{
    const int half = n / 2;
    if (y < cy - half || y > cy - half + n - 1) {
        return;
    }
    int lo = cx - half, hi = cx - half + n - 1;
    if (!sp_clip(y, &lo, &hi)) {
        return;
    }
    for (int x = lo; x <= hi; ++x) {
        row[x] = px;
    }
}

/* Reuses the surface layer's 5x7 glyph sampler: one font on the glass, and
 * the shell inherits its exact metrics for free. */
static void sp_text_row(uint16_t *row, int y, const char *text, int len,
                        int x0, int y0, int scale, uint16_t px)
{
    if (len <= 0 || y < y0 || y >= y0 + 7 * scale) {
        return;
    }
    int lo = x0, hi = x0 + 6 * scale * len - 1;
    if (!sp_clip(y, &lo, &hi)) {
        return;
    }
    for (int x = lo; x <= hi; ++x) {
        if (surface_text_pixel(text, (size_t)len, x, y, x0, y0, scale)) {
            row[x] = px;
        }
    }
}

static inline int sp_text_cx(int len, int scale, int ox)
{
    return SP_CX + ox - (6 * scale * len) / 2;
}

static inline int sp_len(const char *s, int cap)
{
    int n = 0;
    while (n < cap && s[n] != '\0') {
        ++n;
    }
    return n;
}

/* Colour in panel byte order, pre-scaled by the element's own 0..255 fade so
 * a slide cross-dissolves without ever reading back the frame. */
static uint16_t sp_tint(const jr_display_ctx_t *ctx, uint16_t native, int st)
{
    if (st < 255) {
        const int r = (((native >> 11) & 0x1F) * st) / 255;
        const int g = (((native >> 5) & 0x3F) * st) / 255;
        const int b = ((native & 0x1F) * st) / 255;
        native = (uint16_t)((r << 11) | (g << 5) | b);
    }
    return panel_order_color(ctx, native);
}

/* Round-clipped backdrop. Identical fold to dim_strip/fade_strip, but bounded
 * by the shell circle so the veil never darkens the battery rim or the
 * privacy ring. */
static void sp_veil(const jr_display_ctx_t *ctx, int y1, int y2,
                    uint16_t *pixels, int k)
{
    const bool swap = ctx->board.swap_color_bytes;
    for (int y = y1; y < y2; ++y) {
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        /* Black stays black: most of the face outside the reactor is 0x0000,
         * and folding it was the single largest per-frame cost of the shell
         * (217k pixels a frame, every shell screen). A compare beats the
         * shift-and-mask on the pixels that need nothing. */
        if (k >= 32) {
            if (swap) {
                for (int x = lo; x <= hi; ++x) {
                    if (row[x] != 0U) {
                        row[x] = hud_dim565(row[x], true);
                    }
                }
            } else {
                for (int x = lo; x <= hi; ++x) {
                    if (row[x] != 0U) {
                        row[x] = hud_dim565(row[x], false);
                    }
                }
            }
        } else {
            for (int x = lo; x <= hi; ++x) {
                if (row[x] != 0U) {
                    row[x] = hud_fade565(row[x], swap, k);
                }
            }
        }
    }
}

/* DESK: a progress ring around the number, broken into <=96-unit segments
 * because a span is a wedge intersection and a wedge cannot exceed a half
 * turn. Segmenting also gives the ring visible joints, which reads as
 * mechanical travel rather than a plain pie. */
static void sp_focal_desk(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int oy, int st)
{
    const uint32_t w = __atomic_load_n(&s_desk_word, __ATOMIC_ACQUIRE);
    int prog = (int)(w & 0xFFu);
    if (prog > 100) {
        prog = 100;
    }
    const jr_display_agent_state_t state =
        (jr_display_agent_state_t)((w >> 8) & 0xFu);
    const uint16_t track = sp_tint(ctx, SP_C_TRACK, st);
    const uint16_t fill = sp_tint(ctx, state == JR_DISPLAY_AGENT_NONE
                                           ? SP_C_CYAN
                                           : agent_native_color(state), st);
    const uint16_t ink = sp_tint(ctx, SP_C_INK, st);
    const int cx = SP_CX;

    char num[6];
    const int len = sp_pct(num, 0, (int)sizeof num, (uint32_t)prog);
    const int tx = cx - (18 * len) / 2;

    sp_span_t seg[SP_ARC_SEG_MAX];
    const int nseg = sp_arc_segments(seg, SP_ARC_SEG_MAX, SP_A_TOP,
                                     (prog * 256) / 100);

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        sp_annulus_row(row, y, cx, SP_CY + oy, SP_FOCAL_IN, SP_FOCAL_OUT, NULL, track);
        for (int i = 0; i < nseg; ++i) {
            sp_annulus_row(row, y, cx, SP_CY + oy, SP_FOCAL_IN, SP_FOCAL_OUT,
                           &seg[i], fill);
        }
        sp_text_row(row, y, num, len, tx, SP_FOCAL_TEXT_Y, 3, ink);
    }
}

/* The sky picks the accent. Cyan is the glass's own colour, so an ordinary
 * sky is the ordinary glass; the others are the hues already given meaning
 * elsewhere (amber: look, violet: on-trial) borrowed for a day that is worth
 * a glance. Gold is never returned here — see the palette. */
static uint16_t sp_sky_native(jr_display_sky_t sky)
{
    switch (sky) {
    case JR_DISPLAY_SKY_CLEAR:  return SP_C_AMBER;
    case JR_DISPLAY_SKY_FOG:    return SP_C_FOG;
    case JR_DISPLAY_SKY_RAIN:   return SP_C_RAIN;
    case JR_DISPLAY_SKY_STORM:  return SP_C_VIOLET;
    case JR_DISPLAY_SKY_SNOW:   return SP_C_SNOW;
    case JR_DISPLAY_SKY_PARTLY:
    case JR_DISPLAY_SKY_CLOUDS:
    default:                    return SP_C_CYAN;
    }
}

/* WEATHER: the arc is the day. A 270-degree track stands for 40..100 F; the
 * span from today's low to its high fills in the sky's accent, dimmed, and
 * the temperature right now is a bright mark standing proud of the ring on
 * both sides — so "cool morning, hot afternoon, and it is nearly there" reads
 * as a shape before it reads as numbers. Inside: a thin ring for the chance
 * of rain, and the numbers themselves — the temperature large, the high and
 * low under it, and above it how old all of this is once it is old enough to
 * matter.
 *
 * Honesty rules, in order: muted turns every accent gold like every other
 * screen; stale (> 30 min) turns them grey, because a number from an hour ago
 * is still a number but no longer a colour; and !valid draws the two bare
 * tracks and not one digit. */
static void sp_focal_weather(const jr_display_ctx_t *ctx, int y1, int y2,
                             uint16_t *pixels, int oy, int st)
{
    const bool muted = sp_privacy_muted();
    const uint16_t accent_native =
        muted ? SP_C_GOLD : (s_wx_stale ? SP_C_GREY : sp_sky_native(s_wx_sky));
    const uint16_t rain_native =
        muted ? SP_C_GOLD_DIM : (s_wx_stale ? SP_C_GREY : SP_C_RAIN);
    const uint16_t track = sp_tint(ctx, SP_C_TRACK, st);
    const uint16_t band = sp_tint(ctx, accent_native, (st * 104) / 255);
    const uint16_t mark = sp_tint(ctx, accent_native, st);
    const uint16_t rain = sp_tint(ctx, rain_native, st);
    const uint16_t ink = sp_tint(ctx, SP_C_INK, st);
    const uint16_t grey = sp_tint(ctx, SP_C_GREY, st);
    const int cx = SP_CX;
    const int cy = SP_CY + oy;

    sp_span_t trk[SP_ARC_SEG_MAX], bnd[SP_ARC_SEG_MAX], rn[SP_ARC_SEG_MAX];
    sp_span_t mk = {0, 0, 0, 0};
    const int ntrk = sp_arc_segments(trk, SP_ARC_SEG_MAX, SP_WX_A0, SP_WX_SWEEP);
    const int nbnd = s_wx_valid
        ? sp_arc_segments(bnd, SP_ARC_SEG_MAX, s_wx_band_a0, s_wx_band_sweep)
        : 0;
    const int nrn = s_wx_valid
        ? sp_arc_segments(rn, SP_ARC_SEG_MAX, SP_A_TOP, s_wx_rain_sweep)
        : 0;
    if (s_wx_valid) {
        sp_span_set(&mk, s_wx_mark_a - SP_WX_MARK_HALF,
                    s_wx_mark_a + SP_WX_MARK_HALF);
    }
    const int alen = sp_len(s_wx_age, (int)sizeof s_wx_age - 1);
    const int tlen = sp_len(s_wx_temp, (int)sizeof s_wx_temp - 1);
    const int hlen = sp_len(s_wx_hilo, (int)sizeof s_wx_hilo - 1);
    const int ax = sp_text_cx(alen, 2, 0);
    const int tx = cx - (18 * tlen) / 2;
    const int hx = sp_text_cx(hlen, 2, 0);

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        for (int i = 0; i < ntrk; ++i) {
            sp_annulus_row(row, y, cx, cy, SP_FOCAL_IN, SP_FOCAL_OUT, &trk[i],
                           track);
        }
        for (int i = 0; i < nbnd; ++i) {
            sp_annulus_row(row, y, cx, cy, SP_FOCAL_IN, SP_FOCAL_OUT, &bnd[i],
                           band);
        }
        sp_annulus_row(row, y, cx, cy, SP_WX_RAIN_IN, SP_WX_RAIN_OUT, NULL,
                       track);
        for (int i = 0; i < nrn; ++i) {
            sp_annulus_row(row, y, cx, cy, SP_WX_RAIN_IN, SP_WX_RAIN_OUT,
                           &rn[i], rain);
        }
        if (s_wx_valid) {
            sp_annulus_row(row, y, cx, cy, SP_WX_MARK_IN, SP_WX_MARK_OUT, &mk,
                           mark);
        }
        sp_text_row(row, y, s_wx_age, alen, ax, SP_WX_AGE_Y + oy, 2, grey);
        sp_text_row(row, y, s_wx_temp, tlen, tx, SP_FOCAL_TEXT_Y + oy, 3, ink);
        sp_text_row(row, y, s_wx_hilo, hlen, hx, SP_WX_HILO_Y + oy, 2, ink);
    }
}

/* ACTIVITY: three rows, newest first, on a quiet disc. The disc is the safe
 * area dimmed a second time — the same treatment the detail sheet gives its
 * own band — so twenty-four glyphs of ink sit on a hush rather than on the
 * arc reactor. No ring, no petals: this screen is the receipt, and a receipt
 * is text. The kind is grey and the summary is ink, one scale, one line, so
 * the eye lands on what happened and only then on what did it. With nothing
 * pushed the one honest line is centred where the first row would be. */
static void sp_focal_activity(const jr_display_ctx_t *ctx, int y1, int y2,
                              uint16_t *pixels, int oy, int st)
{
    /* No second dim over the disc: the shell veil already quiets the face,
     * and a per-pixel fold over r<=168 cost ~88k pixels a frame — measured
     * 10 fps on this screen against 16-18 elsewhere. Text on the veil. */
    const uint16_t ink = sp_tint(ctx, SP_C_INK, st);
    const uint16_t grey = sp_tint(ctx, SP_C_GREY, st);

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        if (s_act_rows == 0) {
            static const char k_empty[] = "NOTHING YET";
            const int elen = (int)(sizeof k_empty - 1U);
            sp_text_row(row, y, k_empty, elen, sp_text_cx(elen, 2, 0),
                        SP_CY - 7 + oy, 2, grey);
            continue;
        }
        for (int i = 0; i < s_act_rows; ++i) {
            const int ry = SP_ACT_Y0 + i * SP_ACT_PITCH + oy;
            if (y < ry || y >= ry + 14) {
                continue;
            }
            const int klen = (int)s_act_row_klen[i];
            const int len = sp_len(s_act_row[i], SP_ACT_ROW_CAP - 1);
            const int sx = klen > 0 ? klen + 1 : 0;   /* past "KIND " */
            sp_text_row(row, y, s_act_row[i], klen, SP_ACT_X0, ry, 2, grey);
            if (len > sx) {
                sp_text_row(row, y, s_act_row[i] + sx, len - sx,
                            SP_ACT_X0 + 12 * sx, ry, 2, ink);
            }
        }
    }
}

/* ---- WATCH / POWER: screens that show only live data ---------------------
 *
 * Each is one focal object, procedural, strip-local, and reading a value that
 * something else already publishes. None of them invents content — the side
 * pages died of exactly that, and an empty screen telling the truth beats a
 * full one that lies. */

/* WATCH: a radial clock. The hour is a short fat span, the minute a long thin
 * one, so the two read apart at a glance without needing a dial or numerals —
 * the font is uppercase-only 5x7 and would lose to a bare arc here. */
/* WATCH draws NO focal object. The real watch face is hud_overlay_clock —
 * hour, minute and second hands with a hub, strip-tested, already shipping for
 * the ambient rest face and the watch peek. The WATCH screen asks for that
 * overlay (main.c) rather than drawing a second, worse clock underneath it.
 *
 * This function existed for one commit and drew a two-arc clock. It was a
 * home-made replacement for something better that was already in the tree —
 * kept here as a comment so nobody re-adds it. */

/* POWER: charge as a filled arc from 12 o'clock, plus a solid core while the
 * charger is in. Reads s_power_word, which jr_power already publishes. */
/* STATUS at a glance, the geometry:
 *
 *   y 116..129  LINK  TOOLS      two lamps, lit ink when up, dim when not;
 *                                 10 glyphs, corners r131.5 — under the
 *                                 update ring at r140 and above the focal
 *                                 ring's top at y137
 *   r 76..96    the battery arc  from the top, clockwise, by percent
 *   y 196..209  CHARGING         the power word, grey
 *   y 219..239  74%              scale 3, ink — the number the owner wants
 *   y 250..263  ▂▄▆█ -34         Wi-Fi bars and dBm, lit by the same tiers
 *                                 phones use
 *   y 330       UP 6H 10M        the headline (sp_headline)
 *
 * Everything inside the ring stays within the r76 chord for its row; the
 * lamps are the only text outside it, and they sit below the update ring's
 * inner edge so a flash in progress never overdraws them. */
#define SP_ST_LAMP_Y   116
#define SP_ST_WORD_Y   SP_WX_AGE_Y
#define SP_ST_WIFI_Y   SP_WX_HILO_Y
#define SP_ST_BAR_W    4
#define SP_ST_BAR_STEP 6            /* bar width plus a 2 px gap */
#define SP_ST_BARS_W   (3 * SP_ST_BAR_STEP + SP_ST_BAR_W)
#define SP_ST_BARS_GAP 6            /* between the bars and the number */

static void sp_focal_status(const jr_display_ctx_t *ctx, int y1, int y2,
                            uint16_t *pixels, int oy, int st)
{
    const uint32_t w = __atomic_load_n(&s_power_word, __ATOMIC_ACQUIRE);
    const uint32_t lk = __atomic_load_n(&s_links_word, __ATOMIC_ACQUIRE);
    const int raw_pct = (int)(w & 0xFFu);
    /* 0xFF means the fuel gauge did not answer. Clamping that to 100 drew a
     * FULL ring directly beneath the headline "NO BATTERY" — the most
     * confident possible rendering of a number we do not have. Draw the bare
     * track instead: an empty track reads as "no reading", which is true. */
    const bool have_gauge = raw_pct <= 100;
    const int pct = have_gauge ? raw_pct : 0;
    const bool charging = (w & (1u << 25)) != 0u;
    const bool muted = sp_privacy_muted();
    /* Red below HUD_BATT_LOW_PCT is the one place colour carries meaning on
     * the arc — the SAME red, on the SAME rule, as the battery rim at r215.
     * Gold means muted everywhere on this glass, so the arc and the bars
     * wear it too. */
    const bool low = have_gauge && pct < HUD_BATT_LOW_PCT && !charging;
    const uint16_t lamp = sp_tint(ctx, muted ? SP_C_GOLD : SP_C_CYAN, st);
    const uint16_t fill = low ? sp_tint(ctx, SP_C_RED, st) : lamp;
    const uint16_t cool = sp_tint(ctx, muted ? SP_C_GOLD_DIM : SP_C_CYAN_DIM,
                                  st);
    const uint16_t track = sp_tint(ctx, SP_C_TRACK, st);
    const uint16_t ink = sp_tint(ctx, SP_C_INK, st);
    const uint16_t grey = sp_tint(ctx, SP_C_GREY, st);
    const uint16_t dim = sp_tint(ctx, SP_C_GREY, (st * 96) / 255);
    const int cx = SP_CX;
    const int cy = SP_CY + oy;

    sp_span_t arc[SP_ARC_SEG_MAX];
    const int narc = have_gauge
        ? sp_arc_segments(arc, SP_ARC_SEG_MAX, SP_A_TOP, (pct * 256) / 100)
        : 0;

    /* The words, formatted once per strip from the two words the frame
     * already has: a few digits, no snprintf on this stack. */
    char pctbuf[6];
    const int plen = have_gauge ? sp_pct(pctbuf, 0, (int)sizeof pctbuf,
                                         (uint32_t)pct)
                                : sp_str(pctbuf, 0, (int)sizeof pctbuf, "--");
    const char *word = sp_power_word(w);
    const int wlen = sp_len(word, SP_LABEL_CAP - 1);
    const int bars = sp_wifi_bars(lk);
    char dbm[6];
    const int dlen = bars > 0
        ? sp_int(dbm, 0, (int)sizeof dbm, -(int)((lk >> 8) & 0xFFu))
        : sp_str(dbm, 0, (int)sizeof dbm, "DOWN");
    const int bx = cx - (SP_ST_BARS_W + SP_ST_BARS_GAP + 12 * dlen) / 2;
    const int lx = sp_text_cx(10, 2, 0);
    const bool link_on = (lk & (1u << 1)) != 0u;
    const bool tools_on = ((lk >> 2) & 3u) == 2u;

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        sp_annulus_row(row, y, cx, cy, SP_FOCAL_IN, SP_FOCAL_OUT, NULL, cool);
        for (int i = 0; i < narc; ++i) {
            sp_annulus_row(row, y, cx, cy, SP_FOCAL_IN, SP_FOCAL_OUT, &arc[i],
                           fill);
        }
        sp_text_row(row, y, "LINK", 4, lx, SP_ST_LAMP_Y + oy, 2,
                    link_on ? ink : dim);
        sp_text_row(row, y, "TOOLS", 5, lx + 60, SP_ST_LAMP_Y + oy, 2,
                    tools_on ? ink : dim);
        sp_text_row(row, y, word, wlen, sp_text_cx(wlen, 2, 0),
                    SP_ST_WORD_Y + oy, 2, grey);
        sp_text_row(row, y, pctbuf, plen, cx - (18 * plen) / 2,
                    SP_FOCAL_TEXT_Y + oy, 3, ink);
        /* Four bars, 4/7/10/13 px tall, bottom-aligned with the number's
         * baseline; unlit bars keep the track colour so the glyph reads as
         * a gauge and not as a hole. */
        const int by0 = SP_ST_WIFI_Y + oy;
        if (y >= by0 && y < by0 + 14) {
            for (int i = 0; i < 4; ++i) {
                const int h = 4 + 3 * i;
                if (y >= by0 + 14 - h) {
                    const uint16_t px = i < bars ? lamp : track;
                    const int x0 = bx + i * SP_ST_BAR_STEP;
                    for (int x = x0; x < x0 + SP_ST_BAR_W; ++x) {
                        row[x] = px;
                    }
                }
            }
        }
        sp_text_row(row, y, dbm, dlen, bx + SP_ST_BARS_W + SP_ST_BARS_GAP,
                    SP_ST_WIFI_Y + oy, 2, grey);
    }
}

/* The update ring's colour IS its meaning, so the palette is the contract:
 * cyan is progress, violet is on-trial, amber is "look at this", red is
 * broken. Nothing here is gold — gold is privacy's alone. */
static uint16_t sp_ota_native(jr_display_ota_state_t state)
{
    switch (state) {
    case JR_DISPLAY_OTA_PREFLIGHT:  return SP_C_CYAN_DIM;
    case JR_DISPLAY_OTA_RECEIVING:  return SP_C_CYAN;
    case JR_DISPLAY_OTA_PROBATION:  return SP_C_VIOLET;
    case JR_DISPLAY_OTA_FAILED:     return 0xF800;   /* red */
    case JR_DISPLAY_OTA_BLOCKED:
    case JR_DISPLAY_OTA_ROLLED_BACK:
    default:                        return SP_C_AMBER;
    }
}

/* THE FIRMWARE UPDATE RING, SHELL-WIDE. Concentric with every focal object
 * at r140-154: outside the focal band (r<=104) and the headline's worst-case
 * glyph corner (r131.5), well inside JR_DISPLAY_SAFE_R.
 *
 * It used to be drawn by the SETTINGS focal renderer, so the updater had to
 * navigate the glass to SETTINGS before an upload was visible — and a screen
 * whose only production visitor was the OTA path was a diagnostics page
 * wearing a face. The ring now belongs to no space. It is drawn on whatever
 * is up, AFTER the watch (whose disc clear would otherwise wipe it), and at
 * the focal layer's own weight, so it yields to an open sheet or shade exactly
 * as a focal object does and never meets the sheet's last row.
 *
 * On JARVIS at rest this is the ONE thing the shell may draw. The gate is the
 * OTA word itself, not s_space_on, so the face need not wake the veil, the
 * orbit or a headline to show an update; IDLE and VALID draw nothing at all,
 * which is what keeps every pre-shell scene bit-identical.
 *
 * Segmented because a span is a wedge intersection and a wedge cannot exceed
 * a half turn; the joints also read as mechanical travel rather than a pie. */
static void apply_ota_ring(const jr_display_ctx_t *ctx, int y1, int y2,
                           uint16_t *pixels)
{
    const uint32_t ota = __atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE);
    const jr_display_ota_state_t state = (jr_display_ota_state_t)(ota & 0xFu);
    if (!sp_ota_ringing(state)) {
        return;
    }
    const int under = 255 - (s_shade_ease * 255) / 256;
    const int st = (under * (256 - s_detail_ease)) / 256;
    if (st <= 0) {
        return;
    }
    const uint16_t track = sp_tint(ctx, SP_C_TRACK, st);
    const uint16_t ink = sp_tint(ctx, sp_ota_native(state), st);
    sp_span_t seg[SP_ARC_SEG_MAX];
    const int nseg = sp_arc_segments(
        seg, SP_ARC_SEG_MAX, SP_A_TOP,
        (sp_ota_fill(state, (int)((ota >> 8) & 0xFFu)) * 256) / 100);

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        sp_annulus_row(row, y, SP_CX, SP_CY, SP_OTA_IN, SP_OTA_OUT, NULL,
                       track);
        for (int i = 0; i < nseg; ++i) {
            sp_annulus_row(row, y, SP_CX, SP_CY, SP_OTA_IN, SP_OTA_OUT,
                           &seg[i], ink);
        }
    }
}

/* The short, useful label under the focal object. Callers can override it per
 * space; otherwise it is the space's own live fact, which beats any noun. */
static const char *sp_headline(int space)
{
    if (s_space_head[space][0] != '\0') {
        return s_space_head[space];
    }
    switch (space) {
    case JR_DISPLAY_SPACE_WATCH:
        /* NO HEADLINE. The hands are the readout; printing "11:29 PM" under
         * them stacks a second clock on the first, which is exactly the
         * clutter this screen is supposed to remove. An empty label draws
         * nothing (sp_draw_space returns on len <= 0).
         *
         * The one thing worth saying in words is when there is NO time to
         * show — hands frozen at 12:00 look like a stopped clock rather than
         * an unsynced one. */
        return s_clock_shown_word == 0u ? "NO TIME" : "";
    case JR_DISPLAY_SPACE_STATUS: {
        /* THE ONE THING THAT MATTERS MOST, or the quiet: an update in flight
         * outranks everything (it can brick the device), then a missing
         * gauge, a low cell, no Wi-Fi, no tools key — and when nothing is
         * wrong, how long the device has been up, which is the sentence a
         * healthy device gets to say. The percentage is no longer here: it
         * is the big number inside the ring. A closed session socket is
         * NOT an alarm any more — it closes at rest by design, so the old
         * "NO LINK" fired on every idle device. */
        static char buf[SP_LABEL_CAP];
        const uint32_t ota = __atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE);
        const jr_display_ota_state_t ota_state =
            (jr_display_ota_state_t)(ota & 0xFu);
        if (sp_ota_ringing(ota_state)) {
            if (ota_state == JR_DISPLAY_OTA_RECEIVING) {
                (void)sp_pct(buf, sp_str(buf, 0, SP_LABEL_CAP, "UPDATE "),
                             SP_LABEL_CAP, (ota >> 8) & 0xFFu);
                return buf;
            }
            return sp_ota_name(ota_state);
        }
        const uint32_t w = __atomic_load_n(&s_power_word, __ATOMIC_ACQUIRE);
        const uint32_t lk = __atomic_load_n(&s_links_word, __ATOMIC_ACQUIRE);
        const uint32_t pct = w & 0xFFu;
        const bool charging = (w & (1u << 25)) != 0u;
        if (pct > 100u) {
            return "NO BATTERY";
        }
        if (pct < HUD_BATT_LOW_PCT && !charging) {
            return "LOW BATTERY";
        }
        if ((lk & 1u) == 0u) {
            return "NO WIFI";
        }
        if (((lk >> 2) & 3u) == 0u) {
            return "NO TOOLS";
        }
        if (((lk >> 16) & 0xFFu) >= (uint32_t)(SP_CHIP_HOT_C + 40)) {
            return "RUNNING HOT";
        }
        (void)sp_uptime(buf, sp_str(buf, 0, SP_LABEL_CAP, "UP "), SP_LABEL_CAP,
                        diag_load(&s_jarvis_secs));
        return buf;
    }
    case JR_DISPLAY_SPACE_DESK:
        return s_desk_task[0] != '\0' ? s_desk_task : "NO TASK";
    case JR_DISPLAY_SPACE_WEATHER:
        /* "OVERCAST 83", or "NO WEATHER" — composed with the arcs, so the
         * word and the mark can never describe two different fetches. */
        return s_wx_head;
    case JR_DISPLAY_SPACE_ACTIVITY:
        /* NO HEADLINE, on the rule TOOLS taught: the caption already names
         * the place, and the rows ARE the data. Printing "ACTIVITY" here
         * would stack the screen's name over the caption saying it. */
        return "";
    default:
        return "";
    }
}

static void sp_draw_space(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int space, int oy, int st)
{
    /* JARVIS is the face itself: the shell contributes nothing, which is what
     * keeps every existing scene bit-identical at rest. */
    if (space == JR_DISPLAY_SPACE_JARVIS || st <= 0) {
        return;
    }
    switch (space) {
    case JR_DISPLAY_SPACE_WATCH:
        /* Nothing here on purpose: hud_overlay_clock draws the real watch face
         * over the top. Drawing a focal object as well would put a second
         * clock underneath the good one. */
        break;
    case JR_DISPLAY_SPACE_STATUS:
        sp_focal_status(ctx, y1, y2, pixels, oy, st);
        break;
    case JR_DISPLAY_SPACE_DESK:
        sp_focal_desk(ctx, y1, y2, pixels, oy, st);
        break;
    case JR_DISPLAY_SPACE_WEATHER:
        sp_focal_weather(ctx, y1, y2, pixels, oy, st);
        break;
    case JR_DISPLAY_SPACE_ACTIVITY:
        sp_focal_activity(ctx, y1, y2, pixels, oy, st);
        break;
    default:
        break;                /* a space with no focal object draws none */
    }
    const char *label = sp_headline(space);
    const int len = sp_len(label, SP_LABEL_CAP - 1);
    if (len <= 0) {
        return;
    }
    const uint16_t ink = sp_tint(ctx, SP_C_INK, st);
    const int tx = sp_text_cx(len, 2, 0);
    for (int y = y1; y < y2; ++y) {
        /* The label rides the same vertical offset as the focal object, so the
         * whole screen moves as one piece with the finger. */
        sp_text_row(pixels + (size_t)(y - y1) * HUD_W, y, label, len, tx,
                    SP_LABEL_Y + oy, 2, ink);
    }
}

/* The context sheet: label left, value right, one fact per row. It replaces
 * the focal object rather than crowding it, and it dims its own band a second
 * time so the rows read against whatever was behind them. */
static void sp_draw_detail(const jr_display_ctx_t *ctx, int y1, int y2,
                           uint16_t *pixels, int st)
{
    if (st <= 0) {
        return;
    }
    const int ys = y1 > SP_SHEET_Y0 ? y1 : SP_SHEET_Y0;
    const int ye = (y2 - 1) < SP_SHEET_Y1 ? (y2 - 1) : SP_SHEET_Y1;
    if (ys > ye) {
        return;
    }
    const bool swap = ctx->board.swap_color_bytes;
    const int k = (st * 32) / 255;
    const uint16_t head = sp_tint(ctx, SP_C_CYAN, st);
    const uint16_t key = sp_tint(ctx, SP_C_GREY, st);
    const uint16_t val = sp_tint(ctx, SP_C_INK, st);
    const int hlen = sp_len(s_detail_head, SP_LABEL_CAP - 1);

    for (int y = ys; y <= ye; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (sp_clip(y, &lo, &hi)) {
            if (k >= 32) {
                for (int x = lo; x <= hi; ++x) {
                    row[x] = hud_dim565(row[x], swap);
                }
            } else if (k > 0) {
                for (int x = lo; x <= hi; ++x) {
                    row[x] = hud_fade565(row[x], swap, k);
                }
            }
        }
        sp_text_row(row, y, s_detail_head, hlen, sp_text_cx(hlen, 2, 0),
                    SP_SHEET_HEAD_Y, 2, head);
        for (int i = 0; i < s_detail_rows; ++i) {
            const int ry = SP_SHEET_ROW_Y + i * SP_SHEET_ROW_DY;
            if (y < ry || y >= ry + 14) {
                continue;
            }
            const int klen = sp_len(s_detail_label[i], SP_COL_MAX - 1);
            const int vlen = sp_len(s_detail_value[i], SP_VAL_MAX - 1);
            sp_text_row(row, y, s_detail_label[i], klen, SP_SHEET_LEFT, ry, 2,
                        key);
            sp_text_row(row, y, s_detail_value[i], vlen,
                        SP_SHEET_RIGHT - 12 * vlen, ry, 2, val);
        }
    }
}


/* The minimal voice shade: volume plus an explicit MUTE/LISTEN action. */
static void sp_draw_shade(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int st)
{
    if (st <= 0) {
        return;
    }
    const uint32_t w = __atomic_load_n(&s_status_word, __ATOMIC_ACQUIRE);
    int vol = (int)(w & 0xFFu);
    if (vol > 100) {
        vol = 100;
    }
    const bool muted = sp_privacy_muted();
    const uint16_t track = sp_tint(ctx, SP_C_TRACK, st);
    const uint16_t cyan = sp_tint(ctx, SP_C_CYAN, st);
    const uint16_t gold = sp_tint(ctx, SP_C_GOLD, st);
    const uint16_t amber = sp_tint(ctx, SP_C_AMBER, st);
    const uint16_t grey = sp_tint(ctx, SP_C_GREY, st);
    const uint16_t plate = sp_tint(ctx, SP_C_PLATE, st);
    const uint16_t priv = muted ? gold : cyan;

    const int vspan = (vol * SP_ARC_SPAN) / 100;
    sp_span_t track_span, vfill;
    sp_span_set(&track_span, SP_ARC_A0, SP_ARC_A1);
    if (vspan > 0) {
        sp_span_set(&vfill, SP_ARC_A0, SP_ARC_A0 + vspan);
    }

    const int vcap_y = SP_CY + ((sp_sin(SP_ARC_A0) * SP_VOL_CAP_R) >> 15) - 7;
    const int vcap_lx = SP_CX + ((sp_cos(SP_ARC_A0) * SP_VOL_CAP_R) >> 15) - 6;
    const int vcap_rx = SP_CX + ((sp_cos(SP_ARC_A1) * SP_VOL_CAP_R) >> 15) - 6;

    static const char k_hint[] = "PWR LISTEN  BOOT CLOSE";
    const int hintlen = (int)(sizeof k_hint - 1U);
    const char *plabel = muted ? "LISTEN" : "MUTE";
    const int plen = muted ? 6 : 4;
    const int vlen = sp_len(s_shade_vol, SP_COL_MAX - 1);
    const int llen = sp_len(s_shade_light, SP_COL_MAX - 1);


    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        sp_annulus_row(row, y, SP_CX, SP_CY, SP_VOL_IN, SP_VOL_OUT,
                       &track_span, track);
        if (vspan > 0) {
            sp_annulus_row(row, y, SP_CX, SP_CY, SP_VOL_IN, SP_VOL_OUT,
                           &vfill, cyan);
        }


        sp_text_row(row, y, "-", 1, vcap_lx, vcap_y, 2, cyan);
        sp_text_row(row, y, "+", 1, vcap_rx, vcap_y, 2, cyan);

        sp_text_row(row, y, s_shade_vol, vlen, sp_text_cx(vlen, 2, 0),
                    SP_VOL_TEXT_Y, 2, cyan);
        sp_text_row(row, y, s_shade_light, llen, sp_text_cx(llen, 2, 0),
                    SP_LIGHT_TEXT_Y, 2, amber);
        sp_text_row(row, y, k_hint, hintlen, sp_text_cx(hintlen, 2, 0),
                    SP_HINT_Y, 2, grey);

        sp_annulus_row(row, y, SP_BTN_PRIV_CX, SP_BTN_CY, 0, SP_BTN_R - 4,
                       NULL, plate);
        sp_annulus_row(row, y, SP_BTN_PRIV_CX, SP_BTN_CY, SP_BTN_R - 3,
                       SP_BTN_R, NULL, priv);
        if (muted) {
            const int dy = y - SP_BTN_CY;
            int lo = SP_BTN_PRIV_CX - SP_BTN_R, hi = SP_BTN_PRIV_CX + SP_BTN_R;
            if (sp_clip(y, &lo, &hi)) {
                for (int x = lo; x <= hi; ++x) {
                    const int dx = x - SP_BTN_PRIV_CX;
                    const int d = dx - dy;
                    if (dx * dx + dy * dy <= (SP_BTN_R - 6) * (SP_BTN_R - 6) &&
                        d <= 2 && d >= -2) {
                        row[x] = gold;
                    }
                }
            }
        }
        sp_text_row(row, y, plabel, plen,
                    sp_text_cx(plen, 2, SP_BTN_PRIV_CX - SP_CX),
                    SP_BTN_CY - 7, 2, priv);
    }
}

/* The orbital page indicator: one mark per screen, spaced evenly around the
 * WHOLE dial, with the lit mark travelling on the SAME eased progress the
 * content slides on — so the indicator IS the transition, not a caption on it.
 * Gold when muted: privacy outranks position.
 *
 * It used to be N marks on a short ±30 arc at 12 o'clock, positioned by
 * (2*i - 3) * 7. That was tuned for exactly FOUR spaces and broke twice when
 * the ring grew to seven: the outer marks landed at ±63, outside the ±30 rail
 * they were supposed to sit on, and a wrap from the last screen to the first
 * swept the lit mark backwards across the entire arc.
 *
 * A full circle fixes both by construction. Evenly spaced marks always fit
 * however many screens exist, and the active mark takes the SHORT way round —
 * so a wrap advances one step forward like every other move, instead of
 * rewinding past everything. Position on a ring should be drawn as a ring. */
static void sp_draw_orbit(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int st)
{
    const bool muted = sp_privacy_muted();
    const uint16_t rail = sp_tint(ctx, SP_C_PLATE, st);
    const uint16_t cool = sp_tint(ctx, muted ? SP_C_GOLD_DIM : SP_C_CYAN_DIM, st);
    const uint16_t hot = sp_tint(ctx, muted ? SP_C_GOLD : SP_C_CYAN, st);

    /* One mark per screen the ring shows RIGHT NOW — DESK only while live —
     * and the active mark at the angle sp_fade_tick eased for this frame,
     * which already carries the slide and the re-spacing residue. */
    const int n = sp_ring_n(sp_desk_live());
    const int a_act = (s_orbit_a16 + 8) >> 4;

    sp_span_t act;
    sp_span_set(&act, a_act - 5, a_act + 5);

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        /* The rail is now the whole ring — NULL span means all the way round. */
        sp_annulus_row(row, y, SP_CX, SP_CY, SP_ORB_TRACK_IN, SP_ORB_TRACK_OUT,
                       NULL, rail);
        for (int i = 0; i < n; ++i) {
            const int a = SP_A_TOP + (i * 256) / n;
            sp_dot_row(row, y, SP_CX + ((sp_cos(a) * SP_ORB_R) >> 15),
                       SP_CY + ((sp_sin(a) * SP_ORB_R) >> 15), 5, cool);
        }
        sp_annulus_row(row, y, SP_CX, SP_CY, SP_ORB_MARK_IN, SP_ORB_MARK_OUT,
                       &act, hot);
    }
}

/* The whole shell for one strip, in z-order: veil, the outgoing and incoming
 * spaces cross-dissolving as they slide, context sheet, shade, then an orbital
 * position marker that fades out with the side surfaces under the shade. */
static void apply_space_overlay(const jr_display_ctx_t *ctx, int y1, int y2,
                                uint16_t *pixels)
{
    if (!s_space_on) {
        return;            /* JARVIS at rest: the layer costs one compare */
    }
    const int veil = (s_space_veil * 32) >> 8;
    if (veil > 0) {
        sp_veil(ctx, y1, y2, pixels, veil);
    }
    const int shade = (s_shade_ease * 255) / 256;
    const int under = 255 - shade;
    if (under > 0) {
        const int e = s_space_ease;
        const int focal = (under * (256 - s_detail_ease)) / 256;
        if (focal > 0) {
            /* VERTICAL slide, matching the gesture that drives it. The ring
             * is walked by swiping up and down, so content that moved
             * left/right contradicted the finger — it read as the screen
             * sliding sideways while you pushed it down.
             *
             * Sign: going FORWARD (swipe down) the outgoing screen leaves
             * upward and the incoming one arrives from below, so the stack
             * appears to move with the finger rather than against it. */
            const int slide = (SP_SLIDE_PX * e) / 256;
            if (e < 256) {
                sp_draw_space(ctx, y1, y2, pixels, s_space_from,
                              -s_space_dir * slide,
                              (focal * (256 - e)) / 256);
            }
            sp_draw_space(ctx, y1, y2, pixels, s_space_to,
                          s_space_dir * (SP_SLIDE_PX - slide),
                          (focal * e) / 256);
        }
        if (s_detail_ease > 0) {
            sp_draw_detail(ctx, y1, y2, pixels,
                           (under * s_detail_ease) / 256);
        }
    }
    if (shade > 0) {
        sp_draw_shade(ctx, y1, y2, pixels, shade);
    }
    sp_draw_orbit(ctx, y1, y2, pixels, under);
}

static void apply_hud_overlay(jr_display_ctx_t *ctx, int x1, int y1,
                              int x2, int y2, uint16_t *pixels)
{
    if (pixels == NULL || x1 != 0 || (x2 - x1) != HUD_W) {
        return;
    }
    if (__atomic_load_n(&s_hud_enabled, __ATOMIC_RELAXED) == 0u) {
        return;
    }
    const int nrows = y2 - y1;
    if (nrows <= 0) {
        return;
    }
    const uint32_t w = __atomic_load_n(&s_hud_env_word, __ATOMIC_RELAXED);
    const hud_env_t env = {
        .face     = hud_face_of((jr_face_t)diag_load(&ctx->applied_face)),
        .amp      = (uint8_t)diag_load(&ctx->requested_amplitude),
        .batt_pct = (uint8_t)(w & 0xFFu),
        .charging = (w & (1u << 8)) != 0u,
        .privacy_muted = (w & (1u << 9)) != 0u,
        .ox       = (int8_t)((w >> 16) & 0xFFu),
        .oy       = (int8_t)((w >> 24) & 0xFFu),
    };
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    hud_overlay_frame(pixels, y1, nrows, now_ms,
                      ctx->board.swap_color_bytes, &env);

    /* TRANS-05 tap ripple: the EARLIEST overlay — it is feedback, not
     * chrome, so the ask presentation and the caption both draw over it.
     * Expiry is the age gate alone; only the app task writes the slot. */
    const uint32_t rip_start =
        __atomic_load_n(&s_ripple_start_ms, __ATOMIC_ACQUIRE);
    if (rip_start != 0u && now_ms - rip_start < HUD_RIPPLE_MS) {
        hud_overlay_ripple(pixels, y1, nrows, ctx->board.swap_color_bytes,
                           s_ripple_x, s_ripple_y, now_ms - rip_start,
                           (hud_ripple_kind_t)s_ripple_kind);
    }

    /* Modal ask (STATE-05/06/07): dim everything drawn so far, lay the
     * question + labels over the dimmed face, then the arcs LAST — they are
     * the interactive layer and must sit bright above the face, the HUD
     * accents, and the text. Same strip contract, same buffer. The caption
     * (STATE-04) yields entirely while an ask is up: the ask owns the glass.
     * On dismissal the presentation SINKS over whatever returns underneath —
     * the tap's selected arc stays lit through the exit, which is the demo
     * script's "the arc fills and confirms" beat for free. */
    const int cn = __atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE);
    const int ae = s_ask_ease;
    const int as = ae > 255 ? 255 : ae;
    if (cn > 0) {
        apply_ask_overlay(ctx, y1, y2, pixels, cn, ae);
        hud_overlay_choices(pixels, y1, nrows, ctx->board.swap_color_bytes,
                            s_choices, cn, s_choice_selected, as);
    } else {
        /* The spatial shell owns the glass between the face and the caption:
         * the four spaces, the context sheet, and the control shade. It is
         * skipped entirely while an ask is up (the ask owns the glass) and
         * draws nothing at all in JARVIS at rest. */
        apply_space_overlay(ctx, y1, y2, pixels);

        /* UI-01 clock under the STATE-04 caption. The watch is JARVIS's
         * ambient face, so it yields whenever the shell is presenting ANOTHER
         * screen rather than fighting it for the centre — but it must NOT
         * yield on the WATCH screen itself, which exists to show it.
         *
         * That exception is the whole bug behind "I don't see the clock at all
         * anymore": the ring gained a WATCH screen, and `!s_space_on`
         * suppressed the clock on precisely the one space that wanted it, so
         * the screen rendered the bare baked face with the time reduced to a
         * caption. */
        if (!s_space_on ||
            (jr_display_space_t)(__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE)
                                 & NAV_SPACE_MASK) == JR_DISPLAY_SPACE_WATCH) {
            apply_clock_overlay(ctx, y1, y2, pixels);  /* gates itself */
        }
        /* The firmware update ring sits ABOVE the watch: the clock keeps its
         * dial clean by clearing the disc, and the one job that can brick the
         * device must not be the thing that clear erases. Gated on the OTA
         * word alone, so it shows on JARVIS at rest without waking the shell. */
        apply_ota_ring(ctx, y1, y2, pixels);
        {
            /* The commit ring shares the choice band and is drawn only on the
             * no-ask path, which is what makes the two mutually exclusive. */
            const uint8_t cpct = __atomic_load_n(&s_commit_pct, __ATOMIC_ACQUIRE);
            if (cpct > 0U) {
                hud_overlay_commit(pixels, y1, nrows,
                                   ctx->board.swap_color_bytes, cpct, 255);
            }
        }
        apply_caption_overlay(ctx, y1, y2, pixels);    /* gates on its ease */
        if (ae > 0 && s_ask_shown_n > 0) {
            apply_ask_overlay(ctx, y1, y2, pixels, s_ask_shown_n, ae);
            hud_overlay_choices(pixels, y1, nrows,
                                ctx->board.swap_color_bytes, s_choices,
                                s_ask_shown_n, s_choice_selected, as);
        }
    }

    /* TRANS-01 wake bloom: TOPMOST transient. The wake moment must read over
     * whatever the glass held — most importantly a watch mid fade-out, which
     * is the exact state a "Jarvis" from rest fires it in. It erases itself
     * in HUD_BLOOM_MS, the same license the ripple holds. */
    const uint32_t bloom_start =
        __atomic_load_n(&s_bloom_start_ms, __ATOMIC_ACQUIRE);
    if (bloom_start != 0u && now_ms - bloom_start < HUD_BLOOM_MS) {
        hud_overlay_bloom(pixels, y1, nrows, ctx->board.swap_color_bytes,
                          now_ms - bloom_start);
    }
}

/* Operator / agent identity: the outer rim, and nothing else.
 *
 * This function used to draw the control shade as well — a rectangular top
 * drawer with a pull handle, three quick-action pucks and a progress bar,
 * plus a seven-line gesture legend beneath it. On a round panel the bezel ate
 * the corners of all of it, and the legend documented a gesture map the
 * spatial shell replaces. Both moved into the shell, which draws one control
 * shade in round-native geometry inside JR_DISPLAY_SHELL_R_MAX.
 *
 * What stays here is the part that genuinely belongs to the operator rather
 * than to navigation: eight rim segments at r224-230 in the agent's colour.
 * That band is OUTSIDE everything the shell can reach and clear of the gold
 * privacy ring at r221-222, so Agent Link can never be confused with either
 * the mic state or the space you are in. */
static void apply_shell_overlay(jr_display_ctx_t *ctx, int x1, int y1,
                                int x2, int y2, uint16_t *pixels)
{
    const uint32_t shell = diag_load(&ctx->shell_word);
    if ((shell & JR_DISPLAY_SHELL_AGENT) == 0U || pixels == NULL) {
        return;
    }
    /* THE INTERACTIVE BAND OUTRANKS THE AGENT RIM.
     *
     * r223-231 is the choice/commit band, and it is exclusive by design. The
     * flush order is apply_hud_overlay (choice arcs, commit ring) and THEN
     * this function (agent segments at r224-230), with nothing stopping the
     * second from painting over the first. A tool running while an ask is open
     * therefore buried the very arcs the device was waiting for the owner to
     * touch — the question stayed on the glass while its answers were
     * overwritten by progress from an unrelated job.
     *
     * Ask state wins because it is the only one of the two the owner can act
     * on. The agent rim resumes the moment the ask, its fade, and any commit
     * preview are done; the agent's progress is still on DESK meanwhile. */
    if (s_choice_n > 0 || s_ask_ease > 0 || s_commit_pct > 0U) {
        return;
    }
    const uint8_t progress = shell & JR_DISPLAY_SHELL_PROGRESS;
    const int completed = progress == 0U ? 0 : ((int)progress * 8 + 99) / 100;
    if (completed <= 0) {
        return;
    }
    const jr_display_agent_state_t agent_state = (jr_display_agent_state_t)
        ((shell & JR_DISPLAY_SHELL_STATE_MASK) >>
         JR_DISPLAY_SHELL_STATE_SHIFT);
    /* Written straight in panel order: the old path round-tripped every pixel
     * of every strip through panel_native() to composite a shade that is no
     * longer drawn here. */
    const uint16_t accent = panel_order_color(ctx,
                                              agent_native_color(agent_state));
    const int width = x2 - x1;
    const int rim_out = 2 * 230, rim_in = 2 * 224;

    for (int y = y1; y < y2; ++y) {
        const int dy = 2 * y - ((int)ctx->board.height - 1);
        if (dy * dy > rim_out * rim_out) {
            continue;                  /* strip row misses the rim entirely */
        }
        uint16_t *row = pixels + (size_t)(y - y1) * (size_t)width;
        for (int col = 0; col < width; ++col) {
            const int dx = 2 * (x1 + col) - ((int)ctx->board.width - 1);
            const int r2 = dx * dx + dy * dy;
            if (r2 < rim_in * rim_in || r2 > rim_out * rim_out) {
                continue;
            }
            if (challenge_sector_from_vector(dx, dy) < completed) {
                row[col] = accent;
            }
        }
    }
}

static void snapshot_record_flush(jr_display_ctx_t *ctx, int x1, int y1,
                                  int x2, int y2, const uint16_t *pixels)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t consumer_ms = diag_load(&ctx->snapshot_consumer_ms);
    if (__atomic_load_n(&ctx->snapshot_initialized, __ATOMIC_ACQUIRE) == 0U ||
        pixels == NULL ||
        (uint32_t)(now_ms - consumer_ms) > JR_DISPLAY_SNAPSHOT_ACTIVE_MS ||
        x1 < 0 || y1 < 0 || x2 > (int)ctx->board.width ||
        y2 > (int)ctx->board.height || x2 <= x1 || y2 <= y1) {
        return;
    }
    if (xSemaphoreTake(ctx->snapshot_lock, 0) != pdTRUE) {
        /* Never publish the bottom strip of a frame whose middle was dropped
         * while an HTTP copy held the mutex. The next full refresh starts a
         * clean candidate at row zero. */
        diag_store(&ctx->snapshot_frame_dropped, 1U);
        return;
    }
    uint16_t *mirror = ctx->snapshot_capture_rgb565;
    if (x1 == 0 && x2 == (int)ctx->board.width && y1 == 0) {
        diag_store(&ctx->snapshot_frame_dropped, 0U);
        ctx->snapshot_next_y = 0U;
    }
    int width = x2 - x1;
    int height = y2 - y1;
    bool coherent = diag_load(&ctx->snapshot_frame_dropped) == 0U &&
        x1 == 0 && x2 == (int)ctx->board.width &&
        y1 == (int)ctx->snapshot_next_y;
    if (!coherent) {
        diag_store(&ctx->snapshot_frame_dropped, 1U);
    }
    for (int row = 0; row < height; ++row) {
        uint16_t *dst = mirror +
            (size_t)(y1 + row) * (size_t)ctx->board.width + (size_t)x1;
        const uint16_t *src = pixels + (size_t)row * (size_t)width;
        memcpy(dst, src, (size_t)width * sizeof(uint16_t));
    }
    ctx->snapshot_last_flush_ms = now_ms;
    if (coherent) {
        ctx->snapshot_next_y = (uint16_t)y2;
    }
    if (coherent && y2 == (int)ctx->board.height &&
        ctx->snapshot_next_y == ctx->board.height) {
        /* Publish by pointer swap. HTTP can now copy this generation while
         * the renderer builds the next one in the other buffer. The mutex
         * prevents a later swap during that copy. */
        uint16_t *previous = ctx->snapshot_rgb565;
        ctx->snapshot_rgb565 = mirror;
        ctx->snapshot_capture_rgb565 = previous;
        ctx->snapshot_frame_id++;
        ctx->snapshot_valid = true;
    }
    xSemaphoreGive(ctx->snapshot_lock);
}

static bool IRAM_ATTR panel_color_done(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t *event,
                                       void *user_ctx)
{
    (void)panel_io;
    (void)event;
    jr_display_ctx_t *ctx = (jr_display_ctx_t *)user_ctx;
    if (ctx && ctx->disp) {
        diag_inc(&ctx->flush_completions);
        (void)gfx_disp_flush_ready(ctx->disp, true);
    }
    /* gfx_disp_flush_ready() performs the ISR yield itself. */
    return false;
}

static void panel_flush(gfx_disp_t *disp, int x1, int y1, int x2, int y2,
                        const void *pixels)
{
    jr_display_ctx_t *ctx = (jr_display_ctx_t *)gfx_disp_get_user_data(disp);
    /* Boot-race defense: gfx_disp_add marks the screen dirty and the prio-4
     * render task can reach this flush BEFORE display_engine_init's
     * 'ctx->disp = gfx_disp_add(...)' store completes. The ISR completion
     * handler needs ctx->disp to release the flush wait — publish it from
     * the flush itself (this call strictly precedes its own completion ISR),
     * or the render task deadlocks on its very first frame. */
    if (ctx != NULL && ctx->disp == NULL) {
        ctx->disp = disp;
    }
    if (!ctx || !ctx->board.panel || !pixels) {
        if (ctx) {
            diag_inc(&ctx->flush_errors);
        }
        (void)gfx_disp_flush_ready(disp, true);
        return;
    }

    /* Apply any pending brightness HERE, before the strip is submitted: we are
     * on the render task, the previous flush has completed, and the QSPI bus is
     * idle. This is the only place a panel command may be issued. */
    brightness_pump();

    /* Deep-sleep hand-off, on the same rule: the bus is idle here, so the
     * panel takes DISPOFF and SLPIN safely, and no strip is submitted after
     * them. An unsupported SLPIN is fine — DISPOFF already blanks it. */
    const uint32_t off = diag_load(&s_panel_off);
    if (off == 1U) {
        (void)jarvis_board_set_brightness(0);
        (void)esp_lcd_panel_disp_on_off(ctx->board.panel, false);
        (void)esp_lcd_panel_disp_sleep(ctx->board.panel, true);
        diag_store(&s_panel_off, 2U);
    }
    if (off != 0U) {
        (void)gfx_disp_flush_ready(disp, true);
        return;
    }

    /* Frame-start latch: fades advance once per frame so all of a frame's
     * strips composite at one level (no banding across strip seams). */
    if (x1 == 0 && y1 == 0) {
        overlay_fade_tick();
    }

    /* The gfx-owned DMA buffer is mutable until this submission. Diagnostics
     * may substitute a known pattern, then the mirror records the exact bytes
     * that will be handed to the CO5300—not the pre-render intent. */
    uint16_t *outbound = (uint16_t *)pixels;
    apply_canvas(ctx, x1, y1, x2, y2, outbound);
    apply_test_pattern(ctx, x1, y1, x2, y2, outbound);
    if (diag_load(&ctx->test_pattern) == JR_DISPLAY_TEST_OFF) {
        apply_hud_overlay(ctx, x1, y1, x2, y2, outbound);
        apply_shell_overlay(ctx, x1, y1, x2, y2, outbound);
        apply_surface_overlay(ctx, x1, y1, x2, y2, outbound);
    }
    diag_inc(&ctx->flush_submissions);
    esp_err_t err = esp_lcd_panel_draw_bitmap(ctx->board.panel, x1, y1, x2, y2,
                                              outbound);
    if (err != ESP_OK) {
        /* This strip never reached the panel, so the in-progress software
         * mirror must not be allowed to publish as physical evidence. */
        diag_store(&ctx->snapshot_frame_dropped, 1U);
        __atomic_store_n(&ctx->last_error, err, __ATOMIC_RELAXED);
        diag_inc(&ctx->flush_errors);
        /* Never strand the gfx renderer on a failed DMA submission. */
        (void)gfx_disp_flush_ready(disp, true);
        return;
    }
    snapshot_record_flush(ctx, x1, y1, x2, y2, outbound);
}

static esp_err_t apply_blank(jr_display_ctx_t *ctx)
{
    if (!ctx->gfx || !ctx->anim || !ctx->disp) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(gfx_emote_lock(ctx->gfx), TAG, "gfx lock failed");
    (void)gfx_anim_stop(ctx->anim);
    (void)gfx_obj_set_visible(ctx->anim, false);
    gfx_disp_refresh_all(ctx->disp);
    (void)gfx_emote_unlock(ctx->gfx);
    /* Reset the segment latch: a later present() of the SAME face must not
     * early-return in program_segment with the anim still stopped+hidden —
     * that left the panel permanently black on resume-from-blank. */
    ctx->last_end = -1;
    __atomic_store_n(&ctx->applied_bucket, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->blanked, true, __ATOMIC_RELAXED);
    return ESP_OK;
}

static uint32_t segment_end(const jr_display_clip_t *clip, jr_face_t face,
                            uint8_t bucket)
{
    if (face != JR_FACE_LISTENING && face != JR_FACE_SPEAKING) {
        return clip->last_frame;
    }

    uint32_t end = 1U + ((uint32_t)bucket * (clip->last_frame - 1U)) /
                         JR_DISPLAY_AMP_BUCKETS;
    uint32_t quiet_floor = clip->last_frame / 3U;
    if (quiet_floor < 2U) {
        quiet_floor = 2U;
    }
    if (end < quiet_floor) {
        end = quiet_floor;
    }
    return end > clip->last_frame ? clip->last_frame : end;
}

static esp_err_t program_segment(jr_display_ctx_t *ctx, jr_face_t face,
                                 uint8_t bucket, bool force)
{
    if (ctx->active == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t end = segment_end(ctx->active, face, bucket);
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);

    if (!force && (face == JR_FACE_LISTENING || face == JR_FACE_SPEAKING) &&
            ctx->last_end > 0 && end < (uint32_t)ctx->last_end) {
        if (now_ms - ctx->decay_gate_ms < JR_DISPLAY_DECAY_HOLD_MS) {
            end = (uint32_t)ctx->last_end;
        } else {
            ctx->decay_gate_ms = now_ms;
        }
    } else if (ctx->last_end < 0 || end > (uint32_t)ctx->last_end) {
        ctx->decay_gate_ms = now_ms;
    }

    if (!force && (int)end == ctx->last_end) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gfx_emote_lock(ctx->gfx), TAG, "gfx lock failed");
    esp_err_t err = gfx_anim_set_segment(ctx->anim, 0, end, face_fps(face), true);
    if (err == ESP_OK) {
        err = gfx_anim_start(ctx->anim);
    }
    if (err == ESP_OK) {
        err = gfx_obj_set_visible(ctx->anim, true);
    }
    (void)gfx_emote_unlock(ctx->gfx);
    if (err == ESP_OK) {
        ctx->last_end = (int)end;
        __atomic_store_n(&ctx->applied_bucket, bucket, __ATOMIC_RELAXED);
        diag_inc(&ctx->segment_sets);
    }
    return err;
}

static esp_err_t apply_face(jr_display_ctx_t *ctx, jr_face_t face,
                            uint8_t amplitude, bool *stale)
{
    if (stale != NULL) {
        *stale = false;
    }
    uint8_t bucket = (uint8_t)(((uint32_t)amplitude * JR_DISPLAY_AMP_BUCKETS + 127U) /
                               255U);
    if (bucket > JR_DISPLAY_AMP_BUCKETS) {
        bucket = JR_DISPLAY_AMP_BUCKETS;
    }

    if (ctx->active != NULL && ctx->loaded_face == face &&
            !__atomic_load_n(&ctx->blanked, __ATOMIC_RELAXED)) {
        return program_segment(ctx, face, bucket, false);
    }

    if (face >= JR_FACE_COUNT) {        /* defensive: cb clamps already */
        face = JR_FACE_ERROR;
    }
    const uint32_t requested_before =
        __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
    const jr_face_t latest_before = (jr_face_t)(
        (requested_before & JR_DISPLAY_CMD_FACE_MASK) >>
        JR_DISPLAY_CMD_FACE_SHIFT);
    if ((requested_before & JR_DISPLAY_CMD_BLANK) != 0U ||
        latest_before != face) {
        if (stale != NULL) {
            *stale = true;
        }
        return ESP_OK;
    }
    jr_display_clip_t *clip = &ctx->clips[face];
    if (clip->data == NULL) {
        esp_err_t load_err = clip_load(face, clip);
        if (load_err != ESP_OK) {
            diag_inc(&ctx->asset_load_failures);
            return load_err;
        }
    }
    /* Asset reads can take longer than a very short Gemini turn. Do not show
     * the completed turn's stale face after the request already returned to
     * Listening; leave the current face running and consume the latest word
     * on the next presenter pass. */
    const uint32_t requested =
        __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
    const jr_face_t latest = (jr_face_t)(
        (requested & JR_DISPLAY_CMD_FACE_MASK) >> JR_DISPLAY_CMD_FACE_SHIFT);
    if ((requested & JR_DISPLAY_CMD_BLANK) != 0U || latest != face) {
        if (stale != NULL) {
            *stale = true;
        }
        return ESP_OK;
    }

    esp_err_t err = gfx_emote_lock(ctx->gfx);
    if (err != ESP_OK) {
        return err;                     /* clip stays cached for the retry */
    }
    err = gfx_anim_set_src(ctx->anim, clip->data, clip->size);
    (void)gfx_emote_unlock(ctx->gfx);
    if (err != ESP_OK) {
        /* A rejected replacement must not leave the last valid face stopped.
         * The engine still references the previously bound cache entry —
         * nothing was freed — so just restart it. */
        if (ctx->active != NULL) {
            (void)gfx_emote_lock(ctx->gfx);
            (void)gfx_anim_start(ctx->anim);
            (void)gfx_obj_set_visible(ctx->anim, true);
            (void)gfx_emote_unlock(ctx->gfx);
        }
        return err;
    }

    ctx->active = clip;
    ctx->loaded_face = face;
    ctx->last_end = -1;
    __atomic_store_n(&ctx->applied_bucket, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->blanked, false, __ATOMIC_RELAXED);
    diag_store(&ctx->current_asset_bytes, (uint32_t)clip->size);

    err = program_segment(ctx, face, bucket, true);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "face=%d asset=%u bytes frames=%u bucket=%u",
                 (int)face, (unsigned)clip->size,
                 (unsigned)(clip->total_frames - 1U), (unsigned)bucket);
    }
    return err;
}

static esp_err_t display_engine_init(jr_display_ctx_t *ctx)
{
    ESP_RETURN_ON_ERROR(jarvis_board_display_get(&ctx->board), TAG,
                        "CO5300 board display init failed");

    const esp_vfs_spiffs_conf_t fs = {
        .base_path = JR_DISPLAY_MOUNT_POINT,
        .partition_label = JR_DISPLAY_PARTITION,
        .max_files = 2,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&fs);
    ESP_RETURN_ON_ERROR(err, TAG, "mount '%s' failed", JR_DISPLAY_PARTITION);

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(JR_DISPLAY_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "assets mounted: %u/%u bytes", (unsigned)used, (unsigned)total);
    }

    const gfx_core_config_t gfx_cfg = {
        .fps = JR_DISPLAY_RENDER_FPS,
        .task = {
            .task_priority = JR_DISPLAY_RENDER_PRIORITY,
            .task_stack = JR_DISPLAY_RENDER_STACK,
            .task_affinity = JR_DISPLAY_RENDER_CORE,
            .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        },
    };
    ctx->gfx = gfx_emote_init(&gfx_cfg);
    ESP_RETURN_ON_FALSE(ctx->gfx, ESP_FAIL, TAG, "gfx init failed");

    const esp_lcd_panel_io_callbacks_t panel_callbacks = {
        .on_color_trans_done = panel_color_done,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_register_event_callbacks(ctx->board.io, &panel_callbacks, ctx),
        TAG, "panel flush callback registration failed");

    const gfx_disp_config_t disp_cfg = {
        .h_res = ctx->board.width,
        .v_res = ctx->board.height,
        .flush_cb = panel_flush,
        .update_cb = NULL,
        .user_data = ctx,
        .flags = {
            .swap = ctx->board.swap_color_bytes,
            .buff_dma = true,
            .buff_spiram = false,   /* internal SRAM: spi_master DMAs directly */
            .double_buffer = true,
        },
        .buffers = {
            .buf1 = NULL,
            .buf2 = NULL,
            .buf_pixels = (size_t)ctx->board.width * JR_DISPLAY_STRIP_ROWS,
        },
    };
    ctx->disp = gfx_disp_add(ctx->gfx, &disp_cfg);
    ESP_RETURN_ON_FALSE(ctx->disp, ESP_ERR_NO_MEM, TAG, "gfx display add failed");

    ESP_RETURN_ON_ERROR(gfx_emote_lock(ctx->gfx), TAG, "gfx lock failed");
    ctx->anim = gfx_anim_create(ctx->disp);
    if (ctx->anim) {
        (void)gfx_obj_align(ctx->anim, GFX_ALIGN_CENTER, 0, 0);
        (void)gfx_obj_set_visible(ctx->anim, false);
    }
    (void)gfx_emote_unlock(ctx->gfx);
    ESP_RETURN_ON_FALSE(ctx->anim, ESP_ERR_NO_MEM, TAG, "gfx animation create failed");

    return ESP_OK;
}

static void display_task(void *arg)
{
    jr_display_ctx_t *ctx = (jr_display_ctx_t *)arg;
    __atomic_store_n(&ctx->task, xTaskGetCurrentTaskHandle(), __ATOMIC_RELEASE);
    diag_store(&ctx->task_running, true);
    diag_store(&ctx->init_state, JR_DISPLAY_INIT_STARTING);

    esp_err_t err = display_engine_init(ctx);
    if (err != ESP_OK) {
        __atomic_store_n(&ctx->last_error, err, __ATOMIC_RELAXED);
        diag_store(&ctx->init_state, JR_DISPLAY_INIT_FAILED);
        ESP_LOGE(TAG, "presenter disabled (fail-soft): %s", esp_err_to_name(err));
    } else {
        diag_store(&ctx->init_state, JR_DISPLAY_INIT_READY);
        ESP_LOGI(TAG, "presenter ready: %ux%u, %u fps, %u-row internal DMA strips",
                 (unsigned)ctx->board.width, (unsigned)ctx->board.height,
                 (unsigned)JR_DISPLAY_RENDER_FPS,
                 (unsigned)JR_DISPLAY_STRIP_ROWS);
    }

    uint32_t last_word = UINT32_MAX;
    uint32_t last_test_pattern = UINT32_MAX;
    uint64_t last_diag_ms = 0;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(JR_DISPLAY_DRIVER_MS));
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);

        if (diag_load(&ctx->init_state) == JR_DISPLAY_INIT_READY) {
            uint32_t test_pattern = diag_load(&ctx->test_pattern);
            bool test_pattern_changed = test_pattern != last_test_pattern;
            bool snapshot_refresh =
                diag_exchange(&ctx->snapshot_refresh_requested, 0U) != 0U;
            if (test_pattern_changed || snapshot_refresh) {
                if (gfx_emote_lock(ctx->gfx) == ESP_OK) {
                    gfx_disp_refresh_all(ctx->disp);
                    (void)gfx_emote_unlock(ctx->gfx);
                }
                if (test_pattern_changed) {
                    last_test_pattern = test_pattern;
                    ESP_LOGI(TAG, "panel test pattern=%u",
                             (unsigned)test_pattern);
                }
            }
            uint32_t word = __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
            bool blank = (word & JR_DISPLAY_CMD_BLANK) != 0;
            jr_face_t face = (jr_face_t)((word & JR_DISPLAY_CMD_FACE_MASK) >>
                                         JR_DISPLAY_CMD_FACE_SHIFT);
            uint8_t amplitude = word & JR_DISPLAY_CMD_AMP_MASK;

            if (blank) {
                if (!__atomic_load_n(&ctx->blanked, __ATOMIC_RELAXED)) {
                    err = apply_blank(ctx);
                    if (err == ESP_OK) {
                        diag_inc(&ctx->state_changes);
                    }
                }
                last_word = word;
            } else {
                bool changed = word != last_word;
                bool decay_due =
                    (face == JR_FACE_LISTENING || face == JR_FACE_SPEAKING) &&
                    now_ms - ctx->decay_gate_ms >= JR_DISPLAY_DECAY_HOLD_MS;
                if ((changed && now_ms >= ctx->apply_retry_gate_ms) || decay_due) {
                    jr_face_t before = (jr_face_t)diag_load(&ctx->applied_face);
                    bool was_blanked = __atomic_load_n(&ctx->blanked, __ATOMIC_RELAXED);
                    bool stale = false;
                    err = apply_face(ctx, face, amplitude, &stale);
                    if (stale) {
                        ctx->apply_retry_gate_ms = 0;
                    } else if (err == ESP_OK) {
                        if (was_blanked || before != face) {
                            diag_inc(&ctx->state_changes);
                        }
                        diag_store(&ctx->applied_face, face);
                        __atomic_store_n(&ctx->last_error, ESP_OK, __ATOMIC_RELAXED);
                        ctx->apply_retry_gate_ms = 0;
                        /* Latch ONLY on success: a transient failure (PSRAM
                         * alloc, SPIFFS hiccup) on a non-reactive face was
                         * previously never retried — wrong face persisted. */
                        last_word = word;
                    } else {
                        __atomic_store_n(&ctx->last_error, err, __ATOMIC_RELAXED);
                        ctx->apply_retry_gate_ms = now_ms + 500U;
                        ESP_LOGW(TAG, "face=%d apply failed (retry in 500 ms): %s",
                                 (int)face, esp_err_to_name(err));
                    }
                }
            }
        }

        if (now_ms - last_diag_ms >= 1000U) {
            last_diag_ms = now_ms;
            diag_store(&ctx->task_stack_hwm, uxTaskGetStackHighWaterMark(NULL));
            diag_store(&ctx->free_psram_bytes,
                       heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            if (ctx->gfx) {
                diag_store(&ctx->actual_fps, gfx_timer_get_actual_fps(ctx->gfx));
            }
        }
    }
}

esp_err_t jr_display_start(jr_display_t *out_port)
{
    if (!out_port) {
        return ESP_ERR_INVALID_ARG;
    }
    out_port->ctx = &s_display;
    out_port->blank = display_blank_cb;
    out_port->present = display_present_cb;

    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&s_display.started, &expected, 1U, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        display_task, "jr_present", JR_DISPLAY_TASK_STACK, &s_display,
        JR_DISPLAY_TASK_PRIORITY, &s_display.task, JR_DISPLAY_TASK_CORE);
    if (created != pdPASS) {
        __atomic_store_n(&s_display.started, 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&s_display.last_error, ESP_ERR_NO_MEM, __ATOMIC_RELAXED);
        diag_store(&s_display.init_state, JR_DISPLAY_INIT_FAILED);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool jr_display_is_ready(void)
{
    return diag_load(&s_display.init_state) == JR_DISPLAY_INIT_READY;
}

esp_err_t jr_display_get_diag(jr_display_diag_t *out_diag)
{
    if (!out_diag) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_diag, 0, sizeof(*out_diag));
    out_diag->init_state = (jr_display_init_state_t)diag_load(&s_display.init_state);
    out_diag->last_error = __atomic_load_n(&s_display.last_error, __ATOMIC_RELAXED);
    out_diag->task_running = diag_load(&s_display.task_running) != 0;
    out_diag->blanked = __atomic_load_n(&s_display.blanked, __ATOMIC_RELAXED);
    out_diag->requested_face = (jr_face_t)diag_load(&s_display.requested_face);
    out_diag->applied_face = (jr_face_t)diag_load(&s_display.applied_face);
    out_diag->requested_amplitude = (uint8_t)diag_load(&s_display.requested_amplitude);
    int applied_bucket = __atomic_load_n(&s_display.applied_bucket, __ATOMIC_RELAXED);
    out_diag->applied_bucket = applied_bucket < 0 ? 0 : (uint8_t)applied_bucket;
    out_diag->requests = diag_load(&s_display.requests);
    out_diag->state_changes = diag_load(&s_display.state_changes);
    out_diag->segment_sets = diag_load(&s_display.segment_sets);
    out_diag->asset_load_failures = diag_load(&s_display.asset_load_failures);
    out_diag->flush_submissions = diag_load(&s_display.flush_submissions);
    out_diag->flush_completions = diag_load(&s_display.flush_completions);
    out_diag->flush_errors = diag_load(&s_display.flush_errors);
    out_diag->actual_fps = diag_load(&s_display.actual_fps);
    out_diag->current_asset_bytes = diag_load(&s_display.current_asset_bytes);
    out_diag->free_psram_bytes = diag_load(&s_display.free_psram_bytes);
    out_diag->task_stack_hwm = diag_load(&s_display.task_stack_hwm);
    return ESP_OK;
}

static void snapshot_fill_info_locked(const jr_display_ctx_t *ctx,
                                      jr_display_snapshot_info_t *out_info)
{
    memset(out_info, 0, sizeof(*out_info));
    out_info->width = ctx->board.width;
    out_info->height = ctx->board.height;
    out_info->bytes = ctx->snapshot_bytes;
    out_info->frame_id = ctx->snapshot_frame_id;
    out_info->last_flush_ms = ctx->snapshot_last_flush_ms;
    out_info->valid = ctx->snapshot_valid;
    out_info->test_pattern =
        (jr_display_test_pattern_t)diag_load(&ctx->test_pattern);
}

esp_err_t jr_display_snapshot_get_info(jr_display_snapshot_info_t *out_info)
{
    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!jr_display_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(snapshot_ensure_buffer(&s_display), TAG,
                        "display mirror allocation failed");
    uint32_t requested_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (xSemaphoreTake(s_display.snapshot_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint64_t baseline_frame = s_display.snapshot_frame_id;
    xSemaphoreGive(s_display.snapshot_lock);

    /* Arm the exact-submission mirror and ask the display owner to force a
     * full refresh. Returning an older merely-valid frame is actively harmful
     * to diagnostics: a test-pattern request would otherwise appear to pass
     * while the downloaded evidence still contained the previous face. */
    diag_store(&s_display.snapshot_consumer_ms, requested_ms);
    diag_store(&s_display.snapshot_refresh_requested, 1U);
    if (s_display.task != NULL) {
        xTaskNotifyGive(s_display.task);
    }

    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(JR_DISPLAY_SNAPSHOT_WAIT_MS);
    do {
        if (xSemaphoreTake(s_display.snapshot_lock,
                           pdMS_TO_TICKS(25)) == pdTRUE) {
            snapshot_fill_info_locked(&s_display, out_info);
            bool new_frame = out_info->valid &&
                out_info->frame_id > baseline_frame;
            bool submitted_after_request =
                (int32_t)((uint32_t)out_info->last_flush_ms - requested_ms) >= 0;
            xSemaphoreGive(s_display.snapshot_lock);
            if (new_frame && submitted_after_request) {
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while ((TickType_t)(xTaskGetTickCount() - started) < timeout);

    return ESP_ERR_TIMEOUT;
}

esp_err_t jr_display_snapshot_copy_rgb565(void *dst, size_t dst_size,
                                          jr_display_snapshot_info_t *out_info)
{
    if (dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    jr_display_snapshot_info_t info;
    ESP_RETURN_ON_ERROR(jr_display_snapshot_get_info(&info), TAG,
                        "display mirror unavailable");
    if (dst_size < info.bytes || info.bytes == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (xSemaphoreTake(s_display.snapshot_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(dst, s_display.snapshot_rgb565, info.bytes);
    snapshot_fill_info_locked(&s_display, &info);
    xSemaphoreGive(s_display.snapshot_lock);
    if (out_info != NULL) {
        *out_info = info;
    }
    return ESP_OK;
}

esp_err_t jr_display_set_test_pattern(jr_display_test_pattern_t pattern)
{
    if (pattern < JR_DISPLAY_TEST_OFF ||
        pattern > JR_DISPLAY_TEST_TOUCH_CHALLENGE) {
        return ESP_ERR_INVALID_ARG;
    }
    diag_store(&s_display.test_pattern, (uint32_t)pattern);
    if (s_display.task != NULL) {
        xTaskNotifyGive(s_display.task);
    }
    return ESP_OK;
}

jr_display_test_pattern_t jr_display_get_test_pattern(void)
{
    return (jr_display_test_pattern_t)diag_load(&s_display.test_pattern);
}

esp_err_t jr_display_set_touch_challenge(int sector, uint8_t progress)
{
    if (sector < -1 || sector > 7 || progress > 3U) {
        return ESP_ERR_INVALID_ARG;
    }
    __atomic_store_n(&s_display.challenge_sector, sector, __ATOMIC_RELAXED);
    diag_store(&s_display.challenge_progress, progress);
    diag_store(&s_display.test_pattern, JR_DISPLAY_TEST_TOUCH_CHALLENGE);
    if (s_display.task != NULL) {
        xTaskNotifyGive(s_display.task);
    }
    return ESP_OK;
}

/* Navigation is defined with the rest of the shell's public API below; the
 * shell-state setter needs one step of it for the DESK strand. */
typedef enum {
    NAV_OP_NEXT = 0,
    NAV_OP_PREV,
    NAV_OP_UP,
    NAV_OP_DOWN,
    NAV_OP_HOME,
    NAV_OP_SET,
} nav_op_t;
static void nav_step(nav_op_t op, uint32_t arg);

void jr_display_set_shell_state(bool shade_open, bool agent_active,
                                uint8_t agent_progress,
                                jr_display_agent_state_t agent_state)
{
    if (agent_state < JR_DISPLAY_AGENT_NONE ||
        agent_state > JR_DISPLAY_AGENT_FAILED) {
        agent_state = JR_DISPLAY_AGENT_NONE;
    }
    uint32_t word = (shade_open ? JR_DISPLAY_SHELL_SHADE : 0U) |
                    (agent_active ? JR_DISPLAY_SHELL_AGENT : 0U) |
                    ((uint32_t)agent_state << JR_DISPLAY_SHELL_STATE_SHIFT) |
                    agent_progress;
    uint32_t previous = __atomic_exchange_n(&s_display.shell_word, word,
                                             __ATOMIC_RELAXED);
    if (previous == word) {
        return;
    }
    /* THE STRAND. If the job ends while DESK is the screen under the owner's
     * eyes, the ring no longer admits DESK — so walk them one step forward,
     * to ACTIVITY, where what just finished is the newest row. Forward, not
     * back: the finished job's receipt is the natural next thing to see. The
     * step goes through nav_step, so the slide, the serial and the orbit all
     * treat it as an ordinary move, and any open sheet closes with it. Only
     * on the live->dark EDGE: an explicit nav_set(DESK) while dark is a
     * caller's decision and is left alone. */
    if ((previous & JR_DISPLAY_SHELL_AGENT) != 0U && !agent_active &&
        jr_display_nav_space() == JR_DISPLAY_SPACE_DESK) {
        nav_step(NAV_OP_NEXT, 0u);
    }
    if (s_display.task != NULL) {
        xTaskNotifyGive(s_display.task);
    }
}

esp_err_t jr_display_surface_present(const jr_display_surface_t *surface)
{
    if (surface == NULL ||
        surface->kind < JR_DISPLAY_SURFACE_NOTICE ||
        surface->kind > JR_DISPLAY_SURFACE_CONSENT ||
        surface->action_count > JR_DISPLAY_SURFACE_ACTION_CAP ||
        strnlen(surface->title, JR_DISPLAY_SURFACE_TITLE_CAP) == 0U ||
        strnlen(surface->title, JR_DISPLAY_SURFACE_TITLE_CAP) >=
            JR_DISPLAY_SURFACE_TITLE_CAP ||
        strnlen(surface->body, JR_DISPLAY_SURFACE_BODY_CAP) >=
            JR_DISPLAY_SURFACE_BODY_CAP) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < surface->action_count; ++i) {
        size_t length = strnlen(surface->action_labels[i],
                                JR_DISPLAY_SURFACE_LABEL_CAP);
        if (length == 0U || length >= JR_DISPLAY_SURFACE_LABEL_CAP) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    taskENTER_CRITICAL(&s_surface_mux);
    s_surface = *surface;
    s_surface_active = true;
    taskEXIT_CRITICAL(&s_surface_mux);
    if (s_display.task != NULL) {
        xTaskNotifyGive(s_display.task);
    }
    return ESP_OK;
}

void jr_display_surface_dismiss(void)
{
    taskENTER_CRITICAL(&s_surface_mux);
    memset(&s_surface, 0, sizeof s_surface);
    s_surface_active = false;
    taskEXIT_CRITICAL(&s_surface_mux);
    if (s_display.task != NULL) {
        xTaskNotifyGive(s_display.task);
    }
}

bool jr_display_surface_is_active(void)
{
    bool active;
    taskENTER_CRITICAL(&s_surface_mux);
    active = s_surface_active;
    taskEXIT_CRITICAL(&s_surface_mux);
    return active;
}

int jr_display_surface_hit_test(uint16_t x, uint16_t y)
{
    uint8_t count;
    bool active;
    taskENTER_CRITICAL(&s_surface_mux);
    active = s_surface_active;
    count = s_surface.action_count;
    taskEXIT_CRITICAL(&s_surface_mux);
    if (!active || count == 0U || count > JR_DISPLAY_SURFACE_ACTION_CAP ||
        x < 70U || x > 396U || y < 300U || y > 354U) {
        return -1;
    }
    const int left = 70, right = 396, gap = 8;
    int button_w = (right - left - gap * ((int)count - 1)) / (int)count;
    for (uint8_t i = 0; i < count; ++i) {
        int bx0 = left + (int)i * (button_w + gap);
        if ((int)x >= bx0 && (int)x <= bx0 + button_w) {
            return (int)i;
        }
    }
    return -1;
}

/* ===== SPATIAL SHELL: public navigation, hit test and content ============
 *
 * Everything below runs on the CALLER's task. The only shared mutable state
 * is one packed word plus the fixed-capacity text arrays, so no navigation
 * call blocks, allocates, or touches the panel. */

static void nav_wake(void)
{
    /* The render task sleeps between frames when nothing is moving; a nav
     * change has to draw, so poke it rather than wait out the idle tick. */
    TaskHandle_t task = __atomic_load_n(&s_display.task, __ATOMIC_ACQUIRE);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

static void nav_step(nav_op_t op, uint32_t arg)
{
    uint32_t cur = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    for (;;) {
        const uint32_t space = cur & NAV_SPACE_MASK;
        const uint32_t ovl = (cur & NAV_OVL_MASK) >> NAV_OVL_SHIFT;
        uint32_t prev = (cur >> NAV_PREV_SHIFT) & NAV_SPACE_MASK;
        uint32_t serial = (cur >> NAV_SERIAL_SHIFT) & 0xFFu;
        uint32_t nspace = space;
        uint32_t novl = ovl;
        uint32_t fwd = cur & NAV_FORWARD_BIT;

        switch (op) {
        case NAV_OP_NEXT:
            /* WRAPS. The ring was clamped for two stated reasons; the owner's
             * endless-scroll model answers one and accepts the other.
             *
             * "Am I at the end" is not a question an endless ring raises —
             * there is no end, by design, and a swipe never dies against a
             * wall. What we accept in exchange is that the page indicator
             * jumps the width of the dial when it wraps; the fix for that is
             * to draw position as a rotating mark rather than a linear one,
             * which is a render change, not a navigation one. */
            nspace = (space + 1u) % (uint32_t)JR_DISPLAY_SPACE_COUNT;
            /* DESK is stepped OVER while nothing lives there, in both
             * directions and across the wrap: the ring is shorter by one,
             * not the same ring with a hole in it. */
            if (nspace == (uint32_t)JR_DISPLAY_SPACE_DESK && !sp_desk_live()) {
                nspace = (nspace + 1u) % (uint32_t)JR_DISPLAY_SPACE_COUNT;
            }
            fwd = NAV_FORWARD_BIT;
            novl = (uint32_t)JR_DISPLAY_OVERLAY_NONE;
            break;
        case NAV_OP_PREV:
            nspace = (space == 0u)
                ? (uint32_t)JR_DISPLAY_SPACE_COUNT - 1u
                : space - 1u;
            if (nspace == (uint32_t)JR_DISPLAY_SPACE_DESK && !sp_desk_live()) {
                nspace = (nspace == 0u)
                    ? (uint32_t)JR_DISPLAY_SPACE_COUNT - 1u
                    : nspace - 1u;
            }
            fwd = 0u;
            novl = (uint32_t)JR_DISPLAY_OVERLAY_NONE;
            break;
        case NAV_OP_UP:
            /* One vertical axis: up closes the shade if it is open, else it
             * opens the detail. Down is the mirror. */
            novl = ovl == (uint32_t)JR_DISPLAY_OVERLAY_SHADE
                       ? (uint32_t)JR_DISPLAY_OVERLAY_NONE
                       : (uint32_t)JR_DISPLAY_OVERLAY_DETAIL;
            break;
        case NAV_OP_DOWN:
            novl = ovl == (uint32_t)JR_DISPLAY_OVERLAY_DETAIL
                       ? (uint32_t)JR_DISPLAY_OVERLAY_NONE
                       : (uint32_t)JR_DISPLAY_OVERLAY_SHADE;
            break;
        case NAV_OP_HOME:
            nspace = (uint32_t)JR_DISPLAY_SPACE_JARVIS;
            novl = (uint32_t)JR_DISPLAY_OVERLAY_NONE;
            fwd = 0u;
            break;
        case NAV_OP_SET:
        default:
            nspace = arg & NAV_SPACE_MASK;
            novl = (uint32_t)JR_DISPLAY_OVERLAY_NONE;
            fwd = nspace > space ? NAV_FORWARD_BIT : 0u;
            break;
        }

        /* The origin and the serial move ONLY on a real space change. An
         * overlay toggle between a swipe and the next rendered frame must not
         * be able to erase the slide that swipe started. */
        if (nspace != space) {
            prev = space;
            serial = (serial + 1u) & 0xFFu;
        }
        const uint32_t next = nspace | (prev << NAV_PREV_SHIFT) |
                              (novl << NAV_OVL_SHIFT) | fwd |
                              (serial << NAV_SERIAL_SHIFT);
        if (next == cur) {
            return;      /* idempotent: no restarted animation, no wake */
        }
        if (__atomic_compare_exchange_n(&s_nav_word, &cur, next, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    nav_wake();
}

void jr_display_nav_next(void) { nav_step(NAV_OP_NEXT, 0u); }
void jr_display_nav_prev(void) { nav_step(NAV_OP_PREV, 0u); }
void jr_display_nav_up(void)   { nav_step(NAV_OP_UP, 0u); }
void jr_display_nav_down(void) { nav_step(NAV_OP_DOWN, 0u); }
void jr_display_nav_home(void) { nav_step(NAV_OP_HOME, 0u); }

void jr_display_nav_set(jr_display_space_t space)
{
    if (space < JR_DISPLAY_SPACE_JARVIS || space >= JR_DISPLAY_SPACE_COUNT) {
        return;
    }
    nav_step(NAV_OP_SET, (uint32_t)space);
}

jr_display_space_t jr_display_nav_space(void)
{
    return (jr_display_space_t)
        (__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) & NAV_SPACE_MASK);
}

jr_display_overlay_t jr_display_nav_overlay(void)
{
    return (jr_display_overlay_t)
        ((__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) & NAV_OVL_MASK) >>
         NAV_OVL_SHIFT);
}

jr_display_action_t jr_display_hit(int x, int y)
{
    /* Diagnostics and modal presentations outrank the shell: a test pattern,
     * a choice ask and a companion surface each own the glass and carry their
     * own hit tests, so the shell must not answer for them. */
    if (diag_load(&s_display.test_pattern) != (uint32_t)JR_DISPLAY_TEST_OFF ||
        jr_display_choices_active() || jr_display_surface_is_active()) {
        return JR_DISPLAY_ACT_NONE;
    }
    const int dx = x - SP_CX;
    const int dy = y - SP_CY;
    const int r2 = dx * dx + dy * dy;
    if (r2 > SP_R_SHELL * SP_R_SHELL) {
        return JR_DISPLAY_ACT_NONE;   /* the outer band is not the shell's */
    }

    if (sp_shade_open()) {
        /* One large action target below the volume arc. */
        const int bdx = x - SP_BTN_PRIV_CX;
        const int bdy = y - SP_BTN_CY;
        if (bdx * bdx + bdy * bdy <= SP_BTN_R * SP_BTN_R) {
            return JR_DISPLAY_ACT_PRIVACY_TOGGLE;
        }
        /* The arcs live above the centre line; left half decrements, right
         * half increments, exactly as the -/+ end caps show. */
        if (dy <= SP_ARC_HIT_DY) {
            if (r2 >= SP_VOL_HIT_IN * SP_VOL_HIT_IN &&
                r2 <= SP_VOL_HIT_OUT * SP_VOL_HIT_OUT) {
                return dx < 0 ? JR_DISPLAY_ACT_VOLUME_DOWN
                              : JR_DISPLAY_ACT_VOLUME_UP;
            }
        }
        return JR_DISPLAY_ACT_DISMISS;
    }

    const uint32_t nav = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    if (((nav & NAV_OVL_MASK) >> NAV_OVL_SHIFT) ==
        (uint32_t)JR_DISPLAY_OVERLAY_DETAIL) {
        return (y >= SP_SHEET_Y0 && y <= SP_SHEET_Y1) ? JR_DISPLAY_ACT_NONE
                                                      : JR_DISPLAY_ACT_DISMISS;
    }
    if ((nav & NAV_SPACE_MASK) == (uint32_t)JR_DISPLAY_SPACE_JARVIS) {
        /* JARVIS draws no shell furniture, so it claims no taps: the caller's
         * existing tap-to-attention path keeps the centre of the glass. */
        return JR_DISPLAY_ACT_NONE;
    }
    return r2 <= SP_FOCAL_HIT * SP_FOCAL_HIT ? JR_DISPLAY_ACT_FOCUS
                                             : JR_DISPLAY_ACT_NONE;
}

void jr_display_space_set_label(jr_display_space_t space, const char *headline,
                                const char *note)
{
    if (space < JR_DISPLAY_SPACE_JARVIS || space >= JR_DISPLAY_SPACE_COUNT) {
        return;
    }
    sp_copy(s_space_head[space], headline);
    sp_copy(s_space_note[space], note);
    nav_wake();
}

void jr_display_jarvis_set_session(bool linked, uint16_t turns,
                                   uint32_t elapsed_s)
{
    diag_store(&s_jarvis_secs, elapsed_s);
    __atomic_store_n(&s_jarvis_word,
                     (uint32_t)turns | (linked ? (1u << 16) : 0u),
                     __ATOMIC_RELEASE);
}

void jr_display_desk_set_task(const char *task, uint8_t progress,
                              jr_display_agent_state_t state)
{
    if (progress > 100U) {
        progress = 100U;
    }
    if (state < JR_DISPLAY_AGENT_NONE || state > JR_DISPLAY_AGENT_FAILED) {
        state = JR_DISPLAY_AGENT_NONE;
    }
    sp_copy(s_desk_task, task);
    /* Release AFTER the text: a racing frame can read a stale word, never a
     * word that points at half-written characters. */
    __atomic_store_n(&s_desk_word,
                     (uint32_t)progress | ((uint32_t)state << 8),
                     __ATOMIC_RELEASE);
}

void jr_display_weather_set(const jr_display_weather_t *weather)
{
    const uint32_t cur = __atomic_load_n(&s_weather_slot, __ATOMIC_ACQUIRE) & 1u;
    jr_display_weather_t *dst = &s_weather[cur ^ 1u];
    if (weather == NULL) {
        memset(dst, 0, sizeof *dst);            /* valid=false: NO WEATHER */
    } else {
        *dst = *weather;
        /* The condition is the one string; bound it here once so the
         * composer can trust the terminator. Sky outside the enum reads as
         * UNKNOWN, which draws in the ordinary cyan. */
        dst->condition[sizeof dst->condition - 1] = '\0';
        if (dst->sky < JR_DISPLAY_SKY_UNKNOWN || dst->sky > JR_DISPLAY_SKY_SNOW) {
            dst->sky = JR_DISPLAY_SKY_UNKNOWN;
        }
    }
    /* The slot flips AFTER the copy: a racing frame reads the old weather
     * whole, never the new one half-written. */
    __atomic_store_n(&s_weather_slot, cur ^ 1u, __ATOMIC_RELEASE);
    nav_wake();
}

void jr_display_activity_push(const char *kind, const char *summary)
{
    if ((kind == NULL || kind[0] == '\0') &&
        (summary == NULL || summary[0] == '\0')) {
        return;                                 /* nothing happened: no row */
    }
    uint32_t n = __atomic_load_n(&s_act_count, __ATOMIC_ACQUIRE);
    if (n > (uint32_t)ACT_MAX) {
        n = (uint32_t)ACT_MAX;
    }
    /* Newest first: shift the older rows down and drop the fourth. */
    for (int i = ACT_MAX - 1; i > 0; --i) {
        memcpy(s_act_kind[i], s_act_kind[i - 1], sizeof s_act_kind[i]);
        memcpy(s_act_sum[i], s_act_sum[i - 1], sizeof s_act_sum[i]);
        s_act_ms[i] = s_act_ms[i - 1];
    }
    size_t k = 0;
    for (; kind != NULL && k + 1U < sizeof s_act_kind[0] && kind[k] != '\0'; ++k) {
        s_act_kind[0][k] = kind[k];
    }
    s_act_kind[0][k] = '\0';
    size_t s = 0;
    for (; summary != NULL && s + 1U < sizeof s_act_sum[0] && summary[s] != '\0';
         ++s) {
        s_act_sum[0][s] = summary[s];
    }
    s_act_sum[0][s] = '\0';
    s_act_ms[0] = (uint32_t)(esp_timer_get_time() / 1000);
    /* The count lands last, so a frame that reads the new count reads the
     * rows it was written for. */
    __atomic_store_n(&s_act_count, n < (uint32_t)ACT_MAX ? n + 1u : n,
                     __ATOMIC_RELEASE);
    nav_wake();
}

void jr_display_power_set(uint8_t percent, uint16_t millivolts,
                          bool usb_present, bool charging)
{
    if (percent > 100U) {
        percent = 0xFFU;
    }
    const uint32_t word =
        (uint32_t)percent | ((uint32_t)millivolts << 8) |
        (usb_present ? (1u << 24) : 0u) |
        (charging ? (1u << 25) : 0u);
    if (__atomic_load_n(&s_power_word, __ATOMIC_ACQUIRE) == word) {
        return;
    }
    __atomic_store_n(&s_power_word, word, __ATOMIC_RELEASE);
    nav_wake();
}
void jr_display_ota_set(jr_display_ota_state_t state, uint8_t percent,
                        uint8_t active_slot, uint8_t target_slot,
                        bool preflight_ok)
{
    if (state < JR_DISPLAY_OTA_IDLE || state > JR_DISPLAY_OTA_ROLLED_BACK) {
        state = JR_DISPLAY_OTA_IDLE;
    }
    if (percent > 100U) {
        percent = 100U;
    }
    /* Anything that is not a real partition index becomes the unknown/none
     * nibble, so the renderer never has to distrust what it reads. */
    if (active_slot > 1U) {
        active_slot = (uint8_t)OTA_SLOT_NONE;
    }
    if (target_slot > 1U) {
        target_slot = (uint8_t)OTA_SLOT_NONE;
    }
    __atomic_store_n(&s_ota_word,
                     (uint32_t)state | ((uint32_t)percent << 8) |
                         ((uint32_t)active_slot << 16) |
                         ((uint32_t)target_slot << 20) |
                         (preflight_ok ? (1u << 24) : 0u),
                     __ATOMIC_RELEASE);
    nav_wake();
}

void jr_display_panel_off_request(void)
{
    if (diag_load(&s_panel_off) == 0U) {
        diag_store(&s_panel_off, 1U);
        nav_wake();
    }
}

bool jr_display_panel_is_off(void)
{
    return diag_load(&s_panel_off) == 2U;
}

void jr_display_links_set(const jr_display_links_t *links)
{
    if (links == NULL) {
        return;
    }
    /* The address lands before the word that gates it (the labels' rule),
     * so a racing frame shows the old address at worst, never a torn one. */
    size_t i = 0;
    for (; i + 1U < sizeof s_links_ip && links->ip[i] != '\0'; ++i) {
        s_links_ip[i] = links->ip[i];
    }
    s_links_ip[i] = '\0';
    const int neg = links->rssi_dbm < 0 ? -(int)links->rssi_dbm : 0;
    const uint32_t word =
        (links->wifi_up ? 1u : 0u) |
        (links->link_open ? (1u << 1) : 0u) |
        (((uint32_t)(links->tools > 2U ? 2U : links->tools)) << 2) |
        (links->desk_live ? (1u << 4) : 0u) |
        (links->radio_saving ? (1u << 5) : 0u) |
        ((uint32_t)(neg > 255 ? 255 : neg) << 8) |
        ((uint32_t)(links->chip_c_valid
                        ? (links->chip_c < -40 ? 0 : links->chip_c + 40)
                        : 0) << 16) |
        ((uint32_t)((links->cpu_mhz / 20u) & 0xFu) << 24);
    if (__atomic_load_n(&s_links_word, __ATOMIC_ACQUIRE) == word) {
        return;
    }
    __atomic_store_n(&s_links_word, word, __ATOMIC_RELEASE);
    nav_wake();
}

void jr_display_set_status(uint8_t volume)
{
    if (volume > 100U) {
        volume = 100U;
    }
    __atomic_store_n(&s_status_word, (uint32_t)volume, __ATOMIC_RELEASE);
}
