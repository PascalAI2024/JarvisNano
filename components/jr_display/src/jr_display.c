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
#include "jr_display/companion.h"
#include <stdatomic.h>
#include "jr_display/hud_render.h"
#include "jr_display/glass.h"
#include "jr_display/glass_icon.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#include "lib/eaf/gfx_eaf_dec.h"   /* engine-private: the one-shot dial decode */
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
/* The engine renders every period whether or not a pixel changed, so the
 * target fps is the idle cost of the glass (~half of core 0 at 24). The rest
 * ladder lowers it; 2 keeps a dark DREAM face and captions alive. */
#define JR_DISPLAY_RENDER_FPS_MIN    2
/* 12-row internal-SRAM DMA strips — the live-safe memory configuration.
 * A 20-row hardware A/B left FPS unchanged at 12 while shrinking the largest
 * internal block from 32 KB to 12.8 KB, proving strip waits were not the
 * current bottleneck. PSRAM strips still require a failing bounce allocation. */
#define JR_DISPLAY_STRIP_ROWS        12
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
    bool embedded;      /* data points into the app image: read-only, never freed */
} jr_display_clip_t;

/* Gfx has two in-flight buffers. A bounded FIFO tags actual submissions,
 * including callbacks that arrive before draw_bitmap returns. No ISR heap. */
#define DISPLAY_TRANSFER_TAGS 8U
#define TRANSFER_RESERVED 16U
#define TRANSFER_OK 1U
#define TRANSFER_DONE 2U
#define TRANSFER_FAILED 4U
#define TRANSFER_PUBLISHED 8U
typedef struct {
    volatile uint32_t flags;
    uint32_t on_word;
    bool glance_time;
    uint32_t done_ms;
} display_transfer_t;

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

    /* Fast voice faces stay resident: reloading ~1 MB from SPIFFS per face
     * change dropped 1-2 frames and churned PSRAM. Baked dials instead retain
     * one decoded shared frame; their compressed data is released only once
     * neither that decode nor the vendor animation parser needs it. */
    jr_display_clip_t clips[JR_FACE_COUNT];
    const jr_display_clip_t *active;    /* visible anim, NULL while a dial blits */
    const jr_display_clip_t *bound;     /* vendor parser retains this even stopped */
    jr_face_t loaded_face;
    volatile bool blanked;
    volatile int applied_bucket;
    int last_end;
    uint64_t decay_gate_ms;
    uint64_t apply_retry_gate_ms;   /* backoff after a failed apply_face */
    uint32_t missing_faces;         /* bit per face whose clip this art image lacks */
    jr_face_t shown_face;           /* the clip actually bound: face, or its fallback */
    bool dream_active;             /* presenter-owned stopped emote */
    bool quiet_active;             /* visible one-shot muted HOME frame */
    uint32_t quiet_clock_key, quiet_env_key, quiet_caption_key;
    uint32_t quiet_flush_error_key;
    uint32_t dream_clock_key;
    uint32_t dream_rim_key;
    jr_display_rest_mode_t rest_mode;
    jr_companion_view_t rest_view;
    bool panel_touched;
    uint8_t panel_restore_brightness;
    uint32_t panel_prepared_word;
    bool panel_prepared;
    bool panel_sleep_attempted;
    bool panel_last_sleep;
    int64_t panel_sleep_attempt_us;
    display_transfer_t transfer_tags[DISPLAY_TRANSFER_TAGS];
    volatile uint32_t transfer_head, transfer_tail;

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
    /* The software render's own cost, apart from the DMA wait: microseconds
     * spent compositing strips (render_us, wrapping) over render_frames
     * frames, and the last whole second's average per frame. One GET of
     * /api/display then gives ms/frame for whatever is on the glass. */
    volatile uint32_t render_us;
    volatile uint32_t render_frames;
    volatile uint32_t render_frame_us;
    /* Where a frame's compositing goes, by layer (wrapping microseconds):
     * dial, canvas, hud, shell, surface. Deltas over render_frames. */
    volatile uint32_t layer_us[5];
    volatile uint32_t init_start_boot_ms;
    volatile uint32_t init_end_boot_ms;
    volatile uint32_t first_transfer_done_boot_ms;
    volatile uint32_t init_started;
    volatile uint32_t init_finished;
    /* 0 = no callback, 2 = publishing, 1 = timestamp available. */
    volatile uint32_t first_transfer_state;
    volatile uint32_t first_glance_time_transfer_done_boot_ms;
    volatile uint32_t first_glance_time_transfer_state;
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

static void nav_wake(void);

static void display_publish(uint32_t word)
{
    const uint32_t prev = __atomic_exchange_n(&s_display.requested_word, word,
                                              __ATOMIC_ACQ_REL);
    diag_store(&s_display.requested_face,
               (word & JR_DISPLAY_CMD_FACE_MASK) >> JR_DISPLAY_CMD_FACE_SHIFT);
    diag_store(&s_display.requested_amplitude, word & JR_DISPLAY_CMD_AMP_MASK);
    diag_inc(&s_display.requests);

    if (prev != word) {
        nav_wake();
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
    case JR_FACE_DIAL_DIVER:  return JR_DISPLAY_MOUNT_POINT "/dial_diver.eaf";
    case JR_FACE_DIAL_DRESS:  return JR_DISPLAY_MOUNT_POINT "/dial_dress.eaf";
    case JR_FACE_DIAL_PILOT:  return JR_DISPLAY_MOUNT_POINT "/dial_pilot.eaf";
    case JR_FACE_DIAL_FUTURE: return JR_DISPLAY_MOUNT_POINT "/dial_future.eaf";
    case JR_FACE_DIAL_SKELETON:  return JR_DISPLAY_MOUNT_POINT "/dial_skeleton.eaf";
    case JR_FACE_DIAL_CELESTIAL: return JR_DISPLAY_MOUNT_POINT "/dial_celestial.eaf";
    default:                return NULL;
    }
}

/* The face a newer firmware shows when the art partition predates the clip:
 * each quiet face grew out of an older one and degrades back to it. A face
 * with no parent returns itself, which is how the loader knows to stop. */
