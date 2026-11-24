/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass3_ring.c — JarvisNano Glass 3.0 Procedural OLED Ring & Voice-Wave Engine.
 *
 * Pure C11, integer-only, zero-libm, zero-heap, stack < 512 B, 100% strip-invariant.
 *   1. CHRONO-ARC  (jr_g3_chrono_arc_rows)  — Home perimeter bezel + 4 sculpted
 *      telemetry capsules + whisper-soft concentric sonar ripples.
 *   2. AETHERIUM   (jr_g3_aetherium_rows)   — 120 radial FFT spectrum filaments,
 *      3 iridescent additive harmonic wave ribbons, millimeter bezel + 4 cardinal
 *      jewels, and lower-center Crystalline Vocal Waveform Ribbon.
 *   3. BEZEL SCRUB (jr_g3_bezel_scrub_rows) — 270-deg luminous rotary dial arc,
 *      41-tick precision inner scale, and faceted glowing jewel indicator.
 */
#include "glass3_ring.h"

#include <string.h>

#include "jr_display/glass.h"

#define G3_W      HUD_W
#define G3_H      HUD_H
#define G3_CX16   (233 * 16) /* pixel x center is 16*x + 8 -> 232.5 px = 3728 */
#define G3_CY16   (233 * 16)
#define G3_CX2    465        /* 2 * 232.5 */
#define G3_CY2    465

#define G3_RIBBON_W (JR_G3_AETH_RIBBON_X1 - JR_G3_AETH_RIBBON_X0 + 1) /* 275 px */
#define G3_RIBBON_CY16 (310 * 16 + 8) /* vertical center of [286, 334] */

/* ---- .rodata LUTs (total: 130 bytes) ------------------------------------- */

/* Quarter-quadrant atan(i / 64) in Q16 turns (0..8192 = 0..45 deg).
 * Linear interpolation gives < 0.0012 deg (< 0.005 px at r=231) max error.
 * Size: 65 * 2 = 130 bytes in .rodata. */
static const uint16_t k_g3_atan_q16[65] = {
        0,   163,   326,   489,   651,   813,   975,  1136,
     1297,  1457,  1617,  1775,  1933,  2090,  2246,  2401,
     2555,  2708,  2860,  3010,  3159,  3307,  3453,  3599,
     3742,  3884,  4025,  4164,  4302,  4438,  4572,  4705,
     4836,  4966,  5094,  5220,  5344,  5467,  5589,  5708,
     5826,  5943,  6058,  6171,  6282,  6392,  6500,  6607,
     6712,  6815,  6917,  7018,  7117,  7214,  7310,  7405,
     7498,  7589,  7679,  7768,  7856,  7942,  8026,  8110,
     8192,
};

/* ---- fast integer primitives --------------------------------------------- */

static inline int g3_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Overflow-proof quadratic falloff alpha: (t/255)^2 * peak_a * (scale_a/255). */
static inline unsigned g3_quad_alpha(int t255, unsigned peak_a, unsigned scale_a)
{
    const uint32_t t = (uint32_t)g3_clamp(t255, 0, 255);
    const uint32_t q = (t * t) >> 8; /* 0..254 */
    return (unsigned)((q * peak_a * scale_a) >> 16);
}

static inline int32_t g3_abs32(int32_t v)
{
    return v < 0 ? -v : v;
}

static uint32_t g3_isqrt(uint32_t n)
{
    if (n == 0u) {
        return 0u;
    }
    uint32_t r = 0u;
    uint32_t bit = 1u << ((31u - (uint32_t)__builtin_clz(n)) & ~1u);
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

/* Saturating additive OLED light blend (matches showpiece_gfx.c sp_add565).
 * Adds native RGB565 `over` scaled by `a/255` onto panel-order `under`. */
static inline uint16_t g3_add565(uint16_t under, uint16_t over, unsigned a,
                                 bool swap_bytes)
{
    if (a == 0u) {
        return under;
    }
    if (a > 255u) {
        a = 255u;
    }
    const uint32_t u = swap_bytes ? glass_bswap(under) : under;
    const uint32_t ur = (u >> 11) & 31u;
    const uint32_t ug = (u >> 5) & 63u;
    const uint32_t ub = u & 31u;
    const uint32_t or_ = (over >> 11) & 31u;
    const uint32_t og = (over >> 5) & 63u;
    const uint32_t ob = over & 31u;
    uint32_t r = ur + ((or_ * a + 127u) >> 8);
    uint32_t g = ug + ((og * a + 127u) >> 8);
    uint32_t b = ub + ((ob * a + 127u) >> 8);
    if (r > 31u) { r = 31u; }
    if (g > 63u) { g = 63u; }
    if (b > 31u) { b = 31u; }
    const uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return swap_bytes ? glass_bswap(out) : out;
}

/* Linear interpolation of two native RGB565 colors by t in 0..255. */
static inline uint16_t g3_lerp565(uint16_t c0, uint16_t c1, int t)
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

/* Exact Q16 screen angle (0=3 o'clock, 16384=6, 32768=9, 49152=12, clockwise)
 * from signed offsets (dy, dx) with |dx|, |dy| <= 65535. */
static uint32_t g3_atan2_q16(int32_t dy, int32_t dx)
{
    const uint32_t ax = (uint32_t)g3_abs32(dx);
    const uint32_t ay = (uint32_t)g3_abs32(dy);
    if (ax == 0u && ay == 0u) {
        return 0u;
    }
    const bool x_dom = (ax >= ay);
    const uint32_t mn = x_dom ? ay : ax;
    const uint32_t mx = x_dom ? ax : ay;
    const uint32_t u = (mn << 14) / mx; /* 0..16384 */
    const uint32_t idx = u >> 8;        /* 0..64 */
    const uint32_t frac = u & 0xFFu;
    uint32_t a = k_g3_atan_q16[idx < 64u ? idx : 64u];
    if (idx < 64u) {
        a += ((uint32_t)(k_g3_atan_q16[idx + 1u] - k_g3_atan_q16[idx]) * frac) >> 8;
    }
    if (x_dom) {
        if (dx >= 0) {
            return dy >= 0 ? a : ((65536u - a) & 0xFFFFu);
        }
        return dy >= 0 ? (32768u - a) : (32768u + a);
    }
    if (dy >= 0) {
        return dx >= 0 ? (16384u - a) : (16384u + a);
    }
    return dx >= 0 ? (49152u + a) : (49152u - a);
}

/* Compute up to 2 x-spans [lo, hi] covering the annulus [r_in, r_out] on row y.
 * Returns number of spans (0, 1, or 2). */
typedef struct {
    int lo, hi;
} g3_xspan_t;

static int g3_annulus_row_spans(int y, int r_in, int r_out, g3_xspan_t out[2])
{
    const int dy2 = 2 * y - G3_CY2;
    const int dy2_sq = dy2 * dy2;
    const int ro2 = 2 * r_out;
    const int ro2_sq = ro2 * ro2;
    if (dy2_sq > ro2_sq) {
        return 0;
    }
    const int ho = (int)g3_isqrt((uint32_t)(ro2_sq - dy2_sq));
    int x0 = (G3_CX2 - ho) / 2 - 1;
    int x1 = (G3_CX2 + ho) / 2 + 1;
    if (x0 < 0) { x0 = 0; }
    if (x1 >= G3_W) { x1 = G3_W - 1; }
    if (x0 > x1) {
        return 0;
    }

    const int ri2 = 2 * r_in;
    const int ri2_sq = ri2 * ri2;
    if (r_in <= 0 || dy2_sq >= ri2_sq) {
        out[0].lo = x0;
        out[0].hi = x1;
        return 1;
    }
    const int hi = (int)g3_isqrt((uint32_t)(ri2_sq - dy2_sq));
    int xl_hi = (G3_CX2 - hi) / 2 + 1;
    int xr_lo = (G3_CX2 + hi) / 2 - 1;
    if (xl_hi >= xr_lo - 1) {
        out[0].lo = x0;
        out[0].hi = x1;
        return 1;
    }
    int n = 0;
    if (x0 <= xl_hi) {
        out[n].lo = x0;
        out[n].hi = xl_hi < x1 ? xl_hi : x1;
        n++;
    }
    if (xr_lo <= x1) {
        out[n].lo = xr_lo > x0 ? xr_lo : x0;
        out[n].hi = x1;
        n++;
    }
    return n;
}

/* Soft quadratic-falloff additive OLED glow orb clipped strictly to rows
 * [ry_lo, ry_hi] and annulus r in [clip_r_min, clip_r_max]. */
static void g3_glow_orb_annulus(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                int32_t cx16, int32_t cy16, int r_px,
                                uint16_t color, uint8_t peak_a,
                                int clip_r_min, int clip_r_max)
{
    if (r_px <= 0 || peak_a == 0u) {
        return;
    }
    const int cx_i = (int)(cx16 >> 4);
    const int cy_i = (int)(cy16 >> 4);
    int ry0 = cy_i - r_px - 1;
    int ry1 = cy_i + r_px + 1;
    if (ry0 < y0) { ry0 = y0; }
    if (ry0 < 0)  { ry0 = 0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry1 >= G3_H) { ry1 = G3_H - 1; }
    if (ry0 > ry1) {
        return;
    }
    int rx0 = cx_i - r_px - 1;
    int rx1 = cx_i + r_px + 1;
    if (rx0 < 0) { rx0 = 0; }
    if (rx1 >= G3_W) { rx1 = G3_W - 1; }

    const int32_t r16 = r_px * 16;
    const int32_t r16_sq = r16 * r16;
    const int rmin2_sq = (2 * clip_r_min) * (2 * clip_r_min);
    const int rmax2_sq = (2 * clip_r_max) * (2 * clip_r_max);

    for (int y = ry0; y <= ry1; ++y) {
        const int dy2 = 2 * y - G3_CY2;
        const int dy2_sq = dy2 * dy2;
        if (dy2_sq > rmax2_sq) {
            continue;
        }
        const int32_t py16 = y * 16 + 8 - cy16;
        const int32_t py_sq = py16 * py16;
        if (py_sq >= r16_sq) {
            continue;
        }
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;
        for (int x = rx0; x <= rx1; ++x) {
            const int dx2 = 2 * x - G3_CX2;
            const int r2_sq = dx2 * dx2 + dy2_sq;
            if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                continue;
            }
            const int32_t px16 = x * 16 + 8 - cx16;
            const int32_t d2 = px16 * px16 + py_sq;
            if (d2 >= r16_sq) {
                continue;
            }
            const uint32_t t = (uint32_t)(((int64_t)(r16_sq - d2) * 255) / r16_sq);
            const unsigned a = (unsigned)((t * t * peak_a) >> 16);
            if (a > 0u) {
                row[x] = g3_add565(row[x], color, a, swap_bytes);
            }
        }
    }
}

