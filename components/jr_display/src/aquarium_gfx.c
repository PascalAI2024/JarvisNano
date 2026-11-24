/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * aquarium_gfx.c — 466x466 Procedural OLED "Nano Reef" Virtual Aquarium
 * Renderer & Fixed-Point Physics Engine.
 *
 * Pure C11, integer fixed-point only, zero heap allocation, zero libm,
 * < 400 B stack, 100% strip-invariant across arbitrary [y0, y0 + nrows),
 * strictly clipped inside r <= JR_AQ_R_CLIP (213 < JR_DISPLAY_SHELL_R_MAX 214).
 */
#include "aquarium_gfx.h"

#include <stddef.h>
#include <string.h>

/* ==========================================================================
 * CONSTANTS & PRECOMPUTED .RODATA TABLES
 * ========================================================================== */

#define AQ_BIOME_REEF      0u
#define AQ_BIOME_ABYSS     1u
#define AQ_BIOME_KOI       2u

#define AQ_SP_CLOWN        0u
#define AQ_SP_TANG         1u
#define AQ_SP_TETRA        2u
#define AQ_SP_KOI          3u
#define AQ_SP_MANDARIN     4u
#define AQ_SP_JELLY        5u
#define AQ_SP_MANTA        6u
#define AQ_SP_SEAHORSE     7u

#define AQ_MAX_CORAL_SEGS  48
#define AQ_MAX_POLYPS      44
#define AQ_NUM_KELP         6
#define AQ_KELP_KNOTS      17
#define AQ_NUM_MOTES       20
#define AQ_NUM_BEAMS        4
#define AQ_NUM_LILIES       4
#define AQ_MAX_WAKES       12

#define AQ_RIPPLE_MS      900u
#define AQ_SHAKE_MS      1200u
#define AQ_TOAST_MS      2200u
#define AQ_TOAST_FADE_MS  400u
#define AQ_WAKE_MS        950u

/* Quarter-wave sine LUT: round(sin(i * pi / 128) * 16384), i = 0..64. */
static const int16_t s_aq_sin_q14[65] = {
        0,   402,   804,  1205,  1606,  2006,  2404,  2801,
     3196,  3590,  3981,  4370,  4756,  5139,  5520,  5897,
     6270,  6639,  7005,  7366,  7723,  8076,  8423,  8765,
     9102,  9434,  9760, 10080, 10394, 10702, 11003, 11297,
    11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395,
    13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978,
    15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986,
    16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379,
    16384,
};

/* Exact floor(sqrt(213^2 - dy^2)) for dy = 0..213 about (233, 233).
 * Guarantees every drawn pixel has r < 214.0 about both (233,233) & (232.5,232.5). */
static const uint8_t s_aq_clip_half[214] = {
    213, 212, 212, 212, 212, 212, 212, 212, 212, 212, 212, 212, 212, 212, 212, 212,
    212, 212, 212, 212, 212, 211, 211, 211, 211, 211, 211, 211, 211, 211, 210, 210,
    210, 210, 210, 210, 209, 209, 209, 209, 209, 209, 208, 208, 208, 208, 207, 207,
    207, 207, 207, 206, 206, 206, 206, 205, 205, 205, 204, 204, 204, 204, 203, 203,
    203, 202, 202, 202, 201, 201, 201, 200, 200, 200, 199, 199, 198, 198, 198, 197,
    197, 196, 196, 196, 195, 195, 194, 194, 193, 193, 193, 192, 192, 191, 191, 190,
    190, 189, 189, 188, 188, 187, 186, 186, 185, 185, 184, 184, 183, 182, 182, 181,
    181, 180, 179, 179, 178, 177, 177, 176, 175, 175, 174, 173, 173, 172, 171, 170,
    170, 169, 168, 167, 167, 166, 165, 164, 163, 163, 162, 161, 160, 159, 158, 157,
    156, 156, 155, 154, 153, 152, 151, 150, 149, 148, 147, 146, 145, 143, 142, 141,
    140, 139, 138, 137, 135, 134, 133, 132, 130, 129, 128, 126, 125, 124, 122, 121,
    119, 118, 116, 115, 113, 112, 110, 108, 107, 105, 103, 101, 100,  98,  96,  94,
     92,  90,  87,  85,  83,  80,  78,  75,  73,  70,  67,  64,  61,  57,  54,  50,
     45,  41,  35,  29,  20,   0,
};

/* Growth stage scales in Q8 (256 = 1.00x). */
static const uint16_t s_aq_stage_scale[4] = { 159u, 210u, 256u, 312u };

/* ==========================================================================
 * PURE INTEGER MATH & RGB565 BLENDING PRIMITIVES
 * ========================================================================== */

static inline int32_t aq_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int32_t aq_abs(int32_t v)
{
    return v < 0 ? -v : v;
}

