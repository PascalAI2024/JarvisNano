/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * hud_render.c — pure procedural arc-reactor HUD (see hud_render.h).
 *
 * Rendering strategy per strip:
 *   1. memset black (AMOLED: black pixels are literally off — free contrast).
 *   2. Scanline circle BANDS (ring, core glow): each row's x-intervals come
 *      from two integer sqrts, so only band pixels are ever touched. Radial
 *      shading avoids a per-pixel sqrt via |r - mid| ~= |r^2 - mid^2|/(2*mid).
 *   3. PLOTTED polar elements (bezel ticks, waveform bars, comets): walk the
 *      element in polar space via the sine LUT and stamp small dots, clipped
 *      to the strip. No per-pixel angle math anywhere.
 *
 * The face crossfade renders both endpoint faces with complementary global
 * intensity for HUD_TRANS_MS; every layer takes a 0..256 scale factor.
 * Plotted layers draw after bands and overwrite them — draw order is the
 * blend policy (later == on top), which keeps the hot path branch-free.
 */
#include "jr_display/hud_render.h"

#include <string.h>

/* ============================ fixed-point helpers ======================== */

/* 256-entry int16 sine LUT, amplitude 32767. cos(i) = sin(i + 64). */
static int16_t s_sin16[256];
static bool    s_luts_ready;

static void luts_build(void)
{
    if (s_luts_ready) {
        return;
    }
    for (int i = 0; i < 256; ++i) {
        int x = i & 127;                    /* half period */
        if (x > 63) {
            x = 127 - x;                    /* fold to quarter wave, 0..64 */
        }
        /* Bhaskara-style integer approximation of sin(pi*x/128):
         * p = x*(128-x) in 0..4096; s = p*(0.775 + 0.225*(p/4096))*8
         * scaled so s(64) = 32767. Max error < 0.5% — invisible here. */
        int32_t p = (int32_t)x * (128 - x);
        int32_t s = (p * (25451 + ((7373 * p) >> 12))) >> 12;
        if (s > 32767) {
            s = 32767;
        }
        s_sin16[i] = (int16_t)((i & 128) ? -s : s);
    }
    s_luts_ready = true;
}

static inline int lsin(int a) { return s_sin16[a & 255]; }   /* +-32767 */
static inline int lcos(int a) { return s_sin16[(a + 64) & 255]; }

/* Integer sqrt (bands call it twice per row, never per pixel). */
static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* RGB888 -> RGB565, optionally byte-swapped for the panel's memory order. */
static uint16_t pack565(int r, int g, int b, bool swap)
{
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return swap ? (uint16_t)((v >> 8) | (v << 8)) : v;
}

static void ramp_build(uint16_t out[HUD_RAMP_LEVELS], int r, int g, int b, bool swap)
{
    for (int i = 0; i < HUD_RAMP_LEVELS; ++i) {
        out[i] = pack565(r * i / 32, g * i / 32, b * i / 32, swap);
    }
}

/* level 0..255 -> ramp entry (33 steps: index 32 == full color). */
static inline uint16_t shade(const uint16_t *ramp, int level)
{
    if (level <= 0) {
        return ramp[0];
    }
    if (level >= 255) {
        return ramp[32];
    }
    return ramp[level >> 3];
}

/* ============================ geometry ================================== */
/* Half-unit center (232.5, 232.5) keeps the round face symmetric: work in
 * doubled coordinates where pixel (x,y) sits at (2x-465, 2y-465) and radii
 * double on use. */
#define DC(v)   (2 * (v) - (HUD_W - 1))

/* Layout (radii in px on the 233-px round face). */
#define R_TICK_IN   210
#define R_TICK_OUT  222
#define R_RING      150
#define R_BAR_BASE   70
#define R_BAR_MAX    64      /* max bar growth; 70+6+64 = 140 < ring inner */
#define R_CORE       26

/* ============================ scanline bands ============================ */

/* Fill the intersection of row y with annulus [r_in, r_out] px, shading by
 * radial distance from the band's midline (cheap anti-aliased edges). */