/* ---- Curved Rounded-Endcap Capsule SDF Helper ---------------------------- */

typedef struct {
    uint16_t a0;           /* track start angle (Q16 clockwise) */
    uint16_t sweep;        /* track angular sweep (Q16) */
    int16_t  cap0_x16, cap0_y16;
    int16_t  cap1_x16, cap1_y16;

    uint16_t fill_a0;      /* live fill start angle (Q16) */
    uint16_t fill_sweep;   /* live fill sweep (0 = unlit track only) */
    int16_t  fcap0_x16, fcap0_y16;
    int16_t  fcap1_x16, fcap1_y16;
    int16_t  head_x16, head_y16; /* moving tip center for jewel flare */

    uint16_t col_deep;     /* outer/edge fill tint */
    uint16_t col_core;     /* main luminous fill body */
    uint16_t col_ridge;    /* specular upper-inner glass highlight */
    uint16_t col_glow;     /* outer OLED bloom color */
    uint8_t  fill_a;       /* 0..255 fill intensity */
    uint8_t  glow_a;       /* 0..255 outer OLED bloom peak */
    uint8_t  dividers;     /* 0 = continuous, 4 = 4-bar Wi-Fi notch dividers */
    uint8_t  shimmer_pos;  /* 0 = none, 1..255 traveling sheen position */
    uint8_t  has_fill;     /* non-zero when fill bar should render */
} g3_capsule_t;

static void g3_capsule_compute_caps16(int32_t r_mid16, uint32_t a0, uint32_t sweep,
                                      int16_t *x0, int16_t *y0,
                                      int16_t *x1, int16_t *y1)
{
    const uint32_t a1 = (a0 + sweep) & 0xFFFFu;
    *x0 = (int16_t)(G3_CX16 + (int32_t)(((int64_t)r_mid16 * glass_cos_q15(a0)) >> 15));
    *y0 = (int16_t)(G3_CY16 + (int32_t)(((int64_t)r_mid16 * glass_sin_q15(a0)) >> 15));
    *x1 = (int16_t)(G3_CX16 + (int32_t)(((int64_t)r_mid16 * glass_cos_q15(a1)) >> 15));
    *y1 = (int16_t)(G3_CY16 + (int32_t)(((int64_t)r_mid16 * glass_sin_q15(a1)) >> 15));
}

static void g3_capsule_compute_caps(int32_t r_mid16, uint32_t a0, uint32_t sweep,
                                    int32_t *x0, int32_t *y0,
                                    int32_t *x1, int32_t *y1)
{
    const uint32_t a1 = (a0 + sweep) & 0xFFFFu;
    *x0 = G3_CX16 + (int32_t)(((int64_t)r_mid16 * glass_cos_q15(a0)) >> 15);
    *y0 = G3_CY16 + (int32_t)(((int64_t)r_mid16 * glass_sin_q15(a0)) >> 15);
    *x1 = G3_CX16 + (int32_t)(((int64_t)r_mid16 * glass_cos_q15(a1)) >> 15);
    *y1 = G3_CY16 + (int32_t)(((int64_t)r_mid16 * glass_sin_q15(a1)) >> 15);
}

/* Exact Euclidean distance in Q4 px from pixel (px16, py16) at polar (r16, ang)
 * to a circular arc spine [a0, a0 + sweep] at radius r_mid16 with semicircular
 * round endcaps at (c0x, c0y) and (c1x, c1y). Returns >= 4096 when far away. */
static int32_t g3_arc_spine_dist16(int32_t px16, int32_t py16,
                                   int32_t r16, uint32_t ang,
                                   int32_t r_mid16, uint32_t a0, uint32_t sweep,
                                   int32_t c0x, int32_t c0y,
                                   int32_t c1x, int32_t c1y)
{
    const uint32_t rel = (ang - a0) & 0xFFFFu;
    if (rel <= sweep) {
        return g3_abs32(r16 - r_mid16);
    }
    const uint32_t d_end = rel - sweep;
    const uint32_t d_start = 65536u - rel;
    const uint32_t d_min = d_end < d_start ? d_end : d_start;
    if (d_min > 2400u) {
        return 4096;
    }
    const int32_t ex = d_end < d_start ? c1x : c0x;
    const int32_t ey = d_end < d_start ? c1y : c0y;
    const int32_t dx = px16 - ex;
    const int32_t dy = py16 - ey;
    const int32_t d2 = dx * dx + dy * dy;
    if (d2 >= 4096 * 4096) {
        return 4096;
    }
    return (int32_t)g3_isqrt((uint32_t)d2);
}

/* ==========================================================================
 * OPTION 1: CHRONO-ARC (Home Perimeter Micrometer Bezel + 4 Sculpted Capsules
 *                       + Whisper-Soft Concentric Sonar Ripples)
 * ========================================================================== */

/* Clock-degree helper: 0 deg = 12 o'clock, increasing clockwise, Q16 turns. */
static inline uint32_t g3_clk_deg_q16(int deg_tenths)
{
    /* deg_tenths in [-3600, 3600]: 3600 = 360.0 deg = 65536 Q16 turns */
    int32_t q = (deg_tenths * 65536) / 3600;
    return (uint32_t)(GLASS_Q16_12_OCLOCK + q) & 0xFFFFu;
}

static void g3_chrono_sonar_ripples(uint16_t *dst, int y0, int y1,
                                    bool swap_bytes, uint32_t now_ms,
                                    uint16_t ripple_col, int strength)
{
    const int rmin2_sq = (2 * JR_G3_CHRONO_RIPPLE_R_MIN) * (2 * JR_G3_CHRONO_RIPPLE_R_MIN);
    const int rmax2_sq = (2 * JR_G3_CHRONO_RIPPLE_R_MAX) * (2 * JR_G3_CHRONO_RIPPLE_R_MAX);

    /* Two expanding sonar waves + one stationary hairline calibration ring */
    int32_t ring_r16[3];
    int     ring_a[3];
    int32_t ring_hw[3];

    for (int k = 0; k < 2; ++k) {
        const uint32_t phase = (now_ms + (uint32_t)k * 2400u) % 4800u;
        /* Radius sweeps smoothly from r=108 to r=166 */
        ring_r16[k] = 108 * 16 + (int32_t)((phase * (58u * 16u)) / 4800u);
        /* Half-sine bell envelope peaked at 22/255 (strictly in 10..24/255) */
        const uint32_t env_ang = (phase * 32768u) / 4800u;
        const int env = (glass_sin_q15(env_ang) * 22) >> 15;
        ring_a[k] = g3_clamp(env, 0, 22);
        ring_hw[k] = 28; /* 1.75 px soft sonar wavefront */
    }
    /* Subtle stationary horological calibration hairline at r=138 (alpha 11) */
    ring_r16[2] = 138 * 16;
    ring_a[2] = 11;
    ring_hw[2] = 14;

    int ry0 = 232 - JR_G3_CHRONO_RIPPLE_R_MAX;
    int ry1 = 233 + JR_G3_CHRONO_RIPPLE_R_MAX;
    if (ry0 < y0) { ry0 = y0; }
    if (ry0 < 0)  { ry0 = 0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry1 >= G3_H) { ry1 = G3_H - 1; }
    if (ry0 > ry1) {
        return;
    }

    for (int y = ry0; y <= ry1; ++y) {
        const int dy2 = 2 * y - G3_CY2;
        const int dy2_sq = dy2 * dy2;
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;

        for (int k = 0; k < 3; ++k) {
            if (ring_a[k] <= 0) {
                continue;
            }
            const int r_c = (int)(ring_r16[k] >> 4);
            const int band_in = g3_clamp(r_c - 3, JR_G3_CHRONO_RIPPLE_R_MIN, JR_G3_CHRONO_RIPPLE_R_MAX);
            const int band_out = g3_clamp(r_c + 3, JR_G3_CHRONO_RIPPLE_R_MIN, JR_G3_CHRONO_RIPPLE_R_MAX);
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, band_in, band_out, spans);
            if (nsp == 0) {
                continue;
            }
            const int32_t rc16 = ring_r16[k];
            const int32_t hw16 = ring_hw[k];
            const int peak_a = (ring_a[k] * strength) >> 8;
            if (peak_a <= 0) {
                continue;
            }
            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t r16 = (int32_t)g3_isqrt((uint32_t)r2_sq << 6);
                    const int32_t dr = g3_abs32(r16 - rc16);
                    if (dr >= hw16) {
                        continue;
                    }
                    /* If stationary hairline is overlapped by an active wave, let the wave own the pixel */
                    if (k == 2 && (g3_abs32(r16 - ring_r16[0]) < ring_hw[0] ||
                                   g3_abs32(r16 - ring_r16[1]) < ring_hw[1])) {
                        continue;
                    }
                    const int cov = ((hw16 - dr) * 255) / hw16;
                    unsigned a = (unsigned)((cov * peak_a) >> 8);
                    if (a > 24u) {
                        a = 24u;
                    }
                    if (a > 0u) {
                        row[x] = g3_add565(row[x], ripple_col, a, swap_bytes);
                    }
                }
            }
        }
    }
}

