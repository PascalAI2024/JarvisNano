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
#include "jr_display/tilt.h"
#include "jr_display/glass.h"
#include "jr_display/perf_knob.h"
#include "jr_display/glass_icon.h"
#include "showpiece_gfx.h"
#include "glass3_ring.h"
#include "glass3_screens.h"

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
/* Right-sized from measured high-water: peak use 1,784 B (2,748 B free at
 * 5,120 B on live device), so 4,096 B reclaims 1,024 B of internal SRAM while
 * keeping >1.7 KB measured headroom. Re-measure via /api/diag/tasks after any
 * render change before trusting this number. */
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
    case JR_FACE_DIAL_ORRERY:    return JR_DISPLAY_MOUNT_POINT "/dial_orrery.eaf";
    case JR_FACE_DIAL_REACTOR:   return JR_DISPLAY_MOUNT_POINT "/dial_reactor.eaf";
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
    case JR_FACE_DIAL_CELESTIAL:
    case JR_FACE_DIAL_ORRERY:
    case JR_FACE_DIAL_REACTOR: return JR_FACE_IDLE;
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
    case JR_FACE_DIAL_CELESTIAL:
    case JR_FACE_DIAL_ORRERY:
    case JR_FACE_DIAL_REACTOR: return 24;
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
extern const uint8_t s_embedded_orrery_start[]    asm("_binary_dial_orrery_eaf_start");
extern const uint8_t s_embedded_orrery_end[]      asm("_binary_dial_orrery_eaf_end");
extern const uint8_t s_embedded_reactor_start[]   asm("_binary_dial_reactor_eaf_start");
extern const uint8_t s_embedded_reactor_end[]     asm("_binary_dial_reactor_eaf_end");
static const uint8_t *embedded_dial(jr_face_t face, size_t *size)
{
    switch (face) {
    case JR_FACE_DIAL_SKELETON:
        *size = (size_t)(s_embedded_skeleton_end - s_embedded_skeleton_start);
        return s_embedded_skeleton_start;
    case JR_FACE_DIAL_CELESTIAL:
        *size = (size_t)(s_embedded_celestial_end - s_embedded_celestial_start);
        return s_embedded_celestial_start;
    case JR_FACE_DIAL_ORRERY:
        *size = (size_t)(s_embedded_orrery_end - s_embedded_orrery_start);
        return s_embedded_orrery_start;
    case JR_FACE_DIAL_REACTOR:
        *size = (size_t)(s_embedded_reactor_end - s_embedded_reactor_start);
        return s_embedded_reactor_start;
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

/* THE ONE TRANSITION GRAMMAR. Every change of what the glass shows (a slide to
 * another screen, a new watch style, a caption swapping its words) runs the
 * same two phases on one linear progress p, 0..256: the outgoing content
 * eases out over [0, cut) and is GONE at cut; the incoming eases in over
 * [cut, 256]. At no p are both above zero, so two screens' focal content never
 * sit on each other, and a dip through dark is the whole of the cross-fade.
 * `cut` is 112 for a slide (4 of ~10 frames out, 6 in) and 96 for a style
 * change (3 of 8). tr_out falls on a smoothstep (it accelerates away), tr_in
 * rises ease-out cubic (it arrives fast and settles). Both return 0..256. */
#define TR_CUT_SLIDE 112
#define TR_CUT_STYLE 96

static inline int tr_out(int p, int cut)
{
    if (p >= cut) {
        return 0;
    }
    return 256 - fade_smoothstep((p * 256) / cut);
}

static inline int tr_in(int p, int cut)
{
    if (p <= cut) {
        return 0;
    }
    if (p >= 256) {
        return 256;
    }
    const int r = 256 - ((p - cut) * 256) / (256 - cut);    /* 256..0 */
    return 256 - ((r * r * r) >> 16);
}

/* Brightness of the layer under the shell (the face, or a baked dial) through a
 * transition: the outgoing layer dims out, the incoming one comes up after the
 * cut, and a layer that is not part of the picture (a data screen, or a watch
 * with no art, which draws on black) stays at zero. */
static inline int tr_layer(int p, int cut, bool vis_from, bool vis_to)
{
    return p < cut ? (vis_from ? tr_out(p, cut) : 0)
                   : (vis_to ? tr_in(p, cut) : 0);
}

/* ===== THE PANEL DIP (perf knob bit 0, JR_KNOB_PANEL_DIP) ===================
 *
 * A change of ring screen, entering or leaving WATCH and a watch style change
 * pass "through the dark". On knob 0 the framebuffer does it, per pixel, and a
 * transition frame costs two to three times a settled one. With the bit the
 * compositor draws the old screen and then the new one at full strength, and the
 * panel's brightness register does the dimming: s_dip (0..256, 256 = no dip)
 * scales the brightness brightness_pump sends, `sent = shown * light(dip)`.
 *
 * The dip is clocked by the display's millisecond clock, not by frames, so a slow
 * frame carries several brightness steps and the ramp stays smooth:
 *   OUT   falls over tr_out's time (the transition's total times cut/256),
 *   HOLD  dark, until the new content has been composed AND flushed once, with
 *         DIP_HOLD_CAP_MS as the longest it may wait,
 *   IN    rises over tr_in's time.
 * Phases advance in the pump (brightness_pump, any strip). The compositor never
 * reads the phase mid-frame: sp_fade_tick latches "the new content is on" once a
 * frame (s_dip_swapped), so a frame composes at one state. A new transition during
 * a dip restarts OUT from the level the dip had, and the glass keeps showing the
 * screen it was showing, so no half-swapped content is ever lit.
 * Render task only (the three diag reads are plain words). */
static uint32_t s_frame_no;               /* defined with overlay_fade_tick below */
#define DIP_GAMMA        2        /* the light of a pixel at half value is a quarter: dip^2 */
#define DIP_HOLD_CAP_MS  300      /* the longest the dark waits for the new screen (SP_STYLE_HOLD_MS's idea) */
#define DIP_WRITE_MS     16       /* the panel refreshes at 60 Hz: one brightness write per refresh at most */

typedef enum { DIP_IDLE, DIP_OUT, DIP_HOLD, DIP_IN } dip_phase_t;
typedef enum { DIP_MODE_NONE, DIP_MODE_SPACE, DIP_MODE_STYLE } dip_mode_t;

static bool        s_dip_on;               /* the knob bit, latched once a frame by knob_apply */
static dip_phase_t s_dip_phase = DIP_IDLE;
static dip_mode_t  s_dip_mode;             /* which transition the compositor draws the dip way */
static volatile int s_dip = 256;
static int         s_dip_cut, s_dip_l0;
static uint32_t    s_dip_t0, s_dip_out_ms, s_dip_in_ms;
static uint32_t    s_dip_entry_next = UINT32_MAX; /* the entry clock the new screen starts with, at the swap */
static bool        s_dip_swapped;          /* the compositor draws the NEW content (latched per frame) */
static bool        s_dip_ready;            /* ... and the layer under it is bound: this frame is "the new screen" */
static bool        s_dip_flushed;          /* the first ready frame has been flushed whole */
static bool        s_dip_tail;             /* the dip is over but the panel still has to be told */
static uint32_t    s_dip_ready_frame;
static volatile uint32_t s_dip_hold_ms;    /* the last dark hold */
static volatile uint32_t s_dip_writes;     /* brightness writes the dip caused */
static uint32_t    s_dip_write_ms;
static bool        s_dip_wrote;

/* The light the panel gives at dip/256 of the brightness: dip^DIP_GAMMA, 0..256. */
static int dip_light(int dip)
{
    int f = 256;
    for (int i = 0; i < DIP_GAMMA; ++i) {
        f = (f * dip) >> 8;
    }
    return f;
}

static inline uint32_t dip_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Move the phases to `now`. Called from the pump on every strip while a dip runs. */
static void dip_advance(uint32_t now)
{
    if (s_dip_phase == DIP_OUT) {
        const uint32_t el = now - s_dip_t0;
        if (el >= s_dip_out_ms) {
            s_dip = 0;
            s_dip_phase = DIP_HOLD;
            s_dip_t0 = s_dip_t0 + s_dip_out_ms;     /* the hold counts from the dark, not from this call */
        } else {
            s_dip = (s_dip_l0 * tr_out((int)((el * (uint32_t)s_dip_cut) / s_dip_out_ms), s_dip_cut)) / 256;
        }
    }
    if (s_dip_phase == DIP_HOLD) {
        s_dip = 0;
        const uint32_t held = now - s_dip_t0;
        const bool done = s_dip_ready && s_dip_flushed;
        if (done || held >= DIP_HOLD_CAP_MS) {      /* the cap: never stay dark, whatever is on the glass */
            s_dip_hold_ms = held;
            s_dip_phase = DIP_IN;
            s_dip_t0 = now;
        }
    }
    if (s_dip_phase == DIP_IN) {
        const uint32_t el = now - s_dip_t0;
        if (el >= s_dip_in_ms) {
            s_dip = 256;
            s_dip_phase = DIP_IDLE;
            s_dip_tail = true;
        } else {
            s_dip = tr_in(s_dip_cut + (int)((el * (uint32_t)(256 - s_dip_cut)) / s_dip_in_ms), s_dip_cut);
        }
    }
}

/* A transition starts (render task, sp_fade_tick). `total_ms` is the whole of the
 * grammar's time (JR_DISPLAY_SPACE_MS, SP_STYLE_MS). From a dip already under way
 * it dims on from the level it has, over a proportional share of tr_out's time. */
static void dip_begin(dip_mode_t mode, int cut, uint32_t total_ms, uint32_t now)
{
    dip_advance(now);
    const int l0 = s_dip_phase == DIP_IDLE ? 256 : s_dip;
    const uint32_t out_full = (total_ms * (uint32_t)cut) / 256U;
    s_dip_mode = mode;
    s_dip_cut = cut;
    s_dip_l0 = l0;
    s_dip_out_ms = (out_full * (uint32_t)l0) / 256U;
    s_dip_in_ms = total_ms - out_full;
    s_dip_phase = DIP_OUT;
    s_dip_t0 = now;
    s_dip_swapped = false;
    s_dip_ready = false;
    s_dip_flushed = false;
    if (s_dip_out_ms == 0U) {
        dip_advance(now);                           /* already dark: straight to the hold */
    }
}

/* Drop the dip: the knob went off, or the panel is going to sleep. The pump
 * restores the brightness the dip had taken (s_dip_tail). */
static void dip_abort(void)
{
    if (s_dip_phase != DIP_IDLE || s_dip < 256) {
        s_dip_tail = true;
    }
    s_dip_phase = DIP_IDLE;
    s_dip_mode = DIP_MODE_NONE;
    s_dip_swapped = false;
    s_dip = 256;
}

/* The whole of a frame has been flushed (panel_flush, last strip; the preview and
 * the tests call it at the end of a frame): the first frame of the new content
 * is on the glass, the dark may end. */
static void dip_frame_done(void)
{
    if (s_dip_phase != DIP_IDLE && s_dip_ready && !s_dip_flushed && s_frame_no >= s_dip_ready_frame) {
        s_dip_flushed = true;
    }
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
 * decoded once, in panel byte order, into a PSRAM frame (dial_show); on
 * WATCH the anim is hidden and each strip copies the frame's rows here, in the
 * anim's place under every overlay.
 *
 * THE DIAL CACHE (N17). Each baked dial keeps its own decoded frame in
 * s_dial_slot[], allocated on first use, so a style change to a dial shown
 * before costs no flash read and no decode, and a first decode goes into the
 * dial's own frame while the render task keeps drawing the old one. What the
 * strips read is s_dial_frame, the frame of the dial on the glass. s_dial_blit,
 * s_dial_frame and s_dial_frame_face change only under the render lock, which
 * the render task holds for the whole frame, so a strip never sees a switch
 * halfway. A frame is marked valid only after the whole decode succeeded. */
static uint16_t *s_dial_frame;                       /* width*height, PSRAM */
static jr_face_t s_dial_frame_face = JR_FACE_COUNT;  /* the art it holds */
static bool      s_dial_blit;

#define DIAL_SLOTS          8
#define DIAL_PSRAM_FLOOR    (2800U * 1024U)        /* free PSRAM a new frame must leave */
#define DIAL_PSRAM_LOW      (2400U * 1024U)        /* below this the idle frames go */
#define DIAL_WARM_SETTLE_MS 1500U                  /* on the glass this long before a warm-up */

typedef struct {
    uint16_t *px;       /* width*height, PSRAM; NULL until the dial is first wanted */
    bool      valid;    /* px holds one whole decode (set last, cleared first) */
    uint32_t  used;     /* s_dial_clock at the last show; the smallest is evicted first */
} dial_slot_t;

/* Presenter task only, except the counters, which the HTTP task reads. */
static dial_slot_t s_dial_slot[DIAL_SLOTS];
static uint16_t   *s_dial_shared;                    /* today's one frame, the fallback */
static jr_face_t   s_dial_shared_face = JR_FACE_COUNT;
static uint32_t    s_dial_clock;
static int         s_dial_filling = -1;              /* slot being decoded into, or -1 */
static uint64_t    s_dial_glass_ms;                  /* when the dial on the glass arrived */
static uint8_t     s_dial_warm_skip;                 /* slot bits whose warm-up failed */
static size_t      s_dial_frame_bytes;
static uint32_t    s_dial_held_n, s_dial_held_bytes; /* decoded frames, bytes held */
static uint32_t    s_dial_hits, s_dial_misses, s_dial_evictions, s_dial_warms;
static uint32_t    s_dial_decode_ms;                 /* the last decode, whoever asked */
#ifdef ESP_PLATFORM
/* MINIMAL static dial baked once into PSRAM on demand (reuses dial_cache
 * ownership/eviction rules; cuts MINIMAL L2 from ~40 ms to ~4 ms). */
static dial_slot_t      s_minimal_slot;
static bool             s_minimal_swap;
static bool             s_dial_minimal;
static volatile uint32_t s_minimal_want;
#endif
static void dial_cache_release(void);

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

/* The dial copy takes the span of each row. The panel is round: the four corners of the 466 x 466
 * frame are never seen. Per row, the columns inside a circle of radius 234
 * (one pixel wider than the 233.0 the glass shows); s_dial_span[d] is the
 * largest k with the row's pixel 233 + k inside, d the row's distance from the
 * centre line (0 = rows 232 and 233). 233 bytes in flash .rodata (zero BSS). */
#define DIAL_SPAN_N 233
static const uint8_t s_dial_span[DIAL_SPAN_N] = {
    232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232,
    232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 231, 231, 231, 231, 231, 231,
    231, 231, 230, 230, 230, 230, 230, 230, 229, 229, 229, 229, 229, 229, 228, 228,
    228, 228, 227, 227, 227, 227, 227, 226, 226, 226, 226, 225, 225, 225, 224, 224,
    224, 224, 223, 223, 223, 222, 222, 222, 221, 221, 221, 220, 220, 220, 219, 219,
    219, 218, 218, 218, 217, 217, 216, 216, 216, 215, 215, 214, 214, 214, 213, 213,
    212, 212, 211, 211, 210, 210, 209, 209, 208, 208, 207, 207, 206, 206, 205, 205,
    204, 204, 203, 203, 202, 201, 201, 200, 200, 199, 198, 198, 197, 196, 196, 195,
    195, 194, 193, 193, 192, 191, 190, 190, 189, 188, 188, 187, 186, 185, 185, 184,
    183, 182, 181, 181, 180, 179, 178, 177, 176, 176, 175, 174, 173, 172, 171, 170,
    169, 168, 167, 166, 165, 164, 163, 162, 161, 160, 159, 158, 157, 156, 155, 154,
    153, 151, 150, 149, 148, 147, 145, 144, 143, 142, 140, 139, 138, 136, 135, 133,
    132, 131, 129, 128, 126, 124, 123, 121, 120, 118, 116, 115, 113, 111, 109, 107,
    105, 103, 101,  99,  97,  95,  93,  90,  88,  85,  83,  80,  77,  74,  71,  68,
     65,  61,  58,  54,  49,  45,  39,  33,  25,
};

static void apply_dial(const jr_display_ctx_t *ctx, int x1, int y1,
                       int x2, int y2, uint16_t *pixels)
{
    if (!s_dial_blit || s_dial_frame == NULL || pixels == NULL) {
        return;
    }
    const int width = x2 - x1;
    if (ctx->board.width == 2 * DIAL_SPAN_N && y2 <= 2 * DIAL_SPAN_N) {
        for (int row = y1; row < y2; ++row) {
            const int d = row < DIAL_SPAN_N ? DIAL_SPAN_N - 1 - row : row - DIAL_SPAN_N;
            int a = DIAL_SPAN_N - 1 - s_dial_span[d];          /* first column of the glass */
            int b = DIAL_SPAN_N + s_dial_span[d] + 1;          /* one past the last */
            if (a < x1) a = x1;
            if (b > x2) b = x2;
            if (a < b) {
                memcpy(pixels + (size_t)(row - y1) * (size_t)width + (size_t)(a - x1),
                       s_dial_frame + (size_t)row * ctx->board.width + (size_t)a,
                       (size_t)(b - a) * sizeof(uint16_t));
            }
        }
        return;
    }
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
static glass_font_id_t  s_choice_q_font = GLASS_FONT_TEXT;
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
static uint8_t          s_caption_label;           /* 0: speech; else jr_display_label_kind_t + 1 */
#define JR_CAPTION_STYLE_Y 338                      /* a style label: clear of every dial's six o'clock marks */

/* WHAT IS ON THE GLASS (render task only, 56 bytes). The app task rewrites the
 * staged lines above whenever it likes; the glass draws these copies, which
 * caption_tick refreshes. A caption that speaks refreshes them at once (a live
 * transcript must not lag its voice). A label replacing a label waits for the
 * cut of the slide, so the old name can ease out first. */
static char             s_capd_line[2][20];
static int16_t          s_capd_y[2];
static bool             s_capd_grey;
static bool             s_capd_lease;
static bool             s_capd_label;
static uint32_t         s_cap_rev_seen;
static int              s_cap_p = 256;             /* the label's own phase, 0..256 */
static bool             s_cap_dip;                 /* old name on glass: out, swap, in */
static bool             s_cap_was_on;              /* the caption flag as of the last tick: a name that has
                                                    * expired and is fading is not "on the glass" for a new one */
static bool             s_cap_pend;                /* the swap to the staged words is due at the cut */
static uint32_t         s_cap_age_ms;              /* settled time a label has been up */
static uint32_t         s_cap_hold_ms;             /* 0: this caption never times out */
static int              s_cap_alpha;               /* s_caption_ease through the phase, 0..256 */

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
static void tilt_tick(uint32_t now_ms);          /* defined with the watch below */

/* The split timers (below) cost a timer read per part per strip, so they run
 * only while GET /api/display?split=1 has asked for them (jr_display_split_arm,
 * a few seconds a request). s_split_on is the render task's own copy, decided
 * once a frame in overlay_fade_tick; split_mark and split_now read it. */
static volatile uint32_t s_split_deadline_ms;
static bool s_split_on;

/* The dev-only perf knob (perf_knob.h). httpd writes the word; the render task
 * copies it once a frame into hud_perf_knob and glass_perf_knob (knob_apply,
 * from overlay_fade_tick), so a frame never sees it change half way. */
static volatile uint32_t s_perf_knob_word;
static void knob_apply(void);

void jr_display_set_perf_knob(uint32_t knob)
{
    __atomic_store_n(&s_perf_knob_word, knob & JR_KNOB_KNOWN_MASK, __ATOMIC_RELAXED);
}

uint32_t jr_display_perf_knob(void)
{
    return __atomic_load_n(&s_perf_knob_word, __ATOMIC_RELAXED);
}

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

static void caption_tick(uint32_t dt_ms);      /* defined with the shell state below */

static void overlay_fade_tick(void)
{
    s_frame_no++;
    knob_apply();
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
        tilt_tick(now);              /* CELESTIAL's lean, read once a frame */
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
    caption_tick(dt);
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
    const glass_metrics_t ml = glass_font_metrics(GLASS_FONT_LABEL);
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
        const int lw = glass_text_width(GLASS_FONT_LABEL, s_choice_label_buf[i],
                                        (size_t)len, 1);
        hud_choice_label_anchor(&tmp[i], 198, lw > 0 ? lw : 1,
                                ml.cap_h > 0 ? ml.cap_h : 11,
                                &s_choice_label_x[i], &s_choice_label_y[i]);
    }
    memcpy(s_choices, tmp, sizeof tmp);

    /* Question furniture: wrapped ONCE into owned lines and measured in Glass-2
     * Barlow. Use GLASS_FONT_VALUE (cap 24) when both lines fit comfortably inside
     * the safe diameter, else GLASS_FONT_TEXT (cap 15). */
    s_choice_q_line[0][0] = '\0';
    s_choice_q_line[1][0] = '\0';
    if (question != NULL) {
        hud_wrap2(question, 24, s_choice_q_line[0], s_choice_q_line[1]);
    }
    const int q1 = (int)strnlen(s_choice_q_line[0], 24U);
    const int q2 = (int)strnlen(s_choice_q_line[1], 24U);
    const int w1_val = glass_text_width(GLASS_FONT_VALUE, s_choice_q_line[0], (size_t)q1, 0);
    const int w2_val = glass_text_width(GLASS_FONT_VALUE, s_choice_q_line[1], (size_t)q2, 0);
    const bool use_val = (w1_val <= 284 && w2_val <= 284);
    const glass_font_id_t qf = use_val ? GLASS_FONT_VALUE : GLASS_FONT_TEXT;
    const int qtrk = use_val ? 0 : 1;
    const int w1 = use_val ? w1_val : glass_text_width(qf, s_choice_q_line[0], (size_t)q1, qtrk);
    const int w2 = use_val ? w2_val : glass_text_width(qf, s_choice_q_line[1], (size_t)q2, qtrk);
    const glass_metrics_t qm = glass_font_metrics(qf);
    s_choice_q_font = qf;
    s_choice_q_x[0] = (HUD_W - w1) / 2;
    s_choice_q_x[1] = (HUD_W - w2) / 2;
    if (q2 > 0) {
        const int gap = use_val ? 8 : 10;
        const int total_h = qm.cap_h * 2 + gap;
        s_choice_q_y[0] = 192 - total_h / 2;
        s_choice_q_y[1] = s_choice_q_y[0] + qm.cap_h + gap;
    } else {
        s_choice_q_y[0] = 192 - qm.cap_h / 2;
        s_choice_q_y[1] = 192;
    }

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

static void caption_store(const char *text, uint8_t label)
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
        s_caption_label == label &&
        strlen(s_caption_text) == len && memcmp(s_caption_text, text, len) == 0) {
        return;
    }
    memcpy(s_caption_text, text, len);
    s_caption_text[len] = '\0';
    hud_wrap2(s_caption_text, 19, s_caption_line[0], s_caption_line[1]);
    const int c2 = (int)strnlen(s_caption_line[1], 19U);
    s_caption_y[0] = (c2 > 0) ? JR_CAPTION_TWO_Y0
                   : (label == (uint8_t)JR_DISPLAY_LABEL_WATCH_STYLE + 1U) ? JR_CAPTION_STYLE_Y
                   : JR_CAPTION_ONE_Y;
    s_caption_y[1] = JR_CAPTION_TWO_Y1;
    s_caption_grey = caption_is_quiet(s_caption_text);
    s_caption_lease = caption_is_lease(s_caption_text);
    s_caption_label = label;
    __atomic_store_n(&s_caption_on, JR_DISPLAY_CAPTION_ACTIVE |
                     (caption_is_ambient(s_caption_text)
                         ? JR_DISPLAY_CAPTION_AMBIENT : 0), __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_caption_revision, 1U, __ATOMIC_RELEASE);
    nav_wake();
}

void jr_display_caption_set(const char *text)
{
    caption_store(text, 0U);
}

void jr_display_caption_label(const char *text, jr_display_label_kind_t kind)
{
    caption_store(text, (uint8_t)kind + 1U);
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
static _Atomic bool s_hbm_want;           /* CO5300 HBM 0x66 target (any task) */
static _Atomic bool s_hbm_have;           /* CO5300 HBM 0x66 applied (render task) */

/* Wave 2: 60 Hz LCD_TE (GPIO13) tear-free flush stats & backoff (render task writes). */
#define JR_DISPLAY_TE_TIMEOUT_MS      18U
#define JR_DISPLAY_TE_MIN_FPS         12U
#define JR_DISPLAY_TE_BACKOFF_STREAK   3U
#define JR_DISPLAY_TE_BACKOFF_MS    1000U
static _Atomic uint32_t s_te_waits;
static _Atomic uint32_t s_te_timeouts;
static uint32_t         s_te_consec_timeouts;
static uint32_t         s_te_backoff_until_ms;

void jr_display_set_hbm(bool enabled)
{
    if (atomic_exchange_explicit(&s_hbm_want, enabled, memory_order_acq_rel) != enabled) {
        nav_wake();
    }
}

bool jr_display_get_hbm(void)
{
    return atomic_load_explicit(&s_hbm_want, memory_order_acquire);
}

void jr_display_get_te_stats(bool *enabled, uint32_t *waits, uint32_t *timeouts)
{
    if (enabled != NULL) {
        *enabled = jarvis_board_te_enabled();
    }
    if (waits != NULL) {
        *waits = atomic_load_explicit(&s_te_waits, memory_order_relaxed);
    }
    if (timeouts != NULL) {
        *timeouts = atomic_load_explicit(&s_te_timeouts, memory_order_relaxed);
    }
}

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
    /* The panel dip (knob bit 0) multiplies what the mood slew asks for; its
     * writes are held to one per DIP_WRITE_MS. With no dip running (and none to
     * undo) none of this is read: the clock is not even touched. */
    const bool dipping = s_dip_phase != DIP_IDLE || s_dip_tail;
    uint32_t now = 0U;
    if (dipping) {
        now = dip_now_ms();
        dip_advance(now);
        want = (uint8_t)((want * dip_light(s_dip)) >> 8);
    }
    /* Apply CO5300 High-Brightness Mode (0x63=0xFF, 0x66 bit 1 HBM_EN) on the
     * sole panel-command owner. Turning HBM off forces normal 0x51 brightness
     * restore right afterward so the transition back is smooth and exact. */
    const bool hbm_want = atomic_load_explicit(&s_hbm_want, memory_order_acquire);
    const bool hbm_have = atomic_load_explicit(&s_hbm_have, memory_order_relaxed);
    bool force_brightness = false;
    if (hbm_want != hbm_have) {
        if (jarvis_board_set_hbm(hbm_want) == ESP_OK) {
            atomic_store_explicit(&s_hbm_have, hbm_want, memory_order_release);
            if (!hbm_want) {
                force_brightness = true;
            }
        }
    }
    if (!force_brightness && want == s_brightness_have) {
        if (dipping && s_dip_phase == DIP_IDLE) {
            s_dip_tail = false;   /* told, and back at the shown value */
        }
        return;   /* also kills the 8.9 writes/sec of identical values */
    }
    if (!force_brightness && dipping && s_dip_wrote &&
        (uint32_t)(now - s_dip_write_ms) < DIP_WRITE_MS) {
        return;   /* the panel refreshed this a moment ago: the next strip tries again */
    }
    if (jarvis_board_set_brightness(want) == ESP_OK) {
        s_brightness_have = want;
        if (dipping) {
            s_dip_writes++;
            s_dip_write_ms = now;
            s_dip_wrote = true;
            if (s_dip_phase == DIP_IDLE) {
                s_dip_tail = false;
            }
        }
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
/* WATCH STYLE: four bits (17..20) above the 8-bit serial, preserved by every
 * nav step, changed only by NAV_OP_STYLE. Pinned by the shell suite. */
#define NAV_STYLE_SHIFT   17
#define NAV_STYLE_MASK    (0xFu << NAV_STYLE_SHIFT)
_Static_assert((uint32_t)JR_WATCH_STYLE_COUNT <= 16u,
               "JR_WATCH_STYLE_COUNT exceeds the nav word's style field");

/* Wave 4: Lock-free audio spectrum (16 bands), astronomical YMD date, and 3D
 * Geodesic Hologram toggle. Zero internal SRAM bloat (24 B total). */
static _Atomic uint64_t s_spec_lo;
static _Atomic uint64_t s_spec_hi;
static volatile uint32_t s_ymd_word;
static volatile uint32_t s_holo_on;
static volatile uint32_t s_holo_explicit;

static void spectrum_load(uint8_t out[16])
{
    const uint64_t lo = atomic_load_explicit(&s_spec_lo, memory_order_relaxed);
    const uint64_t hi = atomic_load_explicit(&s_spec_hi, memory_order_relaxed);
    for (int i = 0; i < 8; ++i) {
        out[i]     = (uint8_t)((lo >> (i * 8)) & 0xFFu);
        out[i + 8] = (uint8_t)((hi >> (i * 8)) & 0xFFu);
    }
}

void jr_display_spectrum_set(const uint8_t bands[16])
{
    uint64_t lo = 0u, hi = 0u;
    if (bands != NULL) {
        for (int i = 0; i < 8; ++i) {
            lo |= ((uint64_t)bands[i]) << (i * 8);
            hi |= ((uint64_t)bands[i + 8]) << (i * 8);
        }
    }
    atomic_store_explicit(&s_spec_lo, lo, memory_order_relaxed);
    atomic_store_explicit(&s_spec_hi, hi, memory_order_relaxed);
}

void jr_display_date_set_ymd(uint16_t year, uint8_t month, uint8_t day)
{
    const uint32_t w = ((uint32_t)year << 16) | ((uint32_t)month << 8) | (uint32_t)day;
    __atomic_store_n(&s_ymd_word, w, __ATOMIC_RELEASE);
}

void jr_display_holo_set(bool enabled)
{
    __atomic_store_n(&s_holo_explicit, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_holo_on, enabled ? 1u : 0u, __ATOMIC_RELEASE);
}

bool jr_display_holo_get(void)
{
    return __atomic_load_n(&s_holo_on, __ATOMIC_ACQUIRE) != 0u;
}

/* Glass 3.0 unified ring & screen mode. Static init stays CLASSIC so host
 * pixel-identity tests stay deterministic; ESP_PLATFORM boots into AUTO
 * (Chrono-Arc + Aetherium + Atmosphaera + Cortex Command) in jr_display_start(). */
static volatile uint32_t s_glass3_mode = (uint32_t)JR_GLASS3_CLASSIC;
static volatile uint32_t s_glass3_explicit;

void jr_display_glass3_set(jr_glass3_mode_t mode)
{
    if ((uint32_t)mode > (uint32_t)JR_GLASS3_CLASSIC) {
        mode = JR_GLASS3_AUTO;
    }
    __atomic_store_n(&s_glass3_explicit, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_glass3_mode, (uint32_t)mode, __ATOMIC_RELEASE);
}

jr_glass3_mode_t jr_display_glass3_get(void)
{
    return (jr_glass3_mode_t)__atomic_load_n(&s_glass3_mode, __ATOMIC_ACQUIRE);
}

const char *jr_display_glass3_name(jr_glass3_mode_t mode)
{
    switch (mode) {
    case JR_GLASS3_AUTO:      return "auto";
    case JR_GLASS3_CHRONO:    return "chrono";
    case JR_GLASS3_AETHERIUM: return "aetherium";
    case JR_GLASS3_CLASSIC:   return "classic";
    default:                  return "auto";
    }
}

static inline bool glass3_active(void)
{
    return __atomic_load_n(&s_glass3_mode, __ATOMIC_ACQUIRE) !=
           (uint32_t)JR_GLASS3_CLASSIC;
}

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

/* N16.4 Timer overlay: lock-free packed word + fixed label buffer.
 *   bits 15:0   rem_s    (0..65535 seconds remaining)
 *   bits 22:16  pct_left (0..100 percent remaining)
 *   bit  23     ringing  (1 = expired and ringing)
 *   bit  24     active   (1 = timer active/visible)
 */
#define TIMER_REM_MASK    0x0000FFFFu
#define TIMER_PCT_SHIFT   16u
#define TIMER_PCT_MASK    (0x7Fu << TIMER_PCT_SHIFT)
#define TIMER_RINGING_BIT (1u << 23)
#define TIMER_ACTIVE_BIT  (1u << 24)
#define TIMER_LABEL_CAP   16u
static _Atomic uint32_t s_timer_word;
static char             s_timer_label[TIMER_LABEL_CAP];

static inline bool sp_timer_ringing(void)
{
    return (atomic_load_explicit(&s_timer_word, memory_order_acquire) &
            (TIMER_ACTIVE_BIT | TIMER_RINGING_BIT)) ==
           (TIMER_ACTIVE_BIT | TIMER_RINGING_BIT);
}

/* Wave 2 Chronograph Stopwatch overlay: lock-free atomics.
 * s_sw_word:
 *   bit  0      active (1 = visible)
 *   bit  1      running (1 = actively sweeping)
 *   bits 9:2    lap_count (0..255)
 * s_sw_elapsed_ms holds the elapsed milliseconds (written before s_sw_word). */
#define SW_ACTIVE_BIT   (1u << 0)
#define SW_RUNNING_BIT  (1u << 1)
#define SW_LAP_SHIFT    2u
#define SW_LAP_MASK     (0xFFu << SW_LAP_SHIFT)
static _Atomic uint32_t s_sw_word;
static _Atomic uint32_t s_sw_elapsed_ms;

static inline bool sp_stopwatch_active(void)
{
    return (atomic_load_explicit(&s_sw_word, memory_order_acquire) & SW_ACTIVE_BIT) != 0u;
}

static inline bool sp_stopwatch_running(void)
{
    return (atomic_load_explicit(&s_sw_word, memory_order_acquire) &
            (SW_ACTIVE_BIT | SW_RUNNING_BIT)) == (SW_ACTIVE_BIT | SW_RUNNING_BIT);
}

/* Glass-2 transient rim level arc (left rim cyan for volume, right rim amber
 * for brightness). Self-expires in 1200 ms with a smooth 200 ms fade-out.
 *   bits 7:0   pct (0..100)
 *   bit  8     is_brightness (0 = volume left, 1 = brightness right)
 *   bit  9     armed (distinguishes valid timestamp 0 from cold init)
 */
#define RIM_LEVEL_TTL_MS   1200U
#define RIM_LEVEL_FADE_MS   200U
#define RIM_LEVEL_PCT_MASK 0x00FFu
#define RIM_LEVEL_BRT_BIT  (1u << 8)
#define RIM_LEVEL_ARM_BIT  (1u << 9)
static _Atomic uint32_t s_rim_level_word;
static _Atomic uint32_t s_rim_level_start_ms;

static inline bool rim_level_sample(uint32_t now_ms, uint32_t *out_word, uint32_t *out_age)
{
    const uint32_t t0 = atomic_load_explicit(&s_rim_level_start_ms, memory_order_acquire);
    const uint32_t w = atomic_load_explicit(&s_rim_level_word, memory_order_acquire);
    if ((w & RIM_LEVEL_ARM_BIT) == 0u) {
        return false;
    }
    const uint32_t age = now_ms - t0;
    if (age >= RIM_LEVEL_TTL_MS) {
        return false;
    }
    if (out_word != NULL) {
        *out_word = w;
    }
    if (out_age != NULL) {
        *out_age = age;
    }
    return true;
}

static inline bool rim_level_active(uint32_t now_ms)
{
    return rim_level_sample(now_ms, NULL, NULL);
}

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
static int16_t s_wx_temp_f;           /* numeric current temp for thermal arc */
static int16_t s_wx_hi_f;
static int16_t s_wx_lo_f;
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
static char    s_wx_outlook[16];      /* "RAIN 60% 2H" / "DRY 12H" / ""        */
static bool    s_wx_outlook_rain;     /* true when outlook predicts rain       */
static uint32_t s_wx_compose_rev;     /* bumped on each sp_compose_weather     */

/* ACTIVITY, composed once per frame: one row per entry, "KIND SUMMARY", cut
 * to the row's width with the tree's own shortening mark. */
#define SP_ACT_ROW_GLYPHS 24
#define SP_ACT_ROW_CAP    (SP_ACT_ROW_GLYPHS + 1)
static char    s_act_row[ACT_MAX][SP_ACT_ROW_CAP];
#define SP_ACT_AGE_CAP 12
static char    s_act_age[ACT_MAX][SP_ACT_AGE_CAP];   /* "JUST NOW", "7M AGO", "EARLIER" */
static int     s_act_rows;
static int     s_act_klen[ACT_MAX];
static int     s_act_alen[ACT_MAX];
static int     s_act_kw[ACT_MAX];
static int     s_act_aw[ACT_MAX];
static uint32_t s_act_compose_rev;

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

/* WATCH THROUGH A TRANSITION (render task only; 12 bytes of internal RAM).
 *
 *   s_style_shown  the style the glass is drawing. It follows the nav word's
 *                  style at once everywhere except on a settled WATCH, where a
 *                  change runs the dip: the old style eases out, this flips at
 *                  the cut, the new one eases in (s_style_prog 0..256).
 *   s_style_seen   the nav word's style as of the last frame.
 *   s_style_from   the style being dipped out of.
 *   s_watch_s      strength of the hands (and the drawn dials) this frame,
 *                  0..256: the slide's in/out weight, the style dip, and the
 *                  clock's own gate, whichever is least.
 *   s_dial_want    the baked dial the presenter should have bound, JR_FACE_COUNT
 *                  for none. The presenter reads it (watch_dial_face), so the
 *                  engine is asked to rebind only after our own compositor has
 *                  dimmed the old picture to black, never before. */
#define SP_STYLE_MS 340                    /* 3 frames out, 5 in at 24 fps */
#define SP_STYLE_HOLD_MS 300               /* longest the cut waits for the new layer to bind */
static uint16_t s_style_hold_ms;           /* how long it has waited this change */
static uint8_t  s_style_shown;
static uint8_t  s_style_seen;
static uint8_t  s_style_from;
static int      s_style_prog = 256;
static int      s_watch_s;
static int      s_style_black;       /* 0..256: how dark the layer under the dip is */
static volatile uint32_t s_dial_want = (uint32_t)JR_FACE_COUNT;
/* 1 while the watch is on the glass with nothing baked under it (JARVIS, MINIMAL):
 * the presenter then invalidates the glass every pass, as it does for a blitted
 * dial, instead of leaving the hands to the face clip's own ~8 fps tick. */
static volatile uint32_t s_watch_tick;
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
#define SP_SHADE_PACE_NUM 7U       /* the entry clock runs at 7/4: 700 ms of authoring in 400 */
#define SP_SHADE_PACE_DEN 4U
#define SP_SHADE_OPEN_MS  300      /* the veil and the shade's own alpha, in (7 frames)   */
#define SP_SHADE_CLOSE_MS 310      /* and out (7.4 frames): leaving is quicker than arriving */
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
        {
            const uint32_t tw = atomic_load_explicit(&s_timer_word, memory_order_acquire);
            if ((tw & TIMER_ACTIVE_BIT) != 0u) {
                sp_str(s_detail_label[4], 0, SP_COL_MAX, "TIMER");
                if ((tw & TIMER_RINGING_BIT) != 0u) {
                    sp_str(s_detail_value[4], 0, SP_COL_MAX, "RINGING");
                } else {
                    sp_clock_str(s_detail_value[4], SP_COL_MAX, tw & TIMER_REM_MASK);
                }
                s_detail_rows = 5;
            }
        }
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
        {
            const uint32_t tw = atomic_load_explicit(&s_timer_word, memory_order_acquire);
            if ((tw & TIMER_ACTIVE_BIT) != 0u) {
                sp_str(s_detail_label[3], 0, SP_COL_MAX, "TIMER");
                if ((tw & TIMER_RINGING_BIT) != 0u) {
                    sp_str(s_detail_value[3], 0, SP_COL_MAX, "RINGING");
                } else {
                    sp_clock_str(s_detail_value[3], SP_COL_MAX, tw & TIMER_REM_MASK);
                }
                s_detail_rows = 4;
            }
        }
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
    s_wx_outlook[0] = '\0';
    s_wx_outlook_rain = false;
    if (!w->valid) {
        sp_str(s_wx_head, 0, SP_LABEL_CAP, "NO WEATHER");
        ++s_wx_compose_rev;
        return;
    }

    /* Headline: the word alone, fitted at a word (wx_cond_fit), never
     * clipped mid-glyph. The number is the disc's — the panel read "76"
     * over "OVERCAST 76", one reading printed twice. */
    char cond[SP_LABEL_CAP];
    (void)wx_cond_fit(cond, sizeof cond, w->condition, SP_LABEL_CAP - 1);
    (void)sp_str(s_wx_head, 0, SP_LABEL_CAP, cond);

    s_wx_temp_f = w->temp_f;
    s_wx_hi_f = w->hi_f;
    s_wx_lo_f = w->lo_f;
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
        if (s_wx_hours > 0) {
            int peak_pct = 0;
            int peak_h = 0;
            for (int k = 0; k < SP_WX_HOURS; ++k) {
                const int p = s_wx_hour_pct[k];
                if (p != SP_WX_HOUR_NONE && p > peak_pct) {
                    peak_pct = p;
                    peak_h = k;
                }
            }
            if (peak_pct >= 20) {
                s_wx_outlook_rain = true;
                int ol = sp_str(s_wx_outlook, 0, (int)sizeof s_wx_outlook, "RAIN ");
                ol = sp_num(s_wx_outlook, ol, (int)sizeof s_wx_outlook, (uint32_t)peak_pct);
                if (peak_h == 0) {
                    (void)sp_str(s_wx_outlook, ol, (int)sizeof s_wx_outlook, "% NOW");
                } else {
                    ol = sp_str(s_wx_outlook, ol, (int)sizeof s_wx_outlook, "% ");
                    ol = sp_num(s_wx_outlook, ol, (int)sizeof s_wx_outlook, (uint32_t)peak_h);
                    (void)sp_str(s_wx_outlook, ol, (int)sizeof s_wx_outlook, "H");
                }
            } else {
                s_wx_outlook_rain = false;
                int ol = sp_str(s_wx_outlook, 0, (int)sizeof s_wx_outlook, "DRY ");
                ol = sp_num(s_wx_outlook, ol, (int)sizeof s_wx_outlook, (uint32_t)s_wx_hours);
                (void)sp_str(s_wx_outlook, ol, (int)sizeof s_wx_outlook, "H");
            }
        }
    }
    const uint32_t sw = __atomic_load_n(&s_sun_word, __ATOMIC_ACQUIRE);
    if ((sw & (1u << 22)) != 0u) {
        len = sp_hhmm(s_wx_sun, 0, (int)sizeof s_wx_sun, (int)(sw & 0x7FFu));
        len = sp_str(s_wx_sun, len, (int)sizeof s_wx_sun, " ");
        (void)sp_hhmm(s_wx_sun, len, (int)sizeof s_wx_sun, (int)((sw >> 11) & 0x7FFu));
    }
    ++s_wx_compose_rev;
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
        /* Hoist label width measurements out of the 39-strip loop. */
        s_act_klen[i] = sp_len(s_act_kind[i], ACT_KIND_CAP - 1);
        s_act_alen[i] = sp_len(s_act_age[i], SP_ACT_AGE_CAP - 1);
        s_act_kw[i] = glass_text_width(GLASS_FONT_LABEL, s_act_kind[i], (size_t)s_act_klen[i], 2);
        s_act_aw[i] = glass_text_width(GLASS_FONT_LABEL, s_act_age[i], (size_t)s_act_alen[i], 2);
    }
    for (uint32_t i = n; i < (uint32_t)ACT_MAX; ++i) {
        s_act_klen[i] = 0;
        s_act_alen[i] = 0;
        s_act_kw[i] = 0;
        s_act_aw[i] = 0;
    }
    ++s_act_compose_rev;
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

/* The caption, once a frame, render task, after the slide has stepped.
 *
 * Refreshes what is on the glass from the staged words, runs a label's phase
 * (out, swap at the cut, in), and times a label out: the clock starts only when
 * the transition that named it has settled, runs 600 ms (a screen) or 1200 ms
 * (a style), and then the flag clears and the ordinary 250 ms fade takes it off.
 * Anything on the glass under a sheet is not drawn (apply_caption_overlay), so a
 * label up when a sheet opens is expired at once rather than left to reappear. */
static void caption_copy_staged(void)
{
    memcpy(s_capd_line[0], s_caption_line[0], sizeof s_capd_line[0] - 1U);
    memcpy(s_capd_line[1], s_caption_line[1], sizeof s_capd_line[1] - 1U);
    s_capd_line[0][sizeof s_capd_line[0] - 1U] = '\0';
    s_capd_line[1][sizeof s_capd_line[1] - 1U] = '\0';
    s_capd_y[0] = (int16_t)s_caption_y[0];
    s_capd_y[1] = (int16_t)s_caption_y[1];
    s_capd_grey = s_caption_grey;
    s_capd_lease = s_caption_lease;
    s_capd_label = s_caption_label != 0U;
}

static int caption_phase_factor(void)
{
    if (s_cap_p >= 256) {
        return 256;
    }
    if (s_cap_dip) {
        return s_cap_p < TR_CUT_SLIDE ? tr_out(s_cap_p, TR_CUT_SLIDE)
                                      : tr_in(s_cap_p, TR_CUT_SLIDE);
    }
    return tr_in(s_cap_p, TR_CUT_SLIDE);       /* nothing was up: it arrives with its screen */
}

static void caption_tick(uint32_t dt_ms)
{
    const uint32_t rev = __atomic_load_n(&s_caption_revision, __ATOMIC_ACQUIRE);
    const int on = __atomic_load_n(&s_caption_on, __ATOMIC_ACQUIRE);
    const bool sheet = s_shade_ease > 0 || s_detail_ease > 0;
    if (rev != s_cap_rev_seen) {
        s_cap_rev_seen = rev;
        if (on != 0) {
            const bool label = s_caption_label != 0U;
            if (!label) {
                caption_copy_staged();             /* speech: no dip, no lag */
                s_cap_p = 256;
                s_cap_dip = false;
                s_cap_pend = false;
                s_cap_hold_ms = 0U;
            } else {
                const int f_now = caption_phase_factor();
                /* Only a name that is still held eases out. One that has timed
                 * out and is on its 250 ms fade is already leaving: dipping it
                 * again would bring the old words back for the length of the
                 * dip (a style name read "WATCH - DRESS" over MINIMAL's dip). */
                const bool up = s_cap_was_on && s_caption_prog > 0 && f_now > 0;
                if (up) {
                    /* a name is on the glass: it eases out where it stands, the
                     * new one swaps in at the cut. Entering the dip at the point
                     * that matches how far the old one has got keeps a quick
                     * second swipe from jumping its brightness. */
                    s_cap_dip = true;
                    s_cap_pend = true;
                    s_cap_p = (TR_CUT_SLIDE * (256 - f_now)) / 256;
                } else {
                    caption_copy_staged();         /* nothing to ease out */
                    s_cap_dip = false;
                    s_cap_pend = false;
                    s_cap_p = 0;
                }
                s_cap_hold_ms = s_caption_label == (uint8_t)JR_DISPLAY_LABEL_WATCH_STYLE + 1U
                                    ? JR_DISPLAY_LABEL_STYLE_HOLD_MS
                                    : JR_DISPLAY_LABEL_SCREEN_HOLD_MS;
            }
            s_cap_age_ms = 0U;
        } else {
            s_cap_hold_ms = 0U;
        }
    }
    if (s_cap_p < 256) {
        int step = (int)((dt_ms * 256u) / (uint32_t)JR_DISPLAY_SPACE_MS);
        if (step < 1) {
            step = 1;
        }
        s_cap_p += step;
        if (s_cap_pend && s_cap_p >= TR_CUT_SLIDE) {
            caption_copy_staged();
            s_cap_pend = false;
        }
        if (s_cap_p >= 256) {
            s_cap_p = 256;
            s_cap_dip = false;
        }
    }
    if (s_cap_hold_ms != 0U && on != 0) {
        const bool settled = s_space_prog >= 256 && s_style_prog >= 256 && s_cap_p >= 256;
        if (sheet) {
            s_cap_age_ms = s_cap_hold_ms;
        } else if (settled) {
            s_cap_age_ms += dt_ms;
        }
        if (s_cap_age_ms >= s_cap_hold_ms) {
            s_cap_hold_ms = 0U;
            jr_display_caption_clear();
        }
    }
    s_cap_was_on = on != 0;
    s_cap_alpha = (s_caption_ease * caption_phase_factor()) >> 8;
}

/* No caption of any kind is drawn while the control shade or the detail sheet is up
 * (they carry their own hint line in the band); only the pinned one outranks. */
static bool caption_hidden_by_sheet(void)
{
    return (s_shade_ease > 0 || s_detail_ease > 0) && !atomic_load(&s_caption_pinned);
}

/* The baked dial a style shows, JR_FACE_COUNT for the styles drawn in code
 * (JARVIS, MINIMAL). */
static jr_face_t watch_dial_of_style(uint8_t style)
{
    switch (style) {
    case JR_WATCH_DIVER:     return JR_FACE_DIAL_DIVER;
    case JR_WATCH_DRESS:     return JR_FACE_DIAL_DRESS;
    case JR_WATCH_PILOT:     return JR_FACE_DIAL_PILOT;
    case JR_WATCH_FUTURE:    return JR_FACE_DIAL_FUTURE;
    case JR_WATCH_SKELETON:      return JR_FACE_DIAL_SKELETON;
    case JR_WATCH_CELESTIAL:     return JR_FACE_DIAL_CELESTIAL;
    case JR_WATCH_STYLE_ORRERY:  return JR_FACE_DIAL_ORRERY;
    case JR_WATCH_STYLE_REACTOR: return JR_FACE_DIAL_REACTOR;
    default:                     return JR_FACE_COUNT;
    }
}

static inline bool watch_style_has_dial(uint8_t style)
{
    return watch_dial_of_style(style) != JR_FACE_COUNT;
}

/* True once the presenter has really switched to what watch_publish asked for:
 * the wanted dial is the one being blitted, or no dial is wanted and none is. A
 * layer that is coming up waits for this, covered, so the engine's latency (a
 * decode, a rebind) shows as a moment of black and never as the old picture. */
static bool watch_layer_ready(void)
{
    const jr_face_t want = (jr_face_t)__atomic_load_n(&s_dial_want, __ATOMIC_RELAXED);
    if (want == JR_FACE_COUNT) {
#ifdef ESP_PLATFORM
        if (__atomic_load_n(&s_minimal_want, __ATOMIC_RELAXED) != 0u) {
            return !s_dial_blit || s_dial_minimal;
        }
#endif
        return !s_dial_blit;
    }
    return s_dial_blit && s_dial_frame_face == want;
}

/* True once the presenter has bound the layer a style draws on: its dial, or,
 * for a style with no art, nothing (the dial released). */
static bool watch_style_bound(uint8_t style)
{
    const jr_face_t f = watch_dial_of_style(style);
#ifdef ESP_PLATFORM
    if (style == (uint8_t)JR_WATCH_MINIMAL) {
        return !s_dial_blit || s_dial_minimal;
    }
#endif
    return f == JR_FACE_COUNT ? !s_dial_blit : (s_dial_blit && s_dial_frame_face == f);
}

/* Once a frame, render task, after the slide and the style dip have stepped:
 * decide the dial the presenter should bind and the strength of the hands.
 *
 * Entering WATCH the old screen eases out first and the dial is asked for only
 * at the cut, when the glass is dark; leaving, the dial is held until the cut so
 * it can dim out with the hands, and released after. A style change asks for the
 * new dial at its own cut (s_style_shown flips there). The hands take the least
 * of the slide's weight, the dip's weight and the clock's gate; away from WATCH
 * (the peek) the clock's own fade is all there is. */
static void watch_publish(void)
{
    const bool clock_on = (__atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE) & (1u << 16)) != 0u;
    const int p = s_space_prog;
    const bool to_watch = s_space_to == (uint8_t)JR_DISPLAY_SPACE_WATCH;
    const bool from_watch = s_space_from == (uint8_t)JR_DISPLAY_SPACE_WATCH;
    jr_face_t want = JR_FACE_COUNT;
    int w = 256;
    int strength;
    /* With the panel dip the hands are never part strength: they are all there or
     * not there, and the cut is the dark (s_dip_swapped), not the progress. */
    const bool dip_sp = s_dip_mode == DIP_MODE_SPACE;
    const bool past_cut = dip_sp ? s_dip_swapped : p >= TR_CUT_SLIDE;
    if (to_watch) {
        if (!from_watch && p < 256) {                       /* entering */
            w = dip_sp ? (s_dip_swapped ? 256 : 0) : tr_in(p, TR_CUT_SLIDE);
            if (past_cut && clock_on) {
                want = watch_dial_of_style(s_style_shown);
            }
        } else if (clock_on) {
            want = watch_dial_of_style(s_style_shown);
        }
        if (s_style_prog < 256 && s_dip_mode != DIP_MODE_STYLE) {   /* a style change */
            const int ws = s_style_prog < TR_CUT_STYLE ? tr_out(s_style_prog, TR_CUT_STYLE)
                                                       : tr_in(s_style_prog, TR_CUT_STYLE);
            w = w < ws ? w : ws;
        }
        const int gate = clock_on ? 256 : s_clock_ease;
        strength = w < gate ? w : gate;
    } else if (from_watch && p < 256) {                     /* leaving */
        w = dip_sp ? (s_dip_swapped ? 0 : 256) : tr_out(p, TR_CUT_SLIDE);
        if (!past_cut) {
            want = watch_dial_of_style(s_style_shown);
        }
        strength = w;
    } else {
        strength = s_clock_ease;
        if (from_watch && !clock_on) {                      /* the slide away is done */
            s_clock_prog = 0;
            s_clock_ease = 0;
            strength = 0;
        }
    }
    s_watch_s = strength;
    __atomic_store_n(&s_watch_tick, (strength > 0 && want == JR_FACE_COUNT) ? 1u : 0u,
                     __ATOMIC_RELAXED);
    bool dial_changed =
        __atomic_exchange_n(&s_dial_want, (uint32_t)want, __ATOMIC_ACQ_REL) != (uint32_t)want;
#ifdef ESP_PLATFORM
    const bool want_min =
        want == JR_FACE_COUNT && s_style_shown == (uint8_t)JR_WATCH_MINIMAL &&
        ((to_watch && clock_on && (from_watch || p >= 256 || past_cut)) ||
         (!to_watch && from_watch && p < 256 && !past_cut));
    const uint32_t min_u = want_min ? 1u : 0u;
    if (__atomic_exchange_n(&s_minimal_want, min_u, __ATOMIC_ACQ_REL) != min_u) {
        dial_changed = true;
    }
#endif
    if (dial_changed) {
        TaskHandle_t presenter = __atomic_load_n(&s_display.task, __ATOMIC_ACQUIRE);
        if (presenter != NULL) {
            xTaskNotifyGive(presenter);   /* the dial must not wait a period */
        }
    }
}

/* The panel dip may light the new content only when the layer under it is what the
 * presenter has really bound (the dial, the face coming back from a parked
 * backdrop): a late rebind shows as a longer dark, never as the old picture. */
static bool sp_dip_layer_ready(void)
{
    if (s_dip_mode == DIP_MODE_STYLE) {
        return watch_style_bound(s_style_shown);
    }
    const bool vis_to = !sp_space_is_black(s_space_to) &&
                        !(s_space_to == (uint8_t)JR_DISPLAY_SPACE_WATCH && !watch_style_has_dial(s_style_shown));
    return !vis_to || (watch_layer_ready() && !s_parked);
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

    /* The panel dip: the frame in which the glass has gone dark is the first one
     * drawn with the new content, and the style (if one is waiting) flips with
     * it. Latched once a frame so a frame composes at one state. */
    if (s_dip_mode != DIP_MODE_NONE && !s_dip_swapped && s_dip_phase != DIP_OUT) {
        s_dip_swapped = true;
        s_style_shown = s_style_seen;
    }
    if (s_dip_entry_next != UINT32_MAX && (s_dip_mode != DIP_MODE_SPACE || s_dip_swapped)) {
        s_entry_ms = s_dip_entry_next;
        s_dip_entry_next = UINT32_MAX;
    }

    if (serial != s_space_serial_seen) {
        s_space_serial_seen = serial;
        /* The panel dip: a change of screen while the glass is still showing the
         * one it left (OUT, not yet swapped) keeps showing that one, so no screen
         * that was never lit pops in at part brightness. */
        const bool glass_holds_old = s_dip_on && s_dip_mode == DIP_MODE_SPACE && !s_dip_swapped &&
                                     s_dip_phase != DIP_IDLE;
        if (!glass_holds_old) {
            s_space_from = (uint8_t)((nav >> NAV_PREV_SHIFT) & NAV_SPACE_MASK);
        }
        s_space_to = space;
        if (s_dip_on) {
            if (s_dip_mode == DIP_MODE_STYLE) {
                s_style_prog = 256;      /* the style dip is folded into this one: its flip happens at the swap below */
            }
            dip_begin(DIP_MODE_SPACE, TR_CUT_SLIDE, (uint32_t)JR_DISPLAY_SPACE_MS, now_ms);
        }
        s_space_dir = (nav & NAV_FORWARD_BIT) != 0U ? 1 : -1;
        s_space_prog = 0;
        s_space_hold_ms = now_ms + (uint32_t)JR_DISPLAY_SPACE_HOLD_MS;
        const uint32_t entry_start = (space == (uint8_t)JR_DISPLAY_SPACE_STATUS ||
                                      space == (uint8_t)JR_DISPLAY_SPACE_WEATHER ||
                                      space == (uint8_t)JR_DISPLAY_SPACE_DESK ||
                                      space == (uint8_t)JR_DISPLAY_SPACE_ACTIVITY)
                                         ? 0U : SP_ENTRY_TOTAL_MS;
        if (s_dip_on) {
            s_dip_entry_next = entry_start;   /* (the screen on the glass keeps its own clock until the swap) */
        } else {
            s_entry_ms = entry_start;
        }
    } else {
        /* The entry clock starts when the incoming screen does, at the cut:
         * running it from the swipe would sweep the ring while it is still
         * invisible and show the screen already half drawn. */
        /* (With the panel dip the cut is the dark's end: the clock runs once the
         * new screen is being lit, not while it is flushed behind the dark.) */
        const bool cut_passed = s_dip_mode == DIP_MODE_SPACE
                                    ? (s_dip_swapped ? s_dip_phase != DIP_HOLD : s_dip_entry_next != UINT32_MAX)
                                    : s_space_prog >= TR_CUT_SLIDE;
        if (s_entry_ms < SP_ENTRY_TOTAL_MS && cut_passed) {
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

    /* The watch's style, through the dip. On a settled WATCH a new style does
     * not appear in the frame the nav word changes: the old dial and hands ease
     * out, the style flips at the cut, the new ones ease in. Anywhere else the
     * glass is not showing the watch, so the style follows at once. */
    {
        const uint8_t style_nav = (uint8_t)((nav & NAV_STYLE_MASK) >> NAV_STYLE_SHIFT);
        if (style_nav != s_style_seen) {
            const bool on_watch = space == (uint8_t)JR_DISPLAY_SPACE_WATCH &&
                                  s_space_to == space && s_space_prog >= 256;
            s_style_seen = style_nav;
            if (on_watch && s_style_shown != style_nav) {
                s_style_from = s_style_shown;
                s_style_prog = 0;
                s_style_hold_ms = 0;
                if (s_dip_on) {
                    dip_begin(DIP_MODE_STYLE, TR_CUT_STYLE, (uint32_t)SP_STYLE_MS, now_ms);
                }
            } else if (s_style_prog >= 256 && !(s_dip_mode == DIP_MODE_SPACE && !s_dip_swapped)) {
                s_style_shown = style_nav;      /* (a screen change in its dark waits to flip it at the swap) */
                s_style_from = style_nav;
            }
        }
        if (s_dip_mode == DIP_MODE_STYLE) {
            /* The panel dips and the style flips in the dark (the latch above);
             * s_style_prog only says "a style change is under way" until the dip
             * is over, and nothing is drawn at part strength. */
            s_style_prog += (int)((dt_ms * 256u) / (uint32_t)SP_STYLE_MS);
            if (s_style_prog > 255) {
                s_style_prog = 255;
            }
            if (s_dip_phase == DIP_IDLE) {
                s_style_prog = 256;
                s_style_from = s_style_shown;
                s_dip_mode = DIP_MODE_NONE;
            }
        } else if (s_style_prog < 256) {
            int ystep = (int)((dt_ms * 256u) / (uint32_t)SP_STYLE_MS);
            if (ystep < 1) {
                ystep = 1;
            }
            s_style_prog += ystep;
            if (s_style_prog >= TR_CUT_STYLE) {
                s_style_shown = s_style_seen;
            }
            /* The new style's layer is bound by the presenter a frame or two
             * after the cut, and the ease-in is drawn only once it is. Left to
             * run, those frames were taken out of the ease (the dial came up
             * 0, 55 %, 95 %). Hold at the cut until it is bound, then ease in
             * over the full five frames; a layer that never binds is waited
             * for SP_STYLE_HOLD_MS and no longer. */
            if (s_style_prog > TR_CUT_STYLE && s_style_hold_ms < SP_STYLE_HOLD_MS &&
                !watch_style_bound(s_style_shown)) {
                s_style_prog = TR_CUT_STYLE;
                s_style_hold_ms = (uint16_t)(s_style_hold_ms + (dt_ms < SP_STYLE_HOLD_MS ? dt_ms : SP_STYLE_HOLD_MS));
            }
            if (s_style_prog >= 256) {
                s_style_prog = 256;
                s_style_from = s_style_shown;
            }
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
    if (s_dip_mode == DIP_MODE_SPACE) {
        /* the screen change is not over until the panel is lit again */
        if (s_dip_phase != DIP_IDLE && s_space_prog > 255) {
            s_space_prog = 255;
        }
        if (s_dip_phase == DIP_IDLE && s_space_prog >= 256) {
            s_dip_mode = DIP_MODE_NONE;
        }
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
        /* (With the panel dip the mark, like the screens, is at the old place until
         * the glass is dark and at the new one after it.) */
        const int mark_ease = s_dip_mode == DIP_MODE_SPACE ? (s_dip_swapped ? 256 : 0) : s_space_ease;
        const int target = a_from + (d * mark_ease) / 256;
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

    /* The shade is lopsided on purpose now: it answers the swipe that opens it
     * in about 11 frames (the veil in 7, the arcs and the button on the
     * authored 700 ms timeline run at 7/4 speed) and leaves faster than it
     * came, in about 7.5. */
    if (sp_shade_open()) {
        int ostep = (int)((dt_ms * 256u) / (uint32_t)SP_SHADE_OPEN_MS);
        if (ostep < 1) {
            ostep = 1;
        }
        s_shade_prog += ostep;
        if (s_shade_prog > 256) {
            s_shade_prog = 256;
        }
        if (!s_shade_was_open) {
            s_shade_was_open = true;
            s_shade_ms = 0U;
            s_breath_ms = 0U;
        } else if (s_shade_ms < SP_SHADE_ENTRY_MS) {
            s_shade_ms += (dt_ms * SP_SHADE_PACE_NUM) / SP_SHADE_PACE_DEN;
            if (s_shade_ms > SP_SHADE_ENTRY_MS) {
                s_shade_ms = SP_SHADE_ENTRY_MS;
            }
        }
        if (sp_privacy_muted()) {
            s_breath_ms = (s_breath_ms + dt_ms) % SP_BREATH_MS;
        }
    } else {
        int clstep = (int)((dt_ms * 256u) / (uint32_t)SP_SHADE_CLOSE_MS);
        if (clstep < 1) {
            clstep = 1;
        }
        s_shade_prog -= clstep;
        if (s_shade_prog < 0) {
            s_shade_prog = 0;
        }
        s_shade_was_open = false;
        s_shade_ms = SP_SHADE_ENTRY_MS;
    }
    s_shade_ease = fade_smoothstep(s_shade_prog);

    watch_publish();
    if (s_dip_mode != DIP_MODE_NONE && s_dip_swapped && !s_dip_ready && sp_dip_layer_ready()) {
        s_dip_ready = true;                  /* this frame is the new screen, whole: its flush ends the dark */
        s_dip_ready_frame = s_frame_no;
    }

    /* WHAT IS UNDER THE SHELL, through a slide. The face (or a baked dial) is
     * the layer; a data screen, and a watch that draws on black, are not.
     * tr_layer gives its brightness: the outgoing layer dims out, the incoming
     * one comes up after the cut. A settled screen is just yes or no. The
     * backdrop veil that used to hold a ring screen's face at a quarter is gone
     * from slides (the layer is dark by the cut, which is darker still); only
     * the sheet and the shade veil now. */
    int veil = 0;
    const bool dip_sp = s_dip_mode == DIP_MODE_SPACE;
    const bool to_black = sp_space_is_black(s_space_to);
    const bool from_black = sp_space_is_black(s_space_from);
    int black = 0;
    if (dip_sp) {
        /* the panel dip: what is under the shell is the settled one of whichever
         * screen is on the glass; the dimming is the panel's */
        black = (s_dip_swapped ? to_black : from_black) ? 256 : 0;
    } else if (s_space_prog >= 256) {
        black = to_black ? 256 : 0;
    } else {
        const bool vis_from = !from_black &&
            !(s_space_from == (uint8_t)JR_DISPLAY_SPACE_WATCH && !watch_style_has_dial(s_style_from));
        const bool vis_to = !to_black &&
            !(s_space_to == (uint8_t)JR_DISPLAY_SPACE_WATCH && !watch_style_has_dial(s_style_shown));
        int layer = tr_layer(s_space_prog, TR_CUT_SLIDE, vis_from, vis_to);
        /* the incoming layer is not there until the presenter has bound it:
         * the dial (or the face coming back from one) stays covered until the
         * engine has really switched, so a late rebind never shows */
        if (s_space_prog >= TR_CUT_SLIDE && vis_to && !watch_layer_ready()) {
            layer = 0;
        }
        black = 256 - layer;
    }
    s_space_black = black;
    /* A style change on a settled WATCH dips the same layer: the old dial dims
     * out, the new one (or black, for a style with no art) comes up. It is not
     * a slide, so it asks nothing of the orbit rail and has its own number. */
    s_style_black = 0;
    if (s_style_prog < 256 && s_dip_mode != DIP_MODE_STYLE &&
        s_space_to == (uint8_t)JR_DISPLAY_SPACE_WATCH) {
        const bool vf = watch_style_has_dial(s_style_from);
        const bool vt = watch_style_has_dial(s_style_shown);
        int layer = tr_layer(s_style_prog, TR_CUT_STYLE, vf, vt);
        if (s_style_prog >= TR_CUT_STYLE && vt && !watch_layer_ready()) {
            layer = 0;
        }
        s_style_black = 256 - layer;
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
/* Modal ask overlay (Glass-2): dims the strip cleanly, renders the wrapped
 * question and per-arc labels in anti-aliased Barlow (glass_text_row), and
 * draws crisp anti-aliased round-capped choice arcs at r224-230 via
 * glass_arc_row (with a soft cyan glow on the selected confirmation arc).
 * Pure function of (s_choices, s_choice_*, e, y) -> 12-row strip invariant. */
static void apply_ask_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                              uint16_t *pixels, int cn, int e)
{
    const int k = (e * 32) >> 8;
    if (k <= 0) {
        return;
    }
    const uint8_t alpha = (uint8_t)(e > 255 ? 255 : e);
    const int sel = s_choice_selected;
    const bool swap = ctx->board.swap_color_bytes;
    if (k >= 32) {
        dim_strip(ctx, y1, y2, pixels);
    } else {
        fade_strip(ctx, y1, y2, pixels, k);
    }

    const glass_font_id_t qf = s_choice_q_font;
    const int qtrk = (qf == GLASS_FONT_VALUE) ? 0 : 1;
    const glass_metrics_t qm = glass_font_metrics(qf);
    const glass_metrics_t ml = glass_font_metrics(GLASS_FONT_LABEL);

    /* Choice arcs live strictly in r224..230 (inside the r223-231 choice band,
     * clear of the r221-222 privacy ring and the r232.5 glass edge). */
    const int32_t cx16 = 233 * 16;
    const int32_t cy16 = 233 * 16;
    const int32_t r_in16 = 224 * 16;
    const int32_t r_out16 = 230 * 16;
    const int max_n = cn > HUD_CHOICE_MAX ? HUD_CHOICE_MAX : cn;
    int arc_y0[HUD_CHOICE_MAX], arc_y1[HUD_CHOICE_MAX];
    int32_t arc_start[HUD_CHOICE_MAX], arc_sweep[HUD_CHOICE_MAX];
    bool arc_ok[HUD_CHOICE_MAX];
    for (int i = 0; i < max_n; ++i) {
        arc_ok[i] = false;
        if (s_choices[i].label == NULL) {
            continue;
        }
        const int span_q8 = (s_choices[i].a1 - s_choices[i].a0) & 255;
        if (span_q8 <= 0) {
            continue;
        }
        arc_start[i] = (int32_t)((s_choices[i].a0 & 255) << 8);
        arc_sweep[i] = (int32_t)(span_q8 << 8);
        if (glass_arc_rows(cx16, cy16, r_in16, r_out16,
                           arc_start[i], arc_sweep[i], true,
                           &arc_y0[i], &arc_y1[i])) {
            arc_ok[i] = true;
        }
    }

    const uint8_t a_unsel = (uint8_t)(((unsigned)alpha * (sel >= 0 ? 95u : 190u)) / 255u);
    const uint8_t a_lab_unsel = (uint8_t)(((unsigned)alpha * (sel >= 0 ? 130u : 225u)) / 255u);

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;

        /* Question lines: anti-aliased Barlow in cool white. */
        for (int l = 0; l < 2; ++l) {
            const char *text = s_choice_q_line[l];
            const int ty = s_choice_q_y[l];
            if (text[0] == '\0' || y < ty + qm.ink_top || y > ty + qm.ink_bottom) {
                continue;
            }
            const size_t qlen = strnlen(text, 24U);
            if (qlen > 0U) {
                glass_text_row(row, y, 0, HUD_W - 1, qf, text, qlen,
                               s_choice_q_x[l], ty, qtrk, 0xF79Eu, alpha, swap);
            }
        }

        /* Per-arc labels: anti-aliased GLASS_FONT_LABEL at cached safe anchors. */
        for (int i = 0; i < max_n; ++i) {
            const char *lab = s_choices[i].label;
            const int cap = s_choice_label_len[i];
            const int ly = s_choice_label_y[i];
            if (lab == NULL || cap <= 0 ||
                y < ly + ml.ink_top || y > ly + ml.ink_bottom) {
                continue;
            }
            const size_t len = strnlen(lab, (size_t)cap);
            if (len == 0U) {
                continue;
            }
            const bool is_sel = (i == sel);
            glass_text_row(row, y, 0, HUD_W - 1, GLASS_FONT_LABEL, lab, len,
                           s_choice_label_x[i], ly, 1,
                           is_sel ? 0xF79Eu : 0x07FFu,
                           is_sel ? alpha : a_lab_unsel, swap);
        }

        /* Crisp Glass-2 choice arcs in r224..230 (selected arc glows & turns bright white). */
        for (int i = 0; i < max_n; ++i) {
            if (!arc_ok[i]) {
                continue;
            }
            const bool is_sel = (i == sel);
            const int pad = is_sel ? 5 : 0;
            if (y < arc_y0[i] - pad || y > arc_y1[i] + pad) {
                continue;
            }
            if (is_sel) {
                glass_glow_row(row, y, 0, HUD_W - 1, cx16, cy16, r_in16, r_out16,
                               arc_start[i], arc_sweep[i], true,
                               4 * 16, 115u, 0x07FFu, alpha, swap);
            }
            if (y >= arc_y0[i] && y <= arc_y1[i]) {
                glass_arc_row(row, y, 0, HUD_W - 1, cx16, cy16, r_in16, r_out16,
                              arc_start[i], arc_sweep[i],
                              is_sel ? 0xF79Eu : 0x07FFu,
                              is_sel ? alpha : a_unsel, swap);
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
#define JR_CAPTION_HALO 3          /* a label's halo reaches this far past its glyphs */

static void apply_caption_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                                  uint16_t *pixels)
{
    const bool label = s_capd_label;
    const glass_metrics_t m = glass_font_metrics(GLASS_FONT_LABEL);
    /* A label has no plate, so its band is only as tall as its glyphs and their
     * halo; a message keeps the round-safe band the plate fills. */
    int band_lo = JR_CAPTION_Y0, band_hi = JR_CAPTION_Y1;
    if (glass3_active() && band_hi > 414) {
        /* Keep caption dim plate clear of Chrono-Arc's bottom bezel arc (y>=419). */
        band_hi = 414;
    }
    if (label) {
        const int two = s_capd_line[1][0] != '\0';
        band_lo = s_capd_y[0] + m.ink_top - JR_CAPTION_HALO;
        band_hi = (two ? s_capd_y[1] : s_capd_y[0]) + m.ink_bottom + JR_CAPTION_HALO;
    }
    int ys = y1 > band_lo ? y1 : band_lo;
    int ye = (y2 - 1) < band_hi ? (y2 - 1) : band_hi;
    if (ys > ye) {
        return;                    /* strip misses the band: one compare */
    }
    const int e = s_cap_alpha;
    if (((e * 32) >> 8) <= 0) {
        return;
    }
    /* Over an active remote canvas, keep lease & pinned OTA banners only so
     * ambient screen hints never cover pushed artwork. */
    if (s_canvas_ease > 0 && !s_capd_lease &&
        !atomic_load_explicit(&s_caption_pinned, memory_order_relaxed)) {
        return;
    }
    /* No caption, of any kind, is drawn over the control shade or the detail
     * sheet: both carry their own hint line in this band, and a caption over
     * them wrapped across it. (Only the pinned one, the OTA warning, outranks.) */
    if (caption_hidden_by_sheet()) {
        return;
    }
    int face_left = 256 - (s_space_black > s_shade_ease ? s_space_black : s_shade_ease);
    if (face_left < 0 || label) {
        face_left = 0;             /* a label wears a halo, not a plate */
    }
    const int k = (((e * 32) >> 8) * face_left) >> 8;
    const bool swap = ctx->board.swap_color_bytes;
    const int c = e > 255 ? 255 : e;
    const uint16_t color = s_capd_grey ? SP_C_GREY : SP_C_INK;
    const uint16_t halo = panel_order_color(ctx, 0x0000u);
    static const int8_t halo_dx[8] = { -2, 2, 0, 0, -1, 1, -1, 1 };
    static const int8_t halo_dy[8] = { 0, 0, -2, 2, -1, -1, 1, 1 };
    int tx[2], len[2];
    for (int l = 0; l < 2; ++l) {
        len[l] = (int)strnlen(s_capd_line[l], 19U);
        tx[l] = (HUD_W - glass_text_width(GLASS_FONT_LABEL, s_capd_line[l],
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
            const int ty = s_capd_y[l];
            if (len[l] <= 0) {
                continue;
            }
            if (label) {
                if (y < ty + m.ink_top - JR_CAPTION_HALO || y > ty + m.ink_bottom + JR_CAPTION_HALO) {
                    continue;
                }
                const uint8_t ha = (uint8_t)((c * 230) >> 8);
                for (int h = 0; h < 8; ++h) {
                    glass_text_row(row, y, xlo, xhi, GLASS_FONT_LABEL, s_capd_line[l],
                                   (size_t)len[l], tx[l] + halo_dx[h], ty + halo_dy[h],
                                   JR_CAPTION_TRACK, halo, ha, swap);
                }
            } else if (y < ty + m.ink_top || y > ty + m.ink_bottom) {
                continue;
            }
            glass_text_row(row, y, xlo, xhi, GLASS_FONT_LABEL, s_capd_line[l],
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

/* WHICH DIAL, IF ANY. On WATCH with the clock published and a style whose
 * dial is baked art, the face under the hands is that dial. JARVIS keeps its
 * black disc and MINIMAL draws its own dial, so both answer JR_FACE_COUNT. */
static jr_face_t watch_dial_face(void)
{
    /* The render task decides (watch_publish) and the presenter obeys: the dial
     * is not bound at the instant the nav word changes but when our own
     * compositor has covered the old picture, and it is released only after the
     * dial has dimmed out. */
    return (jr_face_t)__atomic_load_n(&s_dial_want, __ATOMIC_RELAXED);
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

/* The dial's own vocabulary: the condition the display holds (the WMO names of
 * jr_weather_condition) folded to one short whole word, so the cell never shows
 * a cut word or a stop. NULL for a condition with no word here (UNKNOWN): the
 * line then carries the temperature alone. Order matters: a thunderstorm is a
 * storm before it is rain, snow showers are snow before they are showers. */
static const char *watch_wx_word(const char *cond)
{
    static const struct { const char *key, *word; } T[] = {
        { "THUNDER", "STORM" }, { "STORM", "STORM" },
        { "SNOW", "SNOW" },     { "SLEET", "SNOW" },
        { "FOG", "FOG" },
        { "SHOWER", "RAIN" },   { "DRIZZLE", "RAIN" }, { "RAIN", "RAIN" },
        { "CLOUD", "CLOUDY" },  { "OVERCAST", "CLOUDY" },
        { "CLEAR", "CLEAR" },
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; ++i) {
        if (strstr(cond, T[i].key) != NULL) {
            return T[i].word;
        }
    }
    return NULL;
}

static int watch_wx_line(char *dst, size_t cap, int temp_f, const char *cond)
{
    const char *word = watch_wx_word(cond);
    int n = snprintf(dst, cap, word != NULL ? "%d* %s" : "%d*", temp_f, word != NULL ? word : "");
    if (n < 0 || (size_t)n >= cap || n > WATCH_WX_GLYPHS) {
        dst[0] = '\0';
        return 0;
    }
    return n;
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
    char jdate[8];               /* JARVIS, under the centre: "TUE 10"   */
    int  jdate_n;
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

/* A resting dial (see watch_rest_day) is drawn as a still. Latched at the start
 * of each frame, render task only, so every strip of a frame agrees. */
static bool s_watch_still;

/* The JARVIS dial's date: glass LABEL type in a subtle hairline capsule in the
 * 6-o'clock zone (y=314) so the hands no longer collide with it near the hub. */
#define JARVIS_DATE_Y     (233 + 81)
#define JARVIS_DATE_TRACK 3

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
    w->jdate_n = w->have_date ? snprintf(w->jdate, sizeof w->jdate, "%s %02d", WD[wday], mday) : 0;

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
        const uint16_t paper = sp_tint(ctx, 0xFFFF, st);
        const glass_metrics_t dm = glass_font_metrics(GLASS_FONT_TEXT);
        const int dw = glass_text_width(GLASS_FONT_TEXT, w->day, 2, 1);
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
                /* the glass digits, not the 5x7 bitmap: a photographic dial
                 * gets the same type as the rest of the glass. The window is
                 * 41 x 26 px; the TEXT digits are 15 px tall. */
                const int ty = (HUD_WATCH_DATE_Y0 + HUD_WATCH_DATE_Y1) / 2 - dm.cap_h / 2;
                if (y >= ty + dm.ink_top && y <= ty + dm.ink_bottom) {
                    glass_text_row(row, y, HUD_WATCH_DATE_X0, HUD_WATCH_DATE_X1 - 1,
                                   GLASS_FONT_TEXT, w->day, 2,
                                   (HUD_WATCH_DATE_X0 + HUD_WATCH_DATE_X1 - dw) / 2, ty, 1,
                                   0x0000u, 255, ctx->board.swap_color_bytes);
                }
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

/* TILT PARALLAX. The composition root registers a source (milli-g, screen
 * axes); the render task asks it ONCE A FRAME, and only while CELESTIAL is the
 * watch on a lit panel at the awake cadence. Anywhere else the shaper is reset
 * and the source is never called, so the sensor is not a display input on any
 * other screen. The shaped lean (tilt.h) goes to the dial in hud_watch_t and
 * to one packed word for GET /api/display. */
static jr_display_tilt_fn_t volatile s_tilt_src;
static jr_tilt_t s_tilt;                      /* render task only */
static uint32_t s_tilt_prev_ms;               /* render task only */
static volatile uint32_t s_tilt_word;         /* jr_tilt_pack(x, y); httpd reads */

void jr_display_set_tilt_source(jr_display_tilt_fn_t fn)
{
    __atomic_store_n(&s_tilt_src, fn, __ATOMIC_RELEASE);
}

static void tilt_tick(uint32_t now_ms)
{
    const jr_display_tilt_fn_t src = __atomic_load_n(&s_tilt_src, __ATOMIC_ACQUIRE);
    int x_mg = 0, y_mg = 0;
    const bool dial_tilt =
        s_style_shown == (uint8_t)JR_WATCH_CELESTIAL ||
        s_style_shown == (uint8_t)JR_WATCH_STYLE_ORRERY ||
        s_style_shown == (uint8_t)JR_WATCH_STYLE_REACTOR;
    const bool holo_tilt =
        (__atomic_load_n(&s_holo_on, __ATOMIC_ACQUIRE) != 0u) &&
        ((jr_display_space_t)(__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) & NAV_SPACE_MASK) ==
            JR_DISPLAY_SPACE_JARVIS);
    const bool live = src != NULL &&
        (dial_tilt || holo_tilt) &&
        (__atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE) & 3U) == 0U &&
        jr_display_render_fps() >= JR_DISPLAY_RENDER_FPS &&
        src(&x_mg, &y_mg);
    if (live) {
        jr_tilt_step(&s_tilt, x_mg, y_mg, (int)(now_ms - s_tilt_prev_ms));
    } else {
        jr_tilt_reset(&s_tilt);
    }
    s_tilt_prev_ms = now_ms;
    __atomic_store_n(&s_tilt_word, jr_tilt_pack(s_tilt.x, s_tilt.y), __ATOMIC_RELAXED);
}

/* True while the glass is changing what the watch is: a slide to or from WATCH,
 * or a style dip. Only then does the hands' strength differ from the settled
 * one, and only then is the dial dimmed by the shell instead of cleared. */
static bool watch_in_motion(void)
{
    /* (With the panel dip the watch is never drawn dimmed by the shell: each side
     * of the dark is the settled watch.) */
    return ((s_space_from == (uint8_t)JR_DISPLAY_SPACE_WATCH ||
             s_space_to == (uint8_t)JR_DISPLAY_SPACE_WATCH) && s_space_prog < 256 &&
            s_dip_mode != DIP_MODE_SPACE) ||
           (s_style_prog < 256 && s_dip_mode != DIP_MODE_STYLE);
}

static void apply_clock_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                                uint16_t *pixels)
{
    const int e = s_watch_s;
    if (e <= 0) {
        return;
    }
    int64_t sm = split_now();
    /* THE SHEET OUTRANKS THE FACE, so on WATCH it is the watch that goes
     * underneath: with the detail sheet or the control shade up, apply_hud_overlay
     * draws this BEFORE the shell, and the shell's own veil then dims the dial
     * and the hands together and the sheet lands on top. (This used to return
     * here instead, and the clear below, drawn after the shell, painted over the
     * sheet: 966 -> 944 lit pixels across the tap on WATCH, against 10741 -> 2312
     * on POWER, the sheet opened invisibly. Returning fixed that and made the
     * hands pop back at full strength the frame the shade finished closing.) */
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
    const int style = (int)s_style_shown;
    const jr_face_t dial = watch_dial_of_style(s_style_shown);
#ifdef ESP_PLATFORM
    const bool baked = (dial != JR_FACE_COUNT && ctx->shown_face == dial) ||
                       (style == (int)JR_WATCH_MINIMAL && s_dial_blit && s_dial_minimal);
#else
    const bool baked = dial != JR_FACE_COUNT && ctx->shown_face == dial;
#endif
    const bool moving = watch_in_motion();
    if (moving && dial != JR_FACE_COUNT && !baked) {
        return;      /* the dial is not bound yet: the shell holds the glass dark until it is */
    }
    if (!baked && !moving) {
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
    /* THE PEEK IS HANDS ONLY. The dial furniture (ticks, rings, the day's arc,
     * the date) belongs to the JARVIS watch style on the WATCH screen; any other
     * screen that shows the time for a few seconds shows the three hands and the
     * hub, as it always did. */
    const bool on_watch =
        (jr_display_space_t)(__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) & NAV_SPACE_MASK) ==
            JR_DISPLAY_SPACE_WATCH ||
        (s_space_from == (uint8_t)JR_DISPLAY_SPACE_WATCH && s_space_prog < 256);
    if (style == (int)JR_WATCH_JARVIS && !on_watch) {
        hud_overlay_clock(pixels, y1, y2 - y1, ctx->board.swap_color_bytes,
                          (int)((w >> 8) & 0xFFu), (int)(w & 0xFFu),
                          (int)((w >> 17) & 0x3Fu), strength);
        split_mark(JR_DISPLAY_SPLIT_CLOCK_FACE, &sm);
        return;
    }
    if (style == (int)JR_WATCH_JARVIS) {
        /* its own instrument: ticks, rings, the day's arc and the hands (one
         * call: the hands are part of the dial), then the date under the centre */
        const int hh = (int)((w >> 8) & 0xFFu), mm = (int)(w & 0xFFu);
        hud_jarvis_t j = { .hh = hh, .mm = mm, .ss = s_watch_still ? 0 : (int)((w >> 17) & 0x3Fu),
                           .seconds = !s_watch_still, .sun_now_q16 = -1 };
        const uint32_t sw = __atomic_load_n(&s_sun_word, __ATOMIC_ACQUIRE);
        if ((sw & (1u << 22)) != 0u) {
            /* a 24 h scale, noon at 12 o'clock, clockwise: q16 turns of the minute of the day */
            const int rise = (int)(sw & 0x7FFu), set = (int)((sw >> 11) & 0x7FFu);
            const int span = (set - rise + 1440) % 1440;
            j.sun = span > 0;
            j.sun_rise_q16 = ((((rise - 720) % 1440) + 1440) % 1440) * 65536 / 1440;
            j.sun_sweep_q16 = span * 65536 / 1440;
            if (hh < 24 && mm < 60) {
                const int since = (hh * 60 + mm - rise + 1440) % 1440;
                if (since <= span) {
                    j.sun_now_q16 = ((((hh * 60 + mm - 720) % 1440) + 1440) % 1440) * 65536 / 1440;
                }
            }
        }
        hud_overlay_jarvis_dial(pixels, y1, y2 - y1, ctx->board.swap_color_bytes, strength, &j);
        const watch_words_t *ww = &s_watch_words;
        if (ww->have_date && strength >= 128) {
            const glass_metrics_t ml = glass_font_metrics(GLASS_FONT_LABEL);
            const int tw = glass_text_width(GLASS_FONT_LABEL, ww->jdate, (size_t)ww->jdate_n, JARVIS_DATE_TRACK);
            const int tx = (HUD_W - tw) / 2 + JARVIS_DATE_TRACK / 2;
            const bool swap = ctx->board.swap_color_bytes;
            const int pad_x = 10;
            const int pad_y = 4;
            const int cap_y0 = JARVIS_DATE_Y + ml.ink_top - pad_y;
            const int cap_y1 = JARVIS_DATE_Y + ml.ink_bottom + pad_y;
            const int cap_h = cap_y1 - cap_y0;
            const int cap_r = cap_h / 2;
            const int cap_cy = (cap_y0 + cap_y1) / 2;
            const int cap_x0 = tx - pad_x;
            const int cap_x1 = tx + tw + pad_x - JARVIS_DATE_TRACK;
            const int cap_lx = cap_x0 + cap_r;
            const int cap_rx = cap_x1 - cap_r;
            const uint8_t border_a = (uint8_t)((78 * strength) / 255);
            const uint8_t fill_a = (uint8_t)((120 * strength) / 255);
            for (int y = y1; y < y2; ++y) {
                if (y < cap_y0 || y > cap_y1) {
                    continue;
                }
                uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
                const int dy = y - cap_cy;
                const int rem = cap_r * cap_r - dy * dy;
                const int half_end = rem > 0 ? sp_isqrt(rem) : 0;
                int xlo = cap_lx - half_end;
                int xhi = cap_rx + half_end;
                if (xlo < 0) { xlo = 0; }
                if (xhi >= HUD_W) { xhi = HUD_W - 1; }
                if (y == cap_y0 || y == cap_y1) {
                    for (int x = xlo; x <= xhi; ++x) {
                        row[x] = glass_blend565(row[x], 0xFD43u, border_a, swap);
                    }
                } else {
                    for (int x = xlo; x <= xhi; ++x) {
                        if (x == xlo || x == xhi) {
                            row[x] = glass_blend565(row[x], 0xFD43u, border_a, swap);
                        } else {
                            row[x] = glass_blend565(row[x], 0x0000u, fill_a, swap);
                        }
                    }
                }
                if (y >= JARVIS_DATE_Y + ml.ink_top && y <= JARVIS_DATE_Y + ml.ink_bottom) {
                    glass_text_row(row, y, 0, HUD_W - 1, GLASS_FONT_LABEL,
                                   ww->jdate, (size_t)ww->jdate_n, tx, JARVIS_DATE_Y, JARVIS_DATE_TRACK,
                                   0xFD43u, (uint8_t)((175 * strength) / 255), swap);
                }
            }
        }
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
        .ss = s_watch_still ? 0 : (int)((w >> 17) & 0x3Fu),
        .ms = s_watch_still ? 0 : s_clock_phase_ms,
        .still = s_watch_still,
        .batt_pct = (pw & 0xFFu) <= 100u ? (int)(pw & 0xFFu) : -1,
        .wx_valid = wx->valid,
        .temp_f = wx->temp_f,
        .dial_baked = baked,
        /* shadows at the awake cadence only: the ladder's slower frames are
         * a resting watch nobody is studying, and the shadow is the widest
         * blend a hand carries */
        .shadow = awake,
        .alive = awake && !s_watch_still,   /* SKELETON's wheel and CELESTIAL's stars rest with the shadows */
        .light = &s_light,
        .tilt_x = s_tilt.x,
        .tilt_y = s_tilt.y,
    };
    split_mark(JR_DISPLAY_SPLIT_CLOCK_CELLS, &sm);
    if (style == (int)JR_WATCH_STYLE_ORRERY) {
        const uint32_t frame_ms = s_fade_prev_ms != 0u ? s_fade_prev_ms
                                                       : (uint32_t)(esp_timer_get_time() / 1000);
        const uint32_t ymd = __atomic_load_n(&s_ymd_word, __ATOMIC_ACQUIRE);
        const uint16_t year = (uint16_t)(ymd >> 16);
        const uint8_t month = (uint8_t)((ymd >> 8) & 0xFFu);
        const uint8_t day   = (uint8_t)(ymd & 0xFFu);
        jr_showpiece_orrery_rows(pixels, y1, y2 - y1, ctx->board.swap_color_bytes,
                                 frame_ms, year, month, day,
                                 hw.hh, hw.mm, hw.ss,
                                 hw.tilt_x, hw.tilt_y, hw.light, hw.alive, strength);
        hud_overlay_watch(pixels, y1, y2 - y1, ctx->board.swap_color_bytes, &hw,
                          strength, style);
    } else if (style == (int)JR_WATCH_STYLE_REACTOR) {
        const uint32_t frame_ms = s_fade_prev_ms != 0u ? s_fade_prev_ms
                                                       : (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t spec[16];
        spectrum_load(spec);
        const uint8_t amp = (uint8_t)diag_load(&ctx->requested_amplitude);
        if (!baked) {
            hud_overlay_watch(pixels, y1, y2 - y1, ctx->board.swap_color_bytes, &hw,
                              strength, style);
        }
        jr_showpiece_reactor_rows(pixels, y1, y2 - y1, ctx->board.swap_color_bytes,
                                  frame_ms, hw.hh, hw.mm, hw.ss, hw.ms,
                                  hw.still, hw.alive, hw.tilt_x, hw.tilt_y,
                                  spec, amp, strength);
    } else {
        hud_overlay_watch(pixels, y1, y2 - y1, ctx->board.swap_color_bytes, &hw,
                          strength, style);
    }
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
#define SP_LEAVE_PX    60             /* how far the one leaving gets before it is gone */

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
#define SP_C_MINT      0x2F74   /* N9.11 OTA probation: cool emerald-mint, distinct from violet/cyan/gold */
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
         * (217k pixels a frame, every shell screen). A 32-bit aligned word-pair
         * zero-skip skips two black pixels in one load/compare while keeping
         * the exact per-pixel blend identical for every non-black pixel. */
        if (k >= 32) {
            int x = lo;
            if (x <= hi && (((uintptr_t)&row[x]) & 3U) != 0U) {
                if (row[x] != 0U) {
                    row[x] = hud_dim565(row[x], swap);
                }
                ++x;
            }
            if (swap) {
                for (; x + 1 <= hi; x += 2) {
                    uint32_t pair;
                    memcpy(&pair, &row[x], sizeof pair);
                    if (pair == 0U) {
                        continue;
                    }
                    if (row[x] != 0U) {
                        row[x] = hud_dim565(row[x], true);
                    }
                    if (row[x + 1] != 0U) {
                        row[x + 1] = hud_dim565(row[x + 1], true);
                    }
                }
                if (x <= hi && row[x] != 0U) {
                    row[x] = hud_dim565(row[x], true);
                }
            } else {
                for (; x + 1 <= hi; x += 2) {
                    uint32_t pair;
                    memcpy(&pair, &row[x], sizeof pair);
                    if (pair == 0U) {
                        continue;
                    }
                    if (row[x] != 0U) {
                        row[x] = hud_dim565(row[x], false);
                    }
                    if (row[x + 1] != 0U) {
                        row[x + 1] = hud_dim565(row[x + 1], false);
                    }
                }
                if (x <= hi && row[x] != 0U) {
                    row[x] = hud_dim565(row[x], false);
                }
            }
        } else {
            /* a flat dial under a part-strength veil folds the same pixel again
             * and again: remember the last one */
            uint16_t last_in = 0U, last_out = 0U;
            int x = lo;
            if (x <= hi && (((uintptr_t)&row[x]) & 3U) != 0U) {
                const uint16_t v = row[x];
                if (v != 0U) {
                    last_in = v;
                    last_out = hud_fade565(v, swap, k);
                    row[x] = last_out;
                }
                ++x;
            }
            for (; x + 1 <= hi; x += 2) {
                uint32_t pair;
                memcpy(&pair, &row[x], sizeof pair);
                if (pair == 0U) {
                    continue;
                }
                for (int j = 0; j < 2; ++j) {
                    const uint16_t v = row[x + j];
                    if (v != 0U) {
                        if (v != last_in) {
                            last_in = v;
                            last_out = hud_fade565(v, swap, k);
                        }
                        row[x + j] = last_out;
                    }
                }
            }
            if (x <= hi) {
                const uint16_t v = row[x];
                if (v != 0U) {
                    if (v != last_in) {
                        last_in = v;
                        last_out = hud_fade565(v, swap, k);
                    }
                    row[x] = last_out;
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
#define SG_RING_R_IN16  (108 * 16)
#define SG_RING_R_OUT16 (122 * 16)
#define SG_RING_DY      (-10)   /* the ring sits a little high: the stats need the room below */
#define SG_STAT_LABEL_Y 358     /* inside the orbit rail (r186), cleanly below r128..132 tick ring */
#define SG_STAT_VALUE_Y 372
#define SG_STAT_DX      54
#define SG_AGE_Y        372     /* WEATHER: peak-rain pill / age cleanly below outer rain ring */
#define SG_RISE_PX      8
#define SG_WX_ICON_SIZE GLASS_ICON_SM   /* the sky, above its word inside the ring */
#define SG_WX_ICON_GAP  10
#define SG_BOLT_INK_W   14   /* the small bolt's ink is this wide inside its 28 px box */
#define SG_BOLT_GAP     6
#define SG_FADE_MS      300
#define SG_GLOW16       48       /* 3.0 px luminous halo around focal rings */
#define SG_RAIN_SWEEP   3823    /* 21 degrees of a 30 degree slot */
#define SG_TICK_SWEEP   2184    /* a dry hour: 12 degree subtle dark-sapphire track segment */
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

/* An hour's mark from its chance of rain: a dry hour is a subtle 2.5 px dark-sapphire
 * track segment, a wet one a luminous 21 degree stroke whose width (4..8 px) and alpha
 * grow with the chance; `f` is the entry build (Q16) that grows it from nothing. */
static void sg_rain_mark(int pct, uint32_t f, int32_t *w16, int32_t *sweep, int *alpha)
{
    int w, a, sw;
    if (pct < SP_WX_HOUR_DRY) {
        w = 2 * 16 + 8;
        a = 120;
        sw = SG_TICK_SWEEP;
    } else {
        w = (4 * 16) + (4 * 16 * pct) / 100;
        a = 135 + (120 * pct) / 100;
        sw = SG_RAIN_SWEEP;
    }
    *w16 = (int32_t)(((int64_t)w * f) >> 16);
    *sweep = sw;
    *alpha = (int)glass_frac_alpha((uint8_t)a, f);
}

/* ---- Luminous OLED instrument helpers (rodata LUTs, zero float, strip-invariant) --- */

static const uint8_t k_sp_bayer[16] = {
    0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5
};

/* 24 evenly spaced unit directions (15 deg step, starting at 12 o'clock clockwise) in Q15. */
static const int16_t k_sp_dir24[24][2] = {
    {      0, -32767 }, {   8481, -31651 }, {  16384, -28378 }, {  23170, -23170 },
    {  28378, -16384 }, {  31651,  -8481 }, {  32767,      0 }, {  31651,   8481 },
    {  28378,  16384 }, {  23170,  23170 }, {  16384,  28378 }, {   8481,  31651 },
    {      0,  32767 }, {  -8481,  31651 }, { -16384,  28378 }, { -23170,  23170 },
    { -28378,  16384 }, { -31651,   8481 }, { -32767,      0 }, { -31651,  -8481 },
    { -28378, -16384 }, { -23170, -23170 }, { -16384, -28378 }, {  -8481, -31651 },
};

static inline uint16_t sp_add565_dither(uint16_t under, uint32_t add_r8, uint32_t add_g8,
                                        uint32_t add_b8, uint32_t d, bool swap)
{
    const uint32_t u = swap ? glass_bswap(under) : under;
    uint32_t r = (((u >> 11) << 3) + add_r8 + (d >> 1)) >> 3;
    uint32_t g = ((((u >> 5) & 63u) << 2) + add_g8 + (d >> 2)) >> 2;
    uint32_t b = (((u & 31u) << 3) + add_b8 + (d >> 1)) >> 3;
    if (r > 31u) r = 31u;
    if (g > 63u) g = 63u;
    if (b > 31u) b = 31u;
    const uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return swap ? glass_bswap(out) : out;
}

/* Linear RGB565 mix: t in 0..256 (0 -> c0, 256 -> c1). */
static uint16_t sp_lerp565(uint16_t c0, uint16_t c1, int t)
{
    if (t <= 0) return c0;
    if (t >= 256) return c1;
    const int r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
    const int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    const int r = r0 + (((r1 - r0) * t + 128) >> 8);
    const int g = g0 + (((g1 - g0) * t + 128) >> 8);
    const int b = b0 + (((b1 - b0) * t + 128) >> 8);
    return (uint16_t)(((r & 31) << 11) | ((g & 63) << 5) | (b & 31));
}

/* Volumetric two-zone radial OLED bloom: pure integer, zero per-pixel sqrt,
 * 4x4 Bayer dithered so pure black -> vivid glow has zero RGB565 banding.
 * mode 0: bright core fading quartically to 0 at r_max.
 * mode 1: crystal backlight peaking smoothly around ~0.62*r_max (55% in center
 *         so hero numerals stay crisp) and fading smoothly to 0 at r_max. */
typedef struct {
    int cx, cy, r_max, r2_max;
    uint32_t inv_r2;
    uint32_t lut[65];
    bool active;
} sp_bloom_t;

static void sp_bloom_init(sp_bloom_t *b, int cx, int cy, int r_max,
                          uint16_t col_inner, uint16_t col_outer,
                          uint8_t peak, int alpha, int mode)
{
    b->active = false;
    if (r_max <= 0 || peak == 0u || alpha <= 0) {
        return;
    }
    b->cx = cx;
    b->cy = cy;
    b->r_max = r_max;
    b->r2_max = r_max * r_max;
    b->inv_r2 = (64u << 16) / (uint32_t)b->r2_max;
    const uint32_t a_eff = ((uint32_t)peak * (uint32_t)(alpha > 255 ? 255 : alpha)) >> 8;
    if (a_eff == 0u) {
        return;
    }
    const uint32_t r0 = (((col_inner >> 11) & 31u) << 3) | (((col_inner >> 11) & 31u) >> 2);
    const uint32_t g0 = (((col_inner >> 5) & 63u) << 2) | (((col_inner >> 5) & 63u) >> 4);
    const uint32_t b0 = ((col_inner & 31u) << 3) | ((col_inner & 31u) >> 2);
    const uint32_t r1 = (((col_outer >> 11) & 31u) << 3) | (((col_outer >> 11) & 31u) >> 2);
    const uint32_t g1 = (((col_outer >> 5) & 63u) << 2) | (((col_outer >> 5) & 63u) >> 4);
    const uint32_t b1 = ((col_outer & 31u) << 3) | ((col_outer & 31u) >> 2);

    for (unsigned i = 0; i <= 64u; ++i) {
        const uint32_t inv = 64u - i; /* 64 at center -> 0 at edge */
        uint32_t w;
        if (mode == 1) {
            /* Smooth radial crystal profile: 58% at center, 100% around r^2/R^2=0.38, 0 at edge. */
            const uint32_t fall = (inv * inv) >> 4; /* 0..256 */
            const uint32_t bowl = i <= 24u ? (148u + (108u * i) / 24u) : 256u;
            w = (fall * bowl) >> 8;
        } else {
            w = (inv * inv) >> 4; /* 0..256 quartic-like (1 - r^2/R^2)^2 */
        }
        const uint32_t sc = (a_eff * w) >> 8;
        if (sc == 0u) {
            b->lut[i] = 0u;
            continue;
        }
        const uint32_t cr = (r0 * inv + r1 * i) >> 6;
        const uint32_t cg = (g0 * inv + g1 * i) >> 6;
        const uint32_t cb = (b0 * inv + b1 * i) >> 6;
        const uint32_t ar = (cr * sc + 127u) / 255u;
        const uint32_t ag = (cg * sc + 127u) / 255u;
        const uint32_t ab = (cb * sc + 127u) / 255u;
        b->lut[i] = (ar << 20) | (ag << 10) | ab;
    }
    b->active = true;
}

static void sp_bloom_row(uint16_t *row, int y, int lo, int hi,
                         const sp_bloom_t *b, bool swap)
{
    if (!b->active) {
        return;
    }
    const int dy = y - b->cy;
    const int dy2 = dy * dy;
    if (dy2 >= b->r2_max) {
        return;
    }
    const int half = sp_isqrt(b->r2_max - dy2);
    int xa = b->cx - half;
    int xb = b->cx + half;
    if (xa < lo) xa = lo;
    if (xb > hi) xb = hi;
    if (xa > xb) {
        return;
    }
    const uint8_t *by = &k_sp_bayer[((unsigned)y & 3u) * 4u];
    int dx = xa - b->cx;
    for (int x = xa; x <= xb; ++x, ++dx) {
        const uint32_t r2 = (uint32_t)(dx * dx + dy2);
        if (r2 >= (uint32_t)b->r2_max) {
            continue;
        }
        const uint32_t idx = (r2 * b->inv_r2) >> 16;
        const uint32_t pk = b->lut[idx < 64u ? idx : 64u];
        if (pk == 0u) {
            continue;
        }
        row[x] = sp_add565_dither(row[x], (pk >> 20) & 1023u, (pk >> 10) & 1023u,
                                  pk & 1023u, by[(unsigned)x & 3u], swap);
    }
}

/* Luminous multi-layer glowing orb: soft Bayer-dithered additive radial halo +
 * crisp colored body + bright specular core. Fast and 12-row strip invariant. */
static void sp_orb_row(uint16_t *row, int y, int lo, int hi,
                       int32_t cx16, int32_t cy16, int32_t r_core16, int32_t r_halo16,
                       uint16_t col, uint16_t core_col, uint8_t alpha, bool swap)
{
    if (alpha == 0u || r_halo16 <= 0) {
        return;
    }
    const int32_t y16 = y * 16 + 8;
    const int32_t dy16 = y16 - cy16;
    const int32_t ady = dy16 < 0 ? -dy16 : dy16;
    if (ady >= r_halo16 + 16) {
        return;
    }
    if (r_halo16 > r_core16) {
        const int32_t r2h = r_halo16 * r_halo16;
        if (dy16 * dy16 < r2h) {
            const int32_t half16 = sp_isqrt(r2h - dy16 * dy16);
            int xa = (int)((cx16 - half16 - 8 + 15) >> 4);
            int xb = (int)((cx16 + half16 - 8) >> 4);
            if (xa < lo) xa = lo;
            if (xb > hi) xb = hi;
            const uint32_t cr = (((col >> 11) & 31u) << 3) | (((col >> 11) & 31u) >> 2);
            const uint32_t cg = (((col >> 5) & 63u) << 2) | (((col >> 5) & 63u) >> 4);
            const uint32_t cb = ((col & 31u) << 3) | ((col & 31u) >> 2);
            const uint8_t *by = &k_sp_bayer[((unsigned)y & 3u) * 4u];
            const uint32_t peak_a = ((uint32_t)alpha * 170u) >> 8;
            for (int x = xa; x <= xb; ++x) {
                const int32_t dx16 = x * 16 + 8 - cx16;
                const int32_t d2 = dx16 * dx16 + dy16 * dy16;
                if (d2 >= r2h) continue;
                const uint32_t u = (uint32_t)((r2h - d2) * 256 / r2h);
                const uint32_t w = (u * u) >> 8; /* 0..256 */
                const uint32_t sc = (peak_a * w) >> 8;
                if (sc == 0u) continue;
                row[x] = sp_add565_dither(row[x], (cr * sc) >> 8, (cg * sc) >> 8,
                                          (cb * sc) >> 8, by[(unsigned)x & 3u], swap);
            }
        }
    }
    if (r_core16 > 0 && ady < r_core16 + 16) {
        glass_disc_row(row, y, lo, hi, cx16, cy16, r_core16, col, alpha, swap);
        const int32_t r_spec = (r_core16 * 5) / 9;
        if (r_spec >= 12) {
            glass_disc_row(row, y, lo, hi, cx16, cy16, r_spec, core_col, alpha, swap);
        }
    }
}

/* Crisp anti-aliased radial tick mark along unit direction (ux, uy) Q15 from
 * radius r0_16 to r1_16 (in 1/16 px) with half-width hw16 (in 1/16 px). */
static void sp_radial_tick_row(uint16_t *row, int y, int lo, int hi,
                               int32_t cx16, int32_t cy16, int32_t ux, int32_t uy,
                               int32_t r0_16, int32_t r1_16, int32_t hw16,
                               uint16_t col, uint8_t alpha, bool swap)
{
    if (alpha == 0u) {
        return;
    }
    const int32_t y0 = cy16 + ((uy * r0_16) >> 15);
    const int32_t y1 = cy16 + ((uy * r1_16) >> 15);
    const int32_t ymin = (y0 < y1 ? y0 : y1) - hw16 - 16;
    const int32_t ymax = (y0 > y1 ? y0 : y1) + hw16 + 16;
    const int32_t y16 = y * 16 + 8;
    if (y16 < ymin || y16 > ymax) {
        return;
    }
    const int32_t x0 = cx16 + ((ux * r0_16) >> 15);
    const int32_t x1 = cx16 + ((ux * r1_16) >> 15);
    int xa = (int)(((x0 < x1 ? x0 : x1) - hw16 - 24) >> 4);
    int xb = (int)(((x0 > x1 ? x0 : x1) + hw16 + 24) >> 4);
    if (xa < lo) xa = lo;
    if (xb > hi) xb = hi;
    const int32_t dy16 = y16 - cy16;
    for (int x = xa; x <= xb; ++x) {
        const int32_t dx16 = x * 16 + 8 - cx16;
        const int32_t proj = (dx16 * ux + dy16 * uy) >> 15;
        if (proj < r0_16 - 16 || proj > r1_16 + 16) continue;
        int32_t perp = (dx16 * uy - dy16 * ux) >> 15;
        if (perp < 0) perp = -perp;
        if (perp >= hw16 + 16) continue;
        int cr = 255;
        if (proj < r0_16) {
            cr = 128 + (proj - r0_16) * 8;
        } else if (proj > r1_16) {
            cr = 128 + (r1_16 - proj) * 8;
        }
        int cp = 128 + (hw16 - perp) * 8;
        if (cr <= 0 || cp <= 0) continue;
        if (cr > 255) cr = 255;
        if (cp > 255) cp = 255;
        const unsigned cov = (unsigned)((cr * cp) >> 8);
        const unsigned a = (cov * ((unsigned)alpha + 1u)) >> 8;
        if (a > 0u) {
            row[x] = glass_blend565(row[x], col, a, swap);
        }
    }
}

/* Sleek translucent rounded pill backdrop with continuous 1-px luminous rim. */
static void sp_pill_row(uint16_t *row, int y, int lo, int hi,
                        int cx, int cy, int hw, int hh,
                        uint16_t fill_col, uint8_t fill_a,
                        uint16_t rim_col, uint8_t rim_a, bool swap)
{
    if ((fill_a == 0u && rim_a == 0u) || hh <= 1 || hw <= hh) {
        return;
    }
    const int dy = y - cy;
    const int ady = dy < 0 ? -dy : dy;
    if (ady > hh) {
        return;
    }
    const int cap = sp_isqrt(hh * hh - dy * dy);
    const int inner_cap = ady < hh ? sp_isqrt((hh - 1) * (hh - 1) - dy * dy) : -1;
    const int span = (hw - hh) + cap;
    const int inner_span = inner_cap >= 0 ? (hw - hh) + inner_cap : -1;
    int xa = cx - span;
    int xb = cx + span;
    const int ixa = cx - inner_span;
    const int ixb = cx + inner_span;
    if (xa < lo) xa = lo;
    if (xb > hi) xb = hi;
    if (xa > xb) {
        return;
    }
    const bool is_edge_row = ady >= hh;
    for (int x = xa; x <= xb; ++x) {
        const bool is_rim = is_edge_row || inner_span < 0 || x < ixa || x > ixb;
        if (is_rim && rim_a > 0u) {
            row[x] = glass_blend565(row[x], rim_col, rim_a, swap);
        } else if (fill_a > 0u) {
            row[x] = glass_blend565(row[x], fill_col, fill_a, swap);
        }
    }
}

/* 4-bar ascending Wi-Fi signal icon: 14 px wide, heights 4/7/10/13 px aligned
 * to y_bot, lit bars glowing in `lit_col`, unlit bars in dim slate. */
static void sp_wifi_bars_row(uint16_t *row, int y, int lo, int hi,
                             int x0, int y_bot, int bars,
                             uint16_t lit_col, uint8_t alpha, bool swap)
{
    static const uint8_t k_h[4] = { 4, 7, 10, 13 };
    if (alpha == 0u || y < y_bot - 14 || y > y_bot + 1) {
        return;
    }
    const uint16_t dim_col = 0x2965u; /* subtle cool slate */
    const uint8_t dim_a = (uint8_t)(((unsigned)alpha * 145u) >> 8);
    const uint8_t glow_a = (uint8_t)(((unsigned)alpha * 55u) >> 8);
    for (int i = 0; i < 4; ++i) {
        const int bx = x0 + i * 4;
        const int y_top = y_bot - (int)k_h[i] + 1;
        const bool lit = i < bars;
        if (y >= y_top && y <= y_bot) {
            const uint16_t c = lit ? lit_col : dim_col;
            const uint8_t a = lit ? alpha : dim_a;
            for (int dx = 0; dx < 2; ++dx) {
                const int px = bx + dx;
                if (px >= lo && px <= hi) {
                    row[px] = glass_blend565(row[px], c, a, swap);
                }
            }
            if (lit && glow_a > 0u) {
                if (bx - 1 >= lo && bx - 1 <= hi) {
                    row[bx - 1] = glass_blend565(row[bx - 1], lit_col, glow_a, swap);
                }
                if (bx + 2 >= lo && bx + 2 <= hi) {
                    row[bx + 2] = glass_blend565(row[bx + 2], lit_col, glow_a, swap);
                }
            }
        } else if (lit && glow_a > 0u && y == y_top - 1) {
            for (int dx = 0; dx < 2; ++dx) {
                const int px = bx + dx;
                if (px >= lo && px <= hi) {
                    row[px] = glass_blend565(row[px], lit_col, glow_a, swap);
                }
            }
        }
    }
}

/* Vivid OLED thermal color map for WEATHER's temperature instrument ring.
 * Never returns SP_C_GOLD (0xFEA0) or SP_C_AMBER (0xFD20). */
static uint16_t sp_thermal_color(int temp_f)
{
    static const int16_t k_t[6] = { 32, 50, 65, 75, 85, 95 };
    static const uint16_t k_c[6] = {
        0x24FFu, /* icy electric blue   */
        0x06DFu, /* vivid sky cyan      */
        0x1F56u, /* luminous mint-teal  */
        0xFE04u, /* warm solar amber    */
        0xFB60u, /* vivid solar orange  */
        0xF924u, /* fiery coral-red     */
    };
    if (temp_f <= k_t[0]) return k_c[0];
    if (temp_f >= k_t[5]) return k_c[5];
    for (int i = 0; i < 5; ++i) {
        if (temp_f <= k_t[i + 1]) {
            const int span = k_t[i + 1] - k_t[i];
            const int frac = ((temp_f - k_t[i]) * 256) / span;
            return sp_lerp565(k_c[i], k_c[i + 1], frac);
        }
    }
    return k_c[5];
}

/* Chromatic event orb color for ACTIVITY timeline feed. */
static uint16_t sp_act_kind_color(const char *kind, bool muted)
{
    if (muted) {
        return SP_C_GOLD;
    }
    if (kind == NULL || kind[0] == '\0') {
        return SP_C_INK;
    }
    if (strncmp(kind, "TIM", 3) == 0 || strncmp(kind, "ALA", 3) == 0) {
        return SP_C_AMBER;   /* TIMER / TIME / ALARM -> warm amber */
    }
    if (strncmp(kind, "RAI", 3) == 0 || strncmp(kind, "WEA", 3) == 0) {
        return 0x3E7Fu;      /* RAIN / WEATHER -> electric sky-blue */
    }
    if (strncmp(kind, "TAS", 3) == 0 || strncmp(kind, "DES", 3) == 0 ||
        strncmp(kind, "COD", 3) == 0 || strncmp(kind, "AGE", 3) == 0) {
        return SP_C_VIOLET;  /* TASK / DESK / CODEX -> vibrant violet */
    }
    if (strncmp(kind, "WEB", 3) == 0 || strncmp(kind, "PRI", 3) == 0 ||
        strncmp(kind, "SEA", 3) == 0) {
        return SP_C_CYAN;    /* WEB / PRICE / SEARCH -> neon cyan */
    }
    return 0xDFDFu;          /* SAID / ASK / default -> ice-white */
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
/* Settled 12-hour rain ring segment records in PSRAM (12 x 2048 B = 24 KB).
 * Allocated once in jr_display_start() alongside orbit/fring/shade records and
 * rebuilt only when the 12-hour rain percentages change. Each segment replays
 * bit-identically to glass_arc_row(); any segment that does not fit falls back
 * to glass_arc_row(). */
#define SG_WX_REC_CAP   2048u
#define SG_WX_TH_SEGS   8
#define SG_WX_TH_IN16   (114 * 16)
#define SG_WX_TH_OUT16  (126 * 16)
#define SG_WX_RAIN_RC16 (138 * 16)
#define SG_WX_DIAL_START 24576  /* 7:30 o'clock (135 deg CW from 3 o'clock) */
#define SG_WX_DIAL_SWEEP 49152  /* 270 deg CW -> 4:30 o'clock, 90 deg cradle at 6 */

static uint8_t *s_wx_rec;
static uint8_t  s_wx_rec_pct[SP_WX_HOURS];
static uint16_t s_wx_rec_have;
static bool     s_wx_rec_valid;

static void wx_rec_alloc(void)
{
    if (s_wx_rec == NULL) {
        s_wx_rec = heap_caps_malloc((size_t)SP_WX_HOURS * SG_WX_REC_CAP,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_wx_rec_have = 0u;
    s_wx_rec_valid = false;
}

typedef struct {
    bool     valid;
    uint32_t rev;
    int      oy, st;
    uint32_t ent;
    bool     wx_valid;
    bool     wx_stale;
    bool     muted;
    jr_display_sky_t sky;
    int      wx_hours;
    int16_t  temp_f, hi_f, lo_f;
    char     temp[sizeof s_wx_temp];
    char     hilo_src[sizeof s_wx_hilo];
    uint8_t  hour_pct[SP_WX_HOURS];
    int      nseg;
    uint8_t  sg_k[SP_WX_HOURS];
    uint8_t  sg_pct[SP_WX_HOURS];
    int32_t  sg_start[SP_WX_HOURS], sg_sweep[SP_WX_HOURS];
    int32_t  sg_ri[SP_WX_HOURS], sg_ro[SP_WX_HOURS];
    uint8_t  sg_a[SP_WX_HOURS];
    int      sg_y0[SP_WX_HOURS], sg_y1[SP_WX_HOURS];
    /* Thermal gradient arc segments (lo_f -> hi_f) */
    int      th_n;
    int32_t  th_arc_start, th_arc_sweep;
    int32_t  th_start[SG_WX_TH_SEGS], th_sweep[SG_WX_TH_SEGS];
    uint16_t th_col[SG_WX_TH_SEGS];
    int      th_y0[SG_WX_TH_SEGS], th_y1[SG_WX_TH_SEGS];
    int32_t  pip_x16, pip_y16;
    uint16_t pip_col;
    bool     have_pip;
    int      out_w;
    char     hilo[16];
    int      hlen;
    int      tlen, hw, dw;
} sp_wx_frame_cache_t;
static sp_wx_frame_cache_t s_wx_fc;

static void sp_focal_weather(const jr_display_ctx_t *ctx, int y1, int y2,
                             uint16_t *pixels, int oy, int st, uint32_t ent)
{
    const bool swap = ctx->board.swap_color_bytes;
    const bool muted = sp_privacy_muted();

    /* Glass 3.0 ATMOSPHAERA: 360-deg thermal/solar perimeter arc, live Sun
     * Jewel, curved planetary horizon (y>=266), and optical weather emblem
     * (x=248..344, y=148..232) with strictly non-overlapping Barlow text. */
    if (glass3_active()) {
        const uint32_t cw = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
        const bool clk_ok = (cw & (1u << 16)) != 0u;
        const uint8_t hh = clk_ok ? (uint8_t)(((cw >> 8) & 0xFFu) % 24u) : 14u;
        const uint8_t mm = clk_ok ? (uint8_t)((cw & 0xFFu) % 60u) : 0u;
        const uint8_t rain0 = (s_wx_valid && s_wx_hours > 0 && s_wx_hour_pct[0] != SP_WX_HOUR_NONE)
                              ? (uint8_t)s_wx_hour_pct[0] : 0u;
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const jr_g3_atmosphaera_t atmo = {
            .hour = hh,
            .minute = mm,
            .weather_code = s_wx_valid ? (uint8_t)s_wx_sky : (uint8_t)JR_G3_WX_PARTLY,
            .is_day = (hh >= 6u && hh < 19u),
            .temp_f = s_wx_valid ? (int16_t)s_wx_temp_f : 72,
            .rain_pct = rain0,
            .now_ms = now_ms,
            .strength = (uint8_t)(st > 255 ? 255 : (st < 0 ? 0 : st)),
        };
        jr_g3_atmosphaera_rows(pixels, y1, y2 - y1, swap, &atmo);

        const uint16_t accent = muted ? SP_C_GOLD : (s_wx_stale ? SP_C_GREY : sp_sky_native(s_wx_sky));
        const char *head = sp_headline(JR_DISPLAY_SPACE_WEATHER);
        const int llen = sp_len(head, SP_LABEL_CAP - 1);
        const int tlen = sp_len(s_wx_temp, (int)sizeof s_wx_temp - 1);
        const int hlen = sp_len(s_wx_hilo, (int)sizeof s_wx_hilo - 1);
        const int olen = sp_len(s_wx_outlook, (int)sizeof s_wx_outlook - 1);
        const int slen = sp_len(s_wx_sun, (int)sizeof s_wx_sun - 1);
        const int hw = glass_text_width(GLASS_FONT_HERO, s_wx_temp, (size_t)tlen, 0);
        const int dw = glass_text_width(GLASS_FONT_VALUE, "*", 1, 0);
        /* Place giant temp cleanly inside [JR_G3_ATMOS_TEMP_X0..X1] (96..242),
         * clear of the sculpted weather emblem at x=248..344. */
        int tx0 = (JR_G3_ATMOS_TEMP_X0 + JR_G3_ATMOS_TEMP_X1 - (hw + 4 + dw)) / 2;
        if (tx0 < JR_G3_ATMOS_TEMP_X0) tx0 = JR_G3_ATMOS_TEMP_X0;
        const int ty0 = 166 + oy;

        sg_txt_t t_label, t_temp, t_deg, t_hilo, t_sub;
        sg_txt_make(&t_label, GLASS_FONT_LABEL, head, llen, SP_CX, 114 + oy, 3,
                    s_wx_valid ? accent : SP_C_GREY, st);
        sg_txt_at(&t_temp, GLASS_FONT_HERO, s_wx_temp, tlen, tx0, ty0, SP_C_INK, st);
        sg_txt_at(&t_deg, GLASS_FONT_VALUE, "*", tlen > 0 ? 1 : 0, tx0 + hw + 4, ty0, accent, st);
        sg_txt_make(&t_hilo, GLASS_FONT_TEXT, s_wx_hilo, hlen, SP_CX,
                    JR_G3_ATMOS_STATS_Y0 + 4 + oy, 1, SP_C_INK, st);
        const char *sub = olen > 0 ? s_wx_outlook : s_wx_sun;
        const int sublen = olen > 0 ? olen : slen;
        sg_txt_make(&t_sub, GLASS_FONT_LABEL, sub, sublen, SP_CX,
                    JR_G3_ATMOS_STATS_Y0 + 28 + oy, 2,
                    olen > 0 ? (muted ? SP_C_GOLD : 0x6EF7u) : SP_C_GREY, st);

        for (int y = y1; y < y2; ++y) {
            int lo = 0, hi = HUD_W - 1;
            if (!sp_clip(y, &lo, &hi)) continue;
            uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
            sg_txt_row(row, y, lo, hi, swap, &t_label);
            sg_txt_row(row, y, lo, hi, swap, &t_temp);
            sg_txt_row(row, y, lo, hi, swap, &t_deg);
            sg_txt_row(row, y, lo, hi, swap, &t_hilo);
            sg_txt_row(row, y, lo, hi, swap, &t_sub);
        }
        return;
    }
    const uint16_t accent =
        muted ? SP_C_GOLD : (s_wx_stale ? SP_C_GREY : sp_sky_native(s_wx_sky));
    const uint16_t rain =
        muted ? SP_C_GOLD : (s_wx_sky == JR_DISPLAY_SKY_STORM ? 0x54FFu : SP_C_RAIN);
    const uint16_t dry_track = muted ? SP_C_GOLD_DIM : 0x0A31u; /* subtle dark sapphire */
    const int cx16 = SP_CX * 16;
    const int cyr = SP_CY + oy + SG_RING_DY;
    const int cy16 = cyr * 16;

    /* Hoist the 12 rain segment calculations, thermal gradient arc segments,
     * glass_arc_rows(), hilo opening, and text widths out of the 39-strip loop. */
    sp_wx_frame_cache_t *fc = &s_wx_fc;
    const bool need_fc =
        !fc->valid || fc->rev != s_wx_compose_rev || fc->oy != oy || fc->st != st ||
        fc->ent != ent || fc->wx_valid != s_wx_valid || fc->wx_stale != s_wx_stale ||
        fc->muted != muted || fc->sky != s_wx_sky || fc->wx_hours != s_wx_hours ||
        fc->temp_f != s_wx_temp_f || fc->hi_f != s_wx_hi_f || fc->lo_f != s_wx_lo_f ||
        memcmp(fc->temp, s_wx_temp, sizeof s_wx_temp) != 0 ||
        memcmp(fc->hilo_src, s_wx_hilo, sizeof s_wx_hilo) != 0 ||
        (s_wx_valid && s_wx_hours > 0 &&
         memcmp(fc->hour_pct, s_wx_hour_pct, SP_WX_HOURS) != 0);
    if (need_fc) {
        fc->rev = s_wx_compose_rev;
        fc->oy = oy;
        fc->st = st;
        fc->ent = ent;
        fc->wx_valid = s_wx_valid;
        fc->wx_stale = s_wx_stale;
        fc->muted = muted;
        fc->sky = s_wx_sky;
        fc->wx_hours = s_wx_hours;
        fc->temp_f = s_wx_temp_f;
        fc->hi_f = s_wx_hi_f;
        fc->lo_f = s_wx_lo_f;
        memcpy(fc->temp, s_wx_temp, sizeof s_wx_temp);
        memcpy(fc->hilo_src, s_wx_hilo, sizeof s_wx_hilo);
        memcpy(fc->hour_pct, s_wx_hour_pct, SP_WX_HOURS);

        /* Outer 12-hour rain ring at r134..142 (centre r138) */
        int nseg = 0;
        for (int k = 0; k < SP_WX_HOURS && s_wx_valid && !s_wx_stale && s_wx_hours > 0; ++k) {
            const int pct = s_wx_hour_pct[k];
            if (pct == SP_WX_HOUR_NONE) {
                continue;
            }
            const uint32_t f = glass_stagger_q16(ent, (uint32_t)k * SG_RAIN_STAGGER_MS, SG_RAIN_BUILD_MS);
            int32_t w16, sweep;
            int a;
            sg_rain_mark(pct, f, &w16, &sweep, &a);
            if (w16 < 8 || a <= 0) {
                continue;
            }
            fc->sg_k[nseg] = (uint8_t)k;
            fc->sg_pct[nseg] = (uint8_t)pct;
            fc->sg_ri[nseg] = SG_WX_RAIN_RC16 - w16 / 2;
            fc->sg_ro[nseg] = SG_WX_RAIN_RC16 + (w16 + 1) / 2;
            fc->sg_start[nseg] = (sg_slot_centre_q16(k) - sweep / 2) & 0xFFFF;
            fc->sg_sweep[nseg] = sweep;
            fc->sg_a[nseg] = (uint8_t)((a * st + 127) / 255);
            if (!glass_arc_rows(cx16, cy16, fc->sg_ri[nseg], fc->sg_ro[nseg],
                                fc->sg_start[nseg], fc->sg_sweep[nseg], true,
                                &fc->sg_y0[nseg], &fc->sg_y1[nseg])) {
                fc->sg_y0[nseg] = 1;
                fc->sg_y1[nseg] = 0;
            }
            ++nseg;
        }
        fc->nseg = nseg;

        /* Main focal Precision Temperature Instrument Ring (r114..126):
         * thermal gradient arc spanning today's lo_f -> hi_f with glowing temp pip. */
        fc->th_n = 0;
        fc->have_pip = false;
        fc->th_arc_start = 0;
        fc->th_arc_sweep = 0;
        if (s_wx_valid && !s_wx_stale) {
            const uint32_t f_th = glass_ease_out_cubic_q16(ent, SP_ENTRY_RING_MS);
            const int lo_t = s_wx_lo_f <= s_wx_hi_f ? s_wx_lo_f : s_wx_hi_f;
            const int hi_t = s_wx_hi_f >= s_wx_lo_f ? s_wx_hi_f : s_wx_lo_f;
            const int mid_t = (lo_t + hi_t) / 2;
            int shift = (mid_t - 60) * 140;
            if (shift < -6000) shift = -6000;
            if (shift > 6000) shift = 6000;
            const int32_t full_span = 32768; /* 180 deg rich thermal arc inside 270 deg dial */
            const int32_t base_start = (SG_WX_DIAL_START + (SG_WX_DIAL_SWEEP - full_span) / 2 + shift) & 0xFFFF;
            const int32_t sweep_now = (int32_t)(((int64_t)full_span * f_th) >> 16);
            fc->th_arc_start = base_start;
            fc->th_arc_sweep = sweep_now;
            if (sweep_now >= 256) {
                const int segs = SG_WX_TH_SEGS;
                const int32_t step = sweep_now / segs;
                for (int i = 0; i < segs; ++i) {
                    const int32_t s0 = (base_start + i * step) & 0xFFFF;
                    int32_t sw = (i == segs - 1) ? (sweep_now - i * step) : (step + 180);
                    if (sw < 64) sw = 64;
                    const int seg_t = lo_t + ((hi_t - lo_t) * (2 * i + 1)) / (2 * segs);
                    fc->th_start[i] = s0;
                    fc->th_sweep[i] = sw;
                    fc->th_col[i] = muted ? SP_C_GOLD : sp_thermal_color(seg_t);
                    if (!glass_arc_rows(cx16, cy16, SG_WX_TH_IN16 + 16, SG_WX_TH_OUT16 - 16,
                                        s0, sw, true, &fc->th_y0[i], &fc->th_y1[i])) {
                        fc->th_y0[i] = 1;
                        fc->th_y1[i] = 0;
                    }
                }
                fc->th_n = segs;
            }
            if (f_th >= GLASS_ONE_Q16 / 2u) {
                int frac = 500;
                if (hi_t > lo_t) {
                    frac = ((s_wx_temp_f - lo_t) * 1000) / (hi_t - lo_t);
                    if (frac < 0) frac = 0;
                    if (frac > 1000) frac = 1000;
                }
                const uint32_t a_pip = (uint32_t)(base_start + ((int64_t)sweep_now * frac) / 1000) & 0xFFFFu;
                const int32_t r_mid16 = (SG_WX_TH_IN16 + SG_WX_TH_OUT16) / 2;
                fc->pip_x16 = cx16 + ((glass_cos_q15(a_pip) * r_mid16) >> 15);
                fc->pip_y16 = cy16 + ((glass_sin_q15(a_pip) * r_mid16) >> 15);
                fc->pip_col = muted ? SP_C_GOLD : sp_thermal_color(s_wx_temp_f);
                fc->have_pip = true;
            }
        }

        fc->hlen = sg_hilo_open(fc->hilo, (int)sizeof fc->hilo, s_wx_hilo);
        fc->tlen = sp_len(s_wx_temp, (int)sizeof s_wx_temp - 1);
        fc->hw = glass_text_width(GLASS_FONT_HERO, s_wx_temp, (size_t)fc->tlen, 0);
        if (fc->dw == 0) {
            fc->dw = glass_text_width(GLASS_FONT_VALUE, "*", 1, 0);
        }
        const int olen_m = sp_len(s_wx_outlook, (int)sizeof s_wx_outlook - 1);
        fc->out_w = olen_m > 0 ? glass_text_width(GLASS_FONT_LABEL, s_wx_outlook, (size_t)olen_m, 2) : 0;
        fc->valid = true;
    }

    const int nseg = fc->nseg;
    const bool bare = !(s_wx_valid && !s_wx_stale && s_wx_hours > 0);
    bool use_wx_rec = false;
    if (!bare && oy == 0 && ent >= SP_ENTRY_TOTAL_MS && nseg > 0 && s_wx_rec != NULL) {
        if (!s_wx_rec_valid || memcmp(s_wx_rec_pct, s_wx_hour_pct, SP_WX_HOURS) != 0) {
            memcpy(s_wx_rec_pct, s_wx_hour_pct, SP_WX_HOURS);
            s_wx_rec_have = 0u;
            for (int i = 0; i < nseg; ++i) {
                const uint8_t k = fc->sg_k[i];
                if (glass_shape_record(s_wx_rec + (size_t)k * SG_WX_REC_CAP, SG_WX_REC_CAP,
                                       cx16, cy16, fc->sg_ri[i], fc->sg_ro[i],
                                       fc->sg_start[i], fc->sg_sweep[i], true) != 0u) {
                    s_wx_rec_have |= (uint16_t)(1u << k);
                }
            }
            s_wx_rec_valid = true;
        }
        use_wx_rec = s_wx_rec_have != 0u;
    }

    /* text */
    const char *head = sp_headline(JR_DISPLAY_SPACE_WEATHER);
    const int llen = sp_len(head, SP_LABEL_CAP - 1);
    const bool updating = sp_ota_ringing((jr_display_ota_state_t)
        (__atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xFu));
    const int tlen = fc->tlen;
    const int slen = sp_len(s_wx_sun, (int)sizeof s_wx_sun - 1);
    const int olen = sp_len(s_wx_outlook, (int)sizeof s_wx_outlook - 1);
    const int alen = sp_len(s_wx_age, (int)sizeof s_wx_age - 1);
    const glass_metrics_t hero = glass_font_metrics(GLASS_FONT_HERO);
    const int hero_top = cyr - hero.cap_h / 2 - 2;
    const int base = hero_top + hero.cap_h;
    const uint32_t t_lbl = 0, t_hero = 150, t_hl = 250, t_sun = 330, t_age = 410;

    sg_txt_t t_label, t_temp, t_deg, t_hilo, t_sun_t, t_out_t, t_age_t;
    sg_txt_make(&t_label, GLASS_FONT_LABEL, head, llen, SP_CX,
                hero_top - 26 + sg_ent_rise(ent, t_lbl), 3, s_wx_valid ? accent : SP_C_GREY,
                sg_ent_alpha(st, ent, t_lbl));
    sg_ico_t t_ico;
    sg_ico_make(&t_ico, s_wx_icon, SG_WX_ICON_SIZE, SP_CX,
                hero_top - 26 - SG_WX_ICON_GAP - glass_icon_px(SG_WX_ICON_SIZE) + sg_ent_rise(ent, t_lbl),
                accent, sg_ent_alpha(st, ent, t_lbl));
    const int x0 = SP_CX - (fc->hw + 4 + fc->dw) / 2;
    const int hero_y = hero_top + sg_ent_rise(ent, t_hero);
    const int hero_a = sg_ent_alpha(st, ent, t_hero);
    sg_txt_at(&t_temp, GLASS_FONT_HERO, s_wx_temp, tlen, x0, hero_y, SP_C_INK, hero_a);
    sg_txt_at(&t_deg, GLASS_FONT_VALUE, "*", tlen > 0 ? 1 : 0, x0 + fc->hw + 4, hero_y, accent, hero_a);
    sg_txt_make(&t_hilo, GLASS_FONT_TEXT, fc->hilo, fc->hlen, SP_CX, base + 16 + sg_ent_rise(ent, t_hl), 1,
                SP_C_INK, sg_ent_alpha(st, ent, t_hl));
    const int sun_a = sg_ent_alpha(st, ent, t_sun);
    sg_txt_make(&t_sun_t, GLASS_FONT_TEXT, s_wx_sun, slen, SP_CX, base + 40 + sg_ent_rise(ent, t_sun),
                1, SP_C_GREY, sun_a);
    const int age_rise = sg_ent_rise(ent, t_age);
    const int age_a = updating ? 0 : sg_ent_alpha(st, ent, t_age);
    const uint16_t out_col = muted ? SP_C_GOLD : (s_wx_outlook_rain ? 0x3E7Fu : 0x6EF7u);
    const int out_y = SG_AGE_Y + oy + age_rise;
    sg_txt_make(&t_out_t, GLASS_FONT_LABEL, s_wx_outlook, olen, SP_CX, out_y, 2, out_col, age_a);
    sg_txt_make(&t_age_t, GLASS_FONT_LABEL, s_wx_age, alen, SP_CX,
                (olen > 0 ? out_y + 18 : out_y), 2, SP_C_GREY, age_a);

    /* Saturated multi-zone radial sky bloom tuned by actual weather condition.
     * Drops all color on stale/invalid weather; muted shifts to privacy gold. */
    const int atmo_a = sg_ent_alpha(st, ent, t_lbl);
    const bool draw_atmo = s_wx_valid && !s_wx_stale && atmo_a > 0;
    const int bloom_r = 128;
    const int glow_y0 = cyr - bloom_r, glow_y1 = cyr + bloom_r;
    sp_bloom_t sky_bloom;
    sky_bloom.active = false;
    if (draw_atmo && y1 <= glow_y1 && y2 > glow_y0) {
        uint16_t b_in = 0x165Au, b_out = 0x1914u; /* rich cool indigo-teal mist */
        uint8_t b_pk = 104u;
        if (muted) {
            b_in = SP_C_GOLD;
            b_out = SP_C_GOLD_DIM;
            b_pk = 78u;
        } else if (s_wx_sky == JR_DISPLAY_SKY_CLEAR || s_wx_sky == JR_DISPLAY_SKY_PARTLY) {
            b_in = 0xFE44u;  /* lush warm solar amber-gold core (not SP_C_GOLD) */
            b_out = 0xC280u; /* deep warm solar amber outer aura */
            b_pk = 112u;
        } else if (s_wx_sky == JR_DISPLAY_SKY_RAIN) {
            b_in = 0x05FFu;  /* electric sapphire-cyan volumetric core */
            b_out = 0x0217u; /* deep midnight sapphire outer aura */
            b_pk = 114u;
        } else if (s_wx_sky == JR_DISPLAY_SKY_STORM) {
            b_in = 0x34FFu;  /* electric cyan-indigo storm core */
            b_out = 0x4898u; /* deep storm violet-sapphire aura */
            b_pk = 118u;
        }
        sp_bloom_init(&sky_bloom, SP_CX, cyr, bloom_r, b_in, b_out, b_pk, atmo_a, 1);
    }

    /* Glowing golden sunrise -> sunset progress arc + bright sun orb at 6 o'clock */
    const uint32_t sw = __atomic_load_n(&s_sun_word, __ATOMIC_ACQUIRE);
    const bool have_sun_arc = draw_atmo && slen > 0 && (sw & (1u << 22)) != 0u && sun_a > 0;
    int32_t sun_dot_x16 = 0, sun_dot_y16 = 0;
    int32_t sun_lit_start = 0, sun_lit_sweep = 0;
    bool sun_is_day = false;
    const int32_t sun_r_in16 = 102 * 16;
    const int32_t sun_r_out16 = 105 * 16;
    const int32_t sun_a_right = 11833; /* 6 o'clock - 25 deg (sunset, right) */
    const int32_t sun_sweep = 9102;    /* 50 deg total sweep across bottom */
    if (have_sun_arc) {
        const int rise = (int)(sw & 0x7FFu);
        const int set = (int)((sw >> 11) & 0x7FFu);
        const int span = (set - rise + 1440) % 1440;
        const uint32_t cw = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
        int prog = 500;
        if ((cw & (1u << 16)) != 0u && span > 0) {
            const int hh = (int)((cw >> 8) & 0xFFu);
            const int mm = (int)(cw & 0xFFu);
            const int now_min = (hh % 24) * 60 + (mm % 60);
            const int since = (now_min - rise + 1440) % 1440;
            if (since <= span) {
                prog = (since * 1000) / span;
                sun_is_day = true;
            } else {
                prog = (since > span + (1440 - span) / 2) ? 0 : 1000;
            }
        }
        const uint32_t a_dot = (uint32_t)((sun_a_right + sun_sweep) - (prog * sun_sweep) / 1000) & 0xFFFFu;
        const int32_t r_mid16 = (sun_r_in16 + sun_r_out16) / 2;
        sun_dot_x16 = cx16 + ((glass_cos_q15(a_dot) * r_mid16) >> 15);
        sun_dot_y16 = cy16 + ((glass_sin_q15(a_dot) * r_mid16) >> 15);
        sun_lit_start = (int32_t)a_dot;
        sun_lit_sweep = (prog * sun_sweep) / 1000;
    }

    const int ring_y0 = cyr - 148, ring_y1 = cyr + 148;
    const int sun_y0 = cyr + 88, sun_y1 = cyr + 116;
    const bool draw_pill = olen > 0 && age_a > 0 && fc->out_w > 0;
    const int pill_cy = out_y + 5;
    const int pill_hw = fc->out_w / 2 + 10;
    const uint16_t pill_fill = muted ? 0x2100u : (s_wx_outlook_rain ? 0x0148u : 0x0985u);
    const uint16_t sun_gold = muted ? SP_C_GOLD : 0xFDE4u; /* luminous solar gold */
    const uint16_t sun_track = muted ? SP_C_GOLD_DIM : 0x5240u;

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        if (sky_bloom.active && y >= glow_y0 && y <= glow_y1) {
            sp_bloom_row(row, y, lo, hi, &sky_bloom, swap);
        }
        if (y >= ring_y0 && y <= ring_y1) {
            if (!s_wx_valid || s_wx_stale) {
                /* Honest bare track when weather is absent or stale */
                glass_ring_row(row, y, lo, hi, cx16, cy16, SG_WX_TH_IN16, SG_WX_TH_OUT16, 0,
                               GLASS_TURN_Q16, SP_C_TRACK, (uint8_t)st, swap);
            } else {
                /* Recessed 270 deg precision temperature dial track */
                glass_arc_row(row, y, lo, hi, cx16, cy16, SG_WX_TH_IN16, SG_WX_TH_OUT16,
                              SG_WX_DIAL_START, SG_WX_DIAL_SWEEP, SP_C_TRACK,
                              (uint8_t)((st * 150) / 255), swap);
                /* 19 fine precision dial ticks around the 270 deg thermal gauge */
                for (int ti = 0; ti < 24; ++ti) {
                    if (ti >= 10 && ti <= 14) {
                        continue; /* 90 deg cradle at 6 o'clock for sunrise->sunset */
                    }
                    const bool major = (ti == 0 || ti == 6 || ti == 9 || ti == 15 || ti == 18);
                    const int32_t r0 = major ? (107 * 16) : (109 * 16);
                    const int32_t r1 = 112 * 16;
                    const uint8_t ta = (uint8_t)(((major ? 190 : 105) * atmo_a) / 255);
                    sp_radial_tick_row(row, y, lo, hi, cx16, cy16,
                                       k_sp_dir24[ti][0], k_sp_dir24[ti][1],
                                       r0, r1, major ? 12 : 8,
                                       major ? accent : SP_C_GREY, ta, swap);
                }
                /* Luminous halo behind thermal gradient arc */
                if (fc->th_arc_sweep >= 256) {
                    glass_glow_row(row, y, lo, hi, cx16, cy16, SG_WX_TH_IN16 + 16, SG_WX_TH_OUT16 - 16,
                                   fc->th_arc_start, fc->th_arc_sweep, true, 4 * 16, 120u,
                                   fc->pip_col, (uint8_t)st, swap);
                }
                /* Thermal gradient arc segments (lo_f -> hi_f) */
                for (int i = 0; i < fc->th_n; ++i) {
                    if (y >= fc->th_y0[i] && y <= fc->th_y1[i]) {
                        glass_arc_row(row, y, lo, hi, cx16, cy16, SG_WX_TH_IN16 + 16,
                                      SG_WX_TH_OUT16 - 16, fc->th_start[i], fc->th_sweep[i],
                                      fc->th_col[i], (uint8_t)st, swap);
                    }
                }
                /* Glowing temperature pip/orb marking current temp_f */
                if (fc->have_pip) {
                    sp_orb_row(row, y, lo, hi, fc->pip_x16, fc->pip_y16,
                               6 * 16 + 4, 13 * 16, fc->pip_col, 0xFFFFu, (uint8_t)st, swap);
                }
                /* Outer 12-hour luminous rain ring (r134..142) */
                for (int i = 0; i < nseg; ++i) {
                    const uint8_t pct = fc->sg_pct[i];
                    const bool wet = pct >= SP_WX_HOUR_DRY;
                    if (wet && pct >= 25 && y >= fc->sg_y0[i] - 4 && y <= fc->sg_y1[i] + 4) {
                        const uint8_t gpk = (uint8_t)(55u + pct);
                        glass_glow_row(row, y, lo, hi, cx16, cy16, fc->sg_ri[i], fc->sg_ro[i],
                                       fc->sg_start[i], fc->sg_sweep[i], true, 3 * 16, gpk,
                                       rain, fc->sg_a[i], swap);
                    }
                    if (y >= fc->sg_y0[i] && y <= fc->sg_y1[i]) {
                        const uint8_t k = fc->sg_k[i];
                        const uint16_t seg_col = wet ? rain : dry_track;
                        if (use_wx_rec && (s_wx_rec_have & (1u << k)) != 0u) {
                            glass_ring_replay_row(row, y, lo, hi,
                                                  s_wx_rec + (size_t)k * SG_WX_REC_CAP,
                                                  seg_col, fc->sg_a[i], swap);
                        } else {
                            glass_arc_row(row, y, lo, hi, cx16, cy16, fc->sg_ri[i], fc->sg_ro[i],
                                          fc->sg_start[i], fc->sg_sweep[i], seg_col, fc->sg_a[i], swap);
                        }
                    }
                }
            }
        }
        if (have_sun_arc && y >= sun_y0 && y <= sun_y1) {
            glass_arc_row(row, y, lo, hi, cx16, cy16, sun_r_in16, sun_r_out16,
                          sun_a_right, sun_sweep, sun_track,
                          (uint8_t)((175 * sun_a) / 255), swap);
            if (sun_is_day && sun_lit_sweep > 64) {
                glass_glow_row(row, y, lo, hi, cx16, cy16, sun_r_in16, sun_r_out16,
                               sun_lit_start, sun_lit_sweep, true, 3 * 16, 110u,
                               sun_gold, (uint8_t)sun_a, swap);
                glass_arc_row(row, y, lo, hi, cx16, cy16, sun_r_in16, sun_r_out16,
                              sun_lit_start, sun_lit_sweep, sun_gold, (uint8_t)sun_a, swap);
            }
            const uint16_t dot_col = sun_is_day ? sun_gold : (muted ? SP_C_GOLD : 0x9D3Fu);
            sp_orb_row(row, y, lo, hi, sun_dot_x16, sun_dot_y16,
                       4 * 16, 9 * 16, dot_col, 0xFFFFu, (uint8_t)sun_a, swap);
        }
        if (draw_pill && y >= pill_cy - 9 && y <= pill_cy + 9) {
            sp_pill_row(row, y, lo, hi, SP_CX, pill_cy, pill_hw, 9,
                        pill_fill, (uint8_t)((175 * age_a) / 255),
                        out_col, (uint8_t)((155 * age_a) / 255), swap);
        }
        sg_ico_row(row, y, lo, hi, swap, &t_ico);
        sg_txt_row(row, y, lo, hi, swap, &t_label);
        sg_txt_row(row, y, lo, hi, swap, &t_temp);
        sg_txt_row(row, y, lo, hi, swap, &t_deg);
        sg_txt_row(row, y, lo, hi, swap, &t_hilo);
        sg_txt_row(row, y, lo, hi, swap, &t_sun_t);
        sg_txt_row(row, y, lo, hi, swap, &t_out_t);
        sg_txt_row(row, y, lo, hi, swap, &t_age_t);
    }
}

/* ---- the ring while it moves ---------------------------------------------------
 *
 * The three records above are used only once the screen sits still (oy == 0) and
 * the sweep has finished, so for the whole entry (about 22 frames) the ring is
 * drawn the long way: 1.9 M instructions of 4.1 M a frame on the host. Two
 * things keep the records usable while it moves.
 *
 *  - A sliding screen is the same pixels at another height: the record's row
 *    y - oy goes on panel row y (glass_*_replay_row_off). Exact.
 *  - A sweeping arc is the arc of its final value with the swept part left out.
 *    The records are made for the FINAL sweep S (the value the entry is heading
 *    for; it is the same record the settled path replays at the end, so there is
 *    no second hitch), plus one full-turn track. A row whose pixels all lie away
 *    from the moving front (the angle `sweep` plus a margin for the round cap and
 *    the glow) is replayed from them; a row that can touch the front, or the part
 *    between the front and S, is drawn the long way; a row the arc cannot reach is
 *    skipped. Only the rows near the front pay for geometry, about a tenth of them.
 *
 * Which rows can touch which angles comes from a table built once at start: per
 * row, up to two angular intervals (Q16 turns, glass convention) that bound the
 * pixels of the ring's band in that row, a little fat. No float and no libm: the
 * angles come from the glass sine table. 8 bytes a row, 2.1 KB of PSRAM. */
#define SG_RX_HALF     134          /* rows either side of the centre the table has */
#define SG_RX_EXT16    (5 * 16)     /* the band grows by the glow's reach and a pixel of edge */
#define SG_RX_MARGIN   1300         /* Q16 turns kept clear of the front: 14 px at the ring */
#define SG_FR_TRKFULL  8            /* `have` bit: the full-turn track record (block 3) */

typedef struct {
    uint16_t a0[2];                 /* the interval's start, Q16 turn */
    uint16_t len[2];                /* its length; 0 = no such interval */
} sg_rx_t;

static sg_rx_t *s_rx_ring;
static bool s_fring_trkfull;        /* block 3 holds the full-turn track */

/* The Q16 turn of the direction (dx, dy): 0 at 3 o'clock, clockwise with y down. */
static uint16_t sg_arg_q16(int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        return 0;
    }
    int q = 0, x = dx, y = dy;
    if (dx < 0 && dy >= 0) {
        q = 1; x = dy; y = -dx;
    } else if (dx < 0) {
        q = 2; x = -dx; y = -dy;
    } else if (dy < 0) {
        q = 3; x = -dy; y = dx;
    }
    uint32_t lo = 0, hi = 16384;
    while (lo < hi) {               /* the largest angle in the quarter whose tangent is <= y / x */
        const uint32_t mid = (lo + hi + 1u) >> 1;
        if ((int64_t)glass_sin_q15(mid) * x <= (int64_t)glass_cos_q15(mid) * y) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }
    return (uint16_t)(lo + (uint32_t)q * 16384u);
}

static void sg_rx_build(sg_rx_t *t, int32_t r_in16, int32_t r_out16)
{
    const int ro = (r_out16 + SG_RX_EXT16 + 15) / 16;
    const int ri = (r_in16 - SG_RX_EXT16) / 16;
    for (int i = 0; i < 2 * SG_RX_HALF; ++i) {
        const int ry = i - SG_RX_HALF;               /* the row spans dy in [ry, ry + 1] */
        const int a0 = ry < 0 ? -ry : ry, a1 = ry + 1 < 0 ? -(ry + 1) : ry + 1;
        const int near = (ry < 0 && ry + 1 > 0) ? 0 : (a0 < a1 ? a0 : a1);
        const int far = a0 > a1 ? a0 : a1;
        sg_rx_t *r = &t[i];
        memset(r, 0, sizeof *r);
        if (near >= ro) {
            continue;
        }
        const int xo = sp_isqrt(ro * ro - near * near) + 1;
        int xi = 0;
        if (ri > far) {
            xi = sp_isqrt(ri * ri - far * far) - 1;
            if (xi < 0) {
                xi = 0;
            }
        }
        int mn = 32767, mx = -32768;                 /* the right side: |angle| <= a quarter turn */
        for (int c = 0; c < 4; ++c) {
            const int a = (int16_t)sg_arg_q16((c & 1) ? xo : xi, ry + (c >> 1));
            mn = a < mn ? a : mn;
            mx = a > mx ? a : mx;
        }
        r->a0[0] = (uint16_t)(mn - 1);
        r->len[0] = (uint16_t)(mx - mn + 2);
        r->a0[1] = (uint16_t)(32768 - mx - 1);       /* the left side mirrors it */
        r->len[1] = r->len[0];
    }
}

/* Whether a pixel of the row's side k (0 the right half, 1 the left) can lie in
 * the window [w0, w0 + wlen] (a turn is 65536; a window that long is everything). */
static inline bool sg_rx_hits(const sg_rx_t *r, int k, uint16_t w0, uint32_t wlen)
{
    if (r->len[k] == 0u) {
        return false;
    }
    return wlen >= 65535u || (uint16_t)(w0 - r->a0[k]) <= r->len[k] ||
           (uint16_t)(r->a0[k] - w0) <= wlen;
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
 * SG_FRING_CAP bytes in PSRAM (and a fourth, the full-turn track, for the moving ring), allocated once at start (the largest the ring
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
/* The moving ring keeps a second set of the three records, blocks
 * 4 to 6, so two ring screens that share a slide (STATUS leaving, DESK entering)
 * each have theirs; block 3 is the full-turn track both use. */
static struct {
    int32_t sweep;
    uint8_t have;
    uint32_t used;              /* the frame it was last asked for */
} s_fring1;
static uint32_t s_fring0_used;  /* the same for the first set */

static void fring_rec_alloc(void)
{
    if (s_fring_rec == NULL) {
        s_fring_rec = heap_caps_malloc(7u * SG_FRING_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_rx_ring == NULL) {
        s_rx_ring = heap_caps_malloc(2u * SG_RX_HALF * sizeof(sg_rx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_rx_ring != NULL) {
        sg_rx_build(s_rx_ring, SG_RING_R_IN16, SG_RING_R_OUT16);
    }
    /* block 3: the full-turn track the moving ring replays */
    s_fring_trkfull = s_fring_rec != NULL &&
                      glass_ring_record(s_fring_rec + 3u * SG_FRING_CAP, SG_FRING_CAP, SP_CX * 16,
                                        (SP_CY + SG_RING_DY) * 16, SG_RING_R_IN16, SG_RING_R_OUT16) != 0u;
    s_fring.have = 0;
    memset(&s_fring1, 0, sizeof s_fring1);
    s_fring0_used = 0;
}

/* The three records of the ring for `sweep`, into the blocks at `base`; the
 * SG_FR_* bits of the ones that fitted. */
static uint8_t fring_record(uint8_t *base, int32_t sweep)
{
    const int32_t cx16 = SP_CX * 16, cy16 = (SP_CY + SG_RING_DY) * 16;
    uint8_t have = 0;
    if (sweep < GLASS_TURN_Q16 &&
        glass_shape_record(base, SG_FRING_CAP, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                           GLASS_Q16_12_OCLOCK + sweep, GLASS_TURN_Q16 - sweep, false) != 0u) {
        have |= SG_FR_TRACK;
    }
    if (sweep > 0) {
        if (glass_glow_record(base + SG_FRING_CAP, SG_FRING_CAP, cx16, cy16, SG_RING_R_IN16,
                              SG_RING_R_OUT16, GLASS_Q16_12_OCLOCK, sweep, true, SG_GLOW16) != 0u) {
            have |= SG_FR_GLOW;
        }
        if (glass_shape_record(base + 2u * SG_FRING_CAP, SG_FRING_CAP, cx16, cy16,
                               SG_RING_R_IN16, SG_RING_R_OUT16, GLASS_Q16_12_OCLOCK, sweep, true) != 0u) {
            have |= SG_FR_ARC;
        }
    }
    return have;
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
    s_fring.sweep = sweep;
    s_fring.have = fring_record(s_fring_rec, sweep) | 0x80u;   /* tried, so a failure is not retried every strip */
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
        glass_glow_replay_row(row, y, lo, hi, s_fring_rec + SG_FRING_CAP, SG_GLOW16, 170, fill,
                              (uint8_t)st, swap);
    } else {
        glass_glow_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                       GLASS_Q16_12_OCLOCK, sweep, true, SG_GLOW16, 170, fill, (uint8_t)st, swap);
    }
    if (have & SG_FR_ARC) {
        glass_ring_replay_row(row, y, lo, hi, s_fring_rec + 2u * SG_FRING_CAP, fill, (uint8_t)st, swap);
    } else {
        glass_arc_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                      GLASS_Q16_12_OCLOCK, sweep, fill, (uint8_t)st, swap);
    }
}

/* The records the moving ring can use for the final sweep `full`: fring_prepare's
 * three (for the sweep the entry is heading for) and the full-turn track. 0 means
 * the memory is absent: draw it the long way. */
static uint8_t fring_prepare_moving(int32_t full, const uint8_t **base)
{
    *base = s_fring_rec;
    if (s_fring_rec == NULL || s_rx_ring == NULL || !s_fring_trkfull) {
        return 0;
    }
    if (full <= 0) {
        return SG_FR_TRKFULL;       /* no arc: the whole ring is the track */
    }
    uint8_t have;
    if ((s_fring.have & 0x7Fu) != 0u && s_fring.sweep == full) {
        have = s_fring.have & 0x7Fu;
        s_fring0_used = s_frame_no;
    } else if ((s_fring1.have & 0x7Fu) != 0u && s_fring1.sweep == full) {
        have = s_fring1.have & 0x7Fu;
        s_fring1.used = s_frame_no;
        *base = s_fring_rec + 4u * SG_FRING_CAP;
    } else if (s_fring0_used <= s_fring1.used) {
        /* the entry knows its final value from the first frame: record it now,
         * over the set that was asked for longest ago */
        s_fring.sweep = full;
        s_fring.have = fring_record(s_fring_rec, full) | 0x80u;
        s_fring0_used = s_frame_no;
        have = s_fring.have & 0x7Fu;
    } else {
        uint8_t *set1 = s_fring_rec + 4u * SG_FRING_CAP;
        *base = set1;
        s_fring1.sweep = full;
        s_fring1.have = fring_record(set1, full) | 0x80u;
        s_fring1.used = s_frame_no;
        have = s_fring1.have & 0x7Fu;
    }
    return have != 0u ? (uint8_t)(have | SG_FR_TRKFULL) : 0u;
}

/* One side of one row of the ring at height offset oy, the arc swept to `sweep`
 * of its final `full`: the same fills, in the same order, as fring_row. A row's
 * two halves are separate here because they are usually far apart in angle (the
 * right half behind the front, the left half ahead of it). */
static inline void fring_side_moving(uint16_t *row, int y, int lo, int hi, bool swap, int oy,
                                     int32_t sweep, int32_t full, bool arc_on, uint16_t fill,
                                     int st, uint8_t have, const uint8_t *base, const sg_rx_t *rx, int k)
{
    if (lo > hi || rx->len[k] == 0u) {
        return;
    }
    const int ry = y - oy;
    const int32_t cx16 = SP_CX * 16, cy16 = (SP_CY + oy + SG_RING_DY) * 16;
    const uint8_t a_trk = (uint8_t)((st * 44) / 255);
    const uint16_t A = (uint16_t)GLASS_Q16_12_OCLOCK;
    const uint32_t M = SG_RX_MARGIN;
    const bool exact = sweep >= full;

    /* the track: the ring from the front round to 12 o'clock */
    if (!arc_on) {
        glass_ring_replay_row_off(row, y, ry, lo, hi, s_fring_rec + 3u * SG_FRING_CAP, fill, a_trk, swap);
    } else if (exact) {
        if (have & SG_FR_TRACK) {
            glass_ring_replay_row_off(row, y, ry, lo, hi, base, fill, a_trk, swap);
        } else {
            glass_ring_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                           GLASS_Q16_12_OCLOCK + sweep, GLASS_TURN_Q16 - sweep, fill, a_trk, swap);
        }
    } else if (sg_rx_hits(rx, k, (uint16_t)(A + sweep - M), (uint32_t)(GLASS_TURN_Q16 - sweep) + 2u * M)) {
        /* it can touch the track; it can touch the swept part too: the long way */
        if (sg_rx_hits(rx, k, (uint16_t)(A - M), (uint32_t)sweep + 2u * M)) {
            glass_ring_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                           GLASS_Q16_12_OCLOCK + sweep, GLASS_TURN_Q16 - sweep, fill, a_trk, swap);
        } else {
            glass_ring_replay_row_off(row, y, ry, lo, hi, s_fring_rec + 3u * SG_FRING_CAP, fill, a_trk, swap);
        }
    }
    if (!arc_on || !sg_rx_hits(rx, k, (uint16_t)(A - M), (uint32_t)sweep + 2u * M)) {
        return;                     /* the front has not reached this side of the row */
    }
    /* the glow and the arc: replayed unless the side can touch the front or the part
     * between it and the final sweep, which the records have and the frame has not */
    const bool replay = exact || !sg_rx_hits(rx, k, (uint16_t)(A + sweep - M), (uint32_t)(full - sweep) + 2u * M);
    if (replay && (have & SG_FR_GLOW)) {
        glass_glow_replay_row_off(row, y, ry, lo, hi, base + SG_FRING_CAP, SG_GLOW16, 170, fill,
                                  (uint8_t)st, swap);
    } else {
        glass_glow_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                       GLASS_Q16_12_OCLOCK, sweep, true, SG_GLOW16, 170, fill, (uint8_t)st, swap);
    }
    if (replay && (have & SG_FR_ARC)) {
        glass_ring_replay_row_off(row, y, ry, lo, hi, base + 2u * SG_FRING_CAP, fill, (uint8_t)st, swap);
    } else {
        glass_arc_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                      GLASS_Q16_12_OCLOCK, sweep, fill, (uint8_t)st, swap);
    }
}

static inline void fring_row_moving(uint16_t *row, int y, int lo, int hi, bool swap, int oy,
                                    int32_t sweep, int32_t full, bool arc_on, uint16_t fill,
                                    int st, uint8_t have, const uint8_t *base)
{
    const int i = y - (SP_CY + oy + SG_RING_DY) + SG_RX_HALF;
    if (i < 0 || i >= 2 * SG_RX_HALF) {
        return;
    }
    const sg_rx_t *rx = &s_rx_ring[i];
    fring_side_moving(row, y, lo > SP_CX ? lo : SP_CX, hi, swap, oy, sweep, full, arc_on, fill, st,
                      have, base, rx, 0);
    fring_side_moving(row, y, lo, hi < SP_CX - 1 ? hi : SP_CX - 1, swap, oy, sweep, full, arc_on,
                      fill, st, have, base, rx, 1);
}

/* Once a frame, before anything draws: the word httpd set becomes the one the
 * drawing code reads. Bits that need memory or a restart would be handled here
 * on the frame their value changes; there are none at present. */
static void knob_apply(void)
{
    const uint32_t k = __atomic_load_n(&s_perf_knob_word, __ATOMIC_RELAXED);
    hud_perf_knob = k;
    glass_perf_knob = k;
    const bool dip = (k & JR_KNOB_PANEL_DIP) != 0u;
    if (!dip && (s_dip_on || s_dip_phase != DIP_IDLE || s_dip_mode != DIP_MODE_NONE)) {
        dip_abort();           /* the bit went off mid-dip: the panel is brought back, the shell goes on as on knob 0 */
    }
    s_dip_on = dip;
}

/* ACTIVITY: up to three blocks, newest first. Line one is the kind and the age
 * in the label face, line two the summary in the text face; each older block
 * is a step quieter, so the eye lands on what just happened. On entry the
 * blocks fade in and rise, 90 ms apart. With nothing pushed the one honest
 * line is centred where the first block would be. No second dim over the disc:
 * the shell veil already quiets the face and a per-pixel fold over r<=168 once
 * cost ~88k pixels a frame. */
#define SG_ACT_STAGGER_MS 90
#define SP_ACT_SPINE_X    114
#define SP_ACT_TEXT_X     132

static void sp_focal_activity(const jr_display_ctx_t *ctx, int y1, int y2,
                              uint16_t *pixels, int oy, int st, uint32_t ent)
{
    const bool swap = ctx->board.swap_color_bytes;
    const bool muted = sp_privacy_muted();
    static const uint8_t k_fade[ACT_MAX] = {255, 220, 180};
    sg_txt_t t_kind[ACT_MAX], t_age[ACT_MAX], t_sum[ACT_MAX], t_clear, t_empty;
    int orb_y[ACT_MAX] = {0};
    int row_a[ACT_MAX] = {0};
    uint16_t ev_col[ACT_MAX] = {0};
    const int cy = SP_CY + oy;

    const int empty_a = sg_ent_alpha(st, ent, 0);
    const int empty_rise = sg_ent_rise(ent, 0);
    const int radar_cy = cy - 28 + empty_rise;
    const uint16_t radar_col = muted ? SP_C_GOLD : SP_C_CYAN;

    sg_txt_make(&t_clear, GLASS_FONT_LABEL, "ALL CLEAR", s_act_rows == 0 ? 9 : 0, SP_CX,
                cy + 30 + empty_rise, 3, radar_col, empty_a);
    sg_txt_make(&t_empty, GLASS_FONT_TEXT, "NOTHING YET", s_act_rows == 0 ? 11 : 0, SP_CX,
                cy + 48 + empty_rise, 1, SP_C_GREY, empty_a);

    for (int i = 0; i < ACT_MAX; ++i) {
        const bool have = i < s_act_rows;
        const uint32_t start = (uint32_t)i * SG_ACT_STAGGER_MS;
        const int a = (sg_ent_alpha(st, ent, start) * k_fade[i]) / 255;
        const int top = SP_ACT_Y0 + i * SP_ACT_PITCH + oy + sg_ent_rise(ent, start);
        const char *kind = have ? s_act_kind[i] : "";
        const char *age = have ? s_act_age[i] : "";
        const int klen = have ? (s_act_compose_rev != 0u ? s_act_klen[i] : sp_len(kind, ACT_KIND_CAP - 1)) : 0;
        const int alen = have ? (s_act_compose_rev != 0u ? s_act_alen[i] : sp_len(age, SP_ACT_AGE_CAP - 1)) : 0;
        const int kw = have ? (s_act_compose_rev != 0u ? s_act_kw[i] : glass_text_width(GLASS_FONT_LABEL, kind, (size_t)klen, 2)) : 0;
        ev_col[i] = sp_act_kind_color(kind, muted);
        orb_y[i] = top + 16;
        row_a[i] = have ? a : 0;

        sg_txt_at(&t_kind[i], GLASS_FONT_LABEL, kind, klen, SP_ACT_TEXT_X, top, ev_col[i], a);
        t_kind[i].tracking = 2;
        sg_txt_at(&t_age[i], GLASS_FONT_LABEL, age, alen,
                  SP_ACT_TEXT_X + kw + (klen > 0 ? SP_ACT_GAP : 0), top, SP_C_GREY, (a * 2) / 3);
        t_age[i].tracking = 2;
        const int slen = have ? sp_len(s_act_row[i], SP_ACT_ROW_CAP - 1) : 0;
        sg_txt_at(&t_sum[i], GLASS_FONT_TEXT, s_act_row[i], slen, SP_ACT_TEXT_X, top + SP_ACT_SUM_DY,
                  SP_C_INK, a);
    }

    /* Soft deep-cyan/indigo radial ambient backdrop glow so ACTIVITY feels alive on OLED */
    sp_bloom_t bg_bloom;
    bg_bloom.active = false;
    const int bg_r = 152;
    if (st > 0 && y1 <= cy + bg_r && y2 > cy - bg_r) {
        const uint16_t b_in = muted ? SP_C_GOLD : 0x051Au;   /* luminous deep cyan-indigo */
        const uint16_t b_out = muted ? SP_C_GOLD_DIM : 0x0892u; /* midnight indigo */
        sp_bloom_init(&bg_bloom, SP_CX, cy, bg_r, b_in, b_out, muted ? 40u : 64u, st, 0);
    }

    /* Empty state: luminous standby concentric radar + breathing cyan/indigo core bloom */
    sp_bloom_t radar_bloom;
    radar_bloom.active = false;
    const int radar_r = 62;
    if (s_act_rows == 0 && empty_a > 0 && y1 <= radar_cy + radar_r && y2 > radar_cy - radar_r) {
        const uint32_t ph = (s_breath_ms * 65536u) / 3000u;
        const int breath = 128 + (int)(glass_sin_q15(ph) >> 8); /* 0..256 */
        const uint8_t pk = (uint8_t)(72 + ((30 * breath) >> 8));
        const uint16_t r_in = muted ? SP_C_GOLD : 0x06DFu;
        const uint16_t r_out = muted ? SP_C_GOLD_DIM : 0x10B6u;
        sp_bloom_init(&radar_bloom, SP_CX, radar_cy, radar_r, r_in, r_out, pk, empty_a, 0);
    }

    const int spine_y0 = s_act_rows > 0 ? orb_y[0] - 12 : 1;
    const int spine_y1 = s_act_rows > 0 ? orb_y[s_act_rows - 1] + 12 : 0;
    const uint16_t spine_col = muted ? SP_C_GOLD : 0x1D5Bu;
    const int32_t cx16 = SP_CX * 16;
    const int32_t rcy16 = radar_cy * 16;

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        if (bg_bloom.active) {
            sp_bloom_row(row, y, lo, hi, &bg_bloom, swap);
        }
        if (s_act_rows == 0 && empty_a > 0) {
            if (radar_bloom.active) {
                sp_bloom_row(row, y, lo, hi, &radar_bloom, swap);
            }
            if (y >= radar_cy - 56 && y <= radar_cy + 56) {
                glass_glow_row(row, y, lo, hi, cx16, rcy16, 44 * 16, 46 * 16,
                               0, GLASS_TURN_Q16, false, 3 * 16, 85u,
                               radar_col, (uint8_t)empty_a, swap);
                glass_ring_row(row, y, lo, hi, cx16, rcy16, 44 * 16, 46 * 16,
                               0, GLASS_TURN_Q16, radar_col,
                               (uint8_t)((empty_a * 165) / 255), swap);
                glass_ring_row(row, y, lo, hi, cx16, rcy16, 25 * 16, 27 * 16,
                               0, GLASS_TURN_Q16, spine_col,
                               (uint8_t)((empty_a * 125) / 255), swap);
                /* 4 crisp cardinal radar reticle ticks */
                for (int ti = 0; ti < 24; ti += 6) {
                    sp_radial_tick_row(row, y, lo, hi, cx16, rcy16,
                                       k_sp_dir24[ti][0], k_sp_dir24[ti][1],
                                       47 * 16, 53 * 16, 10, radar_col,
                                       (uint8_t)((empty_a * 180) / 255), swap);
                }
                sp_orb_row(row, y, lo, hi, cx16, rcy16, 4 * 16, 11 * 16,
                           radar_col, 0xFFFFu, (uint8_t)empty_a, swap);
            }
            sg_txt_row(row, y, lo, hi, swap, &t_clear);
            sg_txt_row(row, y, lo, hi, swap, &t_empty);
        } else if (s_act_rows > 0) {
            /* Vertical luminous hairline timeline spine */
            if (y >= spine_y0 && y <= spine_y1 && st > 0) {
                int fade = 255;
                if (y - spine_y0 < 12) {
                    fade = ((y - spine_y0 + 1) * 255) / 12;
                } else if (spine_y1 - y < 12) {
                    fade = ((spine_y1 - y + 1) * 255) / 12;
                }
                const uint8_t sa = (uint8_t)((fade * 145 * st) / (255 * 255));
                const uint8_t ha = (uint8_t)((fade * 52 * st) / (255 * 255));
                if (SP_ACT_SPINE_X - 2 >= lo && SP_ACT_SPINE_X - 2 <= hi && ha > 0u) {
                    row[SP_ACT_SPINE_X - 2] = glass_blend565(row[SP_ACT_SPINE_X - 2], spine_col, ha, swap);
                }
                if (SP_ACT_SPINE_X - 1 >= lo && SP_ACT_SPINE_X - 1 <= hi && sa > 0u) {
                    row[SP_ACT_SPINE_X - 1] = glass_blend565(row[SP_ACT_SPINE_X - 1], spine_col, sa, swap);
                }
                if (SP_ACT_SPINE_X >= lo && SP_ACT_SPINE_X <= hi && sa > 0u) {
                    row[SP_ACT_SPINE_X] = glass_blend565(row[SP_ACT_SPINE_X], spine_col, sa, swap);
                }
                if (SP_ACT_SPINE_X + 1 >= lo && SP_ACT_SPINE_X + 1 <= hi && ha > 0u) {
                    row[SP_ACT_SPINE_X + 1] = glass_blend565(row[SP_ACT_SPINE_X + 1], spine_col, ha, swap);
                }
            }
            for (int i = 0; i < s_act_rows; ++i) {
                /* Glowing color-coded event orb on the timeline spine */
                if (row_a[i] > 0) {
                    sp_orb_row(row, y, lo, hi, SP_ACT_SPINE_X * 16, orb_y[i] * 16,
                               4 * 16, 10 * 16, ev_col[i], 0xFFFFu, (uint8_t)row_a[i], swap);
                }
                /* Subtle horizontal hairline divider between timeline entries */
                if (i + 1 < s_act_rows && row_a[i] > 0) {
                    const int sep_y = (orb_y[i] + orb_y[i + 1]) / 2 + 4;
                    if (y == sep_y) {
                        const int xa = SP_ACT_TEXT_X > lo ? SP_ACT_TEXT_X : lo;
                        const int xb = 342 < hi ? 342 : hi;
                        const uint8_t da = (uint8_t)((row_a[i] * 56) / 255);
                        for (int x = xa; x <= xb; ++x) {
                            row[x] = glass_blend565(row[x], spine_col, da, swap);
                        }
                    }
                }
                sg_txt_row(row, y, lo, hi, swap, &t_kind[i]);
                sg_txt_row(row, y, lo, hi, swap, &t_age[i]);
                sg_txt_row(row, y, lo, hi, swap, &t_sum[i]);
            }
        }
    }
}

/* DESK: Holographic Violet Companion Core. Deep electric violet-magenta radial
 * bloom behind the r108..122 progress ring, subtle orbital scanner ticks at
 * r127..132, and a bright violet-white leading tip on the progress arc. */
static void sp_focal_desk(const jr_display_ctx_t *ctx, int y1, int y2,
                          uint16_t *pixels, int oy, int st, uint32_t ent)
{
    const uint32_t w = __atomic_load_n(&s_desk_word, __ATOMIC_ACQUIRE);
    int prog = (int)(w & 0xFFu);
    if (prog > 100) {
        prog = 100;
    }
    const jr_display_agent_state_t state = (jr_display_agent_state_t)((w >> 8) & 0xFu);
    const bool muted = sp_privacy_muted();
    const uint16_t fill = muted ? SP_C_GOLD
                                : (state == JR_DISPLAY_AGENT_NONE ? SP_C_VIOLET : agent_native_color(state));
    const bool swap = ctx->board.swap_color_bytes;

    /* Glass 3.0 CORTEX COMMAND (Desk/Codex mode): 4 chamfered LED telemetry
     * quadrants + crosshair reticle + frosted mission card + live sparkline. */
    if (glass3_active()) {
        const uint32_t pw_w = __atomic_load_n(&s_power_word, __ATOMIC_ACQUIRE);
        const uint8_t bp = (uint8_t)((pw_w & 0xFFu) <= 100u ? (pw_w & 0xFFu) : 0u);
        const bool chg = (pw_w & (1u << 25)) != 0u;
        const uint32_t fp = diag_load(&ctx->free_psram_bytes);
        uint8_t mem_p = (uint8_t)((fp * 100u) / (8u * 1024u * 1024u));
        if (mem_p > 100u) mem_p = 100u;
        const uint32_t fps = diag_load(&ctx->actual_fps);
        uint8_t cpu_p = (uint8_t)((fps * 100u) / 24u);
        if (cpu_p < 15u) cpu_p = 15u;
        if (cpu_p > 100u) cpu_p = 100u;
        jr_g3_cortex_t cx = {
            .mem_pct = mem_p,
            .cpu_pct = cpu_p,
            .batt_pct = bp,
            .agent_pct = (uint8_t)prog,
            .charging = chg,
            .codex_active = true,
            .row_led = {
                state == JR_DISPLAY_AGENT_FAILED ? JR_G3_LED_RED
                    : (state == JR_DISPLAY_AGENT_SUCCEEDED ? JR_G3_LED_GREEN : JR_G3_LED_VIOLET),
                JR_G3_LED_CYAN,
                muted ? JR_G3_LED_AMBER : JR_G3_LED_GREEN,
            },
            .now_ms = (uint32_t)(esp_timer_get_time() / 1000),
            .strength = (uint8_t)(st > 255 ? 255 : (st < 0 ? 0 : st)),
        };
        spectrum_load(cx.bands);
        jr_g3_cortex_rows(pixels, y1, y2 - y1, swap, &cx);

        const char *task = sp_headline(JR_DISPLAY_SPACE_DESK);
        int tlen = sp_len(task, 14);
        if (tlen == 0) { task = "CODEX DECK"; tlen = 10; }
        const char *sname = sp_agent_name(state);
        char r0[20], r1[20], r2[20];
        int l0 = sp_str(r0, 0, (int)sizeof r0, "STATE  ");
        l0 = sp_str(r0, l0, (int)sizeof r0, sname);
        int l1 = sp_str(r1, 0, (int)sizeof r1, "DONE   ");
        l1 = sp_pct(r1, l1, (int)sizeof r1, (uint32_t)prog);
        int l2 = sp_str(r2, 0, (int)sizeof r2, muted ? "MIC    MUTED" : "LINK   CODEX");

        sg_txt_t t_hdr, t_r0, t_r1, t_r2;
        sg_txt_make(&t_hdr, GLASS_FONT_TEXT, task, tlen, SP_CX, 159 + oy, 1,
                    muted ? SP_C_GOLD : 0xE3FFu, st);
        sg_txt_at(&t_r0, GLASS_FONT_TEXT, r0, l0, JR_G3_CORTEX_TEXT_X0, 200 + oy, SP_C_INK, st);
        sg_txt_at(&t_r1, GLASS_FONT_TEXT, r1, l1, JR_G3_CORTEX_TEXT_X0, 234 + oy, fill, st);
        sg_txt_at(&t_r2, GLASS_FONT_TEXT, r2, l2, JR_G3_CORTEX_TEXT_X0, 268 + oy,
                  muted ? SP_C_GOLD : SP_C_MINT, st);

        for (int y = y1; y < y2; ++y) {
            int lo = 0, hi = HUD_W - 1;
            if (!sp_clip(y, &lo, &hi)) continue;
            uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
            sg_txt_row(row, y, lo, hi, swap, &t_hdr);
            sg_txt_row(row, y, lo, hi, swap, &t_r0);
            sg_txt_row(row, y, lo, hi, swap, &t_r1);
            sg_txt_row(row, y, lo, hi, swap, &t_r2);
        }
        return;
    }
    const int cyr = SP_CY + oy + SG_RING_DY;
    const int cy16 = cyr * 16;
    const int cx16 = SP_CX * 16;

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
    static int s_desk_pw;
    static int s_desk_shown = -1, s_desk_hw;
    if (s_desk_pw == 0) {
        s_desk_pw = glass_text_width(GLASS_FONT_VALUE, "%", 1, 0);
    }
    if (s_desk_shown != shown) {
        s_desk_hw = glass_text_width(GLASS_FONT_HERO, num, (size_t)nlen, 0);
        s_desk_shown = shown;
    }
    const int hw = s_desk_hw;
    const int pw = s_desk_pw;
    const int x0 = SP_CX - (hw + 5 + pw) / 2;
    const uint32_t t_lbl = 0, t_word = 150;

    sg_txt_t t_label, t_hero, t_pc, t_state;
    sg_txt_make(&t_label, GLASS_FONT_LABEL, task, tlen, SP_CX,
                hero_top - 26 + sg_ent_rise(ent, t_lbl), 3, fill, sg_ent_alpha(st, ent, t_lbl));
    sg_txt_at(&t_hero, GLASS_FONT_HERO, num, nlen, x0, hero_top, SP_C_INK, st);
    sg_txt_at(&t_pc, GLASS_FONT_VALUE, "%", 1, x0 + hw + 5, base - val.cap_h, fill, st);
    sg_txt_make(&t_state, GLASS_FONT_TEXT, sname, snlen, SP_CX, base + 16 + sg_ent_rise(ent, t_word),
                1, SP_C_GREY, sg_ent_alpha(st, ent, t_word));

    /* Deep electric violet-magenta holographic radial bloom */
    const int bloom_r = 122;
    sp_bloom_t desk_bloom;
    desk_bloom.active = false;
    if (st > 0 && y1 <= cyr + bloom_r && y2 > cyr - bloom_r) {
        uint16_t b_in = 0xD0DFu, b_out = 0x4814u; /* electric violet-magenta -> deep cosmic purple */
        uint8_t b_pk = 116u;
        if (muted) {
            b_in = SP_C_GOLD;
            b_out = SP_C_GOLD_DIM;
            b_pk = 78u;
        } else if (state == JR_DISPLAY_AGENT_WAITING) {
            b_in = 0xFD20u; b_out = 0x7140u; b_pk = 106u;
        } else if (state == JR_DISPLAY_AGENT_SUCCEEDED) {
            b_in = 0x2F74u; b_out = 0x0368u; b_pk = 106u;
        } else if (state == JR_DISPLAY_AGENT_FAILED) {
            b_in = 0xF863u; b_out = 0x6000u; b_pk = 106u;
        } else if (state == JR_DISPLAY_AGENT_VERIFYING) {
            b_in = 0x68DFu; b_out = 0x2834u; b_pk = 110u;
        }
        sp_bloom_init(&desk_bloom, SP_CX, cyr, bloom_r, b_in, b_out, b_pk, st, 1);
    }

    /* Leading tip orb coordinates along progress arc */
    const bool have_tip = sweep > 256 && sweep < GLASS_TURN_Q16 && st > 0;
    int32_t tip_x16 = 0, tip_y16 = 0;
    const uint16_t tip_col = muted ? SP_C_GOLD : (state == JR_DISPLAY_AGENT_WORKING || state == JR_DISPLAY_AGENT_NONE ? 0xE3FFu : fill);
    if (have_tip) {
        const uint32_t a_tip = (uint32_t)(GLASS_Q16_12_OCLOCK + sweep) & 0xFFFFu;
        const int32_t r_mid16 = (SG_RING_R_IN16 + SG_RING_R_OUT16) / 2;
        tip_x16 = cx16 + ((glass_cos_q15(a_tip) * r_mid16) >> 15);
        tip_y16 = cy16 + ((glass_sin_q15(a_tip) * r_mid16) >> 15);
    }

    const int ring_y0 = cyr - 136, ring_y1 = cyr + 136;
    const uint8_t rec = (oy == 0 && f_ring >= GLASS_ONE_Q16) ? (uint8_t)(fring_prepare(sweep) & 0x7Fu) : 0u;
    const uint8_t *mbase = s_fring_rec;
    const uint8_t mov = rec == 0u ? fring_prepare_moving(full, &mbase) : 0u;
    const uint16_t tick_dim = muted ? SP_C_GOLD_DIM : 0x3890u;

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        if (desk_bloom.active) {
            sp_bloom_row(row, y, lo, hi, &desk_bloom, swap);
        }
        if (y >= ring_y0 && y <= ring_y1) {
            /* Subtle concentric orbital scanner hairline & 24 precision scanner ticks (r127..132) */
            glass_ring_row(row, y, lo, hi, cx16, cy16, 129 * 16, 130 * 16 + 8,
                           0, GLASS_TURN_Q16, fill, (uint8_t)((st * 68) / 255), swap);
            for (int ti = 0; ti < 24; ++ti) {
                const bool lit = ((int32_t)ti * GLASS_TURN_Q16) / 24 <= sweep && sweep > 0;
                const bool major = (ti % 6 == 0);
                const int32_t r0 = major ? (126 * 16) : (127 * 16 + 8);
                const int32_t r1 = major ? (133 * 16) : (131 * 16 + 8);
                const uint8_t ta = (uint8_t)(((lit ? (major ? 225 : 175) : (major ? 115 : 75)) * st) / 255);
                sp_radial_tick_row(row, y, lo, hi, cx16, cy16,
                                   k_sp_dir24[ti][0], k_sp_dir24[ti][1],
                                   r0, r1, major ? 12 : 8,
                                   lit ? tip_col : tick_dim, ta, swap);
            }
            if (mov != 0u) {
                fring_row_moving(row, y, lo, hi, swap, oy, sweep, full, sweep > 0, fill, st, mov, mbase);
            } else {
                fring_row(row, y, lo, hi, swap, cy16, sweep, sweep > 0, fill, st, rec);
            }
            if (have_tip) {
                sp_orb_row(row, y, lo, hi, tip_x16, tip_y16,
                           6 * 16 + 4, 14 * 16, tip_col, 0xFFFFu, (uint8_t)st, swap);
            }
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
    const uint32_t chip_raw = (lk >> 16) & 0xFFu;
    const bool chip_ok = chip_raw != 0u;
    const int chip_c = chip_ok ? (int)chip_raw - 40 : 0;
    char chipbuf[8];
    int clen = 0;
    if (chip_ok) {
        clen = sp_int(chipbuf, 0, (int)sizeof chipbuf, chip_c);
        clen = sp_str(chipbuf, clen, (int)sizeof chipbuf, "*");
    } else {
        clen = sp_str(chipbuf, 0, (int)sizeof chipbuf, "--");
    }
    const bool updating = sp_ota_ringing((jr_display_ota_state_t)
        (__atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xFu));
    const char *head = sp_headline(JR_DISPLAY_SPACE_STATUS);
    const int hlen = sp_len(head, SP_LABEL_CAP - 1);
    const bool quiet_head = hlen == 0 || (hlen > 3 && head[0] == 'U' && head[1] == 'P' && head[2] == ' ');
    const char *label = quiet_head ? "BATTERY" : head;
    const int label_len = quiet_head ? 7 : hlen;

    /* Glass 3.0 CORTEX COMMAND (Status mode): 4 chamfered LED telemetry
     * quadrants (MEM, CPU, PWR, AGENT) + crosshair reticle + frosted cockpit
     * card with 3 glowing status LEDs + live bottom sparkline. */
    if (glass3_active()) {
        const uint32_t fp = diag_load(&ctx->free_psram_bytes);
        uint8_t mem_p = (uint8_t)((fp * 100u) / (8u * 1024u * 1024u));
        if (mem_p > 100u) mem_p = 100u;
        const uint32_t fps = diag_load(&ctx->actual_fps);
        uint8_t cpu_p = (uint8_t)((fps * 100u) / 24u);
        if (cpu_p < 15u) cpu_p = 15u;
        if (cpu_p > 100u) cpu_p = 100u;
        const bool desk_on = sp_desk_live();
        const uint8_t ag_p = desk_on
            ? (uint8_t)(__atomic_load_n(&s_desk_word, __ATOMIC_ACQUIRE) & 0xFFu)
            : (uint8_t)(bars * 25);
        jr_g3_cortex_t cx = {
            .mem_pct = mem_p,
            .cpu_pct = cpu_p,
            .batt_pct = have_gauge ? (uint8_t)shown : 0u,
            .agent_pct = ag_p > 100u ? 100u : ag_p,
            .charging = charging,
            .codex_active = desk_on,
            .row_led = {
                bars >= 2 ? JR_G3_LED_GREEN : (bars == 1 ? JR_G3_LED_AMBER : JR_G3_LED_RED),
                link_on ? JR_G3_LED_CYAN : JR_G3_LED_AMBER,
                tools_on ? JR_G3_LED_VIOLET : JR_G3_LED_OFF,
            },
            .now_ms = (uint32_t)(esp_timer_get_time() / 1000),
            .strength = (uint8_t)(st > 255 ? 255 : (st < 0 ? 0 : st)),
        };
        spectrum_load(cx.bands);
        jr_g3_cortex_rows(pixels, y1, y2 - y1, swap, &cx);

        char hdr[18], r0[20], r1[20], r2[20];
        int lh = sp_str(hdr, 0, (int)sizeof hdr, charging ? "CHG  " : "PWR  ");
        lh = sp_str(hdr, lh, (int)sizeof hdr, pctbuf);
        if (have_gauge) lh = sp_str(hdr, lh, (int)sizeof hdr, "%");

        int l0 = sp_str(r0, 0, (int)sizeof r0, "WIFI   ");
        l0 = sp_str(r0, l0, (int)sizeof r0, dbm);
        if (bars > 0) l0 = sp_str(r0, l0, (int)sizeof r0, " dBm");

        int l1 = sp_str(r1, 0, (int)sizeof r1, "LINK   ");
        l1 = sp_str(r1, l1, (int)sizeof r1, link_on ? "LIVE " : "IDLE ");
        l1 = sp_str(r1, l1, (int)sizeof r1, chipbuf);

        int l2 = sp_str(r2, 0, (int)sizeof r2, "TOOLS  ");
        l2 = sp_str(r2, l2, (int)sizeof r2, tools_on ? "READY" : "STANDBY");

        sg_txt_t t_hdr, t_r0, t_r1, t_r2;
        sg_txt_make(&t_hdr, GLASS_FONT_TEXT, hdr, lh, SP_CX, 159 + oy, 1,
                    low ? SP_C_RED : (muted ? SP_C_GOLD : SP_C_CYAN), st);
        sg_txt_at(&t_r0, GLASS_FONT_TEXT, r0, l0, JR_G3_CORTEX_TEXT_X0, 200 + oy, SP_C_INK, st);
        sg_txt_at(&t_r1, GLASS_FONT_TEXT, r1, l1, JR_G3_CORTEX_TEXT_X0, 234 + oy,
                  link_on ? SP_C_CYAN : SP_C_GREY, st);
        sg_txt_at(&t_r2, GLASS_FONT_TEXT, r2, l2, JR_G3_CORTEX_TEXT_X0, 268 + oy,
                  tools_on ? 0xD4FFu : SP_C_GREY, st);

        for (int y = y1; y < y2; ++y) {
            int lo = 0, hi = HUD_W - 1;
            if (!sp_clip(y, &lo, &hi)) continue;
            uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
            sg_txt_row(row, y, lo, hi, swap, &t_hdr);
            sg_txt_row(row, y, lo, hi, swap, &t_r0);
            sg_txt_row(row, y, lo, hi, swap, &t_r1);
            sg_txt_row(row, y, lo, hi, swap, &t_r2);
        }
        return;
    }

    const glass_metrics_t hero = glass_font_metrics(GLASS_FONT_HERO);
    const glass_metrics_t val = glass_font_metrics(GLASS_FONT_VALUE);
    const glass_metrics_t tm = glass_font_metrics(GLASS_FONT_TEXT);
    const int hero_top = cyr - hero.cap_h / 2 - 2;
    const int base = hero_top + hero.cap_h;
    static int s_st_pw;
    static int s_st_key = -2, s_st_hw;
    const int st_key = have_gauge ? shown : -1;
    if (s_st_pw == 0) {
        s_st_pw = glass_text_width(GLASS_FONT_VALUE, "%", 1, 0);
    }
    if (s_st_key != st_key) {
        s_st_hw = glass_text_width(GLASS_FONT_HERO, pctbuf, (size_t)plen, 0);
        s_st_key = st_key;
    }
    const int hw = s_st_hw;
    const int pw = have_gauge ? s_st_pw : 0;
    const int x0 = SP_CX - (hw + (have_gauge ? 5 : 0) + pw) / 2;
    const uint32_t t_lbl = 0, t_word = 150, t_wifi = 250, t_link = 310, t_tools = 370, t_chip = 430;
    const int dim_a = (st * 96) / 255;
    const int cx16 = SP_CX * 16;

    sg_txt_t t_label, t_hero, t_pc, t_word_t;
    sg_txt_t t_sl[4], t_sv[4];
    sg_txt_make(&t_label, GLASS_FONT_LABEL, label, label_len, SP_CX,
                hero_top - 26 + sg_ent_rise(ent, t_lbl), 3,
                low ? SP_C_RED : (muted ? SP_C_GOLD : 0x7EFBu),
                sg_ent_alpha(st, ent, t_lbl));
    sg_txt_at(&t_hero, GLASS_FONT_HERO, pctbuf, plen, x0, hero_top, SP_C_INK, st);
    sg_txt_at(&t_pc, GLASS_FONT_VALUE, "%", have_gauge ? 1 : 0, x0 + hw + 5, base - val.cap_h, fill, st);
    const int word_cx = charging ? SP_CX + (SG_BOLT_INK_W + SG_BOLT_GAP) / 2 : SP_CX;
    const int word_y = base + 16 + sg_ent_rise(ent, t_word);
    sg_txt_make(&t_word_t, GLASS_FONT_TEXT, word, wlen, word_cx,
                word_y, 1, SP_C_GREY, sg_ent_alpha(st, ent, t_word));
    sg_ico_t t_bolt;
    if (charging) {
        const int bolt_y = word_y + tm.cap_h / 2 - glass_icon_px(GLASS_ICON_SM) / 2;
        static const char *s_st_word_ptr;
        static int s_st_ww;
        if (s_st_word_ptr != word) {
            s_st_ww = glass_text_width(GLASS_FONT_TEXT, word, (size_t)wlen, 1);
            s_st_word_ptr = word;
        }
        sg_ico_make(&t_bolt, GLASS_ICON_BOLT, GLASS_ICON_SM,
                    word_cx - s_st_ww / 2 - SG_BOLT_GAP - SG_BOLT_INK_W / 2,
                    bolt_y, fill, sg_ent_alpha(st, ent, t_word));
    } else {
        sg_ico_make(&t_bolt, -1, GLASS_ICON_SM, 0, 0, fill, 0);
    }

    /* 4-column bottom telemetry cluster: WIFI · LINK · TOOLS · CHIP */
    static const char *const k_names[4] = {"WIFI", "LINK", "TOOLS", "CHIP"};
    static const int k_col_dx[4] = { -78, -26, 26, 78 };
    const char *vals[4] = {dbm, link_on ? "ON" : "OFF", tools_on ? "ON" : "OFF", chipbuf};
    const int vlen[4] = {dlen, link_on ? 2 : 3, tools_on ? 2 : 3, clen};
    const bool on[4] = {bars > 0, link_on, tools_on, chip_ok};
    const uint32_t starts[4] = {t_wifi, t_link, t_tools, t_chip};
    int stat_ea[4] = {0};
    int stat_vy[4] = {0};
    int wifi_icon_x = 0;
    int led_x[2] = {0, 0}; /* LINK, TOOLS glowing LED centers */
    for (int i = 0; i < 4; ++i) {
        const int cxi = SP_CX + k_col_dx[i];
        const int rise = sg_ent_rise(ent, starts[i]);
        const int ea = updating ? 0 : sg_ent_alpha(st, ent, starts[i]);
        stat_ea[i] = ea;
        stat_vy[i] = SG_STAT_VALUE_Y + oy + rise;
        sg_txt_make(&t_sl[i], GLASS_FONT_LABEL, k_names[i], (int)strlen(k_names[i]), cxi,
                    SG_STAT_LABEL_Y + oy + rise, 2, SP_C_GREY, ea);
        const int va = on[i] ? ea : (ea * dim_a) / (st > 0 ? st : 1);
        uint16_t vcol = on[i] ? SP_C_INK : SP_C_GREY;
        if (i == 3 && chip_ok) {
            vcol = chip_c >= SP_CHIP_HOT_C ? SP_C_RED : (chip_c >= 62 ? 0xFDE4u : SP_C_INK);
        }
        if (i == 0 && bars > 0) {
            /* 4-bar Wi-Fi icon (14 px) + 3 px gap + dBm numeral centred as a pair */
            const int tw = glass_text_width(GLASS_FONT_TEXT, vals[0], (size_t)vlen[0], 0);
            const int tot = 14 + 3 + tw;
            wifi_icon_x = cxi - tot / 2;
            sg_txt_at(&t_sv[0], GLASS_FONT_TEXT, vals[0], vlen[0],
                      wifi_icon_x + 17, stat_vy[0], vcol, va);
        } else if (i == 1 || i == 2) {
            /* Glowing radial-halo LED dot (6 px dia) + 5 px gap + ON/OFF */
            const int tw = glass_text_width(GLASS_FONT_TEXT, vals[i], (size_t)vlen[i], 0);
            const int tot = 6 + 5 + tw;
            const int lx = cxi - tot / 2;
            led_x[i - 1] = lx + 3;
            sg_txt_at(&t_sv[i], GLASS_FONT_TEXT, vals[i], vlen[i],
                      lx + 11, stat_vy[i], vcol, va);
        } else {
            sg_txt_make(&t_sv[i], GLASS_FONT_TEXT, vals[i], vlen[i], cxi,
                        stat_vy[i], 0, vcol, va);
        }
    }

    /* Dynamic state-colored radial OLED bloom inside the battery ring.
     * Even when privacy-muted, keep the jewel-toned state glow inside the ring
     * while the outer privacy ring and battery accent remain gold. */
    const int bloom_r = 109;
    sp_bloom_t core_bloom;
    core_bloom.active = false;
    uint16_t jewel_col = SP_C_CYAN;
    if (low) {
        jewel_col = SP_C_RED;
    } else if (have_gauge && pct >= 60) {
        jewel_col = SP_C_MINT;
    } else if (have_gauge && pct >= 30) {
        jewel_col = SP_C_CYAN;
    } else if (have_gauge && pct >= 20) {
        jewel_col = 0xFDA2u; /* warm jewel amber */
    }
    if (st > 0 && y1 <= cyr + bloom_r && y2 > cyr - bloom_r) {
        uint16_t b_in = 0x055Au, b_out = 0x01CDu;
        uint8_t b_pk = 64u;
        if (low) {
            /* Pulsing crimson reactor core below 20% */
            const uint32_t ph = (s_breath_ms * 65536u) / 1600u;
            const int pulse = 128 + (int)(glass_sin_q15(ph) >> 8); /* 0..256 */
            b_in = 0xF862u;
            b_out = 0x6000u;
            b_pk = (uint8_t)(88 + ((44 * pulse) >> 8));
        } else if (have_gauge && pct >= 60) {
            b_in = 0x2F96u;  /* lush emerald-mint */
            b_out = 0x03A9u; /* deep forest-teal */
            b_pk = 114u;
        } else if (have_gauge && pct >= 30) {
            b_in = 0x07FFu;  /* electric cyan */
            b_out = 0x02B4u; /* deep sapphire-cyan */
            b_pk = 114u;
        } else if (have_gauge && pct >= 20) {
            b_in = 0xFDA2u;  /* warm jewel amber */
            b_out = 0x79C0u; /* deep burnt amber */
            b_pk = 112u;
        } else if (have_gauge && charging) {
            b_in = 0x07B8u;  /* charging cyan-mint */
            b_out = 0x02D0u;
            b_pk = 108u;
        }
        sp_bloom_init(&core_bloom, SP_CX, cyr, bloom_r, b_in, b_out, b_pk, st, 1);
    }

    const int ring_y0 = cyr - 136, ring_y1 = cyr + 136;
    const bool hit_ring = y1 <= ring_y1 && y2 > ring_y0;
    const uint8_t rec = (hit_ring && oy == 0 && f_ring >= GLASS_ONE_Q16)
                            ? (uint8_t)(fring_prepare(sweep) & 0x7Fu) : 0u;
    const uint8_t *mbase = s_fring_rec;
    const uint8_t mov = (hit_ring && rec == 0u)
                            ? fring_prepare_moving(have_gauge ? full : 0, &mbase) : 0u;

    /* Bright travelling plasma shimmer comet along the battery arc while charging */
    const bool shimmer_on = hit_ring && charging && have_gauge && sweep >= 4096 && st > 0 &&
                            ent >= SP_ENTRY_RING_MS &&
                            jr_display_render_fps() >= JR_DISPLAY_RENDER_FPS;
    int32_t sh_start = 0, sh_sweep = 0;
    int32_t sh_hx16 = 0, sh_hy16 = 0;
    int sh_y0 = 1, sh_y1 = 0;
    uint8_t sh_alpha = 0u;
    const uint16_t sh_col = muted ? 0xFF70u : 0xBFFFu;
    if (shimmer_on) {
        const uint32_t cyc = s_breath_ms % 2200u;
        const int32_t plen = sweep / 6 > 6400 ? 6400 : (sweep / 6 < 2400 ? 2400 : sweep / 6);
        const int32_t travel = sweep - plen;
        if (travel > 0) {
            const int32_t off = (int32_t)(((int64_t)travel * cyc) / 2200u);
            sh_start = (GLASS_Q16_12_OCLOCK + off) & 0xFFFF;
            sh_sweep = plen;
            const uint32_t env_a = (cyc * 32768u) / 2200u;
            const int env = (int)(glass_sin_q15(env_a) >> 7); /* 0..255 */
            sh_alpha = (uint8_t)(((env > 0 ? env : 0) * 165 * st) / (255 * 255));
            if (sh_alpha > 0u) {
                const uint32_t a_head = (uint32_t)(sh_start + sh_sweep) & 0xFFFFu;
                const int32_t r_mid16 = (SG_RING_R_IN16 + SG_RING_R_OUT16) / 2;
                sh_hx16 = cx16 + ((glass_cos_q15(a_head) * r_mid16) >> 15);
                sh_hy16 = cy16 + ((glass_sin_q15(a_head) * r_mid16) >> 15);
                if (!glass_arc_rows(cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                                    sh_start, sh_sweep, true, &sh_y0, &sh_y1)) {
                    sh_y0 = 1;
                    sh_y1 = 0;
                }
            }
        }
    }

    const uint16_t outer_col = muted ? SP_C_GOLD : jewel_col;
    const uint16_t tick_dim = muted ? SP_C_GOLD_DIM : 0x2946u;
    const uint16_t wifi_col = muted ? SP_C_GOLD : SP_C_CYAN;
    const uint16_t link_led_col = muted ? SP_C_GOLD : (link_on ? 0x07F8u : 0x6167u);
    const uint16_t tools_led_col = muted ? SP_C_GOLD : (tools_on ? SP_C_MINT : 0x4228u);

    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        if (core_bloom.active) {
            sp_bloom_row(row, y, lo, hi, &core_bloom, swap);
        }
        if (y >= ring_y0 && y <= ring_y1) {
            /* Fine concentric outer halo/tick ring (r128..132) */
            glass_ring_row(row, y, lo, hi, cx16, cy16, 129 * 16, 131 * 16,
                           0, GLASS_TURN_Q16, outer_col, (uint8_t)((st * 66) / 255), swap);
            for (int ti = 0; ti < 24; ++ti) {
                const bool lit = have_gauge && sweep > 0 &&
                                 ((int32_t)ti * GLASS_TURN_Q16) / 24 <= sweep;
                const bool major = (ti % 6 == 0);
                const int32_t r0 = major ? (127 * 16) : (128 * 16);
                const int32_t r1 = major ? (132 * 16) : (131 * 16 + 8);
                const uint8_t ta = (uint8_t)(((lit ? (major ? 225 : 175) : (major ? 105 : 68)) * st) / 255);
                sp_radial_tick_row(row, y, lo, hi, cx16, cy16,
                                   k_sp_dir24[ti][0], k_sp_dir24[ti][1],
                                   r0, r1, major ? 12 : 8,
                                   lit ? outer_col : tick_dim, ta, swap);
            }
            if (mov != 0u) {
                fring_row_moving(row, y, lo, hi, swap, oy, sweep, have_gauge ? full : 0,
                                 have_gauge && sweep > 0, fill, st, mov, mbase);
            } else {
                fring_row(row, y, lo, hi, swap, cy16, sweep, have_gauge && sweep > 0, fill, st, rec);
            }
            if (sh_alpha > 0u) {
                if (y >= sh_y0 && y <= sh_y1) {
                    glass_arc_row(row, y, lo, hi, cx16, cy16, SG_RING_R_IN16 + 16,
                                  SG_RING_R_OUT16 - 16, sh_start, sh_sweep, sh_col,
                                  sh_alpha, swap);
                }
                sp_orb_row(row, y, lo, hi, sh_hx16, sh_hy16,
                           5 * 16 + 4, 12 * 16, sh_col, 0xFFFFu, sh_alpha, swap);
            }
        }
        sg_txt_row(row, y, lo, hi, swap, &t_label);
        sg_txt_row(row, y, lo, hi, swap, &t_hero);
        sg_txt_row(row, y, lo, hi, swap, &t_pc);
        sg_txt_row(row, y, lo, hi, swap, &t_word_t);
        sg_ico_row(row, y, lo, hi, swap, &t_bolt);
        if (bars > 0 && stat_ea[0] > 0) {
            sp_wifi_bars_row(row, y, lo, hi, wifi_icon_x, stat_vy[0] + tm.cap_h - 1,
                             bars, wifi_col, (uint8_t)stat_ea[0], swap);
        }
        if (stat_ea[1] > 0) {
            const int ly = stat_vy[1] + tm.cap_h / 2;
            const uint8_t la = (uint8_t)(link_on ? stat_ea[1] : (stat_ea[1] * 130) / 255);
            sp_orb_row(row, y, lo, hi, led_x[0] * 16, ly * 16,
                       3 * 16, link_on ? (7 * 16) : (4 * 16),
                       link_led_col, link_on ? 0xFFFFu : link_led_col, la, swap);
        }
        if (stat_ea[2] > 0) {
            const int ly = stat_vy[2] + tm.cap_h / 2;
            const uint8_t la = (uint8_t)(tools_on ? stat_ea[2] : (stat_ea[2] * 130) / 255);
            sp_orb_row(row, y, lo, hi, led_x[1] * 16, ly * 16,
                       3 * 16, tools_on ? (7 * 16) : (4 * 16),
                       tools_led_col, tools_on ? 0xFFFFu : tools_led_col, la, swap);
        }
        for (int i = 0; i < 4; ++i) {
            sg_txt_row(row, y, lo, hi, swap, &t_sl[i]);
            sg_txt_row(row, y, lo, hi, swap, &t_sv[i]);
        }
    }
}

/* The update ring's colour IS its meaning, so the palette is the contract:
 * cyan is progress, emerald-mint is on-trial (N9.11: distinct from violet
 * companion rim), amber is "look at this", red is broken. Nothing here is
 * gold — gold is privacy's alone. */
static uint16_t sp_ota_native(jr_display_ota_state_t state)
{
    switch (state) {
    case JR_DISPLAY_OTA_PREFLIGHT:  return SP_C_CYAN_DIM;
    case JR_DISPLAY_OTA_RECEIVING:  return SP_C_CYAN;
    case JR_DISPLAY_OTA_PROBATION:  return SP_C_MINT;
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
 * Anti-aliased via glass_ring_row/glass_arc_row with half-pixel inset radii
 * (SP_OTA_IN + 0.5 .. SP_OTA_OUT - 0.5) so AA edge ramps stay strictly inside
 * [SP_OTA_IN - 1, SP_OTA_OUT + 1] while keeping crisp round caps. */
static void apply_ota_ring(const jr_display_ctx_t *ctx, int y1, int y2,
                           uint16_t *pixels)
{
    const uint32_t ota = __atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE);
    const jr_display_ota_state_t state = (jr_display_ota_state_t)(ota & 0xFu);
    if (!sp_ota_ringing(state)) {
        return;
    }
    if (y1 > SP_CY + SP_OTA_OUT + 2 || y2 <= SP_CY - SP_OTA_OUT - 2) {
        return;
    }
    const int under = 255 - (s_shade_ease * 255) / 256;
    const int st = (under * (256 - s_detail_ease)) / 256;
    if (st <= 0) {
        return;
    }
    const bool swap = ctx->board.swap_color_bytes;
    const uint16_t ink = sp_ota_native(state);
    const int fill = sp_ota_fill(state, (int)((ota >> 8) & 0xFFu));
    const int32_t cx16 = SP_CX * 16;
    const int32_t cy16 = SP_CY * 16;
    const int32_t r_in16 = SP_OTA_IN * 16 + 8;
    const int32_t r_out16 = SP_OTA_OUT * 16 - 8;
    const int32_t sweep = (int32_t)(((int64_t)fill * GLASS_TURN_Q16) / 100);

    for (int y = y1; y < y2; ++y) {
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        if (fill <= 0) {
            glass_ring_row(row, y, lo, hi, cx16, cy16, r_in16, r_out16,
                           0, GLASS_TURN_Q16, SP_C_TRACK, (uint8_t)st, swap);
        } else if (fill >= 100) {
            glass_ring_row(row, y, lo, hi, cx16, cy16, r_in16, r_out16,
                           0, GLASS_TURN_Q16, ink, (uint8_t)st, swap);
        } else {
            glass_ring_row(row, y, lo, hi, cx16, cy16, r_in16, r_out16,
                           (GLASS_Q16_12_OCLOCK + sweep) & 0xFFFF,
                           GLASS_TURN_Q16 - sweep, SP_C_TRACK, (uint8_t)st, swap);
            glass_arc_row(row, y, lo, hi, cx16, cy16, r_in16, r_out16,
                          GLASS_Q16_12_OCLOCK, sweep, ink, (uint8_t)st, swap);
        }
    }
}

/* Format timer remaining seconds as "MM:SS" (or "H:MM:SS" for >= 1 hour). */
static int sp_timer_fmt(char *dst, int cap, uint32_t rem_s)
{
    int len = 0;
    if (rem_s >= 3600u) {
        const uint32_t h = rem_s / 3600u;
        const uint32_t m = (rem_s % 3600u) / 60u;
        const uint32_t s = rem_s % 60u;
        len = sp_num(dst, len, cap, h);
        len = sp_str(dst, len, cap, m < 10u ? ":0" : ":");
        len = sp_num(dst, len, cap, m);
        len = sp_str(dst, len, cap, s < 10u ? ":0" : ":");
        return sp_num(dst, len, cap, s);
    }
    const uint32_t m = rem_s / 60u;
    const uint32_t s = rem_s % 60u;
    if (m < 10u) {
        len = sp_str(dst, len, cap, "0");
    }
    len = sp_num(dst, len, cap, m);
    len = sp_str(dst, len, cap, s < 10u ? ":0" : ":");
    return sp_num(dst, len, cap, s);
}

/* N16.4 Timer Overlay: sits at r202-206 (inside JR_DISPLAY_SHELL_R_MAX=214,
 * clear of battery/privacy rims r215-222 and watch hands r<=190) with a sleek
 * haloed bottom MM:SS readout inside JR_DISPLAY_SAFE_R. Drawn right after
 * apply_ota_ring() so WATCH disc-clear never wipes it. Pure function of
 * (s_timer_word, s_timer_label, now_ms, y) -> 12-row strip invariant. */
#define SP_TIMER_R_IN16   (202 * 16)
#define SP_TIMER_R_OUT16  (206 * 16)
#define SP_TIMER_TIME_Y   378
#define SP_TIMER_LBL_Y    358
#define SP_TIMER_TRACK    2

static void sp_timer_halo_row(uint16_t *row, int y, int lo, int hi,
                              int bx0, int bx1, bool swap)
{
    int x0 = bx0 < lo ? lo : bx0;
    int x1 = bx1 > hi ? hi : bx1;
    (void)y;
    for (int x = x0; x <= x1; ++x) {
        if (row[x] != 0u) {
            row[x] = hud_dim565(row[x], swap);
        }
    }
}

static void apply_timer_overlay(const jr_display_ctx_t *ctx, int y1, int y2,
                                uint16_t *pixels, uint32_t now_ms,
                                bool watch_on_glass)
{
    const uint32_t tw = atomic_load_explicit(&s_timer_word, memory_order_acquire);
    if ((tw & TIMER_ACTIVE_BIT) == 0u) {
        return;
    }
    if (s_choice_n > 0 || s_ask_ease > 0 || s_shade_ease > 0 || s_detail_ease > 0 ||
        s_canvas_ease > 0) {
        return;
    }
    const jr_display_ota_state_t ota_state =
        (jr_display_ota_state_t)(__atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xFu);
    if (sp_ota_ringing(ota_state) && !watch_on_glass) {
        return;  /* OTA outranks timer on non-WATCH screens */
    }
    if (y1 > SP_CY + 208 || y2 <= SP_CY - 208) {
        return;
    }

    const bool swap = ctx->board.swap_color_bytes;
    const bool ringing = (tw & TIMER_RINGING_BIT) != 0u;
    const uint32_t rem_s = tw & TIMER_REM_MASK;
    uint32_t pct = (tw & TIMER_PCT_MASK) >> TIMER_PCT_SHIFT;
    if (pct > 100u) {
        pct = 100u;
    }
    const int32_t cx16 = SP_CX * 16;
    const int32_t cy16 = SP_CY * 16;

    /* Smooth pulse: 2 Hz alternating amber/cyan when ringing, 1 Hz amber pulse
     * when under 60 s. NEVER gold (gold is privacy-only). */
    const uint32_t phase_1hz = (uint32_t)(((uint64_t)(now_ms % 1000u) * GLASS_TURN_Q16) / 1000u);
    const uint32_t phase_2hz = (uint32_t)(((uint64_t)(now_ms % 500u) * GLASS_TURN_Q16) / 500u);
    const int p1 = (int)((32768 - glass_cos_q15(phase_1hz)) >> 8); /* 0..256 */
    const int p2 = (int)((32768 - glass_cos_q15(phase_2hz)) >> 8); /* 0..256 */

    uint16_t arc_col;
    uint8_t arc_alpha;
    if (ringing) {
        arc_col = ((now_ms / 500u) & 1u) == 0u ? SP_C_AMBER : SP_C_CYAN;
        arc_alpha = (uint8_t)(160 + ((95 * p2) >> 8));
    } else if (rem_s < 60u) {
        arc_col = SP_C_AMBER;
        arc_alpha = (uint8_t)(175 + ((80 * p1) >> 8));
    } else {
        arc_col = SP_C_CYAN;
        arc_alpha = 235u;
    }

    const int32_t sweep = (int32_t)(((int64_t)pct * GLASS_TURN_Q16) / 100);

    /* Bottom MM:SS readout + optional ringing label, both haloed inside SAFE_R. */
    const glass_metrics_t ml = glass_font_metrics(GLASS_FONT_LABEL);
    char tbuf[12];
    const int tlen = ringing ? sp_str(tbuf, 0, (int)sizeof tbuf, "00:00")
                             : sp_timer_fmt(tbuf, (int)sizeof tbuf, rem_s);
    const int ty_lo = SP_TIMER_TIME_Y + ml.ink_top;
    const int ty_hi = SP_TIMER_TIME_Y + ml.ink_bottom;
    const bool hit_time = y1 <= ty_hi + 3 && y2 > ty_lo - 3;
    int tx = 0, tw_px = 0;
    if (hit_time) {
        tw_px = glass_text_width(GLASS_FONT_LABEL, tbuf, (size_t)tlen, SP_TIMER_TRACK);
        tx = SP_CX - tw_px / 2;
    }

    char lbuf[TIMER_LABEL_CAP];
    int llen = 0;
    const int ly_lo = SP_TIMER_LBL_Y + ml.ink_top;
    const int ly_hi = SP_TIMER_LBL_Y + ml.ink_bottom;
    const bool hit_lbl = ringing && y1 <= ly_hi + 3 && y2 > ly_lo - 3;
    int lx = 0, lw_px = 0;
    if (hit_lbl) {
        memcpy(lbuf, s_timer_label, sizeof lbuf);
        lbuf[sizeof lbuf - 1] = '\0';
        llen = sp_len(lbuf, (int)sizeof lbuf - 1);
        if (llen > 0) {
            lw_px = glass_text_width(GLASS_FONT_LABEL, lbuf, (size_t)llen, SP_TIMER_TRACK);
            lx = SP_CX - lw_px / 2;
        }
    }

    const bool g3_suppress_chip =
        glass3_active() &&
        (s_cap_alpha > 0 ||
         s_space_to == (uint8_t)JR_DISPLAY_SPACE_STATUS ||
         s_space_to == (uint8_t)JR_DISPLAY_SPACE_WEATHER ||
         s_space_to == (uint8_t)JR_DISPLAY_SPACE_DESK);
    const uint16_t txt_col = ringing ? arc_col : (rem_s < 60u ? SP_C_AMBER : SP_C_INK);

    for (int y = y1; y < y2; ++y) {
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        if (ringing) {
            glass_ring_row(row, y, lo, hi, cx16, cy16, SP_TIMER_R_IN16, SP_TIMER_R_OUT16,
                           0, GLASS_TURN_Q16, arc_col, arc_alpha, swap);
        } else {
            glass_ring_row(row, y, lo, hi, cx16, cy16, SP_TIMER_R_IN16, SP_TIMER_R_OUT16,
                           0, GLASS_TURN_Q16, SP_C_TRACK, 64u, swap);
            if (sweep > 0) {
                glass_arc_row(row, y, lo, hi, cx16, cy16, SP_TIMER_R_IN16, SP_TIMER_R_OUT16,
                              GLASS_Q16_12_OCLOCK, sweep, arc_col, arc_alpha, swap);
            }
        }
        if (!g3_suppress_chip && hit_lbl && llen > 0 && y >= ly_lo - 3 && y <= ly_hi + 3) {
            sp_timer_halo_row(row, y, lo, hi, lx - 6, lx + lw_px + 6, swap);
            if (y >= ly_lo && y <= ly_hi) {
                glass_text_row(row, y, lo, hi, GLASS_FONT_LABEL, lbuf, (size_t)llen,
                               lx, SP_TIMER_LBL_Y, SP_TIMER_TRACK, SP_C_INK, arc_alpha, swap);
            }
        }
        if (!g3_suppress_chip && hit_time && y >= ty_lo - 3 && y <= ty_hi + 3) {
            sp_timer_halo_row(row, y, lo, hi, tx - 6, tx + tw_px + 6, swap);
            if (y >= ty_lo && y <= ty_hi) {
                glass_text_row(row, y, lo, hi, GLASS_FONT_LABEL, tbuf, (size_t)tlen,
                               tx, SP_TIMER_TIME_Y, SP_TIMER_TRACK, txt_col, 245u, swap);
            }
        }
    }
}

/* Format stopwatch elapsed milliseconds as "MM:SS.d" (or "H:MM:SS" >= 1 h). */
static int sp_stopwatch_fmt(char *dst, int cap, uint32_t elapsed_ms)
{
    const uint32_t total_s = elapsed_ms / 1000u;
    const uint32_t tenths = (elapsed_ms % 1000u) / 100u;
    int len = 0;
    if (total_s >= 3600u) {
        return sp_timer_fmt(dst, cap, total_s);
    }
    const uint32_t m = total_s / 60u;
    const uint32_t s = total_s % 60u;
    if (m < 10u) {
        len = sp_str(dst, len, cap, "0");
    }
    len = sp_num(dst, len, cap, m);
    len = sp_str(dst, len, cap, s < 10u ? ":0" : ":");
    len = sp_num(dst, len, cap, s);
    len = sp_str(dst, len, cap, ".");
    return sp_num(dst, len, cap, tenths);
}

/* Wave 2 Chronograph Stopwatch overlay: electric-cyan sweeping chronograph
 * seconds arc at r202-206 + sleek bottom haloed MM:SS.d chip (and L1..L4 lap
 * badge when lap_count > 0). Yields to ask, shade, detail, OTA (non-WATCH),
 * and an active countdown timer. 100% 12-row strip invariant. */
static void apply_stopwatch_overlay(const jr_display_ctx_t *ctx, int y1, int y2,
                                    uint16_t *pixels, bool watch_on_glass)
{
    const uint32_t sw = atomic_load_explicit(&s_sw_word, memory_order_acquire);
    if ((sw & SW_ACTIVE_BIT) == 0u) {
        return;
    }
    if (s_choice_n > 0 || s_ask_ease > 0 || s_shade_ease > 0 || s_detail_ease > 0 ||
        s_canvas_ease > 0) {
        return;
    }
    if ((atomic_load_explicit(&s_timer_word, memory_order_acquire) & TIMER_ACTIVE_BIT) != 0u) {
        return;  /* countdown timer owns r202-206 while active */
    }
    const jr_display_ota_state_t ota_state =
        (jr_display_ota_state_t)(__atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xFu);
    if (sp_ota_ringing(ota_state) && !watch_on_glass) {
        return;
    }
    if (y1 > SP_CY + 208 || y2 <= SP_CY - 208) {
        return;
    }

    const bool swap = ctx->board.swap_color_bytes;
    const bool running = (sw & SW_RUNNING_BIT) != 0u;
    const uint32_t laps = (sw & SW_LAP_MASK) >> SW_LAP_SHIFT;
    const uint32_t elapsed_ms = atomic_load_explicit(&s_sw_elapsed_ms, memory_order_acquire);
    const int32_t cx16 = SP_CX * 16;
    const int32_t cy16 = SP_CY * 16;

    /* 60-second chronograph sweep clockwise from 12 o'clock. */
    const uint32_t cyc_ms = elapsed_ms % 60000u;
    const int32_t sweep = (int32_t)(((uint64_t)cyc_ms * GLASS_TURN_Q16) / 60000u);
    const uint8_t arc_alpha = running ? 245u : 165u;

    /* Leading luminous tip dot on the chronograph sweep when running. */
    const uint32_t tip_a = (uint32_t)(GLASS_Q16_12_OCLOCK + sweep) & 0xFFFFu;
    const int32_t r_mid16 = 204 * 16;
    const int32_t tip_x16 = cx16 + ((glass_cos_q15(tip_a) * r_mid16) >> 15);
    const int32_t tip_y16 = cy16 + ((glass_sin_q15(tip_a) * r_mid16) >> 15);
    const bool hit_tip = running && y1 <= (tip_y16 >> 4) + 5 && y2 > (tip_y16 >> 4) - 5;

    const glass_metrics_t ml = glass_font_metrics(GLASS_FONT_LABEL);
    char tbuf[12];
    const int tlen = sp_stopwatch_fmt(tbuf, (int)sizeof tbuf, elapsed_ms);
    const int ty_lo = SP_TIMER_TIME_Y + ml.ink_top;
    const int ty_hi = SP_TIMER_TIME_Y + ml.ink_bottom;
    const bool hit_time = y1 <= ty_hi + 3 && y2 > ty_lo - 3;
    int tx = 0, tw_px = 0;
    if (hit_time) {
        tw_px = glass_text_width(GLASS_FONT_LABEL, tbuf, (size_t)tlen, SP_TIMER_TRACK);
        tx = SP_CX - tw_px / 2;
    }

    char lbuf[8];
    int llen = 0;
    const int ly_lo = SP_TIMER_LBL_Y + ml.ink_top;
    const int ly_hi = SP_TIMER_LBL_Y + ml.ink_bottom;
    const bool hit_lap = laps > 0u && y1 <= ly_hi + 3 && y2 > ly_lo - 3;
    int lx = 0, lw_px = 0;
    if (hit_lap) {
        llen = sp_str(lbuf, 0, (int)sizeof lbuf, "L");
        llen = sp_num(lbuf, llen, (int)sizeof lbuf, laps > 99u ? 99u : laps);
        lw_px = glass_text_width(GLASS_FONT_LABEL, lbuf, (size_t)llen, SP_TIMER_TRACK);
        lx = SP_CX - lw_px / 2;
    }

    const bool g3_suppress_sw_chip =
        glass3_active() &&
        (s_cap_alpha > 0 ||
         s_space_to == (uint8_t)JR_DISPLAY_SPACE_STATUS ||
         s_space_to == (uint8_t)JR_DISPLAY_SPACE_WEATHER ||
         s_space_to == (uint8_t)JR_DISPLAY_SPACE_DESK);
    const uint16_t txt_col = running ? SP_C_INK : SP_C_GREY;

    for (int y = y1; y < y2; ++y) {
        int lo = 0, hi = HUD_W - 1;
        if (!sp_clip(y, &lo, &hi)) {
            continue;
        }
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        glass_ring_row(row, y, lo, hi, cx16, cy16, SP_TIMER_R_IN16, SP_TIMER_R_OUT16,
                       0, GLASS_TURN_Q16, SP_C_TRACK, 64u, swap);
        if (sweep > 0) {
            glass_arc_row(row, y, lo, hi, cx16, cy16, SP_TIMER_R_IN16, SP_TIMER_R_OUT16,
                          GLASS_Q16_12_OCLOCK, sweep, SP_C_CYAN, arc_alpha, swap);
        }
        if (hit_tip) {
            glass_disc_row(row, y, lo, hi, tip_x16, tip_y16, 3 * 16 + 4,
                           0xBFFFu, 250u, swap);
        }
        if (!g3_suppress_sw_chip && hit_lap && llen > 0 && y >= ly_lo - 3 && y <= ly_hi + 3) {
            sp_timer_halo_row(row, y, lo, hi, lx - 6, lx + lw_px + 6, swap);
            if (y >= ly_lo && y <= ly_hi) {
                glass_text_row(row, y, lo, hi, GLASS_FONT_LABEL, lbuf, (size_t)llen,
                               lx, SP_TIMER_LBL_Y, SP_TIMER_TRACK, SP_C_CYAN, 235u, swap);
            }
        }
        if (!g3_suppress_sw_chip && hit_time && y >= ty_lo - 3 && y <= ty_hi + 3) {
            sp_timer_halo_row(row, y, lo, hi, tx - 6, tx + tw_px + 6, swap);
            if (y >= ty_lo && y <= ty_hi) {
                glass_text_row(row, y, lo, hi, GLASS_FONT_LABEL, tbuf, (size_t)tlen,
                               tx, SP_TIMER_TIME_Y, SP_TIMER_TRACK, txt_col, 245u, swap);
            }
        }
    }
}

/* Glass-2 Transient Rim Level Arc: left rim cyan for volume, right rim amber
 * for brightness (matching sp_draw_shade), strictly in r223-231 so the
 * r221-222 privacy ring is never touched. Self-expires in 1200 ms with a
 * smooth 200 ms fade-out. */
#define RIM_LEVEL_R_IN16   (224 * 16 + 8)
#define RIM_LEVEL_R_OUT16  (230 * 16 - 8)
#define RIM_LEVEL_Y_HALF   160

static void apply_rim_level_overlay(const jr_display_ctx_t *ctx, int y1, int y2,
                                    uint16_t *pixels, uint32_t now_ms)
{
    if (s_choice_n > 0 || s_ask_ease > 0 ||
        __atomic_load_n(&s_commit_pct, __ATOMIC_ACQUIRE) > 0U ||
        s_shade_ease > 0) {
        return;
    }
    uint32_t w = 0u, age = 0u;
    if (!rim_level_sample(now_ms, &w, &age)) {
        return;
    }
    if (y1 > SP_CY + RIM_LEVEL_Y_HALF || y2 <= SP_CY - RIM_LEVEL_Y_HALF) {
        return;
    }
    int alpha = 255;
    if (age >= RIM_LEVEL_TTL_MS - RIM_LEVEL_FADE_MS) {
        alpha = (int)(((RIM_LEVEL_TTL_MS - age) * 255u) / RIM_LEVEL_FADE_MS);
    }
    if (alpha <= 0) {
        return;
    }
    const bool is_brt = (w & RIM_LEVEL_BRT_BIT) != 0u;
    int pct = (int)(w & RIM_LEVEL_PCT_MASK);
    if (pct > 100) {
        pct = 100;
    }
    const bool swap = ctx->board.swap_color_bytes;

    /* Glass 3.0 Rotary Bezel Scrub Overlay: luminous 270-deg precision dial
     * arc + 41-tick inner scale + faceted jewel indicator (r=204..231) plus
     * a crisp upper readout pill strictly inside r < 180. */
    if (glass3_active()) {
        jr_g3_bezel_scrub_rows(pixels, y1, y2 - y1, swap, now_ms, (uint8_t)pct,
                               is_brt ? JR_G3_SCRUB_BRIGHTNESS : JR_G3_SCRUB_VOLUME,
                               alpha);
        char lbl[12];
        int l = sp_str(lbl, 0, (int)sizeof lbl, is_brt ? "BRT " : "VOL ");
        l = sp_pct(lbl, l, (int)sizeof lbl, (uint32_t)pct);
        const int pill_cy = 102;
        const int tw = glass_text_width(GLASS_FONT_LABEL, lbl, (size_t)l, 2);
        const int hw = tw / 2 + 12;
        const uint16_t col = is_brt ? SP_C_CYAN : SP_C_AMBER;
        const glass_metrics_t ml = glass_font_metrics(GLASS_FONT_LABEL);
        sg_txt_t t_sc;
        sg_txt_make(&t_sc, GLASS_FONT_LABEL, lbl, l, SP_CX,
                    pill_cy - ml.cap_h / 2, 2, SP_C_INK, alpha);
        if (y1 <= pill_cy + 14 && y2 > pill_cy - 14) {
            for (int y = y1; y < y2; ++y) {
                if (y < pill_cy - 13 || y > pill_cy + 13) continue;
                int lo = 0, hi = HUD_W - 1;
                if (!sp_clip(y, &lo, &hi)) continue;
                uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
                sp_pill_row(row, y, lo, hi, SP_CX, pill_cy, hw, 12,
                            0x0862u, (uint8_t)((215 * alpha) / 255),
                            col, (uint8_t)((220 * alpha) / 255), swap);
                sg_txt_row(row, y, lo, hi, swap, &t_sc);
            }
        }
        return;
    }
    const int32_t cx16 = SP_CX * 16;
    const int32_t cy16 = SP_CY * 16;
    const int32_t span = 2 * 7282; /* +-40 deg about 9 / 3 o'clock (SG_SH_HALF_Q16) */
    const int32_t sweep = (int32_t)(((int64_t)span * pct) / 100);
    const uint16_t col = is_brt ? SP_C_AMBER : SP_C_CYAN;
    const uint8_t a_trk = (uint8_t)((alpha * 60) / 255);
    const int32_t track_start = is_brt
        ? ((GLASS_TURN_Q16 - 7282) & 0xFFFF)
        : ((GLASS_TURN_Q16 / 2 - 7282) & 0xFFFF);
    const int32_t fill_start = is_brt
        ? ((7282 - sweep) & 0xFFFF)
        : track_start;

    for (int y = y1; y < y2; ++y) {
        if (y < SP_CY - RIM_LEVEL_Y_HALF || y > SP_CY + RIM_LEVEL_Y_HALF) {
            continue;
        }
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        glass_arc_row(row, y, 0, HUD_W - 1, cx16, cy16,
                      RIM_LEVEL_R_IN16, RIM_LEVEL_R_OUT16,
                      track_start, span, SP_C_TRACK, a_trk, swap);
        if (sweep > 0) {
            glass_arc_row(row, y, 0, HUD_W - 1, cx16, cy16,
                          RIM_LEVEL_R_IN16, RIM_LEVEL_R_OUT16,
                          fill_start, sweep, col, (uint8_t)alpha, swap);
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
            /* OPAQUE: the home face's bright core showed through the tint as an
             * orange streak. Black under the tint covers whatever is below it. */
            glass_disc_row(row, y, lo, hi, cx16, bcy16, br16, 0x0000u, (uint8_t)a_btn, swap);
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
/* THE DIAL IS THE SCREEN, NOT ITS BACKDROP. A ring screen's veil pushes the
 * reactor back behind its content; a baked dial is the content, and a
 * half-brightness Submariner is a broken one (measured 2026-09-02 on the panel:
 * the 12 o'clock lume at 62 against 130 in the art). So a slide no longer veils
 * the layer at all: it dips it to black and back (tr_layer), and only a sheet or
 * the shade over the watch veils it. */

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
        uint16_t last_in = 0U, last_out = 0U;           /* as in sp_veil */
        for (int x = lo; x <= hi; ++x) {
            const uint16_t v = row[x];
            if (v != 0U) {
                if (v != last_in) {
                    last_in = v;
                    last_out = sp_fade_to_black565(v, swap, k);
                }
                row[x] = last_out;
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
    /* A complete black makes any other veil moot, and a parked backdrop has
     * no face pixels to fold at all. */
    const int veil = s_space_black >= 256 ? 0 : (s_space_veil * 32) >> 8;
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
        const int focal = (under * (256 - s_detail_ease)) / 256;
        if (focal > 0) {
            /* VERTICAL slide, matching the gesture that drives it. The ring
             * is walked by swiping up and down, so content that moved
             * left/right contradicted the finger — it read as the screen
             * sliding sideways while you pushed it down.
             *
             * Sign: going FORWARD (swipe down) the outgoing screen leaves
             * upward and the incoming one arrives from below, so the stack
             * appears to move with the finger rather than against it.
             *
             * One grammar (tr_out / tr_in): the outgoing screen travels a short
             * way and is gone at the cut; the incoming one starts its full
             * travel from the cut and settles ease-out. They are never both
             * above zero, so the two screens cannot sit on each other. */
            /* The panel dip draws one screen at a time at full strength and in its
             * settled place: the old one until the glass is dark, then the new one
             * with its own entry clock; the dimming is the panel's. */
            const bool dip_sp = s_dip_mode == DIP_MODE_SPACE;
            const int po = dip_sp ? (s_dip_swapped ? 0 : 256) : tr_out(s_space_prog, TR_CUT_SLIDE);
            const int pi = dip_sp ? (s_dip_swapped ? 256 : 0) : tr_in(s_space_prog, TR_CUT_SLIDE);
            if (po > 0) {
                sp_draw_space(ctx, y1, y2, pixels, s_space_from,
                              -s_space_dir * ((SP_LEAVE_PX * (256 - po)) / 256),
                              (focal * po) / 256,
                              dip_sp ? s_entry_ms : SP_ENTRY_TOTAL_MS);   /* (a screen cut in on while it was still entering stays as it was) */
            }
            if (pi > 0) {
                sp_draw_space(ctx, y1, y2, pixels, s_space_to,
                              s_space_dir * ((SP_SLIDE_PX * (256 - pi)) / 256),
                              (focal * pi) / 256, s_entry_ms);
            }
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

static jr_display_rest_mode_t rest_eligible_raw(const jr_display_ctx_t *ctx)
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
           !sp_timer_ringing() &&
           !sp_stopwatch_running() &&
           !rim_level_active(now) &&
           diag_load(&ctx->test_pattern) == JR_DISPLAY_TEST_OFF &&
           jr_display_hud_enabled();
    return allowed ? mode : JR_DISPLAY_REST_NONE;
}

/* REST BY DAY ON WATCH KEEPS THE DIAL. A resting watch used to swap its dial for
 * the glass-font time face in every rest mode, which threw away the one thing
 * the owner chose. By day (a glance, or the nightstand while it is not night and
 * the time is known) the chosen dial stays, drawn as a still: no seconds hand,
 * nothing animating, only the privacy dot, no pixel dimming, and redrawn only when
 * the minute (or what the rim shows) changes. At night, in DREAM, or with no time
 * to show, the time face is the right thing and stays. Render and presenter
 * tasks. */
static bool rest_mode_keeps_dial(const jr_display_ctx_t *ctx, jr_display_rest_mode_t mode)
{
    if (mode == JR_DISPLAY_REST_NONE || mode == JR_DISPLAY_REST_DREAM) {
        return false;
    }
    const uint32_t env = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    const uint32_t clock = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
    (void)ctx;
    return (__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) & NAV_SPACE_MASK) ==
               (uint32_t)JR_DISPLAY_SPACE_WATCH &&
           (clock & (1u << 16)) != 0U &&
           (mode == JR_DISPLAY_REST_GLANCE || (env & JR_DISPLAY_ENV_NIGHT) == 0U);
}

/* The rest face (time over a line) is what rest_eligible names; a resting dial
 * is NONE there, because the dial is drawn by the ordinary path. */
static jr_display_rest_mode_t rest_eligible(const jr_display_ctx_t *ctx)
{
    const jr_display_rest_mode_t mode = rest_eligible_raw(ctx);
    return rest_mode_keeps_dial(ctx, mode) ? JR_DISPLAY_REST_NONE : mode;
}

/* True while the watch rests by day on its dial. */
static bool watch_rest_day(const jr_display_ctx_t *ctx)
{
    return rest_mode_keeps_dial(ctx, rest_eligible_raw(ctx));
}

/* PLAN.md N16.6: Quiet WATCH rest in WHISPER (<= 6 fps). CELESTIAL (stars off
 * when not awake), DRESS, and MINIMAL have no live motion when settled at
 * rest, so redrawing them at 6 fps wastes CPU/SPI power. Follow the exact same
 * minute-clock still rule as daytime NIGHTSTAND/GLANCE when settled & static. */
static bool watch_quiet_whisper(const jr_display_ctx_t *ctx)
{
    if (jr_display_render_fps() > 6U) {
        return false;
    }
    const uint32_t nav = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    if ((nav & (NAV_SPACE_MASK | NAV_OVL_MASK)) != (uint32_t)JR_DISPLAY_SPACE_WATCH) {
        return false;
    }
    const uint32_t style = (nav & NAV_STYLE_MASK) >> NAV_STYLE_SHIFT;
    if (style != (uint32_t)JR_WATCH_CELESTIAL &&
        style != (uint32_t)JR_WATCH_DRESS &&
        style != (uint32_t)JR_WATCH_MINIMAL) {
        return false;
    }
    const uint32_t clock = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
    if ((clock & (1u << 16)) == 0U) {
        return false;
    }
    const uint32_t env = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    const uint32_t request = __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
    const int caption = __atomic_load_n(&s_caption_on, __ATOMIC_ACQUIRE);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t ripple = __atomic_load_n(&s_ripple_start_ms, __ATOMIC_ACQUIRE);
    const uint32_t bloom = __atomic_load_n(&s_bloom_start_ms, __ATOMIC_ACQUIRE);
    return !(env & JR_DISPLAY_ENV_VOICE_BUSY) &&
           !(request & JR_DISPLAY_CMD_BLANK) &&
           !(diag_load(&ctx->shell_word) & (JR_DISPLAY_SHELL_SHADE | JR_DISPLAY_SHELL_AGENT)) &&
           __atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE) == 0 &&
           __atomic_load_n(&s_commit_pct, __ATOMIC_ACQUIRE) == 0U &&
           (!ripple || now - ripple >= HUD_RIPPLE_MS) &&
           (!bloom || now - bloom >= HUD_BLOOM_MS) &&
           !jr_display_canvas_active() && !jr_display_surface_is_active() &&
           (!caption || (caption & JR_DISPLAY_CAPTION_AMBIENT)) &&
           !atomic_load(&s_caption_pinned) &&
           s_space_to == JR_DISPLAY_SPACE_WATCH && s_space_prog == 256 &&
           s_style_prog == 256 &&
           s_detail_ease == 0 && s_shade_ease == 0 &&
           s_ask_ease == 0 && s_canvas_ease == 0 &&
           !sp_ota_ringing((jr_display_ota_state_t)
               (__atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xfu)) &&
           !sp_timer_ringing() &&
           !sp_stopwatch_running() &&
           !rim_level_active(now) &&
           diag_load(&ctx->test_pattern) == JR_DISPLAY_TEST_OFF &&
           jr_display_hud_enabled();
}

static bool watch_should_rest_still(const jr_display_ctx_t *ctx)
{
    return watch_rest_day(ctx) || watch_quiet_whisper(ctx);
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
           !sp_timer_ringing() &&
           !sp_stopwatch_running() &&
           !rim_level_active(now) &&
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

/* WATCH: THE DIAL OWNS THE WHOLE GLASS. The shell's furniture is a guest there:
 * the one rim ring (battery, charging, privacy) is drawn across the dial's art
 * (through DIVER's minute track, inside DRESS's gold, over the movement), and on a
 * device that sits on its charger that was all anyone ever saw. So while WATCH is
 * the screen and nothing is happening the rim is a single cyan privacy dot at 12
 * o'clock when the microphone is live, and nothing when it is muted. "Nothing is
 * happening" is exactly: no voice session (capture, think, speak) or error, no
 * sheet or shade, no question or commit ring, no agent or operator-lease rim, no
 * firmware update. Every one of those brings the full ring back at once; every
 * other screen keeps the ring always. */
static uint8_t watch_rim_mode(const jr_display_ctx_t *ctx, uint32_t env_word)
{
    const uint32_t nav = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    /* the screen on the glass, not the one asked for: a swipe or a dip changes the
     * space at its cut, and the screen leaving keeps its own ring until then */
    const bool sliding = s_space_prog < 256;
    const bool past_cut = s_dip_mode == DIP_MODE_SPACE ? s_dip_swapped : s_space_prog >= TR_CUT_SLIDE;
    const uint8_t shown = sliding && !past_cut ? s_space_from : s_space_to;
    if (shown != (uint8_t)JR_DISPLAY_SPACE_WATCH) {
        return HUD_RIM_RING;
    }
    const uint32_t request = __atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE);
    const jr_face_t face = (jr_face_t)((request & JR_DISPLAY_CMD_FACE_MASK) >>
                                      JR_DISPLAY_CMD_FACE_SHIFT);
    const bool session = face == JR_FACE_LISTENING || face == JR_FACE_THINKING ||
                         face == JR_FACE_SPEAKING || face == JR_FACE_ERROR ||
                         (env_word & JR_DISPLAY_ENV_VOICE_BUSY) != 0U;
    const bool quiet =
           !session && !(nav & NAV_OVL_MASK) &&
           s_detail_ease == 0 && s_shade_ease == 0 && s_ask_ease == 0 &&
           __atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE) == 0 &&
           __atomic_load_n(&s_commit_pct, __ATOMIC_ACQUIRE) == 0U &&
           !(diag_load(&ctx->shell_word) & JR_DISPLAY_SHELL_AGENT) &&
           !(__atomic_load_n(&s_caption_on, __ATOMIC_ACQUIRE) && s_caption_lease) &&
           !sp_ota_ringing((jr_display_ota_state_t)
               (__atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xfu));
    if (!quiet) {
        return HUD_RIM_RING;
    }
    return (env_word & (1u << 9)) != 0U ? HUD_RIM_NONE : HUD_RIM_DOT;
}

/* `alpha` is the strength of the rest face's digits: the ring round a rest face is drawn at the
 * same strength, never brighter than the time it frames. */
static void apply_rest_rims(jr_display_ctx_t *ctx, int y1, int y2, uint16_t *pixels, int alpha)
{
    memset(pixels, 0, (size_t)(y2 - y1) * HUD_W * sizeof *pixels);
    const uint32_t word = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    hud_env_t env = {
        .face = HUD_FACE_IDLE,
        .batt_pct = (uint8_t)word,
        .charging = (word & (1u << 8)) != 0U,
        .privacy_muted = (word & (1u << 9)) != 0U,
        .rim = watch_rim_mode(ctx, word),
        .rim_a = (uint8_t)alpha,
    };
    hud_overlay_frame(pixels, y1, y2 - y1, 0U, ctx->board.swap_color_bytes, &env);
}

/* THE REST FACE, in the glass type. One face for every rest mode (nightstand,
 * glance, dream), on whichever of JARVIS or WATCH is shown: the time large and
 * centred in the HERO digits, and under it at most one quiet line. It was the
 * 5x7 bitmap (a dim "15:46", then "BAT 25%" and "CHARGING" on lines of their
 * own), the crudest thing on the device. The ink is the same dim grey as before
 * (the mood ladder dims the panel itself); only the type and the layout change.
 * Pure function of the time word, the view and the strip, so a face reached by
 * going to rest while shown and one entered at rest are the same pixels. */
#define REST_TIME_CAP_TOP_ONE   (HUD_H / 2 - 30)        /* the time alone: centred */
#define REST_TIME_CAP_TOP_TWO   (HUD_H / 2 - 46)        /* the time over a line    */
#define REST_LINE_GAP           44                       /* cap bottom to the line's cap top */
#define REST_TIME_TRACK         3
#define REST_LINE_TRACK         3
/* The ink is warm amber, never grey: about half strength by day, a quarter at night
 * and in DREAM, and the line at two thirds of the digits. */
#define REST_AMBER565           0xFD43u
#define REST_ALPHA_DAY          128
#define REST_ALPHA_NIGHT        64

/* Returns true when the strip carries the time, and the time is a real one. */
static bool rest_face_rows(const jr_display_ctx_t *ctx, int y1, int y2, uint16_t *pixels,
                           const char *time, const char *line, int alpha)
{
    const bool swap = ctx->board.swap_color_bytes;
    const glass_metrics_t mh = glass_font_metrics(GLASS_FONT_HERO);
    const glass_metrics_t ml = glass_font_metrics(GLASS_FONT_LABEL);
    const int tn = (int)strlen(time), ln = (int)strlen(line);
    const int tw = glass_text_width(GLASS_FONT_HERO, time, (size_t)tn, REST_TIME_TRACK);
    const int lw = ln > 0 ? glass_text_width(GLASS_FONT_LABEL, line, (size_t)ln, REST_LINE_TRACK) : 0;
    const int ty = ln > 0 ? REST_TIME_CAP_TOP_TWO : REST_TIME_CAP_TOP_ONE;
    const int ly = ty + mh.cap_h + REST_LINE_GAP;
    const int tx = (HUD_W - tw) / 2 + REST_TIME_TRACK / 2;
    const int lx = (HUD_W - lw) / 2 + REST_LINE_TRACK / 2;
    for (int y = y1; y < y2; ++y) {
        uint16_t *row = pixels + (size_t)(y - y1) * HUD_W;
        if (y >= ty + mh.ink_top && y <= ty + mh.ink_bottom) {
            glass_text_row(row, y, 0, HUD_W - 1, GLASS_FONT_HERO, time, (size_t)tn, tx, ty,
                           REST_TIME_TRACK, REST_AMBER565, alpha, swap);
        }
        if (ln > 0 && y >= ly + ml.ink_top && y <= ly + ml.ink_bottom) {
            glass_text_row(row, y, 0, HUD_W - 1, GLASS_FONT_LABEL, line, (size_t)ln, lx, ly,
                           REST_LINE_TRACK, REST_AMBER565, (alpha * 2) / 3, swap);
        }
    }
    return strcmp(time, "--:--") != 0 && y1 < ty + mh.ink_bottom + 1 && y2 > ty + mh.ink_top;
}

static void apply_dream_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                                 uint16_t *pixels)
{
    apply_rest_rims(ctx, y1, y2, pixels, REST_ALPHA_NIGHT);
    char text[6];
    dream_clock_text(text, __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE));
    (void)rest_face_rows(ctx, y1, y2, pixels, text, "", REST_ALPHA_NIGHT);
}

static bool apply_companion_overlay(jr_display_ctx_t *ctx, int y1, int y2,
                                    uint16_t *pixels, jr_display_rest_mode_t mode)
{
    /* Same authoritative normal-colour rims as DREAM; fixed charge phase. */
    jr_companion_view_t view;
    companion_view(mode, &view);
    apply_rest_rims(ctx, y1, y2, pixels, view.night ? REST_ALPHA_NIGHT : REST_ALPHA_DAY);
    /* The one quiet line: the next item when a glance has one, else the
     * battery and what the charger is doing. */
    char line[JR_COMPANION_ITEM_CHARS + 16];
    line[0] = '\0';
    if (view.lines[1].text[0] != '\0') {
        snprintf(line, sizeof line, "%s", view.lines[1].text);
    } else if (view.battery_percent <= 100U) {
        snprintf(line, sizeof line, "%u%%%s%s", (unsigned)view.battery_percent,
                 view.lines[3].text[0] != '\0' ? "  " : "", view.lines[3].text);
    } else if (view.lines[3].text[0] != '\0') {
        snprintf(line, sizeof line, "%s", view.lines[3].text);
    }
    return rest_face_rows(ctx, y1, y2, pixels, view.lines[0].text, line,
                          view.night ? REST_ALPHA_NIGHT : REST_ALPHA_DAY);
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
    if (s_watch_s <= 0 || s_detail_ease > 0 || s_shade_ease > 0 || watch_in_motion()) {
        return false;     /* in motion nothing is dead: the shell is drawing the screen that is leaving or arriving */
    }
    const jr_face_t dial = watch_dial_of_style(s_style_shown);
#ifdef ESP_PLATFORM
    if (s_style_shown == (uint8_t)JR_WATCH_MINIMAL && s_dial_blit && s_dial_minimal) {
        return false;
    }
#endif
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
    hud_env_t env = {
        .face     = hud_face_of((jr_face_t)diag_load(&ctx->applied_face)),
        .amp      = (uint8_t)diag_load(&ctx->requested_amplitude),
        .batt_pct = (uint8_t)(w & 0xFFu),
        .charging = (w & (1u << 8)) != 0u,
        .privacy_muted = (w & (1u << 9)) != 0u,
        .ox       = (int8_t)((w >> 16) & 0xFFu),
        .oy       = (int8_t)((w >> 24) & 0xFFu),
    };
    env.rim = watch_rim_mode(ctx, w);      /* a dial owns the glass: a dot, or nothing, until something happens */
    if (env.rim == HUD_RIM_RING && env.face == HUD_FACE_IDLE &&
        (__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) & NAV_SPACE_MASK) ==
            (uint32_t)JR_DISPLAY_SPACE_WATCH &&
        (jr_face_t)((__atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE) & JR_DISPLAY_CMD_FACE_MASK) >>
                    JR_DISPLAY_CMD_FACE_SHIFT) == JR_FACE_ERROR) {
        env.face = HUD_FACE_ERROR;         /* the applied face is the dial; the error is still the voice's */
    }
    int64_t sm = esp_timer_get_time();
    const uint32_t now_ms = (uint32_t)(sm / 1000);
    const bool g3 = glass3_active();
    const jr_glass3_mode_t g3_mode =
        (jr_glass3_mode_t)__atomic_load_n(&s_glass3_mode, __ATOMIC_ACQUIRE);
    const bool canvas_on = s_canvas_ease > 0;
    const bool under_sheet_now = s_detail_ease > 0 || s_shade_ease > 0;
    const int cn_now = __atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE);
    const jr_display_space_t nav_space =
        (jr_display_space_t)(__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) & NAV_SPACE_MASK);
    const bool g3_home_eligible =
        g3 && !canvas_on && !under_sheet_now && cn_now == 0 &&
        s_space_to == (uint8_t)JR_DISPLAY_SPACE_JARVIS &&
        nav_space == JR_DISPLAY_SPACE_JARVIS &&
        s_watch_s == 0 && s_space_black < 256;
    const bool voice_live =
        env.face == HUD_FACE_LISTEN || env.face == HUD_FACE_THINK ||
        env.face == HUD_FACE_SPEAK;
    const bool g3_use_aeth =
        g3_home_eligible &&
        (g3_mode == JR_GLASS3_AETHERIUM ||
         (g3_mode == JR_GLASS3_AUTO && voice_live && !env.privacy_muted));

    if (canvas_on) {
        /* Remote canvas owns the entire 466x466 glass cleanly: suppress voice
         * halos and battery rim so pushed artwork is 100% unobstructed. */
    } else if (g3_home_eligible) {
        const uint32_t frame_ms = s_fade_prev_ms != 0u ? s_fade_prev_ms : now_ms;
        int ring_s = 256 - s_space_black;
        if (ring_s > 255) ring_s = 255;
        if (ring_s > 0) {
            if (g3_use_aeth) {
                jr_g3_aeth_mode_t am = JR_G3_AETH_LISTEN;
                if (env.face == HUD_FACE_SPEAK) {
                    am = JR_G3_AETH_SPEAK;
                } else if (env.face == HUD_FACE_THINK) {
                    am = JR_G3_AETH_THINK;
                }
                uint8_t spec[16];
                spectrum_load(spec);
                jr_g3_aetherium_rows(pixels, y1, nrows, ctx->board.swap_color_bytes,
                                     frame_ms, am, spec, env.amp, ring_s);
            } else {
                const uint32_t lk = __atomic_load_n(&s_links_word, __ATOMIC_ACQUIRE);
                const jr_g3_chrono_env_t cenv = {
                    .batt_pct = env.batt_pct,
                    .charging = env.charging,
                    .wifi_bars = (uint8_t)sp_wifi_bars(lk),
                    .amp = env.amp,
                    .muted = env.privacy_muted,
                    .codex_active = sp_desk_live(),
                    .voice_busy = voice_live,
                };
                jr_g3_chrono_arc_rows(pixels, y1, nrows, ctx->board.swap_color_bytes,
                                      frame_ms, &cenv, ring_s);
            }
        }
        if (env.face == HUD_FACE_ERROR) {
            hud_overlay_frame(pixels, y1, nrows, now_ms,
                              ctx->board.swap_color_bytes, &env);
        }
    } else {
        if (g3 && (s_space_to == (uint8_t)JR_DISPLAY_SPACE_WEATHER ||
                   s_space_to == (uint8_t)JR_DISPLAY_SPACE_STATUS ||
                   s_space_to == (uint8_t)JR_DISPLAY_SPACE_DESK)) {
            /* Atmosphaera & Cortex Command own the full 360-deg perimeter. */
            env.rim = HUD_RIM_NONE;
        }
        hud_overlay_frame(pixels, y1, nrows, now_ms,
                          ctx->board.swap_color_bytes, &env);
    }

    /* Wave 4 / Glass 3.0: Real-Time 3D Geodesic Hologram Core on JARVIS Home.
     * Strictly gated on s_canvas_ease == 0 (never bleeds over remote canvas)
     * and clamped to r <= 56 in Aetherium mode (strictly clear of the lower
     * vocal ribbon y=286..334) and r <= 86 in Chrono-Arc (inside r<106). */
    if (__atomic_load_n(&s_holo_on, __ATOMIC_ACQUIRE) != 0u &&
        !canvas_on &&
        s_space_to == (uint8_t)JR_DISPLAY_SPACE_JARVIS &&
        s_watch_s == 0 && s_detail_ease == 0 && s_shade_ease == 0 &&
        cn_now == 0 && s_ask_ease == 0) {
        if (nav_space == JR_DISPLAY_SPACE_JARVIS) {
            const jr_face_t af = (jr_face_t)diag_load(&ctx->applied_face);
            jr_holo_mode_t hmode = JR_HOLO_IDLE;
            if (env.privacy_muted || af == JR_FACE_MUTED) {
                hmode = JR_HOLO_MUTED;
            } else if (af == JR_FACE_SPEAKING) {
                hmode = JR_HOLO_SPEAK;
            } else if (af == JR_FACE_LISTENING) {
                hmode = JR_HOLO_LISTEN;
            } else if (af == JR_FACE_THINKING || af == JR_FACE_LINKING) {
                hmode = JR_HOLO_THINK;
            }
            const int holo_s = 256 - s_space_black;
            if (holo_s > 0) {
                const uint32_t frame_ms = s_fade_prev_ms != 0u ? s_fade_prev_ms : now_ms;
                uint8_t spec[16];
                spectrum_load(spec);
                const int holo_r = g3_use_aeth ? 56 : (g3_home_eligible ? 86 : 92);
                jr_showpiece_holo_rows(pixels, y1, nrows, ctx->board.swap_color_bytes,
                                       frame_ms, holo_r, hmode, s_tilt.x, s_tilt.y,
                                       spec, env.amp, holo_s > 255 ? 255 : holo_s);
            }
        }
    }

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
    if (cn > 0) {
        apply_ask_overlay(ctx, y1, y2, pixels, cn, ae);
        split_mark(JR_DISPLAY_SPLIT_HUD_ASK, &sm);
    } else {
        /* The spatial shell owns the glass between the face and the caption:
         * the four spaces, the context sheet, and the control shade. It is
         * skipped entirely while an ask is up (the ask owns the glass) and
         * draws nothing at all in JARVIS at rest. */
        /* A style change dips the layer under the dial in the same way a slide
         * does; it is not a slide, so it has no space overlay to carry it. */
        if (s_style_black > 0 && !s_parked) {
            sp_veil_black(ctx, y1, y2, pixels, (s_style_black * 32) >> 8);
        }
        /* WATCH UNDER A SHEET. With the shade or the detail sheet up, the watch
         * is drawn first, so the shell's veil dims the dial and the hands as one
         * and the sheet lands over them; the clock overlay below stands down. */
        const bool under_sheet = s_detail_ease > 0 || s_shade_ease > 0;
        const bool watch_on_glass =
            (jr_display_space_t)(__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) &
                                 NAV_SPACE_MASK) == JR_DISPLAY_SPACE_WATCH ||
            (s_space_from == (uint8_t)JR_DISPLAY_SPACE_WATCH && s_space_prog < 256);
        if (!canvas_on && under_sheet && watch_on_glass) {
            apply_clock_overlay(ctx, y1, y2, pixels);
        }
        if ((!canvas_on || under_sheet) && !watch_clear_follows(ctx)) {
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
        if (!canvas_on && !under_sheet && (!s_space_on || watch_on_glass)) {
            apply_clock_overlay(ctx, y1, y2, pixels);  /* gates itself */
        }
        split_mark(JR_DISPLAY_SPLIT_HUD_CLOCK, &sm);
        /* The firmware update ring sits ABOVE the watch: the clock keeps its
         * dial clean by clearing the disc, and the one job that can brick the
         * device must not be the thing that clear erases. Gated on the OTA
         * word alone, so it shows on JARVIS at rest without waking the shell. */
        apply_ota_ring(ctx, y1, y2, pixels);
        apply_timer_overlay(ctx, y1, y2, pixels, now_ms, watch_on_glass);
        apply_stopwatch_overlay(ctx, y1, y2, pixels, watch_on_glass);
        {
            /* The commit ring shares the choice band and is drawn only on the
             * no-ask path, which is what makes the two mutually exclusive. */
            const uint8_t cpct = __atomic_load_n(&s_commit_pct, __ATOMIC_ACQUIRE);
            if (cpct > 0U) {
                hud_overlay_commit(pixels, y1, nrows,
                                   ctx->board.swap_color_bytes, cpct, 255);
            }
        }
        apply_rim_level_overlay(ctx, y1, y2, pixels, now_ms);
        split_mark(JR_DISPLAY_SPLIT_HUD_OTA, &sm);
        apply_caption_overlay(ctx, y1, y2, pixels);    /* gates on its ease */
        split_mark(JR_DISPLAY_SPLIT_HUD_CAPTION, &sm);
        if (ae > 0 && s_ask_shown_n > 0) {
            apply_ask_overlay(ctx, y1, y2, pixels, s_ask_shown_n, ae);
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
    if (s_choice_n > 0 || s_ask_ease > 0 || s_commit_pct > 0U ||
        s_canvas_ease > 0 ||
        rim_level_active((uint32_t)(esp_timer_get_time() / 1000))) {
        return;
    }
    if (glass3_active() &&
        (s_space_to == (uint8_t)JR_DISPLAY_SPACE_JARVIS ||
         s_space_to == (uint8_t)JR_DISPLAY_SPACE_STATUS ||
         s_space_to == (uint8_t)JR_DISPLAY_SPACE_DESK)) {
        /* Chrono-Arc's 12 o'clock AI capsule & Cortex Command's AGENT quadrant
         * already carry agent progress cleanly in Glass 3.0 without overlapping. */
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

/* The dial copy takes only the columns inside the round glass, so a strip's
 * corners hold whatever the buffer held before. The panel shows black there:
 * make the mirror say so. A pixel is inside when its centre is within the
 * radius (doubled coordinates, so every term is an integer). Only the mirror
 * copy runs this, never the render path. */
static void snapshot_mask_corners(uint16_t *dst, int x1, int width, int y,
                                  int panel_w, int panel_h)
{
    const int dy = 2 * y + 1 - panel_h;
    const int a = sp_isqrt(panel_w * panel_w - dy * dy);   /* |2x+1-w| <= a */
    int lo = (panel_w - 1 - a + 1) / 2;                    /* ceil((w-1-a)/2) */
    int hi = (panel_w - 1 + a) / 2;
    if (a <= 0) {
        lo = panel_w;
        hi = -1;
    }
    for (int x = x1; x < x1 + width; ++x) {
        if (x < lo || x > hi) {
            dst[x - x1] = 0;
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
        snapshot_mask_corners(dst, x1, width, y1 + row, (int)ctx->board.width,
                              (int)ctx->board.height);
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
            /* a dip in flight is dropped here: waking must not restore a dimmed value */
            const bool dipped = s_dip_phase != DIP_IDLE || s_dip_tail;
            dip_abort();
            s_dip_tail = false;
            ctx->panel_restore_brightness = dipped ? (uint8_t)atomic_load(&s_brightness_shown)
                                                   : atomic_load(&s_brightness_have);
            ctx->panel_touched = true;
            ctx->panel_prepared = false;
        }
        if (atomic_load_explicit(&s_hbm_have, memory_order_relaxed)) {
            if (jarvis_board_set_hbm(false) == ESP_OK) {
                atomic_store_explicit(&s_hbm_have, false, memory_order_release);
            }
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
        s_watch_still = watch_should_rest_still(ctx);
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
        if (__atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE) == 0 &&
            s_ask_ease == 0 && s_shade_ease == 0 && s_detail_ease == 0) {
            apply_surface_overlay(ctx, x1, y1, x2, y2, outbound);
        }
        layer_mark(ctx, 4, &mark);
        }
    }
    render_account(ctx, frame_start, (uint32_t)(esp_timer_get_time() - t0),
                   (uint32_t)(t0 / 1000));
    /* Wave 2: 60 Hz V-blank TE sync right before strip 0 submission on lit
     * AWAKE/AMBIENT frames (strictly inside gfx_render, bounded 18 ms timeout
     * with automatic backoff if GPIO13 is temporarily detached by jr_probe). */
    if (frame_start && jarvis_board_te_enabled() &&
        diag_load(&ctx->init_state) == JR_DISPLAY_INIT_READY &&
        (__atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE) & 3U) == 0U &&
        atomic_load_explicit(&s_brightness_have, memory_order_relaxed) > 0U &&
        jr_display_render_fps() >= JR_DISPLAY_TE_MIN_FPS) {
        const uint32_t te_now_ms = (uint32_t)(t0 / 1000);
        if (s_te_consec_timeouts < JR_DISPLAY_TE_BACKOFF_STREAK ||
            (int32_t)(te_now_ms - s_te_backoff_until_ms) >= 0) {
            atomic_fetch_add_explicit(&s_te_waits, 1U, memory_order_relaxed);
            if (jarvis_board_te_wait(JR_DISPLAY_TE_TIMEOUT_MS)) {
                s_te_consec_timeouts = 0U;
            } else {
                atomic_fetch_add_explicit(&s_te_timeouts, 1U, memory_order_relaxed);
                if (++s_te_consec_timeouts >= JR_DISPLAY_TE_BACKOFF_STREAK) {
                    s_te_backoff_until_ms = te_now_ms + JR_DISPLAY_TE_BACKOFF_MS;
                }
            }
        }
    }
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
    if (x2 == HUD_W && y2 >= HUD_H) {
        dip_frame_done();      /* the last strip of a frame is submitted: the panel dip may end its dark */
    }
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
#ifdef ESP_PLATFORM
    s_dial_minimal = false;
#endif
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
           atomic_load(&s_brightness_have) != want ||
           atomic_load_explicit(&s_hbm_want, memory_order_acquire) !=
               atomic_load_explicit(&s_hbm_have, memory_order_relaxed) ||
           s_dip_phase != DIP_IDLE || s_dip_tail;   /* a dip is a brightness still to settle */
}

/* True when no engine animation is driving the glass and the presenter's own
 * pass has to be the frame clock: a blitted dial, a parked backdrop, or a watch
 * with no baked dial under it. */
/* A WATCH AT REST BY DAY is a still: the glass is redrawn when the minute (or
 * what the rim shows) changes and not otherwise. s_still_key is the minute and
 * rim key of the last frame asked for; it is invalid outside rest, so entering
 * rest always draws once. Presenter task only. */
static bool s_rest_day;
static bool s_still_held;                 /* the engine's anim was stopped for the rest */
static uint32_t s_still_key = UINT32_MAX;
static uint32_t s_still_frames;           /* frames asked for while at rest (host evidence) */

static bool presenter_ticks_the_glass(void)
{
    if (sp_stopwatch_running()) {
        s_still_key = UINT32_MAX;
        return true;
    }
    if (!(s_dial_blit || s_parked || __atomic_load_n(&s_watch_tick, __ATOMIC_RELAXED) != 0u)) {
        return false;
    }
    if (!s_rest_day) {
        s_still_key = UINT32_MAX;
        return true;
    }
    const uint32_t timer_w = atomic_load_explicit(&s_timer_word, memory_order_acquire);
    const uint32_t sw_w = atomic_load_explicit(&s_sw_word, memory_order_acquire);
    const uint32_t sw_ms = atomic_load_explicit(&s_sw_elapsed_ms, memory_order_acquire);
    const uint32_t key =
        (__atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE) & JR_DISPLAY_CLOCK_MINUTE_MASK) ^
        (__atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE) & JR_DISPLAY_ENV_RIM_MASK) * 2654435761u ^
        (timer_w * 1140071481u) ^
        ((sw_w ^ sw_ms) * 2246822519u);
    if (key == s_still_key && !brightness_pending()) {
        return false;
    }
    s_still_key = key;
    s_still_frames++;
    return true;
}

static uint32_t presenter_wait_ms(const jr_display_ctx_t *ctx)
{
    if ((ctx->dream_active || ctx->quiet_active || s_rest_day) &&
        !brightness_pending() && !sp_stopwatch_running()) {
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
        dial_cache_release();       /* the glass shows no dial now: give the PSRAM back */
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

/* A resting dial has nothing to animate, so the engine's own clip must not keep
 * invalidating the glass under it: stop and hide the anim once (a blitted dial
 * already has it hidden), and when the rest ends have the face applied again. */
static void presenter_still_tick(jr_display_ctx_t *ctx, uint32_t *last_word)
{
    if (s_rest_day) {
        if (!s_still_held && ctx->gfx != NULL && ctx->anim != NULL && !s_dial_blit &&
            gfx_emote_lock(ctx->gfx) == ESP_OK) {
            (void)gfx_anim_stop(ctx->anim);
            (void)gfx_obj_set_visible(ctx->anim, false);
            (void)gfx_emote_unlock(ctx->gfx);
            ctx->last_end = -1;
            s_still_held = true;
        } else if (s_dial_blit) {
            s_still_held = true;
        }
    } else if (s_still_held) {
        s_still_held = false;
        *last_word = UINT32_MAX;
        ctx->last_end = -1;
        ctx->apply_retry_gate_ms = 0U;
    }
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
    const uint32_t env = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE) ^
                         (atomic_load_explicit(&s_timer_word, memory_order_acquire) * 1140071481u);
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

/* The slot a baked dial's decoded frame lives in, -1 for anything else. */
static int dial_slot_of(jr_face_t dial)
{
    switch (dial) {
    case JR_FACE_DIAL_DIVER:     return 0;
    case JR_FACE_DIAL_DRESS:     return 1;
    case JR_FACE_DIAL_PILOT:     return 2;
    case JR_FACE_DIAL_FUTURE:    return 3;
    case JR_FACE_DIAL_SKELETON:  return 4;
    case JR_FACE_DIAL_CELESTIAL: return 5;
    case JR_FACE_DIAL_ORRERY:    return 6;
    case JR_FACE_DIAL_REACTOR:   return 7;
    default:                     return -1;
    }
}

/* Publish what is held (frames decoded, bytes allocated) for GET /api/display. */
static void dial_cache_publish(void)
{
    uint32_t n = 0, held = 0;
    for (int i = 0; i < DIAL_SLOTS; ++i) {
        n += s_dial_slot[i].valid ? 1U : 0U;
        held += s_dial_slot[i].px != NULL ? 1U : 0U;
    }
    n += (s_dial_shared != NULL && s_dial_shared_face != JR_FACE_COUNT) ? 1U : 0U;
    held += s_dial_shared != NULL ? 1U : 0U;
#ifdef ESP_PLATFORM
    n += s_minimal_slot.valid ? 1U : 0U;
    held += s_minimal_slot.px != NULL ? 1U : 0U;
#endif
    __atomic_store_n(&s_dial_held_n, n, __ATOMIC_RELAXED);
    __atomic_store_n(&s_dial_held_bytes, (uint32_t)(held * s_dial_frame_bytes),
                     __ATOMIC_RELAXED);
}

static void dial_slot_free(dial_slot_t *slot)
{
    if (slot->px != NULL) {
        if (slot->px == s_dial_frame) {         /* the blit is off: nothing reads it */
            s_dial_frame = NULL;
            s_dial_frame_face = JR_FACE_COUNT;
        }
        heap_caps_free(slot->px);
    }
    *slot = (dial_slot_t){ 0 };
}

/* A frame for slot `keep`: the one it has, or a new one if PSRAM stays above
 * the floor afterwards. With allow_evict a short PSRAM frees the dial used
 * longest ago first, never the one on the glass, the one being decoded into
 * or `keep`, and tries again. NULL means no frame can be had. */
static uint16_t *dial_slot_alloc(size_t bytes, int keep, bool allow_evict)
{
    dial_slot_t *slot = &s_dial_slot[keep];
    if (slot->px != NULL) {
        return slot->px;
    }
    for (;;) {
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= bytes + DIAL_PSRAM_FLOOR) {
            uint16_t *px = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (px != NULL) {
                slot->px = px;
                slot->valid = false;
                dial_cache_publish();
                return px;
            }
        }
        if (!allow_evict) {
            return NULL;
        }
        int victim = -1;
        for (int i = 0; i < DIAL_SLOTS; ++i) {
            const dial_slot_t *s = &s_dial_slot[i];
            if (i == keep || i == s_dial_filling || s->px == NULL ||
                (s_dial_blit && s->px == s_dial_frame)) {
                continue;
            }
            if (victim < 0 || s->used < s_dial_slot[victim].used) {
                victim = i;
            }
        }
        if (victim < 0) {
            return NULL;
        }
        dial_slot_free(&s_dial_slot[victim]);
        __atomic_fetch_add(&s_dial_evictions, 1U, __ATOMIC_RELAXED);
        dial_cache_publish();
    }
}

/* Let go of every decoded frame that is not on the glass (all of them once the
 * blit is off). Presenter only. */
static void dial_cache_release(void)
{
    for (int i = 0; i < DIAL_SLOTS; ++i) {
        dial_slot_t *s = &s_dial_slot[i];
        if (s->px == NULL || i == s_dial_filling || (s_dial_blit && s->px == s_dial_frame)) {
            continue;
        }
        dial_slot_free(s);
    }
    if (s_dial_shared != NULL && !(s_dial_blit && s_dial_shared == s_dial_frame)) {
        if (s_dial_shared == s_dial_frame) {
            s_dial_frame = NULL;
            s_dial_frame_face = JR_FACE_COUNT;
        }
        heap_caps_free(s_dial_shared);
        s_dial_shared = NULL;
        s_dial_shared_face = JR_FACE_COUNT;
    }
#ifdef ESP_PLATFORM
    if (s_minimal_slot.px != NULL && !(s_dial_blit && s_dial_minimal)) {
        if (s_minimal_slot.px == s_dial_frame) {
            s_dial_frame = NULL;
            s_dial_frame_face = JR_FACE_COUNT;
        }
        heap_caps_free(s_minimal_slot.px);
        s_minimal_slot = (dial_slot_t){ 0 };
        s_dial_minimal = false;
    }
#endif
    s_dial_warm_skip = 0;
    dial_cache_publish();
}

/* Open a clip and check frame 0 is a panel-sized 8-bit image; eaf_frame_decode
 * trusts the header, not the buffer size. Any mismatch answers an error. */
static esp_err_t dial_open(const jr_display_ctx_t *ctx, const jr_display_clip_t *clip,
                           eaf_format_handle_t *out)
{
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
    const bool fits = head.width == ctx->board.width &&
                      head.height == ctx->board.height && head.bit_depth == 8;
    eaf_free_header(&head);
    if (!fits) {
        (void)eaf_deinit(eaf);
        return ESP_ERR_INVALID_SIZE;
    }
    *out = eaf;
    return ESP_OK;
}

/* Decode frame 0 of a resident dial clip into that dial's own cache frame.
 * Runs on the presenter and does NOT take the render lock: the target frame is
 * on no one's glass, so the render task keeps drawing the old dial meanwhile.
 * What the decoder shares with the running engine is only the decoder table
 * eaf_init fills once with the same values; eaf_frame_decode allocates its
 * working buffers per call (managed_components/espressif2022__esp_emote_gfx/
 * src/lib/eaf/gfx_eaf_dec.c). The frame is marked valid only after the whole
 * decode succeeded; a failed one gives its memory back. With no frame to be had
 * (PSRAM floor, nothing to evict) a demand decode falls back to the one shared
 * frame, decoded under the render lock because that frame may be on the glass;
 * a warm-up never does. The glass is not touched either way: dial_show switches
 * it. */
static esp_err_t dial_fill(jr_display_ctx_t *ctx, jr_face_t dial,
                           const jr_display_clip_t *clip, bool warm)
{
    const int idx = dial_slot_of(dial);
    if (idx < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t bytes =
        (size_t)ctx->board.width * ctx->board.height * sizeof(uint16_t);
    s_dial_frame_bytes = bytes;
    dial_slot_t *slot = &s_dial_slot[idx];
    uint16_t *dst = dial_slot_alloc(bytes, idx, !warm);
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err;
    if (dst != NULL) {
        slot->valid = false;
        eaf_format_handle_t eaf = NULL;
        err = dial_open(ctx, clip, &eaf);
        if (err == ESP_OK) {
            s_dial_filling = idx;
            memset(dst, 0, bytes);          /* a failed block stays black */
            err = eaf_frame_decode(eaf, 0, (uint8_t *)dst, bytes,
                                   ctx->board.swap_color_bytes);
            (void)eaf_deinit(eaf);
            s_dial_filling = -1;
        }
        if (err == ESP_OK) {
            slot->valid = true;
        } else {
            dial_slot_free(slot);
        }
    } else if (warm) {
        return ESP_ERR_NO_MEM;
    } else {
        if (s_dial_shared == NULL) {
            s_dial_shared = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_dial_shared == NULL) {
                return ESP_ERR_NO_MEM;
            }
        }
        eaf_format_handle_t eaf = NULL;
        err = dial_open(ctx, clip, &eaf);
        if (err == ESP_OK) {
            err = gfx_emote_lock(ctx->gfx);
            if (err == ESP_OK) {
                memset(s_dial_shared, 0, bytes);
                err = eaf_frame_decode(eaf, 0, (uint8_t *)s_dial_shared, bytes,
                                       ctx->board.swap_color_bytes);
                s_dial_shared_face = err == ESP_OK ? dial : JR_FACE_COUNT;
                if (s_dial_frame == s_dial_shared) {      /* the glass just changed under us */
                    s_dial_frame_face = s_dial_shared_face;
                    if (err != ESP_OK) {
                        s_dial_blit = false;
                    }
                }
                (void)gfx_emote_unlock(ctx->gfx);
            }
            (void)eaf_deinit(eaf);
        }
    }
    if (err == ESP_OK) {
        __atomic_store_n(&s_dial_decode_ms,
                         (uint32_t)((esp_timer_get_time() - t0) / 1000), __ATOMIC_RELAXED);
    }
    dial_cache_publish();
    return err;
}

static esp_err_t dial_decode(jr_display_ctx_t *ctx, jr_face_t dial,
                             const jr_display_clip_t *clip)
{
    return dial_fill(ctx, dial, clip, false);
}

/* Release a baked dial after a successful decode or replacement binding.
 * gfx_anim_stop/hide retain the vendor EAF parser's pointers into that source;
 * a successful gfx_anim_set_src releases the old parser before we free it. */
static void clip_release_unbound_dial(jr_display_ctx_t *ctx,
                                      const jr_display_clip_t *clip)
{
    const jr_face_t dials[] = { JR_FACE_DIAL_DIVER, JR_FACE_DIAL_DRESS,
                               JR_FACE_DIAL_PILOT, JR_FACE_DIAL_FUTURE,
                               JR_FACE_DIAL_SKELETON, JR_FACE_DIAL_CELESTIAL,
                               JR_FACE_DIAL_ORRERY, JR_FACE_DIAL_REACTOR };
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
    case JR_FACE_DIAL_CELESTIAL:
    case JR_FACE_DIAL_ORRERY:
    case JR_FACE_DIAL_REACTOR: break;
    default:                  return false;
    }
    if ((ctx->missing_faces & (1U << dial)) != 0U) {
        return false;
    }
    jr_display_clip_t *clip = &ctx->clips[dial];
    const int idx = dial_slot_of(dial);
    dial_slot_t *slot = &s_dial_slot[idx];
    uint16_t *frame = slot->valid ? slot->px
                    : (s_dial_shared_face == dial ? s_dial_shared : NULL);
    if (frame != NULL) {
        if (!(s_dial_blit && s_dial_frame == frame)) {
            __atomic_fetch_add(&s_dial_hits, 1U, __ATOMIC_RELAXED);
        }
    } else {
        __atomic_fetch_add(&s_dial_misses, 1U, __ATOMIC_RELAXED);
        if (clip->data == NULL && clip_load(dial, clip) != ESP_OK) {
            return false;
        }
        esp_err_t err = dial_decode(ctx, dial, clip);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "dial=%d decode failed (%s); anim path",
                     (int)dial, esp_err_to_name(err));
            return false;
        }
        frame = slot->valid ? slot->px : s_dial_shared;
    }
    /* The only lock this takes: switch which frame the strips read. The old
     * dial (or the anim) drew all through the decode above. */
    if (gfx_emote_lock(ctx->gfx) != ESP_OK) {
        return false;                   /* the decoded frame is kept for the retry */
    }
    (void)gfx_anim_stop(ctx->anim);
    (void)gfx_obj_set_visible(ctx->anim, false);
    if (!(s_dial_blit && s_dial_frame == frame)) {
        s_dial_glass_ms = (uint64_t)(esp_timer_get_time() / 1000);
    }
    s_dial_frame = frame;
    s_dial_frame_face = dial;
    s_dial_blit = true;
#ifdef ESP_PLATFORM
    s_dial_minimal = false;
#endif
    gfx_disp_refresh_all(ctx->disp);
    clip_release_unbound_dial(ctx, clip);
    (void)gfx_emote_unlock(ctx->gfx);
    if (slot->valid) {
        slot->used = ++s_dial_clock;
    }

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

#ifdef ESP_PLATFORM
/* Rasterize MINIMAL's static ivory disc + 60 ticks + 12 hour dots once into a
 * PSRAM dial_cache slot (off the render lock), then blit it like a baked dial.
 * Respects DIAL_PSRAM_FLOOR and LRU eviction from s_dial_slot[]; returns false
 * if PSRAM cannot spare a frame, falling back to procedural drawing cleanly. */
static bool dial_show_minimal(jr_display_ctx_t *ctx)
{
    if (ctx == NULL || ctx->board.width != HUD_W || ctx->board.height != HUD_H) {
        return false;
    }
    const bool swap = ctx->board.swap_color_bytes;
    const size_t bytes = (size_t)HUD_W * HUD_H * sizeof(uint16_t);
    s_dial_frame_bytes = bytes;

    if (s_minimal_slot.valid && s_minimal_slot.px != NULL && s_minimal_swap == swap) {
        if (!(s_dial_blit && s_dial_minimal)) {
            __atomic_fetch_add(&s_dial_hits, 1U, __ATOMIC_RELAXED);
        }
    } else {
        __atomic_fetch_add(&s_dial_misses, 1U, __ATOMIC_RELAXED);
        if (s_minimal_slot.px == NULL) {
            for (;;) {
                if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= bytes + DIAL_PSRAM_FLOOR) {
                    uint16_t *px = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (px != NULL) {
                        s_minimal_slot.px = px;
                        s_minimal_slot.valid = false;
                        dial_cache_publish();
                        break;
                    }
                }
                int victim = -1;
                for (int i = 0; i < DIAL_SLOTS; ++i) {
                    const dial_slot_t *s = &s_dial_slot[i];
                    if (i == s_dial_filling || s->px == NULL ||
                        (s_dial_blit && s->px == s_dial_frame)) {
                        continue;
                    }
                    if (victim < 0 || s->used < s_dial_slot[victim].used) {
                        victim = i;
                    }
                }
                if (victim < 0) {
                    return false;
                }
                dial_slot_free(&s_dial_slot[victim]);
                __atomic_fetch_add(&s_dial_evictions, 1U, __ATOMIC_RELAXED);
                dial_cache_publish();
            }
        }
        const int64_t t0 = esp_timer_get_time();
        s_minimal_slot.valid = false;
        memset(s_minimal_slot.px, 0, bytes);
        for (int y = 0; y < HUD_H; y += JR_DISPLAY_STRIP_ROWS) {
            const int nrows = y + JR_DISPLAY_STRIP_ROWS > HUD_H ? HUD_H - y : JR_DISPLAY_STRIP_ROWS;
            hud_minimal_dial_bake_rows(s_minimal_slot.px + (size_t)y * HUD_W, y, nrows, swap);
        }
        s_minimal_swap = swap;
        s_minimal_slot.valid = true;
        __atomic_store_n(&s_dial_decode_ms,
                         (uint32_t)((esp_timer_get_time() - t0) / 1000), __ATOMIC_RELAXED);
        dial_cache_publish();
    }

    if (gfx_emote_lock(ctx->gfx) != ESP_OK) {
        return false;
    }
    (void)gfx_anim_stop(ctx->anim);
    (void)gfx_obj_set_visible(ctx->anim, false);
    if (!(s_dial_blit && s_dial_minimal)) {
        s_dial_glass_ms = (uint64_t)(esp_timer_get_time() / 1000);
    }
    s_dial_frame = s_minimal_slot.px;
    s_dial_frame_face = JR_FACE_COUNT;
    s_dial_blit = true;
    s_dial_minimal = true;
    gfx_disp_refresh_all(ctx->disp);
    (void)gfx_emote_unlock(ctx->gfx);
    s_minimal_slot.used = ++s_dial_clock;

    ctx->active = NULL;
    ctx->last_end = -1;
    __atomic_store_n(&ctx->applied_bucket, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->blanked, false, __ATOMIC_RELAXED);
    diag_store(&ctx->current_asset_bytes, (uint32_t)bytes);
    ESP_LOGI(TAG, "MINIMAL dial baked into PSRAM blit, anim hidden");
    return true;
}
#endif

/* WARM THE NEIGHBOURS. Once a dial has been on the glass for a moment, decode
 * the dials of the next and the previous style (the two a swipe reaches) so the
 * swipe finds them decoded. One decode per call, presenter task, off the render
 * lock, and only while nothing else wants the CPU or the flash: no pending
 * change, no slide, style change or dip under way, no voice session, no OTA, no
 * sleep or blank. It never evicts and never takes the shared fallback frame: a
 * warm-up that cannot get a frame above the PSRAM floor does nothing. A decode
 * that fails frees its frame and is not retried until the cache is released. */
static bool dial_warm_allowed(const jr_display_ctx_t *ctx, uint64_t now_ms,
                              jr_face_t requested_face, bool settled)
{
    if (!settled || !s_dial_blit || dial_slot_of(s_dial_frame_face) < 0 ||
        now_ms - s_dial_glass_ms < DIAL_WARM_SETTLE_MS) {
        return false;
    }
    if (ctx->dream_active || ctx->quiet_active || s_parked || s_rest_day ||
        __atomic_load_n(&ctx->blanked, __ATOMIC_RELAXED) ||
        (__atomic_load_n(&ctx->requested_word, __ATOMIC_ACQUIRE) & JR_DISPLAY_CMD_BLANK) != 0U) {
        return false;
    }
    switch (requested_face) {
    case JR_FACE_LISTENING: case JR_FACE_THINKING:
    case JR_FACE_SPEAKING:  case JR_FACE_LINKING: return false;   /* a voice session */
    default: break;
    }
    const uint32_t ota = __atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE) & 0xFu;
    if (ota == (uint32_t)JR_DISPLAY_OTA_PREFLIGHT || ota == (uint32_t)JR_DISPLAY_OTA_RECEIVING ||
        ota == (uint32_t)JR_DISPLAY_OTA_PROBATION) {
        return false;
    }
    return __atomic_load_n(&s_style_prog, __ATOMIC_RELAXED) >= 256 &&
           __atomic_load_n(&s_space_prog, __ATOMIC_RELAXED) >= 256 &&
           s_dip_mode == DIP_MODE_NONE;
}

static void dial_warm_tick(jr_display_ctx_t *ctx, uint64_t now_ms,
                           jr_face_t requested_face, bool settled)
{
    if (!dial_warm_allowed(ctx, now_ms, requested_face, settled)) {
        return;
    }
    int style = -1;
    for (int st = 0; st < (int)JR_WATCH_STYLE_COUNT; ++st) {
        if (watch_dial_of_style((uint8_t)st) == s_dial_frame_face) {
            style = st;
            break;
        }
    }
    if (style < 0) {
        return;
    }
    const size_t bytes =
        (size_t)ctx->board.width * ctx->board.height * sizeof(uint16_t);
    const int n = (int)JR_WATCH_STYLE_COUNT;
    for (int step = 0; step < 2; ++step) {
        const jr_face_t dial = watch_dial_of_style(
            (uint8_t)((style + n + (step == 0 ? 1 : -1)) % n));
        const int idx = dial_slot_of(dial);
        if (idx < 0 || s_dial_slot[idx].valid || (s_dial_warm_skip & (1U << idx)) != 0U ||
            (ctx->missing_faces & (1U << dial)) != 0U) {
            continue;
        }
        s_dial_frame_bytes = bytes;
        if (dial_slot_alloc(bytes, idx, false) == NULL) {
            return;                         /* under the floor: not now */
        }
        jr_display_clip_t *clip = &ctx->clips[dial];
        esp_err_t err = ESP_OK;
        if (clip->data == NULL) {
            err = clip_load(dial, clip);
        }
        if (err == ESP_OK) {
            err = dial_fill(ctx, dial, clip, true);
        }
        if (err == ESP_OK) {
            __atomic_fetch_add(&s_dial_warms, 1U, __ATOMIC_RELAXED);
            clip_release_unbound_dial(ctx, clip);
        } else {
            s_dial_warm_skip |= (uint8_t)(1U << idx);
            dial_slot_free(&s_dial_slot[idx]);
            dial_cache_publish();
        }
        return;                             /* one per pass */
    }
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
        if (load_err == ESP_ERR_NO_MEM) {
            /* Idle decoded watch dials (~434 KB each) yield PSRAM immediately
             * so live voice clips (IDLE/LISTEN/THINK/SPEAK) never OOM-loop. */
            dial_cache_release();
            load_err = clip_load(shown, clip);
        }
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
    bool left_dial_blit = false;
    err = gfx_anim_set_src(ctx->anim, clip->data, clip->size);
    if (err == ESP_OK) {
        const jr_display_clip_t *previous = ctx->bound;
        ctx->bound = clip;
        ctx->active = clip;
        clip_release_unbound_dial(ctx, previous);
        left_dial_blit = s_dial_blit;
        s_dial_blit = false;            /* the anim is the face again */
#ifdef ESP_PLATFORM
        s_dial_minimal = false;
#endif
    }
    (void)gfx_emote_unlock(ctx->gfx);
    if (left_dial_blit &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < (3584U * 1024U)) {
        /* Leaving WATCH for an animated voice face: release all cached watch
         * dials (including the one just taken off the glass) so voice clips
         * have contiguous PSRAM headroom without hitting memory-critical. */
        dial_cache_release();
    }
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
            s_rest_day = watch_should_rest_still(ctx);

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
#ifdef ESP_PLATFORM
                const bool want_minimal =
                    watch_dial_face() == JR_FACE_COUNT &&
                    __atomic_load_n(&s_minimal_want, __ATOMIC_ACQUIRE) != 0u;
                if (want_minimal != (s_dial_blit && s_dial_minimal)) {
                    changed = true;
                }
#endif
                bool decay_due =
                    (face == JR_FACE_LISTENING || face == JR_FACE_SPEAKING) &&
                    now_ms - ctx->decay_gate_ms >= JR_DISPLAY_DECAY_HOLD_MS;
                if ((changed && now_ms >= ctx->apply_retry_gate_ms) || decay_due) {
                    jr_face_t before = (jr_face_t)diag_load(&ctx->applied_face);
                    bool was_blanked = __atomic_load_n(&ctx->blanked, __ATOMIC_RELAXED);
                    bool stale = false;
#ifdef ESP_PLATFORM
                    err = (want_minimal && dial_show_minimal(ctx))
                        ? ESP_OK
                        : (dial_show(ctx, face)
                               ? ESP_OK
                               : apply_face(ctx, face, requested_face, amplitude, &stale));
#else
                    err = dial_show(ctx, face)
                        ? ESP_OK
                        : apply_face(ctx, face, requested_face, amplitude, &stale);
#endif
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
                        s_still_held = false;       /* the anim may be running again */
                    } else {
                        __atomic_store_n(&ctx->last_error, err, __ATOMIC_RELAXED);
                        ctx->apply_retry_gate_ms = now_ms + 500U;
                        ESP_LOGW(TAG, "face=%d apply failed (retry in 500 ms): %s",
                                 (int)face, esp_err_to_name(err));
                    }
                }
            }
            presenter_still_tick(ctx, &last_word);
            dial_warm_tick(ctx, now_ms, requested_face, word == last_word);

            /* THE BLITTED DIAL HAS NO TIMER. The anim's tick was the only
             * thing invalidating the glass; with it hidden, this pass is the
             * cadence. Every pass, unconditionally, or the hands freeze
             * silently. The engine's fps (the ladder) still paces frames.
             * A parked backdrop is the same case: the anim is hidden too. So is
             * a watch with no baked dial (JARVIS, MINIMAL): its hands followed
             * the face clip's own tick, about 8 fps (s_watch_tick). */
            if (presenter_ticks_the_glass() && gfx_emote_lock(ctx->gfx) == ESP_OK) {
                gfx_disp_refresh_all(ctx->disp);
                (void)gfx_emote_unlock(ctx->gfx);
            }
        }

        if (now_ms - last_diag_ms >= 1000U) {
            last_diag_ms = now_ms;
            diag_store(&ctx->task_stack_hwm, uxTaskGetStackHighWaterMark(NULL));
            const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            diag_store(&ctx->free_psram_bytes, free_psram);
            if (free_psram < DIAL_PSRAM_LOW) {
                dial_cache_release();       /* low memory: the idle dials go first */
            }
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
#ifdef ESP_PLATFORM
    if (__atomic_load_n(&s_holo_explicit, __ATOMIC_RELAXED) == 0u) {
        __atomic_store_n(&s_holo_on, 1u, __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&s_glass3_explicit, __ATOMIC_RELAXED) == 0u) {
        __atomic_store_n(&s_glass3_mode, (uint32_t)JR_GLASS3_AUTO, __ATOMIC_RELEASE);
    }
#endif
    orbit_rec_build();         /* before the render task exists to read it */
    fring_rec_alloc();         /* likewise */
    shade_rec_alloc();         /* likewise */
    wx_rec_alloc();            /* likewise */

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
    {
        const uint32_t tw = __atomic_load_n(&s_tilt_word, __ATOMIC_RELAXED);
        out_diag->tilt_x = jr_tilt_unpack_x(tw);
        out_diag->tilt_y = jr_tilt_unpack_y(tw);
    }
    out_diag->dip = (uint32_t)s_dip;
    out_diag->dip_writes = s_dip_writes;
    out_diag->dip_hold_ms = s_dip_hold_ms;
    out_diag->dial_cached = __atomic_load_n(&s_dial_held_n, __ATOMIC_RELAXED);
    out_diag->dial_bytes = __atomic_load_n(&s_dial_held_bytes, __ATOMIC_RELAXED);
    out_diag->dial_hits = __atomic_load_n(&s_dial_hits, __ATOMIC_RELAXED);
    out_diag->dial_misses = __atomic_load_n(&s_dial_misses, __ATOMIC_RELAXED);
    out_diag->dial_evictions = __atomic_load_n(&s_dial_evictions, __ATOMIC_RELAXED);
    out_diag->dial_warms = __atomic_load_n(&s_dial_warms, __ATOMIC_RELAXED);
    out_diag->dial_decode_ms = __atomic_load_n(&s_dial_decode_ms, __ATOMIC_RELAXED);
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
            style = (arg & 0xFu) << NAV_STYLE_SHIFT;
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
        "SKELETON", "CELESTIAL", "ORRERY", "REACTOR" };
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

void jr_display_timer_set(bool active, bool ringing, uint16_t rem_s,
                          uint8_t pct_left, const char *label)
{
    if (!active) {
        s_timer_label[0] = '\0';
        const uint32_t prev = atomic_exchange_explicit(&s_timer_word, 0u, memory_order_acq_rel);
        if (prev != 0u) {
            nav_wake();
        }
        return;
    }
    if (pct_left > 100U) {
        pct_left = 100U;
    }
    /* Publish sanitized label BEFORE release-storing the timer word (same
     * lock-free contract as s_links_ip and s_caption_text). */
    char clean[TIMER_LABEL_CAP];
    size_t n = 0u;
    if (label != NULL) {
        for (; label[n] != '\0' && n + 1u < TIMER_LABEL_CAP; ++n) {
            unsigned char c = (unsigned char)label[n];
            if (c >= 'a' && c <= 'z') {
                c = (unsigned char)(c - 'a' + 'A');
            }
            clean[n] = (c >= 0x20u && c <= 0x7Eu) ? (char)c : ' ';
        }
    }
    clean[n] = '\0';
    const bool lbl_changed = strncmp(s_timer_label, clean, TIMER_LABEL_CAP) != 0;
    memcpy(s_timer_label, clean, TIMER_LABEL_CAP);

    const uint32_t word =
        ((uint32_t)rem_s & TIMER_REM_MASK) |
        (((uint32_t)pct_left << TIMER_PCT_SHIFT) & TIMER_PCT_MASK) |
        (ringing ? TIMER_RINGING_BIT : 0u) |
        TIMER_ACTIVE_BIT;
    const uint32_t prev = atomic_exchange_explicit(&s_timer_word, word, memory_order_acq_rel);
    if (prev != word || lbl_changed) {
        nav_wake();
    }
}

bool jr_display_timer_active(void)
{
    return (atomic_load_explicit(&s_timer_word, memory_order_acquire) & TIMER_ACTIVE_BIT) != 0u;
}

void jr_display_rim_level_show(bool is_brightness, uint8_t pct)
{
    if (pct > 100U) {
        pct = 100U;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t word =
        ((uint32_t)pct & RIM_LEVEL_PCT_MASK) |
        (is_brightness ? RIM_LEVEL_BRT_BIT : 0u) |
        RIM_LEVEL_ARM_BIT;
    atomic_store_explicit(&s_rim_level_word, word, memory_order_release);
    atomic_store_explicit(&s_rim_level_start_ms, now_ms, memory_order_release);
    nav_wake();
}

void jr_display_stopwatch_set(bool active, bool running, uint32_t elapsed_ms, uint8_t lap_count)
{
    if (!active) {
        atomic_store_explicit(&s_sw_elapsed_ms, 0u, memory_order_relaxed);
        const uint32_t prev = atomic_exchange_explicit(&s_sw_word, 0u, memory_order_acq_rel);
        if (prev != 0u) {
            nav_wake();
        }
        return;
    }
    const uint32_t prev_ms = atomic_exchange_explicit(&s_sw_elapsed_ms, elapsed_ms, memory_order_acq_rel);
    const uint32_t word =
        SW_ACTIVE_BIT |
        (running ? SW_RUNNING_BIT : 0u) |
        (((uint32_t)lap_count << SW_LAP_SHIFT) & SW_LAP_MASK);
    const uint32_t prev_w = atomic_exchange_explicit(&s_sw_word, word, memory_order_acq_rel);
    if (prev_w != word || prev_ms != elapsed_ms) {
        nav_wake();
    }
}
