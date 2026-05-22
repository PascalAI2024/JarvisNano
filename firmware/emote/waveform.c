/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reactive "Siri-style" waveform face for the 466x466 AMOLED. Pure motion, no
 * face: a breathing centre line at idle, amplitude bars while listening/
 * speaking, a traveling scan pulse while thinking. The emote_gfx widget set has
 * no vector primitives, so we rasterise each frame into a PSRAM RGB565A8 strip
 * and push it to a gfx image object (gfx_img_set_src invalidates → the engine
 * repaints at its fps). A 20 fps task eases displayed amplitude toward the
 * target for fluid motion.
 *
 * Owned by the emote component (it owns the gfx handle + display arbiter). The
 * public API is in emote.h: emote_face_set_state / _set_synthetic_amplitude /
 * _set_amplitude_source. Wired into emote_start() (see emote.c).
 */

#include "emote.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* newlib's math.h does not define M_PI without _GNU_SOURCE in this toolchain
 * (cf. lua_module_audio.c, which guards it the same way). */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "expression_emote.h"
#include "display_arbiter.h"

static const char *TAG = "app_face";

/* Full-panel square that fills the round 466x466 display. The visual is built
 * from concentric rings + radial arcs centred on the panel — the right primitive
 * for a circular screen (a horizontal strip reads as a thin sliver). Buffer:
 * 466*466*2 colour + 466*466 alpha = ~636 KB/frame, x2 double-buffer = ~1.24 MB
 * PSRAM (fine on the 8 MB part; app ~2.5 MB). */
#define FACE_W            466
#define FACE_H            466
#define FACE_CX           (FACE_W / 2)
#define FACE_CY           (FACE_H / 2)
#define FACE_FPS          20
#define FACE_PERIOD_MS    (1000 / FACE_FPS)
#define FACE_RINGS        5                  /* concentric amplitude rings */
#define FACE_R_MAX        220                /* outer ring radius (panel r=233) */
#define FACE_R_MIN        70                 /* inner ring radius */
#define FACE_OBJ_NAME     "waveform"

/* Brand palette (RGB565). Hot core #FFE25E, accent orange #F5870B, black bg.
 * Byte-swapped (__builtin_bswap16) to panel bus order: this CO5300 QSPI panel
 * runs with the emote engine's swap flag ON, and the gfx blend's OPA_COVER fast
 * path copies source pixels UNswapped (gfx_blend.c:190). The working anim faces
 * land correct because the EAF decoder bakes the swap into the palette
 * (eaf_palette_get_color, swap=true -> bswap16); our runtime buffer must match
 * by pre-swapping here, else the waveform renders the wrong colour (white). */
#define RGB565(r, g, b)   ((uint16_t)__builtin_bswap16((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))))
#define COL_CORE          RGB565(0xFF, 0xE2, 0x5E)
#define COL_ACCENT        RGB565(0xF5, 0x87, 0x0B)
#define COL_DIM           RGB565(0x6A, 0x3A, 0x05)   /* dim accent for the idle rest line */

/* One RGB565A8 frame: w*h*2 colour bytes followed by w*h alpha bytes (the gfx
 * decoder's RGB565A8 layout — colour plane then alpha plane). */
#define FRAME_PX          (FACE_W * FACE_H)
#define FRAME_COLOR_BYTES (FRAME_PX * 2)
#define FRAME_ALPHA_BYTES (FRAME_PX)
#define FRAME_BYTES       (FRAME_COLOR_BYTES + FRAME_ALPHA_BYTES)

/* TEMP DIAGNOSTIC — set 1 to bypass the animated waveform and show a single
 * small solid square via the SAME gfx_img_set_src runtime-buffer path. This is
 * the discriminator: a clean 64x64 orange square centred on screen proves the
 * runtime CPU-drawn RGB565A8 image path works (=> the full-panel waveform's
 * white/spiral is a fullscreen geometry/alignment issue). A skewed/white square
 * proves the runtime-buffer path is unsupported on this engine (=> bake the
 * waveform to EAF anim frames instead). Flip back to 0 once decided. */
#define WAVEFORM_DIAG     0
#define DIAG_W            64
#define DIAG_H            64
#define DIAG_BYTES        (DIAG_W * DIAG_H * 2 + DIAG_W * DIAG_H)

/* The runtime CPU-drawn RGB565A8 image path skews even a 64x64 solid square
 * (WAVEFORM_DIAG proved it on hardware 2026-05-21) — this engine only supports
 * flash-baked images + the AAF/anim player, never a live CPU buffer. Disable the
 * waveform face until it is rebuilt as baked EAF amplitude frames on gfx_anim;
 * meanwhile the existing baked emote face renders correctly. */
