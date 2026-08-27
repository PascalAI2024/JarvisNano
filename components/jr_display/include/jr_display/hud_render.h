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

/* ---- ask text geometry --------------------------------------------------
 * The question and the per-arc labels are TEXT, drawn by jr_display.c's font
 * machinery — this file owns no font. These helpers keep the wrapping and the
 * placement integer-only and host-testable, so the text layer's geometry is
 * pinned by the same suite that pins the arcs. Both are called once per
 * present, never per strip or per frame. */

/* Word-wrap `text` into at most two lines of at most max_cols chars each; the
 * caller's buffers hold max_cols+1 bytes and are always NUL-terminated. Breaks
 * at spaces where possible (break spaces are consumed); a word longer than
 * max_cols hard-splits; no character is lost while the text fits in
 * 2*max_cols, beyond which line 2 hard-truncates. Integer-only, no
 * allocation. */
void hud_wrap2(const char *text, int max_cols, char l1[], char l2[]);

/* Every corner of a label box stays inside this radius. 219 = the arcs' inner
 * edge (223) minus margin: on a ROUND panel the square clamp is not enough —
 * a near-horizontal arc puts the box ~200 px out, where corners run under the
 * arc annulus and off the r=232.5 glass long before they leave the square.
 * Text may overlap the (dimmed) battery rim during a modal ask; it may never
 * touch the arcs' band or the glass edge. */
#define HUD_LABEL_R_SAFE 219

/* Top-left corner of a w-by-h px text box centred on the arc's angular
 * midpoint at radius r. Wrap-aware: an a0 > a1 span crosses 3 o'clock and the
 * midpoint stays inside the span, never on the complementary side. The box is
 * clamped fully inside the 466x466 screen AND radially inside
 * HUD_LABEL_R_SAFE, corner-measured about the half-unit centre (232.5): rows
 * are pulled toward the centre row until the worst row's chord is at least w
 * wide, then x is pulled into that chord. Integer-only. */
void hud_choice_label_anchor(const hud_choice_t *c, int r, int w, int h,
                             int *out_x, int *out_y);

/* Fold of jr_display.c's panel_native -> native_darken -> panel_order_color
 * chain into one masked shift per pixel — the STATE-07 backdrop dim runs over
 * every pixel of every strip while an ask is up, and two per-pixel byte swaps
 * cost real frame rate. Derivation: darken keeps the top 3/4/3 bits of each
 * 5/6/5 channel in place, which is (v >> 2) & 0x39E7 on the native layout; on
 * the byte-swapped layout the same bit permutation splits into the two green
 * bits that cross the byte boundary ((s & 3) << 14) plus the in-byte
 * remainder ((s >> 2) & 0x2739 — bswap(0x39E7) minus those two bits). Proven
 * exhaustively over all 65536 values, both byte orders, in the host suite. */
static inline uint16_t hud_dim565(uint16_t v, bool swap_bytes)
{
    if (!swap_bytes) {
        return (uint16_t)((v >> 2) & 0x39E7u);
    }
    return (uint16_t)(((v & 0x0003u) << 14) | ((v >> 2) & 0x2739u));
}

/* Variable-strength companion to hud_dim565: k=0 leaves v untouched, k=32 is
 * exactly hud_dim565's quarter, and intermediate k interpolates each channel
 * linearly (multiplier m = 32 - 3k/4 in 1/32 units). This exists because the
 * watch dim POPPING between those two endpoints in one frame was the single
 * most-felt transition on the glass — the dim itself has to ease. The
 * red+blue fields multiply together in one pass: 31*32 = 992 stays below
 * bit 10, so blue's product never reaches red's field; green rides alone
 * (63*32 = 2016 < 2^11). At k=32, m=8 collapses to the same bit permutation
 * hud_dim565 performs — pinned exhaustively over all 65536 values, both byte
 * orders, in the host suite. Byte-swapped input round-trips through native
 * order: the split-field fold that keeps hud_dim565 swap-free has no
 * equivalent for a variable multiply, and this path only runs during the
 * few-hundred-ms transition, never at steady state. */
static inline uint16_t hud_fade565(uint16_t v, bool swap_bytes, int k)
{
    if (k <= 0) {
        return v;
    }
    if (k > 32) {
        k = 32;
    }
    if (swap_bytes) {
        v = (uint16_t)((v >> 8) | (v << 8));
    }
    const uint32_t m = 32u - (uint32_t)((24 * k) >> 5);
    uint16_t out = (uint16_t)((((v & 0xF81Fu) * m) >> 5) & 0xF81Fu);
    out |= (uint16_t)((((v & 0x07E0u) * m) >> 5) & 0x07E0u);
    if (swap_bytes) {
        out = (uint16_t)((out >> 8) | (out << 8));
    }
    return out;
}