static void band_row(uint16_t *row, int y, int r_in, int r_out,
                     const uint16_t *ramp, int level)
{
    if (level <= 0 || r_out <= 0) {
        return;
    }
    int dy = DC(y);
    int64_t dy2 = (int64_t)dy * dy;
    int ro2 = 2 * r_out + 1;                    /* doubled + half-px slack */
    int64_t ro2q = (int64_t)ro2 * ro2;
    if (dy2 >= ro2q) {
        return;                                 /* row misses the annulus */
    }
    int ri2 = 2 * r_in - 1;
    if (ri2 < 0) {
        ri2 = 0;
    }
    int64_t ri2q = (int64_t)ri2 * ri2;

    uint32_t xo = isqrt32((uint32_t)(ro2q - dy2));
    uint32_t xi = dy2 >= ri2q ? 0 : isqrt32((uint32_t)(ri2q - dy2));

    int x_out = (465 + (int)xo) >> 1;           /* rightmost column inside */
    int x_in  = (465 + (int)xi + 1) >> 1;       /* leftmost right-arm column */
    if (x_out > HUD_W - 1) {
        x_out = HUD_W - 1;
    }

    int mid2 = r_in + r_out;                    /* doubled midline radius */
    int64_t mid2q = (int64_t)mid2 * mid2;
    int half2 = (r_out - r_in) + 1;             /* doubled half thickness */
    /* |r - mid| in doubled units ~= |r^2 - mid^2| / (2*mid); precompute the
     * reciprocal as a Q16 multiplier to keep the pixel loop division-free. */
    int32_t inv2mid_q16 = (int32_t)((1 << 16) / (2 * mid2));

    for (int x = x_in; x <= x_out; ++x) {
        int dx = DC(x);
        int64_t r2 = (int64_t)dx * dx + dy2;
        int32_t d = (int32_t)(((r2 - mid2q < 0 ? mid2q - r2 : r2 - mid2q)
                               * inv2mid_q16) >> 16);
        int fall = 256 - (d * 256) / half2;     /* 256 at midline, 0 at edge */
        if (fall <= 0) {
            continue;
        }
        uint16_t px = shade(ramp, (level * fall) >> 8);
        row[x] = px;
        row[465 - x] = px;                      /* mirrored left arm */
    }
}

/* Solid glow disc: intensity = level * (1 - (r/r_out)^2) — reads as light. */
static void glow_row(uint16_t *row, int y, int r_out,
                     const uint16_t *ramp, int level)
{
    if (level <= 0 || r_out <= 0) {
        return;
    }
    int dy = DC(y);
    int ro2 = 2 * r_out;
    int64_t ro2q = (int64_t)ro2 * ro2;
    int64_t dy2 = (int64_t)dy * dy;
    if (dy2 >= ro2q) {
        return;
    }
    uint32_t xo = isqrt32((uint32_t)(ro2q - dy2));
    int x_out = (465 + (int)xo) >> 1;
    if (x_out > HUD_W - 1) {
        x_out = HUD_W - 1;
    }
    /* Q16 reciprocal of ro2q fits: ro2q <= (2*233)^2 ~= 217k */
    int32_t inv_q16 = (int32_t)((1u << 16) / (uint32_t)ro2q);
    for (int x = 233; x <= x_out; ++x) {        /* center outward, mirrored */
        int dx = DC(x);
        int32_t r2q = (int32_t)((int64_t)dx * dx + dy2);
        /* (r2q/ro2q) in Q16 is r2q*inv_q16; >>8 leaves the 0..256 ratio */
        int fall = 256 - (int)(((int64_t)r2q * inv_q16) >> 8);
        if (fall <= 0) {
            continue;
        }
        uint16_t px = shade(ramp, (level * fall) >> 8);
        row[x] = px;
        row[465 - x] = px;
    }
}

/* ============================ plotted elements ========================== */

typedef struct {
    uint16_t *base;   /* strip buffer */
    int y0, y1;       /* strip rows [y0, y1) */
} strip_t;

/* n x n dot; anchor at pixel (x,y); clipped to the strip; overwrites (draw
 * order is the blend policy). */