#define WAVEFORM_ENABLED  0

typedef struct {
    emote_handle_t          emote;
    gfx_obj_t              *obj;          /* the waveform image object */
    uint8_t                *buf[2];       /* double buffer (colour+alpha planes) */
    gfx_image_dsc_t         dsc[2];
    int                     cur;          /* index of the buffer being shown */
    TaskHandle_t            task;
    volatile bool           run;
    volatile emote_face_state_t state;
    volatile int            synth_amp;    /* -1 = use live cb; else 0..1000 */
    emote_face_amp_cb_t     amp_cb;
    float                   disp_amp;     /* eased displayed amplitude 0..1 */
    float                   phase;        /* animation phase accumulator */
} face_ctx_t;

static face_ctx_t s_face;

/* ---- pixel helpers -------------------------------------------------------- */

static inline void px(uint8_t *buf, int x, int y, uint16_t color, uint8_t alpha)
{
    if (x < 0 || x >= FACE_W || y < 0 || y >= FACE_H) {
        return;
    }
    uint16_t *cp = (uint16_t *)buf;
    cp[y * FACE_W + x] = color;
    buf[FRAME_COLOR_BYTES + y * FACE_W + x] = alpha;
}

static void clear_frame(uint8_t *buf)
{
    /* colour plane irrelevant where alpha==0; zero both → transparent black */
    memset(buf, 0, FRAME_BYTES);
}

/* Stroke a ring centred on the panel: radius r, half-thickness ht (px), colour
 * + alpha. Walked by angle so it stays smooth at large radii; arc_from..arc_to
 * in radians select a partial arc (full ring = 0..2pi). */
static void draw_ring_arc(uint8_t *buf, float r, int ht, uint16_t color,
                          uint8_t alpha, float arc_from, float arc_to)
{
    if (r < 1.0f) {
        return;
    }
    float outer = r + ht;
    int steps = (int)(outer * 6.5f) + 8;   /* <1px between samples at the edge */
    float span = arc_to - arc_from;
    for (int s = 0; s <= steps; s++) {
        float th = arc_from + span * ((float)s / steps);
        float ct = cosf(th), st = sinf(th);
        for (int t = -ht; t <= ht; t++) {
            float rr = r + t;
            int x = FACE_CX + (int)(rr * ct);
            int y = FACE_CY + (int)(rr * st);
            /* hot core at the ring centreline, accent toward the stroke edges */
            uint16_t c = (abs(t) <= ht / 3) ? color : COL_ACCENT;
            uint8_t a = (uint8_t)(alpha - (abs(t) * (alpha / 2)) / (ht + 1));
            px(buf, x, y, c, a);
        }
    }
}

#define TWO_PI  6.28318531f

/* ---- per-state renderers (write into buf) -------------------------------- */

static void render_idle(uint8_t *buf, float phase)
{
    /* one calm breathing ring: radius + brightness gently pulse. */
    float breath = 0.5f + 0.5f * sinf(phase * 0.9f);          /* 0..1 */
    float r = 150.0f + breath * 14.0f;                        /* ~150..164 */
    uint8_t a = (uint8_t)(70 + breath * 120);                 /* 70..190 */
    draw_ring_arc(buf, r, 2, COL_CORE, a, 0.0f, TWO_PI);
}

static void render_bars(uint8_t *buf, float amp, float phase)
{
    /* concentric amplitude rings: inner rings always present, outer rings light
     * up + thicken with amplitude — the recognisable round voice visualiser. */
    for (int i = 0; i < FACE_RINGS; i++) {
        float n = (float)i / (FACE_RINGS - 1);                /* 0 inner .. 1 outer */
        float r = FACE_R_MIN + n * (FACE_R_MAX - FACE_R_MIN);
        float shimmer = 0.8f + 0.2f * sinf(phase * 2.5f + i * 0.9f);
        float energy = (amp * (1.0f + n) * shimmer) - n * 0.45f;
        if (energy <= 0.02f) {
            continue;
        }
        if (energy > 1.0f) {
            energy = 1.0f;
        }
        int ht = 1 + (int)(energy * 4.0f);                    /* 1..5 px thick */
        uint8_t a = (uint8_t)(60 + energy * 195);             /* 60..255 */
        draw_ring_arc(buf, r, ht, COL_CORE, a, 0.0f, TWO_PI);
    }
}