void jr_g3_chrono_arc_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                           uint32_t now_ms, const jr_g3_chrono_env_t *env,
                           int strength)
{
    if (dst == NULL || nrows <= 0 || strength <= 0) {
        return;
    }
    if (strength > 255) {
        strength = 255;
    }
    const int y1 = y0 + nrows;
    if (y1 <= 0 || y0 >= G3_H) {
        return;
    }

    jr_g3_chrono_env_t def_env;
    if (env == NULL) {
        memset(&def_env, 0, sizeof(def_env));
        def_env.batt_pct = 0xFFu;
        env = &def_env;
    }

    const int breath_q15 = glass_sin_q15((now_ms * 18u) & 0xFFFFu); /* -32767..32767 */
    const int breath_01 = (breath_q15 + 32768) >> 8; /* 0..255 */

    /* ---- 1. Whisper-soft concentric sonar ripples strictly in r in [106, 168] */
    {
        uint16_t rip_col = 0x04BFu; /* sapphire cyan */
        if (env->muted) {
            rip_col = 0xC4C2u;      /* warm muted gold */
        } else if (env->codex_active) {
            rip_col = 0x725Fu;      /* electric violet */
        }
        g3_chrono_sonar_ripples(dst, y0, y1, swap_bytes, now_ms, rip_col, strength);
    }

    /* ---- 2. 360-deg precision micrometer tick bezel strictly in r in [186, 207] */
    {
        const int rmin2_sq = (2 * JR_G3_CHRONO_BEZEL_R_MIN) * (2 * JR_G3_CHRONO_BEZEL_R_MIN);
        const int rmax2_sq = (2 * 207) * (2 * 207);
        int ry0 = 232 - 207;
        int ry1 = 233 + 207;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        const uint16_t col_vernier_core = env->muted ? 0xFFE8u : 0xFFFFu;
        const uint16_t col_vernier_body = env->muted ? 0xFCA2u
                                        : (env->codex_active ? 0xB3FFu : 0x47FFu);
        const uint16_t col_hour_inner   = env->muted ? 0xA3A1u
                                        : (env->codex_active ? 0x621Du : 0x045Bu);
        const uint16_t col_hour_outer   = env->muted ? 0xFDE5u
                                        : (env->codex_active ? 0xB39Fu : 0x3EFFu);

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, JR_G3_CHRONO_BEZEL_R_MIN, 207, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t dy16 = dy2 * 8;
            /* Subtle brushed-metal top-light sheen: 185..255 from bottom to top */
            const int sheen = 220 - ((dy2 * 35) / 466);
            const uint16_t col_gunmetal = g3_lerp565(0x31C8u, 0x6B6Eu, g3_clamp((sheen - 170) * 3, 0, 255));
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t r16 = (int32_t)g3_isqrt((uint32_t)r2_sq << 6);
                    uint16_t pix = row[x];

                    /* Subtle inner & outer precision framing hairlines */
                    const int32_t d_in = g3_abs32(r16 - 188 * 16);
                    if (d_in < 12) {
                        const unsigned ha = (unsigned)((((12 - d_in) * 52) / 12) * strength >> 8);
                        pix = g3_add565(pix, 0x2189u, ha, swap_bytes);
                    }
                    const int32_t d_out = g3_abs32(r16 - 206 * 16);
                    if (d_out < 12) {
                        const unsigned ha = (unsigned)((((12 - d_out) * 58) / 12) * strength >> 8);
                        pix = g3_add565(pix, 0x29AAu, ha, swap_bytes);
                    }

                    /* 180 micrometer ticks (2-deg spacing) */
                    if (r16 >= 188 * 16 && r16 <= 206 * 16) {
                        const uint32_t ang = g3_atan2_q16(dy16, dx2 * 8);
                        const uint32_t ang12 = (ang - GLASS_Q16_12_OCLOCK) & 0xFFFFu;
                        const uint32_t prod = ang12 * 180u;
                        const int k = (int)(((prod + 32768u) >> 16) % 180u);
                        const int32_t frac = (int32_t)((prod + 32768u) & 0xFFFFu) - 32768;
                        const int32_t tan_q4 = (int32_t)(((int64_t)g3_abs32(frac) * r16 * 143) >> 28);

                        if ((k % 45) == 0) {
                            /* 4 Bright Cyan Vernier Ticks at 12/3/6/9 o'clock */
                            const int32_t r_lo = 189 * 16;
                            const int32_t r_hi = 205 * 16;
                            if (r16 >= r_lo - 10 && r16 <= r_hi + 10) {
                                /* Soft vernier glow */
                                if (tan_q4 < 56) {
                                    const int gt = ((56 - tan_q4) * 255) / 56;
                                    const unsigned ga = g3_quad_alpha(gt, 92u, (unsigned)strength);
                                    pix = g3_add565(pix, col_vernier_body, ga, swap_bytes);
                                }
                                /* Crisp vernier blade */
                                if (tan_q4 < 22) {
                                    int cov = ((22 - tan_q4) * 255) / 12;
                                    if (cov > 255) { cov = 255; }
                                    if (r16 < r_lo) {
                                        cov = (cov * (r16 - (r_lo - 10))) / 10;
                                    } else if (r16 > r_hi) {
                                        cov = (cov * ((r_hi + 10) - r16)) / 10;
                                    }
                                    if (cov > 0) {
                                        const uint16_t col = tan_q4 < 8 ? col_vernier_core : col_vernier_body;
                                        const unsigned a = (unsigned)((cov * strength) >> 8);
                                        pix = glass_blend565(pix, col, a, swap_bytes);
                                    }
                                }
                            }
                        } else if ((k % 15) == 0) {
                            /* 12 Sapphire-Cyan Major Hour Ticks */
                            const int32_t r_lo = 190 * 16;
                            const int32_t r_hi = 204 * 16;
                            if (r16 >= r_lo - 8 && r16 <= r_hi + 8 && tan_q4 < 28) {
                                if (tan_q4 < 28 && r16 >= r_lo && r16 <= r_hi) {
                                    const unsigned ga = (unsigned)((((28 - tan_q4) * 42) / 28) * strength >> 8);
                                    pix = g3_add565(pix, col_hour_outer, ga, swap_bytes);
                                }
                                if (tan_q4 < 17) {
                                    int cov = ((17 - tan_q4) * 255) / 10;
                                    if (cov > 255) { cov = 255; }
                                    if (r16 < r_lo) {
                                        cov = (cov * (r16 - (r_lo - 8))) >> 3;
                                    } else if (r16 > r_hi) {
                                        cov = (cov * ((r_hi + 8) - r16)) >> 3;
                                    }
                                    if (cov > 0) {
                                        const int rt = g3_clamp(((r16 - r_lo) * 255) / (r_hi - r_lo), 0, 255);
                                        const uint16_t col = g3_lerp565(col_hour_inner, col_hour_outer, rt);
                                        const unsigned a = (unsigned)((cov * 235 * strength) >> 16);
                                        pix = glass_blend565(pix, col, a, swap_bytes);
                                    }
                                }
                            }
                        } else {
                            /* 180 Crisp Brushed Gunmetal 2-deg Micrometer Ticks */
                            const bool mid_tick = ((k % 5) == 0);
                            const int32_t r_lo = (mid_tick ? 193 : 194) * 16;
                            const int32_t r_hi = 202 * 16;
                            if (r16 >= r_lo - 6 && r16 <= r_hi + 6 && tan_q4 < 11) {
                                int cov = ((11 - tan_q4) * 255) / 8;
                                if (cov > 255) { cov = 255; }
                                if (r16 < r_lo) {
                                    cov = (cov * (r16 - (r_lo - 6))) / 6;
                                } else if (r16 > r_hi) {
                                    cov = (cov * ((r_hi + 6) - r16)) / 6;
                                }
                                if (cov > 0) {
                                    const unsigned base_a = mid_tick ? 185u : 138u;
                                    const unsigned a = (unsigned)(((unsigned)cov * base_a * (unsigned)strength) >> 16);
                                    pix = glass_blend565(pix, col_gunmetal, a, swap_bytes);
                                }
                            }
                        }
                    }

                    row[x] = pix;
                }
            }
        }
    }

    /* ---- 3. 4 Sculpted Luminous Curved Status Capsules (r in [212, 226],
     *         outer glow clipped strictly inside r in [208, 231]) ----------- */
    {
        const int32_t cap_r_mid16 = 219 * 16;
        const int32_t track_hw16  = 7 * 16;  /* r in [212, 226] */
        const int32_t fill_hw16   = 78;      /* ~4.87 px inset luminous core */

        g3_capsule_t caps[4];
        memset(caps, 0, sizeof(caps));

        /* Capsule 0: Top (12 o'clock, -25..+25 deg -> caps strictly inside -30..+30 deg) */
        {
            g3_capsule_t *c = &caps[0];
            c->a0 = (uint16_t)g3_clk_deg_q16(-250);
            c->sweep = (uint16_t)((500 * 65536) / 3600);
            g3_capsule_compute_caps16(cap_r_mid16, c->a0, c->sweep,
                                      &c->cap0_x16, &c->cap0_y16,
                                      &c->cap1_x16, &c->cap1_y16);
            c->has_fill = 1u;
            if (env->muted) {
                /* Warm luxury gold centered privacy bar */
                const uint32_t inset = (uint32_t)c->sweep / 5u;
                c->fill_a0 = (uint16_t)((c->a0 + inset) & 0xFFFFu);
                c->fill_sweep = (uint16_t)(c->sweep - 2u * inset);
                c->col_deep  = 0x9280u;
                c->col_core  = 0xFCA2u;
                c->col_ridge = 0xFFF0u;
                c->col_glow  = 0xD3E0u;
                c->fill_a    = (uint8_t)(210 + (breath_01 >> 3));
                c->glow_a    = (uint8_t)(95 + (breath_01 >> 3));
            } else if (env->codex_active) {
                /* Electric violet breathing pulse + traveling energy sheen */
                c->fill_a0 = c->a0;
                c->fill_sweep = c->sweep;
                c->col_deep  = 0x6119u;
                c->col_core  = 0xB23Fu;
                c->col_ridge = 0xF5FFu;
                c->col_glow  = 0x91DFu;
                c->fill_a    = (uint8_t)(205 + ((breath_01 * 50) >> 8));
                c->glow_a    = (uint8_t)(115 + ((breath_01 * 80) >> 8));
                c->shimmer_pos = (uint8_t)(((now_ms >> 3) % 254u) + 1u);
            } else if (env->voice_busy) {
                /* Vivid cyan-aqua active pulse */
                c->fill_a0 = c->a0;
                c->fill_sweep = c->sweep;
                c->col_deep  = 0x03BCu;
                c->col_core  = 0x07FFu;
                c->col_ridge = 0xEFFFu;
                c->col_glow  = 0x06FFu;
                c->fill_a    = (uint8_t)(215 + ((breath_01 * 40) >> 8));
                c->glow_a    = (uint8_t)(120 + ((breath_01 * 85) >> 8));
                c->shimmer_pos = (uint8_t)(((now_ms >> 3) % 254u) + 1u);
            } else {
                /* Sapphire cyan ready state with calm refined breath */
                c->fill_a0 = c->a0;
                c->fill_sweep = c->sweep;
                c->col_deep  = 0x0276u;
                c->col_core  = 0x05BFu;
                c->col_ridge = 0x7FFFu;
                c->col_glow  = 0x04DFu;
                c->fill_a    = (uint8_t)(195 + ((breath_01 * 36) >> 8));
                c->glow_a    = (uint8_t)(78 + ((breath_01 * 38) >> 8));
            }
            g3_capsule_compute_caps16(cap_r_mid16, c->fill_a0, c->fill_sweep,
                                      &c->fcap0_x16, &c->fcap0_y16,
                                      &c->fcap1_x16, &c->fcap1_y16);
            c->head_x16 = c->fcap1_x16;
            c->head_y16 = c->fcap1_y16;
        }

        /* Capsule 1: Right (1:30..4:30) — Battery capsule, fills upward from 4:30 -> 1:30 */
        {
            g3_capsule_t *c = &caps[1];
            c->a0 = (uint16_t)g3_clk_deg_q16(450); /* 1:30 */
            c->sweep = 16384u;                    /* 90 deg to 4:30 */
            g3_capsule_compute_caps16(cap_r_mid16, c->a0, c->sweep,
                                      &c->cap0_x16, &c->cap0_y16,
                                      &c->cap1_x16, &c->cap1_y16);
            if (env->batt_pct != 0xFFu) {
                const uint32_t pct = env->batt_pct > 100u ? 100u : env->batt_pct;
                if (pct > 0u) {
                    c->has_fill = 1u;
                    uint32_t fs = ((uint32_t)c->sweep * pct) / 100u;
                    if (fs < 1u) { fs = 1u; }
                    c->fill_sweep = (uint16_t)fs;
                    /* Anchor fill at bottom (4:30) rising toward top (1:30) */
                    c->fill_a0 = (uint16_t)(((uint32_t)c->a0 + c->sweep - fs) & 0xFFFFu);
                    g3_capsule_compute_caps16(cap_r_mid16, c->fill_a0, c->fill_sweep,
                                              &c->fcap0_x16, &c->fcap0_y16,
                                              &c->fcap1_x16, &c->fcap1_y16);
                    /* Rising head is at fill_a0 (top of fill) */
                    c->head_x16 = c->fcap0_x16;
                    c->head_y16 = c->fcap0_y16;

                    if (env->charging) {
                        c->col_deep  = 0x03A4u;
                        c->col_core  = 0x06E9u;
                        c->col_ridge = 0xB7F8u;
                        c->col_glow  = 0x05E7u;
                        c->fill_a    = (uint8_t)(215 + ((breath_01 * 40) >> 8));
                        c->glow_a    = (uint8_t)(105 + ((breath_01 * 65) >> 8));
                        c->shimmer_pos = (uint8_t)(255u - (((now_ms >> 3) % 254u) + 1u));
                    } else if (pct < 20u) {
                        c->col_deep  = 0xA260u;
                        c->col_core  = 0xFC60u;
                        c->col_ridge = 0xFFE8u;
                        c->col_glow  = 0xEA60u;
                        c->fill_a    = (uint8_t)(200 + ((breath_01 * 55) >> 8));
                        c->glow_a    = (uint8_t)(105 + ((breath_01 * 75) >> 8));
                    } else {
                        c->col_deep  = 0x0338u;
                        c->col_core  = 0x06FFu;
                        c->col_ridge = 0xBFFFu;
                        c->col_glow  = 0x05DFu;
                        c->fill_a    = 238u;
                        c->glow_a    = 98u;
                    }
                }
            }
        }

        /* Capsule 2: Bottom-Left (6:30..9:00) — Acoustic / Mic capsule */
        {
            g3_capsule_t *c = &caps[2];
            c->a0 = (uint16_t)g3_clk_deg_q16(1950); /* 6:30 */
            c->sweep = (uint16_t)((750 * 65536) / 3600); /* 75 deg to 9:00 */
            g3_capsule_compute_caps16(cap_r_mid16, c->a0, c->sweep,
                                      &c->cap0_x16, &c->cap0_y16,
                                      &c->cap1_x16, &c->cap1_y16);
            if (!env->muted) {
                /* Live acoustic ember floor ~14% rising smoothly with amp to 100% */
                const uint32_t lvl = 36u + (((uint32_t)env->amp * 219u) >> 8);
                c->has_fill = 1u;
                c->fill_a0 = c->a0;
                uint32_t fs = ((uint32_t)c->sweep * lvl) / 255u;
                if (fs < 1u) { fs = 1u; }
                c->fill_sweep = (uint16_t)fs;
                g3_capsule_compute_caps16(cap_r_mid16, c->fill_a0, c->fill_sweep,
                                          &c->fcap0_x16, &c->fcap0_y16,
                                          &c->fcap1_x16, &c->fcap1_y16);
                c->head_x16 = c->fcap1_x16;
                c->head_y16 = c->fcap1_y16;

                c->col_deep  = 0x9260u;
                c->col_core  = g3_lerp565(0xFB80u, 0xFDE4u, env->amp);
                c->col_ridge = 0xFFF2u;
                c->col_glow  = 0xE380u;
                c->fill_a    = (uint8_t)(200 + ((env->amp * 55) >> 8));
                c->glow_a    = (uint8_t)(72 + ((env->amp * 115) >> 8));
            }
        }

        /* Capsule 3: Top-Left (9:30..10:50) — Wi-Fi Link capsule (0..4 bars) */
        {
            g3_capsule_t *c = &caps[3];
            c->a0 = (uint16_t)g3_clk_deg_q16(-750); /* 9:30 */
            c->sweep = (uint16_t)((400 * 65536) / 3600); /* 40 deg to 10:50 */
            c->dividers = 4u;
            g3_capsule_compute_caps16(cap_r_mid16, c->a0, c->sweep,
                                      &c->cap0_x16, &c->cap0_y16,
                                      &c->cap1_x16, &c->cap1_y16);
            const uint32_t bars = env->wifi_bars > 4u ? 4u : env->wifi_bars;
            if (bars > 0u) {
                c->has_fill = 1u;
                c->fill_a0 = c->a0;
                c->fill_sweep = (uint16_t)(((uint32_t)c->sweep * bars) / 4u);
                g3_capsule_compute_caps16(cap_r_mid16, c->fill_a0, c->fill_sweep,
                                          &c->fcap0_x16, &c->fcap0_y16,
                                          &c->fcap1_x16, &c->fcap1_y16);
                c->head_x16 = c->fcap1_x16;
                c->head_y16 = c->fcap1_y16;

                c->col_deep  = 0x0384u;
                c->col_core  = 0x06C9u;
                c->col_ridge = 0xAFF6u;
                c->col_glow  = 0x05A7u;
                c->fill_a    = 236u;
                c->glow_a    = 96u;
            }
        }

        /* Single-pass capsule & OLED bloom render strictly in r in [208, 231] */
        const int rmin2_sq = (2 * 208) * (2 * 208);
        const int rmax2_sq = (2 * JR_G3_CHRONO_BEZEL_R_MAX) * (2 * JR_G3_CHRONO_BEZEL_R_MAX);
        int ry0 = 232 - JR_G3_CHRONO_BEZEL_R_MAX;
        int ry1 = 233 + JR_G3_CHRONO_BEZEL_R_MAX;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, 208, JR_G3_CHRONO_BEZEL_R_MAX, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t py16 = y * 16 + 8;
            const int32_t dy16 = dy2 * 8;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t r16 = (int32_t)g3_isqrt((uint32_t)r2_sq << 6);
                    const uint32_t ang = g3_atan2_q16(dy16, dx2 * 8);
                    const int32_t px16 = x * 16 + 8;

                    /* Find the capsule (if any) near this angle */
                    const g3_capsule_t *hit = NULL;
                    for (int ci = 0; ci < 4; ++ci) {
                        const uint32_t rel = (ang - caps[ci].a0) & 0xFFFFu;
                        if (rel <= caps[ci].sweep + 1200u || rel >= 65536u - 1200u) {
                            hit = &caps[ci];
                            break;
                        }
                    }
                    if (hit == NULL) {
                        continue;
                    }

                    const int32_t d_tr = g3_arc_spine_dist16(
                        px16, py16, r16, ang, cap_r_mid16,
                        hit->a0, hit->sweep,
                        hit->cap0_x16, hit->cap0_y16,
                        hit->cap1_x16, hit->cap1_y16);
                    if (d_tr >= track_hw16 + 96) {
                        continue;
                    }

                    int32_t d_fl = 4096;
                    if (hit->has_fill) {
                        d_fl = g3_arc_spine_dist16(
                            px16, py16, r16, ang, cap_r_mid16,
                            hit->fill_a0, hit->fill_sweep,
                            hit->fcap0_x16, hit->fcap0_y16,
                            hit->fcap1_x16, hit->fcap1_y16);
                    }

                    uint16_t pix = row[x];

                    /* 1. Soft outer OLED bloom around lit fill */
                    if (hit->has_fill && d_fl < fill_hw16 + 104) {
                        const int32_t gd = d_fl > fill_hw16 ? (d_fl - fill_hw16) : 0;
                        const int gt = ((104 - gd) * 255) / 104;
                        const unsigned ga = g3_quad_alpha(gt, (unsigned)hit->glow_a, (unsigned)strength);
                        if (ga > 0u) {
                            pix = g3_add565(pix, hit->col_glow, ga, swap_bytes);
                        }
                    }

                    /* 2. Dark framed recessed glass capsule body */
                    if (d_tr < track_hw16 + 8) {
                        int tcov = 255;
                        if (d_tr > track_hw16 - 8) {
                            tcov = ((track_hw16 + 8 - d_tr) * 255) / 16;
                            tcov = g3_clamp(tcov, 0, 255);
                        }
                        if (tcov > 0) {
                            const unsigned ta = (unsigned)((tcov * 218 * strength) >> 16);
                            pix = glass_blend565(pix, 0x0842u, ta, swap_bytes);
                        }
                        /* Crisp 1 px chamfered metallic glass bezel rim */
                        const int32_t d_rim = g3_abs32(d_tr - (track_hw16 - 6));
                        if (d_rim < 12) {
                            const int rcov = ((12 - d_rim) * 255) / 12;
                            /* Top/inner rim catches cool sapphire specular light */
                            const uint16_t rim_col = (r16 < cap_r_mid16) ? 0x3A0Du : 0x1906u;
                            const unsigned ra = (unsigned)((rcov * 175 * strength) >> 16);
                            pix = glass_blend565(pix, rim_col, ra, swap_bytes);
                        }
                    }

                    /* 3. Sculpted luminous liquid-neon inner fill bar */
                    if (hit->has_fill && d_fl < fill_hw16 + 8) {
                        int fcov = 255;
                        if (d_fl > fill_hw16 - 8) {
                            fcov = ((fill_hw16 + 8 - d_fl) * 255) / 16;
                            fcov = g3_clamp(fcov, 0, 255);
                        }
                        /* Optional crisp bar micro-dividers (Wi-Fi 4 bars) */
                        if (hit->dividers > 0u && fcov > 0) {
                            const uint32_t rel = (ang - hit->a0) & 0xFFFFu;
                            if (rel > 120u && rel + 120u < hit->sweep) {
                                const uint32_t seg = (rel * hit->dividers * 256u) / hit->sweep;
                                const int dist_div = g3_abs32((int32_t)((seg + 128u) & 255u) - 128);
                                if (dist_div < 14) {
                                    fcov = (fcov * dist_div) / 14;
                                }
                            }
                        }
                        if (fcov > 0) {
                            /* 3D cylindrical glass cross-section shading */
                            const int32_t r_off = r16 - cap_r_mid16; /* -78..+78 */
                            const int edge_t = g3_clamp(((fill_hw16 - d_fl) * 255) / fill_hw16, 0, 255);
                            uint16_t col = g3_lerp565(hit->col_deep, hit->col_core, edge_t);
                            /* Specular glass ridge along r_off ~ -22 Q4 */
                            const int32_t d_ridge = g3_abs32(r_off + 20);
                            if (d_ridge < 34) {
                                const int rt = ((34 - d_ridge) * 190) / 34;
                                col = g3_lerp565(col, hit->col_ridge, rt);
                            }
                            /* Living traveling energy sheen (Codex / Charging) */
                            if (hit->shimmer_pos > 0u && hit->fill_sweep > 0u) {
                                const uint32_t frel = (ang - hit->fill_a0) & 0xFFFFu;
                                if (frel <= hit->fill_sweep) {
                                    const int pos = (int)((frel * 255u) / hit->fill_sweep);
                                    const int ds = g3_abs32(pos - (int)hit->shimmer_pos);
                                    if (ds < 42) {
                                        const int st = ((42 - ds) * 165) / 42;
                                        col = g3_lerp565(col, 0xFFFFu, st);
                                    }
                                }
                            }
                            const unsigned fa = (unsigned)((fcov * hit->fill_a * strength) >> 16);
                            pix = glass_blend565(pix, col, fa, swap_bytes);
                        }
                    }

                    row[x] = pix;
                }
            }
        }

        /* Subtle jewel leading-edge flare on active Battery / Mic capsules */
        for (int ci = 1; ci <= 2; ++ci) {
            if (caps[ci].has_fill && caps[ci].fill_sweep > 128u) {
                g3_glow_orb_annulus(dst, y0, y1, swap_bytes,
                                    caps[ci].head_x16, caps[ci].head_y16, 6,
                                    caps[ci].col_ridge,
                                    (uint8_t)((135 * strength) >> 8),
                                    208, JR_G3_CHRONO_BEZEL_R_MAX);
            }
        }
    }
}