static inline void dot_n(const strip_t *s, int x, int y, uint16_t px, int n)
{
    for (int j = 0; j < n; ++j) {
        int yy = y + j;
        if (yy < s->y0 || yy >= s->y1) {
            continue;
        }
        uint16_t *row = s->base + (yy - s->y0) * HUD_W;
        for (int i = 0; i < n; ++i) {
            int xx = x + i;
            if (xx >= 0 && xx < HUD_W) {
                row[xx] = px;
            }
        }
    }
}

/* Polar plot: n x n dot at (angle a in Q8 turns, radius r px) from center. */
static inline void polar_dot(const strip_t *s, int a, int r, uint16_t px, int n)
{
    /* Cull before the cos(): called once per strip, so an off-strip dot must
     * cost as little as possible. See ov_spoke for the full rationale. */
    int y = 232 + ((lsin(a) * r) >> 15);
    if (y + n <= s->y0 || y >= s->y1) {
        return;
    }
    int x = 232 + ((lcos(a) * r) >> 15);
    dot_n(s, x, y, px, n);
}

/* Radial segment r0..r1 at angle a; `wide` adds a second angular column. */
static void radial_seg(const strip_t *s, int a, int r0, int r1,
                       uint16_t px, bool wide)
{
    for (int r = r0; r <= r1; r += 2) {
        polar_dot(s, a, r, px, 2);
        if (wide) {
            polar_dot(s, a + 2, r, px, 2);
        }
    }
}

/* ============================ face layers =============================== */
/* Every layer takes scale 0..256 (crossfade weight) plus animation state. */

static void layer_ticks(hud_t *h, const strip_t *s, int scale, int level)
{
    int lv = (level * scale) >> 8;
    if (lv <= 0) {
        return;
    }
    for (int t = 0; t < 60; ++t) {
        int a = (t * 256) / 60;
        int boost = (t % 15 == 0) ? lv + (lv >> 1) : lv;   /* cardinal marks */
        if (boost > 255) {
            boost = 255;
        }
        radial_seg(s, a, R_TICK_IN, R_TICK_OUT,
                   shade(h->ramp[HUD_COL_TICK], boost), false);
    }
}

static void layer_ring(hud_t *h, const strip_t *s, int scale,
                       hud_color_t col, int level, int radius, int half_th)
{
    int lv = (level * scale) >> 8;
    if (lv <= 0) {
        return;
    }
    for (int y = s->y0; y < s->y1; ++y) {
        band_row(s->base + (y - s->y0) * HUD_W, y,
                 radius - half_th, radius + half_th, h->ramp[col], lv);
    }
}

static void layer_core(hud_t *h, const strip_t *s, int scale,
                       hud_color_t col, int level, int radius)
{
    int lv = (level * scale) >> 8;
    if (lv <= 0) {
        return;
    }
    for (int y = s->y0; y < s->y1; ++y) {
        glow_row(s->base + (y - s->y0) * HUD_W, y, radius, h->ramp[col], lv);
    }
}

static void layer_bars(hud_t *h, const strip_t *s, int scale, hud_color_t col)
{
    if (scale <= 0) {
        return;
    }
    for (int b = 0; b < HUD_BARS; ++b) {
        int hist = h->amp_hist[(h->hist_head + b) % HUD_BARS];
        int len = 6 + (hist * R_BAR_MAX) / 255;
        int a = ((b * 256) / HUD_BARS) + (h->bar_rot >> 8);
        int lv = ((150 + (hist * 105) / 255) * scale) >> 8;
        radial_seg(s, a, R_BAR_BASE, R_BAR_BASE + len,
                   shade(h->ramp[col], lv), true);
    }
}

static void layer_comets(hud_t *h, const strip_t *s, int scale)
{
    if (scale <= 0) {
        return;
    }
    const int TRAIL = 52;                       /* Q8 angle units (~73 deg) */
    for (int c = 0; c < 2; ++c) {
        int head = (h->comet_a + c * 128) & 255;
        for (int d = 0; d < TRAIL; ++d) {
            int lv = ((255 * (TRAIL - d)) / TRAIL * scale) >> 8;
            uint16_t hot  = shade(h->ramp[HUD_COL_CYAN], lv);
            uint16_t cool = shade(h->ramp[HUD_COL_CYAN], (lv * 2) / 5);
            int a = (head - d) & 255;
            /* 3-wide dots close the ~3.7 px angular gap between Q8 steps */
            polar_dot(s, a, R_RING - 1, hot, 3);
            polar_dot(s, a, R_RING + 3, cool, 2);
            polar_dot(s, a, R_RING - 5, cool, 2);
        }
        /* white-hot head blaze */
        uint16_t blaze = shade(h->ramp[HUD_COL_TICK],
                               scale >= 256 ? 255 : scale);
        polar_dot(s, head, R_RING - 1, blaze, 4);
        polar_dot(s, (head + 1) & 255, R_RING - 1, blaze, 3);
    }
}

