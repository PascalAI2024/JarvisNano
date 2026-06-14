/*
 * SPDX-FileCopyrightText: 2026 JarvisRobot
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_layer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_board_manager_includes.h"

#include "display_arbiter.h"
#include "display_hal.h"

static const char *TAG = "ui_layer";

/* -------------------------------------------------------------------------
 * Geometry — ported from docs/prototype/jarvisnano-os.html drawChoices()/
 * pointerdown hit-test. Logical canvas is 466x466, centre (233,233).
 * fill_arc/draw_arc take DEGREES; the prototype works in radians, so the math
 * is done in radians and converted at the HAL boundary.
 * ---------------------------------------------------------------------- */
#define UI_W            466
#define UI_H            466
#define UI_CX           233
#define UI_CY           233
#define UI_SAFE_R       233      /* clip radius for the round bezel (r<=233) */

#define UI_ARC_INNER    118      /* annular wedge inner radius (thicker = bolder, more button-like) */
#define UI_ARC_OUTER    188      /* annular wedge outer radius (still well inside r<=233 bezel) */
#define UI_ARC_EDGE_R   122      /* bright "tappable" inner edge ring radius */
#define UI_ARC_LABEL_R  153      /* label centre radius (mid of the 118..188 wedge band) */
#define UI_TAP_DEADZONE_R 50     /* taps within this radius = centre/hint, ignored; outside = matched by angle (touch reads ~15% compressed, so don't require the exact rim radius) */
#define UI_ARC_TOPGAP   1.20f    /* radians reserved at 12 o'clock for question */
#define UI_ARC_GAP      0.08f    /* radians between adjacent wedges */
#define UI_TAU          6.28318530717958647692f
#define UI_HALF_PI      1.57079632679489661923f
#define UI_RAD2DEG      57.2957795130823208768f

/* Menu ring radius. */
#define UI_MENU_R       150
#define UI_MENU_DOT_R   14
#define UI_MENU_HIT_R   30       /* tap within this of a dot selects it */

/* Largest JPEG accepted by ui_layer_show_image(path); larger files are rejected
 * (an error placeholder is shown) so a bad/huge path can't exhaust PSRAM. */
#define UI_IMG_MAX_BYTES (512 * 1024)

/* Colours (RGB565). The HAL handles the CO5300 byte swap internally. */
#define C565(r, g, b)   ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define UI_COL_BG       C565(0x00, 0x00, 0x00)   /* true-black AMOLED ground */
#define UI_COL_TRACK    C565(0x16, 0x20, 0x30)   /* wedge track halo (visible rim) */
#define UI_COL_WEDGE    C565(0x2B, 0x4F, 0x7E)   /* idle wedge fill — brighter HUD blue */
#define UI_COL_EDGE     C565(0x6E, 0xB9, 0xFF)   /* cyan inner edge = "tappable" cue */
#define UI_COL_ACCENT   C565(0xFF, 0x6A, 0x1F)   /* J.A.R.V.I.S. amber (selected) */
#define UI_COL_ACCENT_D C565(0x1A, 0x0C, 0x03)   /* dark text on amber */
#define UI_COL_WHITE    C565(0xFF, 0xFF, 0xFF)
#define UI_COL_TEXT     C565(0xEA, 0xEE, 0xF4)
#define UI_COL_DIM      C565(0x9A, 0xA3, 0xB2)
#define UI_COL_CARD     C565(0x14, 0x17, 0x1F)
#define UI_COL_CARD_EDG C565(0x27, 0x2C, 0x38)

/* Fonts: the build seeds esp_painter basic fonts 24/28/32/40/48 (see
 * apply_ui_fonts_seed_patch in bootstrap.sh). The HAL falls back to 24 for any
 * size not compiled in, so these stay safe even on a minimal font set. Sizes
 * chosen for the 466x466 panel: 40px questions, 32px wedge labels, 28px hints. */
#define UI_FONT_TITLE   40
#define UI_FONT_LABEL   32
#define UI_FONT_SMALL   28
#define UI_FONT_ROW     24   /* data-panel rows: dense enough for label+value on one line */

/* -------------------------------------------------------------------------
 * Command queue model — public API posts these; the UI task executes them.
 * ---------------------------------------------------------------------- */
typedef enum {
    UI_CMD_SHOW_CHOICE = 0,
    UI_CMD_SHOW_DATA,
    UI_CMD_SHOW_IMAGE,
    UI_CMD_SHOW_MENU,
    UI_CMD_HIGHLIGHT, /* re-render with one option highlighted (tap feedback) */
    UI_CMD_DISMISS,
} ui_cmd_kind_t;