/* ==========================================================================
 * OPTION 2: AETHERIUM (Live Voice Harmonic Ring & Crystalline Vocal Ribbon)
 * ========================================================================== */

typedef struct {
    bool     valid;
    uint32_t now_ms;
    int      mode;
    uint8_t  amp;
    uint8_t  bands[16];

    uint8_t  sym[32];      /* 32-bin mirrored circular FFT spectrum */
    uint8_t  energy;       /* 0..255 overall spectral energy */
    uint16_t phase[3];     /* animated phase offsets for 3 wave ribbons */
    int16_t  wave_amp[3];  /* radial excursion amplitudes in Q4 px */

    uint16_t col_w[3];     /* 3 iridescent wave ribbon base colors */
    uint16_t col_peak;     /* warm gold/ice peak color on loud harmonics */
    uint16_t col_fil_base; /* FFT filament base (cobalt) */
    uint16_t col_fil_mid;  /* FFT filament body (cyan) */
    uint16_t col_fil_tip;  /* FFT filament hot tip */
    uint16_t col_rib_core; /* vocal ribbon primary core */
    uint16_t col_rib_halo; /* vocal ribbon primary halo */
    uint16_t col_rib_sec;  /* vocal ribbon secondary harmonic strand */

    /* Lower-center Crystalline Vocal Ribbon precomputed per-column heights */
    uint8_t  rib_h1[G3_RIBBON_W];  /* primary wave half-height in Q2 px (0..84) */
    uint8_t  rib_h2[G3_RIBBON_W];  /* secondary wave half-height in Q2 px (0..76) */
    uint8_t  rib_win[G3_RIBBON_W]; /* raised-cosine Hanning envelope 0..255 */
} g3_aeth_cache_t;