/* Render ONE face's layers into the strip at crossfade weight `scale`. */
static void face_render(hud_t *h, const strip_t *s, hud_face_t f, int scale)
{
    uint32_t t = h->now_ms;

    switch (f) {
    case HUD_FACE_BOOT: {
        /* A point of light blooms into the ring: expanding wave + core. */
        uint32_t el = t - h->boot_start_ms;
        if (el > HUD_BOOT_MS) {
            el = HUD_BOOT_MS;
        }
        int x = (int)((el << 8) / HUD_BOOT_MS);          /* 0..256 */
        int inv = 256 - x;
        int ease = 256 - ((((inv * inv) >> 8) * inv) >> 8); /* ease-out cubic */
        int rw = (R_RING * ease) >> 8;                   /* wavefront radius */
        int wave_lv = 255 - ((155 * ease) >> 8);         /* fades as it grows */
        if (rw > 6) {
            layer_ring(h, s, scale, HUD_COL_CYAN, wave_lv, rw, 5);
        }
        layer_core(h, s, scale, HUD_COL_CYAN, 120 + ((135 * ease) >> 8), R_CORE);
        if (ease > 170) {                                /* ring+ticks land late */
            int k = ((ease - 170) * 256) / 86;
            layer_ring(h, s, (scale * k) >> 8, HUD_COL_CYAN, 140, R_RING, 2);
            layer_ticks(h, s, (scale * k) >> 8, 28);
        }
        break;
    }
    case HUD_FACE_IDLE: {
        int breath = 128 + (lsin(h->breath_phase >> 8) >> 8);   /* 0..256 */
        layer_ticks(h, s, scale, 25);
        layer_ring(h, s, scale, HUD_COL_CYAN, 90 + ((60 * breath) >> 8),
                   R_RING, 2);
        layer_core(h, s, scale, HUD_COL_CYAN,
                   30 + ((26 * (256 - breath)) >> 8), R_CORE - 6);
        break;
    }
    case HUD_FACE_LISTEN:
        layer_ticks(h, s, scale, 40);
        layer_ring(h, s, scale, HUD_COL_CYAN, 210, R_RING, 3);
        layer_bars(h, s, scale, HUD_COL_CYAN);
        layer_core(h, s, scale, HUD_COL_CYAN, 60 + (h->amp >> 2), R_CORE - 6);
        break;
    case HUD_FACE_THINK: {
        int pulse = 128 + (lsin(h->breath_phase >> 6) >> 8);
        layer_ticks(h, s, scale, 40);
        layer_ring(h, s, scale, HUD_COL_CYAN, 105, R_RING, 2);
        layer_comets(h, s, scale);
        layer_core(h, s, scale, HUD_COL_CYAN, 60 + ((50 * pulse) >> 8),
                   R_CORE - 8);
        break;
    }
    case HUD_FACE_SPEAK: {
        int r = R_RING + (h->amp >> 5);                  /* breathes with voice */
        layer_ticks(h, s, scale, 40);
        layer_ring(h, s, scale, HUD_COL_GOLD, 190, r, 3);
        layer_bars(h, s, scale, HUD_COL_GOLD);
        layer_core(h, s, scale, HUD_COL_GOLD, 60 + (h->amp >> 2), R_CORE - 6);
        break;
    }
    case HUD_FACE_ERROR: {
        /* double-blink on a 1.2 s period: on/off/on/rest */
        uint32_t ph = t % 1200u;
        int lv = (ph < 120 || (ph >= 240 && ph < 360)) ? 220 : 90;
        layer_ticks(h, s, scale, 30);
        layer_ring(h, s, scale, HUD_COL_RED, lv, R_RING, 3);
        layer_core(h, s, scale, HUD_COL_RED, lv >> 2, R_CORE - 8);
        break;
    }
    default:
        break;
    }
}