static void render_thinking(uint8_t *buf, float phase)
{
    /* a bright arc sweeps around the rim over a dim full ring — "processing". */
    float r = 165.0f;
    draw_ring_arc(buf, r, 1, COL_DIM, 70, 0.0f, TWO_PI);      /* dim track */
    float head = fmodf(phase * 1.4f, TWO_PI);                 /* rotating head */
    draw_ring_arc(buf, r, 3, COL_CORE, 255, head, head + 1.1f); /* ~63deg bright arc */
}

/* ---- amplitude source ----------------------------------------------------- */

static float current_target_amp(emote_face_state_t st)
{
    int milli;
    if (s_face.synth_amp >= 0) {
        milli = s_face.synth_amp;        /* CLI synthetic 0..1000 */
    } else if (s_face.amp_cb) {
        milli = s_face.amp_cb(st);       /* live mic/out RMS, 0..1000 */
    } else {
        /* no source: gentle synthetic sweep so `face listen`/`speak` still moves */
        milli = (int)(500 + 500 * sinf(s_face.phase * 2.2f));
    }
    if (milli < 0) milli = 0;
    if (milli > 1000) milli = 1000;
    return milli / 1000.0f;
}

/* ---- frame task ----------------------------------------------------------- */

static void face_task(void *arg)
{
    (void)arg;
    while (s_face.run) {
        emote_face_state_t st = s_face.state;
        if (st == EMOTE_FACE_OFF || !s_face.obj) {
            vTaskDelay(pdMS_TO_TICKS(FACE_PERIOD_MS));
            continue;
        }

        s_face.phase += (float)FACE_PERIOD_MS / 1000.0f * 6.2831853f; /* ~1Hz base */

        /* ease displayed amplitude toward target (fluidity) */
        float target = (st == EMOTE_FACE_LISTENING || st == EMOTE_FACE_SPEAKING)
                           ? current_target_amp(st) : 0.0f;
        s_face.disp_amp += (target - s_face.disp_amp) * 0.25f;

        int next = s_face.cur ^ 1;
        uint8_t *buf = s_face.buf[next];
        clear_frame(buf);

        switch (st) {
        case EMOTE_FACE_IDLE:      render_idle(buf, s_face.phase); break;
        case EMOTE_FACE_LISTENING: render_bars(buf, s_face.disp_amp, s_face.phase); break;
        case EMOTE_FACE_SPEAKING:  render_bars(buf, s_face.disp_amp, s_face.phase); break;
        case EMOTE_FACE_THINKING:  render_thinking(buf, s_face.phase); break;
        default: break;
        }

        /* publish: only the EMOTE arbiter owner should drive the panel */
        if (display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
            emote_lock(s_face.emote);
            gfx_img_set_src(s_face.obj, &s_face.dsc[next]);   /* invalidates */
            emote_unlock(s_face.emote);
            s_face.cur = next;
        }

        vTaskDelay(pdMS_TO_TICKS(FACE_PERIOD_MS));
    }
    s_face.task = NULL;
    vTaskDelete(NULL);
}

/* ---- public API ----------------------------------------------------------- */

static void face_init_dsc(int i)
{
    /* Match the working reference dsc (test_apps icon_rgb565A8) EXACTLY, as a
     * single designated-initializer struct literal (one packed write) — same
     * style as the reference's `const gfx_image_dsc_t = { ... }`. Only
     * cf/magic/w/h/data_size/data are set; stride + flags stay 0. The decoder
     * derives src_stride from header.w (gfx_img.c:117) and never reads
     * header.stride; a stray non-zero stride/flags or a field-by-field RMW on
     * the packed bitfield header is the one thing that differed from the
     * known-good pattern (suspected spiral cause). */
    s_face.dsc[i] = (gfx_image_dsc_t){
        .header = {
            .cf    = GFX_COLOR_FORMAT_RGB565A8,
            .magic = 0x19,                       /* C_ARRAY_HEADER_MAGIC */
            .w     = FACE_W,
            .h     = FACE_H,
        },
        .data_size = FRAME_BYTES,
        .data      = s_face.buf[i],
    };
}