static g3_aeth_cache_t s_aeth_cache;

static void g3_aeth_prepare(g3_aeth_cache_t *c, uint32_t now_ms,
                            jr_g3_aeth_mode_t mode,
                            const uint8_t bands[16], uint8_t amp)
{
    uint8_t zero[16] = { 0 };
    const uint8_t *b = bands ? bands : zero;

    if (c->valid && c->now_ms == now_ms && c->mode == (int)mode &&
        c->amp == amp && memcmp(c->bands, b, 16) == 0) {
        return;
    }

    c->valid = true;
    c->now_ms = now_ms;
    c->mode = (int)mode;
    c->amp = amp;
    memcpy(c->bands, b, 16);

    int sum = 0;
    for (int i = 0; i < 16; ++i) {
        sum += b[i];
    }
    const bool has_spec = (sum > 0);
    const int avg = has_spec ? (sum >> 4) : (int)amp;
    c->energy = (uint8_t)g3_clamp(avg, 0, 255);

    /* Mode-specific iridescent OLED palettes & wave dynamics */
    switch (mode) {
    case JR_G3_AETH_THINK:
        c->col_w[0]     = 0x07FFu; /* electric cyan */
        c->col_w[1]     = 0x12BFu; /* deep cobalt-indigo */
        c->col_w[2]     = 0xC1FFu; /* radiant magenta-violet */
        c->col_peak     = 0xFD84u; /* warm gold peak */
        c->col_fil_base = 0x1195u;
        c->col_fil_mid  = 0x633Fu;
        c->col_fil_tip  = 0xE6FFu;
        c->col_rib_core = 0xF7FFu;
        c->col_rib_halo = 0x54FFu;
        c->col_rib_sec  = 0xC23Fu;
        c->phase[0]     = (uint16_t)((now_ms * 46u) & 0xFFFFu);
        c->phase[1]     = (uint16_t)((now_ms * 33u + 21845u) & 0xFFFFu);
        c->phase[2]     = (uint16_t)((now_ms * 64u + 43690u) & 0xFFFFu);
        c->wave_amp[0]  = 200; /* ~12.5 px */
        c->wave_amp[1]  = 175;
        c->wave_amp[2]  = 215;
        break;

    case JR_G3_AETH_SPEAK:
        c->col_w[0]     = 0x07FFu; /* blazing electric cyan */
        c->col_w[1]     = 0x039Fu; /* electric cobalt */
        c->col_w[2]     = 0xD1DFu; /* vivid magenta-violet */
        c->col_peak     = 0xFD63u; /* warm solar gold peak */
        c->col_fil_base = 0x0276u;
        c->col_fil_mid  = 0x07FFu;
        c->col_fil_tip  = 0xFFE8u;
        c->col_rib_core = 0xFFFFu;
        c->col_rib_halo = 0x07FFu;
        c->col_rib_sec  = 0xC25Fu;
        c->phase[0]     = (uint16_t)((now_ms * 58u) & 0xFFFFu);
        c->phase[1]     = (uint16_t)((now_ms * 41u + 20000u) & 0xFFFFu);
        c->phase[2]     = (uint16_t)((now_ms * 78u + 41000u) & 0xFFFFu);
        c->wave_amp[0]  = 235; /* ~14.7 px */
        c->wave_amp[1]  = 195;
        c->wave_amp[2]  = 240;
        break;

    case JR_G3_AETH_LISTEN:
    default:
        c->col_w[0]     = 0x07FDu; /* luminous electric turquoise-cyan */
        c->col_w[1]     = 0x039Eu; /* deep sapphire-cobalt */
        c->col_w[2]     = 0x927Fu; /* soft iridescence violet-magenta */
        c->col_peak     = 0xFDC6u; /* warm champagne-gold peak */
        c->col_fil_base = 0x0254u;
        c->col_fil_mid  = 0x07FBu;
        c->col_fil_tip  = 0xCFFFu;
        c->col_rib_core = 0xF7FFu;
        c->col_rib_halo = 0x07FBu;
        c->col_rib_sec  = 0x45DFu;
        c->phase[0]     = (uint16_t)((now_ms * 34u) & 0xFFFFu);
        c->phase[1]     = (uint16_t)((now_ms * 25u + 22000u) & 0xFFFFu);
        c->phase[2]     = (uint16_t)((now_ms * 48u + 44000u) & 0xFFFFu);
        c->wave_amp[0]  = 190; /* ~11.9 px */
        c->wave_amp[1]  = 160;
        c->wave_amp[2]  = 195;
        break;
    }

    /* Build 32-bin symmetrically mirrored circular FFT spectrum with organic floor */
    const uint32_t flow = (now_ms * 28u) & 0xFFFFu;
    for (int i = 0; i < 16; ++i) {
        const int s1 = glass_sin_q15(((uint32_t)i * 4096u + flow) & 0xFFFFu);
        const int s2 = glass_sin_q15(((uint32_t)i * 6144u - (flow >> 1)) & 0xFFFFu);
        int floor_v = 26 + (((s1 * 14) + (s2 * 10)) >> 15) + ((int)amp >> 2);
        int v = has_spec ? ((int)b[i] > floor_v ? (int)b[i] : ((int)b[i] * 3 + floor_v) >> 2)
                         : floor_v;
        v = g3_clamp(v, 10, 255);
        c->sym[i] = (uint8_t)v;
        c->sym[31 - i] = (uint8_t)v;
    }

    /* Precompute lower-center Crystalline Vocal Waveform Ribbon (275 columns) */
    const uint32_t r_p1 = (now_ms * 72u) & 0xFFFFu;
    const uint32_t r_p2 = (now_ms * 108u) & 0xFFFFu;
    const int base_drive = 46 + ((c->energy * 145 + (int)amp * 64) >> 8); /* 46..255 */

    for (int i = 0; i < G3_RIBBON_W; ++i) {
        /* Raised-cosine window: sin^2(pi * i / (W - 1)) -> 0 at i=0 and i=W-1 */
        const uint32_t w_ang = (uint32_t)((i * 32768) / (G3_RIBBON_W - 1));
        const int sw = glass_sin_q15(w_ang); /* 0..32767 */
        const int win = (int)(((int64_t)sw * sw * 255) >> 30); /* 0..255 */
        c->rib_win[i] = (uint8_t)g3_clamp(win, 0, 255);

        /* Symmetric distance from horizontal center maps bass at center, highs at edges */
        const int dist_c = g3_abs32(i - (G3_RIBBON_W / 2)); /* 0..137 */
        const int b_q8 = (dist_c * 15 * 256) / (G3_RIBBON_W / 2);
        const int bi = g3_clamp(b_q8 >> 8, 0, 14);
        const int bf = b_q8 & 255;
        const int bval = (int)c->sym[bi] + ((((int)c->sym[bi + 1] - (int)c->sym[bi]) * bf) >> 8);

        /* Traveling dual-harmonic crystalline carrier waves */
        const uint32_t x_ang1 = (uint32_t)((i * 3 * 65536) / G3_RIBBON_W);
        const uint32_t x_ang2 = (uint32_t)((i * 5 * 65536) / G3_RIBBON_W);
        const int c1 = glass_sin_q15((x_ang1 + r_p1) & 0xFFFFu);
        const int c2 = glass_sin_q15((x_ang2 - r_p2) & 0xFFFFu);
        const int c3 = glass_sin_q15((x_ang1 * 2u - r_p1 + 16384u) & 0xFFFFu);

        /* Combine live FFT band + harmonic modulation -> primary & secondary heights */
        const int mod1 = 155 + (((c1 * 62) + (c2 * 38)) >> 15); /* ~55..255 */
        const int mod2 = 140 + (((c2 * 70) + (c3 * 45)) >> 15);
        const int drv1 = (bval * 170 + base_drive * 85) >> 8;
        const int drv2 = (bval * 135 + base_drive * 120) >> 8;

        /* Max half-height: 82 Q2 px = 20.5 px (keeps ribbon inside [289, 331] < [286, 334]) */
        int h1 = (int)(((int64_t)win * drv1 * mod1 * 82) >> 24);
        int h2 = (int)(((int64_t)win * drv2 * mod2 * 70) >> 24);
        if (win > 12 && h1 < 6) {
            h1 = 6; /* crisp 1.5 px living filament floor */
        }
        c->rib_h1[i] = (uint8_t)g3_clamp(h1, 0, 82);
        c->rib_h2[i] = (uint8_t)g3_clamp(h2, 0, 72);
    }
}