/* ============================ public API ================================ */

void hud_init(hud_t *h, uint32_t now_ms, bool swap_bytes)
{
    memset(h, 0, sizeof *h);
    luts_build();
    h->swap_bytes = swap_bytes;
    h->cur = HUD_FACE_BOOT;
    h->prev = HUD_FACE_BOOT;
    h->boot_start_ms = now_ms;
    h->now_ms = now_ms;
    h->trans_start_ms = now_ms;
    h->req = ((uint32_t)HUD_FACE_IDLE << 8);
    ramp_build(h->ramp[HUD_COL_CYAN],   0, 229, 255, swap_bytes);
    ramp_build(h->ramp[HUD_COL_GOLD], 255, 180,  40, swap_bytes);
    ramp_build(h->ramp[HUD_COL_RED],  255,  64,  48, swap_bytes);
    ramp_build(h->ramp[HUD_COL_TICK],  90, 200, 220, swap_bytes);
}

void hud_set(hud_t *h, hud_face_t face, uint8_t amp)
{
    if (face >= HUD_FACE_COUNT) {
        face = HUD_FACE_IDLE;
    }
    h->req = ((uint32_t)face << 8) | amp;       /* single aligned 32-bit store */
}

void hud_restart_boot(hud_t *h, uint32_t now_ms)
{
    h->boot_done = false;
    h->boot_start_ms = now_ms;
    h->now_ms = now_ms;
    h->cur = HUD_FACE_BOOT;
    h->prev = HUD_FACE_BOOT;
    h->trans_start_ms = now_ms;
}

void hud_tick(hud_t *h, uint32_t now_ms)
{
    uint32_t dt = now_ms - h->now_ms;
    if (dt > 100) {
        dt = 100;                               /* clamp across stalls */
    }
    h->now_ms = now_ms;

    uint32_t req = h->req;                      /* single mailbox read */
    hud_face_t want = (hud_face_t)((req >> 8) & 0xFF);
    int amp_in = (int)(req & 0xFF);

    /* fast attack, slow decay — alive, not jittery */
    if (amp_in > h->amp) {
        h->amp += ((amp_in - h->amp) * 3) >> 2;
    } else {
        h->amp -= (h->amp - amp_in) >> 3;
    }

    /* the boot bloom holds the stage until it finishes */
    if (!h->boot_done) {
        if (now_ms - h->boot_start_ms >= HUD_BOOT_MS) {
            h->boot_done = true;
            h->prev = HUD_FACE_BOOT;
            h->cur = want;
            h->trans_start_ms = now_ms;
        }
    } else if (want != h->cur) {
        h->prev = h->cur;
        h->cur = want;
        h->trans_start_ms = now_ms;
    }

    /* free-running animators */
    h->breath_phase += (int)(dt * 13);          /* ~0.2 Hz idle breathing */
    h->comet_a = (h->comet_a + (int)(dt * 160) / 1000) & 255; /* 1 rev/1.6 s */
    h->bar_rot += (int)dt;                      /* slow waveform drift ~4 u/s */

    /* waveform history: newest at hist_head, bars read it oldest-first */
    bool reactive = h->cur == HUD_FACE_LISTEN || h->cur == HUD_FACE_SPEAK;
    h->hist_head = (h->hist_head + HUD_BARS - 1) % HUD_BARS;
    h->amp_hist[h->hist_head] = (uint8_t)(reactive ? h->amp : 4);
}

void hud_render_rows(hud_t *h, uint16_t *dst, int y0, int nrows)
{
    memset(dst, 0, (size_t)nrows * HUD_W * sizeof(uint16_t));
    strip_t s = { .base = dst, .y0 = y0, .y1 = y0 + nrows };

    if (!h->boot_done) {
        face_render(h, &s, HUD_FACE_BOOT, 256);
        return;
    }
    uint32_t el = h->now_ms - h->trans_start_ms;
    int k = el >= HUD_TRANS_MS ? 256 : (int)((el << 8) / HUD_TRANS_MS);
    if (k < 256 && h->prev != h->cur) {
        face_render(h, &s, h->prev, 256 - k);
    }
    face_render(h, &s, h->cur, k);
}

