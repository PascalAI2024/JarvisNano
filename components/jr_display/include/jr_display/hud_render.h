/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * hud_render.h — pure procedural "arc reactor" HUD renderer (466x466 RGB565).
 *
 * This is the visual identity of JarvisRobot v5 (docs/VISION.md): a breathing
 * arc-reactor ring, an amplitude-reactive waveform, orbiting thinking comets,
 * and a boot bloom — all computed, no baked assets, no full framebuffer.
 * Rendering is strip-oriented: the caller asks for N rows at a time and blits
 * them, so peak RAM is one strip, never a 434 KB frame.
 *
 * Pure C, no ESP-IDF / FreeRTOS includes — host-compilable for tests, exactly
 * like jr_core/jr_dsp. All math is integer (LUT sine, incremental r², integer
 * sqrt for scanline band intervals; angular elements are plotted polar→
 * cartesian so no atan is ever needed).
 *
 * Concurrency contract (matches the presenter split in jr_display.c):
 *   - hud_set() may be called from ANY task; it only stores one aligned
 *     32-bit word (face|amp) — atomic on Xtensa/RISC-V by alignment.
 *   - hud_tick() + hud_render_rows() must be called from ONE render task;
 *     they own every other field.
 */
#ifndef JR_DISPLAY_HUD_RENDER_H
#define JR_DISPLAY_HUD_RENDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUD_W 466
#define HUD_H 466

/* Faces mirror jr_face_t plus the boot intro. The presenter maps 1:1. */
typedef enum {
    HUD_FACE_BOOT = 0,   /* bloom intro; auto-promotes to the pending face */
    HUD_FACE_IDLE,
    HUD_FACE_LISTEN,
    HUD_FACE_THINK,
    HUD_FACE_SPEAK,
    HUD_FACE_ERROR,
    HUD_FACE_COUNT
} hud_face_t;

#define HUD_BARS      48   /* waveform bars around the ring */
#define HUD_RAMP_LEVELS 33 /* 0..32 inclusive brightness ramp per palette entry */

typedef enum {
    HUD_COL_CYAN = 0,
    HUD_COL_GOLD,
    HUD_COL_RED,
    HUD_COL_TICK,
    HUD_COL_COUNT
} hud_color_t;

typedef struct {
    /* ---- cross-task mailbox (the ONLY field other tasks may touch) ---- */
    volatile uint32_t req;        /* (face << 8) | amp — single aligned write */

    /* ---- render-task-owned animation state ---- */
    hud_face_t cur, prev;         /* crossfade endpoints */
    uint32_t   trans_start_ms;    /* crossfade start; k=1 after HUD_TRANS_MS */
    bool       boot_done;         /* bloom finished; face requests now honored */
    uint32_t   boot_start_ms;
    uint32_t   now_ms;            /* last hud_tick time */

    int        amp;               /* smoothed amplitude 0..255 (attack/decay) */
    uint8_t    amp_hist[HUD_BARS];/* per-frame history feeding the bars */
    int        hist_head;
    int        comet_a;           /* comet head angle, 0..255 wrap (Q8) */
    int        bar_rot;           /* slow waveform rotation, Q8 angle units */
    int        breath_phase;      /* free-running phase accumulator */

    /* ---- palette (built once; byte order baked in) ---- */
    uint16_t   ramp[HUD_COL_COUNT][HUD_RAMP_LEVELS];
    bool       swap_bytes;
} hud_t;

/* Crossfade duration between faces. */
#define HUD_TRANS_MS 250u
/* Boot bloom duration before the first real face is honored. */
#define HUD_BOOT_MS  1500u

/* Initialize state + palette. swap_bytes: true if the panel wants big-endian
 * RGB565 in memory (QSPI CO5300 path — confirmed against the v4 blit). */
void hud_init(hud_t *h, uint32_t now_ms, bool swap_bytes);

/* Post the desired face + amplitude (0..255). Callable from any task. */
void hud_set(hud_t *h, hud_face_t face, uint8_t amp);

/* Re-arm the boot bloom to start at now_ms — called by the presenter the
 * moment the panel actually lights, so the intro plays from its true start
 * (panel bring-up takes ~1.4 s of vendor-mandated delays). Render-task only.
 * Face requests posted via hud_set() meanwhile are preserved and honored
 * once the bloom completes. */
void hud_restart_boot(hud_t *h, uint32_t now_ms);

/* Advance animation state one frame. Render-task only. */
void hud_tick(hud_t *h, uint32_t now_ms);

/* Render rows [y0, y0+nrows) of the current frame into dst
 * (nrows * HUD_W uint16 pixels). Render-task only. */
void hud_render_rows(hud_t *h, uint16_t *dst, int y0, int nrows);

/* ---- Overlay mode -------------------------------------------------------
 * Composite a single procedural element OVER an already-rendered strip,
 * instead of taking over the whole frame. This is how the JarvisNano OS design
 * lands incrementally on top of the existing baked-EAF faces: the emote engine
 * still draws the face, and these draw the round-native furniture on top.
 *
 * Same strip contract as hud_render_rows: dst holds rows [y0, y0+nrows) of a
 * full HUD_W-wide frame, and only those rows are touched. Stateless — the
 * animation phase comes from now_ms — so any task may call them, and they are
 * safe to invoke directly from the panel flush path.
 */