void jr_g3_aetherium_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                          uint32_t now_ms, jr_g3_aeth_mode_t mode,
                          const uint8_t bands[16], uint8_t amp,
                          int strength)
{
    if (dst == NULL || nrows <= 0 || strength <= 0) {
        return;
    }
    if (strength > 255) {
        strength = 255;
    }
    const int y1 = y0 + nrows;
    if (y1 <= 0 || y0 >= G3_H) {
        return;
    }

    g3_aeth_prepare(&s_aeth_cache, now_ms, mode, bands, amp);
    const g3_aeth_cache_t *c = &s_aeth_cache;

    /* ---- 1. Single-pass Polar Ring Shader strictly in r in [160, 231]:
     *         outer millimeter ticks + 120 FFT filaments + 3 harmonic ribbons */
    {
        const int rmin2_sq = (2 * JR_G3_AETH_RING_R_MIN) * (2 * JR_G3_AETH_RING_R_MIN);
        const int rmax2_sq = (2 * JR_G3_AETH_RING_R_MAX) * (2 * JR_G3_AETH_RING_R_MAX);
        int ry0 = 232 - JR_G3_AETH_RING_R_MAX;
        int ry1 = 233 + JR_G3_AETH_RING_R_MAX;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        const uint32_t p0 = c->phase[0];
        const uint32_t p1 = c->phase[1];
        const uint32_t p2 = c->phase[2];

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, JR_G3_AETH_RING_R_MIN,
                                                 JR_G3_AETH_RING_R_MAX, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t dy16 = dy2 * 8;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t r16 = (int32_t)g3_isqrt((uint32_t)r2_sq << 6);
                    const uint32_t ang = g3_atan2_q16(dy16, dx2 * 8);
                    const uint32_t ang12 = (ang - GLASS_Q16_12_OCLOCK) & 0xFFFFu;

                    /* 120-division polar grid (shared by outer ticks & 120 FFT filaments) */
                    const uint32_t prod120 = ang12 * 120u;
                    const int k120 = (int)(((prod120 + 32768u) >> 16) % 120u);
                    const int32_t frac120 = (int32_t)((prod120 + 32768u) & 0xFFFFu) - 32768;
                    /* 2*pi/(120*65536) * 2^28 = 214.506 -> 215 */
                    const int32_t tan120_q4 = (int32_t)(((int64_t)g3_abs32(frac120) * r16 * 215) >> 28);

                    uint16_t pix = row[x];

                    /* Layer A: Outer Millimeter Tick Ring (r in [218, 228]) */
                    if (r16 >= 217 * 16 && r16 <= 229 * 16) {
                        /* Subtle perimeter guide ring at r=223 */
                        const int32_t d_rail = g3_abs32(r16 - 223 * 16);
                        if (d_rail < 10) {
                            const unsigned ra = (unsigned)((((10 - d_rail) * 24) / 10) * strength >> 8);
                            pix = g3_add565(pix, 0x0358u, ra, swap_bytes);
                        }
                        if ((k120 % 30) != 0) { /* leave 12/3/6/9 clean for cardinal jewels */
                            const bool major = ((k120 % 5) == 0);
                            const int32_t t_lo = (major ? 218 : 220) * 16;
                            const int32_t t_hi = (major ? 228 : 226) * 16;
                            const int32_t hw = major ? 13 : 9;
                            if (r16 >= t_lo - 6 && r16 <= t_hi + 6 && tan120_q4 < hw) {
                                int cov = ((hw - tan120_q4) * 255) / (hw > 8 ? 8 : hw);
                                if (cov > 255) { cov = 255; }
                                if (r16 < t_lo) {
                                    cov = (cov * (r16 - (t_lo - 6))) / 6;
                                } else if (r16 > t_hi) {
                                    cov = (cov * ((t_hi + 6) - r16)) / 6;
                                }
                                if (cov > 0) {
                                    const uint16_t tc = major ? 0x3D5Eu : 0x296Bu;
                                    const unsigned ta = (unsigned)((cov * (major ? 205 : 145) * strength) >> 16);
                                    pix = glass_blend565(pix, tc, ta, swap_bytes);
                                }
                            }
                        }
                    }

                    /* Smoothly interpolated circular FFT value (0..255) */
                    if (r16 <= 216 * 16) {
                        const uint32_t sp_i = ang12 >> 11; /* 0..31 */
                        const int sp_f = (int)((ang12 >> 3) & 0xFFu);
                        const int s0 = c->sym[sp_i];
                        const int s1 = c->sym[(sp_i + 1u) & 31u];
                        const int spec = s0 + (((s1 - s0) * sp_f) >> 8);

                        /* Layer B: 120 Radial FFT Spectrum Filaments (r in [164, 212]) */
                        if (r16 >= 164 * 16 && r16 <= 212 * 16 && tan120_q4 < 24) {
                            const int32_t f_lo = 184 * 16 - ((spec * 20 * 16) >> 8); /* down to 164 */
                            const int32_t f_hi = 189 * 16 + ((spec * 23 * 16) >> 8); /* up to 212 */
                            if (r16 >= f_lo - 8 && r16 <= f_hi + 8) {
                                int fcov = 0;
                                if (tan120_q4 < 12) {
                                    fcov = ((12 - tan120_q4) * 255) / 8;
                                    if (fcov > 255) { fcov = 255; }
                                } else {
                                    /* Fine optical fiber outer halo */
                                    fcov = ((24 - tan120_q4) * 70) / 12;
                                }
                                if (r16 < f_lo) {
                                    fcov = (fcov * (r16 - (f_lo - 8))) >> 3;
                                } else if (r16 > f_hi) {
                                    fcov = (fcov * ((f_hi + 8) - r16)) >> 3;
                                }
                                if (fcov > 0) {
                                    const int32_t flen = f_hi - f_lo;
                                    const int rt = flen > 0 ? g3_clamp(((r16 - f_lo) * 255) / flen, 0, 255) : 128;
                                    uint16_t fcol;
                                    if (rt > 195 && spec > 140) {
                                        fcol = g3_lerp565(c->col_fil_mid, c->col_fil_tip, (rt - 195) * 4);
                                    } else {
                                        fcol = g3_lerp565(c->col_fil_base, c->col_fil_mid, rt);
                                    }
                                    const unsigned fa = (unsigned)((fcov * (85 + ((spec * 135) >> 8)) * strength) >> 16);
                                    pix = g3_add565(pix, fcol, fa, swap_bytes);
                                }
                            }
                        }

                        /* Layer C: 3 Phase-Shifted Iridescent Harmonic Wave Ribbons (r ~ 178..208) */
                        if (r16 >= 166 * 16 && r16 <= 216 * 16) {
                            const int mod = 72 + ((spec * 128 + (int)amp * 55) >> 8);

                            const int32_t w0_s = (glass_sin_q15((ang12 * 3u + p0) & 0xFFFFu) * 3 +
                                                  glass_sin_q15((ang12 * 5u - p0 * 2u) & 0xFFFFu) * 2) / 5;
                            const int32_t w1_s = (glass_sin_q15((ang12 * 2u - p1) & 0xFFFFu) * 3 +
                                                  glass_sin_q15((ang12 * 4u + p1) & 0xFFFFu) * 2) / 5;
                            const int32_t w2_s = (glass_sin_q15((ang12 * 4u + p2) & 0xFFFFu) * 3 +
                                                  glass_sin_q15((ang12 * 7u - p2) & 0xFFFFu) * 2) / 5;

                            int32_t rw[3];
                            rw[0] = 192 * 16 + (int32_t)(((int64_t)w0_s * c->wave_amp[0] * mod) >> 23);
                            rw[1] = 190 * 16 + (int32_t)(((int64_t)w1_s * c->wave_amp[1] * mod) >> 23);
                            rw[2] = 194 * 16 + (int32_t)(((int64_t)w2_s * c->wave_amp[2] * mod) >> 23);

                            for (int w = 0; w < 3; ++w) {
                                if (rw[w] < 176 * 16) { rw[w] = 176 * 16; }
                                if (rw[w] > 208 * 16) { rw[w] = 208 * 16; }
                                const int32_t dr = g3_abs32(r16 - rw[w]);
                                if (dr >= 92) {
                                    continue;
                                }
                                /* Loud harmonic peak iridescence -> warm gold crest */
                                const int32_t disp = g3_abs32(rw[w] - 192 * 16);
                                uint16_t wcol = c->col_w[w];
                                if (spec > 155 || disp > 150) {
                                    const int pt = g3_clamp(((spec - 140) * 2) + ((disp - 130) >> 1), 0, 210);
                                    wcol = g3_lerp565(wcol, c->col_peak, pt);
                                }
                                /* Silky quadratic OLED plasma ribbon aura */
                                const int at = ((92 - dr) * 255) / 92;
                                const unsigned aura_a = g3_quad_alpha(at, w == 0 ? 115u : 95u, (unsigned)strength);
                                if (aura_a > 0u) {
                                    pix = g3_add565(pix, wcol, aura_a, swap_bytes);
                                }
                                /* Razor-luminous incandescent ribbon core */
                                if (dr < 20) {
                                    const unsigned core_a = (unsigned)((((20 - dr) * (w == 0 ? 195 : 155)) / 20) * strength >> 8);
                                    const uint16_t ccol = dr < 8 ? g3_lerp565(wcol, 0xFFFFu, 150) : wcol;
                                    pix = g3_add565(pix, ccol, core_a, swap_bytes);
                                }
                            }
                        }
                    }

                    row[x] = pix;
                }
            }
        }
    }

    /* ---- 2. 4 Glowing Cyan Cardinal Dots at 12/3/6/9 o'clock (r = 223) ---- */
    {
        static const uint32_t k_card_q16[4] = {
            GLASS_Q16_12_OCLOCK, 0u, 16384u, 32768u
        };
        const uint8_t halo_a = (uint8_t)((g3_clamp(175 + ((int)amp >> 2), 0, 255) * strength) >> 8);
        const uint8_t core_a = (uint8_t)strength;
        for (int i = 0; i < 4; ++i) {
            const int32_t cx16 = G3_CX16 + ((223 * 16 * glass_cos_q15(k_card_q16[i])) >> 15);
            const int32_t cy16 = G3_CY16 + ((223 * 16 * glass_sin_q15(k_card_q16[i])) >> 15);
            g3_glow_orb_annulus(dst, y0, y1, swap_bytes, cx16, cy16, 6,
                                0x06FFu, halo_a, 214, JR_G3_AETH_RING_R_MAX);
            g3_glow_orb_annulus(dst, y0, y1, swap_bytes, cx16, cy16, 2,
                                0xFFFFu, core_a, 214, JR_G3_AETH_RING_R_MAX);
        }
    }

    /* ---- 3. Live Crystalline Vocal Waveform Ribbon strictly inside
     *         x in [96, 370], y in [286, 334] (leaves y in [165, 275] clean!) */
    {
        int ry0 = JR_G3_AETH_RIBBON_Y0;
        int ry1 = JR_G3_AETH_RIBBON_Y1;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        for (int y = ry0; y <= ry1; ++y) {
            const int32_t ady16 = g3_abs32(y * 16 + 8 - G3_RIBBON_CY16);
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int x = JR_G3_AETH_RIBBON_X0; x <= JR_G3_AETH_RIBBON_X1; ++x) {
                const int idx = x - JR_G3_AETH_RIBBON_X0;
                const int win = c->rib_win[idx];
                if (win == 0) {
                    continue;
                }
                const int32_t h1 = (int32_t)c->rib_h1[idx] * 4; /* Q4 px */
                const int32_t h2 = (int32_t)c->rib_h2[idx] * 4;
                const unsigned ws = ((unsigned)win * (unsigned)strength) >> 8; /* 0..255 */
                if (ws == 0u) {
                    continue;
                }
                uint16_t pix = row[x];

                /* Subtle luminous translucent interior fill between dual strands */
                if (ady16 < h1 && h1 > 8) {
                    const uint32_t it = (uint32_t)(((h1 - ady16) * 255) / h1);
                    const unsigned ia = (unsigned)((it * 38u * ws) >> 16);
                    if (ia > 0u) {
                        pix = g3_add565(pix, c->col_rib_halo, ia, swap_bytes);
                    }
                }

                /* Centerline optical horizon filament */
                if (ady16 < 16) {
                    const unsigned ha = (unsigned)((((uint32_t)(16 - ady16) * 44u) / 16u) * ws >> 8);
                    if (ha > 0u) {
                        pix = g3_add565(pix, c->col_rib_halo, ha, swap_bytes);
                    }
                }

                /* Secondary harmonic iridescent strand (symmetric +-h2) */
                const int32_t d2 = g3_abs32(ady16 - h2);
                if (d2 < 44) {
                    const int t2 = ((44 - d2) * 255) / 44;
                    const unsigned a2 = g3_quad_alpha(t2, 130u, ws);
                    if (a2 > 0u) {
                        pix = g3_add565(pix, c->col_rib_sec, a2, swap_bytes);
                    }
                }

                /* Primary dual symmetric glowing cyan-white crystalline strand (+-h1) */
                const int32_t d1 = g3_abs32(ady16 - h1);
                if (d1 < 64) {
                    const int t1 = ((64 - d1) * 255) / 64;
                    const unsigned halo1 = g3_quad_alpha(t1, 165u, ws);
                    if (halo1 > 0u) {
                        pix = g3_add565(pix, c->col_rib_halo, halo1, swap_bytes);
                    }
                    if (d1 < 20) {
                        const unsigned core1 = (unsigned)((((uint32_t)(20 - d1) * 235u) / 20u) * ws >> 8);
                        if (core1 > 0u) {
                            pix = g3_add565(pix, c->col_rib_core, core1, swap_bytes);
                        }
                    }
                }

                row[x] = pix;
            }
        }
    }
}