/* Called from emote_start() once the engine + arbiter are up. */
esp_err_t emote_face_init(emote_handle_t emote)
{
    ESP_RETURN_ON_FALSE(emote, ESP_ERR_INVALID_ARG, TAG, "null emote handle");
    if (s_face.obj) {
        return ESP_OK;   /* already initialised */
    }
    s_face.emote = emote;
    s_face.synth_amp = -1;

#if !WAVEFORM_ENABLED
    ESP_LOGW(TAG, "waveform face disabled (runtime-buffer render unsupported on this "
                  "engine; EAF amplitude-frame pivot pending) — baked emote face stays up");
    return ESP_OK;
#endif

#if WAVEFORM_DIAG
    {
        uint8_t *db = heap_caps_malloc(DIAG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!db) {
            db = heap_caps_malloc(DIAG_BYTES, MALLOC_CAP_8BIT);
        }
        ESP_RETURN_ON_FALSE(db, ESP_ERR_NO_MEM, TAG, "no mem for diag buf");
        uint16_t *cp = (uint16_t *)db;
        for (int i = 0; i < DIAG_W * DIAG_H; i++) {
            cp[i] = COL_ACCENT;                       /* solid orange */
        }
        memset(db + DIAG_W * DIAG_H * 2, 0xFF, DIAG_W * DIAG_H);  /* full alpha */

        static gfx_image_dsc_t diag_dsc;
        diag_dsc = (gfx_image_dsc_t){
            .header = {
                .cf    = GFX_COLOR_FORMAT_RGB565A8,
                .magic = 0x19,
                .w     = DIAG_W,
                .h     = DIAG_H,
            },
            .data_size = DIAG_BYTES,
            .data      = db,
        };
        s_face.obj = emote_create_obj_by_type(emote, EMOTE_OBJ_TYPE_IMAGE, FACE_OBJ_NAME);
        ESP_RETURN_ON_FALSE(s_face.obj, ESP_FAIL, TAG, "diag: failed to create img obj");
        gfx_obj_align(s_face.obj, GFX_ALIGN_CENTER, 0, 0);
        gfx_img_set_src(s_face.obj, &diag_dsc);
        emote_set_obj_visible(emote, FACE_OBJ_NAME, true);
        emote_set_anim_visible(emote, false);
        ESP_LOGW(TAG, "WAVEFORM_DIAG: showing %dx%d solid square (runtime-buffer path test)",
                 DIAG_W, DIAG_H);
        return ESP_OK;
    }
#endif

    for (int i = 0; i < 2; i++) {
        s_face.buf[i] = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_face.buf[i]) {
            s_face.buf[i] = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_8BIT);  /* fallback */
        }
        ESP_RETURN_ON_FALSE(s_face.buf[i], ESP_ERR_NO_MEM, TAG, "no mem for face buf %d", i);
        memset(s_face.buf[i], 0, FRAME_BYTES);
        face_init_dsc(i);
    }

    /* create the image object on the emote display, centred, hidden initially */
    s_face.obj = emote_create_obj_by_type(emote, EMOTE_OBJ_TYPE_IMAGE, FACE_OBJ_NAME);
    ESP_RETURN_ON_FALSE(s_face.obj, ESP_FAIL, TAG, "failed to create waveform img obj");
    gfx_obj_align(s_face.obj, GFX_ALIGN_CENTER, 0, 0);
    gfx_img_set_src(s_face.obj, &s_face.dsc[0]);
    emote_set_obj_visible(emote, FACE_OBJ_NAME, false);

    s_face.run = true;
    s_face.state = EMOTE_FACE_OFF;
    BaseType_t ok = xTaskCreate(face_task, "face", 4096, NULL, 3, &s_face.task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "failed to start face task");

    ESP_LOGI(TAG, "reactive waveform face ready (%dx%d @ %d fps)", FACE_W, FACE_H, FACE_FPS);
    return ESP_OK;
}

esp_err_t emote_face_set_state(emote_face_state_t state)
{
    s_face.state = state;
    if (!s_face.obj) {
        return ESP_OK;   /* will apply once initialised */
    }
    /* show the waveform for any active state; hide (restore emote eye) on OFF */
    bool show = (state != EMOTE_FACE_OFF);
    emote_set_obj_visible(s_face.emote, FACE_OBJ_NAME, show);
    emote_set_anim_visible(s_face.emote, !show);   /* hide the emote eye while active */
    if (state == EMOTE_FACE_IDLE || state == EMOTE_FACE_THINKING) {
        s_face.disp_amp = 0.0f;
    }
    return ESP_OK;
}

void emote_face_set_synthetic_amplitude(int amp_milli)
{
    s_face.synth_amp = amp_milli;   /* <0 restores the live cb */
}

void emote_face_set_amplitude_source(emote_face_amp_cb_t cb)
{
    s_face.amp_cb = cb;
}