/* Per-pixel mix, m/32 toward `over` (m 0..32): the canvas entrance/exit
 * crossfade. Both operands arrive in panel byte order; the swapped panel
 * round-trips through native order (the split green field rules out an
 * in-place fold, exactly as in hud_fade565). Weights summing to 32 keep each
 * field's sum below the next field — max 31*32 for red+blue's shared pass,
 * 63*32 < 2^11 for green — so one two-field multiply per operand suffices.
 * Endpoints pinned exhaustively in the host suite: m=0 returns `under`
 * bit-exactly, m=32 returns `over`, both byte orders. Transition frames only;
 * a settled canvas is a memcpy. */
static inline uint16_t hud_mix565(uint16_t under, uint16_t over, int m,
                                  bool swap_bytes)
{
    if (m <= 0) {
        return under;
    }
    if (m > 32) {
        m = 32;
    }
    if (swap_bytes) {
        under = (uint16_t)((under >> 8) | (under << 8));
        over = (uint16_t)((over >> 8) | (over << 8));
    }
    const uint32_t wo = (uint32_t)m;
    const uint32_t wu = 32u - wo;
    uint16_t out = (uint16_t)(((((under & 0xF81Fu) * wu) +
                                ((over & 0xF81Fu) * wo)) >> 5) & 0xF81Fu);
    out |= (uint16_t)(((((under & 0x07E0u) * wu) +
                        ((over & 0x07E0u) * wo)) >> 5) & 0x07E0u);
    if (swap_bytes) {
        out = (uint16_t)((out >> 8) | (out << 8));
    }
    return out;
}

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

/* ---- transient + caption support ----------------------------------------
 * Pure helpers behind STATE-04 (caption chip) and TRANS-05 (tap ripple),
 * host-testable like everything else here. */

/* Half-chord of the glass at row y about (233, 233), radius 233: columns
 * [233-half, 233+half] lie on (or within half a pixel of) the glass; 0 when
 * the row misses it entirely. The caption band dims exactly this interval —
 * staying inside the glass is its only geometric constraint. */
int hud_glass_chord(int y);

/* TRANS-05: transient tap ripple — an expanding, fading 2 px ring from the
 * tap point (cx, cy). Radius grows linearly HUD_RIPPLE_R0 -> HUD_RIPPLE_R1
 * over HUD_RIPPLE_MS while intensity fades to zero; age_ms >= HUD_RIPPLE_MS
 * paints nothing, so expiry needs no state write. Pure motion that erases
 * itself is what licenses it OVER the baked face (the design rule bans
 * persistent furniture there, not feedback). Every painted pixel is clipped
 * to the glass (r <= 232 about (233, 233)) — a rim tap cannot paint into the
 * framebuffer corners. Same strip contract as the other overlays; strictly
 * y-culled, stateless, integer-only. */
#define HUD_RIPPLE_MS  400u
#define HUD_RIPPLE_R0  4
#define HUD_RIPPLE_R1  56
void hud_overlay_ripple(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                        int cx, int cy, uint32_t age_ms);

/* TRANS-01: the wake bloom — VISION.md's "point of light blooms into the
 * ring". A small seed of light at the centre collapses as a cyan wavefront
 * expands from it out to the baked face's ring radius over HUD_BLOOM_MS,
 * ease-out cubic (fast birth, gentle landing), fading as it grows so it
 * reads as light spreading, not a shape being drawn. Fired once when the
 * wake word lands; age_ms >= HUD_BLOOM_MS paints nothing, so expiry needs no
 * state write — the same self-erasing license the ripple holds for drawing
 * transient motion OVER the baked face. Everything stays inside r<=152,
 * always on the glass. Stateless, integer-only, y-culled; same strip
 * contract as every other overlay. */
#define HUD_BLOOM_MS   600u
void hud_overlay_bloom(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                       uint32_t age_ms);

/* UI-01: ambient watch hands over a dimmed face. The baked bezel ticks at
 * r200-214 already form the dial, so the whole watch is two hands and a hub:
 * hour hand r20..110 in bright cyan, minute hand r20..175 in white, a 5 px
 * white hub at the centre. 12 o'clock is a=192 in the LUT convention and both
 * hands sweep clockwise with increasing a (mm=15 -> a=0, 3 o'clock).
 * Everything stays inside r<=192 (seconds tip r190 plus dot spill, riding
 * the free r185-194 band) — on the glass, clear of the baked ticks at r200.
 * hh 0..23 (folded mod 12), mm 0..59, ss 0..59; out-of-range values are
 * folded, never trusted. The gold 1 px seconds hand ticks at the publisher's
 * cadence (~1 Hz) so the resting watch reads as alive.
 *
 * strength 0..255 scales every hand and the hub together; <= 0 paints
 * NOTHING — that hard gate is what lets the presenter run the watch's
 * fade-out through this call while the harness still proves off ==
 * paints-nothing. 255 is the settled watch. Stateless, integer-only,
 * y-culled; same strip contract as every other overlay. */
void hud_overlay_clock(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                       int hh, int mm, int ss, int strength);

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