/* ============================ overlay mode ============================== */
/* See hud_render.h. These composite one element over an already-rendered
 * strip rather than owning the frame, so the baked-EAF face keeps drawing
 * underneath and the round-native furniture lands on top of it. */

/* Lazily-built palette for overlay use. Overlays are stateless (no hud_t), so
 * the ramps live here, rebuilt only if the panel byte order ever changes. */
static uint16_t s_ov_cyan[HUD_RAMP_LEVELS];
static uint16_t s_ov_tick[HUD_RAMP_LEVELS];
static uint16_t s_ov_gold[HUD_RAMP_LEVELS];
static uint16_t s_ov_red[HUD_RAMP_LEVELS];
static bool     s_ov_ready;
static bool     s_ov_swap;

static void overlay_palette(bool swap)
{
    if (s_ov_ready && s_ov_swap == swap) {
        return;
    }
    luts_build();
    ramp_build(s_ov_cyan, 0, 229, 255, swap);
    ramp_build(s_ov_tick, 90, 200, 220, swap);
    ramp_build(s_ov_gold, 255, 180, 40, swap);
    ramp_build(s_ov_red,  255, 64,  48, swap);
    s_ov_swap  = swap;
    s_ov_ready = true;
}

/* ---- RADII: the overlay lives in the baked art's NEGATIVE SPACE ----------
 *
 * The baked rwave_*.eaf faces are not blank backdrops — each is already a
 * complete arc-reactor HUD with its own core, spokes, rings and bezel ticks.
 * Drawing a second ring or a second set of spokes on top produces two HUDs
 * beating against each other, which is exactly what the first version did.
 *
 * Measured on hardware (6 frames averaged across animation phases, baked art
 * only, via the /api/display/hud toggle) the art leaves precisely three bands
 * free — mean AND peak luminance both zero:
 *
 *     r135-149   (15 px)   -> the thinking comet
 *     r185-194   (10 px)   -> breathing ring / listen countdown
 *     r215-239   (25 px)   -> battery rim, and the choice arcs later
 *                             (which the prototype wanted hugging the bezel)
 *
 * Everything else is occupied: core r0-94, inner ring r125-134, outer ring and
 * segments r155-179, bezel ticks r200-214. Put nothing there.
 * Re-measure if the baked art is ever changed. */
#define OV_R_COMET   142   /* centre of the free r135-149 band */
#define OV_R_BREATH  190   /* centre of the free r185-194 band */
#define OV_R_BATT    227   /* centre of the free r215-239 band */

/* One revolution every ~2639 ms == the prototype's `angle = now/420` rad
 * (2*pi*420 ms per turn), expressed in the Q8 turn units the LUT uses. */
#define OV_SPIN_PERIOD_MS  2639
/* ~1.1 rad of trail == 0.175 turns == 45 Q8 units. */
#define OV_TRAIL_Q8        45

void hud_overlay_thinking(uint16_t *dst, int y0, int nrows, uint32_t now_ms,
                          bool swap_bytes)
{
    if (dst == NULL || nrows <= 0) {
        return;
    }
    overlay_palette(swap_bytes);

    const strip_t s = { dst, y0, y0 + nrows };

    /* NO track ring. The comet rides an implied path.
     *
     * The prototype drew a dim ring for the comet to follow, which is right on
     * a blank canvas — but the baked art is ALREADY built from concentric
     * rings, so adding another reads as a competing HUD however dim it is, and
     * however "free" its radius. A moving dot with a trail is unmistakably an
     * accent; a static concentric ring is not. Only draw motion here. */

    const int head = (int)(((uint64_t)now_ms * 256u) / OV_SPIN_PERIOD_MS) & 255;

    /* Trailing tail, dimmest first so the head overwrites cleanly (draw order
     * is the blend policy here, exactly as in layer_comets). */
    for (int d = OV_TRAIL_Q8 - 1; d >= 0; --d) {
        const int lv = (255 * (OV_TRAIL_Q8 - d)) / OV_TRAIL_Q8;
        const uint16_t hot  = shade(s_ov_cyan, lv);
        const uint16_t cool = shade(s_ov_cyan, (lv * 2) / 5);
        const int a = (head - d) & 255;
        /* 3-wide closes the ~3.7 px angular gap between adjacent Q8 steps. */
        polar_dot(&s, a, OV_R_COMET, hot, 3);
        polar_dot(&s, a, OV_R_COMET + 3, cool, 2);
    }

    /* White-hot head. */
    const uint16_t blaze = shade(s_ov_tick, 255);
    polar_dot(&s, head, OV_R_COMET, blaze, 4);
    polar_dot(&s, (head + 1) & 255, OV_R_COMET, blaze, 3);
}