#define UI_STR_MAX     48   /* per option/label/value/title/question */
#define UI_PATH_MAX    96

typedef struct {
    ui_cmd_kind_t kind;
    /* choice / menu / data text */
    char question[UI_STR_MAX];
    char opts[UI_LAYER_MAX_OPTIONS][UI_STR_MAX];
    char vals[UI_LAYER_MAX_ROWS][UI_STR_MAX];
    int n;
    int highlight;       /* index to highlight for UI_CMD_HIGHLIGHT */
    /* image */
    uint8_t *jpeg;       /* heap-owned copy; UI task frees after render */
    size_t jpeg_len;
    char path[UI_PATH_MAX];
} ui_cmd_t;

/* -------------------------------------------------------------------------
 * Scene state — owned and mutated ONLY by the UI task, except the result
 * fields, which are guarded by s_result_mx so pollers/waiters are safe.
 * ---------------------------------------------------------------------- */
typedef struct {
    float a0;  /* wedge start angle, radians (raw, may be negative) */
    float a1;  /* wedge end angle, radians */
} ui_wedge_t;

static QueueHandle_t s_cmd_q;
static TaskHandle_t  s_ui_task;
static SemaphoreHandle_t s_result_mx;     /* guards result + active + wedges */
static SemaphoreHandle_t s_result_sig;    /* binary: a tap resolved a result */
static bool s_inited;

/* Board panel handles (grabbed once in ui_layer_init). */
static esp_lcd_panel_handle_t    s_panel_handle;
static esp_lcd_panel_io_handle_t s_io_handle;
static int s_lcd_width  = UI_W;
static int s_lcd_height = UI_H;
static display_hal_panel_if_t s_panel_if = DISPLAY_HAL_PANEL_IF_IO;

/* Live scene model (read by ui_layer_on_tap / _is_active under s_result_mx). */
static volatile ui_scene_t s_scene = UI_SCENE_NONE;
static int        s_opt_count;
static ui_wedge_t s_wedges[UI_LAYER_MAX_OPTIONS];   /* CHOICE_ARCS hit regions */
static int        s_result_index = -1;
static bool       s_result_done;

/* True while the display_hal is created + ui_layer owns the arbiter. Only the
 * UI task writes this; readers use it to await handoff. */
static bool s_hal_up;

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */
static inline float ui_norm_angle(float a)
{
    /* wrap into [0, TAU) */
    a = fmodf(a, UI_TAU);
    if (a < 0.0f) {
        a += UI_TAU;
    }
    return a;
}

static void ui_result_reset(void)
{
    if (s_result_mx && xSemaphoreTake(s_result_mx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_result_index = -1;
        s_result_done = false;
        xSemaphoreGive(s_result_mx);
    }
    /* drain any stale signal so a new scene starts clean */
    if (s_result_sig) {
        xSemaphoreTake(s_result_sig, 0);
    }
}

static void ui_safe_strcpy(char *dst, const char *src, size_t cap)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, cap);
}

/* -------------------------------------------------------------------------
 * Board handle grab — mirrors firmware/emote/emote.c:391-410. Also derives the
 * panel interface from the board sub_type so display_hal_create gets it right.
 * ---------------------------------------------------------------------- */
static display_hal_panel_if_t ui_panel_if_from_subtype(const char *sub_type)
{
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_DSI_SUPPORT
    if (sub_type && (strcmp(sub_type, "dsi") == 0 || strcmp(sub_type, "mipi_dsi") == 0)) {
        return DISPLAY_HAL_PANEL_IF_MIPI_DSI;
    }
#endif
    if (sub_type && (strcmp(sub_type, "rgb") == 0 || strcmp(sub_type, "rgb_3wire_spi") == 0)) {
        return DISPLAY_HAL_PANEL_IF_RGB;
    }
    /* spi / qspi / parlio all flush via esp_lcd_panel_io */
    return DISPLAY_HAL_PANEL_IF_IO;
}

static esp_err_t ui_grab_board_display(void)
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

    dev_display_lcd_handles_t *handles = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t  *cfg     = (dev_display_lcd_config_t *)lcd_config;

    ESP_RETURN_ON_FALSE(handles && cfg && handles->panel_handle, ESP_ERR_INVALID_STATE, TAG,
                        "display_lcd handle/config is NULL");

    s_panel_handle = handles->panel_handle;
    s_io_handle    = handles->io_handle;
    s_lcd_width    = cfg->lcd_width  ? cfg->lcd_width  : UI_W;
    s_lcd_height   = cfg->lcd_height ? cfg->lcd_height : UI_H;
    s_panel_if     = ui_panel_if_from_subtype(cfg->sub_type);
    return ESP_OK;
