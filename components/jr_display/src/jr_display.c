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
/* Measured peak 2,496 B; 5120 leaves 2,624 B margin. Was 6144. */
#define JR_DISPLAY_TASK_STACK        5120
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
/* 12-row internal-SRAM DMA strips — the v4-proven shipping config. PSRAM
 * strips (116-row) fail on hardware: spi_master can't DMA from unaligned
 * external RAM and tries a 108 KB internal bounce alloc per flush, which
 * never succeeds (verified live: setup_dma_priv_buffer alloc failures). */
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
    jr_display_clip_t clips[JR_FACE_ERROR + 1];
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
    if (face < JR_FACE_IDLE || face > JR_FACE_ERROR) {
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
    default:                return NULL;
    }
}

static uint32_t face_fps(jr_face_t face)
{
    switch (face) {
    case JR_FACE_ERROR: return 8;
    default:            return 24;   /* engine + panel ceiling (P2.7) */
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

static uint16_t native_darken(uint16_t pixel)
{
    uint16_t r = (pixel >> 11) & 0x1fU;
    uint16_t g = (pixel >> 5) & 0x3fU;
    uint16_t b = pixel & 0x1fU;
    return (uint16_t)(((r >> 2) << 11) | ((g >> 2) << 5) | (b >> 2));
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
            bool card = x >= 52 && x <= 413 && y >= 72 && y <= 388;
            if (!card) {
                native = native_darken(native);
            } else {
                bool border = x < 56 || x > 409 || y < 76 || y > 384;
                native = border ? accent : 0x0842;
                if (y >= 88 && y <= 108 && x >= 178 && x <= 287)
                    native = (x <= 181 || x >= 284 || y <= 91 || y >= 105)
                        ? accent : 0x1084;
                if (surface_text_pixel("CODEX DESK", 10U, x, y, 202, 95, 1) ||
                    surface_text_pixel(surface.title, 24U, x, y, 88, 128, 2) ||
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
 * and the two tilt offsets pre-resolved to signed pixels in bits 16-31. Doing
 * the float trigonometry at push time keeps it out of the render path. */
static volatile uint32_t s_hud_env_word =
    0x000000FFu;   /* battery unknown, level, not charging */
static volatile uint32_t s_hud_enabled = 1u;

/* ---- STATE-05 choice arcs -------------------------------------------------
 * The flush path reads these; the composition root writes them. Guarded by a
 * generation counter rather than a mutex: the flush runs at 16 fps on the
 * render task and must never block, and a torn read is bounded to one frame of
 * a stale arc, which is invisible. Labels are borrowed (see the header). */
static hud_choice_t     s_choices[HUD_CHOICE_MAX];
static volatile int     s_choice_n;
static volatile int     s_choice_selected = -1;

void jr_display_present_choices(const char *const *labels, int n)
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
    memcpy(s_choices, tmp, sizeof tmp);
    s_choice_selected = -1;
    __atomic_store_n(&s_choice_n, n, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "choices: presenting %d arcs", n);
}

void jr_display_dismiss_choices(void)
{
    if (__atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE) == 0) {
        return;                            /* idempotent */
    }
    __atomic_store_n(&s_choice_n, 0, __ATOMIC_RELEASE);
    s_choice_selected = -1;
    ESP_LOGI(TAG, "choices: dismissed");
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
                            float roll_deg, float pitch_deg)
{
    int8_t ox = 0, oy = 0;
    hud_tilt_offset(roll_deg, pitch_deg, &ox, &oy);
    const uint32_t word = (uint32_t)batt_pct
                        | (charging ? (1u << 8) : 0u)
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
    default:                return HUD_FACE_IDLE;
    }
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
        .ox       = (int8_t)((w >> 16) & 0xFFu),
        .oy       = (int8_t)((w >> 24) & 0xFFu),
    };
    hud_overlay_frame(pixels, y1, nrows,
                      (uint32_t)(esp_timer_get_time() / 1000),
                      ctx->board.swap_color_bytes, &env);

    /* Choice arcs draw LAST: they are the interactive layer and must sit above
     * the face and the HUD accents. Same strip contract, same buffer. */
    const int cn = __atomic_load_n(&s_choice_n, __ATOMIC_ACQUIRE);
    if (cn > 0) {
        hud_overlay_choices(pixels, y1, nrows, ctx->board.swap_color_bytes,
                            s_choices, cn, s_choice_selected);
    }
}

static void apply_shell_overlay(jr_display_ctx_t *ctx, int x1, int y1,
                                int x2, int y2, uint16_t *pixels)
{
    uint32_t shell = diag_load(&ctx->shell_word);
    bool shade_open = (shell & JR_DISPLAY_SHELL_SHADE) != 0U;
    bool agent_active = (shell & JR_DISPLAY_SHELL_AGENT) != 0U;
    if ((!shade_open && !agent_active) || pixels == NULL) {
        return;
    }
    uint8_t progress = shell & JR_DISPLAY_SHELL_PROGRESS;
    jr_display_agent_state_t agent_state = (jr_display_agent_state_t)
        ((shell & JR_DISPLAY_SHELL_STATE_MASK) >>
         JR_DISPLAY_SHELL_STATE_SHIFT);
    uint16_t accent = agent_native_color(agent_state);
    int completed_segments = progress == 0U
        ? 0 : ((int)progress * 8 + 99) / 100;
    int width = x2 - x1;
    int height = y2 - y1;

    for (int row = 0; row < height; ++row) {
        int y = y1 + row;
        for (int col = 0; col < width; ++col) {
            int x = x1 + col;
            uint16_t *slot = &pixels[(size_t)row * (size_t)width + (size_t)col];
            uint16_t native = panel_native(ctx, *slot);

            if (shade_open && y < 194) {
                native = native_darken(native);
                /* Pull handle. */
                if (y >= 14 && y <= 18 && x >= 203 && x <= 262) {
                    native = 0x7DFF;
                }
                /* Three radial quick-action controls: voice, lab, Agent Link. */
                static const int centers[3] = {145, 233, 321};
                for (int i = 0; i < 3; ++i) {
                    int dx = x - centers[i];
                    int dy = y - 104;
                    int d2 = dx * dx + dy * dy;
                    if (d2 >= 21 * 21 && d2 <= 27 * 27) {
                        native = i == 2 && agent_active ? accent : 0x07FF;
                    } else if (d2 < 8 * 8) {
                        native = i == 0 ? 0x07FF : i == 1 ? 0xFD20 : accent;
                    }
                }
                if (y >= 166 && y <= 169 && x >= 92 && x <= 373) {
                    native = 0x18C3;
                }
                if (agent_active && y >= 178 && y <= 184 &&
                    x >= 112 && x <= 353) {
                    int filled = 112 + ((int)progress * 241) / 100;
                    native = x <= filled ? accent : 0x2104;
                }
            }

            if (agent_active && completed_segments > 0) {
                int dx = 2 * x - ((int)ctx->board.width - 1);
                int dy = 2 * y - ((int)ctx->board.height - 1);
                int r2 = dx * dx + dy * dy;
                if (r2 >= (2 * 224) * (2 * 224) &&
                    r2 <= (2 * 230) * (2 * 230)) {
                    int sector = challenge_sector_from_vector(dx, dy);
                    if (sector < completed_segments) {
                        native = accent;
                    }
                }
            }
            *slot = panel_order_color(ctx, native);
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

    /* The gfx-owned DMA buffer is mutable until this submission. Diagnostics
     * may substitute a known pattern, then the mirror records the exact bytes
     * that will be handed to the CO5300—not the pre-render intent. */
    uint16_t *outbound = (uint16_t *)pixels;
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

static esp_err_t apply_face(jr_display_ctx_t *ctx, jr_face_t face, uint8_t amplitude)
{
    uint8_t bucket = (uint8_t)(((uint32_t)amplitude * JR_DISPLAY_AMP_BUCKETS + 127U) /
                               255U);
    if (bucket > JR_DISPLAY_AMP_BUCKETS) {
        bucket = JR_DISPLAY_AMP_BUCKETS;
    }

    if (ctx->active != NULL && ctx->loaded_face == face &&
            !__atomic_load_n(&ctx->blanked, __ATOMIC_RELAXED)) {
        return program_segment(ctx, face, bucket, false);
    }

    if (face > JR_FACE_ERROR) {         /* defensive: cb clamps already */
        face = JR_FACE_ERROR;
    }
    jr_display_clip_t *clip = &ctx->clips[face];
    if (clip->data == NULL) {
        esp_err_t load_err = clip_load(face, clip);
        if (load_err != ESP_OK) {
            diag_inc(&ctx->asset_load_failures);
            return load_err;
        }
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
                    err = apply_face(ctx, face, amplitude);
                    if (err == ESP_OK) {
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