static jr_face_t face_fallback(jr_face_t face)
{
    switch (face) {
    case JR_FACE_RESTING:  return JR_FACE_IDLE;
    case JR_FACE_MUTED:    return JR_FACE_IDLE;
    case JR_FACE_LINKING:  return JR_FACE_THINKING;
    /* A dial the partition lacks: the idle face (always present) loads
     * underneath and the watch clears its disc and draws the black stand-in
     * dial over it. One level, like every other fallback. */
    case JR_FACE_DIAL_DIVER:
    case JR_FACE_DIAL_DRESS:
    case JR_FACE_DIAL_PILOT:
    case JR_FACE_DIAL_FUTURE:
    case JR_FACE_DIAL_SKELETON:
    case JR_FACE_DIAL_CELESTIAL: return JR_FACE_IDLE;
    default:               return face;
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
    /* ONE FRAME, BUT ITS TIMER IS THE ONLY THING THAT INVALIDATES THE
     * GLASS. The anim timer's tick is what marks the display dirty, so a
     * dial scheduled at 8 fps capped every baked-dial watch — hands, sweep
     * and all — at 8 flushed frames a second whatever the ladder asked
     * (measured 7.8–8.4 fps on all four dials against 11.4 on JARVIS,
     * 2026-09-05). The ladder's render cadence still slows it at rest. */
    case JR_FACE_DIAL_DIVER:
    case JR_FACE_DIAL_DRESS:
    case JR_FACE_DIAL_PILOT:
    case JR_FACE_DIAL_FUTURE:
    case JR_FACE_DIAL_SKELETON:
    case JR_FACE_DIAL_CELESTIAL: return 24;
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

/* SKELETON and CELESTIAL ride in the app image, not in emote_assets (the
 * partition cannot take them: docs/reference/asset-pipeline.md). CMake
 * EMBED_FILES puts the two EAFs in flash rodata; the dial path only needs a
 * pointer and a size (eaf_init + one eaf_frame_decode read the source, never
 * write it), so no file read and no PSRAM copy. On the host the stubs hand
 * back the real file bytes (probe/stubs/idf_stubs.inc). */
#ifdef ESP_PLATFORM
extern const uint8_t s_embedded_skeleton_start[] asm("_binary_dial_skeleton_eaf_start");
extern const uint8_t s_embedded_skeleton_end[]   asm("_binary_dial_skeleton_eaf_end");
extern const uint8_t s_embedded_celestial_start[] asm("_binary_dial_celestial_eaf_start");
extern const uint8_t s_embedded_celestial_end[]   asm("_binary_dial_celestial_eaf_end");
static const uint8_t *embedded_dial(jr_face_t face, size_t *size)
{
    switch (face) {
    case JR_FACE_DIAL_SKELETON:
        *size = (size_t)(s_embedded_skeleton_end - s_embedded_skeleton_start);
        return s_embedded_skeleton_start;
    case JR_FACE_DIAL_CELESTIAL:
        *size = (size_t)(s_embedded_celestial_end - s_embedded_celestial_start);
        return s_embedded_celestial_start;
    default:
        return NULL;
    }
}
#else
const uint8_t *jr_host_embedded_dial(jr_face_t face, size_t *size);
#define embedded_dial jr_host_embedded_dial
#endif

static esp_err_t clip_load_embedded(const uint8_t *blob, size_t size,
                                    jr_display_clip_t *out)
{
    memset(out, 0, sizeof(*out));
    if (blob == NULL || size == 0U || size > JR_DISPLAY_ASSET_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t frames = eaf_frame_count(blob, size);
    if (frames < 2) {
        return ESP_ERR_INVALID_CRC;
    }
    /* The one cast away from const: the field is shared with heap clips and
     * is only ever written for those (embedded is the guard). */
    out->data = (uint8_t *)(uintptr_t)blob;
    out->size = size;
    out->total_frames = frames;
    out->last_frame = frames - 1U;
    out->embedded = true;
    return ESP_OK;
}

static esp_err_t clip_load(jr_face_t face, jr_display_clip_t *out)
{
    size_t embedded_size = 0;
    const uint8_t *embedded = out ? embedded_dial(face, &embedded_size) : NULL;
    if (embedded != NULL) {
        return clip_load_embedded(embedded, embedded_size, out);
    }
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
    nav_wake();
    return ESP_OK;
}

void jr_display_canvas_clear(void)
{
    if (__atomic_exchange_n(&s_canvas_until_ms, 0U, __ATOMIC_ACQ_REL) != 0U) {
        nav_wake();
    }
}

bool jr_display_canvas_active(void)
{
    uint32_t until = __atomic_load_n(&s_canvas_until_ms, __ATOMIC_ACQUIRE);
    return until != 0U &&
           (int32_t)((uint32_t)(esp_timer_get_time() / 1000) - until) < 0;
}

/* THE DIAL OFF THE ENGINE (N14.2). A baked dial is one still frame, yet as
 * an anim the engine decoded it again every frame: 224 KB of blocks and a
 * 434 KB palette expand, most of the ~37 ms engine share on DIVER. It is
 * decoded once, in panel byte order, into one shared PSRAM frame
 * (dial_show); on WATCH the anim is hidden and each strip copies the frame's
 * rows here, in the anim's place under every overlay. s_dial_blit and the
 * frame change only under the render lock, which the render task holds for
 * the whole frame, so a strip never sees a half-decoded dial. */
static uint16_t *s_dial_frame;                       /* width*height, PSRAM */
static jr_face_t s_dial_frame_face = JR_FACE_COUNT;  /* the art it holds */
static bool      s_dial_blit;

/* THE PARKED BACKDROP. A data screen (STATUS, WEATHER, DESK, ACTIVITY) sits on
 * true black, so once the slide has settled the face is not decoded at all:
 * the presenter stops the anim and hides it, the strip is the engine's black
 * background plus the shell, and sp_veil is skipped. s_park_want is the render
 * task's verdict (atomic, no lock); s_parked is the presenter's fact, changed
 * only under the render lock like s_dial_blit, so a strip never sees it flip
 * mid-frame. A hidden anim invalidates nothing, so while parked the presenter
 * refreshes the glass every pass (see display_task) or the screen freezes.
 * The face state machine keeps running underneath: apply_face and
 * program_segment still bind and program the requested face, and anim_run is
 * the one place that starts and shows it, so unparking brings up the face
 * requested now, not the one that was up when the screen was entered. */
static bool s_parked;
static int  s_park_want;
static int  s_space_black;   /* 0..256: how far the backdrop has gone to black */

static void apply_dial(const jr_display_ctx_t *ctx, int x1, int y1,
                       int x2, int y2, uint16_t *pixels)
{
    if (!s_dial_blit || s_dial_frame == NULL || pixels == NULL) {
        return;
    }
    const int width = x2 - x1;
    for (int row = y1; row < y2; ++row) {
        memcpy(pixels + (size_t)(row - y1) * (size_t)width,
               s_dial_frame + (size_t)row * ctx->board.width + (size_t)x1,
               (size_t)width * sizeof(uint16_t));
    }
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
    static const uint8_t DEG[] = {0x00,0x06,0x09,0x09,0x06};   /* '*' draws a degree ring */
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
    case '*': g=DEG; break;
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
            uint16_t native = panel_order_color(ctx, *slot);
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
 * privacy mute in bit 9, DREAM intent in bit 10, and tilt offsets as signed
 * pixels in bits 16-31. Doing the float trigonometry at push time keeps it out
 * of the render path. */
static volatile uint32_t s_hud_env_word =
    0x000000FFu;   /* battery unknown, level, listening */
#define JR_DISPLAY_ENV_DREAM (1u << 10)
#define JR_DISPLAY_ENV_MODE_MASK (3u << 10)
#define JR_DISPLAY_ENV_NIGHT (1u << 12)
#define JR_DISPLAY_ENV_USB (1u << 13)
#define JR_DISPLAY_ENV_REST_MASK (0xfu << 10)
#define JR_DISPLAY_ENV_VOICE_BUSY (1u << 14)
#define JR_DISPLAY_ENV_RIM_MASK 0x3ffu
#define JR_DISPLAY_CLOCK_MINUTE_MASK 0x1ffffu
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
static int              s_caption_y[2];            /* cap-height tops of the lines  */
static bool             s_caption_grey;            /* a quiet hint, not a message   */
#define JR_CAPTION_ONE_Y  396                       /* one line: its cap top         */
#define JR_CAPTION_TWO_Y0 380                       /* two lines: first, second      */
#define JR_CAPTION_TWO_Y1 400
#define JR_CAPTION_TRACK  3                         /* tracked label, like the glass */
static volatile int     s_caption_on;
#define JR_DISPLAY_CAPTION_ACTIVE 1
#define JR_DISPLAY_CAPTION_AMBIENT 2

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

/* The day for the dials that carry one, and the millisecond phase of the
 * seconds hand. s_clock_set_ms is the esp_timer ms at which the published
 * second last changed; the flush latches (now - set) once per frame into
 * s_clock_phase_ms so every strip of a frame draws the same sweep angle. */
static volatile uint32_t s_clock_date_word;   /* mday | mon<<5 | wday<<9 | valid<<12 */
static volatile uint32_t s_sun_word;          /* rise | set<<11 | valid<<22 (minutes) */
static volatile uint32_t s_clock_set_ms;
static int s_clock_phase_ms;                  /* render task only, 0..999 */

/* THE LIGHT ON THE WATCH HANDS. The composition root pushes the in-plane UP
 * vector (milli-g, screen axes: x right, y down) into ONE packed word: gx in
 * bits 0-11 and gy in bits 12-23, both signed 12-bit, valid in bit 24. The
 * render task eases a filtered copy toward it once a frame
 * (overlay_fade_tick) and solves the hud_light_t from the filtered values;
 * every strip of the frame reads that one solution. No data, or invalid:
 * the rest light the watch always had. */
#define JR_LIGHT_VALID    (1u << 24)
#define JR_LIGHT_CLAMP_MG 1200
static volatile uint32_t s_light_word;
static int s_light_fx_q8, s_light_fy_q8;      /* filtered, mg * 256; render task only */
static uint32_t s_light_prev_ms;              /* render task only */
static hud_light_t s_light;                   /* render task writes; HTTP diag reads, torn reads are cosmetic */
static volatile int s_light_gx_mg, s_light_gy_mg;   /* the filtered values, for the diag */

static inline int light_field(uint32_t w, unsigned shift)
{
    const int v = (int)((w >> shift) & 0xFFFu);
    return v >= 0x800 ? v - 0x1000 : v;
}

static uint32_t light_word_pack(int gx_mg, int gy_mg, bool valid)
{
    gx_mg = gx_mg < -JR_LIGHT_CLAMP_MG ? -JR_LIGHT_CLAMP_MG
          : gx_mg > JR_LIGHT_CLAMP_MG ? JR_LIGHT_CLAMP_MG : gx_mg;
    gy_mg = gy_mg < -JR_LIGHT_CLAMP_MG ? -JR_LIGHT_CLAMP_MG
          : gy_mg > JR_LIGHT_CLAMP_MG ? JR_LIGHT_CLAMP_MG : gy_mg;
    return ((uint32_t)gx_mg & 0xFFFu) | (((uint32_t)gy_mg & 0xFFFu) << 12) |
           (valid ? JR_LIGHT_VALID : 0u);
}

void jr_display_set_light(int gx_mg, int gy_mg, bool valid)
{
    __atomic_store_n(&s_light_word, light_word_pack(gx_mg, gy_mg, valid),
                     __ATOMIC_RELEASE);
}

/* Once a frame, render task, from the watch branch of overlay_fade_tick. */
static void light_tick(uint32_t now_ms)
{
    const uint32_t w = __atomic_load_n(&s_light_word, __ATOMIC_ACQUIRE);
    const int tx = (w & JR_LIGHT_VALID) ? light_field(w, 0) : 0;
    const int ty = (w & JR_LIGHT_VALID) ? light_field(w, 12) : 0;
    uint32_t dt = now_ms - s_light_prev_ms;
    s_light_prev_ms = now_ms;
    if (dt > 1000u) {
        dt = 1000u;
    }
    s_light_fx_q8 = hud_light_ease(s_light_fx_q8, tx, (int)dt);
    s_light_fy_q8 = hud_light_ease(s_light_fy_q8, ty, (int)dt);
    s_light_gx_mg = s_light_fx_q8 / 256;
    s_light_gy_mg = s_light_fy_q8 / 256;
    hud_light_solve(s_light_gx_mg, s_light_gy_mg, &s_light);
}

/* UI-01 clock state: (on << 16) | (hh << 8) | mm in ONE word, published with
 * a single release-store, so the flush can never read a torn time (an on flag
 * from one set with minutes from another). SINGLE-WRITER: the app task calls
 * jr_display_clock_set at ~1 Hz; the flush only reads. 0 at boot = off. */
static volatile uint32_t s_clock_word;
static volatile uint32_t s_caption_revision; /* changed caption bytes, not ticks */

/* Advance both fades ONE step. Called from panel_flush only at the y==0
 * strip — the same frame-start definition snapshot_record_flush relies on —
 * so every strip of a frame composites at the SAME level and a fade can
 * never band across strip boundaries. State lives beside the canvas section
 * above (its first consumer); this reads the intent words published there
 * and at s_clock_word. */
static void brightness_slew(uint32_t dt_ms);   /* defined with the pump below */
static void sp_fade_tick(uint32_t now_ms, uint32_t dt_ms, int cstep);
static void watch_compose(void);               /* defined with the watch below */

/* The split timers (below) cost a timer read per part per strip, so they run
 * only while GET /api/display?split=1 has asked for them (jr_display_split_arm,
 * a few seconds a request). s_split_on is the render task's own copy, decided
 * once a frame in overlay_fade_tick; split_mark and split_now read it. */
static volatile uint32_t s_split_deadline_ms;
static bool s_split_on;

/* Frames composed since boot, counted once a frame by overlay_fade_tick. The
 * recorded shapes (fring_prepare, shade_rec_fill) wait for a value to hold for
 * a second frame before they record it, so a number being dragged never pays
 * for a record it will throw away. */
static uint32_t s_frame_no;

typedef struct {
    int32_t k0, k1;
    uint32_t first;     /* the frame this key was first asked for */
} sp_settle_t;

/* True once (k0, k1) has been asked for on an earlier frame as well. */
static bool sp_settled(sp_settle_t *g, int32_t k0, int32_t k1)
{
    if (g->k0 != k0 || g->k1 != k1) {
        g->k0 = k0;
        g->k1 = k1;
        g->first = s_frame_no;
        return false;
    }
    return g->first != s_frame_no;
}

static void overlay_fade_tick(void)
{
    s_frame_no++;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    s_split_on = (int32_t)(__atomic_load_n(&s_split_deadline_ms, __ATOMIC_RELAXED) - now) > 0;
    uint32_t dt = now - s_fade_prev_ms;
    s_fade_prev_ms = now;
    /* Physical brightness keeps its slew cap; finite UI transitions consume
     * elapsed time so a 200/500 ms rest frame does not stretch a 250 ms fade
     * over several seconds. The longest fade bounds the integer products. */
    brightness_slew(dt > 100u ? 100u : dt);
    if (dt > JR_DISPLAY_FADE_MS) {
        dt = JR_DISPLAY_FADE_MS;
    }
    int step = (int)((dt * 256u) / JR_DISPLAY_FADE_MS);
    if (step < 1) {
        step = 1;
    }

    const uint32_t cw = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
    if ((cw & (1u << 16)) != 0u) {
        s_clock_shown_word = cw;
        const uint32_t since =
            now - __atomic_load_n(&s_clock_set_ms, __ATOMIC_RELAXED);
        s_clock_phase_ms = since > 999u ? 999 : (int)since;
        watch_compose();             /* the words on the watch, once a frame */
        light_tick(now);             /* the room's light, solved once a frame */
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
    nav_wake();
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
    nav_wake();
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
    nav_wake();
}

bool jr_display_caption_pinned(void)
{
    return atomic_load(&s_caption_pinned);
}

bool jr_display_caption_foreground(void)
{
    const int caption = __atomic_load_n(&s_caption_on, __ATOMIC_ACQUIRE);
    return atomic_load(&s_caption_pinned) ||
        ((caption & JR_DISPLAY_CAPTION_ACTIVE) &&
         !(caption & JR_DISPLAY_CAPTION_AMBIENT));
}

/* Only these fixed idle hints may yield to the minute clock. Results, errors,
 * consent and arbitrary text remain foreground, even with stale mood intent. */
static bool caption_is_ambient(const char *text)
{
    static const char *const quiet[] = {
        "JARVIS", "MUTED - HOLD TO LISTEN", "MUTED - HOLD TO RESUME",
        "MUTED - TAP TO WAKE", "ASLEEP - TAP TO WAKE", "ASLEEP. TAP TO WAKE",
        "LISTENING", "RESTING - TAP TO WAKE",
        "POWER CONNECTED", "CHARGE COMPLETE", "ON BATTERY",
    };
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; ++i) {
        if (strcmp(text, quiet[i]) == 0) {
            return true;
        }
    }
    /* Fixed power status, not arbitrary text: canonical decimal 0..100%. */
    if (strncmp(text, "CHARGING ", 9U) == 0) {
        const char *p = text + 9;
        if (*p < '0' || *p > '9' || (*p == '0' && p[1] != '%')) { return false; }
        unsigned percent = 0U, digits = 0U;
        while (*p >= '0' && *p <= '9' && digits < 3U) {
            percent = percent * 10U + (unsigned)(*p++ - '0'); digits++;
        }
        return percent <= 100U && *p == '%' && p[1] == '\0';
    }
    return false;
}

/* A caption is grey when it is furniture: the idle hints and the names the
 * shell prints after a swipe. Results, errors and consent text keep the ink
 * colour so they stay the louder thing on the glass. */
static bool caption_is_quiet(const char *text)
{
    static const char *const names[] = {
        "WEATHER", "STATUS", "DESK", "ACTIVITY", "CONTROLS CLOSED",
        "CONTROLS - UP TO CLOSE", "DETAIL - DOWN TO CLOSE",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (strcmp(text, names[i]) == 0) {
            return true;
        }
    }
    return caption_is_ambient(text);
}

/* The operator lease's own caption ("CODEX MODE - DOUBLE TAP TO EXIT", and the
 * ZEROCHAT twin). It tells the owner how to take the glass back, so it is for
 * the glass itself: the control shade and the detail sheet carry their own
 * hint lines in the same band and the lease caption draws over them. While
 * either is open apply_caption_overlay leaves this caption out. */
static bool s_caption_lease;      /* the current caption is the lease's; render reads it */

static bool caption_is_lease(const char *text)
{
    static const char tail[] = " - DOUBLE TAP TO EXIT";
    const size_t n = strlen(text), t = sizeof tail - 1U;
    return n > t && strcmp(text + n - t, tail) == 0;
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
    if (__atomic_load_n(&s_caption_on, __ATOMIC_ACQUIRE) != 0 &&
        strlen(s_caption_text) == len && memcmp(s_caption_text, text, len) == 0) {
        return;
    }
    memcpy(s_caption_text, text, len);
    s_caption_text[len] = '\0';
    hud_wrap2(s_caption_text, 19, s_caption_line[0], s_caption_line[1]);
    const int c2 = (int)strnlen(s_caption_line[1], 19U);
    s_caption_y[0] = (c2 > 0) ? JR_CAPTION_TWO_Y0 : JR_CAPTION_ONE_Y;
    s_caption_y[1] = JR_CAPTION_TWO_Y1;
    s_caption_grey = caption_is_quiet(s_caption_text);
    s_caption_lease = caption_is_lease(s_caption_text);
    __atomic_store_n(&s_caption_on, JR_DISPLAY_CAPTION_ACTIVE |
                     (caption_is_ambient(s_caption_text)
                         ? JR_DISPLAY_CAPTION_AMBIENT : 0), __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_caption_revision, 1U, __ATOMIC_RELEASE);
    nav_wake();
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
    __atomic_add_fetch(&s_caption_revision, 1U, __ATOMIC_RELEASE);
    nav_wake();
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
    jr_display_dream_set(false);
    nav_wake();
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
    if (__atomic_exchange_n(&s_commit_pct, pct > 100U ? 100U : pct,
                            __ATOMIC_ACQ_REL) != (pct > 100U ? 100U : pct)) {
        nav_wake();
    }
}

void jr_display_bloom(void)
{
    __atomic_store_n(&s_bloom_start_ms,
                     (uint32_t)(esp_timer_get_time() / 1000),
                     __ATOMIC_RELEASE);
    jr_display_dream_set(false);
    nav_wake();
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
    const uint32_t prev = __atomic_load_n(&s_clock_word, __ATOMIC_RELAXED);
    if (on && ((prev & (1u << 16)) == 0u ||
               ((prev >> 17) & 0x3Fu) != (uint32_t)ss)) {
        __atomic_store_n(&s_clock_set_ms,
                         (uint32_t)(esp_timer_get_time() / 1000),
                         __ATOMIC_RELAXED);
    }
    __atomic_store_n(&s_clock_word, word, __ATOMIC_RELEASE);
    const bool dream = (__atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE) &
                        JR_DISPLAY_ENV_MODE_MASK) != 0U;
    if ((prev ^ word) & (dream ? JR_DISPLAY_CLOCK_MINUTE_MASK : UINT32_MAX)) {
        nav_wake();
    }
}

void jr_display_sun_set(int rise_min, int set_min)
{
    if (rise_min < 0 || set_min < 0 || rise_min >= 1440 || set_min >= 1440 ||
        set_min <= rise_min) {
        __atomic_store_n(&s_sun_word, 0u, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&s_sun_word,
                     (uint32_t)rise_min | ((uint32_t)set_min << 11) | (1u << 22),
                     __ATOMIC_RELEASE);
}

void jr_display_clock_set_date(int wday, int mday, int mon)
{
    if (wday < 0 || wday > 6 || mday < 1 || mday > 31 || mon < 0 || mon > 11) {
        __atomic_store_n(&s_clock_date_word, 0u, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&s_clock_date_word,
                     (uint32_t)mday | ((uint32_t)mon << 5) |
                         ((uint32_t)wday << 9) | (1u << 12),
                     __ATOMIC_RELEASE);
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
 * the render task owns the bus. The previous strip's DMA may still be in
 * flight there (the engine pipelines strips, N14.3); esp_lcd's tx_param
 * drains queued color transactions before it sends a command.
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
static _Atomic uint8_t s_brightness_have = 100; /* presenter reads settling */
static _Atomic int s_brightness_shown = 100;    /* render task slews */

esp_err_t jr_display_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    if (__atomic_exchange_n(&s_brightness_want, percent, __ATOMIC_ACQ_REL) != percent) {
        nav_wake();
    }
    return ESP_OK;
}

static uint8_t s_render_fps = JR_DISPLAY_RENDER_FPS;   /* any task (atomic) */

esp_err_t jr_display_set_render_fps(uint8_t fps)
{
    if (fps < JR_DISPLAY_RENDER_FPS_MIN) {
        fps = JR_DISPLAY_RENDER_FPS_MIN;
    }
    if (fps > JR_DISPLAY_RENDER_FPS) {
        fps = JR_DISPLAY_RENDER_FPS;
    }
    if (__atomic_exchange_n(&s_render_fps, fps, __ATOMIC_ACQ_REL) == fps) {
        return ESP_OK;
    }
    nav_wake();
    gfx_handle_t gfx = s_display.gfx;
    if (gfx == NULL) {
        return ESP_OK;   /* not up yet: init reads s_render_fps */
    }
    return gfx_emote_set_fps(gfx, fps);
}

uint8_t jr_display_render_fps(void)
{
    return __atomic_load_n(&s_render_fps, __ATOMIC_ACQUIRE);
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
/* WATCH STYLE: three bits above the 8-bit serial, preserved by every nav step,
 * changed only by NAV_OP_STYLE. Pinned by the shell suite. */
#define NAV_STYLE_SHIFT   17
#define NAV_STYLE_MASK    (0x7u << NAV_STYLE_SHIFT)
_Static_assert((uint32_t)JR_WATCH_STYLE_COUNT <= 8u,
               "JR_WATCH_STYLE_COUNT exceeds the nav word's style field");

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
static volatile uint32_t s_status_word;      /* vol | brightness cap<<8     */

/* The shade's light arc and number show the OWNER's setting (the cap that
 * /api/device/levels reports), not the level the mood ladder has dimmed the
 * panel to: dragging the arc to 80 and reading 30 back is a lie. 0 in the cap
 * byte means nobody has said yet, and the panel's own target stands in. */
static int shade_light(uint32_t status_word)
{
    int cap = (int)((status_word >> 8) & 0xFFu);
    if (cap == 0) {
        cap = (int)__atomic_load_n(&s_brightness_want, __ATOMIC_ACQUIRE);
    }
    return cap > 100 ? 100 : cap;
}

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
static uint32_t s_act_wall[ACT_MAX];  /* wall-clock s at the push; 0 = no valid clock */
static uint8_t  s_act_live;           /* bit i: s_act_ms[i] is from this boot */
static volatile uint32_t s_act_count;

/* THE RECEIPT OUTLIVES THE SLEEP. On battery the device deep-sleeps whenever
 * it is left alone, so a RAM-only list was empty nearly every time the owner
 * looked. A copy lives in RTC slow memory the loader never initialises
 * (RTC_NOINIT_ATTR): it survives deep sleep and a software reset, and holds
 * garbage after a cold power-on — hence the magic (version in the low byte),
 * the per-field bounds and the checksum, all checked before a row is
 * believed. Written on push only (rows are rare); no flash is touched. */
#define ACT_RTC_MAGIC   0x41435401u        /* "ACT" v1 */
#define ACT_WALL_VALID  1577836800u        /* 2020-01-01: main's own clock test */
typedef struct {
    uint32_t magic;
    uint32_t count;
    char     kind[ACT_MAX][ACT_KIND_CAP];
    char     sum[ACT_MAX][ACT_SUM_CAP];
    uint32_t wall[ACT_MAX];
    uint32_t check;
} act_rtc_t;
static RTC_NOINIT_ATTR act_rtc_t s_act_rtc;
static bool s_act_restored;               /* app task only */
static volatile uint32_t s_power_word = 0xFFu; /* pct | mv<<8 | usb/charge */
/* STATUS links: bit0 wifi, bit1 session open, bits3:2 tools (0 none,
 * 1 starting, 2 ready), bit4 desk live, bit5 radio saving, bits15:8 -dBm,
 * bits23:16 die temperature + 40 (0 = no reading), bits27:24 CPU MHz / 20. */
#define SP_CHIP_HOT_C  70
static volatile uint32_t s_links_word;
static char s_links_ip[16];
/* 0 running, 1 panel-off requested, 2 panel is off (render task only). */
static volatile uint32_t s_panel_off;
/* Low bits: running=0, off requested=1, off acknowledged=2, recovery=3.
 * Upper bits are a generation. Old callbacks cannot acknowledge new intent. */
static volatile uint32_t s_panel_on_ack_word;
static volatile uint32_t s_panel_on_ack_valid;
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
#define SP_WX_STALE_MIN 30 /* beyond this the accent loses its colour         */
#define SP_WX_AGE_MIN   2  /* below this the age is not worth a line          */
static char s_wx_head[SP_LABEL_CAP];  /* "OVERCAST 83" / "NO WEATHER"         */
static char s_wx_age[10];             /* "12M AGO" / "STALE 45M" / ""         */
static char s_wx_temp[6];             /* "83" / "-12" / ""                    */
static char s_wx_hilo[10];            /* "H86 L76" / ""                       */
static bool s_wx_valid;
static bool s_wx_stale;
static jr_display_sky_t s_wx_sky;
static int s_wx_icon = -1;       /* glass_icon_id_t of the sky, or -1: none */
/* THE NEXT TWELVE HOURS, one 30 degree slot each round the ring, clockwise
 * from 12 o'clock, slot 0 being now. */
#define SP_WX_HOURS      12
#define SP_WX_HOUR_NONE  255          /* not in the answer: the slot is dark   */
static uint8_t s_wx_hour_pct[SP_WX_HOURS];
static int     s_wx_hours;            /* known slots; 0 draws no ring          */
static char    s_wx_sun[12];          /* "7:02 19:35" / ""                     */

/* ACTIVITY, composed once per frame: one row per entry, "KIND SUMMARY", cut
 * to the row's width with the tree's own shortening mark. */
#define SP_ACT_ROW_GLYPHS 24
#define SP_ACT_ROW_CAP    (SP_ACT_ROW_GLYPHS + 1)
static char    s_act_row[ACT_MAX][SP_ACT_ROW_CAP];
#define SP_ACT_AGE_CAP 12
static char    s_act_age[ACT_MAX][SP_ACT_AGE_CAP];   /* "JUST NOW", "7M AGO", "EARLIER" */
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
static bool     watch_art_is_content(void);   /* a baked dial on WATCH is the screen */
static bool     s_space_on;
static uint8_t  s_detail_space;
static uint32_t s_space_hold_ms;

/* The entry clock of the screen that is becoming current: ms since its slide
 * began, set to 0 by sp_fade_tick when STATUS or WEATHER arrives and counted
 * up by the frame's dt until SP_ENTRY_TOTAL_MS, where it parks. At rest it is
 * parked, so a staged screen draws settled. The glass data screens read it;
 * the screen sliding OUT always draws settled. While it is running the shell
 * is asking for frames (see rest_eligible and quiet_eligible); after, none. */
#define SP_ENTRY_RING_MS   900U
#define SP_ENTRY_TOTAL_MS  900U
static uint32_t s_entry_ms = SP_ENTRY_TOTAL_MS;

/* The shade's own clocks. s_shade_ms is its entry (arcs sweep, the button
 * rises), started when the shade opens and parked at the total the moment it
 * closes. s_breath_ms is the muted button's slow breathing: it advances ONLY
 * while the shade is open and the microphone is muted, so a closed shade runs
 * no animation and the face is free to rest. */
#define SP_SHADE_ENTRY_MS 700U
#define SP_BREATH_MS      3600U
static uint32_t s_shade_ms = SP_SHADE_ENTRY_MS;
static uint32_t s_breath_ms;
static bool s_shade_was_open;

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

static uint32_t act_wall_now(void);
static uint32_t act_rtc_restore(void);

/* How long ago ACTIVITY row i happened, when that is known. A row from this
 * boot ages by esp_timer; a row carried over a sleep or a reset ages by the
 * wall clock, and only when both its stamp and now are real. Anything else
 * has no age — the sheet says EARLIER rather than invent one. */
static bool act_age_min(uint32_t i, uint32_t *out)
{
    if ((s_act_live & (1u << i)) != 0u) {
        *out = sp_age_min(s_act_ms[i]);
        return true;
    }
    const uint32_t now = act_wall_now();
    if (s_act_wall[i] == 0u || now == 0u || now < s_act_wall[i]) {
        return false;
    }
    *out = (now - s_act_wall[i]) / 60u;
    return true;
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
            uint32_t age_min = 0u;
            sp_str(s_detail_label[i], 0, SP_COL_MAX, s_act_kind[i]);
            if (!act_age_min(i, &age_min)) {
                sp_str(s_detail_value[i], 0, SP_COL_MAX, "EARLIER");
            } else if (age_min == 0u) {
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

/* WEATHER for this frame. Everything numeric is gated on valid: an unfetched
 * or failed weather prints NO number anywhere — not a zero, not a dash pair
 * shaped like one — and the headline says why. */
static int wx_cond_fit(char *dst, size_t cap, const char *cond, int room);

/* Which of the answer's hours is this one: the clock's hour on the fetch
 * day, +24 the day after (the stale gate keeps the answer within a day);
 * -1 when the clock or the date is unset, or the answer had no hours. */
static int sp_rain_now_idx(uint32_t clock_word, uint32_t date_word, int anchor_mday)
{
    if ((clock_word & (1u << 16)) == 0u || (date_word & (1u << 12)) == 0u ||
        anchor_mday <= 0) {
        return -1;
    }
    const int hh = (int)((clock_word >> 8) & 0xFFu);
    return (int)(date_word & 0x1Fu) == anchor_mday ? hh : hh + 24;
}

/* Slot k holds hour now+k: 0..100, or SP_WX_HOUR_NONE where the answer
 * has nothing (a gap, a value that is not a percentage, past its end).
 * Returns the known slots. */
static int sp_rain_hours(uint8_t out[SP_WX_HOURS], const uint8_t *pp, int n,
                         int now_idx)
{
    int known = 0;
    for (int k = 0; k < SP_WX_HOURS; ++k) {
        const int i = now_idx + k;
        const uint8_t v = (now_idx >= 0 && i < n) ? pp[i] : SP_WX_HOUR_NONE;
        out[k] = v <= 100u ? v : SP_WX_HOUR_NONE;
        known += out[k] != SP_WX_HOUR_NONE;
    }
    return known;
}

/* Minutes past midnight as H:MM, 24-hour. */
static int sp_hhmm(char *dst, int len, int cap, int minute)
{
    len = sp_num(dst, len, cap, (uint32_t)(minute / 60));
    len = sp_str(dst, len, cap, minute % 60 < 10 ? ":0" : ":");
    return sp_num(dst, len, cap, (uint32_t)(minute % 60));
}

/* After sunset or before sunrise, from the two words the shell already holds.
 * Without a sun or a clock it is day: the sun is the icon that is true of every
 * unknown. */
static bool sp_wx_is_night(void)
{
    const uint32_t sw = __atomic_load_n(&s_sun_word, __ATOMIC_ACQUIRE);
    const uint32_t cw = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
    if ((sw & (1u << 22)) == 0u || (cw & (1u << 16)) == 0u) {
        return false;
    }
    const int now = (int)((cw >> 8) & 0xFFu) * 60 + (int)(cw & 0xFFu);
    return now < (int)(sw & 0x7FFu) || now >= (int)((sw >> 11) & 0x7FFu);
}

/* The sky as an icon. Every sky the tools side can name is mapped; a sky it
 * could not name (UNKNOWN: windy, dust, a word it has no rule for) gets no icon
 * rather than a cloud that says something the answer did not. A partly cloudy
 * night is a plain cloud: the sun behind it would be wrong. */
static int sp_wx_icon_for(jr_display_sky_t sky, bool night)
{
    switch (sky) {
    case JR_DISPLAY_SKY_CLEAR:  return night ? GLASS_ICON_MOON : GLASS_ICON_SUN;
    case JR_DISPLAY_SKY_PARTLY: return night ? GLASS_ICON_CLOUD : GLASS_ICON_PARTLY;
    case JR_DISPLAY_SKY_CLOUDS: return GLASS_ICON_CLOUD;
    case JR_DISPLAY_SKY_FOG:    return GLASS_ICON_FOG;
    case JR_DISPLAY_SKY_RAIN:   return GLASS_ICON_RAIN;
    case JR_DISPLAY_SKY_STORM:  return GLASS_ICON_THUNDER;
    case JR_DISPLAY_SKY_SNOW:   return GLASS_ICON_SNOW;
    default:                    return -1;
    }
}

static void sp_compose_weather(void)
{
    const jr_display_weather_t *w =
        &s_weather[__atomic_load_n(&s_weather_slot, __ATOMIC_ACQUIRE) & 1u];
    s_wx_valid = w->valid;
    s_wx_sky = w->sky;
    s_wx_icon = w->valid ? sp_wx_icon_for(w->sky, sp_wx_is_night()) : -1;
    s_wx_age[0] = '\0';
    s_wx_temp[0] = '\0';
    s_wx_hilo[0] = '\0';
    s_wx_stale = false;
    s_wx_hours = 0;
    s_wx_sun[0] = '\0';
    if (!w->valid) {
        sp_str(s_wx_head, 0, SP_LABEL_CAP, "NO WEATHER");
        return;
    }

    /* Headline: the word alone, fitted at a word (wx_cond_fit), never
     * clipped mid-glyph. The number is the disc's — the panel read "76"
     * over "OVERCAST 76", one reading printed twice. */
    char cond[SP_LABEL_CAP];
    (void)wx_cond_fit(cond, sizeof cond, w->condition, SP_LABEL_CAP - 1);
    (void)sp_str(s_wx_head, 0, SP_LABEL_CAP, cond);

    (void)sp_int(s_wx_temp, 0, (int)sizeof s_wx_temp, w->temp_f);
    int len = sp_str(s_wx_hilo, 0, (int)sizeof s_wx_hilo, "H");
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

    /* The hours only while fresh: a stale answer's "now" is a guess. */
    if (!s_wx_stale) {
        const int now_idx = sp_rain_now_idx(
            __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE),
            __atomic_load_n(&s_clock_date_word, __ATOMIC_ACQUIRE),
            (int)w->rain_hours_mday);
        s_wx_hours = sp_rain_hours(s_wx_hour_pct, w->rain_hours,
                                   JR_DISPLAY_RAIN_HOURS, now_idx);
    }
    const uint32_t sw = __atomic_load_n(&s_sun_word, __ATOMIC_ACQUIRE);
    if ((sw & (1u << 22)) != 0u) {
        len = sp_hhmm(s_wx_sun, 0, (int)sizeof s_wx_sun, (int)(sw & 0x7FFu));
        len = sp_str(s_wx_sun, len, (int)sizeof s_wx_sun, " ");
        (void)sp_hhmm(s_wx_sun, len, (int)sizeof s_wx_sun, (int)((sw >> 11) & 0x7FFu));
    }
}

/* Fit a spoken line into an ACTIVITY row. Whole, when it fits. Otherwise the
 * last complete sentence that fits, ending on its own stop with nothing after
 * it: a cut that appends "." to a fragment leaves "FORGIVE ME, SIR. I." and
 * reads like an ending. Failing that, the last whole word with "..." (no font
 * here has U+2026), never a bare ".". `cap` counts the terminator. */
size_t jr_display_activity_fit(char *dst, size_t cap, const char *src)
{
    if (cap == 0U) {
        return 0U;
    }
    const size_t max = cap - 1U;
    while (*src == ' ') {
        src++;
    }
    const size_t n = strlen(src);
    if (n <= max) {
        memcpy(dst, src, n);
        dst[n] = '\0';
        return n;
    }
    if (max < 4U) {
        memcpy(dst, src, max);
        dst[max] = '\0';
        return max;
    }
    size_t end = 0U;
    for (size_t i = 0U; i < max; i++) {
        if ((src[i] == '.' || src[i] == '!' || src[i] == '?') &&
            (src[i + 1U] == ' ' || src[i + 1U] == '\0')) {
            end = i + 1U;
        }
    }
    if (end >= 4U) {
        memcpy(dst, src, end);
        dst[end] = '\0';
        return end;
    }
    size_t keep = max - 3U;
    size_t cut = keep;
    while (cut > 0U && src[cut] != ' ') {
        cut--;
    }
    if (cut >= max / 2U) {
        keep = cut;
    }
    while (keep > 0U && (src[keep - 1U] == ' ' || src[keep - 1U] == ',' ||
                         src[keep - 1U] == ';' || src[keep - 1U] == ':' ||
                         src[keep - 1U] == '-')) {
        keep--;
    }
    memcpy(dst, src, keep);
    memcpy(dst + keep, "...", 4);
    return keep + 3U;
}

/* ACTIVITY rows for this frame. s_act_row[i] is the summary alone, cut to the
 * row's 24 glyphs by jr_display_activity_fit() when it does not fit. s_act_age[i] is the age
 * under the sheet's own truth rule: a live row ages by esp_timer, a carried one
 * by the wall clock, anything else says EARLIER rather than invent a number. */
static void sp_compose_activity(void)
{
    uint32_t n = __atomic_load_n(&s_act_count, __ATOMIC_ACQUIRE);
    if (n > (uint32_t)ACT_MAX) {
        n = (uint32_t)ACT_MAX;
    }
    s_act_rows = (int)n;
    for (uint32_t i = 0; i < n; ++i) {
        char *row = s_act_row[i];
        row[0] = '\0';
        const int slen = sp_len(s_act_sum[i], ACT_SUM_CAP - 1);
        if (slen < SP_ACT_ROW_GLYPHS) {   /* a full store may have been cut by the push: mark it */
            (void)sp_str(row, 0, SP_ACT_ROW_CAP, s_act_sum[i]);
        } else {
            (void)jr_display_activity_fit(row, SP_ACT_ROW_GLYPHS, s_act_sum[i]);
        }
        uint32_t age_min = 0u;
        if (!act_age_min(i, &age_min)) {
            (void)sp_str(s_act_age[i], 0, SP_ACT_AGE_CAP, "EARLIER");
        } else if (age_min == 0u) {
            (void)sp_str(s_act_age[i], 0, SP_ACT_AGE_CAP, "JUST NOW");
        } else {
            (void)sp_str(s_act_age[i],
                         sp_age(s_act_age[i], 0, SP_ACT_AGE_CAP, age_min),
                         SP_ACT_AGE_CAP, " AGO");
        }
    }
}

/* One composition pass per frame: the shade readouts, the two live-data
 * screens, and the detail sheet if it is showing. */
static void sp_compose(void)
{
    /* The weather and activity strings are read only while their screen is on
     * the glass (or sliding to or from it), so every other screen skips them:
     * a settled STATUS used to format both every frame for nothing. */
    if (s_space_from == (uint8_t)JR_DISPLAY_SPACE_WEATHER ||
        s_space_to == (uint8_t)JR_DISPLAY_SPACE_WEATHER) {
        sp_compose_weather();
    }
    if (s_space_from == (uint8_t)JR_DISPLAY_SPACE_ACTIVITY ||
        s_space_to == (uint8_t)JR_DISPLAY_SPACE_ACTIVITY) {
        sp_compose_activity();
    }
    const uint32_t w = __atomic_load_n(&s_status_word, __ATOMIC_ACQUIRE);
    const uint32_t vol = (w & 0xFFu) > 100u ? 100u : (w & 0xFFu);
    const uint32_t brt = (uint32_t)shade_light(w);

    /* BOTH NUMBERS MUST SURVIVE AT 100: the store holds 10 glyphs and "100%"
     * is 4. The icons beside the arcs say which is which. */
    (void)sp_pct(s_shade_vol, 0, SP_COL_MAX, vol);
    (void)sp_pct(s_shade_light, 0, SP_COL_MAX, brt);

    if (s_detail_ease > 0) {
        sp_compose_detail(s_detail_space);
    }
}

/* Which screens sit on true black with the face parked. JARVIS is the face.
 * WATCH stays out: its baked dials ARE the backdrop, its other styles share
 * the DREAM / nightstand rest path with JARVIS, and both keep today's veil. */
static bool sp_space_is_black(uint8_t space)
{
    return space != (uint8_t)JR_DISPLAY_SPACE_JARVIS &&
           space != (uint8_t)JR_DISPLAY_SPACE_WATCH;
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
        s_entry_ms = (space == (uint8_t)JR_DISPLAY_SPACE_STATUS ||
                      space == (uint8_t)JR_DISPLAY_SPACE_WEATHER ||
                      space == (uint8_t)JR_DISPLAY_SPACE_DESK ||
                      space == (uint8_t)JR_DISPLAY_SPACE_ACTIVITY)
                         ? 0U : SP_ENTRY_TOTAL_MS;
    } else {
        if (s_entry_ms < SP_ENTRY_TOTAL_MS) {
            s_entry_ms += dt_ms;
            if (s_entry_ms > SP_ENTRY_TOTAL_MS) {
                s_entry_ms = SP_ENTRY_TOTAL_MS;
            }
        }
        if (s_space_to != space) {
            s_space_to = space;      /* serial wrap: snap rather than lie */
            s_space_from = space;
        }
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
        if (!s_shade_was_open) {
            s_shade_was_open = true;
            s_shade_ms = 0U;
            s_breath_ms = 0U;
        } else if (s_shade_ms < SP_SHADE_ENTRY_MS) {
            s_shade_ms += dt_ms;
            if (s_shade_ms > SP_SHADE_ENTRY_MS) {
                s_shade_ms = SP_SHADE_ENTRY_MS;
            }
        }
        if (sp_privacy_muted()) {
            s_breath_ms = (s_breath_ms + dt_ms) % SP_BREATH_MS;
        }
    } else {
        s_shade_prog -= cstep;
        if (s_shade_prog < 0) {
            s_shade_prog = 0;
        }
        s_shade_was_open = false;
        s_shade_ms = SP_SHADE_ENTRY_MS;
    }
    s_shade_ease = fade_smoothstep(s_shade_prog);

    /* The backdrop veil is the deepest any live element asks for. Because a
     * slide out of JARVIS still carries the outgoing space's weight, going
     * home fades the veil away with the slide instead of cutting it a frame
     * early. */
    int veil = 0;
    const bool to_black = sp_space_is_black(s_space_to);
    const bool from_black = sp_space_is_black(s_space_from);
    if (s_space_to != (uint8_t)JR_DISPLAY_SPACE_JARVIS && !to_black &&
        !watch_art_is_content()) {
        veil = s_space_ease;     /* WATCH with its dial: the art is the screen */
    }
    if (s_space_from != (uint8_t)JR_DISPLAY_SPACE_JARVIS && !from_black &&
        256 - s_space_ease > veil) {
        veil = 256 - s_space_ease;
    }
    /* A data screen takes the backdrop all the way to black rather than to a
     * quarter. Between two data screens it stays black for the whole slide
     * (the larger of the two eases would dip to half at the midpoint and let
     * the face flash through); going home it fades back in with the slide. */
    int black = 0;
    if (to_black) {
        black = from_black ? 256 : s_space_ease;
    } else if (from_black) {
        black = 256 - s_space_ease;
    }
    s_space_black = black;
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
    s_space_on = veil > 0 || black > 0 || s_space_prog < 256 ||
                 (int32_t)(now_ms - s_space_hold_ms) < 0;

    /* Park the face only when the black is complete on a data screen, the
     * slide has settled, and nothing else is drawing over the backdrop (a
     * canvas, a surface, the ask dim, a test pattern). Any of those coming up
     * withdraws the verdict and the presenter brings the face back. */
    const bool park = to_black && black >= 256 &&
                      (from_black || s_space_prog >= 256) &&
                      s_canvas_ease == 0 && s_ask_ease == 0 &&
                      !jr_display_canvas_active() &&
                      !jr_display_surface_is_active() &&
                      diag_load(&s_display.test_pattern) == JR_DISPLAY_TEST_OFF;
    if (__atomic_exchange_n(&s_park_want, park ? 1 : 0, __ATOMIC_ACQ_REL) !=
            (park ? 1 : 0)) {
        TaskHandle_t presenter = __atomic_load_n(&s_display.task, __ATOMIC_ACQUIRE);
        if (presenter != NULL) {
            xTaskNotifyGive(presenter);   /* the face must not wait a period */
        }
    }
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
    uint32_t cur = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    for (;;) {
        const uint32_t next = word | (cur & (JR_DISPLAY_ENV_REST_MASK | JR_DISPLAY_ENV_VOICE_BUSY));
        if (next == cur) {
            return;
        }
        if (__atomic_compare_exchange_n(&s_hud_env_word, &cur, next, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            /* Tilt stays current for WATCH but does not move the DREAM rim. */
            if (!(next & JR_DISPLAY_ENV_MODE_MASK) ||
                ((next ^ cur) & JR_DISPLAY_ENV_RIM_MASK)) {
                nav_wake();
            }
            return;
        }
    }
}

void jr_display_rest_set(jr_display_rest_mode_t mode, bool usb_present, bool night)
{
    if ((unsigned)mode > JR_DISPLAY_REST_GLANCE) { mode = JR_DISPLAY_REST_NONE; }
    const uint32_t bits = mode == JR_DISPLAY_REST_NONE ? 0U :
        ((uint32_t)mode << 10) | (usb_present ? JR_DISPLAY_ENV_USB : 0U) |
        (night && mode == JR_DISPLAY_REST_NIGHTSTAND && usb_present ?
            JR_DISPLAY_ENV_NIGHT : 0U);
    uint32_t cur = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    for (;;) {
        const uint32_t next = (cur & ~JR_DISPLAY_ENV_REST_MASK) | bits;
        if (next == cur) {
            return;
        }
        if (__atomic_compare_exchange_n(&s_hud_env_word, &cur, next, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            nav_wake();
            return;
        }
    }
}

void jr_display_dream_set(bool enabled)
{
    jr_display_rest_set(enabled ? JR_DISPLAY_REST_DREAM : JR_DISPLAY_REST_NONE,
                        false, false);
}

void jr_display_rest_voice_busy_set(bool busy)
{
    const uint32_t previous = busy ?
        __atomic_fetch_or(&s_hud_env_word, JR_DISPLAY_ENV_VOICE_BUSY, __ATOMIC_ACQ_REL) :
        __atomic_fetch_and(&s_hud_env_word, ~JR_DISPLAY_ENV_VOICE_BUSY, __ATOMIC_ACQ_REL);
    if (((previous & JR_DISPLAY_ENV_VOICE_BUSY) != 0U) != busy) { nav_wake(); }
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
    case JR_FACE_DIAL_DIVER:
    case JR_FACE_DIAL_DRESS:
    case JR_FACE_DIAL_PILOT:
    case JR_FACE_DIAL_FUTURE:
    case JR_FACE_DIAL_SKELETON:
    case JR_FACE_DIAL_CELESTIAL: return HUD_FACE_IDLE;
    default:                return HUD_FACE_IDLE;
    }
}

/* Full-strip backdrop dim shared by the ask overlay (STATE-07) and the clock
 * (UI-01). hud_dim565 folds the panel_order_color -> darken (>>2 per channel) ->
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

/* The two text colours the caption and the shell share. */
#define SP_C_INK       0xF79E   /* cool white: never pure, never clinical */
#define SP_C_GREY      0x8C71

/* STATE-04: the caption chip, now drawn in the glass type. The label is
 * GLASS_FONT_LABEL tracked +3, grey for furniture (the idle hints and the
 * screen names a swipe prints) and ink for a message, fading in and out on
 * s_caption_ease as it always did.
 *
 * THE BAND. The old chip dimmed a round-safe band (y360-430) behind its text.
 * Over a face or a watch dial that is still what keeps the letters readable, so
 * it stays there. On a data screen, or under the shade, the backdrop is
 * already black or veiled and the band only dimmed the screen's own content (on
 * STATUS the stats row ends at y382). Its weight therefore follows how little
 * of the face is left: it is the old weight times (256 - max(black, shade)) / 256,
 * so it fades out with the slide that blackens the backdrop and is gone, with
 * no pixel touched, once the screen is black. Nineteen characters per line
 * (hud_wrap2) is about 230 px of label, inside the glass chord at these rows. */
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
    const int e = s_caption_ease;
    if (((e * 32) >> 8) <= 0) {
        return;
    }
    if (s_caption_lease && (s_shade_ease > 0 || s_detail_ease > 0)) {
        return;                    /* the sheet's own hint owns this band */
    }
    int face_left = 256 - (s_space_black > s_shade_ease ? s_space_black : s_shade_ease);
    if (face_left < 0) {
        face_left = 0;
    }
    const int k = (((e * 32) >> 8) * face_left) >> 8;
    const bool swap = ctx->board.swap_color_bytes;
    const int c = e > 255 ? 255 : e;
    const uint16_t color = s_caption_grey ? SP_C_GREY : SP_C_INK;
    const glass_metrics_t m = glass_font_metrics(GLASS_FONT_LABEL);
    int tx[2], len[2];
    for (int l = 0; l < 2; ++l) {
        len[l] = (int)strnlen(s_caption_line[l], 19U);
        tx[l] = (HUD_W - glass_text_width(GLASS_FONT_LABEL, s_caption_line[l],
                                          (size_t)len[l], JR_CAPTION_TRACK)) / 2;
    }
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
        } else if (k > 0) {
            for (int x = xlo; x <= xhi; ++x) {
                row[x] = hud_fade565(row[x], swap, k);
            }
        }
        for (int l = 0; l < 2; ++l) {
            const int ty = s_caption_y[l];
            if (len[l] <= 0 || y < ty + m.ink_top || y > ty + m.ink_bottom) {
                continue;
            }
            glass_text_row(row, y, xlo, xhi, GLASS_FONT_LABEL, s_caption_line[l],
                           (size_t)len[l], tx[l], ty, JR_CAPTION_TRACK, color,
                           (uint8_t)c, swap);
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
static void sp_text_row(uint16_t *row, int y, const char *text, int len,
                        int x0, int y0, int scale, uint16_t px);
static void sp_dot_row(uint16_t *row, int y, int cx, int cy, int n, uint16_t px);
static uint16_t sp_tint(const jr_display_ctx_t *ctx, uint16_t native, int st);
static bool sp_clip(int y, int *xlo, int *xhi);
static int sp_wifi_bars(uint32_t lk);
static bool sp_privacy_muted(void);
static void watch_sun_arc(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int style, int st, int hh, int mm);

/* WHICH DIAL, IF ANY. On WATCH with the clock published and a style whose
 * dial is baked art, the face under the hands is that dial. JARVIS keeps its
 * black disc and MINIMAL draws its own dial, so both answer JR_FACE_COUNT. */
static jr_face_t watch_dial_face(void)
{
    const uint32_t nav = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    if ((nav & NAV_SPACE_MASK) != (uint32_t)JR_DISPLAY_SPACE_WATCH) {
        return JR_FACE_COUNT;
    }
    if ((__atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE) & (1u << 16)) == 0u) {
        return JR_FACE_COUNT;
    }
    switch ((nav & NAV_STYLE_MASK) >> NAV_STYLE_SHIFT) {
    case JR_WATCH_DIVER:  return JR_FACE_DIAL_DIVER;
    case JR_WATCH_DRESS:  return JR_FACE_DIAL_DRESS;
    case JR_WATCH_PILOT:  return JR_FACE_DIAL_PILOT;
    case JR_WATCH_FUTURE: return JR_FACE_DIAL_FUTURE;
    case JR_WATCH_SKELETON:  return JR_FACE_DIAL_SKELETON;
    case JR_WATCH_CELESTIAL: return JR_FACE_DIAL_CELESTIAL;
    default:              return JR_FACE_COUNT;
    }
}

/* THE WORDS ON THE WATCH. DIVER's date window and FUTURE's four cells are
 * the only text a dial carries, drawn with the shell's own glyphs after the
 * hands so the type matches the ring. Every value is one the display already
 * holds — the clock and date words, the weather slot, the power and links
 * words — and an absent value leaves its cell blank or dim, never invented.
 *
 * Geometry comes from the HUD_WATCH_* table in hud_render.h — the one place
 * the art's measured boxes live — so this file and the stand-in dial agree. */
#define WX_STALE_MIN 120u

/* FUTURE's weather line, shortened rather than cut. The cell is 193 px:
 * 15 glyphs at scale 2, and "75* " plus a 13-glyph condition is 17, so a
 * hard clip showed "75* LIGHT DRIZZ" on the panel (2026-09-05). A condition
 * that does not fit its room first loses its leading qualifier (light
 * drizzle is still drizzle); if that is not enough it is cut at the last
 * space that leaves a word, else mid-word, with the last glyph spent on the
 * "." mark, so a reader knows the word went on. The WEATHER headline fits
 * its twelve glyphs by the same rule. Returns the length written. */
static int wx_cond_fit(char *dst, size_t cap, const char *cond, int room)
{
    static const char *const QUAL[] = {
        "LIGHT ", "HEAVY ", "MODERATE ", "PARTLY ", "SLIGHT ", "MOSTLY ",
    };
    if (room < 2 || (size_t)room >= cap) {
        room = (int)cap - 1 < 2 ? 2 : (int)cap - 1;
    }
    int n = (int)strlen(cond);
    if (n > room) {
        for (size_t i = 0; i < sizeof QUAL / sizeof *QUAL; ++i) {
            const size_t ql = strlen(QUAL[i]);
            if (strncmp(cond, QUAL[i], ql) == 0 && cond[ql] != '\0') {
                cond += ql;
                n = (int)strlen(cond);
                break;
            }
        }
    }
    if (n <= room) {
        memcpy(dst, cond, (size_t)n + 1);
        return n;
    }
    int keep = room - 1;                        /* one glyph for the mark */
    int cut = keep;
    while (cut > 0 && cond[cut] != ' ') {
        cut--;
    }
    if (cut >= 4) {                             /* what is left is a word */
        keep = cut;
    }
    memcpy(dst, cond, (size_t)keep);
    dst[keep] = '.';
    dst[keep + 1] = '\0';
    return keep + 1;
}

#define WATCH_WX_GLYPHS 15

static int watch_wx_line(char *dst, size_t cap, int temp_f, const char *cond)
{
    int n = snprintf(dst, cap, "%d* ", temp_f);
    if (n < 0 || (size_t)n + 2 >= cap) {
        dst[0] = '\0';
        return 0;
    }
    return n + wx_cond_fit(dst + n, cap - (size_t)n, cond, WATCH_WX_GLYPHS - n);
}

/* THE WORDS, ONCE PER FRAME. watch_cells used to compose every string —
 * four snprintfs, three atomic word loads, the weather slot — in every one
 * of the 39 strips, though nothing in them changes between strips.
 * watch_compose runs at the frame start (overlay_fade_tick, the same latch
 * that fixes the sweep phase) and leaves the strings, their lengths and
 * their x origins here; the strips only place glyphs. Render task only. */
typedef struct {
    bool have_date;
    char day[3];                 /* DIVER's window: the day of the month */
    char date[20];               /* FUTURE, above: "TUE 02 SEP"          */
    int  date_n, date_x0;
    bool wx_ok, wx_stale;
    char wx1[24];                /* FUTURE, below: "75* DRIZZLE"          */
    int  wx1_n, wx1_x0;
    char wx2[12];                /* "82 / 68"                             */
    int  wx2_n, wx2_x0;
    int  pct;                    /* battery, or >100 for no answer        */
    char bat[6];
    int  bat_n;
    int  bars;                   /* Wi-Fi bars 0..4                       */
    const char *link;            /* "LIVE" / "IDLE" / "MUTED"             */
    int  link_n;
} watch_words_t;

static watch_words_t s_watch_words;

static void watch_compose(void)
{
    static const char *const WD[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    static const char *const MO[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    watch_words_t *w = &s_watch_words;
    if ((__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) & NAV_SPACE_MASK) !=
        (uint32_t)JR_DISPLAY_SPACE_WATCH) {
        return;                      /* only the watch carries these words */
    }
    const uint32_t dw = __atomic_load_n(&s_clock_date_word, __ATOMIC_ACQUIRE);
    const int mday = (int)(dw & 0x1Fu), mon = (int)((dw >> 5) & 0xFu), wday = (int)((dw >> 9) & 0x7u);
    w->have_date = (dw & (1u << 12)) != 0u;
    w->day[0] = (char)('0' + mday / 10);
    w->day[1] = (char)('0' + mday % 10);
    w->day[2] = '\0';
    if (w->have_date) {
        w->date_n = snprintf(w->date, sizeof w->date, "%s %02d %s", WD[wday], mday, MO[mon]);
    } else {
        w->date_n = snprintf(w->date, sizeof w->date, "NO DATE");
    }
    w->date_x0 = HUD_WATCH_CELL_DATE_CX - (6 * 2 * w->date_n) / 2;

    const jr_display_weather_t *wx =
        &s_weather[__atomic_load_n(&s_weather_slot, __ATOMIC_ACQUIRE) & 1u];
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    w->wx_ok = wx->valid;
    w->wx_stale = w->wx_ok && (now - wx->fetched_ms) / 60000u >= WX_STALE_MIN;
    if (w->wx_ok) {
        w->wx1_n = watch_wx_line(w->wx1, sizeof w->wx1, (int)wx->temp_f, wx->condition);
        w->wx2_n = snprintf(w->wx2, sizeof w->wx2, "%d / %d", (int)wx->hi_f, (int)wx->lo_f);
    } else {
        w->wx1_n = snprintf(w->wx1, sizeof w->wx1, "NO WEATHER");
        w->wx2_n = 0;
    }
    w->wx1_x0 = HUD_WATCH_CELL_WX_CX - (6 * 2 * w->wx1_n) / 2;
    w->wx2_x0 = HUD_WATCH_CELL_WX_CX - (6 * 2 * w->wx2_n) / 2;

    const uint32_t pw = __atomic_load_n(&s_power_word, __ATOMIC_ACQUIRE);
    const uint32_t lk = __atomic_load_n(&s_links_word, __ATOMIC_ACQUIRE);
    w->pct = (int)(pw & 0xFFu);
    w->bat_n = w->pct <= 100 ? snprintf(w->bat, sizeof w->bat, "%d%%", w->pct)
                             : snprintf(w->bat, sizeof w->bat, "--");
    w->bars = sp_wifi_bars(lk);
    w->link = sp_privacy_muted() ? "MUTED" : ((lk & 2u) != 0u ? "LIVE" : "IDLE");
    w->link_n = (int)strlen(w->link);
}

static void watch_cells(const jr_display_ctx_t *ctx, int y1, int y2,
                        uint16_t *pixels, int style, int st, bool baked)
{
    const watch_words_t *w = &s_watch_words;

    if (style == JR_WATCH_DIVER) {
        if (!w->have_date) {
            return;
        }
        const uint16_t ink = sp_tint(ctx, 0x0000, 255);
        const uint16_t paper = sp_tint(ctx, 0xFFFF, st);
        for (int y = y1; y < y2; ++y) {
            uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
            if (!baked && y >= HUD_WATCH_DATE_Y0 && y < HUD_WATCH_DATE_Y1) {
                int lo = HUD_WATCH_DATE_X0, hi = HUD_WATCH_DATE_X1 - 1;
                if (sp_clip(y, &lo, &hi)) {
                    for (int x = lo; x <= hi; ++x) {
                        row[x] = paper;
                    }
                }
            }
            if (st >= 128) {
                sp_text_row(row, y, w->day, 2,
                            (HUD_WATCH_DATE_X0 + HUD_WATCH_DATE_X1) / 2 - 11,
                            (HUD_WATCH_DATE_Y0 + HUD_WATCH_DATE_Y1) / 2 - 7, 2, ink);
            }
        }
        return;
    }
    if (style != JR_WATCH_FUTURE || st < 128) {
        return;
    }
    const uint16_t cyan = sp_tint(ctx, 0x073F, st);
    const uint16_t dim  = sp_tint(ctx, 0x073F, st / 3);
    const uint16_t white = sp_tint(ctx, 0xFFFF, st);
    const uint16_t wxpx = (w->wx_ok && !w->wx_stale) ? cyan : dim;
    const int pct = w->pct;

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        sp_text_row(row, y, w->date, w->date_n, w->date_x0, HUD_WATCH_CELL_DATE_CY - 7, 2,
                    w->have_date ? cyan : dim);
        sp_text_row(row, y, w->wx1, w->wx1_n, w->wx1_x0, HUD_WATCH_CELL_WX_CY - 16, 2, wxpx);
        if (w->wx2_n > 0) {
            sp_text_row(row, y, w->wx2, w->wx2_n, w->wx2_x0, HUD_WATCH_CELL_WX_CY + 2, 2, wxpx);
        }
        /* battery: ten dots in a shallow arc over the number */
        if (y >= HUD_WATCH_CELL_BAT_CY - 16 && y <= HUD_WATCH_CELL_BAT_CY - 6) {
            for (int i = 0; i < 10; ++i) {
                const int cx = HUD_WATCH_CELL_BAT_CX - 22 + i * 5;
                const int cy = HUD_WATCH_CELL_BAT_CY - 8 - ((i < 5 ? i : 9 - i) * 3) / 2;
                const bool lit = pct <= 100 && pct >= (i + 1) * 10 - 5;
                if (y >= cy && y < cy + 3) {
                    sp_dot_row(row, y, cx, cy, 3, lit ? cyan : dim);
                }
            }
        }
        sp_text_row(row, y, w->bat, w->bat_n, HUD_WATCH_CELL_BAT_CX - (6 * 2 * w->bat_n) / 2,
                    HUD_WATCH_CELL_BAT_CY, 2, pct <= 100 ? white : dim);
        /* link: four bars, then the word */
        for (int i = 0; i < 4; ++i) {
            const int h = 4 + i * 3, x0 = HUD_WATCH_CELL_LINK_CX - 13 + i * 6;
            if (y >= HUD_WATCH_CELL_LINK_CY - h && y < HUD_WATCH_CELL_LINK_CY) {
                int lo = x0, hi = x0 + 3;
                if (sp_clip(y, &lo, &hi)) {
                    for (int x = lo; x <= hi; ++x) {
                        row[x] = i < w->bars ? cyan : dim;
                    }
                }
            }
        }
        sp_text_row(row, y, w->link, w->link_n, HUD_WATCH_CELL_LINK_CX - (6 * w->link_n) / 2,
                    HUD_WATCH_CELL_LINK_CY + 6, 1, white);
    }
}

/* Split timers: where the hud layer's time goes, microseconds summed over a
 * frame's strips like layer_us. Written by the render task only, read by httpd
 * (relaxed; a torn pair across two counters is harmless for a gauge). The
 * space_* and clock_* entries are inside hud_space and hud_clock, not beside
 * them. One timer read per part per strip, never per row. */
static volatile uint32_t s_split_us[JR_DISPLAY_SPLIT_COUNT];

static const char *const k_split_names[JR_DISPLAY_SPLIT_COUNT] = {
    "hud_frame", "hud_space", "hud_clock", "hud_ota", "hud_caption", "hud_ask",
    "hud_bloom", "space_veil", "space_screen", "space_sheet", "space_shade",
    "space_orbit", "clock_clear", "clock_face", "clock_cells",
};

const char *jr_display_split_name(unsigned index)
{
    return index < (unsigned)JR_DISPLAY_SPLIT_COUNT ? k_split_names[index] : NULL;
}

#define JR_SPLIT_ARM_MS 8000u

/* httpd: the next few seconds of frames are timed. */
void jr_display_split_arm(void)
{
    __atomic_store_n(&s_split_deadline_ms,
                     (uint32_t)(esp_timer_get_time() / 1000) + JR_SPLIT_ARM_MS,
                     __ATOMIC_RELAXED);
}

/* The mark a part starts from: the clock while timing, else nothing read. */
static inline int64_t split_now(void)
{
    return s_split_on ? esp_timer_get_time() : 0;
}

/* Charge the time since *mark to one split and move the mark. Unarmed it is one
 * predictable branch. */
static inline void split_mark(jr_display_split_t which, int64_t *mark)
{
    if (!s_split_on) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    __atomic_add_fetch(&s_split_us[which], (uint32_t)(now - *mark),
                       __ATOMIC_RELAXED);
    *mark = now;
}

static void apply_clock_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                                uint16_t *pixels)
{
    const int e = s_clock_ease;
    if (e <= 0) {
        return;
    }
    int64_t sm = split_now();
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
    /* THE DIAL, BAKED OR NOT. A style with baked art shows it as the face
     * under the hands (watch_dial_face), so the disc is NOT cleared: the
     * hands blend over the art. When the partition lacks that clip the face
     * pipeline fell back to RESTING, ctx->shown_face says so, and the watch
     * clears the disc and draws its black stand-in dial as before. JARVIS
     * and MINIMAL never have art; MINIMAL paints its own warm-white dial. */
    const int style = (int)jr_display_watch_style();
    const jr_face_t dial = watch_dial_face();
    const bool baked = dial != JR_FACE_COUNT && ctx->shown_face == dial;
    if (!baked) {
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
    }

    split_mark(JR_DISPLAY_SPLIT_CLOCK_CLEAR, &sm);
    const uint32_t w = s_clock_shown_word;
    const int strength = e > 255 ? 255 : e;
    if (style == (int)JR_WATCH_JARVIS) {
        hud_overlay_clock(pixels, y1, y2 - y1, ctx->board.swap_color_bytes,
                          (int)((w >> 8) & 0xFFu), (int)(w & 0xFFu),
                          (int)((w >> 17) & 0x3Fu), strength);
        split_mark(JR_DISPLAY_SPLIT_CLOCK_FACE, &sm);
        return;
    }
    const uint32_t pw = __atomic_load_n(&s_power_word, __ATOMIC_ACQUIRE);
    const jr_display_weather_t *wx =
        &s_weather[__atomic_load_n(&s_weather_slot, __ATOMIC_ACQUIRE) & 1u];
    const bool awake = jr_display_render_fps() >= JR_DISPLAY_RENDER_FPS;
    const hud_watch_t hw = {
        .hh = (int)((w >> 8) & 0xFFu),
        .mm = (int)(w & 0xFFu),
        .ss = (int)((w >> 17) & 0x3Fu),
        .ms = s_clock_phase_ms,
        .batt_pct = (pw & 0xFFu) <= 100u ? (int)(pw & 0xFFu) : -1,
        .wx_valid = wx->valid,
        .temp_f = wx->temp_f,
        .dial_baked = baked,
        /* shadows at the awake cadence only: the ladder's slower frames are
         * a resting watch nobody is studying, and the shadow is the widest
         * blend a hand carries */
        .shadow = awake,
        .alive = awake,         /* SKELETON's wheel and CELESTIAL's stars rest with the shadows */
        .light = &s_light,
    };
    watch_sun_arc(ctx, y1, y2, pixels, style, strength, hw.hh, hw.mm);
    split_mark(JR_DISPLAY_SPLIT_CLOCK_CELLS, &sm);
    hud_overlay_watch(pixels, y1, y2 - y1, ctx->board.swap_color_bytes, &hw,
                      strength, style);
    split_mark(JR_DISPLAY_SPLIT_CLOCK_FACE, &sm);
    watch_cells(ctx, y1, y2, pixels, style, strength, baked);
    split_mark(JR_DISPLAY_SPLIT_CLOCK_CELLS, &sm);
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
/* The rail is a 4 px glass track down the middle of the band (it was 8 px of
 * opaque annulus): the sheet of pixels it writes every strip of every frame
 * is the biggest single cost of a settled screen, and the mark and the dots
 * stand proud of it. Anti-aliasing adds one faint pixel each side. */
#define SP_ORB_TRACK_IN  188
#define SP_ORB_TRACK_OUT 192
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

/* THE CONTROL SHADE. Volume is an arc on the left rim and brightness one on the
 * right, each +-40 degrees about 9 and 3 o'clock, with the speaker or the sun
 * and the percentage inside it; the centre is the mute button. Drawn by
 * sp_draw_shade with the glass primitives. Brightness stays display-only: it
 * has no touch action, as before.
 *
 * TOUCH. The mute button's target is its drawn disc (SP_BTN_R at SP_BTN_CY).
 * The volume target is the left rim, SP_VOL_HIT_IN out to the shell clip: the
 * upper half is volume UP and the lower half DOWN, the way the arc fills (from
 * the bottom end up). The old target was the top half of a ring at r124-160,
 * left half down, right half up, matching a -/+ pair that no longer exists;
 * the right rim is the brightness arc now and a tap there closes the shade like
 * any other tap outside a control. Both moved together with
 * test_shade_hits_resolve_to_controls. */
#define SP_BTN_CY      206
#define SP_BTN_R       62
#define SP_BTN_PRIV_CX SP_CX

#define SP_VOL_HIT_IN  156
#define SP_VOL_HIT_DY  130

#define SP_C_CYAN      0x07FF
#define SP_C_CYAN_DIM  0x0330
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
#define SP_C_RAIN      0x049F   /* blue-cyan: the rain hours                */
#define SP_C_SNOW      SP_C_INK

/* WEATHER: an hour under SP_WX_HOUR_DRY percent is a faint tick, not a stroke. */
#define SP_WX_HOUR_DRY  10     /* under this an hour is bare track           */

/* ACTIVITY geometry: up to three two-line blocks (kind and age in the label
 * face, the summary in the text face), 62 px apart, centred about the axis.
 * The worst corner is a 24-glyph summary at the outer block, inside the shell
 * clip and clear of the update ring at r140-154 only while no update runs. */
#define SP_ACT_PITCH   62
#define SP_ACT_Y0      (SP_CY - SP_ACT_PITCH - 17)   /* top of the first block's label line */
#define SP_ACT_SUM_DY  20                             /* summary line below the label line */
#define SP_ACT_GAP     12                             /* kind to age */

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

/* sp_isqrt(SP_R_SHELL^2 - dy^2) for dy = 0..SP_R_SHELL-1: every row of every
 * shell screen asks for it, and the shift-and-subtract root cost 250
 * instructions a row (0.1 M a frame). The frame-identity suite pins the table
 * against the root. */
#define SP_CLIP_ROWS 214
static const uint8_t k_clip_half[SP_CLIP_ROWS] = {
    214, 213, 213, 213, 213, 213, 213, 213, 213, 213, 213, 213, 213, 213, 213, 213,
    213, 213, 213, 213, 213, 212, 212, 212, 212, 212, 212, 212, 212, 212, 211, 211,
    211, 211, 211, 211, 210, 210, 210, 210, 210, 210, 209, 209, 209, 209, 208, 208,
    208, 208, 208, 207, 207, 207, 207, 206, 206, 206, 205, 205, 205, 205, 204, 204,
    204, 203, 203, 203, 202, 202, 202, 201, 201, 201, 200, 200, 200, 199, 199, 198,
    198, 198, 197, 197, 196, 196, 195, 195, 195, 194, 194, 193, 193, 192, 192, 191,
    191, 190, 190, 189, 189, 188, 188, 187, 187, 186, 185, 185, 184, 184, 183, 182,
    182, 181, 181, 180, 179, 179, 178, 177, 177, 176, 175, 175, 174, 173, 172, 172,
    171, 170, 169, 169, 168, 167, 166, 166, 165, 164, 163, 162, 161, 160, 160, 159,
    158, 157, 156, 155, 154, 153, 152, 151, 150, 149, 148, 147, 146, 145, 144, 143,
    142, 140, 139, 138, 137, 136, 135, 133, 132, 131, 129, 128, 127, 125, 124, 123,
    121, 120, 118, 117, 115, 114, 112, 110, 109, 107, 105, 104, 102, 100, 98, 96,
    94, 92, 90, 88, 85, 83, 81, 78, 76, 73, 70, 67, 64, 61, 57, 54,
    50, 45, 41, 35, 29, 20,
};
_Static_assert(SP_R_SHELL == SP_CLIP_ROWS, "k_clip_half is built for this shell radius");

/* THE round safe-area guard. Folds [*xlo, *xhi] into both the panel and the
 * shell circle for this row; false means the row has nothing to draw. */
static bool sp_clip(int y, int *xlo, int *xhi)
{
    const int dy = y - SP_CY;
    const int ady = dy < 0 ? -dy : dy;
    if (ady >= SP_CLIP_ROWS) {
        return false;
    }
    const int half = k_clip_half[ady];
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

/* THE DAY ARC. A 24-hour scale with noon at 12 o'clock: the daylight is a
 * thin gold band on the tick scale from sunrise round the top to sunset, and
 * a dot on the band is now. Only the two dials with a data vocabulary carry
 * it (FUTURE, PILOT); a luxury dial does not grow a gadget. Drawn before the
 * hands so a tip crossing the band stays a hand. Nothing is drawn without a
 * published sun, and nothing is invented for the dot when the clock is off. */
#define SP_SUN_R_IN   204
#define SP_SUN_R_OUT  208
#define SP_SUN_A_NOON 192   /* SP_A_TOP: 12 o'clock */
static int sp_sun_angle(int minute)
{
    /* 1440 minutes on 256 units, noon at the top, clockwise. */
    return (SP_SUN_A_NOON + ((minute - 720) * 256) / 1440 + 512) & 255;
}

static void watch_sun_arc(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int style, int st, int hh, int mm)
{
    if (style != JR_WATCH_FUTURE && style != JR_WATCH_PILOT) {
        return;
    }
    const uint32_t sw = __atomic_load_n(&s_sun_word, __ATOMIC_ACQUIRE);
    if ((sw & (1u << 22)) == 0u || st < 128) {
        return;
    }
    const int rise = (int)(sw & 0x7FFu), set = (int)((sw >> 11) & 0x7FFu);
    const int a0 = sp_sun_angle(rise);
    const int sweep = ((sp_sun_angle(set) - a0) + 256) & 255;
    sp_span_t seg[SP_ARC_SEG_MAX];
    const int nseg = sp_arc_segments(seg, SP_ARC_SEG_MAX, a0, sweep);
    const uint16_t gold = sp_tint(ctx, 0xFD20, (st * 150) / 255);
    const uint16_t now_px = sp_tint(ctx, 0xFFE0, st);
    const bool have_now = hh >= 0 && hh < 24 && mm >= 0 && mm < 60;
    int dx = 0, dy = 0;
    if (have_now) {
        /* The dot: a small square on the band's centre line at now's angle
         * (the orbit rail's own arithmetic, 1/256 turns, Q15 sine). */
        const int a = sp_sun_angle(hh * 60 + mm);
        dx = (sp_cos(a) * ((SP_SUN_R_IN + SP_SUN_R_OUT) / 2)) >> 15;
        dy = (sp_sin(a) * ((SP_SUN_R_IN + SP_SUN_R_OUT) / 2)) >> 15;
    }
    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        for (int i = 0; i < nseg; ++i) {
            sp_annulus_row(row, y, SP_CX, SP_CY, SP_SUN_R_IN, SP_SUN_R_OUT, &seg[i], gold);
        }
        if (have_now) {
            sp_dot_row(row, y, SP_CX + dx, SP_CY + dy, 3, now_px);
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

/* hud_dim565 over x = lo..hi, two pixels to a 32-bit word. The fold is a mask
 * and a shift per pixel and sends black to black, so no pixel needs a test (the
 * test cost more than the fold on the busy face); the shift moves bits across
 * the pair's boundary only into bits the mask clears, so each half is exactly
 * hud_dim565 of its pixel. Rows start on a 4-byte boundary (HUD_W is even and
 * strips are carved from an aligned buffer), so an even x is a word. Pinned
 * against hud_dim565 over random rows of every parity in the frame suite. */
static inline __attribute__((always_inline)) void veil_dim_row(uint16_t *row, int lo, int hi, bool swap)
{
    int x = lo;
    if ((x & 1) != 0 && x <= hi) {
        row[x] = hud_dim565(row[x], swap);
        ++x;
    }
    for (; x + 1 <= hi; x += 2) {
        uint32_t w;
        void *p = __builtin_assume_aligned(row + x, 4);
        memcpy(&w, p, sizeof w);
        w = swap ? (((w & 0x00030003u) << 14) | ((w >> 2) & 0x27392739u))
                 : ((w >> 2) & 0x39E739E7u);
        memcpy(p, &w, sizeof w);
    }
    if (x <= hi) {
        row[x] = hud_dim565(row[x], swap);
    }
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
                veil_dim_row(row, lo, hi, true);
            } else {
                veil_dim_row(row, lo, hi, false);
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

static const char *sp_headline(int space);

/* ---- THE GLASS DATA SCREENS: STATUS and WEATHER ----------------------------
 *
 * Both screens are one ring at r118 (10 px stroke, round caps, starting at 12
 * o'clock) with the numbers inside it, drawn with the anti-aliased glass_*
 * primitives instead of 5x7 bitmaps and opaque annuli. Every primitive takes
 * the shell's slide fade `st` as its alpha, so the vertical slide and the
 * cross-dissolve work as before. Colour rules are the shell's: cyan live, gold
 * muted, red for a low cell, grey labels, ink values.
 *
 * ENTRY. When a screen becomes current the shell starts an entry clock
 * (s_entry_ms, advanced in sp_fade_tick). STATUS: the ring sweeps to its value
 * in 900 ms (ease-out-cubic) while the number counts up with it; the label,
 * power word and the three stats then fade in and rise 8 px, staggered.
 * WEATHER: the twelve rain slots build clockwise 45 ms apart. When the clock
 * passes SP_ENTRY_TOTAL_MS the screen is still and asks for nothing. The
 * frame rate on these screens is about 14 fps, so the steps are few and big:
 * every element gets at least three frames of fade.
 *
 * Nothing here owns a buffer: strings are formatted once per strip, and every
 * element early-outs on the rows it cannot touch. */
#define SG_RING_R_IN16  (113 * 16)
#define SG_RING_R_OUT16 (123 * 16)
#define SG_RING_DY      (-10)   /* the ring sits a little high: the stats need the room below */
#define SG_STAT_LABEL_Y 352     /* inside the orbit rail (r186), under the ring (y346) */
#define SG_STAT_VALUE_Y 367
#define SG_STAT_DX      56
#define SG_AGE_Y        362     /* WEATHER: how old it is, where STATUS has its stats */
#define SG_RISE_PX      8
#define SG_WX_ICON_SIZE GLASS_ICON_SM   /* the sky, above its word inside the ring */
#define SG_WX_ICON_GAP  10
#define SG_BOLT_INK_W   14   /* the small bolt's ink is this wide inside its 28 px box */
#define SG_BOLT_GAP     6
#define SG_FADE_MS      300
#define SG_GLOW16       40       /* 2.5 px each side; the dearest part of the frame */
#define SG_RAIN_SWEEP   3641    /* 20 degrees of a 30 degree slot */
#define SG_TICK_SWEEP   546     /* a dry hour: 3 degrees */
#define SG_RAIN_STAGGER_MS 45
#define SG_RAIN_BUILD_MS   250

/* One piece of text, measured when a row of the strip first draws it (a strip
 * that misses the line never pays for the glyph walk: ACTIVITY spent a quarter
 * of its frame measuring lines on strips that did not touch them). */
typedef struct {
    const char *s;
    int cx;                 /* centre to measure from while `pending` */
    bool pending;           /* x0 is not worked out yet */
    int len, x0, y_top, y_lo, y_hi, tracking;
    glass_font_id_t font;
    uint16_t color;
    uint8_t alpha;
} sg_txt_t;

static void sg_txt_make(sg_txt_t *t, glass_font_id_t font, const char *s, int len, int cx,
                        int y_top, int tracking, uint16_t native, int alpha)
{
    const glass_metrics_t m = glass_font_metrics(font);
    t->s = s;
    t->len = alpha > 0 ? len : 0;
    t->font = font;
    t->tracking = tracking;
    t->cx = cx;
    t->pending = true;
    t->x0 = cx;
    t->y_top = y_top;
    t->y_lo = y_top + m.ink_top;
    t->y_hi = y_top + m.ink_bottom;
    t->color = native;
    t->alpha = (uint8_t)(alpha > 255 ? 255 : (alpha < 0 ? 0 : alpha));
}

static inline void sg_txt_row(uint16_t *row, int y, int lo, int hi, bool swap, sg_txt_t *t)
{
    if (t->len > 0 && y >= t->y_lo && y <= t->y_hi) {
        if (t->pending) {
            t->x0 = t->cx - glass_text_width(t->font, t->s, (size_t)t->len, t->tracking) / 2;
            t->pending = false;
        }
        glass_text_row(row, y, lo, hi, t->font, t->s, (size_t)t->len, t->x0, t->y_top,
                       t->tracking, t->color, t->alpha, swap);
    }
}

/* One icon, centred on cx, its box top at y_top. An id below zero is no icon. */
typedef struct {
    int id, size, x0, y_top, y_hi;
    uint16_t color;
    uint8_t alpha;
} sg_ico_t;

static void sg_ico_make(sg_ico_t *t, int id, glass_icon_size_t size, int cx, int y_top,
                        uint16_t native, int alpha)
{
    const int px = glass_icon_px(size);
    t->id = id;
    t->size = (int)size;
    t->x0 = cx - px / 2;
    t->y_top = y_top;
    t->y_hi = y_top + px;
    t->color = native;
    t->alpha = (uint8_t)(id < 0 || alpha <= 0 ? 0 : (alpha > 255 ? 255 : alpha));
}

static inline void sg_ico_row(uint16_t *row, int y, int lo, int hi, bool swap, const sg_ico_t *t)
{
    if (t->alpha > 0u && y >= t->y_top && y < t->y_hi) {
        glass_icon_row(row, y, lo, hi, (glass_icon_id_t)t->id, (glass_icon_size_t)t->size, t->x0,
                       t->y_top, t->color, t->alpha, swap);
    }
}

/* Same text, left edge given (the % and degree sign ride beside the number). */
static void sg_txt_at(sg_txt_t *t, glass_font_id_t font, const char *s, int len, int x0,
                      int y_top, uint16_t native, int alpha)
{
    sg_txt_make(t, font, s, len, 0, y_top, 0, native, alpha);
    t->x0 = x0;
    t->pending = false;
}

/* The staggered element's fade and rise at entry time `ent`. */
static inline int sg_ent_alpha(int st, uint32_t ent, uint32_t start_ms)
{
    return (int)glass_frac_alpha((uint8_t)st, glass_stagger_q16(ent, start_ms, SG_FADE_MS));
}

static inline int sg_ent_rise(uint32_t ent, uint32_t start_ms)
{
    return glass_rise_px(SG_RISE_PX, glass_stagger_q16(ent, start_ms, SG_FADE_MS));
}

/* Centre of rain slot k: twelve 30 degree slots from 12 o'clock, clockwise. */
static int32_t sg_slot_centre_q16(int k)
{
    return (int32_t)((GLASS_Q16_12_OCLOCK + ((2 * k + 1) * GLASS_TURN_Q16) / 24) & 0xFFFF);
}

/* An hour's mark from its chance of rain: a dry hour is a faint 2 px tick,
 * a wet one a 20 degree stroke whose width (3..10 px) and alpha grow with the
 * chance; `f` is the entry build (Q16) that grows it from nothing. */
static void sg_rain_mark(int pct, uint32_t f, int32_t *w16, int32_t *sweep, int *alpha)
{
    int w, a, sw;
    if (pct < SP_WX_HOUR_DRY) {
        w = 2 * 16;
        a = 90;
        sw = SG_TICK_SWEEP;
    } else {
        w = (3 * 16) + (7 * 16 * pct) / 100;
        a = 100 + (155 * pct) / 100;
        sw = SG_RAIN_SWEEP;
    }
    *w16 = (int32_t)(((int64_t)w * f) >> 16);
    *sweep = sw;
    *alpha = (int)glass_frac_alpha((uint8_t)a, f);
}

/* "H86 L76" -> "H 86  L 76": the composed line, opened up to read as two
 * figures. */
static int sg_hilo_open(char *dst, int cap, const char *src)
{
    int n = 0;
    for (; *src != '\0' && n + 3 < cap; ++src) {
        if (*src == ' ') {
            dst[n++] = ' ';
            dst[n++] = ' ';
        } else {
            dst[n++] = *src;
            if (*src == 'H' || *src == 'L') {
                dst[n++] = ' ';
            }
        }
    }
    dst[n] = '\0';
    return n;
}

/* The sky picks the accent. Cyan is the glass's own colour, so an ordinary
 * sky is the ordinary glass; the others are the hues already given meaning
 * elsewhere (amber: look, violet: on-trial) borrowed for a day that is worth
 * a glance. Gold is never returned here â€” see the palette. */
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

/* WEATHER: the ring is the next twelve hours. Slot k is hour now+k, a 30
 * degree slot clockwise from 12 o'clock, drawn as a round-capped stroke whose
 * width and alpha follow that hour's chance of rain; a dry hour is a faint
 * tick, an hour the answer lacks draws nothing. Inside: the condition word as
 * the tracked label in the sky's accent, the temperature large with its degree
 * sign, high and low, and the sun line. How old all of this is, once it is old
 * enough to matter, sits under the ring (and gives way to an update, which
 * crosses that row).
 *
 * Honesty rules, in order: muted turns every accent gold like every other
 * screen; stale (> 30 min) turns them grey and draws no hours (a stale
 * answer's "now" is a guess), and with no hours the ring is a bare faint
 * track; !valid draws that track, "NO WEATHER" and not one digit. */
static void sp_focal_weather(const jr_display_ctx_t *ctx, int y1, int y2,
                             uint16_t *pixels, int oy, int st, uint32_t ent)
{
    const bool swap = ctx->board.swap_color_bytes;
    const bool muted = sp_privacy_muted();
    const uint16_t accent =
        muted ? SP_C_GOLD : (s_wx_stale ? SP_C_GREY : sp_sky_native(s_wx_sky));
    const uint16_t rain = muted ? SP_C_GOLD : SP_C_RAIN;
    const int cx16 = SP_CX * 16;
    const int cyr = SP_CY + oy + SG_RING_DY;
    const int cy16 = cyr * 16;

    /* the twelve slots */
    int32_t sg_start[SP_WX_HOURS], sg_sweep[SP_WX_HOURS], sg_ri[SP_WX_HOURS], sg_ro[SP_WX_HOURS];
    uint8_t sg_a[SP_WX_HOURS];
    int nseg = 0;
    for (int k = 0; k < SP_WX_HOURS && s_wx_valid && s_wx_hours > 0; ++k) {
        const int pct = s_wx_hour_pct[k];
        if (pct == SP_WX_HOUR_NONE) {
            continue;
        }
        const uint32_t f = glass_stagger_q16(ent, (uint32_t)k * SG_RAIN_STAGGER_MS, SG_RAIN_BUILD_MS);
        int32_t w16, sweep;
        int a;
        sg_rain_mark(pct, f, &w16, &sweep, &a);
        if (w16 < 8 || a <= 0) {
            continue;                 /* not yet begun */
        }
        const int32_t rc = (113 * 16 + 123 * 16) / 2;
        sg_ri[nseg] = rc - w16 / 2;
        sg_ro[nseg] = rc + (w16 + 1) / 2;
        sg_start[nseg] = (sg_slot_centre_q16(k) - sweep / 2) & 0xFFFF;
        sg_sweep[nseg] = sweep;
        sg_a[nseg] = (uint8_t)((a * st + 127) / 255);
        ++nseg;
    }
    const bool bare = !(s_wx_valid && s_wx_hours > 0);

    /* text */
    char hilo[16];
    const int hlen = sg_hilo_open(hilo, (int)sizeof hilo, s_wx_hilo);
    const char *head = sp_headline(JR_DISPLAY_SPACE_WEATHER);
    const int llen = sp_len(head, SP_LABEL_CAP - 1);
    const bool updating = sp_ota_ringing((jr_display_ota_state_t)
        (__atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xFu));
    const int tlen = sp_len(s_wx_temp, (int)sizeof s_wx_temp - 1);
    const int slen = sp_len(s_wx_sun, (int)sizeof s_wx_sun - 1);
    const int alen = sp_len(s_wx_age, (int)sizeof s_wx_age - 1);
    const glass_metrics_t hero = glass_font_metrics(GLASS_FONT_HERO);
    const int hero_top = cyr - hero.cap_h / 2 - 2;
    const int base = hero_top + hero.cap_h;
    const uint32_t t_lbl = 0, t_hero = 150, t_hl = 250, t_sun = 330, t_age = 410;

    sg_txt_t t_label, t_temp, t_deg, t_hilo, t_sun_t, t_age_t;
    sg_txt_make(&t_label, GLASS_FONT_LABEL, head, llen, SP_CX,
                hero_top - 26 + sg_ent_rise(ent, t_lbl), 3, s_wx_valid ? accent : SP_C_GREY,
                sg_ent_alpha(st, ent, t_lbl));
    sg_ico_t t_ico;
    sg_ico_make(&t_ico, s_wx_icon, SG_WX_ICON_SIZE, SP_CX,
                hero_top - 26 - SG_WX_ICON_GAP - glass_icon_px(SG_WX_ICON_SIZE) + sg_ent_rise(ent, t_lbl),
                accent, sg_ent_alpha(st, ent, t_lbl));
    const int hw = glass_text_width(GLASS_FONT_HERO, s_wx_temp, (size_t)tlen, 0);
    const int dw = glass_text_width(GLASS_FONT_VALUE, "*", 1, 0);
    const int x0 = SP_CX - (hw + 4 + dw) / 2;
    const int hero_y = hero_top + sg_ent_rise(ent, t_hero);
    const int hero_a = sg_ent_alpha(st, ent, t_hero);
    sg_txt_at(&t_temp, GLASS_FONT_HERO, s_wx_temp, tlen, x0, hero_y, SP_C_INK, hero_a);
    sg_txt_at(&t_deg, GLASS_FONT_VALUE, "*", tlen > 0 ? 1 : 0, x0 + hw + 4, hero_y, accent, hero_a);
    sg_txt_make(&t_hilo, GLASS_FONT_TEXT, hilo, hlen, SP_CX, base + 16 + sg_ent_rise(ent, t_hl), 1,
                SP_C_INK, sg_ent_alpha(st, ent, t_hl));
    sg_txt_make(&t_sun_t, GLASS_FONT_TEXT, s_wx_sun, slen, SP_CX, base + 40 + sg_ent_rise(ent, t_sun),
                1, SP_C_GREY, sg_ent_alpha(st, ent, t_sun));
    sg_txt_make(&t_age_t, GLASS_FONT_LABEL, s_wx_age, alen, SP_CX,
                SG_AGE_Y + oy + sg_ent_rise(ent, t_age), 2, SP_C_GREY,
                updating ? 0 : sg_ent_alpha(st, ent, t_age));

    const int ring_y0 = cyr - 130, ring_y1 = cyr + 130;
    /* the rows each mark can reach: the other rows' calls change nothing */
    int sg_y0[SP_WX_HOURS], sg_y1[SP_WX_HOURS];
    for (int i = 0; i < nseg; ++i) {
        if (!glass_arc_rows(cx16, cy16, sg_ri[i], sg_ro[i], sg_start[i], sg_sweep[i], true,
                            &sg_y0[i], &sg_y1[i])) {
            sg_y0[i] = 1;
            sg_y1[i] = 0;
        }
    }
    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        if (y >= ring_y0 && y <= ring_y1) {
            if (bare) {
                glass_ring_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16, 0,
                               GLASS_TURN_Q16, SP_C_TRACK, (uint8_t)st, swap);
            }
            for (int i = 0; i < nseg; ++i) {
                if (y >= sg_y0[i] && y <= sg_y1[i]) {
                    glass_arc_row(row, y, lo, hi, cx16, cy16, sg_ri[i], sg_ro[i], sg_start[i],
                                  sg_sweep[i], rain, sg_a[i], swap);
                }
            }
        }
        sg_ico_row(row, y, lo, hi, swap, &t_ico);
        sg_txt_row(row, y, lo, hi, swap, &t_label);
        sg_txt_row(row, y, lo, hi, swap, &t_temp);
        sg_txt_row(row, y, lo, hi, swap, &t_deg);
        sg_txt_row(row, y, lo, hi, swap, &t_hilo);
        sg_txt_row(row, y, lo, hi, swap, &t_sun_t);
        sg_txt_row(row, y, lo, hi, swap, &t_age_t);
    }
}

/* The battery / desk ring at rest. Its three layers (the track, the glow, the
 * filled arc) are the same shapes every frame until the number moves, and each
 * cost 0.4 to 1 M instructions a frame in geometry that never changes. Once the
 * entry has finished and the screen sits still (oy == 0) they are recorded
 * (glass_shape_record / glass_glow_record) and replayed: the same fills and
 * blends, so the same pixels, for any fade, colour and clip. A change of the
 * sweep records again, once it has held for a second frame; while the entry
 * runs, or the screen is being dragged, the ring is drawn the long way, as
 * before. The records are three blocks of
 * SG_FRING_CAP bytes in PSRAM, allocated once at start (the largest the ring
 * needs, at 99 %, is 10 KB); if one is absent or did not fit, that layer is
 * drawn the long way. Internal RAM: a pointer and 8 bytes of key. */
#define SG_FRING_CAP   12288
#define JR_FRING_REC   1        /* tells the host fixtures the recorded ring exists */
enum { SG_FR_TRACK = 1, SG_FR_GLOW = 2, SG_FR_ARC = 4 };
static uint8_t *s_fring_rec;    /* track | glow | arc, SG_FRING_CAP bytes each */
static struct {
    int32_t sweep;              /* the sweep the records were made for */
    uint8_t have;               /* SG_FR_* parts recorded for it; bit 7: tried */
} s_fring;

static void fring_rec_alloc(void)
{
    if (s_fring_rec == NULL) {
        s_fring_rec = heap_caps_malloc(3u * SG_FRING_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_fring.have = 0;
}

/* Records the ring for `sweep` if the block is there and it is not already. */
static uint8_t fring_prepare(int32_t sweep)
{
    if (s_fring_rec == NULL) {
        return 0;
    }
    if (s_fring.have != 0 && s_fring.sweep == sweep) {
        return s_fring.have;
    }
    static sp_settle_t gate;
    if (!sp_settled(&gate, sweep, 0)) {
        return 0;                 /* a new value: draw it the long way for a frame first */
    }
    const int32_t cx16 = SP_CX * 16, cy16 = (SP_CY + SG_RING_DY) * 16;
    uint8_t have = 0;
    if (sweep < GLASS_TURN_Q16 &&
        glass_shape_record(s_fring_rec, SG_FRING_CAP, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                           GLASS_Q16_12_OCLOCK + sweep, GLASS_TURN_Q16 - sweep, false) != 0u) {
        have |= SG_FR_TRACK;
    }
    if (sweep > 0) {
        if (glass_glow_record(s_fring_rec + SG_FRING_CAP, SG_FRING_CAP, cx16, cy16, SG_RING_R_IN16,
                              SG_RING_R_OUT16, GLASS_Q16_12_OCLOCK, sweep, true, SG_GLOW16) != 0u) {
            have |= SG_FR_GLOW;
        }
        if (glass_shape_record(s_fring_rec + 2u * SG_FRING_CAP, SG_FRING_CAP, cx16, cy16,
                               SG_RING_R_IN16, SG_RING_R_OUT16, GLASS_Q16_12_OCLOCK, sweep, true) != 0u) {
            have |= SG_FR_ARC;
        }
    }
    s_fring.sweep = sweep;
    s_fring.have = have | 0x80u;          /* tried, so a failure is not retried every strip */
    return s_fring.have;
}

/* One row of the ring: the track where the fill is not, then (arc_on) the glow
 * and the fill. `have` is fring_prepare()'s answer, 0 to draw the long way. */
static inline void fring_row(uint16_t *row, int y, int lo, int hi, bool swap, int32_t cy16,
                             int32_t sweep, bool arc_on, uint16_t fill, int st, uint8_t have)
{
    const int32_t cx16 = SP_CX * 16;
    const uint8_t a_trk = (uint8_t)((st * 44) / 255);
    if (have & SG_FR_TRACK) {
        glass_ring_replay_row(row, y, lo, hi, s_fring_rec, fill, a_trk, swap);
    } else {
        glass_ring_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                       GLASS_Q16_12_OCLOCK + sweep, GLASS_TURN_Q16 - sweep, fill, a_trk, swap);
    }
    if (!arc_on) {
        return;
    }
    if (have & SG_FR_GLOW) {
        glass_glow_replay_row(row, y, lo, hi, s_fring_rec + SG_FRING_CAP, SG_GLOW16, 140, fill,
                              (uint8_t)st, swap);
    } else {
        glass_glow_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                       GLASS_Q16_12_OCLOCK, sweep, true, SG_GLOW16, 140, fill, (uint8_t)st, swap);
    }
    if (have & SG_FR_ARC) {
        glass_ring_replay_row(row, y, lo, hi, s_fring_rec + 2u * SG_FRING_CAP, fill, (uint8_t)st, swap);
    } else {
        glass_arc_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                      GLASS_Q16_12_OCLOCK, sweep, fill, (uint8_t)st, swap);
    }
}

/* ACTIVITY: up to three blocks, newest first. Line one is the kind and the age
 * in the label face, line two the summary in the text face; each older block
 * is a step quieter, so the eye lands on what just happened. On entry the
 * blocks fade in and rise, 90 ms apart. With nothing pushed the one honest
 * line is centred where the first block would be. No second dim over the disc:
 * the shell veil already quiets the face and a per-pixel fold over r<=168 once
 * cost ~88k pixels a frame. */
#define SG_ACT_STAGGER_MS 90
static void sp_focal_activity(const jr_display_ctx_t *ctx, int y1, int y2,
                              uint16_t *pixels, int oy, int st, uint32_t ent)
{
    const bool swap = ctx->board.swap_color_bytes;
    static const uint8_t k_fade[ACT_MAX] = {255, 200, 150};
    sg_txt_t t_kind[ACT_MAX], t_age[ACT_MAX], t_sum[ACT_MAX], t_empty;
    const glass_metrics_t tm = glass_font_metrics(GLASS_FONT_TEXT);

    sg_txt_make(&t_empty, GLASS_FONT_TEXT, "NOTHING YET", s_act_rows == 0 ? 11 : 0, SP_CX,
                SP_CY - tm.cap_h / 2 - tm.ink_top + oy + sg_ent_rise(ent, 0), 1, SP_C_GREY,
                sg_ent_alpha(st, ent, 0));
    for (int i = 0; i < ACT_MAX; ++i) {
        const bool have = i < s_act_rows;
        const uint32_t start = (uint32_t)i * SG_ACT_STAGGER_MS;
        const int a = (sg_ent_alpha(st, ent, start) * k_fade[i]) / 255;
        const int top = SP_ACT_Y0 + i * SP_ACT_PITCH + oy + sg_ent_rise(ent, start);
        const char *kind = have ? s_act_kind[i] : "";
        const char *age = have ? s_act_age[i] : "";
        const int klen = have ? sp_len(kind, ACT_KIND_CAP - 1) : 0;
        const int alen = have ? sp_len(age, SP_ACT_AGE_CAP - 1) : 0;
        const int kw = glass_text_width(GLASS_FONT_LABEL, kind, (size_t)klen, 2);
        const int aw = glass_text_width(GLASS_FONT_LABEL, age, (size_t)alen, 2);
        const int total = kw + (klen > 0 && alen > 0 ? SP_ACT_GAP : 0) + aw;
        const int x0 = SP_CX - total / 2;
        sg_txt_at(&t_kind[i], GLASS_FONT_LABEL, kind, klen, x0, top, SP_C_GREY, a);
        t_kind[i].tracking = 2;
        sg_txt_at(&t_age[i], GLASS_FONT_LABEL, age, alen,
                  x0 + kw + (klen > 0 ? SP_ACT_GAP : 0), top, SP_C_GREY, (a * 2) / 3);
        t_age[i].tracking = 2;
        const int slen = have ? sp_len(s_act_row[i], SP_ACT_ROW_CAP - 1) : 0;
        sg_txt_make(&t_sum[i], GLASS_FONT_TEXT, s_act_row[i], slen, SP_CX, top + SP_ACT_SUM_DY, 0,
                    SP_C_INK, a);
    }

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        sg_txt_row(row, y, lo, hi, swap, &t_empty);
        for (int i = 0; i < s_act_rows; ++i) {
            sg_txt_row(row, y, lo, hi, swap, &t_kind[i]);
            sg_txt_row(row, y, lo, hi, swap, &t_age[i]);
            sg_txt_row(row, y, lo, hi, swap, &t_sum[i]);
        }
    }
}

/* DESK: the STATUS ring around the job's progress. The ring is r113-123 on the
 * agent's state colour (cyan with none); the number counts up with the sweep on
 * entry; the task, shortened by main.c, is the tracked label above it and the
 * agent's state is the word below. */
static void sp_focal_desk(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int oy, int st, uint32_t ent)
{
    const uint32_t w = __atomic_load_n(&s_desk_word, __ATOMIC_ACQUIRE);
    int prog = (int)(w & 0xFFu);
    if (prog > 100) {
        prog = 100;
    }
    const jr_display_agent_state_t state = (jr_display_agent_state_t)((w >> 8) & 0xFu);
    const uint16_t fill = state == JR_DISPLAY_AGENT_NONE ? SP_C_CYAN : agent_native_color(state);
    const bool swap = ctx->board.swap_color_bytes;
    const int cyr = SP_CY + oy + SG_RING_DY;
    const int cy16 = cyr * 16;

    const uint32_t f_ring = glass_ease_out_cubic_q16(ent, SP_ENTRY_RING_MS);
    const int shown = (int)glass_count_up(prog, f_ring);
    const int32_t full = (int32_t)(((int64_t)prog * GLASS_TURN_Q16) / 100);
    const int32_t sweep = (int32_t)(((int64_t)full * f_ring) >> 16);

    char num[6];
    const int nlen = sp_num(num, 0, (int)sizeof num, (uint32_t)shown);
    const char *task = sp_headline(JR_DISPLAY_SPACE_DESK);
    const int tlen = sp_len(task, SP_LABEL_CAP - 1);
    const char *sname = sp_agent_name(state);
    const int snlen = sp_len(sname, SP_COL_MAX - 1);

    const glass_metrics_t hero = glass_font_metrics(GLASS_FONT_HERO);
    const glass_metrics_t val = glass_font_metrics(GLASS_FONT_VALUE);
    const int hero_top = cyr - hero.cap_h / 2 - 2;
    const int base = hero_top + hero.cap_h;
    const int hw = glass_text_width(GLASS_FONT_HERO, num, (size_t)nlen, 0);
    const int pw = glass_text_width(GLASS_FONT_VALUE, "%", 1, 0);
    const int x0 = SP_CX - (hw + 5 + pw) / 2;
    const uint32_t t_lbl = 0, t_word = 150;

    sg_txt_t t_label, t_hero, t_pc, t_state;
    sg_txt_make(&t_label, GLASS_FONT_LABEL, task, tlen, SP_CX,
                hero_top - 26 + sg_ent_rise(ent, t_lbl), 3, SP_C_GREY, sg_ent_alpha(st, ent, t_lbl));
    sg_txt_at(&t_hero, GLASS_FONT_HERO, num, nlen, x0, hero_top, SP_C_INK, st);
    sg_txt_at(&t_pc, GLASS_FONT_VALUE, "%", 1, x0 + hw + 5, base - val.cap_h, fill, st);
    sg_txt_make(&t_state, GLASS_FONT_TEXT, sname, snlen, SP_CX, base + 16 + sg_ent_rise(ent, t_word),
                1, SP_C_GREY, sg_ent_alpha(st, ent, t_word));

    const int ring_y0 = cyr - 130, ring_y1 = cyr + 130;
    /* at rest: the ring's three layers replayed from their records */
    const uint8_t rec = (oy == 0 && f_ring >= GLASS_ONE_Q16) ? (uint8_t)(fring_prepare(sweep) & 0x7Fu) : 0u;
    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        if (y >= ring_y0 && y <= ring_y1) {
            fring_row(row, y, lo, hi, swap, cy16, sweep, sweep > 0, fill, st, rec);
        }
        sg_txt_row(row, y, lo, hi, swap, &t_label);
        sg_txt_row(row, y, lo, hi, swap, &t_hero);
        sg_txt_row(row, y, lo, hi, swap, &t_pc);
        sg_txt_row(row, y, lo, hi, swap, &t_state);
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
/* STATUS at a glance, the geometry (y is from the top of the 466 px panel):
 *
 *   headline    the in-ring label: BATTERY while all is well, else the worst
 *               thing wrong (LOW BATTERY, NO WIFI, UPDATE 42% ...). The uptime
 *               the healthy headline used to say is on the detail sheet.
 *   r 113..123  the battery ring, centred 8 px above the panel centre; track
 *               at low alpha, the fill from 12 o'clock clockwise by percent,
 *               round-capped, with a narrow glow on the filled part only
 *   inside      BATTERY (label) / 74 with a small % / the power word (grey)
 *   y 352..382  three stats, label over value: WIFI (dBm or DOWN), LINK,
 *               TOOLS; an off stat is dim. They stay inside the orbit rail
 *               (r186) and so cross the update ring's band (r140-154): while
 *               an update rings they are not drawn, so nothing of the screen
 *               is ever overdrawn by it.
 *
 * Every state the old screen told is told here: no gauge is a bare track and
 * "--" (never a full ring under "NO BATTERY"), low is red, charging says so
 * in the power word, muted turns the accent gold. */
static void sp_focal_status(const jr_display_ctx_t *ctx, int y1, int y2,
                            uint16_t *pixels, int oy, int st, uint32_t ent)
{
    const uint32_t w = __atomic_load_n(&s_power_word, __ATOMIC_ACQUIRE);
    const uint32_t lk = __atomic_load_n(&s_links_word, __ATOMIC_ACQUIRE);
    const int raw_pct = (int)(w & 0xFFu);
    /* 0xFF means the fuel gauge did not answer. Clamping that to 100 drew a
     * FULL ring directly beneath the headline "NO BATTERY" â€” the most
     * confident possible rendering of a number we do not have. Draw the bare
     * track instead: an empty track reads as "no reading", which is true. */
    const bool have_gauge = raw_pct <= 100;
    const int pct = have_gauge ? raw_pct : 0;
    const bool charging = (w & (1u << 25)) != 0u;
    const bool muted = sp_privacy_muted();
    /* Red below HUD_BATT_LOW_PCT is the one place colour carries meaning on
     * the ring â€” the SAME red, on the SAME rule, as the battery rim at r215.
     * Gold means muted everywhere on this glass, so the ring wears it too. */
    const bool low = have_gauge && pct < HUD_BATT_LOW_PCT && !charging;
    const uint16_t accent = muted ? SP_C_GOLD : SP_C_CYAN;
    const uint16_t fill = low ? SP_C_RED : accent;
    const bool swap = ctx->board.swap_color_bytes;
    const int cyr = SP_CY + oy + SG_RING_DY;
    const int cy16 = cyr * 16;

    /* The entry: ring and number share one eased fraction. */
    const uint32_t f_ring = glass_ease_out_cubic_q16(ent, SP_ENTRY_RING_MS);
    const int shown = have_gauge ? (int)glass_count_up(pct, f_ring) : 0;
    const int32_t full = (int32_t)(((int64_t)pct * GLASS_TURN_Q16) / 100);
    const int32_t sweep = (int32_t)(((int64_t)full * f_ring) >> 16);

    /* The words, formatted once per strip: a few digits, no snprintf. */
    char pctbuf[6];
    const int plen = have_gauge ? sp_num(pctbuf, 0, (int)sizeof pctbuf, (uint32_t)shown)
                                : sp_str(pctbuf, 0, (int)sizeof pctbuf, "--");
    const char *word = sp_power_word(w);
    const int wlen = sp_len(word, SP_LABEL_CAP - 1);
    const int bars = sp_wifi_bars(lk);
    char dbm[6];
    const int dlen = bars > 0
        ? sp_int(dbm, 0, (int)sizeof dbm, -(int)((lk >> 8) & 0xFFu))
        : sp_str(dbm, 0, (int)sizeof dbm, "DOWN");
    const bool link_on = (lk & (1u << 1)) != 0u;
    const bool tools_on = ((lk >> 2) & 3u) == 2u;
    const bool updating = sp_ota_ringing((jr_display_ota_state_t)
        (__atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xFu));
    const char *head = sp_headline(JR_DISPLAY_SPACE_STATUS);
    const int hlen = sp_len(head, SP_LABEL_CAP - 1);
    /* The headline's job is the worst thing wrong. Healthy, it is the uptime
     * and the in-ring label stays BATTERY; otherwise the headline IS the
     * label ("LOW BATTERY", "NO WIFI", "UPDATE 42%"), where it clears the
     * update ring. */
    const bool quiet_head = hlen == 0 || (hlen > 3 && head[0] == 'U' && head[1] == 'P' && head[2] == ' ');
    const char *label = quiet_head ? "BATTERY" : head;
    const int label_len = quiet_head ? 7 : hlen;

    const glass_metrics_t hero = glass_font_metrics(GLASS_FONT_HERO);
    const glass_metrics_t val = glass_font_metrics(GLASS_FONT_VALUE);
    const int hero_top = cyr - hero.cap_h / 2 - 2;
    const int base = hero_top + hero.cap_h;
    const int hw = glass_text_width(GLASS_FONT_HERO, pctbuf, (size_t)plen, 0);
    const int pw = have_gauge ? glass_text_width(GLASS_FONT_VALUE, "%", 1, 0) : 0;
    const int x0 = SP_CX - (hw + (have_gauge ? 5 : 0) + pw) / 2;
    const uint32_t t_lbl = 0, t_word = 150, t_wifi = 250, t_link = 330, t_tools = 410;
    const int dim_a = (st * 96) / 255;

    sg_txt_t t_label, t_hero, t_pc, t_word_t;
    sg_txt_t t_sl[3], t_sv[3];
    sg_txt_make(&t_label, GLASS_FONT_LABEL, label, label_len, SP_CX,
                hero_top - 26 + sg_ent_rise(ent, t_lbl), 3, SP_C_GREY, sg_ent_alpha(st, ent, t_lbl));
    sg_txt_at(&t_hero, GLASS_FONT_HERO, pctbuf, plen, x0, hero_top, SP_C_INK, st);
    sg_txt_at(&t_pc, GLASS_FONT_VALUE, "%", have_gauge ? 1 : 0, x0 + hw + 5, base - val.cap_h, fill, st);
    /* Charging: the bolt stands beside the word and the pair is centred as one,
     * so the word keeps its place in the middle of the ring when it is alone. */
    const int ww = glass_text_width(GLASS_FONT_TEXT, word, (size_t)wlen, 1);
    const int word_cx = charging ? SP_CX + (SG_BOLT_INK_W + SG_BOLT_GAP) / 2 : SP_CX;
    sg_txt_make(&t_word_t, GLASS_FONT_TEXT, word, wlen, word_cx,
                base + 16 + sg_ent_rise(ent, t_word), 1, SP_C_GREY, sg_ent_alpha(st, ent, t_word));
    sg_ico_t t_bolt;
    sg_ico_make(&t_bolt, charging ? GLASS_ICON_BOLT : -1, GLASS_ICON_SM,
                word_cx - ww / 2 - SG_BOLT_GAP - SG_BOLT_INK_W / 2,
                base + 16 + glass_font_metrics(GLASS_FONT_TEXT).cap_h / 2 - glass_icon_px(GLASS_ICON_SM) / 2 +
                    sg_ent_rise(ent, t_word),
                fill, sg_ent_alpha(st, ent, t_word));
    static const char *const k_names[3] = {"WIFI", "LINK", "TOOLS"};
    const char *vals[3] = {dbm, link_on ? "ON" : "OFF", tools_on ? "ON" : "OFF"};
    const int vlen[3] = {dlen, link_on ? 2 : 3, tools_on ? 2 : 3};
    const bool on[3] = {bars > 0, link_on, tools_on};
    const uint32_t starts[3] = {t_wifi, t_link, t_tools};
    for (int i = 0; i < 3; ++i) {
        const int cxi = SP_CX + (i - 1) * SG_STAT_DX;
        const int rise = sg_ent_rise(ent, starts[i]);
        const int ea = updating ? 0 : sg_ent_alpha(st, ent, starts[i]);
        sg_txt_make(&t_sl[i], GLASS_FONT_LABEL, k_names[i], (int)strlen(k_names[i]), cxi,
                    SG_STAT_LABEL_Y + oy + rise, 2, SP_C_GREY, ea);
        sg_txt_make(&t_sv[i], GLASS_FONT_TEXT, vals[i], vlen[i], cxi,
                    SG_STAT_VALUE_Y + oy + rise, 0, on[i] ? SP_C_INK : SP_C_GREY,
                    on[i] ? ea : (ea * dim_a) / (st > 0 ? st : 1));
    }

    const int ring_y0 = cyr - 130, ring_y1 = cyr + 130;
    /* at rest: the ring's three layers replayed from their records */
    const uint8_t rec = (oy == 0 && f_ring >= GLASS_ONE_Q16) ? (uint8_t)(fring_prepare(sweep) & 0x7Fu) : 0u;
    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        if (y >= ring_y0 && y <= ring_y1) {
            /* the track only where the fill is not: no pixel is written twice */
            fring_row(row, y, lo, hi, swap, cy16, sweep, have_gauge && sweep > 0, fill, st, rec);
        }
        sg_txt_row(row, y, lo, hi, swap, &t_label);
        sg_txt_row(row, y, lo, hi, swap, &t_hero);
        sg_txt_row(row, y, lo, hi, swap, &t_pc);
        sg_txt_row(row, y, lo, hi, swap, &t_word_t);
        sg_ico_row(row, y, lo, hi, swap, &t_bolt);
        for (int i = 0; i < 3; ++i) {
            sg_txt_row(row, y, lo, hi, swap, &t_sl[i]);
            sg_txt_row(row, y, lo, hi, swap, &t_sv[i]);
        }
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
                          uint16_t *pixels, int space, int oy, int st, uint32_t ent)
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
        sp_focal_status(ctx, y1, y2, pixels, oy, st, ent);
        break;
    case JR_DISPLAY_SPACE_DESK:
        sp_focal_desk(ctx, y1, y2, pixels, oy, st, ent);
        break;
    case JR_DISPLAY_SPACE_WEATHER:
        sp_focal_weather(ctx, y1, y2, pixels, oy, st, ent);
        break;
    case JR_DISPLAY_SPACE_ACTIVITY:
        sp_focal_activity(ctx, y1, y2, pixels, oy, st, ent);
        break;
    default:
        break;                /* a space with no focal object draws none */
    }
    if (space == JR_DISPLAY_SPACE_STATUS || space == JR_DISPLAY_SPACE_WEATHER ||
        space == JR_DISPLAY_SPACE_DESK) {
        return;                   /* the glass screens set their own headline */
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
    const int hlen = sp_len(s_detail_head, SP_LABEL_CAP - 1);
    sg_txt_t t_head, t_key[SP_ROWS_MAX], t_val[SP_ROWS_MAX];
    sg_txt_make(&t_head, GLASS_FONT_LABEL, s_detail_head, hlen, SP_CX, SP_SHEET_HEAD_Y - 2, 3,
                SP_C_CYAN, st);
    for (int i = 0; i < SP_ROWS_MAX; ++i) {
        const bool have = i < s_detail_rows;
        const int klen = have ? sp_len(s_detail_label[i], SP_COL_MAX - 1) : 0;
        const int vlen = have ? sp_len(s_detail_value[i], SP_VAL_MAX - 1) : 0;
        const int ry = SP_SHEET_ROW_Y + i * SP_SHEET_ROW_DY - 2;
        sg_txt_at(&t_key[i], GLASS_FONT_LABEL, s_detail_label[i], klen, SP_SHEET_LEFT, ry + 2,
                  SP_C_GREY, st);
        t_key[i].tracking = 2;
        sg_txt_make(&t_val[i], GLASS_FONT_TEXT, s_detail_value[i], vlen, 0, ry, 0, SP_C_INK, st);
        t_val[i].x0 = SP_SHEET_RIGHT - glass_text_width(GLASS_FONT_TEXT, s_detail_value[i],
                                                        (size_t)vlen, 0);
        t_val[i].pending = false;
    }

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
        sg_txt_row(row, y, lo, hi, swap, &t_head);
        for (int i = 0; i < s_detail_rows; ++i) {
            sg_txt_row(row, y, lo, hi, swap, &t_key[i]);
            sg_txt_row(row, y, lo, hi, swap, &t_val[i]);
        }
    }
}


/* THE SHADE, on the glass primitives (geometry and touch: see SP_BTN_CY).
 *
 *   left rim   volume, cyan, the speaker and the number inside the arc
 *   right rim  brightness, amber, the sun and the number inside the arc
 *   centre     the mute button: a disc, the microphone, cyan live and gold
 *              muted; muted adds a slash on the microphone and a slow
 *              breathing ring (s_breath_ms runs only then, only with the
 *              shade open). Under it the state and one line of hint.
 *   bottom     the two key hints, one quiet line
 *
 * ENTRY (s_shade_ms, 700 ms): the arcs sweep to their values (ease-out-cubic,
 * brightness 90 ms behind) while the numbers count up; the button's disc grows
 * from 71% to full as it fades in; the icons, the state, the hint and the keys
 * follow, staggered. Every alpha is multiplied by the shade fade `st`. */
#define SG_SH_R_IN16   (180 * 16)
#define SG_SH_R_OUT16  (188 * 16)
#define SG_SH_HALF_Q16 7282            /* each arc: +-40 degrees about 9 and 3 o'clock */
#define SG_SH_Y0       104             /* rows the arcs can touch */
#define SG_SH_Y1       362
#define SG_SH_CLUSTER_DX 128           /* the icon and number sit this far from the centre line */
#define SG_SH_ICON_Y   (SP_CY - 25)     /* icon + gap + number: 51 px, centred on the arcs */
#define SG_SH_NUM_Y    (SP_CY + 11)
#define SG_SH_STATE_Y  (SP_BTN_CY + SP_BTN_R + 20)
#define SG_SH_HINT_Y   (SP_BTN_CY + SP_BTN_R + 44)
#define SG_SH_KEYS_Y   372

/* The breathing ring's two numbers at phase `ms`: how far out it has grown
 * (0..4 px, in 1/16 px) and its alpha. A raised cosine, so it rests at the
 * button's edge, swells, and settles; the same function every strip. */
static void sg_breath(uint32_t ms, int32_t *grow16, int *alpha)
{
    const uint32_t phase = (uint32_t)(((uint64_t)(ms % SP_BREATH_MS) * GLASS_TURN_Q16) / SP_BREATH_MS);
    const int b = (int)((32768 - glass_cos_q15(phase)) >> 8);   /* 0..256 */
    *grow16 = (4 * 16 * b) >> 8;
    *alpha = 130 - ((100 * b) >> 8);
}

/* The control shade's four arcs (the volume and brightness tracks and their
 * fills) at rest. The tracks never change, so they are recorded once at start;
 * a fill is recorded when its value has held for a frame after its entry, and
 * again when it moves (see fring_prepare). Four blocks of SG_SH_REC_CAP bytes in
 * PSRAM (the largest arc here is 6 KB); a block that is absent or did not fit
 * is drawn the long way. Internal RAM: a pointer and 84 bytes of keys. */
#define SG_SH_REC_CAP  8192
#define SG_SH_RECS     4          /* left track, right track, left fill, right fill */
#define JR_SHADE_REC   1          /* tells the host fixtures the recorded arcs exist */
static uint8_t *s_shade_rec;
static struct {
    int32_t start, sweep;
    bool ok;                      /* the block holds a record for (start, sweep) */
    sp_settle_t gate;
} s_shade_k[SG_SH_RECS];

static bool shade_rec_make(int i, int32_t start, int32_t sweep)
{
    s_shade_k[i].start = start;
    s_shade_k[i].sweep = sweep;
    s_shade_k[i].ok = glass_shape_record(s_shade_rec + (size_t)i * SG_SH_REC_CAP, SG_SH_REC_CAP,
                                         SP_CX * 16, SP_CY * 16, SG_SH_R_IN16, SG_SH_R_OUT16, start,
                                         sweep, true) != 0u;
    return s_shade_k[i].ok;
}

static void shade_rec_alloc(void)
{
    if (s_shade_rec == NULL) {
        s_shade_rec = heap_caps_malloc((size_t)SG_SH_RECS * SG_SH_REC_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    memset(s_shade_k, 0, sizeof s_shade_k);
    if (s_shade_rec != NULL) {
        shade_rec_make(0, (GLASS_TURN_Q16 / 2 - SG_SH_HALF_Q16) & 0xFFFF, 2 * SG_SH_HALF_Q16);
        shade_rec_make(1, (GLASS_TURN_Q16 - SG_SH_HALF_Q16) & 0xFFFF, 2 * SG_SH_HALF_Q16);
    }
}

/* The record of block i for (start, sweep), or NULL to draw the arc itself. The
 * tracks' blocks are fixed; a fill's is made once the value has held. */
static const uint8_t *shade_rec_get(int i, int32_t start, int32_t sweep, bool fill)
{
    if (s_shade_rec == NULL) {
        return NULL;
    }
    if (!(s_shade_k[i].ok && s_shade_k[i].start == start && s_shade_k[i].sweep == sweep)) {
        if (!fill || !sp_settled(&s_shade_k[i].gate, start, sweep) || !shade_rec_make(i, start, sweep)) {
            return NULL;
        }
    }
    return s_shade_rec + (size_t)i * SG_SH_REC_CAP;
}

static inline void shade_arc_row(uint16_t *row, int y, int lo, int hi, bool swap, const uint8_t *rec,
                                 int32_t start, int32_t sweep, uint16_t color, uint8_t alpha)
{
    if (rec != NULL) {
        glass_ring_replay_row(row, y, lo, hi, rec, color, alpha, swap);
    } else {
        glass_arc_row(row, y, lo, hi, SP_CX * 16, SP_CY * 16, SG_SH_R_IN16, SG_SH_R_OUT16, start,
                      sweep, color, alpha, swap);
    }
}

static void sp_draw_shade(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int st, uint32_t ent, uint32_t breath_ms)
{
    if (st <= 0) {
        return;
    }
    const uint32_t w = __atomic_load_n(&s_status_word, __ATOMIC_ACQUIRE);
    int vol = (int)(w & 0xFFu);
    if (vol > 100) {
        vol = 100;
    }
    const int brt = shade_light(w);
    const bool swap = ctx->board.swap_color_bytes;
    const bool muted = sp_privacy_muted();
    const uint16_t priv = muted ? SP_C_GOLD : SP_C_CYAN;
    const int cx16 = SP_CX * 16;

    /* the arcs, swept to their values */
    const uint32_t f_vol = glass_stagger_q16(ent, 0, 600);
    const uint32_t f_lgt = glass_stagger_q16(ent, 90, 600);
    const int32_t span = 2 * SG_SH_HALF_Q16;
    const int32_t vsweep = (int32_t)(((int64_t)((int64_t)span * vol / 100) * f_vol) >> 16);
    const int32_t lsweep = (int32_t)(((int64_t)((int64_t)span * brt / 100) * f_lgt) >> 16);
    const int32_t l_start = (GLASS_TURN_Q16 / 2 - SG_SH_HALF_Q16) & 0xFFFF;      /* lower left, up to upper left */
    const int32_t r_track = (GLASS_TURN_Q16 - SG_SH_HALF_Q16) & 0xFFFF;          /* upper right, down to lower right */
    const int32_t r_start = (SG_SH_HALF_Q16 - lsweep) & 0xFFFF;                  /* lower right, up */
    const uint8_t a_track = (uint8_t)((st * 44) / 255);

    /* the numbers count up with their arcs */
    char vbuf[SP_COL_MAX], lbuf[SP_COL_MAX];
    const int vlen = sp_pct(vbuf, 0, (int)sizeof vbuf, (uint32_t)glass_count_up(vol, f_vol));
    const int llen = sp_pct(lbuf, 0, (int)sizeof lbuf, (uint32_t)glass_count_up(brt, f_lgt));

    const uint32_t t_ic = 150, t_state = 250, t_hint = 320, t_keys = 400;
    const int a_ic = sg_ent_alpha(st, ent, t_ic);
    sg_ico_t i_spk, i_sun;
    sg_ico_make(&i_spk, GLASS_ICON_SPEAKER, GLASS_ICON_SM, SP_CX - SG_SH_CLUSTER_DX,
                SG_SH_ICON_Y + sg_ent_rise(ent, t_ic), SP_C_CYAN, a_ic);
    sg_ico_make(&i_sun, GLASS_ICON_SUN, GLASS_ICON_SM, SP_CX + SG_SH_CLUSTER_DX,
                SG_SH_ICON_Y + sg_ent_rise(ent, t_ic), SP_C_AMBER, a_ic);
    sg_txt_t t_vol, t_lgt, t_state_t, t_hint_t, t_keys_t;
    sg_txt_make(&t_vol, GLASS_FONT_TEXT, vbuf, vlen, SP_CX - SG_SH_CLUSTER_DX,
                SG_SH_NUM_Y + sg_ent_rise(ent, t_ic), 1, SP_C_INK, a_ic);
    sg_txt_make(&t_lgt, GLASS_FONT_TEXT, lbuf, llen, SP_CX + SG_SH_CLUSTER_DX,
                SG_SH_NUM_Y + sg_ent_rise(ent, t_ic), 1, SP_C_INK, a_ic);
    sg_txt_make(&t_state_t, GLASS_FONT_TEXT, muted ? "MUTED" : "LISTENING", muted ? 5 : 9, SP_CX,
                SG_SH_STATE_Y + sg_ent_rise(ent, t_state), 2, priv, sg_ent_alpha(st, ent, t_state));
    sg_txt_make(&t_hint_t, GLASS_FONT_LABEL, muted ? "TAP TO LISTEN" : "TAP TO MUTE",
                muted ? 13 : 11, SP_CX, SG_SH_HINT_Y + sg_ent_rise(ent, t_hint), 3, SP_C_GREY,
                sg_ent_alpha(st, ent, t_hint));
    sg_txt_make(&t_keys_t, GLASS_FONT_LABEL, "PWR LISTEN   BOOT CLOSE", 23, SP_CX,
                SG_SH_KEYS_Y + sg_ent_rise(ent, t_keys), 2, SP_C_GREY,
                (sg_ent_alpha(st, ent, t_keys) * 150) / 255);

    /* the button: grows from 71% as it fades in */
    const uint32_t f_btn = glass_stagger_q16(ent, 0, 420);
    const int a_btn = (int)glass_frac_alpha((uint8_t)st, f_btn);
    const int32_t br16 = (int32_t)(((int64_t)SP_BTN_R * 16 * (182 + (int)((74u * f_btn) >> 16))) >> 8);
    sg_ico_t i_mic;
    sg_ico_make(&i_mic, muted ? GLASS_ICON_MIC_MUTED : GLASS_ICON_MIC, GLASS_ICON_LG, SP_CX,
                SP_BTN_CY - glass_icon_px(GLASS_ICON_LG) / 2, priv,
                (int)glass_frac_alpha((uint8_t)st, glass_stagger_q16(ent, 120, 300)));
    int32_t grow16 = 0;
    int breath_a = 0;
    if (muted) {
        sg_breath(breath_ms, &grow16, &breath_a);
        breath_a = (breath_a * a_btn) / 255;
    }
    const int32_t bcy16 = SP_BTN_CY * 16;
    const int btn_y0 = SP_BTN_CY - SP_BTN_R - 14, btn_y1 = SP_BTN_CY + SP_BTN_R + 14;   /* the breath reaches R+12 */

    /* at rest: the arcs replayed from their records */
    const uint8_t *rec_tl = shade_rec_get(0, l_start, span, false);
    const uint8_t *rec_tr = shade_rec_get(1, r_track, span, false);
    const uint8_t *rec_vf = (f_vol >= GLASS_ONE_Q16 && vsweep > 0) ? shade_rec_get(2, l_start, vsweep, true) : NULL;
    const uint8_t *rec_lf = (f_lgt >= GLASS_ONE_Q16 && lsweep > 0) ? shade_rec_get(3, r_start, lsweep, true) : NULL;
    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        if (y >= SG_SH_Y0 && y <= SG_SH_Y1) {
            shade_arc_row(row, y, lo, hi, swap, rec_tl, l_start, span, SP_C_CYAN, a_track);
            shade_arc_row(row, y, lo, hi, swap, rec_tr, r_track, span, SP_C_AMBER, a_track);
            if (vsweep > 0) {
                shade_arc_row(row, y, lo, hi, swap, rec_vf, l_start, vsweep, SP_C_CYAN, (uint8_t)st);
            }
            if (lsweep > 0) {
                shade_arc_row(row, y, lo, hi, swap, rec_lf, r_start, lsweep, SP_C_AMBER, (uint8_t)st);
            }
        }
        if (y >= btn_y0 && y <= btn_y1) {
            if (breath_a > 0) {
                glass_ring_row(row, y, lo, hi, cx16, bcy16, SP_BTN_R * 16 + 80 + grow16,
                               SP_BTN_R * 16 + 128 + grow16, 0, GLASS_TURN_Q16, priv,
                               (uint8_t)breath_a, swap);
            }
            glass_disc_row(row, y, lo, hi, cx16, bcy16, br16, priv,
                           (uint8_t)((a_btn * 34) / 255), swap);
            glass_ring_row(row, y, lo, hi, cx16, bcy16, br16 - 48, br16, 0, GLASS_TURN_Q16, priv,
                           (uint8_t)a_btn, swap);
        }
        sg_ico_row(row, y, lo, hi, swap, &i_mic);
        sg_ico_row(row, y, lo, hi, swap, &i_spk);
        sg_ico_row(row, y, lo, hi, swap, &i_sun);
        sg_txt_row(row, y, lo, hi, swap, &t_vol);
        sg_txt_row(row, y, lo, hi, swap, &t_lgt);
        sg_txt_row(row, y, lo, hi, swap, &t_state_t);
        sg_txt_row(row, y, lo, hi, swap, &t_hint_t);
        sg_txt_row(row, y, lo, hi, swap, &t_keys_t);
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
/* The rail's track never moves, so what glass_ring_row would do for it is
 * recorded once (about 10 KB, in PSRAM, at start) and replayed: the identical
 * fills and blends without the per-row geometry. NULL (no PSRAM, or the record
 * did not fit) falls back to drawing the ring the long way. Internal RAM: this
 * pointer, 4 bytes. */
static uint8_t *s_orbit_rec;
#define JR_ORBIT_REC 1      /* tells the host fixtures the recorded rail exists */

static void orbit_rec_build(void)
{
    if (s_orbit_rec != NULL) {
        return;
    }
    const size_t need = glass_ring_record(NULL, 0, SP_CX * 16, SP_CY * 16,
                                          SP_ORB_TRACK_IN * 16, SP_ORB_TRACK_OUT * 16);
    if (need == 0u) {
        return;
    }
    uint8_t *buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return;
    }
    if (glass_ring_record(buf, need, SP_CX * 16, SP_CY * 16,
                          SP_ORB_TRACK_IN * 16, SP_ORB_TRACK_OUT * 16) != need) {
        heap_caps_free(buf);
        return;
    }
    s_orbit_rec = buf;
}

/* The rows the lit mark (a 10/256-turn arc at the track's radius, round caps)
 * can touch, so the other ~390 rows do not ask the glass layer to find that out
 * (a geometry lookup each). a_act is the mark's centre in 1/256 turns, 0 at
 * 3 o'clock, clockwise. The bound is the ends' and any cardinal crossing's
 * height, plus 6 px for the cap radius and the anti-aliasing ramp. */
static void orbit_mark_rows(int a_act, int *y_lo, int *y_hi)
{
    const int a0 = a_act - 5, a1 = a_act + 5;
    int s_min = sp_sin(a0), s_max = s_min;
    const int s1 = sp_sin(a1);
    if (s1 < s_min) {
        s_min = s1;
    }
    if (s1 > s_max) {
        s_max = s1;
    }
    const int n0 = a0 & 255;
    if (((SP_A_TOP - n0) & 255) <= 10) {
        s_min = -32768;               /* the top of the circle is inside the mark */
    }
    if (((64 - n0) & 255) <= 10) {
        s_max = 32768;                /* ... or the bottom */
    }
    /* the lowest point is at the inner radius where the mark is above the
     * centre line, the outer where it is below, so take both radii */
    const int lo_in = (s_min * SP_ORB_MARK_IN) >> 15, lo_out = (s_min * SP_ORB_MARK_OUT) >> 15;
    const int hi_in = (s_max * SP_ORB_MARK_IN) >> 15, hi_out = (s_max * SP_ORB_MARK_OUT) >> 15;
    *y_lo = SP_CY + (lo_in < lo_out ? lo_in : lo_out) - 6;
    *y_hi = SP_CY + (hi_in > hi_out ? hi_in : hi_out) + 6;
}

static void sp_draw_orbit(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int st)
{
    const bool muted = sp_privacy_muted();
    const bool swap = ctx->board.swap_color_bytes;
    const uint16_t cool = muted ? SP_C_GOLD_DIM : SP_C_CYAN_DIM;
    const uint16_t hot = muted ? SP_C_GOLD : SP_C_CYAN;

    /* One mark per screen the ring shows RIGHT NOW (DESK only while live) and
     * the active mark at the angle sp_fade_tick eased for this frame, which
     * already carries the slide and the re-spacing residue. Rail, dots and mark
     * are glass primitives like the screens inside them; the dot centres are
     * worked out once, not once per row. */
    const int n = sp_ring_n(sp_desk_live());
    const int a_act = (s_orbit_a16 + 8) >> 4;
    const int32_t mark_start = (int32_t)(((a_act - 5) * 256) & 0xFFFF);
    int mark_y_lo, mark_y_hi;
    orbit_mark_rows(a_act, &mark_y_lo, &mark_y_hi);
    int dx16[16], dy16[16];
    const int nd = n < 16 ? n : 16;
    for (int i = 0; i < nd; ++i) {
        const int a = SP_A_TOP + (i * 256) / n;
        dx16[i] = (SP_CX + ((sp_cos(a) * SP_ORB_R) >> 15)) * 16;
        dy16[i] = (SP_CY + ((sp_sin(a) * SP_ORB_R) >> 15)) * 16;
    }

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        /* The whole ring; the glass ring takes the rows it touches and no more. */
        if (s_orbit_rec != NULL) {
            glass_ring_replay_row(row, y, lo, hi, s_orbit_rec, SP_C_PLATE, (uint8_t)st, swap);
        } else {
            glass_ring_row(row, y, lo, hi, SP_CX * 16, SP_CY * 16, SP_ORB_TRACK_IN * 16,
                           SP_ORB_TRACK_OUT * 16, 0, GLASS_TURN_Q16, SP_C_PLATE, (uint8_t)st, swap);
        }
        for (int i = 0; i < nd; ++i) {
            if (y >= dy16[i] / 16 - 4 && y <= dy16[i] / 16 + 4) {
                glass_disc_row(row, y, lo, hi, dx16[i], dy16[i], 40, cool, (uint8_t)st, swap);
            }
        }
        if (y >= mark_y_lo && y <= mark_y_hi) {
            glass_arc_row(row, y, lo, hi, SP_CX * 16, SP_CY * 16, SP_ORB_MARK_IN * 16,
                          SP_ORB_MARK_OUT * 16, mark_start, 10 * 256, hot, (uint8_t)st, swap);
        }
    }
}

/* The whole shell for one strip, in z-order: veil, the outgoing and incoming
 * spaces cross-dissolving as they slide, context sheet, shade, then an orbital
 * position marker that fades out with the side surfaces under the shade. */
/* THE DIAL IS THE SCREEN, NOT ITS BACKDROP. The veil exists to push the
 * reactor back behind a ring screen's content. On WATCH with a baked dial
 * the art IS the content, and a half-brightness Submariner is a broken one
 * (measured 2026-09-02 on the panel: the 12 o'clock lume at 62 against 130
 * in the art, every dial "not black" but dim). A sheet or the shade over the
 * watch still veils it, as anywhere else; a slide into WATCH keeps the
 * outgoing screen's veil and fades it with the slide. */
static bool watch_art_is_content(void)
{
    const jr_face_t dial = watch_dial_face();
    return s_space_to == (uint8_t)JR_DISPLAY_SPACE_WATCH &&
           dial != JR_FACE_COUNT && s_display.shown_face == dial &&
           s_detail_ease == 0 && s_shade_ease == 0;
}

/* The veil that does not stop at a quarter: weight k 0..32 takes each channel
 * from untouched to zero. A parked backdrop is already black, so the caller
 * skips it; a frame between "black is complete" and "the presenter parked"
 * still has face pixels and clears them here. */
static inline uint16_t sp_fade_to_black565(uint16_t v, bool swap, int k)
{
    if (swap) {
        v = (uint16_t)((v >> 8) | (v << 8));
    }
    const uint32_t m = (uint32_t)(32 - k);
    uint16_t out = (uint16_t)((((v & 0xF81Fu) * m) >> 5) & 0xF81Fu);
    out |= (uint16_t)((((v & 0x07E0u) * m) >> 5) & 0x07E0u);
    if (swap) {
        out = (uint16_t)((out >> 8) | (out << 8));
    }
    return out;
}

static void sp_veil_black(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int k)
{
    const bool swap = ctx->board.swap_color_bytes;
    for (int y = y1; y < y2; ++y) {
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        if (k >= 32) {
            memset(row + lo, 0, (size_t)(hi - lo + 1) * sizeof(uint16_t));
            continue;
        }
        for (int x = lo; x <= hi; ++x) {
            if (row[x] != 0U) {
                row[x] = sp_fade_to_black565(row[x], swap, k);
            }
        }
    }
}

static void apply_space_overlay(const jr_display_ctx_t *ctx, int y1, int y2,
                                uint16_t *pixels)
{
    if (!s_space_on) {
        return;            /* JARVIS at rest: the layer costs one compare */
    }
    const bool settled = s_space_from == s_space_to || s_space_ease >= 256;
    /* A complete black makes any other veil moot, and a parked backdrop has
     * no face pixels to fold at all. */
    const int veil = ((settled && watch_art_is_content()) || s_space_black >= 256)
                         ? 0 : (s_space_veil * 32) >> 8;
    int64_t sm = split_now();
    if (veil > 0) {
        sp_veil(ctx, y1, y2, pixels, veil);
    }
    if (s_space_black > 0 && !s_parked) {
        sp_veil_black(ctx, y1, y2, pixels, (s_space_black * 32) >> 8);
    }
    split_mark(JR_DISPLAY_SPLIT_SPACE_VEIL, &sm);
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
                              (focal * (256 - e)) / 256, SP_ENTRY_TOTAL_MS);
            }
            sp_draw_space(ctx, y1, y2, pixels, s_space_to,
                          s_space_dir * (SP_SLIDE_PX - slide),
                          (focal * e) / 256, s_entry_ms);
        }
        split_mark(JR_DISPLAY_SPLIT_SPACE_SCREEN, &sm);
        if (s_detail_ease > 0) {
            sp_draw_detail(ctx, y1, y2, pixels,
                           (under * s_detail_ease) / 256);
            split_mark(JR_DISPLAY_SPLIT_SPACE_SHEET, &sm);
        }
    }
    if (shade > 0) {
        sp_draw_shade(ctx, y1, y2, pixels, shade, s_shade_ms, s_breath_ms);
        split_mark(JR_DISPLAY_SPLIT_SPACE_SHADE, &sm);
    }
    sp_draw_orbit(ctx, y1, y2, pixels, under);
    split_mark(JR_DISPLAY_SPLIT_SPACE_ORBIT, &sm);
}

static jr_display_rest_mode_t rest_eligible(const jr_display_ctx_t *ctx)
{
    const uint32_t env = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    const uint32_t request = __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
    const jr_face_t face = (jr_face_t)((request & JR_DISPLAY_CMD_FACE_MASK) >>
                                      JR_DISPLAY_CMD_FACE_SHIFT);
    const uint32_t nav = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    const uint32_t space = nav & NAV_SPACE_MASK;
    const int caption = __atomic_load_n(&s_caption_on, __ATOMIC_ACQUIRE);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t ripple = __atomic_load_n(&s_ripple_start_ms, __ATOMIC_ACQUIRE);
    const uint32_t bloom = __atomic_load_n(&s_bloom_start_ms, __ATOMIC_ACQUIRE);
    const jr_display_rest_mode_t mode = (jr_display_rest_mode_t)
        ((env & JR_DISPLAY_ENV_MODE_MASK) >> 10);
    const bool usb = (__atomic_load_n(&s_power_word, __ATOMIC_ACQUIRE) &
                      (1u << 24)) != 0U;
    const bool allowed = mode != JR_DISPLAY_REST_NONE && !(env & JR_DISPLAY_ENV_VOICE_BUSY) &&
           (mode != JR_DISPLAY_REST_DREAM || (!(env & (1u << 8)) && !usb)) &&
           (mode != JR_DISPLAY_REST_NIGHTSTAND || (usb && (env & JR_DISPLAY_ENV_USB))) &&
           !(request & JR_DISPLAY_CMD_BLANK) &&
           (face == JR_FACE_IDLE || face == JR_FACE_RESTING || face == JR_FACE_MUTED ||
            (face == JR_FACE_LISTENING && mode != JR_DISPLAY_REST_DREAM)) &&
           (space == JR_DISPLAY_SPACE_JARVIS || space == JR_DISPLAY_SPACE_WATCH) &&
           !(nav & NAV_OVL_MASK) &&
           !(diag_load(&ctx->shell_word) & (JR_DISPLAY_SHELL_SHADE | JR_DISPLAY_SHELL_AGENT)) &&
           __atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE) == 0 &&
           __atomic_load_n(&s_commit_pct, __ATOMIC_ACQUIRE) == 0U &&
           (!ripple || now - ripple >= HUD_RIPPLE_MS) &&
           (!bloom || now - bloom >= HUD_BLOOM_MS) &&
           !jr_display_canvas_active() && !jr_display_surface_is_active() &&
           (!caption || (caption & JR_DISPLAY_CAPTION_AMBIENT)) &&
           !atomic_load(&s_caption_pinned) &&
           (s_space_from == s_space_to || s_space_ease >= 256) &&
           s_detail_ease == 0 && s_shade_ease == 0 &&
           s_ask_ease == 0 && s_canvas_ease == 0 &&
           !glass_entry_animating(s_entry_ms, SP_ENTRY_TOTAL_MS) &&
           !sp_ota_ringing((jr_display_ota_state_t)
               (__atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xfu)) &&
           diag_load(&ctx->test_pattern) == JR_DISPLAY_TEST_OFF &&
           jr_display_hud_enabled();
    return allowed ? mode : JR_DISPLAY_REST_NONE;
}

static bool dream_eligible(const jr_display_ctx_t *ctx)
{
    return rest_eligible(ctx) == JR_DISPLAY_REST_DREAM;
}

/* WHISPER's cadence is only a ceiling. A muted, settled HOME has no moving
 * face to justify dirtying it on every engine period. Keep every foreground
 * owner and every finite fade on the normal animation path. WATCH and the
 * charging comet remain dynamic; clock/rim facts are explicitly refreshed. */
static bool quiet_eligible(const jr_display_ctx_t *ctx)
{
    const uint32_t env = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    const uint32_t request = __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
    const jr_face_t face = (jr_face_t)((request & JR_DISPLAY_CMD_FACE_MASK) >>
                                      JR_DISPLAY_CMD_FACE_SHIFT);
    const uint32_t nav = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    const int caption = __atomic_load_n(&s_caption_on, __ATOMIC_ACQUIRE);
    const uint32_t clock = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t ripple = __atomic_load_n(&s_ripple_start_ms, __ATOMIC_ACQUIRE);
    const uint32_t bloom = __atomic_load_n(&s_bloom_start_ms, __ATOMIC_ACQUIRE);
    return (env & (1u << 9)) && !(env & ((1u << 8) | JR_DISPLAY_ENV_VOICE_BUSY |
                                      JR_DISPLAY_ENV_MODE_MASK)) &&
           jr_display_render_fps() <= 6U && !(request & JR_DISPLAY_CMD_BLANK) &&
           (face == JR_FACE_IDLE || face == JR_FACE_RESTING || face == JR_FACE_MUTED) &&
           (nav & (NAV_SPACE_MASK | NAV_OVL_MASK)) == JR_DISPLAY_SPACE_JARVIS &&
           !(diag_load(&ctx->shell_word) & (JR_DISPLAY_SHELL_SHADE | JR_DISPLAY_SHELL_AGENT)) &&
           __atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE) == 0 &&
           __atomic_load_n(&s_commit_pct, __ATOMIC_ACQUIRE) == 0U &&
           (!ripple || now - ripple >= HUD_RIPPLE_MS) &&
           (!bloom || now - bloom >= HUD_BLOOM_MS) &&
           !jr_display_canvas_active() && !jr_display_surface_is_active() &&
           (!caption || (caption & JR_DISPLAY_CAPTION_AMBIENT)) &&
           !atomic_load(&s_caption_pinned) &&
           s_space_to == JR_DISPLAY_SPACE_JARVIS && s_space_prog == 256 &&
           s_space_serial_seen == (uint8_t)(nav >> NAV_SERIAL_SHIFT) &&
           s_detail_ease == 0 && s_shade_ease == 0 &&
           s_ask_ease == 0 && s_canvas_ease == 0 &&
           !glass_entry_animating(s_entry_ms, SP_ENTRY_TOTAL_MS) &&
           s_caption_prog == (caption ? 256 : 0) &&
           s_clock_prog == ((clock & (1u << 16)) ? 256 : 0) &&
           !sp_ota_ringing((jr_display_ota_state_t)
               (__atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xfu)) &&
           diag_load(&ctx->test_pattern) == JR_DISPLAY_TEST_OFF &&
           jr_display_hud_enabled();
}