#endif
}

/* -------------------------------------------------------------------------
 * display_hal lifecycle on the UI task. Acquire LUA (reused for v1: it already
 * short-circuits the emote flush + preempts the EMOTE owner via lua_depth), then
 * create the HAL framebuffers. Await any in-flight emote flush BEFORE the first
 * present so the handoff frame is not torn.
 * ---------------------------------------------------------------------- */
static esp_err_t ui_hal_up(void)
{
    if (s_hal_up) {
        return ESP_OK;
    }
    if (!s_panel_handle) {
        esp_err_t ge = ui_grab_board_display();
        if (ge != ESP_OK) {
            ESP_LOGE(TAG, "no display handle: %s", esp_err_to_name(ge));
            return ge;
        }
    }

    esp_err_t err = display_arbiter_acquire(DISPLAY_ARBITER_OWNER_LUA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "arbiter acquire failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Let the emote engine observe the ownership change and quiesce its flush
     * (its flush callback early-returns when it is no longer the EMOTE owner).
     * One emote frame interval (~42 ms @ 24 fps) is plenty to drain an in-flight
     * strip flush before we create our own framebuffers and present. */
    vTaskDelay(pdMS_TO_TICKS(60));

    err = display_hal_create(s_panel_handle, s_io_handle, s_panel_if, s_lcd_width, s_lcd_height);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display_hal_create failed: %s", esp_err_to_name(err));
        display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
        return err;
    }
    s_hal_up = true;
    return ESP_OK;
}

static void ui_hal_down(void)
{
    if (!s_hal_up) {
        /* Still ensure we are not silently holding the arbiter. */
        if (display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_LUA)) {
            display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
        }
        return;
    }

    /* Finish any frame we may have left open, then await the flush to complete
     * before tearing down the framebuffers (both sides own flush-done
     * semaphores — destroying buffers under an in-flight DMA would tear). */
    if (display_hal_is_frame_active()) {
        display_hal_end_frame();
    }
    display_hal_animation_info_t info = {0};
    for (int i = 0; i < 10; ++i) {
        if (display_hal_get_animation_info(&info) != ESP_OK || !info.flush_in_flight) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    display_hal_destroy();
    s_hal_up = false;

    /* Release LUA -> arbiter restores EMOTE -> emote owner-changed cb repaints
     * the live face. */
    display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
}

/* -------------------------------------------------------------------------
 * Renderers (all run on the UI task while s_hal_up).
 * ---------------------------------------------------------------------- */
static void ui_clip_round(void)
{
    /* The HAL has no circular clip; a square clip to the inscribed bounds keeps
     * all draws inside the panel. The round bezel itself masks r>233 physically,
     * and every scene element is laid out inside r<=233. */
    display_hal_set_clip_rect(0, 0, s_lcd_width, s_lcd_height);
}