static inline int32_t aq_min(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static inline int32_t aq_max(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

/* Sine of angle in 0..255 (wraps), returns Q14 in [-16384..+16384]. */
static inline int32_t aq_sin(int32_t a)
{
    const uint32_t idx = (uint32_t)a & 255u;
    if (idx <= 64u) {
        return s_aq_sin_q14[idx];
    }
    if (idx <= 128u) {
        return s_aq_sin_q14[128u - idx];
    }
    if (idx <= 192u) {
        return -s_aq_sin_q14[idx - 128u];
    }
    return -s_aq_sin_q14[256u - idx];
}

static inline int32_t aq_cos(int32_t a)
{
    return aq_sin(a + 64);
}

static uint32_t aq_isqrt(uint32_t n)
{
    if (n == 0u) {
        return 0u;
    }
    uint32_t r = 0u;
    uint32_t bit = 1u << 30;
    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static inline uint32_t aq_hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static inline uint16_t aq_bswap(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t aq_rgb565(int r, int g, int b)
{
    r = aq_clamp(r, 0, 255);
    g = aq_clamp(g, 0, 255);
    b = aq_clamp(b, 0, 255);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t aq_lerp565(uint16_t c0, uint16_t c1, int t)
{
    if (t <= 0) {
        return c0;
    }
    if (t >= 255) {
        return c1;
    }
    const int r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
    const int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    const int r = r0 + (((r1 - r0) * t) >> 8);
    const int g = g0 + (((g1 - g0) * t) >> 8);
    const int b = b0 + (((b1 - b0) * t) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t aq_blend565(uint16_t under, uint16_t over, unsigned a, bool swap)
{
    if (a == 0u) {
        return under;
    }
    if (a >= 255u) {
        return swap ? aq_bswap(over) : over;
    }
    const uint32_t wo = (a + 2u) >> 2; /* 0..64 */
    const uint32_t wu = 64u - wo;
    const uint32_t u = swap ? aq_bswap(under) : under;
    const uint32_t rb = ((u & 0xF81Fu) * wu + (uint32_t)(over & 0xF81Fu) * wo + 0x10020u) >> 6;
    const uint32_t g  = ((u & 0x07E0u) * wu + (uint32_t)(over & 0x07E0u) * wo + (32u << 5)) >> 6;
    const uint16_t out = (uint16_t)((rb & 0xF81Fu) | (g & 0x07E0u));
    return swap ? aq_bswap(out) : out;
}

static inline uint16_t aq_add565(uint16_t under, uint16_t over, unsigned a, bool swap)
{
    if (a == 0u) {
        return under;
    }
    if (a > 255u) {
        a = 255u;
    }
    const uint32_t u = swap ? aq_bswap(under) : under;
    uint32_t r = ((u >> 11) & 31u) + ((((over >> 11) & 31u) * a + 127u) >> 8);
    uint32_t g = ((u >> 5) & 63u)  + ((((over >> 5) & 63u)  * a + 127u) >> 8);
    uint32_t b = (u & 31u)         + (((over & 31u)         * a + 127u) >> 8);
    if (r > 31u) { r = 31u; }
    if (g > 63u) { g = 63u; }
    if (b > 31u) { b = 31u; }
    const uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return swap ? aq_bswap(out) : out;
}

static inline bool aq_clip_row(int y, int *lo, int *hi)
{
    const int dy = y - JR_AQ_CY;
    const int ady = dy < 0 ? -dy : dy;
    if (ady > JR_AQ_R_CLIP) {
        return false;
    }
    const int half = (int)s_aq_clip_half[ady];
    *lo = JR_AQ_CX - half;
    *hi = JR_AQ_CX + half;
    return true;
}

/* ==========================================================================
 * SIMULATION & PRECOMPUTED FRAME STRUCTURES (STATIC BSS, ZERO HEAP)
 * ========================================================================== */

typedef struct {
    bool     active;
    uint8_t  species;
    uint8_t  stage;
    uint8_t  hunger;
    uint16_t seed;
    int32_t  x_q8, y_q8;
    int32_t  vx_q8, vy_q8;
    int8_t   facing;         /* +1 right, -1 left */
    uint32_t last_flip_ms;
    uint32_t tail_q8;        /* phase in Q8 (>>8 = 0..255) */
    uint32_t pulse_q8;       /* jellyfish bell / wing flap phase */
    int32_t  wander_tx_q8, wander_ty_q8;
    uint32_t wander_next_ms;
    uint32_t wake_next_ms;
    /* Per-frame render cache (frozen after step) */
    int16_t  fx, fy;
    int16_t  bx0, bx1, by0, by1;
    uint16_t scale_q8;
    uint16_t inv_scale_q8;
    int16_t  wag_q8;         /* current tail deflection (-256..+256) */
    int16_t  pitch_q8;       /* gentle nose tilt */
    int16_t  pulse_q8_val;   /* bell expansion / wing undulation (-128..+128) */
    uint8_t  elder_glow;     /* 0..255 shimmer for stage==3 */
} aq_fish_sim_t;

typedef struct {
    bool     active;
    int32_t  x_q8, y_q8;
    int32_t  vy_q8;
    uint16_t phase;
    uint32_t born_ms;
    int16_t  px, py;
} aq_pellet_t;

typedef struct {
    bool     active;
    int16_t  cx, cy;
    uint32_t born_ms;
    int16_t  r_px;
    uint8_t  alpha;
    uint16_t col;
} aq_ripple_t;

typedef struct {
    bool     active;
    bool     burst;
    int32_t  x_q8, y_q8;
    int32_t  vy_q8;
    uint8_t  rad;
    uint8_t  wobble_amp;
    uint16_t wobble_phase;
    int16_t  px, py;
} aq_bubble_t;

typedef struct {
    bool     active;
    int16_t  cx, cy;
    uint32_t born_ms;
    int16_t  r_px;
    uint8_t  alpha;
} aq_wake_t;

typedef struct {
    int16_t  x, y;
    uint8_t  rad;
    uint8_t  alpha;
    uint16_t col;
} aq_mote_t;

typedef struct {
    int16_t  x0, y0, x1, y1;
    int16_t  y_min, y_max;
    uint8_t  r0, r1;
    uint16_t col_base;
    uint16_t col_hi;
} aq_coral_seg_t;

typedef struct {
    int16_t  cx, cy;
    uint8_t  rad;
    uint8_t  phase;
    uint16_t col;
    uint8_t  alpha;
} aq_polyp_t;

typedef struct {
    int16_t  y_top, y_bot;
    int16_t  kx[AQ_KELP_KNOTS];
    uint8_t  hw[AQ_KELP_KNOTS];
    uint16_t col_dark;
    uint16_t col_mid;
    uint16_t col_hi;
} aq_kelp_t;

typedef struct {
    int16_t  x_top;
    int16_t  slope_q8;   /* dx per dy in Q8 */
    int16_t  half_w;
    uint16_t col;
    uint8_t  peak_a;
} aq_beam_t;

typedef struct {
    int16_t  cx, cy;
    int16_t  rad;
    int16_t  notch_dir;  /* Q8 angle 0..255 */
    int16_t  ncos, nsin; /* Q14 unit vector of notch bisector */
    bool     has_lotus;
} aq_lily_t;

typedef struct {
    bool     initialized;
    uint32_t now_ms;
    uint32_t prev_ms;
    uint32_t rng;
    int16_t  tilt_x, tilt_y;
    uint8_t  biome;
    uint8_t  last_biome;
    uint8_t  coral_level;
    uint8_t  algae;
    uint8_t  hour;
    uint8_t  fish_count;
    bool     is_night;

    uint32_t shake_until_ms;
    uint32_t toast_start_ms;
    bool     toast_active;

    /* Cached geometry keys */
    uint8_t  built_coral_biome;
    uint8_t  built_coral_level;
    uint8_t  built_terrain_biome;

    aq_fish_sim_t  fish[JR_AQ_MAX_FISH];
    aq_pellet_t    pellets[JR_AQ_MAX_PELLETS];
    aq_ripple_t    ripples[JR_AQ_MAX_RIPPLES];
    aq_bubble_t    bubbles[JR_AQ_MAX_BUBBLES];
    aq_wake_t      wakes[AQ_MAX_WAKES];
    aq_mote_t      motes[AQ_NUM_MOTES];
    aq_coral_seg_t coral_segs[AQ_MAX_CORAL_SEGS];
    int            coral_seg_count;
    aq_polyp_t     polyps[AQ_MAX_POLYPS];
    int            polyp_count;
    aq_kelp_t      kelp[AQ_NUM_KELP];
    aq_beam_t      beams[AQ_NUM_BEAMS];
    aq_lily_t      lilies[AQ_NUM_LILIES];

    /* Per-frame precomputed row/column caches */
    uint16_t row_water[JR_AQ_H];
    uint16_t row_air[JR_AQ_H];
    uint16_t koi_rad_lut[96];
    int16_t  men_y[JR_AQ_W];
    int16_t  men_y_min;
    int16_t  men_y_max;
    uint16_t men_crest_col;
    uint16_t men_glow_col;
    int16_t  seabed_back[JR_AQ_W];
    int16_t  seabed_front[JR_AQ_W];
    int16_t  seabed_y_min;
    uint16_t seabed_back_col;
    uint16_t seabed_front_col;
    uint16_t seabed_rim_col;
} aq_ctx_t;

static aq_ctx_t s_aq;

static inline uint32_t aq_rand(void)
{
    s_aq.rng = s_aq.rng * 1664525u + 1013904223u;
    return s_aq.rng;
}

/* ==========================================================================
 * TERRAIN, CORAL REEF & KELP BUILDERS (CALLED ONCE PER FRAME OR ON CHANGE)
 * ========================================================================== */

static void aq_add_coral_seg(int x0, int y0, int x1, int y1,
                             int r0, int r1, uint16_t col_base, uint16_t col_hi)
{
    if (s_aq.coral_seg_count >= AQ_MAX_CORAL_SEGS) {
        return;
    }
    aq_coral_seg_t *s = &s_aq.coral_segs[s_aq.coral_seg_count++];
    s->x0 = (int16_t)x0;
    s->y0 = (int16_t)y0;
    s->x1 = (int16_t)x1;
    s->y1 = (int16_t)y1;
    s->r0 = (uint8_t)r0;
    s->r1 = (uint8_t)r1;
    const int rmax = r0 > r1 ? r0 : r1;
    s->y_min = (int16_t)(aq_min(y0, y1) - rmax - 1);
    s->y_max = (int16_t)(aq_max(y0, y1) + rmax + 1);
    s->col_base = col_base;
    s->col_hi = col_hi;
}

static void aq_add_polyp(int cx, int cy, int rad, uint16_t col, uint8_t phase)
{
    if (s_aq.polyp_count >= AQ_MAX_POLYPS) {
        return;
    }
    aq_polyp_t *p = &s_aq.polyps[s_aq.polyp_count++];
    p->cx = (int16_t)cx;
    p->cy = (int16_t)cy;
    p->rad = (uint8_t)rad;
    p->col = col;
    p->phase = phase;
    p->alpha = 180u;
}

static void aq_build_terrain(uint8_t biome)
{
    if (s_aq.built_terrain_biome == (uint8_t)(biome + 1u)) {
        return;
    }
    s_aq.built_terrain_biome = (uint8_t)(biome + 1u);

    int ymin = JR_AQ_H;
    for (int x = 0; x < JR_AQ_W; ++x) {
        const int dx = x - JR_AQ_CX;
        /* Gently concave floor curving up slightly at sides to embrace the round bowl */
        const int bowl = (dx * dx) / 720; /* 0 at center, ~62 at edges */
        const int w1 = (aq_sin((x * 3) / 2) * 14) >> 14;
        const int w2 = (aq_sin(x * 3 + 47) * 7) >> 14;
        int yb = 382 - bowl / 3 + w1 + w2;
        int yf = 404 - bowl / 4 + ((aq_sin(x * 2 + 19) * 10) >> 14);
        if (biome == AQ_BIOME_ABYSS) {
            yb += 10;
            yf += 8;
        }
        yb = aq_clamp(yb, 326, 444);
        yf = aq_clamp(yf, yb + 8, 446);
        s_aq.seabed_back[x] = (int16_t)yb;
        s_aq.seabed_front[x] = (int16_t)yf;
        if (yb < ymin) {
            ymin = yb;
        }
    }
    s_aq.seabed_y_min = (int16_t)ymin;

    /* Koi radial emerald-jade pond gradient LUT indexed by (r2 >> 9) in [0..95] */
    for (int i = 0; i < 96; ++i) {
        /* Center r=0 (deep teal-jade), mid r=140 (rich sunlit emerald), rim r=213 (shaded mossy stone) */
        const int r_est = (int)aq_isqrt((uint32_t)i << 9);
        if (r_est < 135) {
            const int t = (r_est * 255) / 135;
            s_aq.koi_rad_lut[i] = aq_lerp565(aq_rgb565(3, 30, 32), aq_rgb565(9, 64, 54), t);
        } else {
            const int t = aq_clamp(((r_est - 135) * 255) / 78, 0, 255);
            s_aq.koi_rad_lut[i] = aq_lerp565(aq_rgb565(9, 64, 54), aq_rgb565(4, 28, 26), t);
        }
    }
}

static void aq_add_branch_tree(int bx, int by, int h, int lean,
                               uint16_t cb, uint16_t ch, uint16_t pcol, uint8_t ph)
{
    const int mx = bx + lean / 2;
    const int my = by - (h * 55) / 100;
    const int tx = bx + lean;
    const int ty = by - h;
    aq_add_coral_seg(bx, by, mx, my, 7, 5, cb, ch);
    aq_add_coral_seg(mx, my, tx, ty, 5, 3, cb, ch);
    aq_add_polyp(tx, ty, 4, pcol, ph);

    const int lx = mx - (h * 34) / 100;
    const int ly = my - (h * 36) / 100;
    aq_add_coral_seg(mx, my, lx, ly, 4, 2, cb, ch);
    aq_add_polyp(lx, ly, 4, pcol, (uint8_t)(ph + 43u));

    const int rx = mx + (h * 32) / 100;
    const int ry = my - (h * 30) / 100;
    aq_add_coral_seg(mx, my, rx, ry, 4, 2, cb, ch);
    aq_add_polyp(rx, ry, 3, pcol, (uint8_t)(ph + 91u));
}

static void aq_build_corals(uint8_t biome, uint8_t coral_level)
{
    if (s_aq.built_coral_biome == (uint8_t)(biome + 1u) &&
        s_aq.built_coral_level == coral_level) {
        return;
    }
    s_aq.built_coral_biome = (uint8_t)(biome + 1u);
    s_aq.built_coral_level = coral_level;
    s_aq.coral_seg_count = 0;
    s_aq.polyp_count = 0;

    if (biome == AQ_BIOME_KOI) {
        return;
    }

    const bool abyss = (biome == AQ_BIOME_ABYSS);
    const uint16_t c_rose_b   = abyss ? aq_rgb565(24, 10, 48) : aq_rgb565(138, 36, 74);
    const uint16_t c_rose_h   = abyss ? aq_rgb565(110, 26, 150) : aq_rgb565(235, 96, 138);
    const uint16_t c_teal_b   = abyss ? aq_rgb565(6, 26, 56) : aq_rgb565(16, 98, 108);
    const uint16_t c_teal_h   = abyss ? aq_rgb565(20, 145, 195) : aq_rgb565(56, 206, 196);
    const uint16_t c_amber_b  = abyss ? aq_rgb565(34, 14, 62) : aq_rgb565(152, 76, 26);
    const uint16_t c_amber_h  = abyss ? aq_rgb565(165, 42, 195) : aq_rgb565(246, 158, 56);
    const uint16_t c_plum_b   = abyss ? aq_rgb565(18, 12, 54) : aq_rgb565(74, 34, 114);
    const uint16_t c_plum_h   = abyss ? aq_rgb565(80, 62, 210) : aq_rgb565(154, 88, 214);

    const uint16_t p_cyan    = 0x07FFu;
    const uint16_t p_magenta = 0xF8B8u;
    const uint16_t p_amber   = abyss ? 0xB1FFu : 0xFD40u;
    const uint16_t p_mint    = 0x3FF4u;

    /* Level 1+: 2 foundational reef clusters on left & right */
    aq_add_branch_tree(138, 408, 48, -10, c_rose_b, c_rose_h, p_magenta, 12u);
    aq_add_branch_tree(326, 406, 46,  12, c_teal_b, c_teal_h, p_cyan,    78u);

    /* Level 2+: Center-right coral + brain dome on left */
    if (coral_level >= 2u) {
        aq_add_branch_tree(246, 412, 44, -6, c_amber_b, c_amber_h, p_amber, 134u);
        aq_add_coral_seg(176, 414, 196, 414, 14, 12, c_plum_b, c_plum_h);
        aq_add_polyp(182, 402, 3, p_cyan, 20u);
        aq_add_polyp(194, 404, 3, p_mint, 110u);
    }

    /* Level 3+: Outer twin staghorns + central sea fan */
    if (coral_level >= 3u) {
        aq_add_branch_tree( 94, 392, 44, -14, c_teal_b, c_teal_h, p_cyan,    44u);
        aq_add_branch_tree(372, 390, 44,  14, c_rose_b, c_rose_h, p_magenta, 168u);
        aq_add_branch_tree(212, 416, 52,   8, c_plum_b, c_plum_h, p_mint,    210u);
    }

    /* Level 4+: Tall crown branches & terraced brain corals */
    if (coral_level >= 4u) {
        aq_add_branch_tree(162, 404, 62, -12, c_amber_b, c_amber_h, p_amber,   56u);
        aq_add_branch_tree(296, 408, 58,  10, c_plum_b,  c_plum_h,  p_magenta, 189u);
        aq_add_coral_seg(268, 418, 286, 418, 12, 11, c_teal_b, c_teal_h);
        aq_add_polyp(274, 408, 4, p_cyan, 95u);
    }

    /* Level 5: Full royal coral sanctuary with extra radiant polyps */
    if (coral_level >= 5u) {
        aq_add_branch_tree(120, 398, 58,  -8, c_plum_b,  c_plum_h,  p_cyan,    15u);
        aq_add_branch_tree(348, 398, 56,  10, c_amber_b, c_amber_h, p_mint,    142u);
        aq_add_branch_tree(234, 412, 64,  -4, c_rose_b,  c_rose_h,  p_magenta, 233u);
        aq_add_polyp(152, 376, 5, p_cyan,    66u);
        aq_add_polyp(312, 376, 5, p_amber,   192u);
        aq_add_polyp(234, 348, 5, p_magenta, 31u);
    }
}

static void aq_update_kelp(uint32_t now_ms, int tilt_x, uint8_t biome, bool is_night)
{
    static const int16_t k_root_x[AQ_NUM_KELP] = {  76, 108, 148, 320, 358, 392 };
    static const int16_t k_height[AQ_NUM_KELP] = { 126, 158, 108, 114, 162, 128 };
    static const uint8_t k_phase[AQ_NUM_KELP]  = {   0,  48, 102, 154, 206,  28 };
    static const uint8_t k_width[AQ_NUM_KELP]  = {   6,   8,   6,   6,   8,   6 };

    const bool abyss = (biome == AQ_BIOME_ABYSS);
    const int t_wave = (int)((now_ms / 24u) & 255u);

    for (int i = 0; i < AQ_NUM_KELP; ++i) {
        aq_kelp_t *k = &s_aq.kelp[i];
        const int rx = k_root_x[i];
        const int y_bot = (int)s_aq.seabed_front[rx] + 10;
        const int h = k_height[i];
        k->y_bot = (int16_t)aq_min(y_bot, 444);
        k->y_top = (int16_t)aq_max(y_bot - h, 178);

        if (abyss) {
            k->col_dark = aq_rgb565(6, 22, 44);
            k->col_mid  = aq_rgb565(12, 74, 122);
            k->col_hi   = aq_rgb565(42, 232, 252);
        } else if (is_night) {
            k->col_dark = aq_rgb565(4, 34, 30);
            k->col_mid  = aq_rgb565(10, 86, 68);
            k->col_hi   = aq_rgb565(36, 196, 158);
        } else {
            k->col_dark = aq_rgb565(6, 68, 46);
            k->col_mid  = aq_rgb565(18, 142, 86);
            k->col_hi   = aq_rgb565(78, 224, 134);
        }

        for (int s = 0; s < AQ_KELP_KNOTS; ++s) {
            /* s = 0 is root (bottom), s = 16 is tip (top).
             * Gentle underwater sway: max +-18 px at tip + +-32 px IMU gravity lean. */
            const int frac = s; /* 0..16 */
            const int amp1 = (frac * 18) >> 4; /* 0..18 px */
            const int amp2 = (frac * 5) >> 4;  /* 0..5 px  */
            const int sway1 = (aq_sin(t_wave + (int)k_phase[i] - s * 11) * amp1) >> 14;
            const int sway2 = (aq_sin(t_wave * 2 + (int)k_phase[i] * 2 - s * 17) * amp2) >> 14;
            const int lean = (tilt_x * frac * frac) >> 11;
            k->kx[s] = (int16_t)aq_clamp(rx + sway1 + sway2 + lean, 32, JR_AQ_W - 32);

            /* Graceful organic leaf frond width with soft alternating leaf lobes */
            int w = ((int)k_width[i] * (19 - frac)) / 17;
            if ((s & 1) != 0 && s > 1 && s < 15) {
                w += 2;
            }
            if (w < 1) {
                w = 1;
            }
            k->hw[s] = (uint8_t)w;
        }
    }
}

static void aq_update_reef_palette(uint32_t now_ms, uint8_t hour, int tilt_x)
{
    uint16_t air_top, air_bot;
    uint16_t w_top, w_mid, w_deep, w_bot;
    uint16_t beam_col;
    uint8_t  beam_a;

    if (hour >= 5u && hour <= 8u) {
        /* Warm golden dawn */
        s_aq.is_night = false;
        air_top = aq_rgb565(18, 16, 44);
        air_bot = aq_rgb565(72, 42, 66);
        w_top   = aq_rgb565(34, 122, 138);
        w_mid   = aq_rgb565(16, 76, 114);
        w_deep  = aq_rgb565(8, 36, 72);
        w_bot   = aq_rgb565(4, 16, 36);
        s_aq.men_crest_col = aq_rgb565(255, 224, 172);
        s_aq.men_glow_col  = aq_rgb565(132, 206, 212);
        beam_col = aq_rgb565(255, 204, 124);
        beam_a   = 38u;
        s_aq.seabed_back_col  = aq_rgb565(10, 34, 56);
        s_aq.seabed_front_col = aq_rgb565(14, 42, 58);
        s_aq.seabed_rim_col   = aq_rgb565(74, 122, 136);
    } else if (hour >= 9u && hour <= 16u) {
        /* Vibrant sunlit tropical lagoon */
        s_aq.is_night = false;
        air_top = aq_rgb565(8, 28, 46);
        air_bot = aq_rgb565(24, 72, 102);
        w_top   = aq_rgb565(16, 152, 184);
        w_mid   = aq_rgb565(10, 88, 142);
        w_deep  = aq_rgb565(5, 42, 86);
        w_bot   = aq_rgb565(3, 18, 44);
        s_aq.men_crest_col = aq_rgb565(214, 252, 255);
        s_aq.men_glow_col  = aq_rgb565(78, 226, 244);
        beam_col = aq_rgb565(132, 248, 255);
        beam_a   = 44u;
        s_aq.seabed_back_col  = aq_rgb565(8, 38, 66);
        s_aq.seabed_front_col = aq_rgb565(12, 54, 74);
        s_aq.seabed_rim_col   = aq_rgb565(64, 152, 174);
    } else if (hour >= 17u && hour <= 20u) {
        /* Violet-amber tropical sunset */
        s_aq.is_night = false;
        air_top = aq_rgb565(28, 14, 44);
        air_bot = aq_rgb565(94, 40, 62);
        w_top   = aq_rgb565(62, 62, 114);
        w_mid   = aq_rgb565(30, 42, 92);
        w_deep  = aq_rgb565(14, 22, 62);
        w_bot   = aq_rgb565(6, 10, 30);
        s_aq.men_crest_col = aq_rgb565(255, 184, 134);
        s_aq.men_glow_col  = aq_rgb565(188, 112, 168);
        beam_col = aq_rgb565(255, 146, 118);
        beam_a   = 34u;
        s_aq.seabed_back_col  = aq_rgb565(16, 20, 48);
        s_aq.seabed_front_col = aq_rgb565(22, 26, 52);
        s_aq.seabed_rim_col   = aq_rgb565(122, 88, 126);
    } else {
        /* Midnight bioluminescent indigo */
        s_aq.is_night = true;
        air_top = aq_rgb565(2, 4, 12);
        air_bot = aq_rgb565(6, 14, 34);
        w_top   = aq_rgb565(8, 22, 56);
        w_mid   = aq_rgb565(5, 14, 38);
        w_deep  = aq_rgb565(3, 8, 24);
        w_bot   = aq_rgb565(1, 4, 14);
        s_aq.men_crest_col = aq_rgb565(116, 186, 238);
        s_aq.men_glow_col  = aq_rgb565(42, 106, 168);
        beam_col = aq_rgb565(88, 152, 232);
        beam_a   = 20u;
        s_aq.seabed_back_col  = aq_rgb565(4, 10, 26);
        s_aq.seabed_front_col = aq_rgb565(6, 14, 32);
        s_aq.seabed_rim_col   = aq_rgb565(28, 72, 118);
    }

    if (s_aq.biome == AQ_BIOME_ABYSS) {
        s_aq.is_night = true;
        s_aq.seabed_back_col  = aq_rgb565(5, 3, 14);
        s_aq.seabed_front_col = aq_rgb565(9, 5, 22);
        s_aq.seabed_rim_col   = aq_rgb565(36, 18, 86);
    }

    /* Precompute vertical air & water row LUTs (smooth 3-stop quadratic depth) */
    for (int y = 0; y < JR_AQ_H; ++y) {
        const int ta = aq_clamp(((y - 20) * 255) / 60, 0, 255);
        s_aq.row_air[y] = aq_lerp565(air_top, air_bot, ta);

        /* Smoothly blended continuous depth curve with zero visible slope knee */
        const int u = aq_clamp(((y - 46) * 255) / 396, 0, 255);
        const uint16_t c_hi = aq_lerp565(w_top, w_mid, aq_min(255, (u * 3) / 2));
        const uint16_t c_lo = aq_lerp565(w_deep, w_bot, aq_max(0, (u - 128) * 2));
        const int mix = (u * u) >> 8;
        s_aq.row_water[y] = aq_lerp565(c_hi, c_lo, mix);
    }

    /* Precompute tilted undulating water surface meniscus y per column x */
    const int wph1 = (int)((now_ms / 18u) & 255u);
    const int wph2 = (int)((now_ms / 29u) & 255u);
    const int base_y = 62 + (s_aq.tilt_y >> 5);
    int mmin = JR_AQ_H;
    int mmax = 0;
    for (int x = 0; x < JR_AQ_W; ++x) {
        const int dx = x - JR_AQ_CX;
        const int tilt_off = (tilt_x * dx) >> 10; /* +-50 px at full tilt */
        const int wave1 = (aq_sin((x * 2) + wph1) * 4) >> 14;
        const int wave2 = (aq_sin((x * 3) - wph2) * 2) >> 14;
        const int my = aq_clamp(base_y + tilt_off + wave1 + wave2, 34, 128);
        s_aq.men_y[x] = (int16_t)my;
        if (my < mmin) { mmin = my; }
        if (my > mmax) { mmax = my; }
    }
    s_aq.men_y_min = (int16_t)mmin;
    s_aq.men_y_max = (int16_t)mmax;

    /* 4 diagonal volumetric sun/moon caustic shafts */
    static const int16_t k_bx[AQ_NUM_BEAMS] = { 126, 202, 278, 348 };
    static const int16_t k_bw[AQ_NUM_BEAMS] = {  26,  34,  28,  22 };
    const int bph = (int)((now_ms / 34u) & 255u);
    for (int i = 0; i < AQ_NUM_BEAMS; ++i) {
        const int sway = (aq_sin(bph + i * 67) * 18) >> 14;
        const int pulse = (aq_sin(bph * 2 + i * 91) * 10) >> 14;
        s_aq.beams[i].x_top = (int16_t)(k_bx[i] + sway);
        s_aq.beams[i].slope_q8 = (int16_t)(54 + i * 10 - (tilt_x >> 3));
        s_aq.beams[i].half_w = k_bw[i];
        s_aq.beams[i].col = beam_col;
        s_aq.beams[i].peak_a = (uint8_t)aq_clamp((int)beam_a + pulse, 8, 64);
    }
}

static void aq_update_motes(uint32_t now_ms, uint8_t biome)
{
    static const uint16_t k_mote_cols[4] = {
        0x07FFu, /* electric cyan */
        0xF81Fu, /* neon magenta */
        0x99FFu, /* luminous violet */
        0x3FF8u, /* bioluminescent mint */
    };
    const bool abyss = (biome == AQ_BIOME_ABYSS);
    const int t = (int)(now_ms / 38u);
    for (int i = 0; i < AQ_NUM_MOTES; ++i) {
        const uint32_t h = aq_hash32((uint32_t)i * 7919u + 0xA91Bu);
        const int cx = 64 + (int)(h % 338u);
        const int cy = 84 + (int)((h >> 10) % 304u);
        const int sp = 1 + (int)((h >> 20) & 1u);
        const int dx = (aq_sin(t * sp + (int)(h & 255u)) * 18) >> 14;
        const int dy = (aq_cos(t + (int)((h >> 8) & 255u)) * 14) >> 14;
        const int px = cx + dx;
        const int py = cy + dy;
        const int ox = px - JR_AQ_CX;
        const int oy = py - JR_AQ_CY;
        if (ox * ox + oy * oy > 192 * 192) {
            s_aq.motes[i].alpha = 0u;
            continue;
        }
        const int tw = (aq_sin(t * 3 + i * 37) + 16384) >> 7; /* 0..256 */
        s_aq.motes[i].x = (int16_t)px;
        s_aq.motes[i].y = (int16_t)py;
        s_aq.motes[i].rad = (uint8_t)(abyss ? (2 + (i % 3)) : (1 + (i % 2)));
        s_aq.motes[i].col = k_mote_cols[i & 3];
        s_aq.motes[i].alpha = (uint8_t)(abyss ? (75 + ((tw * 155) >> 8))
                                              : (35 + ((tw * 105) >> 8)));
    }
}

static void aq_update_lilies(uint32_t now_ms)
{
    static const int16_t k_lx[AQ_NUM_LILIES] = { 108, 372,  92, 364 };
    static const int16_t k_ly[AQ_NUM_LILIES] = { 340, 142, 138, 342 };
    static const int16_t k_lr[AQ_NUM_LILIES] = {  42,  34,  32,  36 };
    static const int16_t k_na[AQ_NUM_LILIES] = { 216,  92,  36, 152 };

    const int t = (int)((now_ms / 60u) & 255u);
    for (int i = 0; i < AQ_NUM_LILIES; ++i) {
        aq_lily_t *l = &s_aq.lilies[i];
        const int bob_x = (aq_sin(t + i * 61) * 3) >> 14;
        const int bob_y = (aq_cos(t + i * 83) * 3) >> 14;
        l->cx = (int16_t)(k_lx[i] + bob_x);
        l->cy = (int16_t)(k_ly[i] + bob_y);
        l->rad = k_lr[i];
        l->notch_dir = (int16_t)((k_na[i] + ((aq_sin(t + i * 41) * 4) >> 14)) & 255);
        l->ncos = (int16_t)aq_cos(l->notch_dir);
        l->nsin = (int16_t)aq_sin(l->notch_dir);
        l->has_lotus = (i == 0);
    }
}

/* ==========================================================================
 * BUBBLES, PELLETS, RIPPLES & FISH PHYSICS
 * ========================================================================== */

static void aq_spawn_bubble(int x, int y, bool burst, uint8_t rad)
{
    int slot = -1;
    for (int i = 0; i < JR_AQ_MAX_BUBBLES; ++i) {
        if (!s_aq.bubbles[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = (int)(aq_rand() % (uint32_t)JR_AQ_MAX_BUBBLES);
    }
    aq_bubble_t *b = &s_aq.bubbles[slot];
    b->active = true;
    b->burst = burst;
    b->x_q8 = x << 8;
    b->y_q8 = y << 8;
    b->rad = rad;
    b->vy_q8 = -(110 + (int32_t)rad * 26 + (int32_t)(aq_rand() % 64u));
    b->wobble_amp = (uint8_t)(2 + (aq_rand() % 4u));
    b->wobble_phase = (uint16_t)(aq_rand() & 255u);
    b->px = (int16_t)x;
    b->py = (int16_t)y;
}

static void aq_init_ambient_bubbles(void)
{
    for (int i = 0; i < 16; ++i) {
        const uint32_t h = aq_hash32((uint32_t)i * 3571u + 17u);
        const int x = 76 + (int)(h % 314u);
        const int y = 100 + (int)((h >> 9) % 290u);
        aq_bubble_t *b = &s_aq.bubbles[i];
        b->active = true;
        b->burst = false;
        b->x_q8 = x << 8;
        b->y_q8 = y << 8;
        b->rad = (uint8_t)(2u + (h % 3u));
        b->vy_q8 = -(95 + (int32_t)b->rad * 22 + (int32_t)((h >> 16) % 48u));
        b->wobble_amp = (uint8_t)(2u + ((h >> 12) & 3u));
        b->wobble_phase = (uint16_t)(h & 255u);
        b->px = (int16_t)x;
        b->py = (int16_t)y;
    }
}

static void aq_seed_fish(int idx, const jr_display_aq_fish_t *src)
{
    aq_fish_sim_t *f = &s_aq.fish[idx];
    memset(f, 0, sizeof *f);
    f->active = true;
    f->species = src->species <= 7u ? src->species : 0u;
    f->stage = src->stage <= 3u ? src->stage : 2u;
    f->hunger = src->hunger <= 100u ? src->hunger : 100u;
    f->seed = src->seed;

    const uint32_t h = aq_hash32(((uint32_t)src->seed << 8) ^ ((uint32_t)idx * 2654435761u) ^
                                 ((uint32_t)f->species * 40503u));
    /* Spread initial positions harmoniously across the round porthole */
    int ix = 118 + (int)((idx * 47 + (int)(h % 53u)) % 230);
    int iy = 128 + (int)((idx * 37 + (int)((h >> 8) % 67u)) % 192);
    if (f->species == AQ_SP_SEAHORSE) {
        ix = (idx & 1) ? 338 : 126;
        iy = 248 + (int)((h >> 8) % 64u);
    } else if (f->species == AQ_SP_JELLY) {
        iy = 135 + (int)((h >> 8) % 120u);
    } else if (f->species == AQ_SP_MANTA) {
        iy = 148 + (int)((h >> 8) % 90u);
    } else if (f->species == AQ_SP_TETRA) {
        iy = 115 + (int)((h >> 8) % 85u);
    }
    /* Clamp inside circular swim arena */
    const int dx = ix - JR_AQ_CX;
    const int dy = iy - JR_AQ_CY;
    if (dx * dx + dy * dy > 148 * 148) {
        ix = JR_AQ_CX + (dx * 3) / 4;
        iy = JR_AQ_CY + (dy * 3) / 4;
    }
    f->x_q8 = ix << 8;
    f->y_q8 = iy << 8;
    f->facing = ((h >> 16) & 1u) ? 1 : -1;
    f->vx_q8 = (int32_t)f->facing * (120 + (int32_t)((h >> 18) % 80u));
    f->vy_q8 = ((int32_t)((h >> 22) % 61u) - 30);
    f->tail_q8 = (h & 255u) << 8;
    f->pulse_q8 = ((h >> 4) & 255u) << 8;
    f->wander_tx_q8 = f->x_q8;
    f->wander_ty_q8 = f->y_q8;
    f->wander_next_ms = s_aq.now_ms + 800u + (h % 1600u);
    f->last_flip_ms = s_aq.now_ms;
    f->wake_next_ms = s_aq.now_ms + 220u + (uint32_t)idx * 60u;
}

static void aq_emit_wake(int cx, int cy, uint32_t now_ms)
{
    int slot = 0;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < AQ_MAX_WAKES; ++i) {
        if (!s_aq.wakes[i].active) {
            slot = i;
            break;
        }
        const uint32_t age = now_ms - s_aq.wakes[i].born_ms;
        if (age > oldest || oldest == UINT32_MAX) {
            oldest = age;
            slot = i;
        }
    }
    s_aq.wakes[slot].active = true;
    s_aq.wakes[slot].cx = (int16_t)cx;
    s_aq.wakes[slot].cy = (int16_t)cy;
    s_aq.wakes[slot].born_ms = now_ms;
    s_aq.wakes[slot].r_px = 4;
    s_aq.wakes[slot].alpha = 88u;
}

JR_AQ_API void jr_aquarium_tap(uint16_t x, uint16_t y)
{
    int tx = (int)x;
    int ty = (int)y;
    const int dx = tx - JR_AQ_CX;
    const int dy = ty - JR_AQ_CY;
    const uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
    if (d2 > 180u * 180u) {
        const uint32_t d = aq_isqrt(d2);
        if (d > 0u) {
            tx = JR_AQ_CX + (dx * 176) / (int)d;
            ty = JR_AQ_CY + (dy * 176) / (int)d;
        }
    }
    if (ty < 72) {
        ty = 72;
    }
    if (ty > 365) {
        ty = 365;
    }

    /* Spawn 3 golden sinking food pellets with small natural scatter */
    static const int8_t k_off_x[3] = { 0, -10, 11 };
    static const int8_t k_off_y[3] = { 0, -5, 4 };
    for (int k = 0; k < 3; ++k) {
        int slot = -1;
        uint32_t oldest = 0u;
        int oldest_idx = 0;
        for (int i = 0; i < JR_AQ_MAX_PELLETS; ++i) {
            if (!s_aq.pellets[i].active) {
                slot = i;
                break;
            }
            const uint32_t age = s_aq.now_ms - s_aq.pellets[i].born_ms;
            if (age >= oldest) {
                oldest = age;
                oldest_idx = i;
            }
        }
        if (slot < 0) {
            slot = oldest_idx;
        }
        aq_pellet_t *p = &s_aq.pellets[slot];
        p->active = true;
        p->x_q8 = aq_clamp(tx + k_off_x[k], 54, JR_AQ_W - 54) << 8;
        p->y_q8 = aq_clamp(ty + k_off_y[k], 70, 365) << 8;
        p->vy_q8 = 128 + k * 22;
        p->phase = (uint16_t)((k * 85 + (int)(aq_rand() & 63u)) & 255);
        p->born_ms = s_aq.initialized ? s_aq.now_ms : 0u;
        p->px = (int16_t)(p->x_q8 >> 8);
        p->py = (int16_t)(p->y_q8 >> 8);
    }

    /* Spawn 1 expanding water ripple ring */
    int rslot = -1;
    uint32_t rold = 0u;
    int rold_idx = 0;
    for (int i = 0; i < JR_AQ_MAX_RIPPLES; ++i) {
        if (!s_aq.ripples[i].active) {
            rslot = i;
            break;
        }
        const uint32_t age = s_aq.now_ms - s_aq.ripples[i].born_ms;
        if (age >= rold) {
            rold = age;
            rold_idx = i;
        }
    }
    if (rslot < 0) {
        rslot = rold_idx;
    }
    aq_ripple_t *rp = &s_aq.ripples[rslot];
    rp->active = true;
    rp->cx = (int16_t)tx;
    rp->cy = (int16_t)ty;
    rp->born_ms = s_aq.initialized ? s_aq.now_ms : 0u;
    rp->r_px = 6;
    rp->alpha = 215u;
    rp->col = (s_aq.biome == AQ_BIOME_KOI) ? aq_rgb565(178, 252, 232) : 0x7FFFu;
}

JR_AQ_API void jr_aquarium_shake(void)
{
    if (s_aq.rng == 0u) {
        s_aq.rng = 0x5EED1234u;
    }
    s_aq.shake_until_ms = s_aq.initialized ? (s_aq.now_ms + AQ_SHAKE_MS) : AQ_SHAKE_MS;

    /* +14 playful rising burst air bubbles across the reef floor */
    for (int i = 0; i < 14; ++i) {
        const int bx = 86 + (int)(aq_rand() % 294u);
        const int by = 320 + (int)(aq_rand() % 78u);
        const uint8_t rad = (uint8_t)(2u + (aq_rand() % 4u));
        aq_spawn_bubble(bx, by, true, rad);
    }

    /* Tangential playful swirl impulse for all active fish */
    for (int i = 0; i < JR_AQ_MAX_FISH; ++i) {
        aq_fish_sim_t *f = &s_aq.fish[i];
        if (!f->active) {
            continue;
        }
        const int32_t dx = (f->x_q8 >> 8) - JR_AQ_CX;
        const int32_t dy = (f->y_q8 >> 8) - JR_AQ_CY;
        const int32_t sign = (i & 1) ? 1 : -1;
        f->vx_q8 += (-dy * sign * 3);
        f->vy_q8 += (dx * sign * 3);
        f->vx_q8 = aq_clamp(f->vx_q8, -760, 760);
        f->vy_q8 = aq_clamp(f->vy_q8, -480, 480);
    }
}

JR_AQ_API const char *jr_aquarium_biome_toast(uint32_t now_ms, uint8_t *out_alpha)
{
    if (!s_aq.toast_active) {
        if (out_alpha != NULL) {
            *out_alpha = 0u;
        }
        return NULL;
    }
    const uint32_t age = now_ms - s_aq.toast_start_ms;
    if (age >= AQ_TOAST_MS) {
        if (out_alpha != NULL) {
            *out_alpha = 0u;
        }
        return NULL;
    }
    uint8_t a = 255u;
    if (age < 180u) {
        a = (uint8_t)((age * 255u) / 180u);
    } else if (age > AQ_TOAST_MS - AQ_TOAST_FADE_MS) {
        const uint32_t rem = AQ_TOAST_MS - age;
        a = (uint8_t)((rem * 255u) / AQ_TOAST_FADE_MS);
    }
    if (out_alpha != NULL) {
        *out_alpha = a;
    }
    switch (s_aq.biome) {
    case AQ_BIOME_ABYSS: return "DEEP ABYSS";
    case AQ_BIOME_KOI:   return "ZEN KOI POND";
    case AQ_BIOME_REEF:
    default:             return "TROPICAL REEF";
    }
}

JR_AQ_API void jr_aquarium_step(uint32_t now_ms, int tilt_x, int tilt_y,
                                const jr_display_aq_state_t *state)
{
    const uint8_t biome = (state != NULL && state->biome <= 2u) ? state->biome : 0u;
    const uint8_t coral = (state != NULL)
        ? (uint8_t)aq_clamp((int)state->coral_level, 1, 5) : 2u;
    const uint8_t algae = (state != NULL && state->algae <= 100u) ? state->algae : 0u;
    const uint8_t hour  = (state != NULL) ? (uint8_t)(state->hour % 24u) : 13u;
    const uint8_t nfish = (state != NULL && state->fish_count <= JR_AQ_MAX_FISH)
        ? state->fish_count : 0u;

    if (!s_aq.initialized) {
        s_aq.initialized = true;
        s_aq.rng = 0x2468ACE1u ^ (now_ms * 2654435761u);
        s_aq.now_ms = now_ms;
        s_aq.prev_ms = now_ms;
        s_aq.last_biome = biome;
        s_aq.toast_start_ms = now_ms;
        s_aq.toast_active = true;
        aq_init_ambient_bubbles();
    }

    uint32_t dt_ms = now_ms - s_aq.prev_ms;
    if (dt_ms > 60u) {
        dt_ms = 42u; /* clamp after sleep/pause to 1 frame @ 24 fps */
    }
    s_aq.prev_ms = now_ms;
    s_aq.now_ms = now_ms;

    if (biome != s_aq.last_biome) {
        s_aq.last_biome = biome;
        s_aq.toast_start_ms = now_ms;
        s_aq.toast_active = true;
    }

    s_aq.biome = biome;
    s_aq.coral_level = coral;
    s_aq.algae = algae;
    s_aq.hour = hour;
    s_aq.fish_count = nfish;
    s_aq.tilt_x = (int16_t)aq_clamp(tilt_x, -256, 256);
    s_aq.tilt_y = (int16_t)aq_clamp(tilt_y, -256, 256);

    /* Sync fish roster while preserving live motion of unchanged fish */
    for (int i = 0; i < JR_AQ_MAX_FISH; ++i) {
        if (i < (int)nfish && state != NULL) {
            const jr_display_aq_fish_t *sf = &state->fish[i];
            const uint8_t sp = sf->species <= 7u ? sf->species : 0u;
            if (!s_aq.fish[i].active ||
                s_aq.fish[i].seed != sf->seed ||
                s_aq.fish[i].species != sp) {
                aq_seed_fish(i, sf);
            } else {
                s_aq.fish[i].stage = sf->stage <= 3u ? sf->stage : 2u;
                s_aq.fish[i].hunger = sf->hunger <= 100u ? sf->hunger : 100u;
            }
        } else {
            s_aq.fish[i].active = false;
        }
    }

    /* Precompute biome geometry & palettes */
    aq_build_terrain(biome);
    aq_build_corals(biome, coral);
    if (biome == AQ_BIOME_KOI) {
        s_aq.is_night = false;
        aq_update_lilies(now_ms);
    } else {
        aq_update_reef_palette(now_ms, hour, s_aq.tilt_x);
        aq_update_kelp(now_ms, s_aq.tilt_x, biome, s_aq.is_night);
        if (biome == AQ_BIOME_ABYSS || s_aq.is_night) {
            aq_update_motes(now_ms, biome);
        }
    }

    /* Scale step factor relative to nominal 42 ms (24 fps) frame, Q8 */
    const int32_t dt_q8 = (int32_t)((dt_ms * 256u) / 42u);

    /* ---- Update food pellets --------------------------------------------- */
    for (int i = 0; i < JR_AQ_MAX_PELLETS; ++i) {
        aq_pellet_t *p = &s_aq.pellets[i];
        if (!p->active) {
            continue;
        }
        if (p->born_ms == 0u) {
            p->born_ms = now_ms;
        }
        if (dt_q8 > 0) {
            p->phase = (uint16_t)((p->phase + ((dt_q8 * 5) >> 8) + 1) & 255);
            const int32_t drift = (aq_sin((int32_t)p->phase) * 58) >> 14;
            const int32_t grav_x = (s_aq.tilt_x * 48) >> 8;
            p->x_q8 += ((drift + grav_x) * dt_q8) >> 8;
            p->y_q8 += (p->vy_q8 * dt_q8) >> 8;
        }
        p->px = (int16_t)(p->x_q8 >> 8);
        p->py = (int16_t)(p->y_q8 >> 8);
        const int dx = (int)p->px - JR_AQ_CX;
        const int dy = (int)p->py - JR_AQ_CY;
        const int floor_y = (biome == AQ_BIOME_KOI)
            ? 400
            : (int)s_aq.seabed_front[aq_clamp(p->px, 0, JR_AQ_W - 1)] - 6;
        if (p->py >= floor_y || dx * dx + dy * dy > 195 * 195 ||
            (now_ms - p->born_ms) > 14000u) {
            p->active = false;
        }
    }

    /* ---- Update expanding water ripples ---------------------------------- */
    for (int i = 0; i < JR_AQ_MAX_RIPPLES; ++i) {
        aq_ripple_t *rp = &s_aq.ripples[i];
        if (!rp->active) {
            continue;
        }
        if (rp->born_ms == 0u) {
            rp->born_ms = now_ms;
        }
        const uint32_t age = now_ms - rp->born_ms;
        if (age >= AQ_RIPPLE_MS) {
            rp->active = false;
            continue;
        }
        /* Ease-out radius 6 -> 88 px, quadratic alpha fade */
        const uint32_t t = (age * 256u) / AQ_RIPPLE_MS; /* 0..255 */
        const uint32_t inv = 256u - t;
        const uint32_t ease = 256u - ((inv * inv) >> 8);
        rp->r_px = (int16_t)(6u + ((82u * ease) >> 8));
        rp->alpha = (uint8_t)((220u * ((inv * inv) >> 8)) >> 8);
        rp->col = (biome == AQ_BIOME_KOI)
            ? aq_rgb565(178, 252, 232)
            : (biome == AQ_BIOME_ABYSS ? 0x07FFu : 0xAFFFu);
    }

    /* ---- Update Koi trailing swim wakes ---------------------------------- */
    for (int i = 0; i < AQ_MAX_WAKES; ++i) {
        aq_wake_t *wk = &s_aq.wakes[i];
        if (!wk->active) {
            continue;
        }
        const uint32_t age = now_ms - wk->born_ms;
        if (age >= AQ_WAKE_MS || biome != AQ_BIOME_KOI) {
            wk->active = false;
            continue;
        }
        const uint32_t t = (age * 256u) / AQ_WAKE_MS;
        const uint32_t inv = 256u - t;
        wk->r_px = (int16_t)(4u + ((30u * t) >> 8));
        wk->alpha = (uint8_t)((84u * ((inv * inv) >> 8)) >> 8);
    }

    /* ---- Update rising air bubbles --------------------------------------- */
    for (int i = 0; i < JR_AQ_MAX_BUBBLES; ++i) {
        aq_bubble_t *b = &s_aq.bubbles[i];
        if (!b->active) {
            continue;
        }
        if (dt_q8 > 0) {
            b->wobble_phase = (uint16_t)((b->wobble_phase + ((dt_q8 * 7) >> 8) + 1) & 255);
            const int32_t wob = (aq_sin((int32_t)b->wobble_phase) * (int32_t)b->wobble_amp * 22) >> 14;
            /* Bubbles float toward real-world UP (opposite horizontal gravity lean) */
            const int32_t grav_up = (-s_aq.tilt_x * 96) >> 8;
            b->x_q8 += ((wob + grav_up) * dt_q8) >> 8;
            b->y_q8 += (b->vy_q8 * dt_q8) >> 8;
        }
        b->px = (int16_t)(b->x_q8 >> 8);
        b->py = (int16_t)(b->y_q8 >> 8);
        const int cx_clamped = aq_clamp(b->px, 0, JR_AQ_W - 1);
        const int top_lim = (biome == AQ_BIOME_KOI) ? 48 : (int)s_aq.men_y[cx_clamped] + 3;
        const int dx = (int)b->px - JR_AQ_CX;
        const int dy = (int)b->py - JR_AQ_CY;
        if (b->py <= top_lim || dx * dx + dy * dy > 202 * 202) {
            if (b->burst) {
                b->active = false;
            } else {
                /* Respawn ambient bubble cleanly near the seabed */
                const int nx = 78 + (int)(aq_rand() % 310u);
                const int ny = 352 + (int)(aq_rand() % 48u);
                b->x_q8 = nx << 8;
                b->y_q8 = ny << 8;
                b->px = (int16_t)nx;
                b->py = (int16_t)ny;
                b->rad = (uint8_t)(2u + (aq_rand() % 3u));
                b->vy_q8 = -(95 + (int32_t)b->rad * 22 + (int32_t)(aq_rand() % 48u));
            }
        }
    }

    /* ---- Update all active fish physics & render caches ------------------ */
    const bool shaking = (int32_t)(s_aq.shake_until_ms - now_ms) > 0;
    const int y_min_swim = (biome == AQ_BIOME_KOI) ? 72 : 82;
    const int y_max_swim = (biome == AQ_BIOME_KOI) ? 386 : 348;

    /* Precompute Neon Tetra school centroid for gentle flocking */
    int32_t tetra_sx = 0, tetra_sy = 0, tetra_svx = 0, tetra_svy = 0;
    int tetra_n = 0;
    for (int i = 0; i < (int)nfish; ++i) {
        if (s_aq.fish[i].active && s_aq.fish[i].species == AQ_SP_TETRA) {
            tetra_sx += s_aq.fish[i].x_q8;
            tetra_sy += s_aq.fish[i].y_q8;
            tetra_svx += s_aq.fish[i].vx_q8;
            tetra_svy += s_aq.fish[i].vy_q8;
            tetra_n++;
        }
    }

    for (int i = 0; i < (int)nfish; ++i) {
        aq_fish_sim_t *f = &s_aq.fish[i];
        if (!f->active) {
            continue;
        }

        /* Find nearest food pellet within 165 px */
        int best_pel = -1;
        int32_t best_d2 = 165 * 165;
        for (int p = 0; p < JR_AQ_MAX_PELLETS; ++p) {
            if (!s_aq.pellets[p].active) {
                continue;
            }
            const int32_t pdx = (s_aq.pellets[p].x_q8 - f->x_q8) >> 8;
            const int32_t pdy = (s_aq.pellets[p].y_q8 - f->y_q8) >> 8;
            const int32_t pd2 = pdx * pdx + pdy * pdy;
            if (pd2 < best_d2) {
                best_d2 = pd2;
                best_pel = p;
            }
        }

        int32_t ax = 0, ay = 0;
        int32_t max_spd = 260;

        if (best_pel >= 0) {
            /* Eagerly steer toward the nearest golden pellet! */
            aq_pellet_t *pl = &s_aq.pellets[best_pel];
            const int32_t pdx = (pl->x_q8 - f->x_q8) >> 8;
            const int32_t pdy = (pl->y_q8 - f->y_q8) >> 8;
            ax += aq_clamp(pdx * 5, -90, 90);
            ay += aq_clamp(pdy * 5, -75, 75);
            max_spd = 520;
            if (best_d2 < 17 * 17) {
                /* CHOMP! Consume pellet + 3 celebratory micro-bubbles! */
                pl->active = false;
                const int mx = (int)(f->x_q8 >> 8) + (int)f->facing * 12;
                const int my = (int)(f->y_q8 >> 8) - 2;
                for (int b = 0; b < 3; ++b) {
                    aq_spawn_bubble(mx + (b - 1) * 5, my - b * 3, true, (uint8_t)(2u + (b & 1)));
                }
            }
        } else {
            /* Species-characteristic cruising & wander target */
            if ((int32_t)(now_ms - f->wander_next_ms) >= 0) {
                const uint32_t r = aq_rand();
                int tx = 96 + (int)(r % 274u);
                int ty = 120 + (int)((r >> 10) % 190u);
                if (f->species == AQ_SP_SEAHORSE) {
                    tx = (i & 1) ? (316 + (int)(r % 50u)) : (104 + (int)(r % 50u));
                    ty = 236 + (int)((r >> 10) % 86u);
                } else if (f->species == AQ_SP_MANTA) {
                    tx = (f->x_q8 < (JR_AQ_CX << 8)) ? (315 + (int)(r % 50u)) : (102 + (int)(r % 50u));
                    ty = 124 + (int)((r >> 10) % 120u);
                } else if (f->species == AQ_SP_TETRA) {
                    ty = 104 + (int)((r >> 10) % 105u);
                } else if (f->hunger >= 75u) {
                    /* Hungry fish cruise closer to the surface awaiting food */
                    ty = 102 + (int)((r >> 10) % 90u);
                }
                const int odx = tx - JR_AQ_CX;
                const int ody = ty - JR_AQ_CY;
                if (odx * odx + ody * ody > 148 * 148) {
                    tx = JR_AQ_CX + (odx * 2) / 3;
                    ty = JR_AQ_CY + (ody * 2) / 3;
                }
                f->wander_tx_q8 = tx << 8;
                f->wander_ty_q8 = ty << 8;
                f->wander_next_ms = now_ms + 2200u + (r % 2600u);
            }

            const int32_t wdx = (f->wander_tx_q8 - f->x_q8) >> 8;
            const int32_t wdy = (f->wander_ty_q8 - f->y_q8) >> 8;

            if (f->species == AQ_SP_JELLY) {
                /* Rhythmic vertical bell thrust + slow float down */
                const int32_t ph = (int32_t)((f->pulse_q8 >> 8) & 255u);
                const int32_t thrust = (ph < 96) ? (-(aq_sin((ph * 128) / 96) * 44) >> 14) : 14;
                ax += aq_clamp(wdx / 5, -12, 12);
                ay += thrust + aq_clamp(wdy / 8, -10, 10);
                max_spd = 175;
            } else if (f->species == AQ_SP_SEAHORSE) {
                /* Gentle upright bobbing near kelp */
                const int32_t bob = (aq_sin((int32_t)((f->pulse_q8 >> 8) & 255u)) * 18) >> 14;
                ax += aq_clamp(wdx / 4, -14, 14);
                ay += aq_clamp(wdy / 4, -14, 14) + bob / 2;
                max_spd = 130;
            } else if (f->species == AQ_SP_MANTA) {
                /* Sweeping wide graceful arcs */
                ax += aq_clamp(wdx / 3, -24, 24);
                ay += aq_clamp(wdy / 4, -18, 18);
                max_spd = 310;
            } else if (f->species == AQ_SP_KOI) {
                /* Serene stately glide */
                ax += aq_clamp(wdx / 3, -22, 22);
                ay += aq_clamp(wdy / 3, -18, 18);
                max_spd = 250;
            } else if (f->species == AQ_SP_TETRA) {
                /* Brisk darting + gentle tetra schooling */
                ax += aq_clamp(wdx / 2, -32, 32);
                ay += aq_clamp(wdy / 2, -24, 24);
                if (tetra_n > 1) {
                    const int32_t cx = tetra_sx / tetra_n;
                    const int32_t cy = tetra_sy / tetra_n;
                    ax += aq_clamp((cx - f->x_q8) >> 10, -18, 18);
                    ay += aq_clamp((cy - f->y_q8) >> 10, -14, 14);
                    ax += aq_clamp((tetra_svx / tetra_n - f->vx_q8) >> 5, -14, 14);
                    ay += aq_clamp((tetra_svy / tetra_n - f->vy_q8) >> 5, -10, 10);
                }
                max_spd = 350;
            } else {
                /* Clownfish, Blue Tang, Mandarin: lively reef cruising */
                ax += aq_clamp(wdx / 3, -26, 26);
                ay += aq_clamp(wdy / 3, -20, 20);
                max_spd = (f->species == AQ_SP_TANG) ? 310 : 250;
            }
        }

        /* Soft circular bowl wall, surface/seabed, and fish-to-fish spacing */
        for (int j = 0; j < (int)nfish; ++j) {
            if (j == i || !s_aq.fish[j].active) {
                continue;
            }
            const int32_t sdx = (f->x_q8 - s_aq.fish[j].x_q8) >> 8;
            const int32_t sdy = (f->y_q8 - s_aq.fish[j].y_q8) >> 8;
            const int32_t sd2 = sdx * sdx + sdy * sdy;
            if (sd2 > 0 && sd2 < 38 * 38) {
                ax += aq_clamp((sdx * 28) / (sd2 / 18 + 6), -36, 36);
                ay += aq_clamp((sdy * 24) / (sd2 / 18 + 6), -30, 30);
            }
        }
        const int32_t bdx = (f->x_q8 >> 8) - JR_AQ_CX;
        const int32_t bdy = (f->y_q8 >> 8) - JR_AQ_CY;
        const int32_t br2 = bdx * bdx + bdy * bdy;
        if (br2 > 152 * 152) {
            ax -= (bdx * 42) >> 7;
            ay -= (bdy * 42) >> 7;
        }
        const int32_t fy_px = f->y_q8 >> 8;
        if (fy_px < y_min_swim + 16) {
            ay += (y_min_swim + 16 - fy_px) * 3;
        } else if (fy_px > y_max_swim - 16) {
            ay -= (fy_px - (y_max_swim - 16)) * 3;
        }

        if (shaking) {
            max_spd = 620;
        }

        /* Integrate velocity & position */
        if (dt_q8 > 0) {
            f->vx_q8 += (ax * dt_q8) >> 8;
            f->vy_q8 += (ay * dt_q8) >> 8;
            /* Gentle water drag */
            f->vx_q8 = (f->vx_q8 * 246) >> 8;
            f->vy_q8 = (f->vy_q8 * 244) >> 8;
            /* Enforce minimum gentle swim speed for horizontal swimmers */
            if (f->species != AQ_SP_JELLY && f->species != AQ_SP_SEAHORSE &&
                aq_abs(f->vx_q8) < 42) {
                f->vx_q8 += (int32_t)f->facing * 18;
            }
            f->vx_q8 = aq_clamp(f->vx_q8, -max_spd, max_spd);
            f->vy_q8 = aq_clamp(f->vy_q8, -(max_spd * 3) / 4, (max_spd * 3) / 4);

            f->x_q8 += (f->vx_q8 * dt_q8) >> 8;
            f->y_q8 += (f->vy_q8 * dt_q8) >> 8;

            /* Hard safety clamp strictly inside r <= 178 */
            const int32_t nx = (f->x_q8 >> 8) - JR_AQ_CX;
            const int32_t ny = (f->y_q8 >> 8) - JR_AQ_CY;
            const uint32_t nr2 = (uint32_t)(nx * nx + ny * ny);
            if (nr2 > 176u * 176u) {
                const uint32_t nr = aq_isqrt(nr2);
                if (nr > 0u) {
                    f->x_q8 = (JR_AQ_CX + (nx * 174) / (int32_t)nr) << 8;
                    f->y_q8 = (JR_AQ_CY + (ny * 174) / (int32_t)nr) << 8;
                    f->vx_q8 = -f->vx_q8 / 2;
                    f->vy_q8 = -f->vy_q8 / 2;
                }
            }
            if ((f->y_q8 >> 8) < y_min_swim) {
                f->y_q8 = y_min_swim << 8;
                if (f->vy_q8 < 0) { f->vy_q8 = -f->vy_q8 / 2; }
            }
            if ((f->y_q8 >> 8) > y_max_swim) {
                f->y_q8 = y_max_swim << 8;
                if (f->vy_q8 > 0) { f->vy_q8 = -f->vy_q8 / 2; }
            }

            /* Facing flip hysteresis (never rapid jitter!) */
            if (now_ms - f->last_flip_ms >= 450u) {
                if (f->facing < 0 && f->vx_q8 > 28) {
                    f->facing = 1;
                    f->last_flip_ms = now_ms;
                } else if (f->facing > 0 && f->vx_q8 < -28) {
                    f->facing = -1;
                    f->last_flip_ms = now_ms;
                }
            }

            /* Advance articulated tail wag proportional to swim speed */
            const int32_t spd = aq_abs(f->vx_q8) + aq_abs(f->vy_q8);
            const uint32_t wag_step = (uint32_t)(10 + (spd >> 4)) * (uint32_t)dt_q8;
            f->tail_q8 += wag_step >> 8;
            const uint32_t pulse_rate = (f->species == AQ_SP_JELLY) ? 5u
                                      : (f->species == AQ_SP_SEAHORSE ? 24u : 8u);
            f->pulse_q8 += (pulse_rate * (uint32_t)dt_q8) >> 8;

            /* Emit subtle top-down swim wakes in Zen Koi Pond */
            if (biome == AQ_BIOME_KOI && spd > 90 &&
                (int32_t)(now_ms - f->wake_next_ms) >= 0) {
                aq_emit_wake((int)(f->x_q8 >> 8) - (int)f->facing * 14,
                             (int)(f->y_q8 >> 8), now_ms);
                f->wake_next_ms = now_ms + 280u + (uint32_t)(i * 35);
            }
        }

        /* Freeze per-frame render attributes for strip-invariant rendering */
        f->fx = (int16_t)(f->x_q8 >> 8);
        f->fy = (int16_t)(f->y_q8 >> 8);
        f->scale_q8 = s_aq_stage_scale[f->stage];
        f->inv_scale_q8 = (uint16_t)((65536u + (uint32_t)f->scale_q8 / 2u) / (uint32_t)f->scale_q8);
        f->wag_q8 = (int16_t)((aq_sin((int32_t)((f->tail_q8 >> 8) & 255u)) * 256) >> 14);
        f->pulse_q8_val = (int16_t)((aq_sin((int32_t)((f->pulse_q8 >> 8) & 255u)) * 128) >> 14);
        f->pitch_q8 = (f->species == AQ_SP_JELLY || f->species == AQ_SP_SEAHORSE)
            ? 0 : (int16_t)aq_clamp((f->vy_q8 * 180) / 300, -140, 140);
        f->elder_glow = (f->stage == 3u)
            ? (uint8_t)(120 + ((aq_sin((int32_t)((now_ms / 18u + (uint32_t)i * 41u) & 255u)) * 70) >> 14))
            : 0u;

        /* Tight bounding box in panel pixels (species-specific half extents) */
        int rx = 30, ry = 18;
        switch (f->species) {
        case AQ_SP_KOI:      rx = 38; ry = 20; break;
        case AQ_SP_MANTA:    rx = 38; ry = 26; break;
        case AQ_SP_JELLY:    rx = 24; ry = 32; break;
        case AQ_SP_SEAHORSE: rx = 20; ry = 26; break;
        case AQ_SP_TETRA:    rx = 28; ry = 12; break;
        case AQ_SP_MANDARIN: rx = 30; ry = 20; break;
        case AQ_SP_TANG:     rx = 32; ry = 18; break;
        default:             rx = 30; ry = 18; break;
        }
        if (f->stage == 3u) {
            rx += 4;
            ry += 4;
        }
        const int sx = ((rx * (int)f->scale_q8) >> 8) + 3;
        const int sy = ((ry * (int)f->scale_q8) >> 8) + 3;
        f->bx0 = (int16_t)aq_max(f->fx - sx, 0);
        f->bx1 = (int16_t)aq_min(f->fx + sx, JR_AQ_W - 1);
        f->by0 = (int16_t)aq_max(f->fy - sy, 0);
        f->by1 = (int16_t)aq_min(f->fy + sy, JR_AQ_H - 1);
    }
}

/* ==========================================================================
 * ROW RENDERERS: BACKDROPS, CORALS, KELP, MOTES, LILY PADS, EFFECTS
 * ========================================================================== */

static void aq_render_bg_row(uint16_t *row, int y, int lo, int hi, bool swap)
{
    if (s_aq.biome == AQ_BIOME_ABYSS) {
        /* Pure OLED #000000 above y=288; subtle deep midnight-violet glow below */
        if (y < 288) {
            memset(row + lo, 0, (size_t)(hi - lo + 1) * sizeof(uint16_t));
            /* Whisper-faint bioluminescent halocline near top surface */
            if (y >= 54 && y <= 62) {
                const int wph = (int)((s_aq.now_ms / 24u) & 255u);
                for (int x = lo; x <= hi; ++x) {
                    const int wy = 58 + ((aq_sin(x * 2 + wph) * 2) >> 14) +
                                   ((s_aq.tilt_x * (x - JR_AQ_CX)) >> 11);
                    const int d = aq_abs(y - wy);
                    if (d <= 1) {
                        const uint16_t c = (d == 0) ? aq_rgb565(18, 42, 82)
                                                    : aq_rgb565(8, 18, 42);
                        row[x] = swap ? aq_bswap(c) : c;
                    }
                }
            }
        } else {
            const int dy = y - 418;
            const int dy2 = dy * dy;
            for (int x = lo; x <= hi; ++x) {
                const int dx = x - JR_AQ_CX;
                const int d2 = (dx * dx) / 2 + dy2;
                if (d2 >= 18000) {
                    row[x] = 0u;
                } else {
                    const int k = (18000 - d2) / 280; /* 0..64 */
                    const uint16_t c = aq_rgb565(k / 3, k / 9, (k * 3) / 4);
                    row[x] = swap ? aq_bswap(c) : c;
                }
            }
        }
        return;
    }

    if (s_aq.biome == AQ_BIOME_KOI) {
        /* Fast DDA radial emerald-jade pond gradient with 2x2 ordered dither */
        const int dy = y - JR_AQ_CY;
        const int dy2 = dy * dy;
        int dx = lo - JR_AQ_CX;
        int32_t r2 = dx * dx + dy2;
        const int ybit = (y & 1) << 1;
        for (int x = lo; x <= hi; ++x) {
            /* 2x2 Bayer dither breaks RGB565 quantization rings cleanly */
            const int dith = ((x & 1) ^ ybit) * 140 - 210;
            int32_t r2d = r2 + dith;
            if (r2d < 0) { r2d = 0; }
            int idx = (int)(r2d >> 9);
            if (idx > 95) { idx = 95; }
            const uint16_t c = s_aq.koi_rad_lut[idx];
            row[x] = swap ? aq_bswap(c) : c;
            r2 += 2 * dx + 1;
            dx++;
        }
        /* Subtle sunlit dappled water caustics (3 soft slow pools) */
        const int t = (int)((s_aq.now_ms / 48u) & 255u);
        static const int16_t k_cx[3] = { 174, 294, 224 };
        static const int16_t k_cy[3] = { 178, 228, 296 };
        const uint16_t dap_col = aq_rgb565(36, 118, 96);
        for (int i = 0; i < 3; ++i) {
            const int cy = k_cy[i] + ((aq_cos(t + i * 87) * 14) >> 14);
            const int ddy = y - cy;
            if (aq_abs(ddy) >= 44) {
                continue;
            }
            const int cx = k_cx[i] + ((aq_sin(t + i * 73) * 16) >> 14);
            const int x0 = aq_max(lo, cx - 44);
            const int x1 = aq_min(hi, cx + 44);
            const int ddy2 = ddy * ddy;
            for (int x = x0; x <= x1; ++x) {
                const int ddx = x - cx;
                const int d2 = ddx * ddx + ddy2;
                if (d2 < 1936) {
                    const unsigned a = (unsigned)(((1936 - d2) * 34) >> 11);
                    if (a > 0u) {
                        row[x] = aq_add565(row[x], dap_col, a, swap);
                    }
                }
            }
        }
        return;
    }

    /* BIOME 0: TROPICAL REEF (circadian air dome + tilted meniscus + ocean gradient) */
    const uint16_t raw_air   = swap ? aq_bswap(s_aq.row_air[y])   : s_aq.row_air[y];
    const uint16_t raw_water = swap ? aq_bswap(s_aq.row_water[y]) : s_aq.row_water[y];

    if (y < s_aq.men_y_min - 1) {
        for (int x = lo; x <= hi; ++x) {
            row[x] = raw_air;
        }
    } else if (y > s_aq.men_y_max + 5) {
        const uint16_t wn = (y + 1 < JR_AQ_H) ? s_aq.row_water[y + 1] : s_aq.row_water[y];
        const uint16_t raw_next = swap ? aq_bswap(wn) : wn;
        if (raw_next == raw_water) {
            for (int x = lo; x <= hi; ++x) {
                row[x] = raw_water;
            }
        } else {
            for (int x = lo; x <= hi; ++x) {
                row[x] = ((x ^ y) & 1) ? raw_next : raw_water;
            }
        }
    } else {
        const uint16_t crest_raw = swap ? aq_bswap(s_aq.men_crest_col) : s_aq.men_crest_col;
        for (int x = lo; x <= hi; ++x) {
            const int my = (int)s_aq.men_y[x];
            if (y < my) {
                row[x] = raw_air;
            } else if (y <= my + 1) {
                row[x] = crest_raw;
            } else if (y <= my + 5) {
                const unsigned a = (unsigned)((6 - (y - my)) * 42);
                row[x] = aq_blend565(raw_water, s_aq.men_glow_col, a, swap);
            } else {
                row[x] = raw_water;
            }
        }
    }

    /* Nighttime twinkling stars in the air dome above meniscus */
    if (s_aq.is_night && y < s_aq.men_y_max) {
        const int tw_t = (int)((s_aq.now_ms / 32u) & 255u);
        for (int s = 0; s < 14; ++s) {
            const uint32_t h = aq_hash32((uint32_t)s * 6151u + 99u);
            const int sy = 26 + (int)(h % 56u);
            if (sy != y) {
                continue;
            }
            const int sx = 92 + (int)((h >> 8) % 282u);
            if (sx >= lo && sx <= hi && y < (int)s_aq.men_y[sx] - 3) {
                const unsigned a = (unsigned)(120 + ((aq_sin(tw_t + s * 53) * 100) >> 14));
                row[sx] = aq_add565(row[sx], 0xD7FFu, a, swap);
            }
        }
    }

    /* Diagonal volumetric caustic sun/moon shafts strictly below meniscus */
    if (y >= s_aq.men_y_min && y < 388) {
        const int depth_fade = aq_clamp(((390 - y) * 255) / 310, 0, 255);
        for (int i = 0; i < AQ_NUM_BEAMS; ++i) {
            const aq_beam_t *bm = &s_aq.beams[i];
            const int dy = y - 56;
            const int bx = (int)bm->x_top + ((dy * (int)bm->slope_q8) >> 8);
            const int hw = (int)bm->half_w + (dy >> 4);
            const int x0 = aq_max(lo, bx - hw);
            const int x1 = aq_min(hi, bx + hw);
            if (x0 > x1) {
                continue;
            }
            const int peak = ((int)bm->peak_a * depth_fade) >> 8;
            if (peak <= 1) {
                continue;
            }
            for (int x = x0; x <= x1; ++x) {
                if (y <= (int)s_aq.men_y[x] + 1) {
                    continue;
                }
                const int dist = aq_abs(x - bx);
                const unsigned a = (unsigned)(((hw - dist) * peak) / hw);
                if (a > 0u) {
                    row[x] = aq_add565(row[x], bm->col, a, swap);
                }
            }
        }
    }
}

static void aq_render_seabed_and_corals_row(uint16_t *row, int y, int lo, int hi, bool swap)
{
    if (s_aq.biome == AQ_BIOME_KOI) {
        return;
    }

    /* Layered back & foreground seabed curves */
    if (y >= s_aq.seabed_y_min) {
        const uint16_t b_raw = swap ? aq_bswap(s_aq.seabed_back_col)  : s_aq.seabed_back_col;
        const uint16_t f_raw = swap ? aq_bswap(s_aq.seabed_front_col) : s_aq.seabed_front_col;
        const uint16_t r_raw = swap ? aq_bswap(s_aq.seabed_rim_col)   : s_aq.seabed_rim_col;
        for (int x = lo; x <= hi; ++x) {
            const int yf = (int)s_aq.seabed_front[x];
            if (y >= yf) {
                if (y <= yf + 1) {
                    row[x] = aq_blend565(f_raw, s_aq.seabed_rim_col, (y == yf) ? 165u : 75u, swap);
                } else {
                    row[x] = f_raw;
                }
            } else if (y >= (int)s_aq.seabed_back[x]) {
                row[x] = (y == (int)s_aq.seabed_back[x])
                    ? aq_blend565(b_raw, s_aq.seabed_rim_col, 65u, swap) : b_raw;
            }
        }
        (void)r_raw;
    }

    /* Procedural branching coral segments (bounding-box culled per row) */
    for (int i = 0; i < s_aq.coral_seg_count; ++i) {
        const aq_coral_seg_t *s = &s_aq.coral_segs[i];
        if (y < s->y_min || y > s->y_max) {
            continue;
        }
        const int dx = (int)s->x1 - (int)s->x0;
        const int dy = (int)s->y1 - (int)s->y0;
        const int len2 = dx * dx + dy * dy;
        const int rmax = (int)(s->r0 > s->r1 ? s->r0 : s->r1);
        const int x0 = aq_max(lo, aq_min(s->x0, s->x1) - rmax - 1);
        const int x1 = aq_min(hi, aq_max(s->x0, s->x1) + rmax + 1);
        for (int x = x0; x <= x1; ++x) {
            int t_q8 = 0;
            if (len2 > 0) {
                t_q8 = (((x - (int)s->x0) * dx + (y - (int)s->y0) * dy) * 256) / len2;
                t_q8 = aq_clamp(t_q8, 0, 256);
            }
            const int px = (int)s->x0 + ((dx * t_q8) >> 8);
            const int py = (int)s->y0 + ((dy * t_q8) >> 8);
            const int rad = (int)s->r0 + ((((int)s->r1 - (int)s->r0) * t_q8) >> 8);
            const int ex = x - px;
            const int ey = y - py;
            const int d2 = ex * ex + ey * ey;
            const int rr = rad * rad;
            if (d2 <= rr + rad * 2) {
                /* Top-left lit coral shading */
                const int hi_mix = aq_clamp(128 - ex * 18 - ey * 16 + (t_q8 >> 2), 0, 255);
                const uint16_t col = aq_lerp565(s->col_base, s->col_hi, hi_mix);
                if (d2 <= rr) {
                    row[x] = swap ? aq_bswap(col) : col;
                } else {
                    const unsigned a = (unsigned)(((rr + rad * 2 - d2) * 220) / (rad * 2 + 1));
                    row[x] = aq_blend565(row[x], col, a, swap);
                }
            }
        }
    }

    /* Bioluminescent glowing coral polyp tips (brighter at night & Abyss!) */
    const int p_t = (int)((s_aq.now_ms / 20u) & 255u);
    const int boost = (s_aq.biome == AQ_BIOME_ABYSS || s_aq.is_night) ? 245 : 145;
    for (int i = 0; i < s_aq.polyp_count; ++i) {
        const aq_polyp_t *p = &s_aq.polyps[i];
        const int gr = (int)p->rad + 4;
        const int dy = y - (int)p->cy;
        if (aq_abs(dy) > gr) {
            continue;
        }
        const int x0 = aq_max(lo, (int)p->cx - gr);
        const int x1 = aq_min(hi, (int)p->cx + gr);
        const int pulse = 180 + ((aq_sin(p_t + (int)p->phase) * 70) >> 14);
        const int peak = (boost * pulse) >> 8;
        const int gr2 = gr * gr;
        const int cr2 = (int)p->rad * (int)p->rad;
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - (int)p->cx;
            const int d2 = dx * dx + dy * dy;
            if (d2 < gr2) {
                if (d2 <= cr2) {
                    const uint16_t core = aq_lerp565(p->col, 0xFFFFu, 110);
                    row[x] = aq_add565(row[x], core, (unsigned)aq_min(peak, 255), swap);
                } else {
                    const unsigned a = (unsigned)(((gr2 - d2) * peak) / (gr2 * 2));
                    row[x] = aq_add565(row[x], p->col, a, swap);
                }
            }
        }
    }
}

static void aq_render_kelp_row(uint16_t *row, int y, int lo, int hi, bool swap)
{
    if (s_aq.biome == AQ_BIOME_KOI) {
        return;
    }
    const bool abyss = (s_aq.biome == AQ_BIOME_ABYSS);
    for (int i = 0; i < AQ_NUM_KELP; ++i) {
        const aq_kelp_t *k = &s_aq.kelp[i];
        if (y < k->y_top || y > k->y_bot) {
            continue;
        }
        const int span = (int)k->y_bot - (int)k->y_top;
        if (span <= 0) {
            continue;
        }
        /* Interpolate knot s in [0..16] from bottom to top */
        const int pos_q8 = (((int)k->y_bot - y) * ((AQ_KELP_KNOTS - 1) << 8)) / span;
        const int s0 = aq_clamp(pos_q8 >> 8, 0, AQ_KELP_KNOTS - 2);
        const int frac = pos_q8 & 255;
        const int cx = (int)k->kx[s0] + ((((int)k->kx[s0 + 1] - (int)k->kx[s0]) * frac) >> 8);
        const int hw = (int)k->hw[s0] + ((((int)k->hw[s0 + 1] - (int)k->hw[s0]) * frac) >> 8);
        const int x0 = aq_max(lo, cx - hw - 1);
        const int x1 = aq_min(hi, cx + hw + 1);
        for (int x = x0; x <= x1; ++x) {
            const int dist = aq_abs(x - cx);
            if (dist > hw + 1) {
                continue;
            }
            const uint16_t col = (x < cx) ? k->col_hi
                               : (dist == 0 ? k->col_mid : k->col_dark);
            unsigned a = (dist <= hw) ? (abyss ? 145u : 178u) : 72u;
            if (abyss && x == cx - hw) {
                row[x] = aq_add565(row[x], k->col_hi, 120u, swap);
            } else {
                row[x] = aq_blend565(row[x], col, a, swap);
            }
        }
    }
}

static void aq_render_motes_row(uint16_t *row, int y, int lo, int hi, bool swap)
{
    if (s_aq.biome != AQ_BIOME_ABYSS && !(s_aq.biome == AQ_BIOME_REEF && s_aq.is_night)) {
        return;
    }
    for (int i = 0; i < AQ_NUM_MOTES; ++i) {
        const aq_mote_t *m = &s_aq.motes[i];
        if (m->alpha == 0u) {
            continue;
        }
        const int gr = (int)m->rad + 3;
        const int dy = y - (int)m->y;
        if (aq_abs(dy) > gr) {
            continue;
        }
        const int x0 = aq_max(lo, (int)m->x - gr);
        const int x1 = aq_min(hi, (int)m->x + gr);
        const int gr2 = gr * gr;
        const int dy2 = dy * dy;
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - (int)m->x;
            const int d2 = dx * dx + dy2;
            if (d2 < gr2) {
                const unsigned a = (unsigned)(((gr2 - d2) * (int)m->alpha) / gr2);
                row[x] = aq_add565(row[x], m->col, a, swap);
            }
        }
    }
}

static void aq_render_wakes_row(uint16_t *row, int y, int lo, int hi, bool swap)
{
    if (s_aq.biome != AQ_BIOME_KOI) {
        return;
    }
    const uint16_t wcol = aq_rgb565(132, 224, 198);
    for (int i = 0; i < AQ_MAX_WAKES; ++i) {
        const aq_wake_t *wk = &s_aq.wakes[i];
        if (!wk->active || wk->alpha == 0u) {
            continue;
        }
        const int r = (int)wk->r_px;
        const int dy = y - (int)wk->cy;
        const int ady = aq_abs(dy);
        if (ady > r + 2) {
            continue;
        }
        const int r_out = r + 1;
        const int r_in = r > 2 ? r - 2 : 0;
        const int x0 = aq_max(lo, (int)wk->cx - r_out);
        const int x1 = aq_min(hi, (int)wk->cx + r_out);
        const int r_out2 = r_out * r_out;
        const int r_in2 = r_in * r_in;
        const int dy2 = dy * dy;
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - (int)wk->cx;
            const int d2 = dx * dx + dy2;
            if (d2 <= r_out2 && d2 >= r_in2) {
                row[x] = aq_add565(row[x], wcol, wk->alpha, swap);
            }
        }
    }
}

/* ==========================================================================
 * 8 PROCEDURAL FISH SPECIES RENDERERS (LOCAL Q4 SPACE, ANTI-ALIASED)
 * ========================================================================== */

static void aq_render_fish_row(uint16_t *row, int y, int lo, int hi,
                               const aq_fish_sim_t *f, bool swap)
{
    if (!f->active || y < f->by0 || y > f->by1) {
        return;
    }
    const int x0 = aq_max(lo, (int)f->bx0);
    const int x1 = aq_min(hi, (int)f->bx1);
    if (x0 > x1) {
        return;
    }

    const int fx = (int)f->fx;
    const int fy = (int)f->fy;
    const int facing = (int)f->facing;
    const int inv_s = (int)f->inv_scale_q8;
    const int wag = (int)f->wag_q8;
    const int pitch = (int)f->pitch_q8;
    const int pulse = (int)f->pulse_q8_val;
    const uint16_t seed = f->seed;
    const uint8_t sp = f->species;
    const bool elder = (f->stage == 3u);
    const bool abyss = (s_aq.biome == AQ_BIOME_ABYSS);
    const uint16_t aura_col = abyss ? 0x07FFu : 0xFFE8u;
    const int dy_raw = y - fy;
    const int j_rx = 248 + (pulse * 34) / 128;
    const int j_ry = 224 - (pulse * 26) / 128;
    const uint32_t j_inv_rx2 = (sp == AQ_SP_JELLY)
        ? (uint32_t)((1u << 24) / (uint32_t)(j_rx * j_rx)) : 0u;
    const uint32_t j_inv_ry2 = (sp == AQ_SP_JELLY)
        ? (uint32_t)((1u << 24) / (uint32_t)(j_ry * j_ry)) : 0u;

    for (int x = x0; x <= x1; ++x) {
        const int dx_raw = (x - fx) * facing;
        const int dy_p = dy_raw + ((dx_raw * pitch) >> 10);

        /* Canonical Q4 fish coordinates (16 units = 1 canonical adult pixel) */
        const int lx = (dx_raw * inv_s) >> 4;
        const int ly = (dy_p * inv_s) >> 4;

        uint16_t col = 0u;
        unsigned alpha = 0u;
        bool additive = false;
        bool edge_rim = false;

        if (sp == AQ_SP_CLOWN) {
            /* Articulated tail bend for rear half (lx < 0) */
            const int bend = (lx < 0) ? ((((-lx) * (-lx)) >> 9) * wag) >> 8 : 0;
            const int by = ly + bend;

            /* Plump clownfish body oval: rx=16 (256 Q4), ry=10 (160 Q4) */
            const int bdist = (lx * lx) / 256 + (by * by) / 100; /* <= 256 inside */
            /* Fan caudal tail: lx in [-420..-210] */
            bool in_tail = false;
            int tail_alpha = 0;
            if (lx <= -192 && lx >= -420) {
                const int tw = 48 + ((-lx - 192) * 115) / 228;
                const int ady = aq_abs(by);
                const int end_curve = -408 + ((ady * ady) >> 8);
                if (ady <= tw && lx >= end_curve) {
                    in_tail = true;
                    tail_alpha = (ady > tw - 16 || lx < end_curve + 16) ? 170 : 245;
                }
            }
            /* Rounded dorsal & ventral fins */
            const bool in_dorsal = (lx >= -160 && lx <= 100 && by <= -118 &&
                                    ((lx + 24) * (lx + 24)) / 180 + ((by + 132) * (by + 132)) / 36 <= 115);
            const bool in_ventral = (lx >= -130 && lx <= 60 && by >= 118 &&
                                     ((lx + 30) * (lx + 30)) / 140 + ((by - 126) * (by - 126)) / 30 <= 96);

            if (bdist <= 278) {
                alpha = (bdist <= 240) ? 255u : (unsigned)(((278 - bdist) * 255) / 38);
                edge_rim = (bdist > 215);
                /* Vibrant sunlit tangerine body shading */
                const int shade = aq_clamp(128 - (by * 120) / 160, 0, 255);
                col = aq_lerp565(0xD240u, 0xFEA0u, shade);

                /* 3 crisp white vertical clownfish bands bordered in deep charcoal */
                const int b1 = aq_abs(lx - 132);           /* head collar */
                const int b2 = aq_abs(lx + 16 + (by >> 4));/* curved mid saddle */
                const int b3 = aq_abs(lx + 196);           /* tail-base bar */
                if (b1 < 22 || b2 < 26 || b3 < 15) {
                    col = 0xF7FEu;
                } else if (b1 < 36 || b2 < 40 || b3 < 26) {
                    col = 0x10A2u;
                }
                /* Glossy eye at (+180, -34) */
                const int ex = lx - 180;
                const int ey = by + 34;
                const int ed2 = ex * ex + ey * ey;
                if (ed2 < 36 * 36) {
                    if (ed2 < 17 * 17) {
                        col = (ex > -4 && ey < -2 && ed2 > 5 * 5) ? 0xFFFFu : 0x0841u;
                    } else {
                        col = 0xF7BEu;
                    }
                }
            } else if (in_tail || in_dorsal || in_ventral) {
                alpha = in_tail ? (unsigned)tail_alpha : 230u;
                /* Black-tipped bright orange fins */
                const bool tip = (in_tail && lx < -378) || (in_dorsal && by < -180) ||
                                 (in_ventral && by > 168);
                col = tip ? 0x10A2u : 0xFC60u;
                edge_rim = tip;
            } else if (elder && bdist <= 345) {
                additive = true;
                col = aura_col;
                alpha = (unsigned)((((345 - bdist) * (int)f->elder_glow) / 67) >> 1);
            }
        } else if (sp == AQ_SP_TANG) {
            /* Regal Blue Tang: tall oval body, dark midnight palette swoosh, canary yellow tail! */
            const int bend = (lx < 0) ? ((((-lx) * (-lx)) >> 9) * wag) >> 8 : 0;
            const int by = ly + bend;
            const int bdist = (lx * lx) / 316 + (by * by) / 140;

            /* Vibrant canary yellow forked wedge tail */
            bool in_tail = false;
            if (lx <= -240 && lx >= -456) {
                const int tw = 34 + ((-lx - 240) * 140) / 216;
                const int ady = aq_abs(by);
                const int fork = -456 + (tw - ady) / 3;
                if (ady <= tw && lx >= fork) {
                    in_tail = true;
                }
            }
            const bool in_dorsal = (lx >= -190 && lx <= 120 && by <= -145 && by >= -210 &&
                                    aq_abs(lx + 35) < 155);

            if (bdist <= 278) {
                alpha = (bdist <= 242) ? 255u : (unsigned)(((278 - bdist) * 255) / 36);
                edge_rim = (bdist > 220);
                const int shade = aq_clamp(140 - (by * 115) / 185, 0, 255);
                col = aq_lerp565(0x0259u, 0x25DFu, shade); /* electric royal sapphire */

                /* Iconic midnight-navy palette swoosh across upper flank */
                const int swoosh_d = ((lx + 24) * (lx + 24)) / 240 + ((by + 36) * (by + 36)) / 68;
                const int hole_d   = ((lx + 28) * (lx + 28)) / 100 + ((by + 16) * (by + 16)) / 28;
                if ((swoosh_d < 130 && hole_d > 56 && lx < 180) ||
                    (lx <= -160 && aq_abs(by) < 58)) {
                    col = 0x0845u;
                }
                /* Yellow tail root & pectoral highlight */
                if (lx < -215 && aq_abs(by) < 42) {
                    col = 0xFFE0u;
                } else if (lx >= 15 && lx <= 86 && by >= 36 && by <= 84) {
                    col = aq_lerp565(col, 0xFFE0u, 150);
                }
                /* Eye at (+198, -42) */
                const int ex = lx - 198;
                const int ey = by + 42;
                const int ed2 = ex * ex + ey * ey;
                if (ed2 < 34 * 34) {
                    col = (ed2 < 16 * 16) ? 0x0841u : 0xEFBFu;
                }
            } else if (in_tail) {
                alpha = 248u;
                /* Crisp canary yellow wedge with thin black edge rim */
                const int tw = 34 + ((-lx - 240) * 140) / 216;
                col = (aq_abs(by) > tw - 18) ? 0x0845u : 0xFFE0u;
                edge_rim = true;
            } else if (in_dorsal) {
                alpha = 225u;
                col = 0x03DFu;
            } else if (elder && bdist <= 345) {
                additive = true;
                col = aura_col;
                alpha = (unsigned)((((345 - bdist) * (int)f->elder_glow) / 67) >> 1);
            }
        } else if (sp == AQ_SP_TETRA) {
            /* Neon Tetra: sleek minnow with blazing electric cyan stripe + fiery crimson rear belly */
            const int bend = (lx < 0) ? ((((-lx) * (-lx)) >> 8) * wag) >> 8 : 0;
            const int by = ly + bend;
            const int bdist = (lx * lx) / 260 + (by * by) / 36;

            bool in_tail = false;
            if (lx <= -200 && lx >= -380) {
                const int tw = 22 + ((-lx - 200) * 78) / 180;
                const int ady = aq_abs(by);
                const int fork = -380 + (tw - ady) / 2;
                if (ady <= tw && lx >= fork) {
                    in_tail = true;
                }
            }
            const bool in_dorsal = (lx >= -65 && lx <= 42 && by <= -64 && by >= -118 &&
                                    (lx + 65) > (-by - 64) * 2 / 3);

            if (bdist <= 278) {
                alpha = (bdist <= 238) ? 255u : (unsigned)(((278 - bdist) * 255) / 40);
                edge_rim = (bdist > 210);
                if (by < -38) {
                    col = 0x1B94u; /* translucent dorsal blue */
                } else if (by <= 6 && lx > -165) {
                    col = 0x07FFu; /* BLAZING electric neon cyan stripe! */
                    edge_rim = true;
                } else if (lx < 32) {
                    col = 0xF884u; /* fiery crimson rear belly & tail base! */
                } else {
                    col = 0xD75Eu; /* pearlescent silver front belly */
                }
                const int ex = lx - 182;
                const int ey = by + 12;
                const int ed2 = ex * ex + ey * ey;
                if (ed2 < 28 * 28) {
                    col = (ed2 < 13 * 13) ? 0x0841u : 0x07FFu;
                }
            } else if (in_tail || in_dorsal) {
                alpha = 155u;
                col = in_tail ? 0xFD9Au : 0x9F7Fu;
            } else if (elder && bdist <= 360) {
                additive = true;
                col = 0x07FFu;
                alpha = (unsigned)((((360 - bdist) * (int)f->elder_glow) / 82) >> 1);
            }
        } else if (sp == AQ_SP_KOI) {
            /* Golden Koi: graceful sinuous elongated body, calico patches, sweeping pectoral fins */
            const int s_wave = (aq_sin((int32_t)(((f->tail_q8 >> 8) - (uint32_t)((lx + 360) >> 4)) & 255u)) *
                                aq_max(0, 180 - lx)) >> 15;
            const int by = ly + (s_wave >> 1);

            /* Elongated tapered koi body: fuller near head (+140), tapering smoothly to tail (-300) */
            const int half_h = (lx >= 0)
                ? (132 - (lx * lx) / 880)
                : (132 - ((-lx) * 95) / 310);
            const bool in_body = (lx >= -310 && lx <= 336 && half_h > 8 && aq_abs(by) <= half_h);

            /* Flowing veil caudal tail: lx in [-520..-290] */
            bool in_tail = false;
            if (lx < -280 && lx >= -520) {
                const int tw = 34 + ((-lx - 280) * 105) / 240;
                const int ady = aq_abs(by);
                const int fork = -520 + (tw - ady) / 2;
                if (ady <= tw && lx >= fork) {
                    in_tail = true;
                }
            }

            /* Sweeping translucent pectoral fins flanking shoulders (stunning top-down!) */
            bool in_pec = false;
            if (lx >= -10 && lx <= 185) {
                const int ady = aq_abs(by);
                const int p_out = 104 + ((185 - lx) * 110) / 195;
                const int p_in  = 96;
                if (ady >= p_in && ady <= p_out && ((lx - 65) * (lx - 65)) / 180 + ((ady - 145) * (ady - 145)) / 50 < 120) {
                    in_pec = true;
                }
            }

            if (in_body) {
                const int edge_d = half_h - aq_abs(by);
                alpha = (edge_d >= 16) ? 255u : (unsigned)((edge_d * 255) / 16);
                edge_rim = (edge_d < 22);
                /* Pearlescent ivory white base with gentle dorsal shading */
                const int sh = aq_clamp(210 - (aq_abs(by) * 90) / (half_h + 1), 80, 255);
                col = aq_lerp565(0xBDF7u, 0xFFFFu, sh);

                /* Seeded organic Kohaku / Sanke golden-amber & vermilion calico patches */
                const int p1x = lx - (80 + (int)((seed & 15u) * 6u));
                const int p1y = by + ((int)((seed >> 4) & 7u) - 3) * 12;
                const int p2x = lx + (55 + (int)(((seed >> 7) & 15u) * 5u));
                const int p2y = by - ((int)((seed >> 11) & 7u) - 3) * 10;
                const int p3x = lx - 220;
                const int p3y = by;
                if (p1x * p1x + p1y * p1y * 2 < 110 * 110) {
                    col = aq_lerp565(0xF940u, 0xFD20u, sh >> 1); /* vermilion-gold patch */
                } else if (p2x * p2x + p2y * p2y * 2 < 128 * 128) {
                    col = aq_lerp565(0xFA40u, 0xFEA0u, sh >> 1); /* golden-amber patch */
                } else if (p3x * p3x + p3y * p3y < 66 * 66) {
                    col = 0xF940u; /* crimson head tancho spot */
                }
                /* Subtle sumi ink accent dot */
                const int sdx = lx + 12;
                const int sdy = by + 32;
                if (sdx * sdx + sdy * sdy < 30 * 30 && (seed & 1u) != 0u) {
                    col = 0x18E3u;
                }
                /* Twin dark koi eyes */
                const int ex = lx - 252;
                if (ex * ex + (aq_abs(by) - 44) * (aq_abs(by) - 44) < 18 * 18) {
                    col = 0x0841u;
                }
            } else if (in_tail || in_pec) {
                alpha = 165u;
                col = aq_lerp565(0xF7BEu, 0xFEA0u, in_tail ? 110 : 85);
            } else if (elder && lx >= -340 && lx <= 360 && half_h > 0 && aq_abs(by) <= half_h + 44) {
                additive = true;
                col = 0xFFE8u;
                alpha = (unsigned)((((half_h + 44 - aq_abs(by)) * (int)f->elder_glow) / 44) >> 1);
            }
        } else if (sp == AQ_SP_MANDARIN) {
            /* Psychedelic Mandarin Dragonet: neon teal + tangerine + violet wavy stripes + tall crest! */
            const int bend = (lx < 0) ? ((((-lx) * (-lx)) >> 9) * wag) >> 8 : 0;
            const int by = ly + bend;
            const int bdist = (lx * lx) / 270 + (by * by) / 86;

            bool in_tail = false;
            if (lx <= -205 && lx >= -410) {
                const int tw = 46 + ((-lx - 205) * 115) / 205;
                const int ady = aq_abs(by);
                if (ady <= tw && lx >= -402 + ((ady * ady) >> 8)) {
                    in_tail = true;
                }
            }
            /* Tall dramatic dragonet dorsal sail & ventral fan */
            const bool in_crest = (lx >= -160 && lx <= 115 && by <= -96 &&
                                   by >= -248 + aq_abs(lx + 15) / 2 + ((aq_sin(lx / 4) * 18) >> 14));
            const bool in_vent = (lx >= -140 && lx <= 65 && by >= 96 && by <= 185);

            if (bdist <= 278) {
                alpha = (bdist <= 240) ? 255u : (unsigned)(((278 - bdist) * 255) / 38);
                edge_rim = (bdist > 218);
                /* Psychedelic wavy zebra bands: teal / tangerine / violet */
                const int wave_x = lx + ((aq_sin(by / 3 + (int)seed) * 42) >> 14);
                const int band = (wave_x + 1024) / 42;
                const int mod = band % 3;
                if (mod == 0) {
                    col = 0x07F8u; /* neon electric teal */
                } else if (mod == 1) {
                    col = 0xFC60u; /* vibrant tangerine */
                } else {
                    col = 0xA1BFu; /* psychedelic royal violet */
                }
                /* Bright amber-ringed eye */
                const int ex = lx - 178;
                const int ey = by + 30;
                const int ed2 = ex * ex + ey * ey;
                if (ed2 < 34 * 34) {
                    col = (ed2 < 16 * 16) ? 0x0841u : 0xFD40u;
                }
            } else if (in_crest || in_tail || in_vent) {
                alpha = 225u;
                const int stripe = ((lx + by + 1024) / 34) & 1;
                col = stripe ? 0x07FFu : (in_crest ? 0xFC60u : 0xA1BFu);
                edge_rim = true;
            } else if (elder && bdist <= 345) {
                additive = true;
                col = 0x07F8u;
                alpha = (unsigned)((((345 - bdist) * (int)f->elder_glow) / 67) >> 1);
            }
        } else if (sp == AQ_SP_JELLY) {
            /* Bioluminescent Moon Jellyfish: pulsing translucent dome + 5 sinusoidal glowing tentacles! */
            const int ux = (dx_raw * inv_s) >> 4;
            const int uy = (dy_raw * inv_s) >> 4;

            if (uy <= 32 && uy >= -j_ry - 24) {
                /* Pulsing dome silhouette (zero per-pixel divide via hoisted Q24 reciprocal) */
                const int norm = (int)(((uint32_t)(ux * ux) * j_inv_rx2 +
                                        (uint32_t)((uy + 16) * (uy + 16)) * j_inv_ry2) >> 16);
                const int skirt = 12 + ((aq_sin(ux / 3 + (int)(f->pulse_q8 >> 8)) * 18) >> 14);
                if (norm <= 270 && uy <= skirt) {
                    additive = true;
                    /* Translucent glass bell: bright neon rim + glowing violet/cyan inner rings */
                    const uint16_t rim_c = (seed & 1u) ? 0x07FFu : 0xD39Fu;
                    const uint16_t mid_c = (seed & 1u) ? 0x6A5Fu : 0xF8B8u;
                    if (norm >= 190 || uy >= skirt - 24) {
                        col = rim_c;
                        alpha = (norm <= 248) ? 215u : (unsigned)(((270 - norm) * 215) / 22);
                    } else {
                        /* Inner radial bioluminescent horseshoe organ rings */
                        const int ring_d = aq_abs(norm - 86);
                        if (ring_d < 26 && uy < -24) {
                            col = 0xF9FFu;
                            alpha = 180u;
                        } else {
                            col = aq_lerp565(mid_c, rim_c, (norm * 255) / 256);
                            alpha = 95u;
                        }
                    }
                } else if (elder && norm <= 335 && uy <= skirt + 20) {
                    additive = true;
                    col = 0x07FFu;
                    alpha = (unsigned)((((335 - norm) * (int)f->elder_glow) / 65) >> 1);
                }
            } else if (uy > 8 && uy <= 448) {
                /* 5 trailing sinusoidal glowing tentacles */
                static const int16_t k_tx[5] = { -144, -72, 0, 72, 144 };
                const int tph = (int)((f->pulse_q8 >> 8) & 255u);
                for (int t = 0; t < 5; ++t) {
                    const int sway = (aq_sin(tph * 2 - uy / 4 + t * 47) * (24 + uy / 8)) >> 14;
                    const int tx = k_tx[t] + sway;
                    const int d = aq_abs(ux - tx);
                    if (d <= 22) {
                        additive = true;
                        const int fade = aq_clamp(((456 - uy) * 210) / 448, 0, 210);
                        alpha = (unsigned)(((24 - d) * fade) / 24);
                        col = (t & 1) ? 0xF8B8u : 0x07FFu;
                        break;
                    }
                }
            }
        } else if (sp == AQ_SP_MANTA) {
            /* Graceful Manta Ray: sweeping undulating diamond wings + electric cyan trim + whip tail! */
            const int flap = (aq_sin((int32_t)((f->pulse_q8 >> 8) & 255u)) * 36) >> 14;
            const int ady = aq_abs(ly);
            const int wing_x_shift = (ady * flap) >> 8;
            const int mx = lx + wing_x_shift;

            /* Diamond chevron wings: widest at mx ~ 40, sweeping to wingtips at ady ~ 340 */
            int wing_half_y = 0;
            if (mx >= -240 && mx <= 235) {
                if (mx >= 24) {
                    /* Leading edge from nose (+235) back to wingtips (+24) */
                    wing_half_y = ((235 - mx) * 340) / 211;
                } else {
                    /* Trailing curved edge from wingtips (+24) to tail root (-240) */
                    wing_half_y = ((mx + 240) * 340) / 264;
                    /* Graceful concave curve behind wings */
                    wing_half_y = (wing_half_y * 7 + (wing_half_y * wing_half_y) / 340) / 8;
                }
            }
            /* Slender whip tail: lx in [-550..-225] */
            const int tail_y = ((aq_sin((int32_t)(((f->tail_q8 >> 8) + (uint32_t)(-lx >> 4)) & 255u)) *
                                 aq_max(0, -lx - 220)) >> 15);
            const bool in_whip = (lx <= -225 && lx >= -550 && aq_abs(ly - tail_y) <= 13);
            /* Twin cephalic head lobes at nose */
            const bool in_horn = (mx >= 210 && mx <= 286 && ady >= 24 && ady <= 62);

            if ((wing_half_y > 10 && ady <= wing_half_y) || in_horn) {
                const int edge_d = in_horn ? 20 : (wing_half_y - ady);
                alpha = (edge_d >= 16) ? 255u : (unsigned)((edge_d * 255) / 16);
                edge_rim = (edge_d < 24);
                /* Deep ocean slate-indigo back with pale shoulder chevrons & cyan wingtip trim */
                const int sh = aq_clamp(180 - (ady >> 1), 30, 220);
                col = aq_lerp565(0x114Bu, 0x2B95u, sh);
                if (edge_d < 22 || ady > 290) {
                    col = 0x07FFu; /* glowing electric cyan wingtip trim! */
                } else if (mx >= 55 && mx <= 135 && ady >= 44 && ady <= 118 &&
                           aq_abs((mx - 95) + (ady - 82)) < 26) {
                    col = 0x7E7Fu; /* pale chevron shoulder markings */
                }
            } else if (in_whip) {
                alpha = 220u;
                col = 0x1A73u;
            } else if (elder && wing_half_y > 0 && ady <= wing_half_y + 42) {
                additive = true;
                col = 0x07FFu;
                alpha = (unsigned)((((wing_half_y + 42 - ady) * (int)f->elder_glow) / 42) >> 1);
            }
        } else {
            /* Species 7: SEAHORSE — upright S-curved golden-amber body, snout, curled tail & fluttering fin! */
            const int ux = lx;
            const int uy = ly;

            /* Spine centerline x_spine(uy) forming iconic upright 'S' */
            int sx = 0, sw = 0;
            bool in_seahorse = false;
            if (uy >= -270 && uy <= -155) {
                /* Head + coronet crown */
                sx = 22;
                sw = 76 - aq_abs(uy + 210);
                if (sw > 14 && aq_abs(ux - sx) <= sw) {
                    in_seahorse = true;
                }
                /* Elongated tubular snout extending forward */
                if (uy >= -222 && uy <= -184 && ux >= 65 && ux <= 210) {
                    in_seahorse = true;
                }
            } else if (uy > -155 && uy <= 110) {
                /* Arched puffed chest & slender waist */
                const int t = uy + 155; /* 0..265 */
                sx = ((aq_sin((t * 145) / 265) * 62) >> 14) - 10;
                sw = 56 + ((aq_sin((t * 128) / 265) * 44) >> 14) - (t / 7);
                if (sw > 12 && aq_abs(ux - sx) <= sw) {
                    in_seahorse = true;
                }
            } else if (uy > 110 && uy <= 325) {
                /* Curled prehensile spiral tail at bottom */
                const int t = uy - 110; /* 0..215 */
                const int ang = (t * 195) / 215; /* curls forward */
                const int cx_coil = 24;
                const int cy_coil = 248;
                const int cr = 82 - (t * 24) / 215;
                const int tx = cx_coil - ((aq_cos(ang) * cr) >> 14);
                const int ty = cy_coil - ((aq_sin(ang) * cr) >> 14);
                const int td2 = (ux - tx) * (ux - tx) + (uy - ty) * (uy - ty);
                const int tw = 34 - (t / 12);
                if (td2 <= tw * tw) {
                    in_seahorse = true;
                }
                /* Direct stem connecting waist into curl */
                if (uy < 215 && aq_abs(ux - (-24 + (t / 4))) <= (32 - t / 14)) {
                    in_seahorse = true;
                }
            }

            /* Rapidly fluttering translucent dorsal fin on back */
            const int fin_w = 56 + ((pulse * 22) >> 7);
            const bool in_fin = (uy >= -85 && uy <= 48 && ux <= -44 && ux >= -44 - fin_w);

            if (in_seahorse) {
                alpha = 250u;
                /* Golden-amber to coral ribbed armor segments */
                const bool ridge = (((uy + 512) / 36) & 1) != 0;
                col = ridge ? 0xFD20u : 0xFA64u;
                if (ux > 24 && uy > -120 && uy < 80) {
                    col = 0xFEEAu; /* sunlit golden belly */
                }
                /* Glossy seahorse eye */
                const int ex = ux - 52;
                const int ey = uy + 216;
                if (ex * ex + ey * ey < 24 * 24) {
                    col = (ex * ex + ey * ey < 11 * 11) ? 0x0841u : 0xFFFFu;
                }
            } else if (in_fin) {
                alpha = 155u;
                col = 0xFFF2u;
            } else if (elder && uy >= -270 && uy <= 260 && aq_abs(ux - sx) <= sw + 34 && sw > 8) {
                additive = true;
                col = 0xFFE8u;
                alpha = (unsigned)((((sw + 34 - aq_abs(ux - sx)) * (int)f->elder_glow) / 34) >> 1);
            }
        }

        if (alpha > 0u) {
            if (additive) {
                row[x] = aq_add565(row[x], col, alpha, swap);
            } else {
                /* Abyss neon rim-light so all creatures pop radiantly in pitch-black water */
                if (abyss && edge_rim) {
                    col = aq_lerp565(col, 0x07FFu, 110);
                }
                row[x] = aq_blend565(row[x], col, alpha, swap);
            }
        }
    }
}

static void aq_render_lilies_row(uint16_t *row, int y, int lo, int hi, bool swap)
{
    if (s_aq.biome != AQ_BIOME_KOI) {
        return;
    }

    for (int i = 0; i < AQ_NUM_LILIES; ++i) {
        const aq_lily_t *l = &s_aq.lilies[i];
        const int r = (int)l->rad;
        const int dy = y - (int)l->cy;
        if (dy < -r - 2 || dy > r + 9) {
            continue;
        }
        const int x0 = aq_max(lo, (int)l->cx - r - 2);
        const int x1 = aq_min(hi, (int)l->cx + r + 8);
        const int r2 = r * r;

        for (int x = x0; x <= x1; ++x) {
            /* Soft dark drop shadow offset (+4, +6) underneath pad */
            const int sdx = x - ((int)l->cx + 4);
            const int sdy = y - ((int)l->cy + 6);
            if (sdx * sdx + sdy * sdy < r2) {
                row[x] = aq_blend565(row[x], 0x0021u, 96u, swap);
            }

            const int dx = x - (int)l->cx;
            const int d2 = dx * dx + dy * dy;
            if (d2 <= r2) {
                /* Crisp V-shaped lily pad notch cut out along (ncos, nsin) */
                const int32_t dot = dx * (int32_t)l->ncos + dy * (int32_t)l->nsin;
                const int32_t crs = aq_abs(dx * (int32_t)l->nsin - dy * (int32_t)l->ncos);
                if (!(dot > 0 && crs * 4 < dot)) {
                    /* Emerald pad radial shading + subtle veins + sunlit lime rim */
                    const int frac = (d2 * 255) / (r2 + 1);
                    uint16_t col = aq_lerp565(aq_rgb565(18, 92, 48), aq_rgb565(42, 146, 72), frac);
                    if (frac > 224) {
                        col = aq_rgb565(84, 186, 98); /* rim highlight */
                    } else if (d2 > 25 && (aq_abs(dx * 2 - dy) < 2 || aq_abs(dx + dy * 2) < 2 ||
                                           aq_abs(dx - dy) < 2)) {
                        col = aq_lerp565(col, aq_rgb565(96, 198, 104), 85); /* leaf veins */
                    }
                    row[x] = swap ? aq_bswap(col) : col;
                }
            }

            /* Blooming glowing pink lotus flower resting cleanly on pad #0! */
            if (l->has_lotus) {
                const int ldx = x - ((int)l->cx - 7);
                const int ldy = y - ((int)l->cy + 6);
                const int ld2 = ldx * ldx + ldy * ldy;
                if (ld2 < 18 * 18) {
                    if (ld2 < 5 * 5) {
                        const uint16_t stam = (ld2 < 2 * 2) ? 0xFFF2u : 0xFFE0u;
                        row[x] = swap ? aq_bswap(stam) : stam;
                    } else {
                        /* Two layered rings of pointed lotus petals (8 outer rose + 8 inner blush) */
                        const int ax = aq_abs(ldx);
                        const int ay = aq_abs(ldy);
                        const int p_orth = aq_min(ax, ay);
                        const int p_diag = aq_abs(ax - ay);
                        const int p_dist = aq_min(p_orth, (p_diag * 181) >> 8);
                        const int max_pw = 2 + (18 - (int)aq_isqrt((uint32_t)ld2)) / 2;
                        if (ld2 < 12 * 12 && p_dist <= max_pw + 1) {
                            const uint16_t ic = aq_lerp565(0xFF3Du, 0xFB38u, (ld2 * 255) / 144);
                            row[x] = aq_blend565(row[x], ic, 245u, swap);
                        } else if (p_dist <= max_pw) {
                            const uint16_t oc = aq_lerp565(0xFB9Au, 0xE8F3u, (ld2 * 255) / 324);
                            row[x] = aq_blend565(row[x], oc, 232u, swap);
                        }
                    }
                }
            }
        }
    }
}

static void aq_render_pellets_bubbles_ripples_row(uint16_t *row, int y, int lo, int hi, bool swap)
{
    /* Golden sinking food pellets */
    for (int i = 0; i < JR_AQ_MAX_PELLETS; ++i) {
        const aq_pellet_t *p = &s_aq.pellets[i];
        if (!p->active) {
            continue;
        }
        const int dy = y - (int)p->py;
        if (aq_abs(dy) > 4) {
            continue;
        }
        const int x0 = aq_max(lo, (int)p->px - 4);
        const int x1 = aq_min(hi, (int)p->px + 4);
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - (int)p->px;
            const int d2 = dx * dx + dy * dy;
            if (d2 <= 5) {
                const uint16_t pc = (dx <= 0 && dy <= 0) ? 0xFFF2u : 0xFD20u;
                row[x] = swap ? aq_bswap(pc) : pc;
            } else if (d2 <= 16) {
                row[x] = aq_add565(row[x], 0xFCA0u, (unsigned)((16 - d2) * 11), swap);
            }
        }
    }

    /* Rising glass air bubbles (translucent rim + crisp top-left white specular glint) */
    for (int i = 0; i < JR_AQ_MAX_BUBBLES; ++i) {
        const aq_bubble_t *b = &s_aq.bubbles[i];
        if (!b->active) {
            continue;
        }
        const int r = (int)b->rad;
        const int dy = y - (int)b->py;
        if (aq_abs(dy) > r) {
            continue;
        }
        const int x0 = aq_max(lo, (int)b->px - r);
        const int x1 = aq_min(hi, (int)b->px + r);
        const int r2 = r * r;
        const int ri2 = (r > 2) ? (r - 1) * (r - 1) : 1;
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - (int)b->px;
            const int d2 = dx * dx + dy * dy;
            if (d2 > r2) {
                continue;
            }
            if (dx == -(r / 2) && dy == -(r / 2)) {
                row[x] = aq_add565(row[x], 0xFFFFu, 225u, swap); /* specular glint */
            } else if (d2 >= ri2) {
                row[x] = aq_add565(row[x], 0x9F7Fu, 140u, swap); /* luminous glass rim */
            } else {
                row[x] = aq_add565(row[x], 0x4E5Fu, 42u, swap);  /* translucent glass fill */
            }
        }
    }

    /* Expanding luminous tap water ripples */
    for (int i = 0; i < JR_AQ_MAX_RIPPLES; ++i) {
        const aq_ripple_t *rp = &s_aq.ripples[i];
        if (!rp->active || rp->alpha == 0u) {
            continue;
        }
        const int r = (int)rp->r_px;
        const int dy = y - (int)rp->cy;
        const int ady = aq_abs(dy);
        if (ady > r + 3) {
            continue;
        }
        const int r_out = r + 2;
        const int r_in = r > 3 ? r - 3 : 0;
        const int dy2 = dy * dy;
        const int ro2 = r_out * r_out;
        const int ri2 = r_in * r_in;

        /* Chord-split the ring into at most 2 tight runs per row */
        const int out_half = (ro2 > dy2) ? (int)aq_isqrt((uint32_t)(ro2 - dy2)) + 1 : 0;
        const int in_half  = (ri2 > dy2) ? (int)aq_isqrt((uint32_t)(ri2 - dy2)) : -1;
        const int seg_lo[2] = { aq_max(lo, (int)rp->cx - out_half),
                                aq_max(lo, (int)rp->cx + (in_half >= 0 ? in_half : -out_half)) };
        const int seg_hi[2] = { aq_min(hi, (int)rp->cx - (in_half >= 0 ? in_half : -out_half)),
                                aq_min(hi, (int)rp->cx + out_half) };
        const int nsegs = (in_half >= 0) ? 2 : 1;
        for (int s = 0; s < nsegs; ++s) {
            const int x0 = (nsegs == 1) ? aq_max(lo, (int)rp->cx - out_half) : seg_lo[s];
            const int x1 = (nsegs == 1) ? aq_min(hi, (int)rp->cx + out_half) : seg_hi[s];
            for (int x = x0; x <= x1; ++x) {
                const int dx = x - (int)rp->cx;
                const int d2 = dx * dx + dy2;
                if (d2 >= ri2 && d2 <= ro2) {
                    const int dr = aq_abs(d2 - r * r);
                    const int span = r * 4 + 8;
                    const unsigned a = (unsigned)(((aq_max(0, span - dr)) * (int)rp->alpha) / span);
                    if (a > 0u) {
                        row[x] = aq_add565(row[x], rp->col, a, swap);
                    }
                }
            }
        }
    }
}

static void aq_render_algae_and_rim_row(uint16_t *row, int y, int lo, int hi, bool swap)
{
    const int dy = y - JR_AQ_CY;
    const int dy2 = dy * dy;

    /* Rim algae moss haze strictly inside r in [192..212] when algae > 15 */
    if (s_aq.algae > 15u) {
        const int alg_str = ((int)(s_aq.algae - 15u) * 145) / 85; /* 0..145 */
        const int r_in2 = 192 * 192;
        const int in_half = (r_in2 > dy2) ? (int)aq_isqrt((uint32_t)(r_in2 - dy2)) : -1;
        const int nsegs = (in_half >= 0) ? 2 : 1;
        for (int s = 0; s < nsegs; ++s) {
            const int x0 = (nsegs == 1) ? lo : (s == 0 ? lo : aq_max(lo, JR_AQ_CX + in_half));
            const int x1 = (nsegs == 1) ? hi : (s == 0 ? aq_min(hi, JR_AQ_CX - in_half) : hi);
            for (int x = x0; x <= x1; ++x) {
                const int dx = x - JR_AQ_CX;
                const int d2 = dx * dx + dy2;
                if (d2 < r_in2) {
                    continue;
                }
                /* Organic mottled moss pattern using cheap fast sine waves */
                const int w = 136 + ((aq_sin((x + y) * 2) * 58 + aq_cos((x - y * 2) * 3) * 44) >> 14);
                const int rad_fade = aq_clamp(((d2 - r_in2) * 255) / (212 * 212 - r_in2), 0, 255);
                const unsigned a = (unsigned)((alg_str * rad_fade * w) >> 16);
                if (a > 2u) {
                    const uint16_t mcol = (w > 150) ? aq_rgb565(42, 118, 48)
                                                    : aq_rgb565(26, 86, 36);
                    row[x] = aq_blend565(row[x], mcol, a, swap);
                }
            }
        }
    }

    /* Subtle recessed glass porthole rim bevel right at r in [210..213] */
    const int rim_in2 = 210 * 210;
    const int in_half = (rim_in2 > dy2) ? (int)aq_isqrt((uint32_t)(rim_in2 - dy2)) : -1;
    const int nsegs = (in_half >= 0) ? 2 : 1;
    for (int s = 0; s < nsegs; ++s) {
        const int x0 = (nsegs == 1) ? lo : (s == 0 ? lo : aq_max(lo, JR_AQ_CX + in_half));
        const int x1 = (nsegs == 1) ? hi : (s == 0 ? aq_min(hi, JR_AQ_CX - in_half) : hi);
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - JR_AQ_CX;
            const int d2 = dx * dx + dy2;
            if (d2 < rim_in2) {
                continue;
            }
            /* Upper-left specular glass highlight, lower-right deep bezel recess */
            const bool hi_arc = (dx + dy) < 0;
            const uint16_t bcol = hi_arc
                ? (s_aq.biome == AQ_BIOME_KOI ? aq_rgb565(92, 174, 148)
                                              : aq_rgb565(82, 198, 224))
                : aq_rgb565(8, 20, 38);
            const unsigned a = hi_arc ? 92u : 135u;
            row[x] = aq_blend565(row[x], bcol, a, swap);
        }
    }
}

/* ==========================================================================
 * PUBLIC STRIP-INVARIANT RENDERER ENTRY POINT
 * ========================================================================== */

JR_AQ_API void jr_aquarium_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes)
{
    if (dst == NULL || nrows <= 0) {
        return;
    }
    if (!s_aq.initialized) {
        jr_display_aq_state_t def = {
            .active = true, .biome = 0u, .coral_level = 2u, .algae = 0u,
            .hour = 13u, .fish_count = 0u,
        };
        jr_aquarium_step(0u, 0, 0, &def);
    }

    const int y_end = aq_min(y0 + nrows, JR_AQ_H);
    const int y_start = aq_max(y0, 0);

    for (int y = y_start; y < y_end; ++y) {
        int lo = 0, hi = JR_AQ_W - 1;
        if (!aq_clip_row(y, &lo, &hi)) {
            continue;
        }
        uint16_t *row = dst + (size_t)(y - y0) * (size_t)JR_AQ_W;

        /* 1. Biome water/void backdrop + meniscus + volumetric sun caustics */
        aq_render_bg_row(row, y, lo, hi, swap_bytes);

        /* 2. Seabed, coral formations (Lv 1..5), glowing polyps, kelp / Koi wakes */
        aq_render_seabed_and_corals_row(row, y, lo, hi, swap_bytes);
        aq_render_kelp_row(row, y, lo, hi, swap_bytes);
        aq_render_wakes_row(row, y, lo, hi, swap_bytes);

        /* 3. Bioluminescent drifting plankton motes (Abyss & Reef Night) */
        aq_render_motes_row(row, y, lo, hi, swap_bytes);

        /* 4. Up to 8 articulated procedural fish (bounding-box culled per row) */
        for (int i = 0; i < (int)s_aq.fish_count; ++i) {
            aq_render_fish_row(row, y, lo, hi, &s_aq.fish[i], swap_bytes);
        }

        /* 5. Floating lotus lily pads & pink lotus flower OVER Koi fish */
        aq_render_lilies_row(row, y, lo, hi, swap_bytes);

        /* 6. Golden food pellets, rising air bubbles & expanding tap ripples */
        aq_render_pellets_bubbles_ripples_row(row, y, lo, hi, swap_bytes);

        /* 7. Rim algae haze (algae > 15) & subtle recessed glass porthole rim */
        aq_render_algae_and_rim_row(row, y, lo, hi, swap_bytes);
    }
}