static void companion_view(jr_display_rest_mode_t mode, jr_companion_view_t *view)
{
    const uint32_t env = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    const uint32_t clock = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
    const jr_companion_input_t input = {
        .requested = mode == JR_DISPLAY_REST_NIGHTSTAND ?
            JR_COMPANION_NIGHTSTAND : JR_COMPANION_GLANCE,
        .usb_present = (__atomic_load_n(&s_power_word, __ATOMIC_ACQUIRE) & (1u << 24)) != 0U,
        .idle = true,
        .privacy_muted = (env & (1u << 9)) != 0U,
        .charging = (env & (1u << 8)) != 0U,
        .time_valid = (clock & (1u << 16)) != 0U,
        .hour = (uint8_t)(clock >> 8), .minute = (uint8_t)clock,
        .battery_percent = (uint8_t)env,
        .next_state = JR_COMPANION_NEXT_UNKNOWN,
    };
    (void)jr_companion_build(&input, view);
    view->night = mode == JR_DISPLAY_REST_NIGHTSTAND && input.time_valid &&
                  (env & JR_DISPLAY_ENV_NIGHT) != 0U;
}

static void dream_clock_text(char text[6], uint32_t clock)
{
    if (!(clock & (1u << 16))) {
        memcpy(text, "--:--", 6U);
        return;
    }
    const unsigned hh = (clock >> 8) & 0xffu;
    const unsigned mm = clock & 0xffu;
    text[0] = (char)('0' + hh / 10u);
    text[1] = (char)('0' + hh % 10u);
    text[2] = ':';
    text[3] = (char)('0' + mm / 10u);
    text[4] = (char)('0' + mm % 10u);
    text[5] = '\0';
}