/* ==========================================================================
 * ROTARY BEZEL SCRUB OVERLAY (Luminous Rotary Dial Arc + Jewel + Tick Scale)
 * ========================================================================== */

void jr_g3_bezel_scrub_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                            uint32_t now_ms, uint8_t value_pct,
                            jr_g3_scrub_kind_t kind, int strength)
{
    if (dst == NULL || nrows <= 0 || strength <= 0) {
        return;
    }
    if (strength > 255) {
        strength = 255;
    }
    const int y1 = y0 + nrows;
    if (y1 <= 0 || y0 >= G3_H) {
        return;
    }

    const uint32_t pct = value_pct > 100u ? 100u : (uint32_t)value_pct;

    /* 270-deg rotary sweep: 7:30 (24576) clockwise through 12:00 to 4:30 (8192) */
    const uint32_t scrub_a0 = 24576u;
    const uint32_t scrub_sweep = 49152u;
    const uint32_t fill_sweep = (scrub_sweep * pct) / 100u;
    const uint32_t ang_val = (scrub_a0 + fill_sweep) & 0xFFFFu;

    /* Palette selection */
    uint16_t col_deep, col_core, col_hot, col_glow, col_tick_lit;
    switch (kind) {
    case JR_G3_SCRUB_BRIGHTNESS:
        col_deep     = 0x035Au; /* deep electric sapphire */
        col_core     = 0x37FFu; /* crisp ice-cyan */
        col_hot      = 0xFFFFu; /* solar white */
        col_glow     = 0x06FFu;
        col_tick_lit = 0x5FFFu;
        break;
    case JR_G3_SCRUB_TIMER:
        col_deep     = 0x5178u; /* deep indigo-violet */
        col_core     = 0xA33Fu; /* electric violet */
        col_hot      = 0xF7FFu; /* bright plasma-white */
        col_glow     = 0x927Fu;
        col_tick_lit = 0xC47Fu;
        break;
    case JR_G3_SCRUB_VOLUME:
    default:
        col_deep     = 0xB300u; /* deep burnished amber */
        col_core     = 0xFC80u; /* luminous electric amber-gold */
        col_hot      = 0xFFF2u; /* champagne highlight */
        col_glow     = 0xEA80u;
        col_tick_lit = 0xFDA4u;
        break;
    }

    const int32_t r_mid16   = 219 * 16; /* arc spine r=219 -> track [214,224], glow [208,229] */
    const int32_t track_hw  = 80;       /* 5.0 px half-width */
    const int32_t fill_hw   = 64;       /* 4.0 px half-width */

    int32_t tcap0_x, tcap0_y, tcap1_x, tcap1_y;
    g3_capsule_compute_caps(r_mid16, scrub_a0, scrub_sweep,
                            &tcap0_x, &tcap0_y, &tcap1_x, &tcap1_y);
    int32_t fcap0_x, fcap0_y, fcap1_x, fcap1_y;
    g3_capsule_compute_caps(r_mid16, scrub_a0, fill_sweep,
                            &fcap0_x, &fcap0_y, &fcap1_x, &fcap1_y);

    const int32_t jewel_x16 = fcap1_x;
    const int32_t jewel_y16 = fcap1_y;
    const int pulse = (glass_sin_q15((now_ms * 32u) & 0xFFFFu) + 32768) >> 8; /* 0..255 */

    /* ---- 1. Single-pass Rotary Dial Arc + 41-Tick Precision Inner Scale
     *         strictly inside r in [204, 231] ------------------------------ */
    {
        const int rmin2_sq = (2 * JR_G3_SCRUB_R_MIN) * (2 * JR_G3_SCRUB_R_MIN);
        const int rmax2_sq = (2 * JR_G3_SCRUB_R_MAX) * (2 * JR_G3_SCRUB_R_MAX);
        int ry0 = 232 - JR_G3_SCRUB_R_MAX;
        int ry1 = 233 + JR_G3_SCRUB_R_MAX;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, JR_G3_SCRUB_R_MIN,
                                                 JR_G3_SCRUB_R_MAX, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t py16 = y * 16 + 8;
            const int32_t dy16 = dy2 * 8;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t r16 = (int32_t)g3_isqrt((uint32_t)r2_sq << 6);
                    const uint32_t ang = g3_atan2_q16(dy16, dx2 * 8);
                    const int32_t px16 = x * 16 + 8;
                    const uint32_t rel = (ang - scrub_a0) & 0xFFFFu;

                    uint16_t pix = row[x];

                    /* Layer A: Subtle 41-Tick Precision Inner Scale (r in [205, 211]) */
                    if (r16 >= 204 * 16 + 8 && r16 <= 212 * 16) {
                        /* Snap nearest tick k in 0..40 across scrub_sweep (49152) */
                        int32_t s_rel = (rel <= scrub_sweep + 1024u)
                            ? (int32_t)rel
                            : ((rel >= 65536u - 1024u) ? (int32_t)rel - 65536 : -10000);
                        if (s_rel >= -600 && s_rel <= (int32_t)scrub_sweep + 600) {
                            const int k = g3_clamp((int)((s_rel * 40 + 24576) / 49152), 0, 40);
                            const int32_t tick_ang = (k * (int32_t)scrub_sweep) / 40;
                            const int32_t d_ang = g3_abs32(s_rel - tick_ang);
                            /* Convert Q16 angular diff to Q4 tangential px: 2*pi/65536 * 2^22 = 402 */
                            const int32_t tan_q4 = (int32_t)(((int64_t)d_ang * r16 * 402) >> 22);
                            const bool major = ((k % 4) == 0);
                            const int32_t t_lo = (major ? 205 : 207) * 16;
                            const int32_t t_hi = 211 * 16;
                            const int32_t hw = major ? 14 : 9;
                            if (r16 >= t_lo - 6 && r16 <= t_hi + 6 && tan_q4 < hw) {
                                int cov = ((hw - tan_q4) * 255) / 8;
                                if (cov > 255) { cov = 255; }
                                if (r16 < t_lo) {
                                    cov = (cov * (r16 - (t_lo - 6))) / 6;
                                } else if (r16 > t_hi) {
                                    cov = (cov * ((t_hi + 6) - r16)) / 6;
                                }
                                if (cov > 0) {
                                    const bool lit = ((uint32_t)k * 5u <= pct * 2u);
                                    const uint16_t tcol = lit ? col_tick_lit : 0x39EBu;
                                    const unsigned ta = (unsigned)((cov * (lit ? (major ? 235 : 175) : (major ? 145 : 95)) * strength) >> 16);
                                    pix = glass_blend565(pix, tcol, ta, swap_bytes);
                                }
                            }
                        }
                    }

                    /* Layer B: Recessed Rotary Track & Razor-Sharp Luminous Dial Arc */
                    if (r16 >= 208 * 16) {
                        const int32_t d_tr = g3_arc_spine_dist16(
                            px16, py16, r16, ang, r_mid16,
                            scrub_a0, scrub_sweep,
                            tcap0_x, tcap0_y, tcap1_x, tcap1_y);

                        /* Dark recessed glass track */
                        if (d_tr < track_hw + 8) {
                            int tcov = 255;
                            if (d_tr > track_hw - 8) {
                                tcov = ((track_hw + 8 - d_tr) * 255) / 16;
                                tcov = g3_clamp(tcov, 0, 255);
                            }
                            if (tcov > 0) {
                                const unsigned ta = (unsigned)((tcov * 218 * strength) >> 16);
                                pix = glass_blend565(pix, 0x0842u, ta, swap_bytes);
                            }
                            const int32_t d_rim = g3_abs32(d_tr - (track_hw - 6));
                            if (d_rim < 12) {
                                const int rcov = ((12 - d_rim) * 255) / 12;
                                const unsigned ra = (unsigned)((rcov * 155 * strength) >> 16);
                                pix = glass_blend565(pix, 0x31ACu, ra, swap_bytes);
                            }
                        }

                        /* Active luminous arc + soft OLED bloom */
                        if (pct > 0u) {
                            const int32_t d_fl = g3_arc_spine_dist16(
                                px16, py16, r16, ang, r_mid16,
                                scrub_a0, fill_sweep,
                                fcap0_x, fcap0_y, fcap1_x, fcap1_y);

                            /* Outer OLED bloom strictly clipped by annulus [204, 231] */
                            if (d_fl < fill_hw + 96) {
                                const int32_t gd = d_fl > fill_hw ? (d_fl - fill_hw) : 0;
                                const int gt = ((96 - gd) * 255) / 96;
                                const unsigned ga = g3_quad_alpha(gt, 125u, (unsigned)strength);
                                if (ga > 0u) {
                                    pix = g3_add565(pix, col_glow, ga, swap_bytes);
                                }
                            }

                            /* Razor-sharp luminous fill stroke with gradient & glass ridge */
                            if (d_fl < fill_hw + 8) {
                                int fcov = 255;
                                if (d_fl > fill_hw - 8) {
                                    fcov = ((fill_hw + 8 - d_fl) * 255) / 16;
                                    fcov = g3_clamp(fcov, 0, 255);
                                }
                                if (fcov > 0) {
                                    uint32_t prog;
                                    if (rel > 57344u) {
                                        prog = 0u; /* round start cap just before 7:30 */
                                    } else if (rel <= fill_sweep && fill_sweep > 0u) {
                                        prog = (rel * 255u) / fill_sweep;
                                    } else {
                                        prog = 255u; /* round head cap just past ang_val */
                                    }
                                    uint16_t col = g3_lerp565(col_deep, col_core, (int)prog);
                                    if (prog > 190u) {
                                        col = g3_lerp565(col, col_hot, (int)((prog - 190u) * 3u));
                                    }
                                    /* Specular cylindrical ridge */
                                    const int32_t d_ridge = g3_abs32((r16 - r_mid16) + 16);
                                    if (d_ridge < 28) {
                                        col = g3_lerp565(col, col_hot, ((28 - d_ridge) * 165) / 28);
                                    }
                                    const unsigned fa = (unsigned)((fcov * 245 * strength) >> 16);
                                    pix = glass_blend565(pix, col, fa, swap_bytes);
                                }
                            }
                        }
                    }

                    row[x] = pix;
                }
            }
        }
    }

    /* ---- 2. Glowing Precision Faceted Jewel Indicator at ang_val (r=219) --- */
    {
        /* Soft wide OLED halo around jewel, strictly clipped to r in [204, 231] */
        const uint8_t halo_a = (uint8_t)(((175 + ((pulse * 55) >> 8)) * strength) >> 8);
        g3_glow_orb_annulus(dst, y0, y1, swap_bytes,
                            jewel_x16, jewel_y16, 11,
                            col_glow, halo_a,
                            JR_G3_SCRUB_R_MIN, JR_G3_SCRUB_R_MAX);

        /* Faceted bezel collar + luminous jewel core strictly in r in [204, 231] */
        const int jx_i = (int)(jewel_x16 >> 4);
        const int jy_i = (int)(jewel_y16 >> 4);
        int ry0 = jy_i - 8;
        int ry1 = jy_i + 8;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }
        int rx0 = jx_i - 8;
        int rx1 = jx_i + 8;
        if (rx0 < 0) { rx0 = 0; }
        if (rx1 >= G3_W) { rx1 = G3_W - 1; }

        const int rmin2_sq = (2 * JR_G3_SCRUB_R_MIN) * (2 * JR_G3_SCRUB_R_MIN);
        const int rmax2_sq = (2 * JR_G3_SCRUB_R_MAX) * (2 * JR_G3_SCRUB_R_MAX);
        const int32_t outer_r16 = 6 * 16 + 8; /* 6.5 px jewel bezel */
        const int32_t core_r16  = 4 * 16 + 8; /* 4.5 px glowing jewel */

        for (int y = ry0; y <= ry1; ++y) {
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t jy16 = y * 16 + 8 - jewel_y16;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int x = rx0; x <= rx1; ++x) {
                const int dx2 = 2 * x - G3_CX2;
                const int r2_sq = dx2 * dx2 + dy2_sq;
                if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                    continue;
                }
                const int32_t jx16 = x * 16 + 8 - jewel_x16;
                const int32_t d2 = jx16 * jx16 + jy16 * jy16;
                if (d2 >= (outer_r16 + 12) * (outer_r16 + 12)) {
                    continue;
                }
                const int32_t d16 = (int32_t)g3_isqrt((uint32_t)d2);
                uint16_t pix = row[x];

                /* Dark metallic collar + bright rim */
                if (d16 < outer_r16 + 8) {
                    int bcov = 255;
                    if (d16 > outer_r16 - 8) {
                        bcov = g3_clamp(((outer_r16 + 8 - d16) * 255) / 16, 0, 255);
                    }
                    const uint16_t collar_col = (d16 > core_r16 + 8) ? 0xCE79u : 0x0842u;
                    const unsigned ba = (unsigned)((bcov * 240 * strength) >> 16);
                    pix = glass_blend565(pix, collar_col, ba, swap_bytes);
                }

                /* Radiant jewel body with blazing white faceted center */
                if (d16 < core_r16 + 8) {
                    int ccov = 255;
                    if (d16 > core_r16 - 8) {
                        ccov = g3_clamp(((core_r16 + 8 - d16) * 255) / 16, 0, 255);
                    }
                    const int ct = g3_clamp(((core_r16 - d16) * 255) / core_r16, 0, 255);
                    uint16_t jcol = g3_lerp565(col_core, 0xFFFFu, (ct * ct) >> 8);
                    const unsigned ja = (unsigned)((ccov * strength) >> 8);
                    pix = glass_blend565(pix, jcol, ja, swap_bytes);
                }

                row[x] = pix;
            }
        }
    }

    (void)ang_val;
}