static void ui_render_choice(const ui_cmd_t *c, int highlight)
{
    const int n = c->n;
    display_hal_begin_frame(true, UI_COL_BG);
    ui_clip_round();

    /* Question at 12 o'clock. */
    if (c->question[0]) {
        display_hal_draw_text_aligned(0, 30, s_lcd_width, 52, c->question, UI_FONT_TITLE,
                                      UI_COL_WHITE, false, 0,
                                      DISPLAY_HAL_TEXT_ALIGN_CENTER,
                                      DISPLAY_HAL_TEXT_VALIGN_MIDDLE);
    }

    const float avail = UI_TAU - UI_ARC_TOPGAP;
    const float span  = (avail - UI_ARC_GAP * (float)n) / (float)n;

    ui_wedge_t local[UI_LAYER_MAX_OPTIONS];

    for (int i = 0; i < n; ++i) {
        const float a0 = -UI_HALF_PI + UI_ARC_TOPGAP * 0.5f + (float)i * (span + UI_ARC_GAP);
        const float a1 = a0 + span;
        const bool  hot = (i == highlight);
        local[i].a0 = a0;
        local[i].a1 = a1;

        const float d0 = a0 * UI_RAD2DEG;
        const float d1 = a1 * UI_RAD2DEG;

        /* track halo (full band) + fill (slightly inset) */
        display_hal_fill_arc(UI_CX, UI_CY, UI_ARC_INNER - 5, UI_ARC_OUTER + 5, d0, d1, UI_COL_TRACK);
        display_hal_fill_arc(UI_CX, UI_CY, UI_ARC_INNER, UI_ARC_OUTER, d0, d1,
                             hot ? UI_COL_ACCENT : UI_COL_WEDGE);
        /* bright inner edge = "this is tappable" (2px for a crisper rim) */
        display_hal_draw_arc(UI_CX, UI_CY, UI_ARC_EDGE_R, d0, d1,
                             hot ? UI_COL_WHITE : UI_COL_EDGE);
        display_hal_draw_arc(UI_CX, UI_CY, UI_ARC_EDGE_R + 1, d0, d1,
                             hot ? UI_COL_WHITE : UI_COL_EDGE);

        /* centred label at the wedge mid-angle */
        const float mid = (a0 + a1) * 0.5f;
        const int lx = UI_CX + (int)lroundf(cosf(mid) * (float)UI_ARC_LABEL_R);
        const int ly = UI_CY + (int)lroundf(sinf(mid) * (float)UI_ARC_LABEL_R);
        /* a label box centred on (lx,ly), wide enough for a 32px word */
        display_hal_draw_text_aligned(lx - 90, ly - 22, 180, 44, c->opts[i], UI_FONT_LABEL,
                                      hot ? UI_COL_ACCENT_D : UI_COL_WHITE, false, 0,
                                      DISPLAY_HAL_TEXT_ALIGN_CENTER,
                                      DISPLAY_HAL_TEXT_VALIGN_MIDDLE);
    }

    /* centre hint + reactor dot */
    display_hal_fill_circle(UI_CX, UI_CY, 11, UI_COL_EDGE);
    display_hal_fill_circle(UI_CX, UI_CY, 6, UI_COL_WHITE);
    display_hal_draw_text_aligned(0, UI_CY + 26, s_lcd_width, 32, "tap to answer", UI_FONT_SMALL,
                                  UI_COL_DIM, false, 0,
                                  DISPLAY_HAL_TEXT_ALIGN_CENTER,
                                  DISPLAY_HAL_TEXT_VALIGN_MIDDLE);

    display_hal_clear_clip_rect();
    display_hal_present();

    /* publish hit regions for ui_layer_on_tap */
    if (s_result_mx && xSemaphoreTake(s_result_mx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_opt_count = n;
        for (int i = 0; i < n; ++i) {
            s_wedges[i] = local[i];
        }
        s_scene = UI_SCENE_CHOICE_ARCS;
        xSemaphoreGive(s_result_mx);
    }
}

static void ui_render_data(const ui_cmd_t *c)
{
    const int n = c->n;
    display_hal_begin_frame(true, UI_COL_BG);
    ui_clip_round();

    /* title at the top */
    if (c->question[0]) {
        display_hal_draw_text_aligned(0, 40, s_lcd_width, 52, c->question, UI_FONT_TITLE,
                                      UI_COL_WHITE, false, 0,
                                      DISPLAY_HAL_TEXT_ALIGN_CENTER,
                                      DISPLAY_HAL_TEXT_VALIGN_MIDDLE);
    }

    /* a central round card holding the rows */
    const int card_w = 330, card_h = 210;
    const int card_x = UI_CX - card_w / 2;
    const int card_y = UI_CY - card_h / 2 + 8;
    display_hal_fill_round_rect(card_x, card_y, card_w, card_h, 22, UI_COL_CARD);
    display_hal_draw_round_rect(card_x, card_y, card_w, card_h, 22, UI_COL_CARD_EDG);

    /* Each row spans the FULL card width: label left-aligned, value right-aligned
     * in the same band. The two-column (half/half) split clipped long values
     * (a 13-char IP can't fit half a card) and collided with the label once the
     * font grew. Transparent text means the two ends never stamp boxes over each
     * other; only genuinely over-long labels+values could meet in the middle. */
    const int row_h = (n > 0) ? (card_h - 24) / n : card_h;
    for (int i = 0; i < n; ++i) {
        const int ry = card_y + 12 + i * row_h;
        display_hal_draw_text_aligned(card_x + 18, ry, card_w - 36, row_h,
                                      c->opts[i], UI_FONT_ROW, UI_COL_DIM, false, 0,
                                      DISPLAY_HAL_TEXT_ALIGN_LEFT,
                                      DISPLAY_HAL_TEXT_VALIGN_MIDDLE);
        display_hal_draw_text_aligned(card_x + 18, ry, card_w - 36, row_h,
                                      c->vals[i], UI_FONT_ROW, UI_COL_TEXT, false, 0,
                                      DISPLAY_HAL_TEXT_ALIGN_RIGHT,
                                      DISPLAY_HAL_TEXT_VALIGN_MIDDLE);
    }

    display_hal_clear_clip_rect();
    display_hal_present();

    if (s_result_mx && xSemaphoreTake(s_result_mx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_opt_count = 0;
        s_scene = UI_SCENE_DATA_PANEL;
        xSemaphoreGive(s_result_mx);
    }
}

/* Load a JPEG from the filesystem (storage mounts under /sdcard) into a PSRAM
 * buffer for ui_layer_show_image(path). Returns a heap buffer the caller must
 * free, or NULL on any error (open / size-out-of-range / OOM / short read);
 * *out_len gets the byte count. Runs on the UI task (the only ui_render_image
 * caller), so the blocking file read never touches the render/voice tasks. */
static uint8_t *ui_load_jpeg_file(const char *path, size_t *out_len)
{
    *out_len = 0;
    if (!path || !path[0]) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "image: cannot open '%s'", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > UI_IMG_MAX_BYTES) {
        ESP_LOGW(TAG, "image: '%s' size %ld out of range (1..%d)", path, sz, UI_IMG_MAX_BYTES);
        fclose(f);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_DEFAULT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "image: OOM for %ld-byte file '%s'", sz, path);
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        ESP_LOGW(TAG, "image: short read %u/%ld on '%s'", (unsigned)rd, sz, path);
        heap_caps_free(buf);
        return NULL;
    }
    *out_len = (size_t)sz;
    return buf;
}