static void apply_rest_rims(jr_display_ctx_t *ctx, int y1, int y2, uint16_t *pixels)
{
    memset(pixels, 0, (size_t)(y2 - y1) * HUD_W * sizeof *pixels);
    const uint32_t word = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    const hud_env_t env = {
        .face = HUD_FACE_IDLE,
        .batt_pct = (uint8_t)word,
        .charging = (word & (1u << 8)) != 0U,
        .privacy_muted = (word & (1u << 9)) != 0U,
    };
    hud_overlay_frame(pixels, y1, y2 - y1, 0U, ctx->board.swap_color_bytes, &env);
}

static void apply_dream_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                                 uint16_t *pixels)
{
    apply_rest_rims(ctx, y1, y2, pixels);
    char text[6];
    dream_clock_text(text, __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE));
    const uint16_t ink = panel_order_color(ctx, 0x2104u);
    for (int y = y1; y < y2; ++y) {
        sp_text_row(pixels + (size_t)(y - y1) * HUD_W, y, text, 5,
                    (HUD_W - 5 * 18) / 2, (HUD_H - 21) / 2, 3, ink);
    }
}

static bool apply_companion_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                                    uint16_t *pixels, jr_display_rest_mode_t mode)
{
    /* Same authoritative normal-colour rims as DREAM; fixed charge phase. */
    apply_rest_rims(ctx, y1, y2, pixels);
    jr_companion_view_t view;
    companion_view(mode, &view);
    const uint16_t ink = panel_order_color(ctx, view.ink_rgb565);
    for (unsigned i = 0; i < JR_COMPANION_LINE_COUNT; ++i) {
        const jr_companion_line_t *line = &view.lines[i];
        const int len = (int)strnlen(line->text, sizeof line->text);
        if (len == 0) { continue; }
        for (int y = y1; y < y2; ++y) {
            sp_text_row(pixels + (size_t)(y - y1) * HUD_W, y, line->text, len,
                        line->x, line->y, line->scale, ink);
        }
    }
    return strcmp(view.lines[0].text, "--:--") != 0 &&
           y1 < view.lines[0].y + 7 * view.lines[0].scale &&
           y2 > view.lines[0].y;
}