/* STATE-03: the "thinking" orbital spinner. A dim track ring at r=150 with a
 * cyan comet orbiting it (~2.6 s/rev) and a ~1.1 rad trailing tail, matching
 * docs/prototype/jarvisnano-os.html. Draw it while the agent is THINKING. */
void hud_overlay_thinking(uint16_t *dst, int y0, int nrows, uint32_t now_ms,
                          bool swap_bytes);

/* Everything the HUD layer needs to know about the world for one frame.
 * Plain data, no pointers — the caller snapshots it and the renderer stays
 * stateless, so this is safe to build in one task and render in another. */
typedef struct {
    uint8_t face;        /* hud_face_t — selects the state element          */
    uint8_t amp;         /* 0..255 audio amplitude, drives the waveform     */
    uint8_t batt_pct;    /* 0..100, or 0xFF when unknown/absent             */
    bool    charging;
    int8_t  ox, oy;      /* parallax offset in px; see hud_tilt_offset()    */
} hud_env_t;

/* The whole HUD for one strip: battery rim + the state's own element, drawn
 * over whatever the face engine already rendered.
 *
 *   IDLE     slow breathing ring — alive, not busy
 *   LISTEN   reactive waveform, cyan, amplitude-driven
 *   THINK    the orbital comet
 *   SPEAK    reactive waveform, white-hot
 *   ERROR    red rim
 *
 * Stateless and integer-only: no allocation, no float, safe to call directly
 * from the panel flush path. */
void hud_overlay_frame(uint16_t *dst, int y0, int nrows, uint32_t now_ms,
                       bool swap_bytes, const hud_env_t *env);

/* ---- STATE-05/06: tap-to-answer choice arcs -----------------------------
 * The agent asks a question, three arcs hug the screen bezel, the user taps
 * one and the answer goes back as a functionResponse.
 *
 * ANGLES are Q8 turn units (0..255) in the LUT's own convention, which is the
 * screen convention (y grows down):
 *
 *      a = 0    ->  3 o'clock        a = 128  ->  9 o'clock
 *      a = 64   ->  6 o'clock        a = 192  -> 12 o'clock
 *
 * A span [a0, a1] sweeps in the direction of INCREASING a (clockwise on
 * glass) and may wrap through 0, i.e. a0 > a1 is legal and means the span
 * crosses 3 o'clock.
 *
 * RADII: the arcs live at r223..231. The baked face leaves r215-239 empty, but
 * the glass ends at r232.5 (466 px about a half-unit centre), so the usable
 * band is r215-232 and it is SPLIT — battery rim r215-220, arcs r223-231 — so
 * the gauge and the arcs can both be live without overdrawing each other. See
 * the radii note in hud_render.c for the measurement and the split. Nothing
 * else may go there, and these must not stray out of it.
 */
#define HUD_CHOICE_MAX 3

typedef struct {
    const char *label;   /* short, may be NULL for an unused slot */
    int         a0, a1;  /* arc span, Q8 turn units 0..255, may wrap */
} hud_choice_t;

/* Fill out[0..n-1] with n evenly spaced, non-overlapping spans that fill the
 * circle MINUS a ~1.2 rad gap centred on 12 o'clock, which is reserved for the
 * question text. Only a0/a1 are written — `label` is left alone, so a caller
 * may declare the labels first and lay the angles out afterwards.
 * n is clamped to 1..HUD_CHOICE_MAX. */
void hud_choice_layout(int n, hud_choice_t *out);

/* Draw the choice arcs over an already-rendered strip. `selected` renders
 * bright (tap confirmation), the rest dim; pass -1 for none. Slots with a NULL
 * label are skipped. Same strip contract as hud_overlay_frame: dst holds rows
 * [y0, y0+nrows) and only those rows are touched. Integer-only, stateless, no
 * allocation — safe to call from the panel flush path. */
void hud_overlay_choices(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                         const hud_choice_t *choices, int n, int selected);

/* Hit-test a touch at panel pixel (x, y) against the arcs. Returns true and
 * writes the index to *out_index on a hit; returns false and writes -1
 * otherwise. Slots with a NULL label never hit.
 *
 * The target is the ARC BAND, not a bearing: a tap must land at r215..255 —
 * the drawn r223..231 plus 8 px of inward and 24 px of outward grace — as well
 * as inside an arc's angular span. Everything inside r215 is baked face art,
 * so poking the face's rings or its bezel ticks can never answer a question by
 * accident. */
bool hud_choice_hit(const hud_choice_t *choices, int n, int x, int y,
                    int *out_index);

/* Map IMU tilt to a HUD parallax offset, clamped to +/-HUD_TILT_MAX px.
 *
 * This is the one place the device beats the browser mockup: a simulated HUD
 * can only ever loop, but this one is anchored to the physical world, so the
 * furniture leans as you tilt the puck and the face reads as sitting behind
 * glass rather than painted on it. Deliberately gentle — this is depth, not a
 * spirit level.
 *
 * roll_deg/pitch_deg come straight from jr_imu. Pass 0,0 to disable. */
#define HUD_TILT_MAX 10
void hud_tilt_offset(float roll_deg, float pitch_deg, int8_t *ox, int8_t *oy);

#ifdef __cplusplus
}
#endif

#endif /* JR_DISPLAY_HUD_RENDER_H */