static void ui_render_image(const ui_cmd_t *c)
{
    display_hal_begin_frame(true, UI_COL_BG);
    ui_clip_round();

    /* Source the JPEG from the in-memory buffer if supplied, otherwise load it
     * from c->path (the POST /api/ui/image {"path":...} and
     * ui_layer_show_image(NULL,0,path) route). */
    const uint8_t *jpeg = c->jpeg;
    size_t jlen = c->jpeg_len;
    uint8_t *loaded = NULL;
    bool path_requested = (!jpeg || !jlen) && c->path[0];
    if (path_requested) {
        loaded = ui_load_jpeg_file(c->path, &jlen);
        jpeg = loaded;
    }

    if (jpeg && jlen) {
        int sw = 0, sh = 0;
        /* probe native size, then fit-scale to ~the inscribed square (≈ 1:1 if it
         * already fits). The HAL scaler takes integer scale_w/scale_h in 1/100. */
        int nat_w = 0, nat_h = 0;
        int scale = 100;
        if (display_hal_jpeg_get_size(jpeg, jlen, &nat_w, &nat_h) == ESP_OK &&
            nat_w > 0 && nat_h > 0) {
            const int fit = (UI_SAFE_R * 2 * 100) / (nat_w > nat_h ? nat_w : nat_h);
            scale = (fit > 0 && fit < 100) ? fit : 100;
        }
        const int draw_w = (nat_w > 0) ? nat_w * scale / 100 : UI_W;
        const int draw_h = (nat_h > 0) ? nat_h * scale / 100 : UI_H;
        const int px = UI_CX - draw_w / 2;
        const int py = UI_CY - draw_h / 2;
        display_hal_draw_jpeg_scaled(px, py, jpeg, jlen, scale, scale, &sw, &sh);
    } else {
        /* Distinguish "nothing supplied" from "the path failed to load". */
        const char *msg = path_requested ? "image load failed" : "no image";
        display_hal_draw_text_aligned(0, UI_CY - 12, s_lcd_width, 24, msg, UI_FONT_LABEL,
                                      UI_COL_DIM, false, 0,
                                      DISPLAY_HAL_TEXT_ALIGN_CENTER,
                                      DISPLAY_HAL_TEXT_VALIGN_MIDDLE);
    }

    display_hal_clear_clip_rect();
    display_hal_present();

    /* Free the file buffer after present() (per the render contract); the decode
     * already copied pixels into the framebuffer during draw_jpeg_scaled. */
    if (loaded) {
        heap_caps_free(loaded);
    }

    if (s_result_mx && xSemaphoreTake(s_result_mx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_opt_count = 0;
        s_scene = UI_SCENE_IMAGE;
        xSemaphoreGive(s_result_mx);
    }
}

static void ui_render_menu(const ui_cmd_t *c, int highlight)
{
    const int n = c->n;
    display_hal_begin_frame(true, UI_COL_BG);
    ui_clip_round();

    /* faint guide ring */
    display_hal_draw_circle(UI_CX, UI_CY, UI_MENU_R, UI_COL_CARD_EDG);

    ui_wedge_t local[UI_LAYER_MAX_OPTIONS];

    for (int i = 0; i < n; ++i) {
        /* items evenly distributed starting at 12 o'clock, clockwise */
        const float a = -UI_HALF_PI + (float)i / (float)n * UI_TAU;
        const int x = UI_CX + (int)lroundf(cosf(a) * (float)UI_MENU_R);
        const int y = UI_CY + (int)lroundf(sinf(a) * (float)UI_MENU_R);
        const bool sel = (i == highlight);
        display_hal_fill_circle(x, y, sel ? UI_MENU_DOT_R + 2 : UI_MENU_DOT_R - 2,
                                sel ? UI_COL_ACCENT : UI_COL_WEDGE);
        display_hal_draw_text_aligned(x - 36, y - 12, 72, 24, c->opts[i], UI_FONT_SMALL,
                                      sel ? UI_COL_ACCENT_D : UI_COL_TEXT, false, 0,
                                      DISPLAY_HAL_TEXT_ALIGN_CENTER,
                                      DISPLAY_HAL_TEXT_VALIGN_MIDDLE);
        /* store the dot centre as a tiny [a0,a1] proxy is not enough; the menu
         * hit-test uses circular distance to each dot. Reuse the wedge slot to
         * stash the dot angle by encoding both endpoints == a (point marker). */
        local[i].a0 = a;
        local[i].a1 = a;
    }

    /* selected item label in the centre */
    if (highlight >= 0 && highlight < n) {
        display_hal_draw_text_aligned(0, UI_CY - 12, s_lcd_width, 24, c->opts[highlight],
                                      UI_FONT_TITLE, UI_COL_WHITE, false, 0,
                                      DISPLAY_HAL_TEXT_ALIGN_CENTER,
                                      DISPLAY_HAL_TEXT_VALIGN_MIDDLE);
    }

    display_hal_clear_clip_rect();
    display_hal_present();

    if (s_result_mx && xSemaphoreTake(s_result_mx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_opt_count = n;
        for (int i = 0; i < n; ++i) {
            s_wedges[i] = local[i];
        }
        s_scene = UI_SCENE_RADIAL_MENU;
        xSemaphoreGive(s_result_mx);
    }
}

/* Cache of the last shown command so a HIGHLIGHT re-render can reuse the text. */
static ui_cmd_t s_last_cmd;

/* -------------------------------------------------------------------------
 * The dedicated UI task — the single owner of display_hal.
 * ---------------------------------------------------------------------- */
static void ui_task(void *arg)
{
    (void)arg;
    ui_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (cmd.kind) {
        case UI_CMD_SHOW_CHOICE:
            if (ui_hal_up() == ESP_OK) {
                s_last_cmd = cmd;
                ui_render_choice(&cmd, -1);
            }
            break;
        case UI_CMD_SHOW_DATA:
            if (ui_hal_up() == ESP_OK) {
                s_last_cmd = cmd;
                ui_render_data(&cmd);
            }
            break;
        case UI_CMD_SHOW_IMAGE:
            if (ui_hal_up() == ESP_OK) {
                s_last_cmd = cmd;
                ui_render_image(&cmd);
            }
            if (cmd.jpeg) {
                free(cmd.jpeg);
                cmd.jpeg = NULL;
            }
            break;
        case UI_CMD_SHOW_MENU:
            if (ui_hal_up() == ESP_OK) {
                s_last_cmd = cmd;
                ui_render_menu(&cmd, 0);
            }
            break;
        case UI_CMD_HIGHLIGHT:
            /* re-render the live scene with one option lit; uses the cached cmd */
            if (s_hal_up) {
                if (s_last_cmd.kind == UI_CMD_SHOW_CHOICE) {
                    ui_render_choice(&s_last_cmd, cmd.highlight);
                } else if (s_last_cmd.kind == UI_CMD_SHOW_MENU) {
                    ui_render_menu(&s_last_cmd, cmd.highlight);
                }
            }
            break;
        case UI_CMD_DISMISS:
            /* mark inactive BEFORE tearing the HAL so taps stop routing here */
            if (s_result_mx && xSemaphoreTake(s_result_mx, pdMS_TO_TICKS(50)) == pdTRUE) {
                s_scene = UI_SCENE_NONE;
                s_opt_count = 0;
                xSemaphoreGive(s_result_mx);
            }
            ui_hal_down();
            memset(&s_last_cmd, 0, sizeof(s_last_cmd));
            break;
        default:
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Command posting
 * ---------------------------------------------------------------------- */
static esp_err_t ui_post(const ui_cmd_t *cmd)
{
    if (!s_inited || !s_cmd_q) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xQueueSend(s_cmd_q, cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
esp_err_t ui_layer_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* Grab the board display handles up front (the HAL is created lazily on the
     * first show, but resolving the handle now surfaces config errors early). */
    esp_err_t err = ui_grab_board_display();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ui_layer_init: display handle grab failed: %s", esp_err_to_name(err));
        return err;
    }

    s_result_mx  = xSemaphoreCreateMutex();
    s_result_sig = xSemaphoreCreateBinary();
    s_cmd_q      = xQueueCreate(6, sizeof(ui_cmd_t));
    if (!s_result_mx || !s_result_sig || !s_cmd_q) {
        ESP_LOGE(TAG, "ui_layer_init: alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* Priority 3 matches the emote render task; stack 6 KB covers the JPEG
     * decode call frames + esp_painter. Pin nowhere (-1) like the emote task. */
    BaseType_t ok = xTaskCreate(ui_task, "ui_layer", 6 * 1024, NULL, 3, &s_ui_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ui_layer_init: task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "ui_layer ready (%dx%d, panel_if=%d)", s_lcd_width, s_lcd_height, (int)s_panel_if);
    return ESP_OK;
}

esp_err_t ui_layer_show_choice(const char *question, const char *const *opts, int n)
{
    if (n < 2 || n > UI_LAYER_MAX_OPTIONS || !opts) {
        return ESP_ERR_INVALID_ARG;
    }
    ui_result_reset();

    ui_cmd_t cmd = {0};
    cmd.kind = UI_CMD_SHOW_CHOICE;
    cmd.n = n;
    ui_safe_strcpy(cmd.question, question, sizeof(cmd.question));
    for (int i = 0; i < n; ++i) {
        ui_safe_strcpy(cmd.opts[i], opts[i], sizeof(cmd.opts[i]));
    }
    return ui_post(&cmd);
}

esp_err_t ui_layer_show_data(const char *title, const char *const *labels,
                             const char *const *values, int n)
{
    if (n < 0 || n > UI_LAYER_MAX_ROWS) {
        return ESP_ERR_INVALID_ARG;
    }
    ui_result_reset();

    ui_cmd_t cmd = {0};
    cmd.kind = UI_CMD_SHOW_DATA;
    cmd.n = n;
    ui_safe_strcpy(cmd.question, title, sizeof(cmd.question));
    for (int i = 0; i < n; ++i) {
        ui_safe_strcpy(cmd.opts[i], labels ? labels[i] : NULL, sizeof(cmd.opts[i]));
        ui_safe_strcpy(cmd.vals[i], values ? values[i] : NULL, sizeof(cmd.vals[i]));
    }
    return ui_post(&cmd);
}

esp_err_t ui_layer_show_image(const uint8_t *jpeg, size_t len, const char *path)
{
    if ((!jpeg || len == 0) && (!path || !path[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    ui_result_reset();

    ui_cmd_t cmd = {0};
    cmd.kind = UI_CMD_SHOW_IMAGE;
    ui_safe_strcpy(cmd.path, path, sizeof(cmd.path));

    if (jpeg && len) {
        /* copy into a heap buffer the UI task owns + frees after render, so the
         * caller may free its buffer the moment this returns. */
        cmd.jpeg = (uint8_t *)malloc(len);
        if (!cmd.jpeg) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(cmd.jpeg, jpeg, len);
        cmd.jpeg_len = len;
    }

    esp_err_t err = ui_post(&cmd);
    if (err != ESP_OK && cmd.jpeg) {
        free(cmd.jpeg);
    }
    return err;
}

esp_err_t ui_layer_show_menu(const char *const *items, int n)
{
    if (n < 1 || n > UI_LAYER_MAX_OPTIONS || !items) {
        return ESP_ERR_INVALID_ARG;
    }
    ui_result_reset();

    ui_cmd_t cmd = {0};
    cmd.kind = UI_CMD_SHOW_MENU;
    cmd.n = n;
    for (int i = 0; i < n; ++i) {
        ui_safe_strcpy(cmd.opts[i], items[i], sizeof(cmd.opts[i]));
    }
    return ui_post(&cmd);
}

esp_err_t ui_layer_dismiss(void)
{
    ui_cmd_t cmd = {0};
    cmd.kind = UI_CMD_DISMISS;
    return ui_post(&cmd);
}

/* -------------------------------------------------------------------------
 * Hit-test — ported from prototype pointerdown (lines 407-416). Runs inline on
 * the touch task; the render highlight is posted as a command so display_hal is
 * still only ever touched by the UI task.
 * ---------------------------------------------------------------------- */
int ui_layer_on_tap(int x, int y)
{
    ui_scene_t   scene;
    int          n;
    ui_wedge_t   wedges[UI_LAYER_MAX_OPTIONS];

    if (!s_result_mx || xSemaphoreTake(s_result_mx, pdMS_TO_TICKS(50)) != pdTRUE) {
        return -1;
    }
    scene = s_scene;
    n = s_opt_count;
    for (int i = 0; i < n && i < UI_LAYER_MAX_OPTIONS; ++i) {
        wedges[i] = s_wedges[i];
    }
    xSemaphoreGive(s_result_mx);

    if ((scene != UI_SCENE_CHOICE_ARCS && scene != UI_SCENE_RADIAL_MENU) || n <= 0) {
        return -1;
    }

    const float dx = (float)x - (float)UI_CX;
    const float dy = (float)y - (float)UI_CY;
    const float dist = sqrtf(dx * dx + dy * dy);
    int chosen = -1;

    if (scene == UI_SCENE_CHOICE_ARCS) {
        /* Select by DIRECTION (angle), not exact rim radius. Measured CST9217
         * taps on the arc band (rendered at radius UI_ARC_INNER..OUTER) report
         * ~15% compressed toward centre — e.g. a rim tap lands at dist ~110-130,
         * just inside the 132 px inner radius — so a strict `dist > UI_ARC_INNER`
         * rejected every real tap (2026-06-13). Exclude only the centre dot +
         * "tap to answer" hint (UI_TAP_DEADZONE_R), then match by angle. This is
         * also a more forgiving UX: tap anywhere toward an option. */
        if (dist <= (float)UI_TAP_DEADZONE_R) {
            return -1; /* centre tap, not on a wedge */
        }
        const float ang = ui_norm_angle(atan2f(dy, dx));
        for (int i = 0; i < n; ++i) {
            const float a0 = ui_norm_angle(wedges[i].a0);
            const float a1 = ui_norm_angle(wedges[i].a1);
            bool inside;
            if (a0 <= a1) {
                inside = (ang >= a0 && ang <= a1);
            } else {
                /* wedge wraps across 0 */
                inside = (ang >= a0 || ang <= a1);
            }
            if (inside) {
                chosen = i;
                break;
            }
        }
    } else { /* UI_SCENE_RADIAL_MENU — nearest dot within UI_MENU_HIT_R */
        float best = (float)UI_MENU_HIT_R;
        for (int i = 0; i < n; ++i) {
            const float a = wedges[i].a0; /* dot angle stored in a0 */
            const float ix = (float)UI_CX + cosf(a) * (float)UI_MENU_R;
            const float iy = (float)UI_CY + sinf(a) * (float)UI_MENU_R;
            const float ddx = (float)x - ix;
            const float ddy = (float)y - iy;
            const float d = sqrtf(ddx * ddx + ddy * ddy);
            if (d < best) {
                best = d;
                chosen = i;
            }
        }
    }

    if (chosen < 0) {
        return -1;
    }

    /* highlight the choice, store the result, then dismiss + signal the waiter */
    ui_cmd_t hl = {0};
    hl.kind = UI_CMD_HIGHLIGHT;
    hl.highlight = chosen;
    ui_post(&hl);

    if (xSemaphoreTake(s_result_mx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_result_index = chosen;
        s_result_done = true;
        /* Clear the hit regions immediately so a second tap during the
         * highlight-then-dismiss window can't resolve a second result.
         * ui_layer_on_tap early-returns once s_opt_count is 0, while s_scene
         * stays non-NONE so ui_layer_is_active() keeps swallowing stray taps
         * (they must not fall through to the Gemini toggle) until DISMISS
         * actually tears the HAL down. */
        s_opt_count = 0;
        xSemaphoreGive(s_result_mx);
    }
    if (s_result_sig) {
        xSemaphoreGive(s_result_sig);
    }

    /* brief visible highlight, then dismiss back to the live face */
    ui_cmd_t dm = {0};
    dm.kind = UI_CMD_DISMISS;
    /* small delay so the highlight frame is seen before teardown; the command
     * queue serialises HIGHLIGHT before DISMISS so the order is guaranteed. */
    vTaskDelay(pdMS_TO_TICKS(400));
    ui_post(&dm);

    return chosen;
}

esp_err_t ui_layer_get_result(int *out_index, bool *out_done)
{
    if (!s_result_mx) {
        if (out_done) {
            *out_done = false;
        }
        return ESP_OK;
    }
    if (xSemaphoreTake(s_result_mx, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (out_index) {
            *out_index = s_result_index;
        }
        if (out_done) {
            *out_done = s_result_done;
        }
        xSemaphoreGive(s_result_mx);
    } else {
        if (out_done) {
            *out_done = false;
        }
    }
    return ESP_OK;
}

bool ui_layer_is_active(void)
{
    /* Any non-NONE scene means the UI owns the panel; the touch dispatcher must
     * route taps here (selectable scenes act on them, static scenes swallow them
     * so they don't toggle a Gemini session behind the UI). */
    return s_scene != UI_SCENE_NONE;
}