/* ---- offset-aware primitives -------------------------------------------
 * The face renderer draws around a fixed centre; the HUD layer needs to shift
 * with tilt, so these take an explicit centre. Same integer discipline. */

static inline void ov_dot(const strip_t *s, int x, int y, uint16_t px, int n)
{
    for (int j = 0; j < n; ++j) {
        const int yy = y + j;
        if (yy < s->y0 || yy >= s->y1) {
            continue;
        }
        uint16_t *row = s->base + (size_t)(yy - s->y0) * HUD_W;
        for (int i = 0; i < n; ++i) {
            const int xx = x + i;
            if (xx >= 0 && xx < HUD_W) {
                row[xx] = px;
            }
        }
    }
}

static inline void ov_polar(const strip_t *s, int cx, int cy, int a, int r,
                            uint16_t px, int n)
{
    const int y = cy + ((lsin(a) * r) >> 15);
    if (y + n <= s->y0 || y >= s->y1) {
        return;                 /* off-strip: skip the cos() and the writes */
    }
    ov_dot(s, cx + ((lcos(a) * r) >> 15), y, px, n);
}

/* NOTE: ov_spoke (radial bar primitive) lived here and was removed with the
 * waveform — the baked art already draws spokes, so nothing needed it. Recover
 * it from git history if a radial element is ever wanted in free space. */

/* One row of an annulus about (cx,cy), filled analytically: solve the two x
 * spans rather than testing every pixel, so cost is the band width and not the
 * row width. */
static void ov_ring_row(uint16_t *row, int y, int cx, int cy, int r_in,
                        int r_out, const uint16_t *ramp, int level)
{
    if (level <= 0 || r_out <= r_in) {
        return;
    }
    const int dy = y - cy;
    const int dy2 = dy * dy;
    if (dy2 >= r_out * r_out) {
        return;                                  /* row misses the annulus */
    }
    const int xo = (int)isqrt32((uint32_t)(r_out * r_out - dy2));
    const int xi = (dy2 < r_in * r_in)
                 ? (int)isqrt32((uint32_t)(r_in * r_in - dy2)) : 0;
    const uint16_t px = shade(ramp, level);
    for (int side = 0; side < 2; ++side) {
        const int a = side ? cx + xi : cx - xo;
        const int b = side ? cx + xo : cx - xi;
        for (int x = a; x <= b; ++x) {
            if (x >= 0 && x < HUD_W) {
                row[x] = px;
            }
        }
    }
}

void hud_tilt_offset(float roll_deg, float pitch_deg, int8_t *ox, int8_t *oy)
{
    /* DISABLED 2026-07-19 — returns 0,0. Kept as a documented dead end.
     *
     * Two independent reasons, either fatal on its own:
     *
     * 1. IT CANNOT WORK WHILE THE FACE IS BAKED ART. Parallax needs the WHOLE
     *    scene to shift together. Only this overlay can move; the .eaf face
     *    cannot. Offsetting one layer against a fixed one is not depth, it is
     *    misregistration — and it reads exactly like two HUDs sliding apart,
     *    which is what a user reported seeing.
     * 2. THE REST REFERENCE WAS WRONG. roll = atan2(gy, gz), and this board
     *    reads gz ~= -1 g when face-up (see jr_imu.h), so a device lying flat
     *    and perfectly still reports roll ~= 178 deg, not 0. That drove a
     *    PERMANENT -10 px offset on a motionless desk. Any future version must
     *    measure deflection from the resting orientation, not from zero.
     *
     * Revisit only if the baked clips are retired (decision D4) and
     * hud_render_rows() draws the whole frame — then the entire scene can lean
     * as one and the idea becomes sound. */
    if (ox) *ox = 0;
    if (oy) *oy = 0;
    (void)roll_deg; (void)pitch_deg;
    return;
#if 0
    /* ~30 deg of tilt reaches full deflection; beyond that it clamps, so the
     * HUD leans convincingly on a desk nudge without sliding off under a big
     * gesture. Roll moves x, pitch moves y. Sign is chosen so the furniture
     * lags the motion, which is what reads as parallax rather than drift. */
    int x = (int)(-roll_deg * HUD_TILT_MAX / 30.0f);
    int y = (int)(pitch_deg * HUD_TILT_MAX / 30.0f);
    if (x >  HUD_TILT_MAX) x =  HUD_TILT_MAX;
    if (x < -HUD_TILT_MAX) x = -HUD_TILT_MAX;
    if (y >  HUD_TILT_MAX) y =  HUD_TILT_MAX;
    if (y < -HUD_TILT_MAX) y = -HUD_TILT_MAX;
    if (ox) *ox = (int8_t)x;
    if (oy) *oy = (int8_t)y;
#endif
}