/* True when apply_clock_overlay is about to clear the whole shell disc on this
 * strip, which makes everything the shell drew into that disc a dead write:
 * the clear is a memset of the disc sp_clip bounds every shell primitive to
 * (the same r214 chord per row), so the veil, the black fold, the outgoing and
 * incoming screens and the orbit rail were all painted for nothing. Measured
 * on the host as 36 % of a MINIMAL frame and 47 % of a JARVIS-style one. The
 * conditions are the clear's own, copied from apply_clock_overlay: the clock
 * is up, neither the detail sheet nor the shade holds the glass, and the dial
 * is not baked art (a baked dial is not cleared, so the shell stays under it).
 * Must be called on the same side of the `choice` branch as the clock. */
static bool watch_clear_follows(const jr_display_ctx_t *ctx)
{
    if ((jr_display_space_t)(__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) &
                             NAV_SPACE_MASK) != JR_DISPLAY_SPACE_WATCH) {
        return false;
    }
    if (s_clock_ease <= 0 || s_detail_ease > 0 || s_shade_ease > 0) {
        return false;
    }
    const jr_face_t dial = watch_dial_face();
    return !(dial != JR_FACE_COUNT && ctx->shown_face == dial);
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
    if (dream_eligible(ctx)) {
        apply_dream_overlay(ctx, y1, y2, pixels);
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
    int64_t sm = esp_timer_get_time();
    const uint32_t now_ms = (uint32_t)(sm / 1000);
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
    split_mark(JR_DISPLAY_SPLIT_HUD_FRAME, &sm);

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
        split_mark(JR_DISPLAY_SPLIT_HUD_ASK, &sm);
    } else {
        /* The spatial shell owns the glass between the face and the caption:
         * the four spaces, the context sheet, and the control shade. It is
         * skipped entirely while an ask is up (the ask owns the glass) and
         * draws nothing at all in JARVIS at rest. */
        if (!watch_clear_follows(ctx)) {
            apply_space_overlay(ctx, y1, y2, pixels);
        }
        split_mark(JR_DISPLAY_SPLIT_HUD_SPACE, &sm);

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
        split_mark(JR_DISPLAY_SPLIT_HUD_CLOCK, &sm);
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
        split_mark(JR_DISPLAY_SPLIT_HUD_OTA, &sm);
        apply_caption_overlay(ctx, y1, y2, pixels);    /* gates on its ease */
        split_mark(JR_DISPLAY_SPLIT_HUD_CAPTION, &sm);
        if (ae > 0 && s_ask_shown_n > 0) {
            apply_ask_overlay(ctx, y1, y2, pixels, s_ask_shown_n, ae);
            hud_overlay_choices(pixels, y1, nrows,
                                ctx->board.swap_color_bytes, s_choices,
                                s_ask_shown_n, s_choice_selected, as);
            split_mark(JR_DISPLAY_SPLIT_HUD_ASK, &sm);
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
        split_mark(JR_DISPLAY_SPLIT_HUD_BLOOM, &sm);
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
     * of every strip through panel_order_color() to composite a shade that is no
     * longer drawn here. */
    const uint16_t accent = panel_order_color(ctx,
                                              agent_native_color(agent_state));
    const int width = x2 - x1;
    const int rim_out = 2 * 230, rim_in = 2 * 224;
    const int cx2 = (int)ctx->board.width - 1;      /* dx = 2x - cx2 */

    for (int y = y1; y < y2; ++y) {
        const int dy = 2 * y - ((int)ctx->board.height - 1);
        if (dy * dy > rim_out * rim_out) {
            continue;                  /* strip row misses the rim entirely */
        }
        /* The band is at most two short runs a row, so bound them with integer
         * square roots and test only inside them (the old loop tested every
         * pixel of the row: 217,000 multiplies a frame to light 8,000). The
         * outer bound is widened by a pixel and the inner hole narrowed by
         * one, so the exact r2 test below still decides every pixel and the
         * output cannot differ. */
        const int a = sp_isqrt(rim_out * rim_out - dy * dy);
        int lo = cx2 - a - 2;
        lo = lo < 0 ? 0 : lo >> 1;
        int hi = (cx2 + a + 2) >> 1;
        int hole_lo = 1, hole_hi = 0;            /* empty: hole_lo > hole_hi */
        const int i2 = rim_in * rim_in - dy * dy;
        if (i2 > 0) {
            const int h = sp_isqrt(i2) - 3;      /* |dx| <= h is surely inside r_in */
            if (h >= 0 && h <= cx2) {
                hole_lo = (cx2 - h + 1) >> 1;
                hole_hi = (cx2 + h) >> 1;
            }
        }
        if (lo < x1) {
            lo = x1;
        }
        if (hi > x2 - 1) {
            hi = x2 - 1;
        }
        uint16_t *row = pixels + (size_t)(y - y1) * (size_t)width;
        for (int run = 0; run < 2; ++run) {
            const int rlo = run == 0 ? lo : (hole_lo > hole_hi ? hi + 1 : (hole_hi + 1 > lo ? hole_hi + 1 : lo));
            const int rhi = run == 0 ? (hole_lo > hole_hi ? hi : (hole_lo - 1 < hi ? hole_lo - 1 : hi)) : hi;
            for (int x = rlo; x <= rhi; ++x) {
                const int dx = 2 * x - cx2;
                const int r2 = dx * dx + dy * dy;
                if (r2 < rim_in * rim_in || r2 > rim_out * rim_out) {
                    continue;
                }
                /* every sector lit (a finished job): nothing left to ask */
                if (completed >= 8 || challenge_sector_from_vector(dx, dy) < completed) {
                    row[x - x1] = accent;
                }
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

static bool IRAM_ATTR panel_request_matches(uint32_t word)
{
    return __atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE) == word;
}

static bool panel_settle(jr_display_ctx_t *ctx, uint32_t word)
{
    while (esp_timer_get_time() - ctx->panel_sleep_attempt_us < 120000) {
        if (!panel_request_matches(word)) { return false; }
        vTaskDelay(pdMS_TO_TICKS(5) + 1U);
    }
    return panel_request_matches(word);
}

static esp_err_t panel_sleep_command(jr_display_ctx_t *ctx, uint32_t word, bool sleep)
{
    if (ctx->panel_sleep_attempted && ctx->panel_last_sleep != sleep &&
        !panel_settle(ctx, word)) { return ESP_ERR_INVALID_STATE; }
    if (!panel_request_matches(word)) { return ESP_ERR_INVALID_STATE; }
    /* A failed transport may still have reached the controller. Preserve the
     * opposite-command exclusion even on an uncertain acknowledgement. */
    ctx->panel_sleep_attempted = true;
    ctx->panel_last_sleep = sleep;
    ctx->panel_sleep_attempt_us = esp_timer_get_time();
    return jarvis_board_display_set_sleep(sleep);
}

/* Sole command owner: invoked on gfx's render task before a strip. False
 * means no DMA is submitted; the caller releases that flush exactly once. */
static bool panel_service(jr_display_ctx_t *ctx)
{
    const uint32_t word = __atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE);
    const uint32_t state = word & 3U;
    if (state == 0U) { return true; }
    if (state == 2U) { return false; }
    if (ctx->panel_prepared && ctx->panel_prepared_word == word) {
        return state == 3U;
    }
    esp_err_t err;
    if (state == 1U) {
        if (!ctx->panel_touched || ctx->panel_prepared) {
            ctx->panel_restore_brightness = atomic_load(&s_brightness_have);
            ctx->panel_touched = true;
            ctx->panel_prepared = false;
        }
        err = jarvis_board_set_brightness(0U);
        if (err == ESP_OK && panel_request_matches(word)) {
            atomic_store(&s_brightness_have, 0U);
            err = esp_lcd_panel_disp_on_off(ctx->board.panel, false);
        }
        if (err == ESP_OK && panel_request_matches(word)) {
            err = panel_sleep_command(ctx, word, true);
        }
        if (err == ESP_OK && panel_settle(ctx, word)) {
            uint32_t expected = word;
            (void)__atomic_compare_exchange_n(&s_panel_off, &expected,
                (word & ~3U) | 2U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
    } else {
        err = panel_sleep_command(ctx, word, false);
        if (err == ESP_OK && panel_settle(ctx, word)) {
            err = esp_lcd_panel_disp_on_off(ctx->board.panel, true);
            if (err == ESP_OK && panel_request_matches(word)) {
                const uint8_t brightness = ctx->panel_touched ?
                    ctx->panel_restore_brightness : atomic_load(&s_brightness_have);
                err = jarvis_board_set_brightness(brightness);
                if (err == ESP_OK && panel_request_matches(word)) {
                    atomic_store(&s_brightness_have, brightness);
                    atomic_store(&s_brightness_shown, brightness);
                    ctx->panel_prepared_word = word;
                    ctx->panel_prepared = true;
                    return true; /* still needs its matching completed strip */
                }
            }
        }
    }
    if (err != ESP_OK) {
        __atomic_store_n(&ctx->last_error, err, __ATOMIC_RELAXED);
        diag_inc(&ctx->flush_errors);
    }
    return false;
}

static void IRAM_ATTR transfer_publish(jr_display_ctx_t *ctx, display_transfer_t *tag)
{
    uint32_t expected = TRANSFER_RESERVED | TRANSFER_OK | TRANSFER_DONE;
    if (!__atomic_compare_exchange_n(&tag->flags, &expected,
        expected | TRANSFER_PUBLISHED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (tag->glance_time) {
        uint32_t empty = 0U;
        if (__atomic_compare_exchange_n(&ctx->first_glance_time_transfer_state,
            &empty, 2U, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            diag_store(&ctx->first_glance_time_transfer_done_boot_ms, tag->done_ms);
            __atomic_store_n(&ctx->first_glance_time_transfer_state, 1U, __ATOMIC_RELEASE);
        }
    }
    uint32_t word = tag->on_word;
    if ((word & 3U) == 3U) {
        if (!__atomic_compare_exchange_n(&s_panel_off, &word, word & ~3U,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { return; }
        word &= ~3U;
    }
    if ((word & 3U) == 0U && panel_request_matches(word)) {
        __atomic_store_n(&s_panel_on_ack_word, word, __ATOMIC_RELAXED);
        __atomic_store_n(&s_panel_on_ack_valid, 1U, __ATOMIC_RELEASE);
    }
}

static void IRAM_ATTR transfer_discard_failed(jr_display_ctx_t *ctx)
{
    uint32_t tail = __atomic_load_n(&ctx->transfer_tail, __ATOMIC_ACQUIRE);
    while (tail != __atomic_load_n(&ctx->transfer_head, __ATOMIC_ACQUIRE)) {
        display_transfer_t *tag = &ctx->transfer_tags[tail % DISPLAY_TRANSFER_TAGS];
        if (!(__atomic_load_n(&tag->flags, __ATOMIC_ACQUIRE) & TRANSFER_FAILED)) { break; }
        (void)__atomic_compare_exchange_n(&ctx->transfer_tail, &tail, tail + 1U,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        tail = __atomic_load_n(&ctx->transfer_tail, __ATOMIC_ACQUIRE);
    }
}

static display_transfer_t *transfer_reserve(jr_display_ctx_t *ctx, bool glance_time)
{
    transfer_discard_failed(ctx);
    const uint32_t head = __atomic_load_n(&ctx->transfer_head, __ATOMIC_RELAXED);
    if (head - __atomic_load_n(&ctx->transfer_tail, __ATOMIC_ACQUIRE) >= DISPLAY_TRANSFER_TAGS) {
        return NULL;
    }
    display_transfer_t *tag = &ctx->transfer_tags[head % DISPLAY_TRANSFER_TAGS];
    const uint32_t flags = __atomic_load_n(&tag->flags, __ATOMIC_ACQUIRE);
    if (flags && !(flags & (TRANSFER_PUBLISHED | TRANSFER_FAILED))) { return NULL; }
    tag->glance_time = glance_time;
    tag->on_word = __atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE);
    tag->done_ms = 0U;
    __atomic_store_n(&tag->flags, TRANSFER_RESERVED, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->transfer_head, head + 1U, __ATOMIC_RELEASE);
    return tag;
}

static void IRAM_ATTR transfer_complete(jr_display_ctx_t *ctx)
{
    transfer_discard_failed(ctx);
    const uint32_t tail = __atomic_load_n(&ctx->transfer_tail, __ATOMIC_ACQUIRE);
    if (tail == __atomic_load_n(&ctx->transfer_head, __ATOMIC_ACQUIRE)) { return; }
    display_transfer_t *tag = &ctx->transfer_tags[tail % DISPLAY_TRANSFER_TAGS];
    tag->done_ms = (uint32_t)(esp_timer_get_time() / 1000);
    __atomic_fetch_or(&tag->flags, TRANSFER_DONE, __ATOMIC_ACQ_REL);
    transfer_publish(ctx, tag);
    uint32_t expected = tail;
    (void)__atomic_compare_exchange_n(&ctx->transfer_tail, &expected, tail + 1U,
        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static bool IRAM_ATTR panel_color_done(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t *event,
                                       void *user_ctx)
{
    (void)panel_io;
    (void)event;
    jr_display_ctx_t *ctx = (jr_display_ctx_t *)user_ctx;
    if (ctx && ctx->disp) {
        uint32_t expected = 0U;
        if (__atomic_compare_exchange_n(&ctx->first_transfer_state, &expected,
                                        2U, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            /* This callback can run before init finishes: gfx_disp_add may
             * immediately flush. Submission/snapshot bytes alone never mark
             * transfer completion. No optical or complete-frame claim. */
            diag_store(&ctx->first_transfer_done_boot_ms,
                       (uint32_t)(esp_timer_get_time() / 1000));
            __atomic_store_n(&ctx->first_transfer_state, 1U, __ATOMIC_RELEASE);
        }
        diag_inc(&ctx->flush_completions);
        transfer_complete(ctx);
        (void)gfx_disp_flush_ready(ctx->disp, true);
    }
    /* gfx_disp_flush_ready() performs the ISR yield itself. */
    return false;
}

/* The render's own clock. Every strip's software pass — canvas, test
 * pattern, the three overlays — is timed around its call in panel_flush and
 * added here; the DMA submit and the wait for it are the panel's cost, not
 * the render's. The frame count moves at each frame start, and once a
 * second the window's average is latched into render_frame_us, so one GET
 * of /api/display says how long the style on the glass takes to draw.
 * Render task only. */
static uint32_t s_render_win_ms;
static uint32_t s_render_win_us;
static uint32_t s_render_win_frames;

static void render_account(jr_display_ctx_t *ctx, bool frame_start,
                           uint32_t us, uint32_t now_ms)
{
    if (frame_start) {
        diag_inc(&ctx->render_frames);
        if (s_render_win_ms == 0u) {
            s_render_win_ms = now_ms;
        }
        /* the window closes on whole frames: latch before this one counts */
        if (s_render_win_frames > 0u && now_ms - s_render_win_ms >= 1000u) {
            diag_store(&ctx->render_frame_us,
                       s_render_win_us / s_render_win_frames);
            s_render_win_us = 0u;
            s_render_win_frames = 0u;
            s_render_win_ms = now_ms;
        }
        s_render_win_frames++;
    }
    __atomic_add_fetch(&ctx->render_us, us, __ATOMIC_RELAXED);
    s_render_win_us += us;
}

/* Charge the time since *mark to one compositing layer and move the mark. */
static inline void layer_mark(jr_display_ctx_t *ctx, int layer, int64_t *mark)
{
    const int64_t now = esp_timer_get_time();
    __atomic_add_fetch(&ctx->layer_us[layer], (uint32_t)(now - *mark),
                       __ATOMIC_RELAXED);
    *mark = now;
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
     * on the render task, and the command's tx_param waits out the previous
     * strip's DMA before it goes on the bus. This is the only place a panel
     * command may be issued. */
    if (!panel_service(ctx)) {
        (void)gfx_disp_flush_ready(disp, true);
        return;
    }
    if ((__atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE) & 3U) == 0U) {
        brightness_pump();
    }

    /* Frame-start latch: fades advance once per frame so all of a frame's
     * strips composite at one level (no banding across strip seams). */
    const bool frame_start = x1 == 0 && y1 == 0;
    if (frame_start) {
        overlay_fade_tick();
    }

    /* The gfx-owned DMA buffer is mutable until this submission. Diagnostics
     * may substitute a known pattern, then the mirror records the exact bytes
     * that will be handed to the CO5300—not the pre-render intent. */
    uint16_t *outbound = (uint16_t *)pixels;
    const int64_t t0 = esp_timer_get_time();
    const jr_display_rest_mode_t rest = rest_eligible(ctx);
    const bool rest_strip = rest != JR_DISPLAY_REST_NONE &&
                            x1 == 0 && x2 == HUD_W && y2 > y1;
    bool glance_time = false;
    if (rest_strip) {
        if (rest == JR_DISPLAY_REST_DREAM) {
            apply_dream_overlay(ctx, y1, y2, outbound);
        } else {
            const bool clock_strip = apply_companion_overlay(ctx, y1, y2, outbound, rest);
            glance_time = rest == JR_DISPLAY_REST_GLANCE && clock_strip;
        }
    } else {
        int64_t mark = t0;
        apply_dial(ctx, x1, y1, x2, y2, outbound);
        layer_mark(ctx, 0, &mark);
        apply_canvas(ctx, x1, y1, x2, y2, outbound);
        apply_test_pattern(ctx, x1, y1, x2, y2, outbound);
        layer_mark(ctx, 1, &mark);
        if (diag_load(&ctx->test_pattern) == JR_DISPLAY_TEST_OFF) {
        apply_hud_overlay(ctx, x1, y1, x2, y2, outbound);
        layer_mark(ctx, 2, &mark);
        apply_shell_overlay(ctx, x1, y1, x2, y2, outbound);
        layer_mark(ctx, 3, &mark);
        apply_surface_overlay(ctx, x1, y1, x2, y2, outbound);
        layer_mark(ctx, 4, &mark);
        }
    }
    render_account(ctx, frame_start, (uint32_t)(esp_timer_get_time() - t0),
                   (uint32_t)(t0 / 1000));
    display_transfer_t *tag = transfer_reserve(ctx, glance_time);
    if (!tag) {
        diag_store(&ctx->snapshot_frame_dropped, 1U);
        __atomic_store_n(&ctx->last_error, ESP_ERR_NO_MEM, __ATOMIC_RELAXED);
        diag_inc(&ctx->flush_errors);
        (void)gfx_disp_flush_ready(disp, true);
        return;
    }
    diag_inc(&ctx->flush_submissions);
    esp_err_t err = esp_lcd_panel_draw_bitmap(ctx->board.panel, x1, y1, x2, y2,
                                              outbound);
    if (err != ESP_OK) {
        const uint32_t flags = __atomic_fetch_or(&tag->flags, TRANSFER_FAILED, __ATOMIC_ACQ_REL);
        transfer_discard_failed(ctx);
        /* This strip never reached the panel, so the in-progress software
         * mirror must not be allowed to publish as physical evidence. */
        diag_store(&ctx->snapshot_frame_dropped, 1U);
        __atomic_store_n(&ctx->last_error, err, __ATOMIC_RELAXED);
        diag_inc(&ctx->flush_errors);
        /* Never strand the gfx renderer on a failed DMA submission. */
        if (!(flags & TRANSFER_DONE)) { (void)gfx_disp_flush_ready(disp, true); }
        return;
    }
    __atomic_fetch_or(&tag->flags, TRANSFER_OK, __ATOMIC_ACQ_REL);
    transfer_publish(ctx, tag);
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
    s_dial_blit = false;
    gfx_disp_refresh_all(ctx->disp);
    (void)gfx_emote_unlock(ctx->gfx);
    /* Reset the segment latch: a later present() of the SAME face must not
     * early-return in program_segment with the anim still stopped+hidden —
     * that left the panel permanently black on resume-from-blank. */
    ctx->last_end = -1;
    ctx->quiet_active = false;
    __atomic_store_n(&ctx->applied_bucket, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->blanked, true, __ATOMIC_RELAXED);
    return ESP_OK;
}

static bool brightness_pending(void)
{
    const uint8_t want = __atomic_load_n(&s_brightness_want, __ATOMIC_ACQUIRE);
    return atomic_load(&s_brightness_shown) != want ||
           atomic_load(&s_brightness_have) != want;
}

static uint32_t presenter_wait_ms(const jr_display_ctx_t *ctx)
{
    if ((ctx->dream_active || ctx->quiet_active) && !brightness_pending()) {
        return 1000U; /* diagnostics/TTL checks; no minute-clock invalidation */
    }
    return (1000U + jr_display_render_fps() - 1U) / jr_display_render_fps();
}

static bool presenter_refresh(jr_display_ctx_t *ctx)
{
    if (gfx_emote_lock(ctx->gfx) != ESP_OK) {
        return false;
    }
    gfx_disp_refresh_all(ctx->disp);
    (void)gfx_emote_unlock(ctx->gfx);
    return true;
}

static bool presenter_refresh_pending(jr_display_ctx_t *ctx, bool pattern_changed)
{
    const bool snapshot = diag_exchange(&ctx->snapshot_refresh_requested, 0U) != 0U;
    const uint32_t panel_state = diag_load(&s_panel_off) & 3U;
    if (pattern_changed || snapshot || panel_state == 1U || panel_state == 3U) {
        /* A stopped emote has no dirty frame. Screenshots and SLPIN still
         * require a flush through the sole panel-command owner. */
        if (!presenter_refresh(ctx)) {
            if (snapshot) {
                diag_store(&ctx->snapshot_refresh_requested, 1U);
            }
            return false;
        }
    }
    return true;
}

static void presenter_pattern_tick(jr_display_ctx_t *ctx, uint32_t *last_pattern)
{
    const uint32_t pattern = diag_load(&ctx->test_pattern);
    const bool changed = pattern != *last_pattern;
    if (presenter_refresh_pending(ctx, changed) && changed) {
        *last_pattern = pattern;
        ESP_LOGI(TAG, "panel test pattern=%u", (unsigned)pattern);
    }
}

static bool presenter_dream_tick(jr_display_ctx_t *ctx, uint32_t *last_word)
{
    const jr_display_rest_mode_t mode = rest_eligible(ctx);
    if (mode == JR_DISPLAY_REST_NONE) {
        if (ctx->dream_active) {
            ctx->dream_active = false;
            *last_word = UINT32_MAX; /* restore even the same face/amp */
            ctx->apply_retry_gate_ms = 0U;
        }
        return false;
    }
    const uint32_t clock = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE) &
                           JR_DISPLAY_CLOCK_MINUTE_MASK;
    const uint32_t rim = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE) &
                         JR_DISPLAY_ENV_RIM_MASK;
    jr_companion_view_t view = {0};
    if (mode != JR_DISPLAY_REST_DREAM) { companion_view(mode, &view); }
    if (!ctx->dream_active) {
        if (apply_blank(ctx) != ESP_OK) {
            return false;
        }
        ctx->dream_active = true;
        ctx->dream_clock_key = clock;
        ctx->dream_rim_key = rim;
        ctx->rest_mode = mode;
        ctx->rest_view = view;
        diag_inc(&ctx->state_changes);
    } else if (ctx->rest_mode != mode || ctx->dream_clock_key != clock ||
               ctx->dream_rim_key != rim ||
               !jr_companion_view_equal(&ctx->rest_view, &view) ||
               brightness_pending()) {
        if (presenter_refresh(ctx)) {
            ctx->dream_clock_key = clock;
            ctx->dream_rim_key = rim;
            ctx->rest_mode = mode;
            ctx->rest_view = view;
        }
    }
    return true;
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

/* THE ONE PLACE THE ANIM COMES UP. Render lock held, a face bound and its
 * segment set: start it and show it, unless the backdrop is parked, in which
 * case it stays stopped and hidden and the unpark does this later. Visibility
 * is therefore "a face is bound and the backdrop is not parked" and nothing
 * else decides it (the dial, blank and park paths hide; this shows). */
static esp_err_t anim_run(jr_display_ctx_t *ctx)
{
    if (s_parked) {
        return ESP_OK;
    }
    esp_err_t err = gfx_anim_start(ctx->anim);
    if (err == ESP_OK) {
        err = gfx_obj_set_visible(ctx->anim, true);
    }
    return err;
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
        err = anim_run(ctx);
    }
    (void)gfx_emote_unlock(ctx->gfx);
    if (err == ESP_OK) {
        ctx->last_end = (int)end;
        __atomic_store_n(&ctx->applied_bucket, bucket, __ATOMIC_RELAXED);
        diag_inc(&ctx->segment_sets);
    }
    return err;
}

static bool presenter_quiet_tick(jr_display_ctx_t *ctx, uint32_t *last_word)
{
    const uint32_t request = __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
    const jr_face_t face = (jr_face_t)((request & JR_DISPLAY_CMD_FACE_MASK) >>
                                      JR_DISPLAY_CMD_FACE_SHIFT);
    if (!quiet_eligible(ctx) || !ctx->active || ctx->active->last_frame < 1U ||
        ctx->loaded_face != face || ctx->blanked || s_parked) {
        if (ctx->quiet_active) {
            ctx->quiet_active = false;
            ctx->last_end = -1;
            *last_word = UINT32_MAX; /* restart even the same visible asset */
            ctx->apply_retry_gate_ms = 0U;
        }
        return false;
    }
    /* Bind/load through apply_face first, retaining missing-art fallback and
     * its retries. Never freeze an old foreground asset or a hidden blank. */
    const uint32_t clock = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
    const uint32_t env = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    const uint32_t caption = __atomic_load_n(&s_caption_revision, __ATOMIC_ACQUIRE);
    const uint32_t flush_errors = diag_load(&ctx->flush_errors);
    if (!ctx->quiet_active) {
        if (gfx_emote_lock(ctx->gfx) != ESP_OK) { return false; }
        if (!quiet_eligible(ctx) || request !=
            __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE)) {
            (void)gfx_emote_unlock(ctx->gfx);
            return false;
        }
        /* Pinned vendor callback prepares frame0 only while current<end.
         * [0,0] stops before decoding; [0,1], nonrepeat, decodes0 once and
         * retains that visible frame after stopping. No framebuffer copy. */
        esp_err_t err = gfx_anim_stop(ctx->anim);
        if (err == ESP_OK) {
            err = gfx_anim_set_segment(ctx->anim, 0U, 1U, face_fps(ctx->shown_face), false);
        }
        /* Not anim_run: test_quiet_vendor.py compiles exactly this sequence,
         * and the entry guard above already refuses it while parked. */
        if (err == ESP_OK) { err = gfx_anim_start(ctx->anim); }
        if (err == ESP_OK) { err = gfx_obj_set_visible(ctx->anim, true); }
        (void)gfx_emote_unlock(ctx->gfx);
        if (err != ESP_OK) {
            (void)program_segment(ctx, ctx->shown_face, 0U, true);
            return false;
        }
        ctx->quiet_active = true;
        ctx->quiet_clock_key = clock;
        ctx->quiet_env_key = env;
        ctx->quiet_caption_key = caption;
        ctx->quiet_flush_error_key = flush_errors;
        diag_inc(&ctx->segment_sets);
    } else if (ctx->quiet_clock_key != clock || ctx->quiet_env_key != env ||
               ctx->quiet_caption_key != caption ||
               ctx->quiet_flush_error_key != flush_errors || brightness_pending()) {
        if (presenter_refresh(ctx)) {
            ctx->quiet_clock_key = clock;
            ctx->quiet_env_key = env;
            ctx->quiet_caption_key = caption;
            ctx->quiet_flush_error_key = flush_errors;
        }
    }
    *last_word = request;
    return true;
}

/* Park or unpark the face under a data screen (presenter, once per pass,
 * BEFORE the face chain so quiet and dream see the settled state). The render
 * task only says what it wants (s_park_want); this is where the engine is
 * touched. Parking stops and hides the anim, the same calls dial_show makes.
 * Unparking runs the anim again if a face is bound, and the face it runs is
 * whatever apply_face last programmed, which kept following the requests all
 * the while parked. A baked dial does not take the anim back here: the face
 * chain right after this pass swaps it to the blit, and showing the anim for
 * that one pass would flash the face. */
static void presenter_backdrop_tick(jr_display_ctx_t *ctx)
{
    const uint32_t request = __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
    const bool want = __atomic_load_n(&s_park_want, __ATOMIC_ACQUIRE) != 0 &&
                      !(request & JR_DISPLAY_CMD_BLANK) &&
                      !__atomic_load_n(&ctx->blanked, __ATOMIC_RELAXED) &&
                      !s_dial_blit && !ctx->dream_active && !ctx->quiet_active;
    if (want == s_parked) {
        return;
    }
    if (gfx_emote_lock(ctx->gfx) != ESP_OK) {
        return;                         /* retried on the next pass */
    }
    if (want) {
        (void)gfx_anim_stop(ctx->anim);
        (void)gfx_obj_set_visible(ctx->anim, false);
        s_parked = true;
    } else {
        s_parked = false;
        if (ctx->active != NULL && !s_dial_blit &&
                !__atomic_load_n(&ctx->blanked, __ATOMIC_RELAXED) &&
                watch_dial_face() == JR_FACE_COUNT) {
            if (anim_run(ctx) != ESP_OK) {
                ctx->last_end = -1;     /* the next change re-programs it */
            }
        }
    }
    gfx_disp_refresh_all(ctx->disp);
    (void)gfx_emote_unlock(ctx->gfx);
}

/* Decode frame 0 of a resident dial clip into the shared frame. Runs on the
 * presenter; the decode itself holds the render lock (one stalled frame per
 * style change) because the frame may be on the glass. Any mismatch — size,
 * depth, a bad clip — answers an error and the caller keeps the anim. */
static esp_err_t dial_decode(jr_display_ctx_t *ctx, jr_face_t dial,
                             const jr_display_clip_t *clip)
{
    const size_t bytes =
        (size_t)ctx->board.width * ctx->board.height * sizeof(uint16_t);
    if (s_dial_frame == NULL) {
        s_dial_frame = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_dial_frame == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    eaf_format_handle_t eaf = NULL;
    esp_err_t err = eaf_init(clip->data, clip->size, &eaf);
    if (err != ESP_OK) {
        return err;
    }
    eaf_header_t head;
    if (eaf_get_frame_info(eaf, 0, &head) != EAF_FORMAT_VALID) {
        (void)eaf_deinit(eaf);
        return ESP_ERR_INVALID_CRC;
    }
    /* eaf_frame_decode trusts the header, not the buffer size */
    const bool fits = head.width == ctx->board.width &&
                      head.height == ctx->board.height && head.bit_depth == 8;
    eaf_free_header(&head);
    if (!fits) {
        (void)eaf_deinit(eaf);
        return ESP_ERR_INVALID_SIZE;
    }
    err = gfx_emote_lock(ctx->gfx);
    if (err == ESP_OK) {
        memset(s_dial_frame, 0, bytes);     /* a failed block stays black */
        err = eaf_frame_decode(eaf, 0, (uint8_t *)s_dial_frame, bytes,
                               ctx->board.swap_color_bytes);
        s_dial_frame_face = err == ESP_OK ? dial : JR_FACE_COUNT;
        if (err != ESP_OK) {
            s_dial_blit = false;
        }
        (void)gfx_emote_unlock(ctx->gfx);
    }
    (void)eaf_deinit(eaf);
    return err;
}

/* Release a baked dial after a successful decode or replacement binding.
 * gfx_anim_stop/hide retain the vendor EAF parser's pointers into that source;
 * a successful gfx_anim_set_src releases the old parser before we free it. */
static void clip_release_unbound_dial(jr_display_ctx_t *ctx,
                                      const jr_display_clip_t *clip)
{
    const jr_face_t dials[] = { JR_FACE_DIAL_DIVER, JR_FACE_DIAL_DRESS,
                               JR_FACE_DIAL_PILOT, JR_FACE_DIAL_FUTURE,
                               JR_FACE_DIAL_SKELETON, JR_FACE_DIAL_CELESTIAL };
    for (size_t i = 0; i < sizeof dials / sizeof dials[0]; ++i) {
        jr_display_clip_t *candidate = &ctx->clips[dials[i]];
        if (clip == candidate && clip != ctx->bound && candidate->data != NULL &&
            !candidate->embedded) {
            heap_caps_free(candidate->data);
            memset(candidate, 0, sizeof *candidate);
            return;
        }
    }
}

/* Put a baked dial on the glass without the engine: load, decode once,
 * hide the anim, blit. false leaves everything as it was, and the caller
 * shows the dial through the anim as before (including the missing-clip
 * fallback). The hidden vendor anim still owns its previously bound source. */
static bool dial_show(jr_display_ctx_t *ctx, jr_face_t dial)
{
    switch (dial) {
    case JR_FACE_DIAL_DIVER:
    case JR_FACE_DIAL_DRESS:
    case JR_FACE_DIAL_PILOT:
    case JR_FACE_DIAL_FUTURE:
    case JR_FACE_DIAL_SKELETON:
    case JR_FACE_DIAL_CELESTIAL: break;
    default:                  return false;
    }
    if ((ctx->missing_faces & (1U << dial)) != 0U) {
        return false;
    }
    jr_display_clip_t *clip = &ctx->clips[dial];
    if (s_dial_frame_face != dial) {
        if (clip->data == NULL && clip_load(dial, clip) != ESP_OK) {
            return false;
        }
        esp_err_t err = dial_decode(ctx, dial, clip);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "dial=%d decode failed (%s); anim path",
                     (int)dial, esp_err_to_name(err));
            return false;
        }
    }
    if (gfx_emote_lock(ctx->gfx) != ESP_OK) {
        return false;
    }
    (void)gfx_anim_stop(ctx->anim);
    (void)gfx_obj_set_visible(ctx->anim, false);
    s_dial_blit = true;
    gfx_disp_refresh_all(ctx->disp);
    clip_release_unbound_dial(ctx, clip);
    (void)gfx_emote_unlock(ctx->gfx);

    ctx->active = NULL;
    ctx->loaded_face = dial;
    ctx->shown_face = dial;
    ctx->last_end = -1;
    __atomic_store_n(&ctx->applied_bucket, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->blanked, false, __ATOMIC_RELAXED);
    diag_store(&ctx->current_asset_bytes,
               (uint32_t)((size_t)ctx->board.width * ctx->board.height * sizeof(uint16_t)));
    ESP_LOGI(TAG, "face=%d dial blit, anim hidden", (int)dial);
    return true;
}

static esp_err_t apply_face(jr_display_ctx_t *ctx, jr_face_t face,
                            jr_face_t requested_face, uint8_t amplitude,
                            bool *stale)
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
        return program_segment(ctx, ctx->shown_face, bucket, false);
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
        latest_before != requested_face) {
        if (stale != NULL) {
            *stale = true;
        }
        return ESP_OK;
    }
    /* A clip this art image lacks (new firmware over an old partition)
     * degrades to its parent face — once, remembered, no retry storm. */
    jr_face_t shown = face;
    while ((ctx->missing_faces & (1U << shown)) != 0U &&
           face_fallback(shown) != shown) {
        shown = face_fallback(shown);
    }
    jr_display_clip_t *clip = &ctx->clips[shown];
    while (clip->data == NULL) {
        esp_err_t load_err = clip_load(shown, clip);
        if (load_err == ESP_OK) {
            break;
        }
        diag_inc(&ctx->asset_load_failures);
        if (load_err == ESP_ERR_NOT_FOUND && face_fallback(shown) != shown) {
            ctx->missing_faces |= 1U << shown;
            ESP_LOGW(TAG, "face=%d has no clip on this art image; showing face=%d "
                     "until /api/ota/assets brings it", (int)shown,
                     (int)face_fallback(shown));
            shown = face_fallback(shown);
            clip = &ctx->clips[shown];
            continue;
        }
        return load_err;
    }
    /* Asset reads can take longer than a very short Gemini turn. Do not show
     * the completed turn's stale face after the request already returned to
     * Listening; leave the current face running and consume the latest word
     * on the next presenter pass. */
    const uint32_t requested =
        __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
    const jr_face_t latest = (jr_face_t)(
        (requested & JR_DISPLAY_CMD_FACE_MASK) >> JR_DISPLAY_CMD_FACE_SHIFT);
    if ((requested & JR_DISPLAY_CMD_BLANK) != 0U || latest != requested_face) {
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
    if (err == ESP_OK) {
        const jr_display_clip_t *previous = ctx->bound;
        ctx->bound = clip;
        ctx->active = clip;
        clip_release_unbound_dial(ctx, previous);
        s_dial_blit = false;            /* the anim is the face again */
    }
    (void)gfx_emote_unlock(ctx->gfx);
    if (err != ESP_OK) {
        /* A rejected replacement must not leave the last valid face stopped.
         * The engine still references the previously bound cache entry —
         * nothing was freed — so just restart it. */
        if (ctx->active != NULL) {
            (void)gfx_emote_lock(ctx->gfx);
            (void)anim_run(ctx);
            (void)gfx_emote_unlock(ctx->gfx);
        }
        return err;
    }

    ctx->active = clip;
    ctx->loaded_face = face;
    ctx->shown_face = shown;
    ctx->last_end = -1;
    __atomic_store_n(&ctx->applied_bucket, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->blanked, false, __ATOMIC_RELAXED);
    diag_store(&ctx->current_asset_bytes, (uint32_t)clip->size);

    err = program_segment(ctx, shown, bucket, true);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "face=%d asset=%u bytes frames=%u bucket=%u",
                 (int)shown, (unsigned)clip->size,
                 (unsigned)(clip->total_frames - 1U), (unsigned)bucket);
    }
    return err;
}

static esp_err_t display_engine_init(jr_display_ctx_t *ctx)
{
    ESP_RETURN_ON_ERROR(jarvis_board_display_get(&ctx->board), TAG,
                        "CO5300 board display init failed");

    /* Board bootstrap is dark. Apply the app's pre-start cap from this sole
     * command owner before gfx can submit its first neutral/content strip.
     * Unseeded boots retain the default100 target; subsequent changes slew. */
    const uint8_t initial = __atomic_load_n(&s_brightness_want, __ATOMIC_ACQUIRE);
    ESP_RETURN_ON_ERROR(jarvis_board_set_brightness(initial), TAG,
                        "initial panel brightness failed");
    atomic_store(&s_brightness_have, initial);
    atomic_store(&s_brightness_shown, initial);

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
        .fps = jr_display_render_fps(),
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

static esp_err_t display_engine_init_timed(jr_display_ctx_t *ctx)
{
    diag_store(&ctx->init_start_boot_ms,
               (uint32_t)(esp_timer_get_time() / 1000));
    __atomic_store_n(&ctx->init_started, 1U, __ATOMIC_RELEASE);
    esp_err_t err = display_engine_init(ctx);
    diag_store(&ctx->init_end_boot_ms,
               (uint32_t)(esp_timer_get_time() / 1000));
    __atomic_store_n(&ctx->init_finished, 1U, __ATOMIC_RELEASE);
    return err;
}

static void display_task(void *arg)
{
    jr_display_ctx_t *ctx = (jr_display_ctx_t *)arg;
    __atomic_store_n(&ctx->task, xTaskGetCurrentTaskHandle(), __ATOMIC_RELEASE);
    diag_store(&ctx->task_running, true);
    diag_store(&ctx->init_state, JR_DISPLAY_INIT_STARTING);

    esp_err_t err = display_engine_init_timed(ctx);
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
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(presenter_wait_ms(ctx)));
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);

        if (diag_load(&ctx->init_state) == JR_DISPLAY_INIT_READY) {
            presenter_pattern_tick(ctx, &last_test_pattern);
            uint32_t word = __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
            bool blank = (word & JR_DISPLAY_CMD_BLANK) != 0;
            jr_face_t face = (jr_face_t)((word & JR_DISPLAY_CMD_FACE_MASK) >>
                                         JR_DISPLAY_CMD_FACE_SHIFT);
            uint8_t amplitude = word & JR_DISPLAY_CMD_AMP_MASK;
            /* THE DIAL IS A FACE. On WATCH with a style that has baked art,
             * the glass shows that dial under the live hands. The swap is
             * made here, where the requested word is read, so a style step
             * or a walk onto WATCH re-applies exactly like a face change:
             * the effective word differs from the last one applied. The
             * composition root's own request still decides staleness. */
            const jr_face_t requested_face = face;
            {
                const jr_face_t dial = watch_dial_face();
                if (dial != JR_FACE_COUNT) {
                    face = dial;
                    word = (word & ~JR_DISPLAY_CMD_FACE_MASK) |
                           ((uint32_t)dial << JR_DISPLAY_CMD_FACE_SHIFT);
                }
            }

            presenter_backdrop_tick(ctx);

            if (presenter_dream_tick(ctx, &last_word)) {
                /* The stopped emote yields to the minute clock. */
            } else if (presenter_quiet_tick(ctx, &last_word)) {
                /* Visible quiet face; changed facts explicitly dirty it. */
            } else if (blank) {
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
                    err = dial_show(ctx, face)
                        ? ESP_OK
                        : apply_face(ctx, face, requested_face, amplitude, &stale);
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
            /* THE BLITTED DIAL HAS NO TIMER. The anim's tick was the only
             * thing invalidating the glass; with it hidden, this pass is the
             * cadence. Every pass, unconditionally, or the hands freeze
             * silently. The engine's fps (the ladder) still paces frames.
             * A parked backdrop is the same case: the anim is hidden too. */
            if ((s_dial_blit || s_parked) && gfx_emote_lock(ctx->gfx) == ESP_OK) {
                gfx_disp_refresh_all(ctx->disp);
                (void)gfx_emote_unlock(ctx->gfx);
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
    (void)act_rtc_restore();   /* the receipt from before the sleep */
    orbit_rec_build();         /* before the render task exists to read it */
    fring_rec_alloc();         /* likewise */
    shade_rec_alloc();         /* likewise */

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
    out_diag->render_us = diag_load(&s_display.render_us);
    out_diag->render_frames = diag_load(&s_display.render_frames);
    out_diag->render_frame_us = diag_load(&s_display.render_frame_us);
    for (int i = 0; i < 5; i++) {
        out_diag->layer_us[i] = diag_load(&s_display.layer_us[i]);
    }
    for (int i = 0; i < (int)JR_DISPLAY_SPLIT_COUNT; i++) {
        out_diag->split_us[i] = diag_load(&s_split_us[i]);
    }
    out_diag->light_gx_mg = s_light_gx_mg;
    out_diag->light_gy_mg = s_light_gy_mg;
    out_diag->light_shadow_dx_q4 = s_light.shadow_dx;
    out_diag->light_shadow_dy_q4 = s_light.shadow_dy;
    jr_display_boot_timing_t *timing = &out_diag->boot_timing;
    /* Read end first: observing its release also includes the start publish. */
    timing->init_finished =
        __atomic_load_n(&s_display.init_finished, __ATOMIC_ACQUIRE) != 0U;
    timing->init_started =
        __atomic_load_n(&s_display.init_started, __ATOMIC_ACQUIRE) != 0U;
    timing->first_transfer_valid =
        __atomic_load_n(&s_display.first_transfer_state, __ATOMIC_ACQUIRE) == 1U;
    if (timing->init_started) {
        timing->init_start_boot_ms = diag_load(&s_display.init_start_boot_ms);
    }
    if (timing->init_finished) {
        timing->init_end_boot_ms = diag_load(&s_display.init_end_boot_ms);
    }
    if (timing->first_transfer_valid) {
        timing->first_transfer_done_boot_ms =
            diag_load(&s_display.first_transfer_done_boot_ms);
    }
    timing->first_glance_time_transfer_valid = __atomic_load_n(
        &s_display.first_glance_time_transfer_state, __ATOMIC_ACQUIRE) == 1U;
    if (timing->first_glance_time_transfer_valid) {
        timing->first_glance_time_transfer_done_boot_ms =
            diag_load(&s_display.first_glance_time_transfer_done_boot_ms);
    }
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
    NAV_OP_STYLE,     /* arg = jr_watch_style_t; space and overlay untouched */
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
        uint32_t style = cur & NAV_STYLE_MASK;

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
        case NAV_OP_STYLE:
            style = (arg & 0x7u) << NAV_STYLE_SHIFT;
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
                              (novl << NAV_OVL_SHIFT) | fwd | style |
                              (serial << NAV_SERIAL_SHIFT);
        if (next == cur) {
            return;      /* idempotent: no restarted animation, no wake */
        }
        if (__atomic_compare_exchange_n(&s_nav_word, &cur, next, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    jr_display_dream_set(false);
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

void jr_display_watch_style_set(jr_watch_style_t style)
{
    if ((int)style < 0 || style >= JR_WATCH_STYLE_COUNT) {
        return;
    }
    nav_step(NAV_OP_STYLE, (uint32_t)style);
}

void jr_display_watch_style_step(int dir)
{
    const int n = (int)JR_WATCH_STYLE_COUNT;
    const int cur = (int)jr_display_watch_style();
    const int next = (cur + n + (dir < 0 ? -1 : 1)) % n;
    nav_step(NAV_OP_STYLE, (uint32_t)next);
}

jr_watch_style_t jr_display_watch_style(void)
{
    const uint32_t nav = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    const uint32_t st = (nav & NAV_STYLE_MASK) >> NAV_STYLE_SHIFT;
    return st < (uint32_t)JR_WATCH_STYLE_COUNT ? (jr_watch_style_t)st
                                               : JR_WATCH_JARVIS;
}

const char *jr_display_watch_style_name(jr_watch_style_t style)
{
    static const char *const names[JR_WATCH_STYLE_COUNT] = {
        "JARVIS", "DIVER", "DRESS", "PILOT", "MINIMAL", "FUTURE",
        "SKELETON", "CELESTIAL" };
    return ((int)style >= 0 && style < JR_WATCH_STYLE_COUNT) ? names[style]
                                                             : names[0];
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
        /* The mute button: its drawn disc, nothing larger. */
        const int bdx = x - SP_BTN_PRIV_CX;
        const int bdy = y - SP_BTN_CY;
        if (bdx * bdx + bdy * bdy <= SP_BTN_R * SP_BTN_R) {
            return JR_DISPLAY_ACT_PRIVACY_TOGGLE;
        }
        /* The volume arc is the left rim: above the middle is louder, below it
         * is quieter, as the arc fills upward. */
        if (dx < 0 && dy >= -SP_VOL_HIT_DY && dy <= SP_VOL_HIT_DY &&
            r2 >= SP_VOL_HIT_IN * SP_VOL_HIT_IN) {
            return dy < 0 ? JR_DISPLAY_ACT_VOLUME_UP : JR_DISPLAY_ACT_VOLUME_DOWN;
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

/* Wall-clock seconds, or 0 when the clock has never been set (SNTP, or the
 * board RTC main seeds from it). An unset clock stamps nothing. */
static uint32_t act_wall_now(void)
{
    const time_t t = time(NULL);
    return t >= (time_t)ACT_WALL_VALID ? (uint32_t)t : 0u;
}

/* FNV-1a over everything but the checksum itself. */
static uint32_t act_rtc_sum(const act_rtc_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(act_rtc_t, check); ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* A field is believed only if it ends inside its cap and every glyph before
 * the end is printable ASCII — the only thing a push can have written. */
static bool act_text_ok(const char *s, size_t cap)
{
    for (size_t i = 0; i < cap; ++i) {
        if (s[i] == '\0') {
            return true;
        }
        if ((unsigned char)s[i] < 0x20u || (unsigned char)s[i] > 0x7Eu) {
            return false;
        }
    }
    return false;
}

static void act_rtc_save(uint32_t n)
{
    act_rtc_t r;
    memset(&r, 0, sizeof r);
    r.magic = ACT_RTC_MAGIC;
    r.count = n;
    memcpy(r.kind, s_act_kind, sizeof r.kind);
    memcpy(r.sum, s_act_sum, sizeof r.sum);
    memcpy(r.wall, s_act_wall, sizeof r.wall);
    r.check = act_rtc_sum(&r);
    s_act_rtc = r;
}

/* Once per boot, before the first push: the rows from before the sleep come
 * back with their wall stamps and no esp_timer stamp (that clock restarted).
 * Anything that fails a check is dropped whole — an empty feed that says so
 * beats one row of cold-boot noise. Returns the rows restored. */
static uint32_t act_rtc_restore(void)
{
    if (s_act_restored) {
        return 0u;
    }
    s_act_restored = true;
    const act_rtc_t r = s_act_rtc;
    if (r.magic != ACT_RTC_MAGIC || r.count == 0u || r.count > (uint32_t)ACT_MAX ||
        r.check != act_rtc_sum(&r)) {
        return 0u;
    }
    for (uint32_t i = 0; i < r.count; ++i) {
        if (!act_text_ok(r.kind[i], ACT_KIND_CAP) || !act_text_ok(r.sum[i], ACT_SUM_CAP)) {
            return 0u;
        }
    }
    memcpy(s_act_kind, r.kind, sizeof s_act_kind);
    memcpy(s_act_sum, r.sum, sizeof s_act_sum);
    memcpy(s_act_wall, r.wall, sizeof s_act_wall);
    memset(s_act_ms, 0, sizeof s_act_ms);
    s_act_live = 0u;
    __atomic_store_n(&s_act_count, r.count, __ATOMIC_RELEASE);
    return r.count;
}

void jr_display_activity_push(const char *kind, const char *summary)
{
    if ((kind == NULL || kind[0] == '\0') &&
        (summary == NULL || summary[0] == '\0')) {
        return;                                 /* nothing happened: no row */
    }
    (void)act_rtc_restore();
    uint32_t n = __atomic_load_n(&s_act_count, __ATOMIC_ACQUIRE);
    if (n > (uint32_t)ACT_MAX) {
        n = (uint32_t)ACT_MAX;
    }
    /* Newest first: shift the older rows down and drop the fourth. */
    for (int i = ACT_MAX - 1; i > 0; --i) {
        memcpy(s_act_kind[i], s_act_kind[i - 1], sizeof s_act_kind[i]);
        memcpy(s_act_sum[i], s_act_sum[i - 1], sizeof s_act_sum[i]);
        s_act_ms[i] = s_act_ms[i - 1];
        s_act_wall[i] = s_act_wall[i - 1];
    }
    s_act_live = (uint8_t)(((uint32_t)s_act_live << 1) & ((1u << ACT_MAX) - 1u));
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
    s_act_wall[0] = act_wall_now();
    s_act_live |= 1u;
    /* The count lands last, so a frame that reads the new count reads the
     * rows it was written for. */
    n = n < (uint32_t)ACT_MAX ? n + 1u : n;
    __atomic_store_n(&s_act_count, n, __ATOMIC_RELEASE);
    act_rtc_save(n);
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
    uint32_t cur = __atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((cur & 3U) == 1U || (cur & 3U) == 2U) { return; }
        const uint32_t next = ((cur & ~3U) + 4U) | 1U;
        if (__atomic_compare_exchange_n(&s_panel_off, &cur, next, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { nav_wake(); return; }
    }
}

bool jr_display_panel_is_off(void)
{
    return (__atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE) & 3U) == 2U;
}

void jr_display_panel_on_request(void)
{
    uint32_t cur = __atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((cur & 3U) == 0U || (cur & 3U) == 3U) { return; }
        const uint32_t next = ((cur & ~3U) + 4U) | 3U;
        if (__atomic_compare_exchange_n(&s_panel_off, &cur, next, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { nav_wake(); return; }
    }
}

bool jr_display_panel_is_on(void)
{
    const uint32_t word = __atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE);
    return (word & 3U) == 0U &&
        __atomic_load_n(&s_panel_on_ack_valid, __ATOMIC_ACQUIRE) != 0U &&
        __atomic_load_n(&s_panel_on_ack_word, __ATOMIC_RELAXED) == word;
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

void jr_display_set_status(uint8_t volume, uint8_t brightness_cap)
{
    if (volume > 100U) {
        volume = 100U;
    }
    if (brightness_cap > 100U) {
        brightness_cap = 100U;
    }
    __atomic_store_n(&s_status_word,
                     (uint32_t)volume | ((uint32_t)brightness_cap << 8), __ATOMIC_RELEASE);
}
