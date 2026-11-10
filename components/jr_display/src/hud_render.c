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
#include "jr_display/perf_knob.h"
#include "balance_wheel_data.h"      /* SKELETON: the wheel sprite    */
#include "celestial_stars_data.h"    /* CELESTIAL: the twinkling stars */

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
static uint16_t s_ov_lume[HUD_RAMP_LEVELS];   /* warm cream: the diver's markers */
static bool     s_ov_ready;
static bool     s_ov_swap;
static int16_t  s_ov_halo_dx[32];
static int16_t  s_ov_halo_dy[32];

static void overlay_palette(bool swap)
{
    if (s_ov_ready && s_ov_swap == swap) {
        return;
    }
    luts_build();
    for (int i = 0; i < 32; ++i) {
        const int a = i * 8;   /* Q8 turn: 32 evenly spaced points */
        s_ov_halo_dx[i] = (int16_t)((lcos(a) * 190) >> 15);
        s_ov_halo_dy[i] = (int16_t)((lsin(a) * 190) >> 15);
    }
    ramp_build(s_ov_cyan, 0, 229, 255, swap);
    ramp_build(s_ov_tick, 90, 200, 220, swap);
    ramp_build(s_ov_gold, 255, 180, 40, swap);
    ramp_build(s_ov_red,  255, 64,  48, swap);
    ramp_build(s_ov_lume, 232, 226, 200, swap);
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
 * Re-measure if the baked art is ever changed.
 *
 * TWO CORRECTIONS to that measurement, both learned the hard way:
 *
 * 1. THE FREE BAND IS r215-239 BUT THE GLASS ENDS AT r232.5. The frame is 466
 *    px wide about a half-unit centre (232.5, 232.5), so nothing beyond
 *    r=232.5 is displayable — it is silently clipped by the rectangular
 *    framebuffer, not drawn at the bezel. The USABLE outer band is therefore
 *    r215..232, not r215..239. An earlier choice-arc pass put 16% of its
 *    pixels off-panel this way and lost a whole shading zone to the clip.
 *
 * 2. THE OUTER BAND HAS THREE TENANTS, SO IT IS SPLIT. The battery rim,
 *    persistent privacy ring, and tap-to-answer arcs may all be live at once.
 *    They occupy disjoint radii so no caller has to know about the others:
 *
 *        r215-220   battery rim + the ERROR ring
 *        r221-222   persistent gold privacy ring
 *        r223-231   choice arcs
 *
 *    Note the primitives here use the INTEGER centre 232, so a pixel drawn at
 *    nominal radius r reaches true radius r+0.5 on the -x/-y side. Every bound
 *    above already carries that half pixel. */
#define OV_R_COMET   142   /* centre of the free r135-149 band */
#define OV_R_BREATH  190   /* centre of the free r185-194 band */
#define OV_R_BATT    218   /* inner half of the outer band; see the split above */
#define OV_R_FACE    232   /* last displayable radius (glass edge is 232.5)     */
#define OV_R_PRIVACY_IN  221 /* mute ring between battery and choice arcs */
#define OV_R_PRIVACY_OUT 222

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
    /* Parallax stays disabled: shifting only the HUD misregisters the baked
     * face. Face-up roll is also about 178 degrees on this board, not zero.
     * Revisit only when the whole scene can move together, using deflection
     * from the resting orientation. */
    if (ox) *ox = 0;
    if (oy) *oy = 0;
    (void)roll_deg; (void)pitch_deg;
}

/* Battery rim: a thin arc at the outer bezel sweeping clockwise from 12
 * o'clock, proportional to charge. Red under 20%, cyan when discharging, and
 * gold while charging. Charging adds a short bright comet that repeatedly
 * travels only through the FILLED portion of the gauge: unmistakable motion
 * without ever claiming charge the battery does not have.
 *
 * It rides the INNER half of the outer free band (OV_R_BATT, see the split in
 * the RADII note) precisely so it can coexist with the choice arcs, which own
 * the outer half. */
/* First arc index whose y reaches target, on one monotone half. */
static int ov_battery_bound(int cy, int begin, int end, int target,
                            bool descending)
{
    while (begin < end) {
        const int mid = begin + (end - begin) / 2;
        const int y = cy + ((lsin((mid - 64) & 255) * OV_R_BATT) >> 15);
        if (descending ? y > target : y < target) {
            begin = mid + 1;
        } else {
            end = mid;
        }
    }
    return begin;
}