/* Battery rim: a thin arc at the outer bezel sweeping clockwise from 12
 * o'clock, proportional to charge. Red under 20%, gold while charging, cyan
 * otherwise. Skipped entirely when no cell is present (batt_pct == 0xFF), so a
 * USB-powered puck shows no misleading gauge. */
static void ov_battery(const strip_t *s, int cx, int cy, const hud_env_t *env)
{
    if (env->batt_pct > 100) {
        return;
    }
    const uint16_t *ramp = (env->batt_pct < 20) ? s_ov_red
                         : (env->charging ? s_ov_gold : s_ov_cyan);
    const int steps = (256 * env->batt_pct) / 100;
    for (int i = 0; i <= steps; ++i) {
        const int a = (i - 64) & 255;            /* -64 Q8 == 12 o'clock */
        ov_polar(s, cx, cy, a, OV_R_BATT, shade(ramp, 200), 2);
    }
}

/* NOTE: there is deliberately NO waveform here.
 *
 * The first version drew 48 amplitude-driven spokes at r=70-128 — directly on
 * top of the baked face's own radial spokes. The baked clips are named
 * rwave_* for "reactive wave": they ALREADY are the amplitude-reactive
 * waveform (the display diag exposes applied_bucket tracking audio level).
 * Adding a second set was pure duplication and read as two competing HUDs.
 *
 * FACE-01 is therefore satisfied by the baked art, not by this layer. If the
 * baked clips are ever retired (decision D4), hud_render_rows() already
 * contains a full procedural face — use that, rather than reinstating an
 * overlay waveform on top of a face that has one. */

void hud_overlay_frame(uint16_t *dst, int y0, int nrows, uint32_t now_ms,
                       bool swap_bytes, const hud_env_t *env)
{
    if (dst == NULL || env == NULL || nrows <= 0) {
        return;
    }
    overlay_palette(swap_bytes);

    const strip_t s = { dst, y0, y0 + nrows };
    const int cx = 232 + env->ox;
    const int cy = 232 + env->oy;

    ov_battery(&s, cx, cy, env);

    switch ((hud_face_t)env->face) {
    case HUD_FACE_IDLE:
        /* Nothing. The baked idle face already breathes; a second breathing
         * ring is one more concentric ring in a design made of them. */
        break;
    case HUD_FACE_LISTEN:
    case HUD_FACE_SPEAK:
        /* Nothing: the baked reactive-wave face already carries these states.
         * The listen-window countdown rim (STATE-02) belongs at OV_R_BREATH. */
        break;
    case HUD_FACE_THINK:
        hud_overlay_thinking(dst, y0, nrows, now_ms, swap_bytes);
        break;
    case HUD_FACE_ERROR:
        for (int y = s.y0; y < s.y1; ++y) {
            if (y >= 0 && y < HUD_H) {
                ov_ring_row(dst + (size_t)(y - s.y0) * HUD_W, y, cx, cy,
                            OV_R_BATT - 2, OV_R_BATT + 2, s_ov_red, 150);
            }
        }
        break;
    default:
        break;
    }
}