static void ov_battery(const strip_t *s, int cx, int cy, uint32_t now_ms,
                       const hud_env_t *env)
{
    if (env->batt_pct > 100) {
        return;
    }
    /* GOLD IN THIS BAND MEANS MUTED, AND NOTHING ELSE.
     *
     * The battery rim sits at r215-220 and the privacy ring at r221-222 — one
     * pixel apart. Painting the rim gold while charging put a second gold
     * tenant against the only ring whose whole job is to say the microphone
     * is off, so a charging device read as a muted one at a glance.
     *
     * Nothing is lost by dropping the hue: the comet below is the charging
     * tell, and this function's own contract is that MOTION carries that
     * meaning. Red under 20% stays, because that is a different claim. */
    const uint16_t *ramp =
        (env->batt_pct < HUD_BATT_LOW_PCT && !env->charging) ? s_ov_red
                                                             : s_ov_cyan;
    const int steps = (256 * env->batt_pct) / 100;
    const uint16_t rim = shade(ramp, 200);
    /* Each half of the clockwise arc is monotone in y. Restrict the
     * original ascending writes to dots whose two rows reach this strip;
     * the repeated twelve-o'clock endpoint at i=256 remains intentional. */
    const int middle = steps < 128 ? steps + 1 : 129;
    int begin = ov_battery_bound(cy, 0, middle, s->y0 - 1, false);
    int end = ov_battery_bound(cy, begin, middle, s->y1, false);
    for (int i = begin; i < end; ++i) {
        ov_polar(s, cx, cy, (i - 64) & 255, OV_R_BATT, rim, 2);
    }
    if (steps >= 129) {
        begin = ov_battery_bound(cy, 129, steps + 1, s->y1 - 1, true);
        end = ov_battery_bound(cy, begin, steps + 1, s->y0 - 2, true);
        for (int i = begin; i < end; ++i) {
            ov_polar(s, cx, cy, (i - 64) & 255, OV_R_BATT, rim, 2);
        }
    }
    if (env->charging && steps > 0) {
        const int head = (int)((now_ms / 12U) % (uint32_t)(steps + 1));
        for (int tail = 0; tail < 14 && head - tail >= 0; ++tail) {
            const int a = (head - tail - 64) & 255;
            ov_polar(s, cx, cy, a, OV_R_BATT,
                     shade(s_ov_cyan, 255 - tail * 7), 3);
        }
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

    ov_battery(&s, cx, cy, now_ms, env);
    if (env->privacy_muted) {
        for (int y = s.y0; y < s.y1; ++y) {
            if (y >= 0 && y < HUD_H) {
                ov_ring_row(dst + (size_t)(y - s.y0) * HUD_W, y, cx, cy,
                            OV_R_PRIVACY_IN, OV_R_PRIVACY_OUT,
                            s_ov_gold, 230);
            }
        }
    }

    switch ((hud_face_t)env->face) {
    case HUD_FACE_IDLE:
        /* Nothing. The baked idle face already breathes; a second breathing
         * ring is one more concentric ring in a design made of them. */
        break;
    case HUD_FACE_LISTEN: {
        /* A 32-point breathing halo in the free r185-194 band. The old solid
         * annulus solved two square roots and wrote ~13k pixels every frame,
         * dropping the healthy panel from 16 to 12 fps. The baked listening
         * face carries the waveform; these sparse points retain the unambiguous
         * listening tell without redrawing another full ring. */
        const int a = (int)(((uint64_t)now_ms * 256u) / 1500u);
        int lv = 120 + ((100 * (128 + (lsin(a) >> 8))) >> 8) + (env->amp >> 2);
        if (lv > 255) {
            lv = 255;
        }
        const uint16_t px = shade(s_ov_cyan, lv);
        for (int i = 0; i < 32; ++i) {
            const int y = cy + s_ov_halo_dy[i];
            if (y + 3 <= s.y0 || y >= s.y1) {
                continue;
            }
            ov_dot(&s, cx + s_ov_halo_dx[i], y, px, 3);
        }
        break;
    }
    case HUD_FACE_SPEAK:
        /* Nothing: the baked reactive-wave face already carries speaking —
         * and the audio itself is the tell. The listen ring above stays
         * LISTEN-only so its presence keeps one meaning. */
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

/* ============================ choice arcs =============================== */
/* STATE-05/06: tappable answer arcs hugging the bezel.
 *
 * They sit at r223..231 — the OUTER half of the free r215-239 band, which is
 * really r215-232 because the glass ends at r232.5 (see the RADII note above
 * for both the band measurement and the split that keeps the battery rim off
 * these arcs). Thick filled bands, not outlines: at this radius a hairline is
 * invisible and, more to the point, untappable.
 *
 * Three pieces of integer geometry, in the order the cost matters:
 *
 *   1. CULL. Each arc's bounding box is derived from its angular span (span
 *      endpoints at both radii, plus whichever cardinal directions the span
 *      contains) and clipped against the strip BEFORE any row work. Drawing
 *      the whole band and letting the clip discard it costs frames.
 *   2. ANNULUS. Each surviving row is solved with two integer sqrts, exactly
 *      as ov_ring_row does, so per-row cost is the band thickness (20 px) and
 *      never the row width (466 px).
 *   3. ANGLE. Membership in [a0,a1] is a pair of CROSS PRODUCTS against the
 *      span's two boundary rays — cross(dir(a), p) = cos(a)*dy - sin(a)*dx —
 *      so the render path never computes a per-pixel angle and never needs an
 *      atan. Spans wider than a half turn invert the test (see `wide`).
 */

#define OV_CENTER        232   /* the half-unit face centre all overlays use */
#define OV_R_CHOICE_IN   223   /* inner edge; battery rim ends at 220        */
#define OV_R_CHOICE_MID  226   /* inner -> full brightness step              */
#define OV_R_CHOICE_HI   229   /* full -> outer brightness step              */
#define OV_R_CHOICE_OUT  231   /* outer edge; glass ends at 232.5            */

#define OV_CHOICE_GAP_Q8 48    /* ~1.18 rad reserved at 12 o'clock for text  */
#define OV_CHOICE_SEP_Q8 6     /* dead angle between neighbouring arcs       */

/* TAP TOLERANCE. The hit test answers a BAND, never an unbounded annulus.
 *
 * The first version gated only on a minimum radius and then tested the angle,
 * so every pixel from r111 out to the corner of the framebuffer answered a
 * question: 102,115 px answered versus 20,954 px painted, and a tap anywhere on
 * the baked face's outer ring (r155-179) or its bezel ticks (r200-214) sent a
 * WRONG ANSWER to Gemini. The band is the target; the slop is what makes it
 * forgiving, and it is asymmetric on purpose:
 *
 *   INWARD is tight. Everything below r215 is baked face art. A user poking a
 *      ring or a tick is looking at the face, not answering — 8 px of grace
 *      reaches r215 and stops exactly where the free band starts.
 *   OUTWARD is generous. A tap aimed at a bezel-hugging arc habitually lands
 *      short of the finger's visual centre, the last 0.5 px of glass is under
 *      the bezel, and there is nothing out there to hit by mistake, so the
 *      slop runs well past the panel edge. */
#define OV_CHOICE_HIT_SLOP_IN   8
#define OV_CHOICE_HIT_SLOP_OUT  24

/* Inclusive angular membership with wraparound. All arguments must already be
 * normalised to 0..255. */
static inline bool span_has(int a, int a0, int a1)
{
    return (a0 <= a1) ? (a >= a0 && a <= a1) : (a >= a0 || a <= a1);
}

/* Bounding box of the sector-annulus, in offsets from the centre. Padded by a
 * pixel to absorb the LUT's rounding; the exact angular rejection is the cross
 * product test, this only has to be conservative. */
static void span_bbox(int a0, int a1, int r_in, int r_out,
                      int *xmin, int *xmax, int *ymin, int *ymax)
{
    int xl = 0, xh = 0, yl = 0, yh = 0;
    const int as[2] = { a0, a1 };
    for (int e = 0; e < 2; ++e) {
        for (int rr = 0; rr < 2; ++rr) {
            const int r = rr ? r_out : r_in;
            const int x = (lcos(as[e]) * r) >> 15;
            const int y = (lsin(as[e]) * r) >> 15;
            if (e == 0 && rr == 0) {
                xl = xh = x;
                yl = yh = y;
                continue;
            }
            if (x < xl) xl = x;
            if (x > xh) xh = x;
            if (y < yl) yl = y;
            if (y > yh) yh = y;
        }
    }
    /* a cardinal direction inside the span pushes that side out to r_out */
    if (span_has(0, a0, a1))   xh =  r_out;
    if (span_has(128, a0, a1)) xl = -r_out;
    if (span_has(64, a0, a1))  yh =  r_out;
    if (span_has(192, a0, a1)) yl = -r_out;
    *xmin = xl - 1;
    *xmax = xh + 1;
    *ymin = yl - 1;
    *ymax = yh + 1;
}

/* One arc: a filled [a0,a1] wedge of the choice annulus, shaded in three radial
 * zones so the band reads as a lit tube rather than a flat slab. The radii come
 * from OV_R_CHOICE_IN/MID/HI/OUT (223/226/229/231) — an earlier comment here
 * said "r217..236", which predates the band being narrowed to clear the
 * privacy ring at r221-222 and the glass edge at r232.5. */
static void choice_arc(uint16_t *dst, int y0, int nrows, int a0, int a1,
                       const uint16_t *ramp, int level)
{
    if (level <= 0) {
        return;
    }
    int xmin, xmax, ymin, ymax;
    span_bbox(a0, a1, OV_R_CHOICE_IN, OV_R_CHOICE_OUT, &xmin, &xmax, &ymin, &ymax);

    int ys = OV_CENTER + ymin;
    int ye = OV_CENTER + ymax;
    if (ys < y0)             ys = y0;
    if (ye > y0 + nrows - 1) ye = y0 + nrows - 1;
    if (ys < 0)              ys = 0;
    if (ye > HUD_H - 1)      ye = HUD_H - 1;
    if (ys > ye) {
        return;                         /* arc misses this strip entirely */
    }
    int xlo = OV_CENTER + xmin;
    int xhi = OV_CENTER + xmax;
    if (xlo < 0)         xlo = 0;
    if (xhi > HUD_W - 1) xhi = HUD_W - 1;
    if (xlo > xhi) {
        return;
    }

    /* boundary rays; `wide` flips the wedge test for spans over a half turn */
    const int  c0 = lcos(a0), s0 = lsin(a0);
    const int  c1 = lcos(a1), s1 = lsin(a1);
    const bool wide = ((a1 - a0) & 255) > 128;

    const uint16_t px_in  = shade(ramp, (level * 3) >> 2);
    const uint16_t px_mid = shade(ramp, level);
    const uint16_t px_out = shade(ramp, (level * 5) >> 3);

    const int ro2 = OV_R_CHOICE_OUT * OV_R_CHOICE_OUT;
    const int ri2 = OV_R_CHOICE_IN  * OV_R_CHOICE_IN;
    const int rm2 = OV_R_CHOICE_MID * OV_R_CHOICE_MID;
    const int rh2 = OV_R_CHOICE_HI  * OV_R_CHOICE_HI;

    for (int y = ys; y <= ye; ++y) {
        const int dy  = y - OV_CENTER;
        const int dy2 = dy * dy;
        if (dy2 >= ro2) {
            continue;
        }
        /* |dx| runs t = t_in .. t_out across the band; the two zone
         * boundaries are two more sqrts, so the pixel loop stays a lookup. */
        const int t_out = (int)isqrt32((uint32_t)(ro2 - dy2));
        const int t_in  = (dy2 < ri2) ? (int)isqrt32((uint32_t)(ri2 - dy2)) : 0;
        const int t_mid = (dy2 < rm2) ? (int)isqrt32((uint32_t)(rm2 - dy2)) : 0;
        const int t_hi  = (dy2 < rh2) ? (int)isqrt32((uint32_t)(rh2 - dy2)) : 0;

        uint16_t *row = dst + (size_t)(y - y0) * HUD_W;
        for (int t = t_in; t <= t_out; ++t) {
            const uint16_t px = (t < t_mid) ? px_in
                              : (t < t_hi)  ? px_mid : px_out;
            /* right arm, then the mirrored left arm (t == 0 only once) */
            for (int side = 0; side < 2; ++side) {
                const int dx = side ? -t : t;
                if (side && t == 0) {
                    continue;
                }
                const int x = OV_CENTER + dx;
                if (x < xlo || x > xhi) {
                    continue;
                }
                const int32_t k0 = (int32_t)c0 * dy - (int32_t)s0 * dx;
                const int32_t k1 = (int32_t)c1 * dy - (int32_t)s1 * dx;
                const bool in = wide ? (k0 >= 0 || k1 <= 0)
                                     : (k0 >= 0 && k1 <= 0);
                if (in) {
                    row[x] = px;
                }
            }
        }
    }
}

void hud_choice_layout(int n, hud_choice_t *out)
{
    if (out == NULL) {
        return;
    }
    if (n < 1) {
        n = 1;
    }
    if (n > HUD_CHOICE_MAX) {
        n = HUD_CHOICE_MAX;
    }
    /* Sweep starts just past the 12 o'clock gap and runs clockwise all the way
     * back to its other edge; each arc gets an equal slice with a dead angle
     * between neighbours so they read as separate targets. */
    const int start = (192 + OV_CHOICE_GAP_Q8 / 2) & 255;
    const int avail = 256 - OV_CHOICE_GAP_Q8;
    const int each  = (avail - (n - 1) * OV_CHOICE_SEP_Q8) / n;

    for (int i = 0; i < n; ++i) {
        const int a0 = (start + i * (each + OV_CHOICE_SEP_Q8)) & 255;
        out[i].a0 = a0;
        out[i].a1 = (a0 + each) & 255;
    }
}

void hud_wrap2(const char *text, int max_cols, char l1[], char l2[])
{
    if (l1 != NULL) {
        l1[0] = '\0';
    }
    if (l2 != NULL) {
        l2[0] = '\0';
    }
    if (text == NULL || max_cols <= 0 || l1 == NULL || l2 == NULL) {
        return;
    }

    int len = 0;
    while (text[len] != '\0') {
        len++;
    }
    if (len <= max_cols) {
        for (int i = 0; i < len; ++i) {
            l1[i] = text[i];
        }
        l1[len] = '\0';
        return;
    }

    /* Last space at or before column max_cols. The space break is taken when
     * the remainder fits on line 2, or when the text overflows both lines
     * anyway (truncation is then inevitable and a word-boundary break reads
     * better). Otherwise a hard split at max_cols keeps every character of a
     * text that fits in 2*max_cols — the text is the question being answered,
     * so losing characters to aesthetics is not an option. */
    int brk = 0;
    for (int i = max_cols; i >= 1; --i) {
        if (text[i] == ' ') {
            brk = i;
            break;
        }
    }
    int l1_len;
    int rest;
    if (brk > 0 && (len - (brk + 1) <= max_cols || len > 2 * max_cols)) {
        l1_len = brk;
        rest = brk + 1;
    } else {
        l1_len = max_cols;
        rest = max_cols;
    }
    while (l1_len > 0 && text[l1_len - 1] == ' ') {
        l1_len--;                       /* spaces at the break are consumed */
    }
    for (int i = 0; i < l1_len; ++i) {
        l1[i] = text[i];
    }
    l1[l1_len] = '\0';

    while (text[rest] == ' ') {
        rest++;
    }
    int n2 = 0;
    while (n2 < max_cols && text[rest + n2] != '\0') {
        l2[n2] = text[rest + n2];       /* hard-truncates past 2*max_cols */
        n2++;
    }
    l2[n2] = '\0';
}

void hud_choice_label_anchor(const hud_choice_t *c, int r, int w, int h,
                             int *out_x, int *out_y)
{
    if (out_x == NULL || out_y == NULL) {
        return;
    }
    *out_x = 0;
    *out_y = 0;
    if (c == NULL) {
        return;
    }
    luts_build();

    /* Angular midpoint, wrap-aware: (a1 - a0) & 255 is the swept width even
     * when the span crosses 3 o'clock (a0 > a1), so the midpoint always lands
     * inside the span, never on the complementary side. */
    const int mid = (c->a0 + (((c->a1 - c->a0) & 255) / 2)) & 255;
    int x = OV_CENTER + ((lcos(mid) * r) >> 15) - w / 2;
    int y = OV_CENTER + ((lsin(mid) * r) >> 15) - h / 2;

    /* Square clamp. High clamp first, so a box wider than the panel still
     * ends pinned at 0, never negative. */
    if (x > HUD_W - w) {
        x = HUD_W - w;
    }
    if (y > HUD_H - h) {
        y = HUD_H - h;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }

    /* RADIAL clamp — the square is not the glass. A near-horizontal arc (both
     * n=2 arcs, the n=3 side arcs) puts the box centre ~200 px out, where the
     * corners of a square-clamped box ran under the arc annulus (r>=223) from
     * ~8 label chars and off the r=232.5 glass edge from ~10; labels may be
     * 19. Pull the whole box inside HUD_LABEL_R_SAFE, corner-measured: rows
     * first, so the worst row's chord is at least w wide, then x into that
     * row's chord. Offsets are measured from the half-unit centre (232.5)
     * conservatively — a row/column at integer offset d from 232 is at most
     * d+1 half-units out — and the chord is the symmetric column interval
     * [233-t, 232+t], so t*t + dy*dy <= R*R puts every corner strictly inside
     * R. A box wider than the glass (w > 2R-1) keeps the square clamp only. */
    const int R = HUD_LABEL_R_SAFE;
    int t_need = (w + 1) / 2;
    if (t_need < 1) {
        t_need = 1;
    }
    if (t_need <= R) {
        const int dy_cap = (int)isqrt32((uint32_t)(R * R - t_need * t_need));
        if (y > OV_CENTER + dy_cap - h) {
            y = OV_CENTER + dy_cap - h;
        }
        if (y < OV_CENTER + 1 - dy_cap) {
            y = OV_CENTER + 1 - dy_cap;
        }
        int dy = y + h - 1 - OV_CENTER;          /* bottom-row offset */
        if (OV_CENTER - y > dy) {
            dy = OV_CENTER - y;                  /* top-row offset */
        }
        dy += 1;                                 /* half-unit cover */
        const int rr = R * R - dy * dy;
        if (rr > 0) {
            const int t_max = (int)isqrt32((uint32_t)rr);
            if (x + w - 1 > OV_CENTER + t_max) {
                x = OV_CENTER + t_max - (w - 1);
            }
            if (x < OV_CENTER + 1 - t_max) {
                x = OV_CENTER + 1 - t_max;
            }
        }
    }
    *out_x = x;
    *out_y = y;
}

void hud_overlay_choices(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                         const hud_choice_t *choices, int n, int selected,
                         int strength)
{
    if (dst == NULL || choices == NULL || nrows <= 0 || n <= 0 ||
        strength <= 0) {
        return;                      /* strength 0 must provably paint nothing */
    }
    if (strength > 255) {
        strength = 255;
    }
    if (n > HUD_CHOICE_MAX) {
        n = HUD_CHOICE_MAX;
    }
    overlay_palette(swap_bytes);

    /* Labels are NOT drawn here: this file owns no font, and the question and
     * its answers are text furniture drawn by the text layer. The pointer is
     * the slot's occupancy flag. One strength scales the selected and
     * unselected levels together so the whole presentation fades as one. */
    for (int i = 0; i < n; ++i) {
        if (choices[i].label == NULL) {
            continue;
        }
        const bool sel = (i == selected);
        choice_arc(dst, y0, nrows, choices[i].a0 & 255, choices[i].a1 & 255,
                   sel ? s_ov_tick : s_ov_cyan,
                   sel ? strength : (96 * strength) / 255);
    }
}

/* Hold-to-commit ring (docs/GLASS_DESIGN.md F1, docs/INPUT_MAP.md).
 *
 * One arc in the choice band, sweeping CLOCKWISE from 12 o'clock in proportion
 * to how long the finger has been down. It shares r223-231 with the choice arcs
 * and is mutually exclusive with them by construction — an ask owns the glass,
 * so a commit can never be in flight while a question is up.
 *
 * Cyan, not gold: the persistent privacy ring sits immediately inside at
 * r221-222 in gold, and two gold rings a pixel apart would read as one. The
 * distinguishing quality here is MOTION — a ring that fills is unlike anything
 * else on the glass — not hue.
 *
 * Drawn with the same primitive as a choice arc, so it inherits the same bbox
 * cull and the same strip contract: stateless, y-culled, integer-only. */
void hud_overlay_commit(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                        uint8_t pct, int level)
{
    if (dst == NULL || nrows <= 0 || level <= 0 || pct == 0U) {
        return;
    }
    if (pct > 100U) {
        pct = 100U;
    }
    overlay_palette(swap_bytes);
    /* 12 o'clock is a=192 in the LUT convention, matching the clock hands and
     * the battery rim's -64 base. Sweep is a full turn scaled by pct. */
    const int a0 = 192;
    int sweep = (255 * (int)pct) / 100;
    if (sweep < 2) {
        sweep = 2;              /* always visible once the contact confirms */
    }
    choice_arc(dst, y0, nrows, a0 & 255, (a0 + sweep) & 255, s_ov_cyan, level);
}

/* Integer atan2: (dx,dy) -> Q8 turn units 0..255, in the LUT's convention.
 * Touch arrives as cartesian pixels but spans are angular, and there is no
 * float in this file, so:
 *
 *   1. Four sign tests pin the QUADRANT, putting the answer in a known
 *      64-unit window [base, base+64).
 *   2. Inside a window narrower than a half turn the predicate
 *          P(m) = "the point lies at or clockwise of direction m"
 *               = cos(m)*dy - sin(m)*dx >= 0
 *      is monotone — true up to the answer, false after it — so six halving
 *      steps find the largest m for which it holds, which is the angle
 *      floored to the LUT's 1.4-degree step.
 *
 * The cross product is exact in int32 (32767 * 233 is ~2^23), so the only
 * error is that flooring: no atan, no approximation series, no table beyond
 * the sine LUT that is already here. */
static int ov_angle_q8(int dx, int dy)
{
    int base;
    if (dx > 0 && dy >= 0) {
        base = 0;                       /* [0,64):    3 o'clock -> 6 o'clock */
    } else if (dx <= 0 && dy > 0) {
        base = 64;                      /* [64,128):  6 -> 9                */
    } else if (dx < 0) {
        base = 128;                     /* [128,192): 9 -> 12               */
    } else {
        base = 192;                     /* [192,256): 12 -> 3               */
    }
    int lo = base;
    for (int step = 32; step >= 1; step >>= 1) {
        const int m = lo + step;
        if (m >= base + 64) {
            continue;
        }
        if ((int32_t)lcos(m & 255) * dy - (int32_t)lsin(m & 255) * dx >= 0) {
            lo = m;
        }
    }
    return lo & 255;
}

bool hud_choice_hit(const hud_choice_t *choices, int n, int x, int y,
                    int *out_index)
{
    if (out_index) {
        *out_index = -1;
    }
    if (choices == NULL || n <= 0) {
        return false;
    }
    if (n > HUD_CHOICE_MAX) {
        n = HUD_CHOICE_MAX;
    }
    luts_build();

    const int dx = x - OV_CENTER;
    const int dy = y - OV_CENTER;

    /* BAND FIRST, angle second. The arcs are a band, so the tap target is a
     * band: reject anything that is not radially near the drawn annulus before
     * the bearing is even computed. Gating on a minimum radius alone would
     * hand the entire rest of the screen — the baked face's rings, its bezel
     * ticks, the framebuffer corners — to whichever arc happened to share its
     * bearing. See the slop note at OV_CHOICE_HIT_SLOP_IN. */
    const int r_lo = OV_R_CHOICE_IN  - OV_CHOICE_HIT_SLOP_IN;
    const int r_hi = OV_R_CHOICE_OUT + OV_CHOICE_HIT_SLOP_OUT;
    const int r2 = dx * dx + dy * dy;
    if (r2 < r_lo * r_lo || r2 > r_hi * r_hi) {
        return false;
    }

    const int a = ov_angle_q8(dx, dy);
    for (int i = 0; i < n; ++i) {
        if (choices[i].label == NULL) {
            continue;
        }
        if (span_has(a, choices[i].a0 & 255, choices[i].a1 & 255)) {
            if (out_index) {
                *out_index = i;
            }
            return true;
        }
    }
    return false;
}

/* ============================ transient overlays ======================== */
/* STATE-04 / TRANS-05 support. See hud_render.h for the contracts. */

int hud_glass_chord(int y)
{
    const int dy = y - 233;
    const int d2 = 233 * 233 - dy * dy;
    return d2 <= 0 ? 0 : (int)isqrt32((uint32_t)d2);
}

void hud_overlay_ripple(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                        int cx, int cy, uint32_t age_ms,
                        hud_ripple_kind_t kind)
{
    if (dst == NULL || nrows <= 0 || age_ms >= HUD_RIPPLE_MS) {
        return;                          /* expired: self-clearing, no state */
    }
    /* REJECT mirrors the sweep: same arithmetic, run backwards, so the ring
     * collapses instead of spreading. No extra state, no second slot. */
    const int span = (int)(((uint32_t)(HUD_RIPPLE_R1 - HUD_RIPPLE_R0) *
                            age_ms) / HUD_RIPPLE_MS);
    const int rad = (kind == HUD_RIPPLE_REJECT)
                        ? (HUD_RIPPLE_R1 - span)
                        : (HUD_RIPPLE_R0 + span);
    const int level = (int)((255u * (HUD_RIPPLE_MS - age_ms)) / HUD_RIPPLE_MS);
    if (level <= 0) {
        return;                          /* fully faded: never stamp black */
    }
    const int r_in  = rad - 1;           /* 2 px band, like ov_ring_row */
    const int r_out = rad + 1;

    /* y-cull before the palette or any row work: strips that miss the ring's
     * bounding box pay exactly this compare. */
    int ys = cy - r_out;
    int ye = cy + r_out;
    if (ys < y0)             ys = y0;
    if (ye > y0 + nrows - 1) ye = y0 + nrows - 1;
    if (ys > ye) {
        return;
    }
    overlay_palette(swap_bytes);
    const uint16_t px = (kind == HUD_RIPPLE_ACCEPT)
                            ? shade(s_ov_cyan, level)
                            : shade(s_ov_tick, level / 2);

    const int ro2 = r_out * r_out;
    const int ri2 = r_in * r_in;
    for (int y = ys; y <= ye; ++y) {
        const int dy = y - cy;
        const int dy2 = dy * dy;
        if (dy2 >= ro2) {
            continue;
        }
        /* the glass chord clips BOTH arms: a rim (or off-glass) tap point
         * must never paint outside the circle, framebuffer corners included */
        const int gdy = y - 233;
        const int g2 = 232 * 232 - gdy * gdy;
        if (g2 <= 0) {
            continue;
        }
        const int g = (int)isqrt32((uint32_t)g2);
        int glo = 233 - g;
        int ghi = 233 + g;
        if (glo < 0)         glo = 0;
        if (ghi > HUD_W - 1) ghi = HUD_W - 1;

        const int xo = (int)isqrt32((uint32_t)(ro2 - dy2));
        const int xi = (dy2 < ri2) ? (int)isqrt32((uint32_t)(ri2 - dy2)) : 0;
        uint16_t *row = dst + (size_t)(y - y0) * HUD_W;
        for (int side = 0; side < 2; ++side) {
            int a = side ? cx + xi : cx - xo;
            int b = side ? cx + xo : cx - xi;
            if (a < glo) a = glo;
            if (b > ghi) b = ghi;
            for (int x = a; x <= b; ++x) {
                row[x] = px;
            }
        }
    }
}

/* TRANS-01 wake bloom geometry. The wavefront lands at the baked face's
 * outer-ring territory (r~150) — crossing the occupied bands on the way out
 * is exactly the point: transient motion may sweep OVER the baked art (the
 * ripple's license), only persistent furniture may not. */
#define HUD_BLOOM_R_SEED   10   /* the initial point of light               */
#define HUD_BLOOM_R_END   150   /* wavefront landing radius (face ring)     */

void hud_overlay_bloom(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                       uint32_t age_ms)
{
    if (dst == NULL || nrows <= 0 || age_ms >= HUD_BLOOM_MS) {
        return;                          /* expired: self-clearing, no state */
    }
    /* Ease-out cubic, exactly the boot bloom's curve: the wave leaps out of
     * the seed and decelerates into the ring — birth is fast, landing soft. */
    const int p = (int)((age_ms << 8) / HUD_BLOOM_MS);       /* 0..255 */
    const int inv = 256 - p;
    const int ease = 256 - ((((inv * inv) >> 8) * inv) >> 8);
    const int rw = HUD_BLOOM_R_SEED +
        (((HUD_BLOOM_R_END - HUD_BLOOM_R_SEED) * ease) >> 8);
    /* The front fades as it expands (255 -> 60): spreading light thins. The
     * seed does the complement — a bright point that dies as the wave leaves
     * it, so the energy reads as TRANSFERRED, not duplicated. */
    const int front_lv = 255 - ((195 * ease) >> 8);
    const int seed_lv  = ((256 - ease) * 255) >> 8;

    /* y-cull the whole call on the wavefront's bounding rows before any
     * palette or row work, ripple-style. */
    const int r_out = rw + 1;
    int ys = 232 - r_out;
    int ye = 232 + r_out;
    if (ys < y0)             ys = y0;
    if (ye > y0 + nrows - 1) ye = y0 + nrows - 1;
    if (ys > ye) {
        return;
    }
    overlay_palette(swap_bytes);

    for (int y = ys; y <= ye; ++y) {
        if (y < 0 || y >= HUD_H) {
            continue;
        }
        uint16_t *row = dst + (size_t)(y - y0) * HUD_W;
        /* 4 px wavefront band; max radius 151 keeps every pixel on glass. */
        ov_ring_row(row, y, 232, 232, rw - 3 < 0 ? 0 : rw - 3, r_out,
                    s_ov_cyan, front_lv);
        if (seed_lv > 0) {
            ov_ring_row(row, y, 232, 232, 0, HUD_BLOOM_R_SEED,
                        s_ov_cyan, seed_lv);
        }
    }
}

/* UI-01 watch geometry. Hands are runs of 3x3 dots stamped every radius step
 * (polar_dot y-culls each stamp), so the whole watch tops out at r_tip + 2 px
 * of dot spill — r192 for the seconds hand, inside the r<=192 promise in the
 * header and clear of the baked bezel ticks at r200. */
#define HUD_CLOCK_R_HUB   5
#define HUD_CLOCK_R_IN    20
#define HUD_CLOCK_R_HOUR  110
#define HUD_CLOCK_R_MIN   175
#define HUD_CLOCK_R_SEC   190

/* ==================== THE WATCHES, SECOND CUT (2026-09-02) =================
 *
 * A watch is a dial and hands, and on this hardware those are two different
 * materials. The dial is baked art shown through the face pipeline (or, when
 * the partition lacks it, a black procedural stand-in drawn here). The hands
 * are live geometry drawn the way hands are made: tapered polygons with an
 * anti-aliased edge, a two-tone bevel whose light flank follows a fixed
 * top-left light as the hand turns, a hairline outline, a soft shadow, and
 * lume where the style has it. Everything is solved per strip row from a
 * few vertices — no buffers beyond one row of coverage on the stack.
 *
 * Fixed point: screen coordinates in Q4 (1/16 px), angles in Q16 turns
 * (65536 per revolution, 12 o'clock at 192 << 8, clockwise increasing) so the
 * seconds hand can sweep between the 1 Hz ticks; the 256-entry LUT is
 * interpolated linearly, which is smooth to the pixel at r = 214.
 *
 * Every style draws inside r <= 214 (WATCH_R_MAX): the shell's own disc. The
 * art owns everything beyond. */

#define WATCH_R_MAX      214
#define WATCH_CX16       (232 * 16 + 8)      /* the true centre, 232.5 */
#define WATCH_CY16       (232 * 16 + 8)
#define WATCH_A12        (192u << 8)          /* 12 o'clock in Q16 turns */
/* PILOT's three sub-dials as the art placed them: index 0 = 3 o'clock
 * (temperature), 1 = 6 (seconds), 2 = 9 (battery); Q4 centres, px radius. */
static const struct { int32_t cx, cy; int r; } s_subdial[3] = {
    { HUD_WATCH_SUB3_CX * 16 + 8, HUD_WATCH_SUB3_CY * 16 + 8, HUD_WATCH_SUB3_R },
    { HUD_WATCH_SUB6_CX * 16 + 8, HUD_WATCH_SUB6_CY * 16 + 8, HUD_WATCH_SUB6_R },
    { HUD_WATCH_SUB9_CX * 16 + 8, HUD_WATCH_SUB9_CY * 16 + 8, HUD_WATCH_SUB9_R },
};
#define WATCH_SHADOW_A   100                  /* /256 ~ 40 %             */

static int32_t sin_q16(uint32_t a16)
{
    const int i = (int)((a16 >> 8) & 255u);
    const int f = (int)(a16 & 255u);
    const int s0 = s_sin16[i];
    const int s1 = s_sin16[(i + 1) & 255];
    return s0 + (((s1 - s0) * f) >> 8);
}

static inline int32_t cos_q16(uint32_t a16)
{
    return sin_q16(a16 + (64u << 8));
}

typedef struct { int r, g, b; } wrgb_t;

/* One row of coverage, shared by every anti-aliased fill. File scope, not
 * the stack: the render task runs on 5 KB of internal RAM and was 2.1 KB
 * from the end before the watch; a 932-byte row per fill on top of the
 * hand's own frames is how a task overflows on the one style that draws
 * the most. Single writer — the render task — and the host probes are
 * single-threaded, so a static row is exactly as safe as a local one. */
static uint16_t s_cov[HUD_W];

typedef struct {
    int n;
    int32_t x[8], y[8];          /* Q4 screen coordinates */
} wpoly_t;

/* 565 <- 565 over 565 at alpha/256, in the panel's byte order. */
static inline uint16_t w_blend(uint16_t bg, wrgb_t fg, int a, bool swap)
{
    if (a <= 0) {
        return bg;
    }
    if (swap) {
        bg = (uint16_t)((bg >> 8) | (bg << 8));
    }
    int r = (bg >> 11) & 31, g = (bg >> 5) & 63, b = bg & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    if (a >= 256) {
        r = fg.r; g = fg.g; b = fg.b;
    } else {
        r += ((fg.r - r) * a) >> 8;
        g += ((fg.g - g) * a) >> 8;
        b += ((fg.b - b) * a) >> 8;
    }
    return pack565(r, g, b, swap);
}

/* Coverage accumulation for one pixel row: AA_SUB sub-rows x 16 sub-pixels
 * per fully covered pixel (AA_FULL). Eight sample lines, not four: a hand's
 * flank near horizontal is a sliver a fraction of a pixel deep, and four
 * lines let it fall between them in one column and not the next — a
 * one-pixel hole in the edge row, which is exactly the combing this
 * renderer exists to end. With every flank at least half a pixel across
 * (watch_hand_spec) a sliver always meets four of eight lines; the probe
 * sweeps 13 hands through 360 degrees to hold that. Sixteen lines cost the
 * same again per frame for no visible gain. `lo`/`hi` track the range.
 *
 * EIGHT LINES FOR HAIRLINES, FOUR FOR THE WIDE HANDS (2026-09-05). The
 * sliver argument above is about flanks a fraction of a pixel across; a
 * hand more than six pixels wide with a tip of a pixel or more meets every
 * one of four lines on every row it crosses, and four lines halve the
 * sample work of the fills that dominate PILOT and DIVER. poly_sub()
 * chooses per polygon; the 13 x 360 combing sweep holds either way.
 *
 * The fills below are compiled at O2 inside an -Os image: they are the
 * hot loop of every watch frame (measured 10 ms a hand at 160 MHz at -Os). */
#define AA_SUB   8
#define AA_STEP  (16 / AA_SUB)
#define AA_FULL  (AA_SUB * 16)
#define AA_SUB_WIDE 4

static inline int poly_sub(int32_t w0, int32_t w1)
{
    return (w0 > 3 * 16 && w1 >= 16) ? AA_SUB_WIDE : AA_SUB;
}
#if defined(__GNUC__) && !defined(__clang__)
#define HOT __attribute__((optimize("O2")))   /* the device toolchain */
#else
#define HOT                                   /* the host probes (clang) */
#endif
static inline void cov_span(uint16_t *cov, int32_t xl, int32_t xr, int *lo, int *hi)
{
    if (xr <= xl) {
        return;
    }
    if (xl < 0) {
        xl = 0;
    }
    if (xr > (HUD_W << 4)) {
        xr = HUD_W << 4;
    }
    if (xr <= xl) {
        return;
    }
    const int p0 = (int)(xl >> 4);
    const int p1 = (int)((xr - 1) >> 4);
    if (p0 < *lo) {
        *lo = p0;
    }
    if (p1 > *hi) {
        *hi = p1;
    }
    if (p0 == p1) {
        cov[p0] = (uint16_t)(cov[p0] + (xr - xl));
        return;
    }
    cov[p0] = (uint16_t)(cov[p0] + (16 - (xl & 15)));
    for (int p = p0 + 1; p < p1; ++p) {
        cov[p] = (uint16_t)(cov[p] + 16);
    }
    cov[p1] = (uint16_t)(cov[p1] + (xr - (p1 << 4)));
}

/* n copies of v, two to a word once p is word aligned (the test is on the
 * pointer, so any buffer is safe). A store loop of 16-bit writes costs a
 * write, a compare and a branch per pixel; this costs them per pair. */
static inline void fill16(uint16_t *p, int n, uint16_t v)
{
    if ((hud_perf_knob & JR_KNOB_HUD_FILL_OLD) != 0u) {    /* the loop before 91b44ec */
        for (int i = 0; i < n; i++) {
            p[i] = v;
        }
        return;
    }
    if (n > 0 && (((uintptr_t)p) & 2u) != 0u) {
        *p++ = v;
        n--;
    }
    const uint32_t w = (uint32_t)v | ((uint32_t)v << 16);
    for (; n >= 2; n -= 2, p += 2) {
        memcpy(__builtin_assume_aligned(p, 4), &w, sizeof w);
    }
    if (n > 0) {
        *p = v;
    }
}

static inline void HOT cov_blend(uint16_t *row, const uint16_t *cov, int lo, int hi,
                                 wrgb_t c, int alpha, bool swap, int full)
{
    /* full coverage at full alpha is a straight write: the MINIMAL dial is
     * a 214 px disc every frame and must not cost a blend per pixel */
    const uint16_t solid = pack565(c.r, c.g, c.b, swap);
    for (int x = lo; x <= hi; ++x) {
        const int cv = cov[x];
        if (cv >= full && alpha >= 256) {
            row[x] = solid;
        } else if (cv != 0) {
            row[x] = w_blend(row[x], c,
                             (cv >= full ? alpha : (cv * alpha) / full),
                             swap);
        }
    }
}

/* Anti-aliased convex polygon fill over whatever the strip already holds.
 *
 * Two monotone chains, not an edge list: a convex polygon's boundary from
 * its top vertex to its bottom vertex is one chain each way, so every
 * sample line crosses exactly one edge of each and the inner loop is two
 * multiply-adds, no search. Each edge is solved from its own start (no
 * accumulated error) with a Q8 slope; coordinates are Q4 so every product
 * fits 32 bits. The first cut of this fill divided per sample line per
 * edge in 64-bit and cost a hand 20 ms a frame at 160 MHz. The coverage
 * row is cleared after use over the touched range only. */
typedef struct {
    int32_t x0, y0, y1, slope;   /* y0 < y1; slope = dx * 256 / dy         */
} aa_edge_t;

/* Walk the vertices from `top` in direction `dir` to `bot`, keeping the
 * edges that descend; returns the count. */
static int aa_chain(const wpoly_t *p, int top, int bot, int dir, aa_edge_t *out)
{
    int n = 0;
    int i = top;
    while (i != bot) {
        const int j = (i + dir + p->n) % p->n;
        const int32_t y0 = p->y[i], y1 = p->y[j];
        if (y1 > y0) {
            out[n].x0 = p->x[i];
            out[n].y0 = y0;
            out[n].y1 = y1;
            out[n].slope = ((p->x[j] - p->x[i]) * 256) / (y1 - y0);
            n++;
        }
        i = j;
    }
    return n;
}

static inline int32_t aa_chain_x(const aa_edge_t *e, int n, int *at, int32_t ys)
{
    while (*at + 1 < n && e[*at].y1 <= ys) {
        (*at)++;
    }
    const aa_edge_t *k = &e[*at];
    return k->x0 + (((ys - k->y0) * k->slope) >> 8);
}

static void HOT aa_fill(const strip_t *s, const wpoly_t *p, wrgb_t c, int alpha,
                        bool swap, int sub)
{
    if (p->n < 3 || alpha <= 0) {
        return;
    }
    const int step = 16 / sub, full = sub * 16;
    int top = 0, bot = 0;
    for (int i = 1; i < p->n; ++i) {
        if (p->y[i] < p->y[top]) top = i;
        if (p->y[i] > p->y[bot]) bot = i;
    }
    const int32_t ymin = p->y[top], ymax = p->y[bot];
    int y_lo = (int)(ymin >> 4), y_hi = (int)((ymax + 15) >> 4);
    if (y_lo < s->y0) y_lo = s->y0;
    if (y_hi > s->y1 - 1) y_hi = s->y1 - 1;
    if (y_lo > y_hi || ymax <= ymin) {
        return;
    }
    aa_edge_t ca[8], cb[8];
    const int na = aa_chain(p, top, bot, +1, ca);
    const int nb = aa_chain(p, top, bot, -1, cb);
    if (na == 0 || nb == 0) {
        return;
    }
    int ia = 0, ib = 0;
    uint16_t *const cov = s_cov;
    for (int y = y_lo; y <= y_hi; ++y) {
        int lo = HUD_W, hi = -1;
        const int32_t yr = (int32_t)y << 4;
        for (int k = 0; k < sub; ++k) {
            const int32_t ys = yr + k * step + step / 2;
            if (ys < ymin || ys >= ymax) {
                continue;
            }
            const int32_t xa = aa_chain_x(ca, na, &ia, ys);
            const int32_t xb = aa_chain_x(cb, nb, &ib, ys);
            if (xa < xb) {
                cov_span(cov, xa, xb, &lo, &hi);
            } else if (xb < xa) {
                cov_span(cov, xb, xa, &lo, &hi);
            }
        }
        if (hi >= lo) {
            cov_blend(s->base + (size_t)(y - s->y0) * HUD_W, cov, lo, hi, c,
                      alpha, swap, full);
            memset(cov + lo, 0, (size_t)(hi - lo + 1) * sizeof cov[0]);
        }
    }
}

/* THE DIAL DISC, RECORDED. The MINIMAL dial is one disc of the same centre and
 * radius every frame, and finding its edge (eight square roots and the coverage
 * of the sixteen sample chords a row) cost more than writing it. Each row's
 * answer, which does not depend on the colour, the alpha or the byte order, is
 * kept the first time the row is drawn: the interior span, then the edge
 * pixels' coverage. A replay is the same fills and blends. The arena is the
 * caller's (PSRAM on the device, NULL until hud_dial_cache_set()), so this file
 * still owns no memory: HUD_H row offsets, then per row six words (interior
 * from, to; edge run lo, count; right run from, count) and the coverage words. */
#define DISC_REC_NONE 0xFFFFu
uint32_t hud_perf_knob;
static uint16_t *s_disc_rec;
static size_t s_disc_cap, s_disc_used;

void hud_dial_cache_set(uint16_t *buf, size_t cap_words)
{
    s_disc_rec = NULL;
    s_disc_cap = 0;
    s_disc_used = 0;
    if (buf != NULL && cap_words > (size_t)HUD_H + 8u && cap_words < 0xFFFFu) {
        for (int y = 0; y < HUD_H; ++y) {
            buf[y] = DISC_REC_NONE;
        }
        s_disc_used = HUD_H;
        s_disc_cap = cap_words;
        s_disc_rec = buf;
    }
}

size_t hud_dial_cache_used(void)
{
    return s_disc_used;
}

/* Keep row y: its interior [in0, in1] (empty when in1 < in0) and the coverage
 * of the edge pixels around it, which are in `cov`. */
static void disc_rec_row(int y, int in0, int in1, int lo, int hi, const uint16_t *cov)
{
    int nl = 0, ra = 0, nr = 0;
    if (hi >= lo && in1 >= in0) {
        if (lo < in0) {
            nl = in0 - lo;
        }
        ra = in1 + 1 > lo ? in1 + 1 : lo;
        if (hi >= ra) {
            nr = hi - ra + 1;
        }
    } else if (hi >= lo) {
        nl = hi - lo + 1;
    }
    if (s_disc_used + 6u + (size_t)nl + (size_t)nr > s_disc_cap) {
        return;                       /* full: this row stays computed */
    }
    uint16_t *r = s_disc_rec + s_disc_used;
    r[0] = (uint16_t)in0;
    r[1] = (uint16_t)in1;
    r[2] = (uint16_t)lo;
    r[3] = (uint16_t)nl;
    r[4] = (uint16_t)ra;
    r[5] = (uint16_t)nr;
    for (int i = 0; i < nl; ++i) {
        r[6 + i] = cov[lo + i];
    }
    for (int i = 0; i < nr; ++i) {
        r[6 + nl + i] = cov[ra + i];
    }
    s_disc_rec[y] = (uint16_t)s_disc_used;
    s_disc_used += 6u + (size_t)nl + (size_t)nr;
}

static void disc_blend_run(uint16_t *row, const uint16_t *cv, int x0, int n, wrgb_t c, int alpha,
                           bool swap, uint16_t solid)
{
    for (int i = 0; i < n; ++i) {
        const int v = cv[i];
        if (v >= AA_FULL && alpha >= 256) {
            row[x0 + i] = solid;
        } else if (v != 0) {
            row[x0 + i] = w_blend(row[x0 + i], c, (v >= AA_FULL ? alpha : (v * alpha) / AA_FULL), swap);
        }
    }
}

static void disc_replay_row(uint16_t *row, const uint16_t *r, wrgb_t c, int alpha, bool swap,
                            uint16_t solid)
{
    const int in0 = (int16_t)r[0], in1 = (int16_t)r[1];
    if (in1 >= in0) {
        if (alpha >= 256) {
            fill16(row + in0, in1 - in0 + 1, solid);
        } else {
            for (int x = in0; x <= in1; ++x) {
                row[x] = w_blend(row[x], c, alpha, swap);
            }
        }
    }
    disc_blend_run(row, r + 6, (int16_t)r[2], r[3], c, alpha, swap, solid);
    disc_blend_run(row, r + 6 + r[3], (int16_t)r[4], r[5], c, alpha, swap, solid);
}

/* aa_disc as it was before 91b44ec, verbatim (git show 91b44ec^:components/
 * jr_display/src/hud_render.c): no record branches, the interior stored by a
 * plain loop. JR_KNOB_DISC_ROW_OLD sends every disc here; the device decides
 * whether the record branches or the word fill in aa_disc cost MINIMAL its
 * 10 ms. */
static void HOT aa_disc_v0(const strip_t *s, int32_t cx, int32_t cy, int32_t rad,
                        wrgb_t c, int alpha, bool swap)
{
    if (rad <= 0 || alpha <= 0) {
        return;
    }
    int y_lo = (int)((cy - rad) >> 4), y_hi = (int)((cy + rad + 15) >> 4);
    if (y_lo < s->y0) y_lo = s->y0;
    if (y_hi > s->y1 - 1) y_hi = s->y1 - 1;
    if (y_lo > y_hi) {
        return;
    }
    uint16_t *const cov = s_cov;
    const int32_t r2 = rad * rad;                 /* <= 3424^2, fits */
    const uint16_t solid = pack565(c.r, c.g, c.b, swap);
    for (int y = y_lo; y <= y_hi; ++y) {
        int32_t hmin = INT32_MAX, hmax = -1;
        int32_t halves[AA_SUB];
        for (int sub = 0; sub < AA_SUB; ++sub) {
            const int32_t dy = ((int32_t)y << 4) + sub * AA_STEP + AA_STEP / 2 - cy;
            const int32_t d2 = r2 - dy * dy;
            const int32_t half = d2 > 0 ? (int32_t)isqrt32((uint32_t)d2) : -1;
            halves[sub] = half;
            if (half >= 0) {
                if (half < hmin) hmin = half;
                if (half > hmax) hmax = half;
            }
        }
        if (hmax < 0) {
            continue;
        }
        uint16_t *row = s->base + (size_t)(y - s->y0) * HUD_W;
        /* pixels wholly inside every chord: [cx - hmin, cx + hmin) in Q4 */
        int in0 = (int)((cx - hmin + 15) >> 4), in1 = (int)((cx + hmin) >> 4) - 1;
        if (hmin == INT32_MAX || in1 < in0) {
            in0 = 1; in1 = 0;                       /* empty */
        }
        if (in0 < 0) in0 = 0;
        if (in1 > HUD_W - 1) in1 = HUD_W - 1;
        if (in1 >= in0) {
            if (alpha >= 256) {
                for (int x = in0; x <= in1; ++x) {
                    row[x] = solid;
                }
            } else {
                for (int x = in0; x <= in1; ++x) {
                    row[x] = w_blend(row[x], c, alpha, swap);
                }
            }
        }
        /* the edges: coverage over the two runs outside the solid span */
        int lo = HUD_W, hi = -1;
        for (int sub = 0; sub < AA_SUB; ++sub) {
            const int32_t half = halves[sub];
            if (half < 0) {
                continue;
            }
            const int32_t xl = cx - half, xr = cx + half;
            if (in1 >= in0) {
                cov_span(cov, xl, (int32_t)in0 << 4, &lo, &hi);
                cov_span(cov, (int32_t)(in1 + 1) << 4, xr, &lo, &hi);
            } else {
                cov_span(cov, xl, xr, &lo, &hi);
            }
        }
        if (hi >= lo && in1 >= in0) {
            /* [lo, hi] spans the whole chord, but the interior [in0, in1] holds
             * no coverage (only the two edge runs were accumulated): walking it
             * cost 430 pixels a row on the MINIMAL dial to find zeros. Blend and
             * clear the two edge runs alone. */
            if (lo < in0) {
                cov_blend(row, cov, lo, in0 - 1, c, alpha, swap, AA_FULL);
                memset(cov + lo, 0, (size_t)(in0 - lo) * sizeof cov[0]);
            }
            const int ra = in1 + 1 > lo ? in1 + 1 : lo;
            if (hi >= ra) {
                cov_blend(row, cov, ra, hi, c, alpha, swap, AA_FULL);
                memset(cov + ra, 0, (size_t)(hi - ra + 1) * sizeof cov[0]);
            }
        } else if (hi >= lo) {
            cov_blend(row, cov, lo, hi, c, alpha, swap, AA_FULL);
            memset(cov + lo, 0, (size_t)(hi - lo + 1) * sizeof cov[0]);
        }
    }
}

/* Anti-aliased disc; centre and radius in Q4. The interior — every pixel
 * inside the narrowest of the sixteen sample chords — is written straight;
 * only the two edge runs go through coverage, so the MINIMAL dial (a 214 px
 * disc every frame) costs a row write, not a row of arithmetic. */
static void HOT aa_disc(const strip_t *s, int32_t cx, int32_t cy, int32_t rad,
                        wrgb_t c, int alpha, bool swap)
{
    if ((hud_perf_knob & JR_KNOB_DISC_ROW_OLD) != 0u) {
        aa_disc_v0(s, cx, cy, rad, c, alpha, swap);
        return;
    }
    if (rad <= 0 || alpha <= 0) {
        return;
    }
    int y_lo = (int)((cy - rad) >> 4), y_hi = (int)((cy + rad + 15) >> 4);
    if (y_lo < s->y0) y_lo = s->y0;
    if (y_hi > s->y1 - 1) y_hi = s->y1 - 1;
    if (y_lo > y_hi) {
        return;
    }
    uint16_t *const cov = s_cov;
    const int32_t r2 = rad * rad;                 /* <= 3424^2, fits */
    const uint16_t solid = pack565(c.r, c.g, c.b, swap);
    const bool rec_on = s_disc_rec != NULL && cx == WATCH_CX16 && cy == WATCH_CY16 &&
                        rad == WATCH_R_MAX * 16;
    for (int y = y_lo; y <= y_hi; ++y) {
        if (rec_on && y >= 0 && y < HUD_H && s_disc_rec[y] != DISC_REC_NONE) {
            disc_replay_row(s->base + (size_t)(y - s->y0) * HUD_W, s_disc_rec + s_disc_rec[y], c,
                            alpha, swap, solid);
            continue;
        }
        int32_t hmin = INT32_MAX, hmax = -1;
        int32_t halves[AA_SUB];
        for (int sub = 0; sub < AA_SUB; ++sub) {
            const int32_t dy = ((int32_t)y << 4) + sub * AA_STEP + AA_STEP / 2 - cy;
            const int32_t d2 = r2 - dy * dy;
            const int32_t half = d2 > 0 ? (int32_t)isqrt32((uint32_t)d2) : -1;
            halves[sub] = half;
            if (half >= 0) {
                if (half < hmin) hmin = half;
                if (half > hmax) hmax = half;
            }
        }
        if (hmax < 0) {
            if (rec_on && y >= 0 && y < HUD_H) {
                disc_rec_row(y, 1, 0, HUD_W, -1, cov);   /* nothing on this row */
            }
            continue;
        }
        uint16_t *row = s->base + (size_t)(y - s->y0) * HUD_W;
        /* pixels wholly inside every chord: [cx - hmin, cx + hmin) in Q4 */
        int in0 = (int)((cx - hmin + 15) >> 4), in1 = (int)((cx + hmin) >> 4) - 1;
        if (hmin == INT32_MAX || in1 < in0) {
            in0 = 1; in1 = 0;                       /* empty */
        }
        if (in0 < 0) in0 = 0;
        if (in1 > HUD_W - 1) in1 = HUD_W - 1;
        if (in1 >= in0) {
            if (alpha >= 256) {
                fill16(row + in0, in1 - in0 + 1, solid);
            } else {
                for (int x = in0; x <= in1; ++x) {
                    row[x] = w_blend(row[x], c, alpha, swap);
                }
            }
        }
        /* the edges: coverage over the two runs outside the solid span */
        int lo = HUD_W, hi = -1;
        for (int sub = 0; sub < AA_SUB; ++sub) {
            const int32_t half = halves[sub];
            if (half < 0) {
                continue;
            }
            const int32_t xl = cx - half, xr = cx + half;
            if (in1 >= in0) {
                cov_span(cov, xl, (int32_t)in0 << 4, &lo, &hi);
                cov_span(cov, (int32_t)(in1 + 1) << 4, xr, &lo, &hi);
            } else {
                cov_span(cov, xl, xr, &lo, &hi);
            }
        }
        if (rec_on && y >= 0 && y < HUD_H) {
            disc_rec_row(y, in0, in1, lo, hi, cov);
        }
        if (hi >= lo && in1 >= in0) {
            /* [lo, hi] spans the whole chord, but the interior [in0, in1] holds
             * no coverage (only the two edge runs were accumulated): walking it
             * cost 430 pixels a row on the MINIMAL dial to find zeros. Blend and
             * clear the two edge runs alone. */
            if (lo < in0) {
                cov_blend(row, cov, lo, in0 - 1, c, alpha, swap, AA_FULL);
                memset(cov + lo, 0, (size_t)(in0 - lo) * sizeof cov[0]);
            }
            const int ra = in1 + 1 > lo ? in1 + 1 : lo;
            if (hi >= ra) {
                cov_blend(row, cov, ra, hi, c, alpha, swap, AA_FULL);
                memset(cov + ra, 0, (size_t)(hi - ra + 1) * sizeof cov[0]);
            }
        } else if (hi >= lo) {
            cov_blend(row, cov, lo, hi, c, alpha, swap, AA_FULL);
            memset(cov + lo, 0, (size_t)(hi - lo + 1) * sizeof cov[0]);
        }
    }
}

/* A hand-local (u along the axis, v across; Q4 px) point to the screen. */
static inline void w_rot(int32_t cx, int32_t cy, int32_t c, int32_t sn,
                         int32_t u, int32_t v, int32_t *x, int32_t *y)
{
    /* |u|, |v| <= 466 * 16 and |c|, |sn| <= 32767: the products fit 32 bits */
    *x = cx + ((u * c - v * sn) >> 15);
    *y = cy + ((u * sn + v * c) >> 15);
}

/* Tapered quad u0..u1 with half-widths w0 -> w1 (all Q4), clipped to one
 * flank when `side` is -1 (light) or +1 (dark); 0 is the whole hand. */
static void hand_poly(wpoly_t *p, int32_t cx, int32_t cy, uint32_t a16,
                      int32_t u0, int32_t u1, int32_t w0, int32_t w1, int side)
{
    const int32_t c = cos_q16(a16), sn = sin_q16(a16);
    const int32_t va0 = side > 0 ? 0 : -w0, vb0 = side < 0 ? 0 : w0;
    const int32_t va1 = side > 0 ? 0 : -w1, vb1 = side < 0 ? 0 : w1;
    p->n = 4;
    w_rot(cx, cy, c, sn, u0, va0, &p->x[0], &p->y[0]);
    w_rot(cx, cy, c, sn, u1, va1, &p->x[1], &p->y[1]);
    w_rot(cx, cy, c, sn, u1, vb1, &p->x[2], &p->y[2]);
    w_rot(cx, cy, c, sn, u0, vb0, &p->x[3], &p->y[3]);
}

static inline wrgb_t w_scale(wrgb_t c, int num, int den)
{
    wrgb_t o = { c.r * num / den, c.g * num / den, c.b * num / den };
    if (o.r > 255) o.r = 255;
    if (o.g > 255) o.g = 255;
    if (o.b > 255) o.b = 255;
    return o;
}

/* One bevelled hand about (cx, cy) at angle a16. The light sits at the top
 * left of the glass, so the flank facing it is bright and the other dark,
 * and the split moves with the hand instead of being painted on. */
typedef struct {
    int32_t u0, u1, w0, w1;      /* Q4: tail, tip, half-width at each   */
    wrgb_t  body;                /* the metal                            */
    int     bevel;               /* 0..128: how far the flanks part      */
    bool    outline;             /* hairline dark rim                    */
    bool    shadow;              /* soft (+2,+3) shadow at ~40 %         */
    int32_t lume_u0, lume_u1, lume_w;  /* Q4 inset, lume_w 0 = none     */
    bool    core;                /* FUTURE: white core stripe            */
} hand_spec_t;

static const wrgb_t W_CREAM  = { 232, 226, 200 };
static const wrgb_t W_DARK   = {  12,  12,  14 };
static const wrgb_t W_WHITE  = { 255, 255, 255 };
static const wrgb_t W_CYAN   = {   0, 229, 255 };
static const wrgb_t W_GOLD   = { 255, 180,  40 };
static const wrgb_t W_IVORY  = { 245, 242, 232 };

/* ---- the light -------------------------------------------------------
 * The rest light is the one the watch shipped with: shadow (+2, +3) px, a
 * bevel light at (-0.6, -0.8), the hub glint at (-rad/3, -rad/3). Tilting
 * the device swings it: the shadow falls downhill (opposite the in-plane UP
 * vector), the bevel light and the glint go to the uphill side. */
#define LIGHT_REST_LEN_Q4   58      /* |(32, 48)| rounded                    */
#define LIGHT_MAX_LEN_Q4    96      /* 6 px                                   */
#define LIGHT_RAMP_MG       320     /* deadband edge -> full tilt light       */
#define LIGHT_GLINT_Q4      23      /* glint reach, ~ |(-16, -16)|            */
#define LIGHT_IN_CLAMP_MG   1200

static const hud_light_t s_light_rest = {
    .shadow_dx = 32, .shadow_dy = 48,
    .bevel_lx = -19661, .bevel_ly = -26214,
    .glint_x = -16, .glint_y = -16,
};

void hud_light_solve(int gx_mg, int gy_mg, hud_light_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_light_rest;
    gx_mg = gx_mg < -LIGHT_IN_CLAMP_MG ? -LIGHT_IN_CLAMP_MG
          : gx_mg > LIGHT_IN_CLAMP_MG ? LIGHT_IN_CLAMP_MG : gx_mg;
    gy_mg = gy_mg < -LIGHT_IN_CLAMP_MG ? -LIGHT_IN_CLAMP_MG
          : gy_mg > LIGHT_IN_CLAMP_MG ? LIGHT_IN_CLAMP_MG : gy_mg;
    const uint32_t m2 = (uint32_t)(gx_mg * gx_mg + gy_mg * gy_mg);
    if (m2 < (uint32_t)(HUD_LIGHT_DEADBAND_MG * HUD_LIGHT_DEADBAND_MG)) {
        return;                                    /* flat: today, exactly */
    }
    const int mag = (int)isqrt32(m2);              /* >= 80 */
    int w = ((mag - HUD_LIGHT_DEADBAND_MG) * 256) / LIGHT_RAMP_MG;
    if (w <= 0) {
        return;
    }
    if (w > 256) {
        w = 256;
    }
    const int mc = mag > 1000 ? 1000 : mag;
    const int len = LIGHT_REST_LEN_Q4 + ((LIGHT_MAX_LEN_Q4 - LIGHT_REST_LEN_Q4) * mc) / 1000;
    /* Every divide below is C's truncating one, so a point-reflected input
     * gives the point-reflected output exactly. */
    const int tdx = -(gx_mg * len) / mag, tdy = -(gy_mg * len) / mag;
    out->shadow_dx = s_light_rest.shadow_dx + ((tdx - s_light_rest.shadow_dx) * w) / 256;
    out->shadow_dy = s_light_rest.shadow_dy + ((tdy - s_light_rest.shadow_dy) * w) / 256;

    /* the light is on the uphill side; lerp the unit vectors, then restore
     * the length unless the two are nearly opposed (the vector then passes
     * through zero and the bevel flattens instead of snapping) */
    const int tlx = (gx_mg * 32768) / mag, tly = (gy_mg * 32768) / mag;
    int lx = (s_light_rest.bevel_lx * (256 - w) + tlx * w) / 256;
    int ly = (s_light_rest.bevel_ly * (256 - w) + tly * w) / 256;
    const int n = (int)isqrt32((uint32_t)(lx * lx) + (uint32_t)(ly * ly));
    if (n >= 16384) {
        lx = (lx * 32768) / n;
        ly = (ly * 32768) / n;
    } else {
        lx *= 2;
        ly *= 2;
    }
    out->bevel_lx = lx;
    out->bevel_ly = ly;

    const int gtx = (gx_mg * LIGHT_GLINT_Q4) / mag, gty = (gy_mg * LIGHT_GLINT_Q4) / mag;
    out->glint_x = s_light_rest.glint_x + ((gtx - s_light_rest.glint_x) * w) / 256;
    out->glint_y = s_light_rest.glint_y + ((gty - s_light_rest.glint_y) * w) / 256;
}

int hud_light_ease(int cur_q8, int target_mg, int dt_ms)
{
    if (dt_ms <= 0) {
        return cur_q8;
    }
    if (dt_ms > 1000) {
        dt_ms = 1000;
    }
    const int tgt = target_mg * 256;
    const int diff = tgt - cur_q8;
    if (diff > -256 && diff < 256) {
        return tgt;                                /* within 1 mg: arrived */
    }
    const int a = (dt_ms * 256) / (HUD_LIGHT_TAU_MS + dt_ms);   /* < 256 */
    return cur_q8 + (diff * a) / 256;
}

static void draw_hand(const strip_t *s, int32_t cx, int32_t cy, uint32_t a16,
                      const hand_spec_t *h, int strength, bool swap,
                      const hud_light_t *lt)
{
    wpoly_t p;
    const int32_t c = cos_q16(a16), sn = sin_q16(a16);
    if (lt == NULL) {
        lt = &s_light_rest;
    }
    /* light L = (-0.6, -0.8) at rest: flank -v has normal (sn, -c)/32767 */
    const int dot = (sn * lt->bevel_lx - c * lt->bevel_ly) >> 15;   /* |.| < 2^31 */
    const int light_side = dot >= 0 ? -1 : 1;
    const int k = (h->bevel * (dot < 0 ? -dot : dot)) >> 15;   /* 0..bevel */
    const wrgb_t light = w_scale(h->body, 128 + k, 128);
    const wrgb_t dark  = w_scale(h->body, 128 - k, 128);
    const int A = strength >= 255 ? 256 : strength + 1;

    if (h->shadow) {
        hand_poly(&p, cx + lt->shadow_dx, cy + lt->shadow_dy, a16, h->u0, h->u1, h->w0 + 8, h->w1 + 8, 0);
        aa_fill(s, &p, W_DARK, (WATCH_SHADOW_A * A) >> 8, swap, poly_sub(h->w0 + 8, h->w1 + 8));
    }
    if (h->core) {
        /* a cyan glow beneath the slim hand */
        hand_poly(&p, cx, cy, a16, h->u0 - 16, h->u1 + 16, h->w0 + 40, h->w1 + 40, 0);
        aa_fill(s, &p, h->body, (56 * A) >> 8, swap, poly_sub(h->w0 + 40, h->w1 + 40));
    }
    if (h->outline) {
        hand_poly(&p, cx, cy, a16, h->u0 - 12, h->u1 + 14, h->w0 + 14, h->w1 + 14, 0);
        aa_fill(s, &p, W_DARK, (220 * A) >> 8, swap, poly_sub(h->w0 + 14, h->w1 + 14));
    }
    const int sub = poly_sub(h->w0, h->w1);
    hand_poly(&p, cx, cy, a16, h->u0, h->u1, h->w0, h->w1, light_side);
    aa_fill(s, &p, light, A, swap, sub);
    hand_poly(&p, cx, cy, a16, h->u0, h->u1, h->w0, h->w1, -light_side);
    aa_fill(s, &p, dark, A, swap, sub);
    if (h->lume_w > 0) {
        hand_poly(&p, cx, cy, a16, h->lume_u0, h->lume_u1, h->lume_w, h->lume_w / 2, 0);
        aa_fill(s, &p, W_CREAM, A, swap, poly_sub(h->lume_w, h->lume_w / 2));
    }
    if (h->core) {
        hand_poly(&p, cx, cy, a16, h->u0 + 8, h->u1 - 24, 12, 6, 0);
        aa_fill(s, &p, W_WHITE, (230 * A) >> 8, swap, AA_SUB);
    }
}

/* Polar helpers about the true centre. */
static inline void w_polar(int32_t r16, uint32_t a16, int32_t *x, int32_t *y)
{
    *x = WATCH_CX16 + ((r16 * cos_q16(a16)) >> 15);
    *y = WATCH_CY16 + ((r16 * sin_q16(a16)) >> 15);
}

static void w_hub(const strip_t *s, int32_t rad16, wrgb_t body, int A, bool swap,
                  const hud_light_t *lt)
{
    if (lt == NULL) {
        lt = &s_light_rest;
    }
    aa_disc(s, WATCH_CX16, WATCH_CY16, rad16 + 16, W_DARK, (200 * A) >> 8, swap);
    aa_disc(s, WATCH_CX16, WATCH_CY16, rad16, body, A, swap);
    /* rest: (-rad/3, -rad/3), exactly; the glint slides toward the light */
    aa_disc(s, WATCH_CX16 + (rad16 * lt->glint_x) / 48,
            WATCH_CY16 + (rad16 * lt->glint_y) / 48, rad16 / 3 + 8,
            W_WHITE, (150 * A) >> 8, swap);
}

/* Can a radial tick from r0 to r1 (Q4, at angle a) reach this strip? Its y
 * extent is that of its two axis ends, widened by two pixels for the
 * half-width, the end corners and the anti-aliased edge — conservative, so
 * a tick that touches the strip is never skipped and the output is the
 * strip-independent one. MINIMAL and the FUTURE stand-in used to rotate
 * and fill all sixty ticks in every strip; a strip holds a handful. */
static inline bool tick_in_strip(const strip_t *s, uint32_t a, int32_t r0, int32_t r1)
{
    const int32_t sn = sin_q16(a);
    int32_t ya = WATCH_CY16 + ((r0 * sn) >> 15);
    int32_t yb = WATCH_CY16 + ((r1 * sn) >> 15);
    if (ya > yb) {
        const int32_t t = ya; ya = yb; yb = t;
    }
    const int y_lo = (int)((ya - 32) >> 4), y_hi = (int)((yb + 32 + 15) >> 4);
    return y_hi >= s->y0 && y_lo < s->y1;
}

/* The black stand-in dial for a style whose baked art is not on the
 * partition: the markers only, in the style's own vocabulary, so the watch
 * still reads before /api/ota/assets brings the real dial. */
static void fallback_dial(const strip_t *s, int style, int A, bool swap)
{
    wpoly_t p;
    for (int i = 0; i < 12; ++i) {
        const uint32_t a = WATCH_A12 + (uint32_t)i * (65536u / 12u);
        int32_t x, y;
        switch (style) {
        case 1: /* DIVER: triangle at 12, bars at 3/6/9, discs elsewhere */
            if (i == 0) {
                p.n = 3;
                p.x[0] = WATCH_CX16;      p.y[0] = WATCH_CY16 - 178 * 16;
                p.x[1] = WATCH_CX16 + 22 * 16; p.y[1] = WATCH_CY16 - 156 * 16;
                p.x[2] = WATCH_CX16 - 22 * 16; p.y[2] = WATCH_CY16 - 156 * 16;
                aa_fill(s, &p, W_CREAM, A, swap, AA_SUB);
            } else if (i % 3 == 0) {
                hand_poly(&p, WATCH_CX16, WATCH_CY16, a, 154 * 16, 180 * 16, 4 * 16, 4 * 16, 0);
                aa_fill(s, &p, W_CREAM, A, swap, AA_SUB);
            } else {
                w_polar(168 * 16, a, &x, &y);
                aa_disc(s, x, y, 6 * 16 + 8, W_CREAM, A, swap);
            }
            break;
        case 2: /* DRESS: gold batons */
            hand_poly(&p, WATCH_CX16, WATCH_CY16, a, 150 * 16, 176 * 16, 32, 32, 0);
            aa_fill(s, &p, (wrgb_t){ 212, 175, 55 }, A, swap, AA_SUB);
            break;
        case 3: /* PILOT: white dots; the sub-dial rings below */
            w_polar(176 * 16, a, &x, &y);
            aa_disc(s, x, y, i % 3 == 0 ? 4 * 16 : 2 * 16 + 8, W_WHITE, A, swap);
            break;
        case 4: /* MINIMAL has no baked clip: the dial is drawn here every
                 * frame — warm white to r 214, a hairline minute track, twelve
                 * small black dots */
            if (i == 0) {
                aa_disc(s, WATCH_CX16, WATCH_CY16, WATCH_R_MAX * 16, W_IVORY, A, swap);
                for (int t = 0; t < 60; ++t) {
                    const uint32_t ta = WATCH_A12 + (uint32_t)t * (65536u / 60u);
                    const int32_t r0 = (t % 5 == 0 ? 196 : 202) * 16;
                    if (!tick_in_strip(s, ta, r0, 208 * 16)) {
                        continue;
                    }
                    hand_poly(&p, WATCH_CX16, WATCH_CY16, ta, r0, 208 * 16, 8, 8, 0);
                    aa_fill(s, &p, W_DARK, ((t % 5 == 0 ? 200 : 90) * A) >> 8, swap, AA_SUB);
                }
            }
            w_polar(176 * 16, a, &x, &y);
            aa_disc(s, x, y, 2 * 16 + 8, W_DARK, A, swap);
            break;
        case 6: /* SKELETON: rose-gold batons */
            hand_poly(&p, WATCH_CX16, WATCH_CY16, a, 150 * 16, 178 * 16, 40, 40, 0);
            aa_fill(s, &p, (wrgb_t){ 226, 150, 118 }, A, swap, AA_SUB);
            break;
        case 7: /* CELESTIAL: gold studs */
            w_polar(176 * 16, a, &x, &y);
            aa_disc(s, x, y, i == 0 ? 5 * 16 : 4 * 16, (wrgb_t){ 226, 184, 92 }, A, swap);
            break;
        default: /* FUTURE: a 60-tick scale, cell frames below */
            break;
        }
    }
    if (style == 3) {
        for (int i = 0; i < 3; ++i) {
            aa_disc(s, s_subdial[i].cx, s_subdial[i].cy, s_subdial[i].r * 16,
                    W_WHITE, (160 * A) >> 8, swap);
            aa_disc(s, s_subdial[i].cx, s_subdial[i].cy, s_subdial[i].r * 16 - 24,
                    W_DARK, A, swap);
        }
    }
    if (style == 5) {
        for (int i = 0; i < 60; ++i) {
            const uint32_t a = WATCH_A12 + (uint32_t)i * (65536u / 60u);
            const int32_t r0 = (i % 5 == 0 ? 198 : 204) * 16;
            if (!tick_in_strip(s, a, r0, 210 * 16)) {
                continue;
            }
            hand_poly(&p, WATCH_CX16, WATCH_CY16, a, r0, 210 * 16, 12, 12, 0);
            aa_fill(s, &p, W_CYAN, ((i % 5 == 0 ? 200 : 110) * A) >> 8, swap, AA_SUB);
        }
        /* the four cell frames: hairline cyan boxes at the cell rectangles */
        static const int cells[4][4] = {
            { HUD_WATCH_CELL_DATE_X0, HUD_WATCH_CELL_DATE_Y0, HUD_WATCH_CELL_DATE_X1 - 1, HUD_WATCH_CELL_DATE_Y1 - 1 },
            { HUD_WATCH_CELL_WX_X0, HUD_WATCH_CELL_WX_Y0, HUD_WATCH_CELL_WX_X1 - 1, HUD_WATCH_CELL_WX_Y1 - 1 },
            { HUD_WATCH_CELL_BAT_X0, HUD_WATCH_CELL_BAT_Y0, HUD_WATCH_CELL_BAT_X1 - 1, HUD_WATCH_CELL_BAT_Y1 - 1 },
            { HUD_WATCH_CELL_LINK_X0, HUD_WATCH_CELL_LINK_Y0, HUD_WATCH_CELL_LINK_X1 - 1, HUD_WATCH_CELL_LINK_Y1 - 1 } };
        for (int i = 0; i < 4; ++i) {
            const int x0 = cells[i][0], y0 = cells[i][1], x1 = cells[i][2], y1 = cells[i][3];
            const int32_t e[4][4] = {
                { x0, y0, x1, y0 }, { x0, y1, x1, y1 }, { x0, y0, x0, y1 }, { x1, y0, x1, y1 } };
            for (int k = 0; k < 4; ++k) {
                p.n = 4;
                p.x[0] = e[k][0] * 16; p.y[0] = e[k][1] * 16;
                p.x[1] = e[k][2] * 16 + 16; p.y[1] = e[k][1] * 16;
                p.x[2] = e[k][2] * 16 + 16; p.y[2] = e[k][3] * 16 + 16;
                p.x[3] = e[k][0] * 16; p.y[3] = e[k][3] * 16 + 16;
                aa_fill(s, &p, W_CYAN, (90 * A) >> 8, swap, AA_SUB);
            }
        }
    }
}

/* Per-style hand specs, in Q4. Index: 0 hour, 1 minute, 2 seconds. */
static bool watch_hand_spec(int style, int hand, hand_spec_t *h)
{
    memset(h, 0, sizeof *h);
    switch (style) {
    case 1: /* DIVER: steel with lume, Mercedes hour, sword minute, lollipop */
        h->body = (wrgb_t){ 205, 210, 215 }; h->bevel = 60; h->outline = true; h->shadow = true;
        if (hand == 0) { h->u0 = -14 * 16; h->u1 = 112 * 16; h->w0 = 5 * 16; h->w1 = 3 * 16 + 8; h->lume_u0 = 20 * 16; h->lume_u1 = 74 * 16; h->lume_w = 2 * 16 + 8; }
        else if (hand == 1) { h->u0 = -14 * 16; h->u1 = 170 * 16; h->w0 = 5 * 16; h->w1 = 24; h->lume_u0 = 24 * 16; h->lume_u1 = 150 * 16; h->lume_w = 2 * 16 + 8; }
        else { h->u0 = -34 * 16; h->u1 = 186 * 16; h->w0 = 22; h->w1 = 12; h->outline = false; h->shadow = false; }
        return true;
    case 2: /* DRESS: dauphine gold, no seconds */
        h->body = (wrgb_t){ 214, 178, 62 }; h->bevel = 110; h->outline = false; h->shadow = true;
        /* tips are half a pixel, never a point: see cov_span */
        if (hand == 0) { h->u0 = -10 * 16; h->u1 = 104 * 16; h->w0 = 6 * 16; h->w1 = 8; }
        else if (hand == 1) { h->u0 = -10 * 16; h->u1 = 166 * 16; h->w0 = 5 * 16; h->w1 = 8; }
        else return false;
        return true;
    case 3: /* PILOT: white sword hands with lume, hairline seconds */
        h->body = (wrgb_t){ 236, 236, 236 }; h->bevel = 40; h->outline = true; h->shadow = true;
        if (hand == 0) { h->u0 = -16 * 16; h->u1 = 108 * 16; h->w0 = 7 * 16; h->w1 = 2 * 16 + 8; h->lume_u0 = 22 * 16; h->lume_u1 = 92 * 16; h->lume_w = 3 * 16 + 8; }
        else if (hand == 1) { h->u0 = -16 * 16; h->u1 = 174 * 16; h->w0 = 6 * 16; h->w1 = 2 * 16; h->lume_u0 = 26 * 16; h->lume_u1 = 156 * 16; h->lume_w = 3 * 16; }
        else { h->u0 = -30 * 16; h->u1 = 184 * 16; h->w0 = 20; h->w1 = 10; h->outline = false; h->shadow = false; }
        return true;
    case 4: /* MINIMAL: thin blued steel, no seconds */
        h->body = (wrgb_t){ 70, 105, 175 }; h->bevel = 70; h->outline = false; h->shadow = true;
        if (hand == 0) { h->u0 = -8 * 16; h->u1 = 98 * 16; h->w0 = 2 * 16 + 8; h->w1 = 24; }
        else if (hand == 1) { h->u0 = -8 * 16; h->u1 = 162 * 16; h->w0 = 2 * 16; h->w1 = 16; }
        else return false;
        return true;
    case 5: /* FUTURE: slim cyan with a white core, hairline gold seconds */
        h->body = W_CYAN; h->bevel = 30; h->outline = false; h->shadow = false; h->core = true;
        if (hand == 0) { h->u0 = -10 * 16; h->u1 = 100 * 16; h->w0 = 3 * 16 + 8; h->w1 = 2 * 16; }
        else if (hand == 1) { h->u0 = -10 * 16; h->u1 = 166 * 16; h->w0 = 3 * 16; h->w1 = 24; }
        else { h->body = W_GOLD; h->core = false; h->u0 = -22 * 16; h->u1 = 192 * 16; h->w0 = 16; h->w1 = 10; }
        return true;
    case 6: /* SKELETON: rose gold over a busy movement; a dark outline and a
             * wide cream lume fill keep the hands readable, an ice-blue
             * hairline seconds is the one cool colour on the dial */
        h->body = (wrgb_t){ 232, 160, 128 }; h->bevel = 70; h->outline = true; h->shadow = true;
        if (hand == 0) { h->u0 = -14 * 16; h->u1 = 108 * 16; h->w0 = 8 * 16 + 8; h->w1 = 4 * 16 + 12; h->lume_u0 = 22 * 16; h->lume_u1 = 94 * 16; h->lume_w = 5 * 16; }
        else if (hand == 1) { h->u0 = -14 * 16; h->u1 = 168 * 16; h->w0 = 7 * 16 + 4; h->w1 = 3 * 16 + 10; h->lume_u0 = 26 * 16; h->lume_u1 = 152 * 16; h->lume_w = 4 * 16 + 4; }
        else { h->body = (wrgb_t){ 170, 225, 255 }; h->u0 = -34 * 16; h->u1 = 186 * 16; h->w0 = 36; h->w1 = 18; h->outline = false; h->shadow = false; }
        return true;
    case 7: /* CELESTIAL: slim polished gold leaves, a fine gold seconds */
        h->body = (wrgb_t){ 238, 196, 96 }; h->bevel = 110; h->outline = true; h->shadow = true;
        if (hand == 0) { h->u0 = -12 * 16; h->u1 = 104 * 16; h->w0 = 5 * 16; h->w1 = 8; }
        else if (hand == 1) { h->u0 = -12 * 16; h->u1 = 168 * 16; h->w0 = 4 * 16; h->w1 = 8; }
        else { h->body = (wrgb_t){ 255, 222, 130 }; h->u0 = -26 * 16; h->u1 = 190 * 16; h->w0 = 16; h->w1 = 8; h->outline = false; h->shadow = false; }
        return true;
    default:
        return false;
    }
}

void hud_watch_hand(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                    int style, int hand, uint32_t angle_q16, bool shadow)
{
    hand_spec_t h;
    if (dst == NULL || nrows <= 0 || !watch_hand_spec(style, hand, &h)) {
        return;
    }
    overlay_palette(swap_bytes);
    luts_build();
    h.shadow = h.shadow && shadow;
    const strip_t s = { dst, y0, y0 + nrows };
    draw_hand(&s, WATCH_CX16, WATCH_CY16, angle_q16, &h, 255, swap_bytes, NULL);
}

/* THE BALANCE WHEEL (SKELETON). A baked sprite (balance_wheel_data.c, cut by
 * firmware/mascot/gen_balance_wheel.py) rotated about the recess centre and
 * alpha-blended over the baked dial, under the hands. Integer only, no heap,
 * no framebuffer: each destination pixel is inverse-mapped into the sprite
 * with a fixed-point rotation walked along the row (no multiply per pixel for
 * the mapping), sampled bilinearly, and blended straight into the strip, so
 * a strip render equals the whole-frame render by construction. */
#define BAL_R_DEST      (JR_BALANCE_R + 1)         /* rows/cols visited       */
#define BAL_PERIOD_MS   1000                        /* one swing per second    */

/* The wheel needs a true rotation: the shared sine LUT above is a coarse
 * Bhaskara fit (good for a hand, but its two quarter-wave halves are one step
 * apart, so sin and cos at 45 degrees are not equal and the sprite would
 * shear by a pixel at the rim). This is sin(pi * i / 128) for i = 0..64,
 * rounded, mirrored into the other quadrants, linearly interpolated: error
 * under 1e-4 of full scale. */
static const int16_t k_bal_sin[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767,
};

static int32_t bal_sin(uint32_t a16)
{
    const uint32_t q = (a16 >> 14) & 3u;
    const uint32_t w = a16 & 0x3FFFu;
    const uint32_t pos = (q & 1u) ? 0x4000u - w : w;           /* 0..16384 */
    const uint32_t i = pos >> 8, f = pos & 255u;
    const int32_t s0 = k_bal_sin[i];
    const int32_t s1 = k_bal_sin[i < 64u ? i + 1u : 64u];
    const int32_t v = s0 + (((s1 - s0) * (int32_t)f) >> 8);
    return (q & 2u) ? -v : v;
}

static inline int32_t bal_cos(uint32_t a16)
{
    return bal_sin(a16 + 0x4000u);
}

int32_t hud_balance_angle_q16(int ss, int ms)
{
    ss = ((ss % 60) + 60) % 60;
    ms = ms < 0 ? 0 : ms > 999 ? 999 : ms;
    const int t = (ss * 1000 + ms) % BAL_PERIOD_MS;
    const uint32_t phase = (uint32_t)(((int64_t)t * 65536) / BAL_PERIOD_MS);
    return (int32_t)(((int64_t)HUD_BALANCE_SWING_Q16 * bal_sin(phase)) >> 15);
}

static inline uint16_t bal_blend(uint16_t d, uint16_t sp, uint32_t a5)
{
    /* both in host order; a5 0..32. The 0x07E0F81F idiom blends the three
     * fields with one multiply. */
    const uint32_t dd = ((uint32_t)d | ((uint32_t)d << 16)) & 0x07E0F81Fu;
    const uint32_t ss = ((uint32_t)sp | ((uint32_t)sp << 16)) & 0x07E0F81Fu;
    const uint32_t r = (((ss - dd) * a5 >> 5) + dd) & 0x07E0F81Fu;
    return (uint16_t)(r | (r >> 16));
}

/* JR_KNOB_BAL_CLASS. The sprite's two byte tables are 63 KB, read through the
 * cache four taps a pixel at whatever angle the wheel has, so the cost on the
 * device may be the reads, not the sums. This keeps, per source cell, what its
 * 2x2 alpha taps are: all clear (skip), all 255 (alpha is 255, no alpha reads
 * or sums), or anything else (the long way). Two bits a cell, 5.3 KB, owned by
 * the caller like the dial record (hud_balance_cache_set); built on first use.
 * The pixels are the same: four 255 taps weigh to 255 whatever the fraction. */
#define BAL_CLS_CLEAR   0u
#define BAL_CLS_SOLID   1u
#define BAL_CLS_PART    2u
#define BAL_CLS_BYTES   ((JR_BALANCE_SIZE * JR_BALANCE_SIZE + 3) / 4)
static uint8_t *s_bal_cls;
static bool s_bal_cls_built;

void hud_balance_cache_set(uint8_t *buf, size_t cap_bytes)
{
    s_bal_cls = (buf != NULL && cap_bytes >= (size_t)BAL_CLS_BYTES) ? buf : NULL;
    s_bal_cls_built = false;
}

static void bal_cls_build(void)
{
    memset(s_bal_cls, 0, BAL_CLS_BYTES);
    for (int iy = 0; iy < JR_BALANCE_SIZE; ++iy) {
        for (int ix = 0; ix < JR_BALANCE_SIZE; ++ix) {
            const size_t k = (size_t)iy * JR_BALANCE_SIZE + (size_t)ix;
            uint32_t cls = BAL_CLS_PART;        /* the last row and column are never a cell's origin */
            if (ix + 1 < JR_BALANCE_SIZE && iy + 1 < JR_BALANCE_SIZE) {
                const uint32_t a00 = jr_balance_alpha[k], a10 = jr_balance_alpha[k + 1];
                const uint32_t a01 = jr_balance_alpha[k + JR_BALANCE_SIZE];
                const uint32_t a11 = jr_balance_alpha[k + JR_BALANCE_SIZE + 1];
                if ((a00 | a10 | a01 | a11) == 0u) {
                    cls = BAL_CLS_CLEAR;
                } else if ((a00 & a10 & a01 & a11) == 255u) {
                    cls = BAL_CLS_SOLID;
                }
            }
            s_bal_cls[k >> 2] = (uint8_t)(s_bal_cls[k >> 2] | (cls << ((k & 3u) * 2u)));
        }
    }
    s_bal_cls_built = true;
}

static int balance_blit_cls(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                            int32_t angle_q16)
{
    const uint8_t *const cm = s_bal_cls;
    const int cx = JR_BALANCE_DIAL_CX, cy = JR_BALANCE_DIAL_CY;
    const int ya = y0 > cy - BAL_R_DEST ? y0 : cy - BAL_R_DEST;
    const int yb = (y0 + nrows - 1) < cy + BAL_R_DEST ? (y0 + nrows - 1) : cy + BAL_R_DEST;
    if (ya > yb) {
        return 0;
    }
    const uint32_t a16 = (uint32_t)angle_q16 & 0xFFFFu;
    const int32_t c = bal_cos(a16), sn = bal_sin(a16);          /* Q15 */
    const int32_t base = (int32_t)JR_BALANCE_C << 15;
    int visited = 0;
    for (int y = ya; y <= yb; ++y) {
        const int dy = y - cy;
        const int rem = BAL_R_DEST * BAL_R_DEST - dy * dy;
        int hx = 0;
        while ((hx + 1) * (hx + 1) <= rem) {
            ++hx;
        }
        const int x0 = cx - hx, x1 = cx + hx;
        int32_t u = base + (x0 - cx) * c + dy * sn;
        int32_t v = base - (x0 - cx) * sn + dy * c;
        uint16_t *px = dst + (size_t)(y - y0) * HUD_W + x0;
        for (int x = x0; x <= x1; ++x, ++px, u += c, v -= sn) {
            ++visited;
            const int ix = u >> 15, iy = v >> 15;
            const size_t k = (size_t)iy * JR_BALANCE_SIZE + (size_t)ix;
            const uint32_t cls = (cm[k >> 2] >> ((k & 3u) * 2u)) & 3u;
            if (cls == BAL_CLS_CLEAR) {
                continue;                       /* a window in the wheel */
            }
            const uint32_t fx = (uint32_t)(u >> 7) & 255u, fy = (uint32_t)(v >> 7) & 255u;
            const uint32_t w00 = (256u - fx) * (256u - fy), w10 = fx * (256u - fy);
            const uint32_t w01 = (256u - fx) * fy, w11 = fx * fy;      /* sum 65536 */
            uint32_t a = 255u;                  /* four 255s weigh to 255 */
            if (cls != BAL_CLS_SOLID) {
                const uint32_t a00 = jr_balance_alpha[k], a10 = jr_balance_alpha[k + 1];
                const uint32_t a01 = jr_balance_alpha[k + JR_BALANCE_SIZE];
                const uint32_t a11 = jr_balance_alpha[k + JR_BALANCE_SIZE + 1];
                a = (a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11 + 32768u) >> 16;
                if (a < 4u) {
                    continue;
                }
            }
            const uint16_t p00 = jr_balance_rgb[k], p10 = jr_balance_rgb[k + 1];
            const uint16_t p01 = jr_balance_rgb[k + JR_BALANCE_SIZE];
            const uint16_t p11 = jr_balance_rgb[k + JR_BALANCE_SIZE + 1];
            const uint32_t r5 = (((p00 >> 11) & 31u) * w00 + ((p10 >> 11) & 31u) * w10 +
                                 ((p01 >> 11) & 31u) * w01 + ((p11 >> 11) & 31u) * w11 + 32768u) >> 16;
            const uint32_t g6 = (((p00 >> 5) & 63u) * w00 + ((p10 >> 5) & 63u) * w10 +
                                 ((p01 >> 5) & 63u) * w01 + ((p11 >> 5) & 63u) * w11 + 32768u) >> 16;
            const uint32_t b5 = ((p00 & 31u) * w00 + (p10 & 31u) * w10 +
                                 (p01 & 31u) * w01 + (p11 & 31u) * w11 + 32768u) >> 16;
            const uint16_t sp = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            const uint32_t a5 = (a + 4u) >> 3;                          /* 0..32 */
            if (a5 >= 32u) {
                *px = swap_bytes ? (uint16_t)((sp >> 8) | (sp << 8)) : sp;
                continue;
            }
            const uint16_t d = swap_bytes ? (uint16_t)((*px >> 8) | (*px << 8)) : *px;
            const uint16_t o = bal_blend(d, sp, a5);
            *px = swap_bytes ? (uint16_t)((o >> 8) | (o << 8)) : o;
        }
    }
    return visited;
}

/* The wheel as it was: four alpha taps a pixel. */
int hud_balance_blit(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                     int32_t angle_q16)
{
    if (dst == NULL || nrows <= 0) {
        return 0;
    }
    if ((hud_perf_knob & JR_KNOB_BAL_CLASS) != 0u && s_bal_cls != NULL) {
        if (!s_bal_cls_built) {
            bal_cls_build();
        }
        return balance_blit_cls(dst, y0, nrows, swap_bytes, angle_q16);
    }
    const int cx = JR_BALANCE_DIAL_CX, cy = JR_BALANCE_DIAL_CY;
    const int ya = y0 > cy - BAL_R_DEST ? y0 : cy - BAL_R_DEST;
    const int yb = (y0 + nrows - 1) < cy + BAL_R_DEST ? (y0 + nrows - 1) : cy + BAL_R_DEST;
    if (ya > yb) {
        return 0;
    }
    const uint32_t a16 = (uint32_t)angle_q16 & 0xFFFFu;
    const int32_t c = bal_cos(a16), sn = bal_sin(a16);          /* Q15 */
    const int32_t base = (int32_t)JR_BALANCE_C << 15;
    int visited = 0;
    for (int y = ya; y <= yb; ++y) {
        const int dy = y - cy;
        const int rem = BAL_R_DEST * BAL_R_DEST - dy * dy;
        int hx = 0;
        while ((hx + 1) * (hx + 1) <= rem) {
            ++hx;
        }
        const int x0 = cx - hx, x1 = cx + hx;
        int32_t u = base + (x0 - cx) * c + dy * sn;
        int32_t v = base - (x0 - cx) * sn + dy * c;
        uint16_t *px = dst + (size_t)(y - y0) * HUD_W + x0;
        for (int x = x0; x <= x1; ++x, ++px, u += c, v -= sn) {
            ++visited;
            const int ix = u >> 15, iy = v >> 15;
            const size_t k = (size_t)iy * JR_BALANCE_SIZE + (size_t)ix;
            const uint32_t a00 = jr_balance_alpha[k], a10 = jr_balance_alpha[k + 1];
            const uint32_t a01 = jr_balance_alpha[k + JR_BALANCE_SIZE];
            const uint32_t a11 = jr_balance_alpha[k + JR_BALANCE_SIZE + 1];
            if ((a00 | a10 | a01 | a11) == 0u) {
                continue;                       /* a window in the wheel */
            }
            const uint32_t fx = (uint32_t)(u >> 7) & 255u, fy = (uint32_t)(v >> 7) & 255u;
            const uint32_t w00 = (256u - fx) * (256u - fy), w10 = fx * (256u - fy);
            const uint32_t w01 = (256u - fx) * fy, w11 = fx * fy;      /* sum 65536 */
            const uint32_t a = (a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11 + 32768u) >> 16;
            if (a < 4u) {
                continue;
            }
            const uint16_t p00 = jr_balance_rgb[k], p10 = jr_balance_rgb[k + 1];
            const uint16_t p01 = jr_balance_rgb[k + JR_BALANCE_SIZE];
            const uint16_t p11 = jr_balance_rgb[k + JR_BALANCE_SIZE + 1];
            const uint32_t r5 = (((p00 >> 11) & 31u) * w00 + ((p10 >> 11) & 31u) * w10 +
                                 ((p01 >> 11) & 31u) * w01 + ((p11 >> 11) & 31u) * w11 + 32768u) >> 16;
            const uint32_t g6 = (((p00 >> 5) & 63u) * w00 + ((p10 >> 5) & 63u) * w10 +
                                 ((p01 >> 5) & 63u) * w01 + ((p11 >> 5) & 63u) * w11 + 32768u) >> 16;
            const uint32_t b5 = ((p00 & 31u) * w00 + (p10 & 31u) * w10 +
                                 (p01 & 31u) * w01 + (p11 & 31u) * w11 + 32768u) >> 16;
            const uint16_t sp = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            const uint32_t a5 = (a + 4u) >> 3;                          /* 0..32 */
            if (a5 >= 32u) {
                *px = swap_bytes ? (uint16_t)((sp >> 8) | (sp << 8)) : sp;
                continue;
            }
            const uint16_t d = swap_bytes ? (uint16_t)((*px >> 8) | (*px << 8)) : *px;
            const uint16_t o = bal_blend(d, sp, a5);
            *px = swap_bytes ? (uint16_t)((o >> 8) | (o << 8)) : o;
        }
    }
    return visited;
}

/* THE STARS (CELESTIAL). A few of the dial's brightest baked stars get a slow
 * glint: additive, a cool white, full on the star pixel, 5/8 on its four
 * neighbours, then (as the glint peaks) 3/8 on the diagonals and 1/4 on the
 * arm tips: a soft five-pixel cross at most, each star
 * on its own period (2 to 5 s) and phase. The level is a squared sine, so a
 * star spends most of its time dark and rises to a short soft flash: the
 * dial reads as alive without anything blinking. The table is picked at bake
 * time from the baked frame (gen_celestial_stars.py). Pure functions of the
 * clock, no state, per-star row culling: a strip render equals the whole
 * frame. */
int hud_celestial_star_level(int i, int ss, int ms)
{
    if (i < 0 || i >= HUD_STAR_COUNT) {
        return 0;
    }
    luts_build();
    ss = ((ss % 60) + 60) % 60;
    ms = ms < 0 ? 0 : ms > 999 ? 999 : ms;
    const hud_star_t *st = &s_stars[i];
    const int t = (ss * 1000 + ms) % (int)st->period_ms;
    const uint32_t ph = (((uint32_t)t * 65536u) / st->period_ms + st->phase_q16) & 0xFFFFu;
    const int l = (sin_q16(ph) + 32768) >> 8;                  /* 0..255 */
    return ((l * l >> 8) * st->strength) >> 8;                 /* 0..254 */
}

static inline void star_add(uint16_t *px, int add_r, int add_g, int add_b, bool swap)
{
    const uint16_t p = swap ? (uint16_t)((*px >> 8) | (*px << 8)) : *px;
    int r = ((p >> 11) & 31) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
    r += add_r; g += add_g; b += add_b;
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    const uint16_t o = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    *px = swap ? (uint16_t)((o >> 8) | (o << 8)) : o;
}

int hud_celestial_twinkle(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                          int ss, int ms)
{
    if (dst == NULL || nrows <= 0) {
        return 0;
    }
    int touched = 0;
    for (int i = 0; i < HUD_STAR_COUNT; ++i) {
        const int sy = s_stars[i].y, sx = s_stars[i].x;
        if (sy + 2 < y0 || sy - 2 >= y0 + nrows) {
            continue;                                   /* not in this strip */
        }
        const int a = hud_celestial_star_level(i, ss, ms);
        if (a < 12) {
            continue;
        }
        for (int dy = -2; dy <= 2; ++dy) {
            const int y = sy + dy;
            if (y < y0 || y >= y0 + nrows) {
                continue;
            }
            for (int dx = -2; dx <= 2; ++dx) {
                const int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);   /* 0..4 */
                int k;                                  /* 0..256 of the glint */
                if (d == 0) {
                    k = 256;
                } else if (d == 1) {
                    k = 160;
                } else if (d == 2 && dx != 0 && dy != 0 && a > 100) {
                    k = 96;                             /* the diagonals */
                } else if (d == 2 && a > 140) {
                    k = 64;                             /* the arm tips, brightest only */
                } else {
                    continue;
                }
                const int amp = (a * k) >> 8;
                star_add(dst + (size_t)(y - y0) * HUD_W + (sx + dx),
                         (amp * 190) >> 9, (amp * 210) >> 9, (amp * 255) >> 9, swap_bytes);
                ++touched;
            }
        }
    }
    return touched;
}

/* What a frame of the watch derives from its inputs, kept across the
 * strips of that frame (hud_overlay_watch). Single writer, the render
 * task; the host probes are single-threaded. */
typedef struct {
    bool        valid;
    int         style, hh, mm, ss, ms;   /* the inputs it was built from   */
    bool        shadow;
    uint32_t    a_h, a_m, a_s;           /* Q16 turns from 12 o'clock       */
    int         sec_permille;            /* the seconds sub-dial's reading  */
    hand_spec_t h[3];
    bool        has[3];
} watch_frame_t;

static watch_frame_t s_wf;

/* A needle in a PILOT sub-dial: (cx, cy) Q4, value 0..1000 across a 270
 * degree throw from 7:30 to 4:30, or a full turn for `full`. */
static void sub_needle(const strip_t *s, int32_t cx, int32_t cy, int rad,
                       int permille, bool full, bool shadow, int A, bool swap,
                       const hud_light_t *lt)
{
    uint32_t a;
    if (full) {
        a = WATCH_A12 + (uint32_t)(((int64_t)permille * 65536) / 1000);
    } else {
        a = (96u << 8) + (uint32_t)(((int64_t)permille * (192 << 8)) / 1000);
    }
    hand_spec_t h = {
        .u0 = -(rad * 16) / 5, .u1 = (rad - 7) * 16, .w0 = 28, .w1 = 8,
        .body = W_WHITE, .bevel = 20, .outline = false, .shadow = shadow,
    };
    draw_hand(s, cx, cy, a, &h, (A * 255) >> 8, swap, lt);
    aa_disc(s, cx, cy, 3 * 16, W_WHITE, A, swap);
}

void hud_overlay_watch(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                       const hud_watch_t *w, int strength, int style)
{
    if (dst == NULL || nrows <= 0 || strength <= 0 || w == NULL) {
        return;
    }
    if (style <= 0 || style > 7) {
        hud_overlay_clock(dst, y0, nrows, swap_bytes, w->hh, w->mm, w->ss, strength);
        return;
    }
    if (y0 > 232 + WATCH_R_MAX + 2 || y0 + nrows <= 232 - WATCH_R_MAX - 2) {
        return;
    }
    if (strength > 255) {
        strength = 255;
    }
    overlay_palette(swap_bytes);
    const strip_t s = { dst, y0, y0 + nrows };
    const int A = strength >= 255 ? 256 : strength + 1;

    /* ONCE PER FRAME, NOT PER STRIP. This is a strip call with no frame
     * hook, so the derivations — three angles, three hand specs — are kept
     * across calls and rebuilt only when their inputs change; every strip
     * of a frame carries the same inputs, so a frame solves them once. Pure
     * functions of the inputs: a strip render equals the whole-frame one. */
    watch_frame_t *f = &s_wf;
    if (!f->valid || f->style != style || f->hh != w->hh || f->mm != w->mm ||
        f->ss != w->ss || f->ms != w->ms || f->shadow != w->shadow) {
        const int hh = ((w->hh % 24) + 24) % 24;
        const int mm = ((w->mm % 60) + 60) % 60;
        const int ss = ((w->ss % 60) + 60) % 60;
        const int ms = w->ms < 0 ? 0 : w->ms > 999 ? 999 : w->ms;
        /* angles in Q16 turns from 12 o'clock */
        f->a_h = WATCH_A12 + (uint32_t)(((int64_t)((hh % 12) * 3600 + mm * 60 + ss) * 65536) / 43200);
        f->a_m = WATCH_A12 + (uint32_t)(((int64_t)(mm * 60 + ss) * 65536) / 3600);
        f->a_s = WATCH_A12 + (uint32_t)(((int64_t)(ss * 1000 + ms) * 65536) / 60000);
        f->sec_permille = (ss * 1000 + ms) / 60;
        for (int i = 0; i < 3; ++i) {
            f->has[i] = watch_hand_spec(style, i, &f->h[i]);
            /* SHADOWS AT THE AWAKE CADENCE ONLY. The shadow is the widest
             * polygon of a hand at 40 %, every pixel a read-modify-write;
             * at rest the ladder has already slowed the frame, and nobody
             * is looking for a shadow on a resting watch. */
            f->h[i].shadow = f->h[i].shadow && w->shadow;
        }
        f->style = style;
        f->hh = w->hh; f->mm = w->mm; f->ss = w->ss; f->ms = w->ms;
        f->shadow = w->shadow;
        f->valid = true;
    }
    const uint32_t a_h = f->a_h, a_m = f->a_m, a_s = f->a_s;
    const hud_light_t *lt = w->light;      /* NULL = the rest light */

    if (!w->dial_baked) {
        fallback_dial(&s, style, A, swap_bytes);
    }

    /* The two live ornaments sit on the baked dial and under every hand. They
     * move only at the awake cadence (w->alive); a resting watch holds them
     * still, like the shadows. */
    if (style == 6 && w->dial_baked) {
        (void)hud_balance_blit(dst, y0, nrows, swap_bytes,
                               w->alive ? hud_balance_angle_q16(w->ss, w->ms) : 0);
    } else if (style == 7 && w->dial_baked && w->alive) {
        (void)hud_celestial_twinkle(dst, y0, nrows, swap_bytes, w->ss, w->ms);
    }

    if (style == 3) {
        /* PILOT complications: 6 = seconds, 9 = battery, 3 = temperature */
        sub_needle(&s, s_subdial[1].cx, s_subdial[1].cy, s_subdial[1].r,
                   f->sec_permille, true, w->shadow, A, swap_bytes, lt);
        if (w->batt_pct >= 0) {
            sub_needle(&s, s_subdial[2].cx, s_subdial[2].cy, s_subdial[2].r,
                       (w->batt_pct > 100 ? 100 : w->batt_pct) * 10, false,
                       w->shadow, A, swap_bytes, lt);
        }
        if (w->wx_valid) {
            int t = w->temp_f < 40 ? 40 : w->temp_f > 100 ? 100 : w->temp_f;
            sub_needle(&s, s_subdial[0].cx, s_subdial[0].cy, s_subdial[0].r,
                       (t - 40) * 1000 / 60, false, w->shadow, A, swap_bytes, lt);
        } else {
            sub_needle(&s, s_subdial[0].cx, s_subdial[0].cy, s_subdial[0].r,
                       0, false, w->shadow, (A * 90) >> 8, swap_bytes, lt);
        }
    }

    if (f->has[0]) {
        draw_hand(&s, WATCH_CX16, WATCH_CY16, a_h, &f->h[0], strength, swap_bytes, lt);
    }
    if (style == 1) {
        /* the Mercedes: a lume disc in the hour hand near its tip */
        int32_t x, y;
        w_polar(84 * 16, a_h, &x, &y);
        aa_disc(&s, x, y, 11 * 16, (wrgb_t){ 205, 210, 215 }, A, swap_bytes);
        aa_disc(&s, x, y, 9 * 16, W_CREAM, A, swap_bytes);
        aa_disc(&s, x, y, 3 * 16, (wrgb_t){ 205, 210, 215 }, A, swap_bytes);
    }
    if (f->has[1]) {
        draw_hand(&s, WATCH_CX16, WATCH_CY16, a_m, &f->h[1], strength, swap_bytes, lt);
    }
    if (f->has[2]) {
        draw_hand(&s, WATCH_CX16, WATCH_CY16, a_s, &f->h[2], strength, swap_bytes, lt);
        int32_t x, y;
        if (style == 1) {
            w_polar(150 * 16, a_s, &x, &y);
            aa_disc(&s, x, y, 7 * 16, (wrgb_t){ 205, 210, 215 }, A, swap_bytes);
            aa_disc(&s, x, y, 5 * 16, W_CREAM, A, swap_bytes);
            w_polar(-28 * 16, a_s, &x, &y);
            aa_disc(&s, x, y, 4 * 16, (wrgb_t){ 205, 210, 215 }, A, swap_bytes);
        } else if (style == 3) {
            w_polar(-24 * 16, a_s, &x, &y);
            aa_disc(&s, x, y, 4 * 16, W_WHITE, A, swap_bytes);
        }
    }

    switch (style) {
    /* Hubs on a baked dial are >= 12 px: the concept art the dials come from
     * has a centre hole of about 10 px that must never show. */
    case 1: w_hub(&s, 12 * 16, (wrgb_t){ 205, 210, 215 }, A, swap_bytes, lt); break;
    case 2: w_hub(&s, 12 * 16, (wrgb_t){ 214, 178, 62 }, A, swap_bytes, lt); break;
    case 3: w_hub(&s, 12 * 16, W_WHITE, A, swap_bytes, lt); break;
    case 4: w_hub(&s, 4 * 16, (wrgb_t){ 70, 105, 175 }, A, swap_bytes, lt); break;
    case 6: w_hub(&s, 13 * 16, (wrgb_t){ 232, 160, 128 }, A, swap_bytes, lt); break;
    case 7: w_hub(&s, 12 * 16, (wrgb_t){ 238, 196, 96 }, A, swap_bytes, lt); break;
    default:
        aa_disc(&s, WATCH_CX16, WATCH_CY16, 20 * 16, W_CYAN, (40 * A) >> 8, swap_bytes);
        aa_disc(&s, WATCH_CX16, WATCH_CY16, 15 * 16, W_CYAN, (90 * A) >> 8, swap_bytes);
        aa_disc(&s, WATCH_CX16, WATCH_CY16, 12 * 16, W_CYAN, A, swap_bytes);
        aa_disc(&s, WATCH_CX16, WATCH_CY16, 4 * 16, W_WHITE, A, swap_bytes);
        break;
    }
}

void hud_overlay_clock_style(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                             int hh, int mm, int ss, int strength, int style)
{
    if (style <= 0 || style > 7) {
        hud_overlay_clock(dst, y0, nrows, swap_bytes, hh, mm, ss, strength);
        return;
    }
    const hud_watch_t w = {
        .hh = hh, .mm = mm, .ss = ss, .ms = 0,
        .batt_pct = -1, .wx_valid = false, .temp_f = 0, .dial_baked = false,
        .shadow = true,
    };
    hud_overlay_watch(dst, y0, nrows, swap_bytes, &w, strength, style);
}

void hud_overlay_clock(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                       int hh, int mm, int ss, int strength)
{
    if (dst == NULL || nrows <= 0 || strength <= 0) {
        return;                      /* strength 0 must provably paint nothing */
    }
    if (strength > 255) {
        strength = 255;
    }
    /* coarse strip cull: the watch lives in rows 232 +/- (R_SEC + 2) —
     * the seconds hand is the longest element. */
    if (y0 > 232 + HUD_CLOCK_R_SEC + 2 ||
        y0 + nrows <= 232 - HUD_CLOCK_R_SEC - 2) {
        return;
    }
    /* fold, never trust: the composition root clamps, but this is public */
    hh = ((hh % 24) + 24) % 24;
    mm = ((mm % 60) + 60) % 60;
    ss = ((ss % 60) + 60) % 60;

    overlay_palette(swap_bytes);
    const strip_t s = { dst, y0, y0 + nrows };

    /* 12 o'clock is a=192; clockwise on glass is increasing a, so minutes
     * and hours sweep exactly like the LUT convention wants:
     *   mm=15 -> 192+64 = 0 (3 o'clock), hh=6 -> 192+128 = 64 (6 o'clock). */
    const int a_m = (192 + (mm * 256) / 60) & 255;
    const int a_h = (192 + (((hh % 12) * 60 + mm) * 256) / 720) & 255;
    const int a_s = (192 + (ss * 256) / 60) & 255;

    /* One strength scales all three hands and the hub, so the whole watch
     * fades as one object — hands emerging at different rates would read as
     * the two-competing-HUDs failure all over again. */
    const uint16_t px_min  = pack565(strength, strength, strength, swap_bytes);
    const uint16_t px_hour = shade(s_ov_cyan, strength);
    const uint16_t px_sec  = shade(s_ov_gold, strength);

    /* hour first, minute second: an overlapping pair reads minute-on-top.
     * The gold seconds hand goes last and thinnest — a 1 Hz heartbeat that
     * makes the resting watch read as alive rather than frozen. It reaches
     * past the minute hand toward the baked bezel ticks. */
    for (int r = HUD_CLOCK_R_IN; r <= HUD_CLOCK_R_HOUR; ++r) {
        polar_dot(&s, a_h, r, px_hour, 3);
    }
    for (int r = HUD_CLOCK_R_IN; r <= HUD_CLOCK_R_MIN; ++r) {
        polar_dot(&s, a_m, r, px_min, 3);
    }
    for (int r = HUD_CLOCK_R_IN; r <= HUD_CLOCK_R_SEC; ++r) {
        polar_dot(&s, a_s, r, px_sec, 1);
    }

    /* filled white hub, solved per row like every other disc here */
    for (int dy = -HUD_CLOCK_R_HUB; dy <= HUD_CLOCK_R_HUB; ++dy) {
        const int y = 232 + dy;
        if (y < s.y0 || y >= s.y1) {
            continue;
        }
        const int half = (int)isqrt32(
            (uint32_t)(HUD_CLOCK_R_HUB * HUD_CLOCK_R_HUB - dy * dy));
        uint16_t *row = dst + (size_t)(y - y0) * HUD_W;
        for (int x = 232 - half; x <= 232 + half; ++x) {
            row[x] = px_min;
        }
    }
}
