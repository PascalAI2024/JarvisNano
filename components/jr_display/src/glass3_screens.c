/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass3_screens.c — JarvisNano Glass 3.0 procedural OLED data-screen engines.
 *
 * Pure C11, integer-only, zero-libm, zero-heap, <512 B stack, 100% strip-
 * invariant across arbitrary [y0, y0+nrows) slices on 466x466 RGB565.
 *
 *   1. ATMOSPHAERA    (jr_g3_atmosphaera_rows) — Weather Screen Instrument
 *   2. CORTEX COMMAND (jr_g3_cortex_rows)      — Status + Codex Telemetry Deck
 */
#include "glass3_screens.h"

#include <stddef.h>

#define G3_W      JR_G3_PANEL_W
#define G3_H      JR_G3_PANEL_H
#define G3_CX16   (233 * 16) /* pixel center is 16*x + 8 -> 232.5 px = 3728 */
#define G3_CY16   (233 * 16)
#define G3_R_MAX  232

/* ==========================================================================
 * PURE INTEGER MATH & OLED COLOR PRIMITIVES (ZERO FLOAT, ZERO LIBM)
 * ========================================================================== */

static inline int32_t g3_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int32_t g3_abs(int32_t v)
{
    return v < 0 ? -v : v;
}

static inline int32_t g3_min(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static inline int32_t g3_max(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static uint32_t g3_isqrt(uint32_t n)
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

/* 65-entry Q15 quarter-wave sine table (130 bytes in .rodata). */
static const int16_t s_g3_sin_q15[65] = {
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

static int32_t g3_sin_q15(uint32_t angle_q16)
{
    uint32_t t = angle_q16 & 0xFFFFu;
    const bool neg = (t >= 0x8000u);
    t &= 0x7FFFu;
    if (t > 0x4000u) {
        t = 0x8000u - t;
    }
    const uint32_t i = t >> 8;
    const uint32_t f = t & 0xFFu;
    int32_t v = s_g3_sin_q15[i < 64u ? i : 64u];
    if (i < 64u) {
        v += ((int32_t)(s_g3_sin_q15[i + 1u] - s_g3_sin_q15[i]) * (int32_t)f) >> 8;
    }
    return neg ? -v : v;
}

static inline int32_t g3_cos_q15(uint32_t angle_q16)
{
    return g3_sin_q15(angle_q16 + 0x4000u);
}

/* 65-entry first-octant atan LUT: round(atan(i/64) * 65536 / (2*pi)) (130 B). */
static const uint16_t s_g3_atan_oct[65] = {
       0u,  163u,  326u,  489u,  651u,  813u,  975u, 1136u,
    1297u, 1457u, 1617u, 1775u, 1933u, 2090u, 2246u, 2401u,
    2555u, 2708u, 2860u, 3010u, 3159u, 3307u, 3453u, 3599u,
    3742u, 3884u, 4025u, 4164u, 4302u, 4438u, 4572u, 4705u,
    4836u, 4966u, 5094u, 5220u, 5344u, 5467u, 5589u, 5708u,
    5826u, 5943u, 6058u, 6171u, 6282u, 6392u, 6500u, 6607u,
    6712u, 6815u, 6917u, 7018u, 7117u, 7214u, 7310u, 7405u,
    7498u, 7589u, 7679u, 7768u, 7856u, 7942u, 8026u, 8110u,
    8192u,
};

/* Pure integer atan2 returning Q16 turns (0..65535):
 *   0 = 3 o'clock (+x), 16384 = 6 o'clock (+y),
 *   32768 = 9 o'clock (-x), 49152 = 12 o'clock (-y).
 * Accuracy < 0.01 deg across all octants. */
static uint32_t g3_atan2_q16(int32_t dy, int32_t dx)
{
    const uint32_t ax = (uint32_t)(dx < 0 ? -dx : dx);
    const uint32_t ay = (uint32_t)(dy < 0 ? -dy : dy);
    if (ax == 0u && ay == 0u) {
        return 0u;
    }
    const uint32_t mn = ax < ay ? ax : ay;
    const uint32_t mx = ax > ay ? ax : ay;
    const uint32_t t_q12 = (mn << 12) / mx; /* 0..4096 */
    const uint32_t idx = t_q12 >> 6;        /* 0..64 */
    const uint32_t frac = t_q12 & 63u;
    uint32_t a = s_g3_atan_oct[idx];
    if (idx < 64u) {
        a += ((uint32_t)(s_g3_atan_oct[idx + 1u] - s_g3_atan_oct[idx]) * frac + 32u) >> 6;
    }
    if (dx >= 0) {
        if (dy >= 0) {
            return (ax >= ay) ? a : (16384u - a);
        }
        return (ay >= ax) ? (49152u + a) : ((65536u - a) & 0xFFFFu);
    }
    if (dy >= 0) {
        return (ay >= ax) ? (16384u + a) : (32768u - a);
    }
    return (ax >= ay) ? (32768u + a) : (49152u - a);
}

/* 4x4 Bayer ordered dither matrix (0..15) for smooth dark OLED gradients. */
static const uint8_t s_g3_bayer4[16] = {
     0u,  8u,  2u, 10u,
    12u,  4u, 14u,  6u,
     3u, 11u,  1u,  9u,
    15u,  7u, 13u,  5u,
};

static inline uint16_t g3_bswap(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t g3_rgb565(int32_t r, int32_t g, int32_t b)
{
    r = g3_clamp(r, 0, 255);
    g = g3_clamp(g, 0, 255);
    b = g3_clamp(b, 0, 255);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Ordered-dithered RGB888 -> native RGB565 using global panel (x, y). */
static inline uint16_t g3_rgb565_dither(int32_t r, int32_t g, int32_t b, int x, int y)
{
    const int32_t d = (int32_t)s_g3_bayer4[((y & 3) << 2) | (x & 3)];
    r = g3_clamp(r + (d >> 1), 0, 255);
    g = g3_clamp(g + (d >> 2), 0, 255);
    b = g3_clamp(b + (d >> 1), 0, 255);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Standard 6-bit rounded alpha blend (matches glass_blend565 bit-for-bit). */
static inline uint16_t g3_blend565(uint16_t under, uint16_t over, unsigned a,
                                   bool swap_bytes)
{
    if (a == 0u) {
        return under;
    }
    if (a >= 255u) {
        return swap_bytes ? g3_bswap(over) : over;
    }
    const uint32_t wo = (a * 64u + 127u) / 255u;
    const uint32_t wu = 64u - wo;
    const uint32_t u = swap_bytes ? g3_bswap(under) : under;
    const uint32_t rb = ((u & 0xF81Fu) * wu + (uint32_t)(over & 0xF81Fu) * wo + 0x10020u) >> 6;
    const uint32_t g  = ((u & 0x07E0u) * wu + (uint32_t)(over & 0x07E0u) * wo + (32u << 5)) >> 6;
    const uint16_t out = (uint16_t)((rb & 0xF81Fu) | (g & 0x07E0u));
    return swap_bytes ? g3_bswap(out) : out;
}

/* Saturating additive OLED light blend (matches showpiece_gfx sp_add565). */
static inline uint16_t g3_add565(uint16_t under, uint16_t over, unsigned a,
                                 bool swap_bytes)
{
    if (a == 0u) {
        return under;
    }
    if (a > 255u) {
        a = 255u;
    }
    const uint32_t u = swap_bytes ? g3_bswap(under) : under;
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
    return swap_bytes ? g3_bswap(out) : out;
}

/* Linear RGB565 interpolation by t in 0..255. */
static inline uint16_t g3_lerp565(uint16_t c0, uint16_t c1, int32_t t)
{
    if (t <= 0) {
        return c0;
    }
    if (t >= 255) {
        return c1;
    }
    const int32_t r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
    const int32_t r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    const int32_t r = r0 + (((r1 - r0) * t) >> 8);
    const int32_t g = g0 + (((g1 - g0) * t) >> 8);
    const int32_t b = b0 + (((b1 - b0) * t) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Scale an RGB565 color's luminance by k in 0..256. */
static inline uint16_t g3_scale565(uint16_t c, int32_t k)
{
    if (k <= 0) {
        return 0x0000u;
    }
    if (k >= 256) {
        return c;
    }
    const uint32_t r = (((c >> 11) & 31u) * (uint32_t)k + 128u) >> 8;
    const uint32_t g = (((c >> 5) & 63u) * (uint32_t)k + 128u) >> 8;
    const uint32_t b = ((c & 31u) * (uint32_t)k + 128u) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Signed distance in Q4 px to axis-aligned rounded rectangle centered at
 * (cx16, cy16) with half-extents (hx16, hy16) and corner radius rad16. */
static int32_t g3_sd_rrect16(int32_t px16, int32_t py16,
                             int32_t cx16, int32_t cy16,
                             int32_t hx16, int32_t hy16, int32_t rad16)
{
    const int32_t qx = g3_abs(px16 - cx16) - (hx16 - rad16);
    const int32_t qy = g3_abs(py16 - cy16) - (hy16 - rad16);
    if (qx > 0 && qy > 0) {
        return (int32_t)g3_isqrt((uint32_t)(qx * qx + qy * qy)) - rad16;
    }
    return g3_max(qx, qy) - rad16;
}

/* Soft quadratic-falloff additive OLED glow orb clipped to [y0, y1),
 * box [bx0..bx1] x [by0..by1], and circle r <= clip_r about panel center. */
static void g3_glow_orb_clipped(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                int32_t cx16, int32_t cy16, int32_t r_px,
                                uint16_t color, uint8_t peak_a,
                                int bx0, int bx1, int by0, int by1, int clip_r)
{
    if (r_px <= 0 || peak_a == 0u) {
        return;
    }
    const int cx_i = cx16 >> 4;
    const int cy_i = cy16 >> 4;
    int ry0 = g3_max(g3_max(cy_i - r_px - 1, by0), y0);
    int ry1 = g3_min(g3_min(cy_i + r_px + 1, by1), y1 - 1);
    if (ry0 > ry1) {
        return;
    }
    int rx0 = g3_max(g3_max(cx_i - r_px - 1, bx0), 0);
    int rx1 = g3_min(g3_min(cx_i + r_px + 1, bx1), G3_W - 1);
    if (rx0 > rx1) {
        return;
    }
    const int32_t r16 = r_px * 16;
    const int32_t r16_sq = r16 * r16;
    const int32_t clip16_sq = (clip_r * 16) * (clip_r * 16);

    for (int y = ry0; y <= ry1; ++y) {
        const int32_t gy = y * 16 + 8 - G3_CY16;
        const int32_t gy_sq = gy * gy;
        if (gy_sq > clip16_sq) {
            continue;
        }
        const int32_t py = y * 16 + 8 - cy16;
        const int32_t py_sq = py * py;
        if (py_sq >= r16_sq) {
            continue;
        }
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;
        for (int x = rx0; x <= rx1; ++x) {
            const int32_t gx = x * 16 + 8 - G3_CX16;
            if (gx * gx + gy_sq > clip16_sq) {
                continue;
            }
            const int32_t px = x * 16 + 8 - cx16;
            const int32_t d2 = px * px + py_sq;
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

/* ==========================================================================
 * 1. ATMOSPHAERA — WEATHER SCREEN INSTRUMENT
 * ========================================================================== */

/* 256-entry 360-deg Thermal/Solar RGB565 LUT (512 bytes in .rodata).
 * Index = angle_q16 >> 8:
 *   128 (6A/9 o'clock) arctic cyan -> 192 (12P/12 o'clock) golden daylight ->
 *     0 (6P/3 o'clock) solar amber-coral -> 64 (12A/6 o'clock) midnight indigo. */
static const uint16_t s_atmos_lut[256] = {
    0xEAE6u, 0xEAE6u, 0xEAC6u, 0xE2C7u, 0xE2A7u, 0xDAA7u, 0xD288u, 0xCA68u,
    0xC249u, 0xBA4Au, 0xB22Au, 0xAA0Bu, 0xA1ECu, 0x99CCu, 0x91ADu, 0x89ADu,
    0x898Eu, 0x816Eu, 0x796Eu, 0x796Fu, 0x796Fu, 0x796Fu, 0x796Fu, 0x796Eu,
    0x714Eu, 0x714Eu, 0x694Eu, 0x694Eu, 0x614Eu, 0x614Eu, 0x594Eu, 0x514Eu,
    0x512Eu, 0x492Eu, 0x412Eu, 0x412Eu, 0x392Du, 0x392Du, 0x312Du, 0x310Du,
    0x290Du, 0x290Du, 0x290Du, 0x210Du, 0x210Du, 0x210Du, 0x210Du, 0x210Du,
    0x210Du, 0x210Du, 0x210Cu, 0x210Cu, 0x210Cu, 0x190Cu, 0x190Bu, 0x190Bu,
    0x190Bu, 0x190Bu, 0x190Bu, 0x190Au, 0x190Au, 0x110Au, 0x110Au, 0x110Au,
    0x110Au, 0x110Au, 0x110Au, 0x110Au, 0x110Au, 0x110Au, 0x110Au, 0x112Bu,
    0x112Bu, 0x112Bu, 0x112Bu, 0x114Bu, 0x114Cu, 0x116Cu, 0x196Cu, 0x196Cu,
    0x198Du, 0x198Du, 0x198Du, 0x19ADu, 0x19AEu, 0x19AEu, 0x19CEu, 0x19CEu,
    0x19CFu, 0x19EFu, 0x19EFu, 0x19EFu, 0x19EFu, 0x19EFu, 0x1A0Fu, 0x1A10u,
    0x1A10u, 0x1A10u, 0x1A10u, 0x1A30u, 0x1A30u, 0x1A51u, 0x1A71u, 0x1A91u,
    0x1AB2u, 0x1AF2u, 0x1B13u, 0x1B54u, 0x1B74u, 0x1BB5u, 0x1BF5u, 0x1C36u,
    0x1C57u, 0x1C97u, 0x1CD8u, 0x1D19u, 0x1D39u, 0x257Au, 0x259Bu, 0x25DBu,
    0x25FCu, 0x263Cu, 0x265Du, 0x267Du, 0x269Du, 0x269Eu, 0x26BEu, 0x26BEu,
    0x26BEu, 0x26BEu, 0x26BEu, 0x26BEu, 0x26BEu, 0x2EDEu, 0x2EDEu, 0x2EDDu,
    0x36DDu, 0x36DDu, 0x36DDu, 0x3EFDu, 0x3EFDu, 0x46FCu, 0x46FCu, 0x4F1Cu,
    0x4F1Cu, 0x571Cu, 0x571Bu, 0x573Bu, 0x5F3Bu, 0x5F3Bu, 0x673Bu, 0x673Bu,
    0x673Bu, 0x673Au, 0x675Au, 0x6F5Au, 0x6F5Au, 0x6F5Au, 0x6F5Au, 0x775Au,
    0x7739u, 0x7F39u, 0x8738u, 0x8F37u, 0x9736u, 0xA736u, 0xAF35u, 0xB714u,
    0xBF13u, 0xCF12u, 0xD712u, 0xDF11u, 0xE710u, 0xE710u, 0xEF10u, 0xEF0Fu,
    0xEF0Fu, 0xEF0Fu, 0xEF0Fu, 0xF6EFu, 0xF6EEu, 0xF6EEu, 0xF6EDu, 0xF6EDu,
    0xF6ECu, 0xFECCu, 0xFECBu, 0xFECBu, 0xFECAu, 0xFECAu, 0xFEC9u, 0xFEC9u,
    0xFEC9u, 0xFEC9u, 0xFEA9u, 0xFEA9u, 0xFEA9u, 0xFE89u, 0xFE89u, 0xFE88u,
    0xFE68u, 0xFE48u, 0xFE48u, 0xFE28u, 0xFE07u, 0xFE07u, 0xFDE7u, 0xFDC7u,
    0xFDA7u, 0xFDA6u, 0xFD86u, 0xFD66u, 0xFD66u, 0xFD46u, 0xFD46u, 0xFD25u,
    0xFD25u, 0xFD25u, 0xFD05u, 0xFD05u, 0xFD05u, 0xFD05u, 0xFD05u, 0xFD05u,
    0xFCE5u, 0xFCE5u, 0xFCC5u, 0xFCA5u, 0xFCA5u, 0xFC85u, 0xFC65u, 0xFC65u,
    0xFC45u, 0xFC25u, 0xFC05u, 0xFC05u, 0xFBE5u, 0xFBE5u, 0xFBC5u, 0xFBC5u,
    0xFBA5u, 0xFBA5u, 0xFBA5u, 0xFBA5u, 0xF3A5u, 0xF385u, 0xF385u, 0xF365u,
    0xF365u, 0xF346u, 0xF326u, 0xEB26u, 0xEB06u, 0xEAE6u, 0xEAE6u, 0xEAE6u,
};

/* Normalize either jr_g3_wx_kind_t (0..7) or standard WMO weather codes. */
static jr_g3_wx_kind_t atmos_norm_wx(uint8_t code)
{
    if (code <= (uint8_t)JR_G3_WX_SNOW) {
        return (code == (uint8_t)JR_G3_WX_UNKNOWN) ? JR_G3_WX_CLEAR : (jr_g3_wx_kind_t)code;
    }
    if (code == 45u || code == 48u) {
        return JR_G3_WX_FOG;
    }
    if ((code >= 51u && code <= 67u) || (code >= 80u && code <= 82u)) {
        return JR_G3_WX_RAIN;
    }
    if ((code >= 71u && code <= 77u) || code == 85u || code == 86u) {
        return JR_G3_WX_SNOW;
    }
    if (code >= 95u) {
        return JR_G3_WX_STORM;
    }
    return JR_G3_WX_CLOUDS;
}

/* Smooth anti-aliased segment helper strictly clipped to emblem box & [y0, y1). */
static void atmos_draw_seg_box(uint16_t *dst, int y0, int y1, bool swap_bytes,
                               int32_t ax16, int32_t ay16,
                               int32_t bx16, int32_t by16,
                               int32_t half_w16, uint16_t col0, uint16_t col1,
                               uint8_t alpha, bool additive,
                               int clip_x0, int clip_x1, int clip_y0, int clip_y1)
{
    if (alpha == 0u || half_w16 <= 0) {
        return;
    }
    const int pad = (half_w16 >> 4) + 2;
    int ry0 = g3_max(g3_max((g3_min(ay16, by16) >> 4) - pad, clip_y0), y0);
    int ry1 = g3_min(g3_min((g3_max(ay16, by16) >> 4) + pad, clip_y1), y1 - 1);
    if (ry0 > ry1) {
        return;
    }
    int rx0 = g3_max((g3_min(ax16, bx16) >> 4) - pad, clip_x0);
    int rx1 = g3_min((g3_max(ax16, bx16) >> 4) + pad, clip_x1);
    if (rx0 > rx1) {
        return;
    }

    const int32_t dx = bx16 - ax16;
    const int32_t dy = by16 - ay16;
    const int32_t len2 = dx * dx + dy * dy;
    if (len2 <= 0) {
        return;
    }
    const int32_t outer = half_w16 + 12;

    for (int y = ry0; y <= ry1; ++y) {
        const int32_t py = y * 16 + 8 - ay16;
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;
        for (int x = rx0; x <= rx1; ++x) {
            const int32_t px = x * 16 + 8 - ax16;
            int32_t t_q8 = (int32_t)(((int64_t)(px * dx + py * dy) << 8) / len2);
            t_q8 = g3_clamp(t_q8, 0, 256);
            const int32_t cx = (dx * t_q8) >> 8;
            const int32_t cy = (dy * t_q8) >> 8;
            const int32_t ex = px - cx;
            const int32_t ey = py - cy;
            const int32_t d2 = ex * ex + ey * ey;
            if (d2 >= outer * outer) {
                continue;
            }
            const int32_t d = (int32_t)g3_isqrt((uint32_t)d2);
            int32_t cov = 255;
            if (d > half_w16 - 4) {
                cov = ((outer - d) * 255) / 16;
                cov = g3_clamp(cov, 0, 255);
            }
            if (cov <= 0) {
                continue;
            }
            const uint16_t col = g3_lerp565(col0, col1, t_q8 > 255 ? 255 : t_q8);
            const unsigned a = (unsigned)((cov * alpha) >> 8);
            if (a > 0u) {
                row[x] = additive ? g3_add565(row[x], col, a, swap_bytes)
                                  : g3_blend565(row[x], col, a, swap_bytes);
            }
        }
    }
}

/* Sculpted optical Sun sphere + 8 slowly rotating tapered rays, strictly
 * clipped inside the weather emblem box [248..344] x [148..232]. */
static void atmos_draw_sun_emblem(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                  int32_t cx16, int32_t cy16, int32_t r_px,
                                  uint32_t now_ms, int32_t strength)
{
    const int bx0 = JR_G3_ATMOS_EMBLEM_X0;
    const int bx1 = JR_G3_ATMOS_EMBLEM_X1;
    const int by0 = JR_G3_ATMOS_EMBLEM_Y0;
    const int by1 = JR_G3_ATMOS_EMBLEM_Y1;

    /* 1. Soft radial solar bloom */
    g3_glow_orb_clipped(dst, y0, y1, swap_bytes, cx16, cy16, r_px + 18,
                        0xFC80u, (uint8_t)((125 * strength) >> 8),
                        bx0, bx1, by0, by1, 190);
    g3_glow_orb_clipped(dst, y0, y1, swap_bytes, cx16, cy16, r_px + 8,
                        0xFEA4u, (uint8_t)((165 * strength) >> 8),
                        bx0, bx1, by0, by1, 190);

    /* 2. Eight crisp anti-aliased optical rays (slow solar drift) */
    const uint32_t spin = (now_ms * 2u) & 0xFFFFu;
    const int32_t r_in16 = (r_px + 4) * 16;
    const int32_t r_out16 = (r_px + 13) * 16;
    for (int k = 0; k < 8; ++k) {
        const uint32_t ang = (spin + (uint32_t)k * 8192u) & 0xFFFFu;
        const int32_t ca = g3_cos_q15(ang);
        const int32_t sa = g3_sin_q15(ang);
        const int32_t ax16 = cx16 + ((r_in16 * ca) >> 15);
        const int32_t ay16 = cy16 + ((r_in16 * sa) >> 15);
        const int32_t bx16 = cx16 + ((r_out16 * ca) >> 15);
        const int32_t by16 = cy16 + ((r_out16 * sa) >> 15);
        atmos_draw_seg_box(dst, y0, y1, swap_bytes,
                           ax16, ay16, bx16, by16, 18,
                           0xFFE8u, 0xFB20u,
                           (uint8_t)((210 * strength) >> 8), true,
                           bx0, bx1, by0, by1);
    }

    /* 3. Sculpted 3D optical sun sphere with warm top-left specular core */
    const int cx_i = cx16 >> 4;
    const int cy_i = cy16 >> 4;
    int ry0 = g3_max(g3_max(cy_i - r_px - 2, by0), y0);
    int ry1 = g3_min(g3_min(cy_i + r_px + 2, by1), y1 - 1);
    if (ry0 > ry1) {
        return;
    }
    int rx0 = g3_max(cx_i - r_px - 2, bx0);
    int rx1 = g3_min(cx_i + r_px + 2, bx1);
    const int32_t r16 = r_px * 16;
    const int32_t r_outer = r16 + 10;

    for (int y = ry0; y <= ry1; ++y) {
        const int32_t dy = y * 16 + 8 - cy16;
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;
        for (int x = rx0; x <= rx1; ++x) {
            const int32_t dx = x * 16 + 8 - cx16;
            const int32_t d2 = dx * dx + dy * dy;
            if (d2 >= r_outer * r_outer) {
                continue;
            }
            const int32_t d = (int32_t)g3_isqrt((uint32_t)d2);
            int32_t cov = 255;
            if (d > r16 - 6) {
                cov = g3_clamp(((r_outer - d) * 255) / 16, 0, 255);
            }
            if (cov <= 0) {
                continue;
            }
            /* Offset highlight center toward top-left for 3D optical glass feel */
            const int32_t hx = dx + (r16 / 3);
            const int32_t hy = dy + (r16 / 3);
            const int32_t hd = (int32_t)g3_isqrt((uint32_t)(hx * hx + hy * hy));
            const int32_t ht = g3_clamp((hd * 255) / (r16 + (r16 >> 1)), 0, 255);
            uint16_t col;
            if (ht < 110) {
                col = g3_lerp565(0xFFF6u, 0xFDE6u, (ht * 255) / 110);
            } else {
                col = g3_lerp565(0xFDE6u, 0xEB42u, ((ht - 110) * 255) / 145);
            }
            const unsigned a = (unsigned)((cov * strength) >> 8);
            row[x] = g3_blend565(row[x], col, a, swap_bytes);
        }
    }
}

/* Glowing sculpted crescent moon + 2 crisp twinkling stars inside emblem box. */
static void atmos_draw_moon_emblem(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                   int32_t cx16, int32_t cy16, int32_t r_px,
                                   uint32_t now_ms, int32_t strength)
{
    const int bx0 = JR_G3_ATMOS_EMBLEM_X0;
    const int bx1 = JR_G3_ATMOS_EMBLEM_X1;
    const int by0 = JR_G3_ATMOS_EMBLEM_Y0;
    const int by1 = JR_G3_ATMOS_EMBLEM_Y1;

    g3_glow_orb_clipped(dst, y0, y1, swap_bytes, cx16 - 48, cy16 + 32, r_px + 14,
                        0x4C7Du, (uint8_t)((105 * strength) >> 8),
                        bx0, bx1, by0, by1, 190);

    const int cx_i = cx16 >> 4;
    const int cy_i = cy16 >> 4;
    int ry0 = g3_max(g3_max(cy_i - r_px - 2, by0), y0);
    int ry1 = g3_min(g3_min(cy_i + r_px + 2, by1), y1 - 1);
    const int rx0 = g3_max(cx_i - r_px - 2, bx0);
    const int rx1 = g3_min(cx_i + r_px + 2, bx1);

    const int32_t r16 = r_px * 16;
    const int32_t cut_cx = cx16 + (r16 * 12) / 24;
    const int32_t cut_cy = cy16 - (r16 * 8) / 24;
    const int32_t cut_r = (r16 * 20) / 24;

    if (ry0 <= ry1) {
        for (int y = ry0; y <= ry1; ++y) {
            const int32_t dy = y * 16 + 8 - cy16;
            const int32_t cdy = y * 16 + 8 - cut_cy;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;
            for (int x = rx0; x <= rx1; ++x) {
                const int32_t dx = x * 16 + 8 - cx16;
                const int32_t d2 = dx * dx + dy * dy;
                if (d2 >= (r16 + 12) * (r16 + 12)) {
                    continue;
                }
                const int32_t d_out = (int32_t)g3_isqrt((uint32_t)d2) - r16;
                const int32_t cdx = x * 16 + 8 - cut_cx;
                const int32_t d_cut = cut_r - (int32_t)g3_isqrt((uint32_t)(cdx * cdx + cdy * cdy));
                /* Crescent SDF = max(d_out, d_cut) */
                const int32_t sd = g3_max(d_out, d_cut);
                if (sd >= 12) {
                    continue;
                }
                const int32_t cov = g3_clamp(((12 - sd) * 255) / 20, 0, 255);
                if (cov <= 0) {
                    continue;
                }
                /* Silvery luminous rim near outer crescent edge */
                const int32_t rim_t = g3_clamp(((-sd) * 255) / (r16 / 2 + 1), 0, 255);
                const uint16_t col = g3_lerp565(0xF7FFu, 0x8DBCu, rim_t);
                const unsigned a = (unsigned)((cov * strength) >> 8);
                row[x] = g3_blend565(row[x], col, a, swap_bytes);
            }
        }
    }

    /* Two delicate twinkling stars in upper-right nook of the crescent */
    const int32_t tw1 = 165 + ((g3_sin_q15((now_ms * 18u) & 0xFFFFu) * 70) >> 15);
    const int32_t tw2 = 150 + ((g3_sin_q15((now_ms * 25u + 22000u) & 0xFFFFu) * 75) >> 15);
    g3_glow_orb_clipped(dst, y0, y1, swap_bytes,
                        cx16 + (r16 * 14) / 20, cy16 - (r16 * 13) / 20, 4,
                        0xD7FFu, (uint8_t)((tw1 * strength) >> 8),
                        bx0, bx1, by0, by1, 190);
    g3_glow_orb_clipped(dst, y0, y1, swap_bytes,
                        cx16 + (r16 * 6) / 20, cy16 - (r16 * 18) / 20, 3,
                        0xBDFEu, (uint8_t)((tw2 * strength) >> 8),
                        bx0, bx1, by0, by1, 190);
}

/* Smooth union SDF of a sculpted modern frosted-glass cloud centered near
 * (cx16, cy16) scaled by `scale_q8` (256 = full size, ~66x36 px). */
static int32_t atmos_cloud_sdf(int32_t px16, int32_t py16,
                               int32_t cx16, int32_t cy16, int32_t scale_q8)
{
    /* Map pixel into normalized cloud local coords (Q4 px at scale 256) */
    const int32_t lx = ((px16 - cx16) * 256) / scale_q8;
    const int32_t ly = ((py16 - cy16) * 256) / scale_q8;

    /* 3 overlapping upper domes + rounded flat base pill */
    const int32_t d1 = (int32_t)g3_isqrt((uint32_t)((lx + 176) * (lx + 176) + (ly - 32) * (ly - 32))) - 224;
    const int32_t d2 = (int32_t)g3_isqrt((uint32_t)((lx - 16) * (lx - 16) + (ly + 96) * (ly + 96))) - 288;
    const int32_t d3 = (int32_t)g3_isqrt((uint32_t)((lx - 208) * (lx - 208) + (ly - 16) * (ly - 16))) - 208;
    const int32_t d4 = g3_sd_rrect16(lx, ly, 0, 112, 392, 112, 112);

    int32_t d = g3_min(g3_min(d1, d2), g3_min(d3, d4));
    return (d * scale_q8) >> 8;
}

static void atmos_draw_frosted_cloud(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                     int32_t cx16, int32_t cy16, int32_t scale_q8,
                                     uint16_t top_col, uint16_t bot_col, uint16_t rim_col,
                                     uint8_t opacity, int32_t strength)
{
    const int bx0 = JR_G3_ATMOS_EMBLEM_X0;
    const int bx1 = JR_G3_ATMOS_EMBLEM_X1;
    const int by0 = JR_G3_ATMOS_EMBLEM_Y0;
    const int by1 = JR_G3_ATMOS_EMBLEM_Y1;

    const int half_w = (36 * scale_q8) >> 8;
    const int half_h = (26 * scale_q8) >> 8;
    const int cx_i = cx16 >> 4;
    const int cy_i = cy16 >> 4;
    int ry0 = g3_max(g3_max(cy_i - half_h, by0), y0);
    int ry1 = g3_min(g3_min(cy_i + half_h, by1), y1 - 1);
    if (ry0 > ry1) {
        return;
    }
    int rx0 = g3_max(cx_i - half_w, bx0);
    int rx1 = g3_min(cx_i + half_w, bx1);

    const int32_t y_top16 = cy16 - ((320 * scale_q8) >> 8);
    const int32_t y_span16 = g3_max((540 * scale_q8) >> 8, 16);

    for (int y = ry0; y <= ry1; ++y) {
        const int32_t py16 = y * 16 + 8;
        const int32_t vt = g3_clamp(((py16 - y_top16) * 255) / y_span16, 0, 255);
        const uint16_t body_col = g3_lerp565(top_col, bot_col, vt);
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;
        for (int x = rx0; x <= rx1; ++x) {
            const int32_t px16 = x * 16 + 8;
            const int32_t sd = atmos_cloud_sdf(px16, py16, cx16, cy16, scale_q8);
            if (sd >= 12) {
                continue;
            }
            const int32_t cov = g3_clamp(((12 - sd) * 255) / 20, 0, 255);
            if (cov <= 0) {
                continue;
            }
            /* Subtle optical frosted-glass specular top rim highlight */
            uint16_t col = body_col;
            if (sd > -24 && vt < 145) {
                const int32_t rim_f = ((24 + sd) * (145 - vt)) / (24 * 145 / 255 + 1);
                col = g3_lerp565(body_col, rim_col, g3_clamp(rim_f, 0, 220));
            }
            const unsigned a = (unsigned)((cov * opacity * strength) >> 16);
            row[x] = g3_blend565(row[x], col, a, swap_bytes);
        }
    }
}

/* Complete sculpted translucent weather emblem strictly inside
 * x in [248, 344], y in [148, 232]. */
static void atmos_draw_emblem_rows(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                   const jr_g3_atmosphaera_t *env, int32_t strength)
{
    if (y1 <= JR_G3_ATMOS_EMBLEM_Y0 || y0 > JR_G3_ATMOS_EMBLEM_Y1) {
        return;
    }
    const jr_g3_wx_kind_t wx = atmos_norm_wx(env->weather_code);
    const int bx0 = JR_G3_ATMOS_EMBLEM_X0;
    const int bx1 = JR_G3_ATMOS_EMBLEM_X1;
    const int by0 = JR_G3_ATMOS_EMBLEM_Y0;
    const int by1 = JR_G3_ATMOS_EMBLEM_Y1;
    const int32_t ecx16 = 296 * 16; /* center of [248..344] */
    const int32_t ecy16 = 190 * 16; /* center of [148..232] */

    switch (wx) {
    case JR_G3_WX_CLEAR:
    case JR_G3_WX_UNKNOWN:
    default:
        if (env->is_day) {
            atmos_draw_sun_emblem(dst, y0, y1, swap_bytes,
                                  ecx16, ecy16, 20, env->now_ms, strength);
        } else {
            atmos_draw_moon_emblem(dst, y0, y1, swap_bytes,
                                   ecx16, ecy16, 21, env->now_ms, strength);
        }
        break;

    case JR_G3_WX_PARTLY:
        /* Sun or crescent moon peeking cleanly behind upper-right of cloud */
        if (env->is_day) {
            atmos_draw_sun_emblem(dst, y0, y1, swap_bytes,
                                  ecx16 + 15 * 16, ecy16 - 12 * 16, 14,
                                  env->now_ms, strength);
        } else {
            atmos_draw_moon_emblem(dst, y0, y1, swap_bytes,
                                   ecx16 + 14 * 16, ecy16 - 12 * 16, 15,
                                   env->now_ms, strength);
        }
        atmos_draw_frosted_cloud(dst, y0, y1, swap_bytes,
                                 ecx16 - 5 * 16, ecy16 + 8 * 16, 232,
                                 env->is_day ? 0xEFFFu : 0xAD5Au,
                                 env->is_day ? 0x8CB7u : 0x4A8Fu,
                                 0xFFFFu, 224u, strength);
        break;

    case JR_G3_WX_CLOUDS:
        /* Layered dual translucent frosted clouds */
        atmos_draw_frosted_cloud(dst, y0, y1, swap_bytes,
                                 ecx16 + 11 * 16, ecy16 - 8 * 16, 198,
                                 env->is_day ? 0xAD7Au : 0x73B5u,
                                 env->is_day ? 0x5B32u : 0x31CCu,
                                 0xDEFDu, 175u, strength);
        atmos_draw_frosted_cloud(dst, y0, y1, swap_bytes,
                                 ecx16 - 5 * 16, ecy16 + 6 * 16, 248,
                                 env->is_day ? 0xEFFFu : 0xB5DBu,
                                 env->is_day ? 0x7BF5u : 0x4A8Fu,
                                 0xFFFFu, 228u, strength);
        break;

    case JR_G3_WX_FOG: {
        /* Frosted cloud + 3 drifting translucent atmospheric mist bars */
        atmos_draw_frosted_cloud(dst, y0, y1, swap_bytes,
                                 ecx16, ecy16 - 10 * 16, 224,
                                 0xD6FDu, 0x6B93u, 0xF7FFu, 210u, strength);
        const int32_t drift = (g3_sin_q15((env->now_ms * 10u) & 0xFFFFu) * 48) >> 15;
        for (int i = 0; i < 3; ++i) {
            const int32_t my16 = (206 + i * 9) * 16;
            const int32_t d_off = (i & 1) ? -drift : drift;
            const int32_t hw16 = (28 - i * 4) * 16;
            atmos_draw_seg_box(dst, y0, y1, swap_bytes,
                               ecx16 - hw16 + d_off, my16,
                               ecx16 + hw16 + d_off, my16, 20,
                               0x6B94u, 0xCEFDu,
                               (uint8_t)(((170 - i * 28) * strength) >> 8), false,
                               bx0, bx1, by0, by1);
        }
        break;
    }

    case JR_G3_WX_RAIN:
    case JR_G3_WX_STORM: {
        const bool storm = (wx == JR_G3_WX_STORM);
        /* Crisp slanted animated rain streaks emerging below the cloud */
        const int n_drops = storm ? 4 : 5;
        const int32_t phase_base = (int32_t)((env->now_ms / 18u) % 24u);
        for (int i = 0; i < n_drops; ++i) {
            const int32_t drop_phase = (phase_base + i * 9) % 24;
            const int32_t sx16 = (268 + i * 14) * 16 - (drop_phase * 5);
            const int32_t sy16 = 202 * 16 + (drop_phase * 11);
            const int32_t ex16 = sx16 - 4 * 16;
            const int32_t ey16 = sy16 + 11 * 16;
            const uint8_t da = (uint8_t)(((170 + (env->rain_pct >> 1)) * strength) >> 8);
            atmos_draw_seg_box(dst, y0, y1, swap_bytes,
                               sx16, sy16, ex16, ey16, 14,
                               0x2559u, 0x7FFFu,
                               da > 240u ? 240u : da, true,
                               bx0, bx1, by0, by1);
        }
        /* Sculpted rain/storm cloud over the top of the rain streaks */
        atmos_draw_frosted_cloud(dst, y0, y1, swap_bytes,
                                 ecx16 + 8 * 16, ecy16 - 14 * 16, 186,
                                 storm ? 0x6B74u : 0x94D8u,
                                 storm ? 0x296Au : 0x424Eu,
                                 0xBDD9u, 185u, strength);
        atmos_draw_frosted_cloud(dst, y0, y1, swap_bytes,
                                 ecx16 - 3 * 16, ecy16 - 6 * 16, 232,
                                 storm ? 0x9CF9u : 0xDEFDu,
                                 storm ? 0x39EBu : 0x5B13u,
                                 0xEFFFu, 232u, strength);
        if (storm) {
            /* Crisp glowing amber-gold lightning bolt beneath storm cloud */
            const int32_t pulse = 195 + ((g3_sin_q15((env->now_ms * 48u) & 0xFFFFu) * 55) >> 15);
            const uint8_t la = (uint8_t)((pulse * strength) >> 8);
            atmos_draw_seg_box(dst, y0, y1, swap_bytes,
                               298 * 16, 200 * 16, 290 * 16, 214 * 16, 18,
                               0xFFF0u, 0xFD60u, la, true, bx0, bx1, by0, by1);
            atmos_draw_seg_box(dst, y0, y1, swap_bytes,
                               290 * 16, 214 * 16, 299 * 16, 214 * 16, 16,
                               0xFFE8u, 0xFD60u, la, true, bx0, bx1, by0, by1);
            atmos_draw_seg_box(dst, y0, y1, swap_bytes,
                               299 * 16, 214 * 16, 289 * 16, 229 * 16, 16,
                               0xFFE8u, 0xFA80u, la, true, bx0, bx1, by0, by1);
        }
        break;
    }

    case JR_G3_WX_SNOW: {
        /* Frosted cloud + 5 softly drifting ice-white snow crystals */
        atmos_draw_frosted_cloud(dst, y0, y1, swap_bytes,
                                 ecx16, ecy16 - 8 * 16, 232,
                                 0xF7FFu, 0x8CF8u, 0xFFFFu, 226u, strength);
        const int32_t phase_base = (int32_t)((env->now_ms / 32u) % 24u);
        for (int i = 0; i < 5; ++i) {
            const int32_t p = (phase_base + i * 7) % 24;
            const int32_t sway = (g3_sin_q15(((uint32_t)p * 2730u + (uint32_t)i * 13000u) & 0xFFFFu) * 40) >> 15;
            const int32_t fx16 = (266 + i * 15) * 16 + sway;
            const int32_t fy16 = (204 + p) * 16;
            g3_glow_orb_clipped(dst, y0, y1, swap_bytes, fx16, fy16, 4,
                                0xBFFFu, (uint8_t)((185 * strength) >> 8),
                                bx0, bx1, by0, by1, 190);
            g3_glow_orb_clipped(dst, y0, y1, swap_bytes, fx16, fy16, 2,
                                0xFFFFu, (uint8_t)((240 * strength) >> 8),
                                bx0, bx1, by0, by1, 190);
        }
        break;
    }
    }
}

void jr_g3_atmosphaera_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                            const jr_g3_atmosphaera_t *env)
{
    if (dst == NULL || env == NULL || nrows <= 0 || env->strength == 0u) {
        return;
    }
    int ry0 = y0 < 0 ? 0 : y0;
    int ry1 = y0 + nrows;
    if (ry1 > G3_H) {
        ry1 = G3_H;
    }
    if (ry0 >= ry1) {
        return;
    }

    const int32_t strength = (int32_t)env->strength;
    const uint32_t hour = (uint32_t)(env->hour % 24u);
    const uint32_t minute = (uint32_t)(env->minute % 60u);
    const uint32_t day_min = hour * 60u + minute;
    /* 6A(360m)=32768(9 o'clock), 12P(720m)=49152(12 o'clock),
     * 6P(1080m)=0(3 o'clock),    12A(0m)=16384(6 o'clock). */
    const uint32_t sun_ang_q16 = (16384u + ((day_min * 65536u + 720u) / 1440u)) & 0xFFFFu;

    /* Horizon palette adapts to day/night & temperature warmth */
    const bool warm_sky = env->is_day && (env->temp_f >= 68);
    const uint16_t hor_crest_col = warm_sky ? 0xFEEFu : (env->is_day ? 0xA7FFu : 0x66BFu);
    const uint16_t hor_bloom_col = warm_sky ? 0xD465u : (env->is_day ? 0x1D5Du : 0x1A96u);
    const uint16_t hor_upper_col = warm_sky ? 0xF5E6u : (env->is_day ? 0x259Du : 0x1A74u);

    /* Radial constants in Q4 px */
    const int32_t bowl_r16    = JR_G3_ATMOS_BOWL_R * 16;    /* 3040 */
    const int32_t arc_in16    = JR_G3_ATMOS_ARC_R_IN * 16;  /* 3040 */
    const int32_t arc_out16   = JR_G3_ATMOS_ARC_R_OUT * 16; /* 3264 */
    const int32_t arc_mid16   = (arc_in16 + arc_out16) >> 1;/* 3152 */
    const int32_t tick_in16   = JR_G3_ATMOS_TICK_R_IN * 16; /* 3328 */
    const int32_t tick_out16  = JR_G3_ATMOS_TICK_R_OUT * 16;/* 3584 */
    const int32_t r_max16     = G3_R_MAX * 16;              /* 3712 */
    const int32_t bowl_r16_sq = bowl_r16 * bowl_r16;
    const int32_t r_max16_sq  = r_max16 * r_max16;

    /* Planetary horizon circle: center (232.5, 620), crest radius R = 350 px
     * -> peak crest sits at y = 270, upper limb glow clears y <= 265 strictly! */
    const int32_t hor_cy16 = 620 * 16;
    const int32_t hor_r16  = 350 * 16;

    /* ---- PASS 1: Atmospheric Bowl + Horizon Arc + 360-deg Thermal Arc + Ticks ---- */
    for (int y = ry0; y < ry1; ++y) {
        const int32_t dy16 = y * 16 + 8 - G3_CY16;
        const int32_t dy_sq = dy16 * dy16;
        if (dy_sq >= r_max16_sq) {
            continue;
        }
        const int32_t chord16 = (int32_t)g3_isqrt((uint32_t)(r_max16_sq - dy_sq));
        int xlo = (G3_CX16 - chord16 - 8) >> 4;
        int xhi = (G3_CX16 + chord16 + 8) >> 4;
        if (xlo < 0) { xlo = 0; }
        if (xhi >= G3_W) { xhi = G3_W - 1; }

        const bool in_horizon_y = (y >= JR_G3_ATMOS_HORIZON_Y_MIN);
        const int32_t hdy16 = (y * 16 + 8) - hor_cy16;
        const int32_t hdy_sq = hdy16 * hdy16;

        uint16_t *row = dst + (size_t)(y - y0) * G3_W;
        for (int x = xlo; x <= xhi; ++x) {
            const int32_t dx16 = x * 16 + 8 - G3_CX16;
            const int32_t d2 = dx16 * dx16 + dy_sq;
            if (d2 > r_max16_sq) {
                continue;
            }

            uint16_t px_col = 0x0000u; /* pure OLED true black rim default */

            if (d2 < bowl_r16_sq) {
                /* Deep midnight-indigo radial atmospheric bowl fading to pure black at r=190 */
                const int32_t fall = (int32_t)(((int64_t)(bowl_r16_sq - d2) * 256) / bowl_r16_sq);
                /* Gentle upper zenith atmospheric richness */
                const int32_t zenith = g3_clamp(256 - ((dy16 + bowl_r16) * 90) / (2 * bowl_r16), 170, 265);
                const int32_t fz = (fall * zenith) >> 8;

                int32_t br, bg, bb;
                if (env->is_day) {
                    br = (12 * fz) >> 8;
                    bg = (26 * fz) >> 8;
                    bb = (60 * fz) >> 8;
                } else {
                    br = (8 * fz) >> 8;
                    bg = (16 * fz) >> 8;
                    bb = (44 * fz) >> 8;
                }
                px_col = g3_rgb565_dither(br, bg, bb, x, y);

                /* Illuminated Curved Planetary Horizon Arc strictly for y >= 266 */
                if (in_horizon_y && d2 < (188 * 16) * (188 * 16)) {
                    const int32_t hd = (int32_t)g3_isqrt((uint32_t)(dx16 * dx16 + hdy_sq));
                    const int32_t sd = hd - hor_r16; /* >0 above crest, <0 below crest */
                    /* Smooth lateral fade approaching the inner thermal ring (r = 166..188) */
                    int32_t edge_f = 255;
                    if (d2 > (166 * 16) * (166 * 16)) {
                        const int32_t r16 = (int32_t)g3_isqrt((uint32_t)d2);
                        edge_f = g3_clamp(((188 * 16 - r16) * 255) / (22 * 16), 0, 255);
                    }
                    if (sd >= -544 && sd <= 80) {
                        /* Continuous 3-part planetary horizon (zero seam between crest & bloom):
                         *   1. Upper atmospheric limb scatter (sd in [0, 80], y >= 266)
                         *   2. Soft lower atmospheric bloom (sd in [-544, 0])
                         *   3. Razor-sharp luminous horizon crest centered at sd = 0 */
                        if (sd > 0) {
                            const int32_t tu = g3_clamp(((80 - sd) * 255) / 80, 0, 255);
                            const unsigned au = (unsigned)((tu * tu * 105 * edge_f) >> 24);
                            px_col = g3_add565(px_col, hor_upper_col, au, false);
                        } else {
                            const int32_t tb = g3_clamp(((544 + sd) * 255) / 544, 0, 255);
                            const unsigned ab = (unsigned)((tb * tb * 155 * edge_f) >> 24);
                            px_col = g3_add565(px_col, hor_bloom_col, ab, false);
                        }
                        const int32_t dist_crest = g3_abs(sd);
                        if (dist_crest < 30) {
                            const int32_t cf = ((30 - dist_crest) * 255) / 30;
                            const unsigned ac = (unsigned)((cf * cf * 245 * edge_f) >> 24);
                            px_col = g3_add565(px_col, hor_crest_col, ac, false);
                        }
                    } else if (sd < -544) {
                        /* Deep quiet obsidian planet floor under bottom micro-stats */
                        const int32_t dark_t = g3_clamp(((-sd - 544) * 255) / 240, 0, 255);
                        px_col = g3_blend565(px_col, 0x0043u, (unsigned)((dark_t * 170) >> 8), false);
                    }
                }
            }

            /* Outer instrument annulus: Thermal Arc (r in [190, 204]) & Micro-Ticks (r in [208, 224]) */
            if (d2 >= (arc_in16 - 16) * (arc_in16 - 16) && d2 <= (tick_out16 + 16) * (tick_out16 + 16)) {
                const int32_t r16 = (int32_t)g3_isqrt((uint32_t)d2);
                const uint32_t ang = g3_atan2_q16(dy16, dx16);

                if (r16 >= arc_in16 - 12 && r16 <= arc_out16 + 12) {
                    /* 360-deg Continuous Thermal/Solar Perimeter Arc */
                    int32_t rcov = 255;
                    if (r16 < arc_in16 + 4) {
                        rcov = g3_clamp(((r16 - (arc_in16 - 12)) * 255) / 16, 0, 255);
                    } else if (r16 > arc_out16 - 4) {
                        rcov = g3_clamp(((arc_out16 + 12 - r16) * 255) / 16, 0, 255);
                    }
                    if (rcov > 0) {
                        /* Smooth 256-entry thermal LUT interpolation */
                        const uint32_t idx0 = (ang >> 8) & 0xFFu;
                        const uint32_t idx1 = (idx0 + 1u) & 0xFFu;
                        uint16_t arc_col = g3_lerp565(s_atmos_lut[idx0], s_atmos_lut[idx1],
                                                      (int32_t)(ang & 0xFFu));

                        /* Sculpted cylindrical glass-conduit bevel across radial width */
                        const int32_t dr_mid = g3_abs(r16 - arc_mid16); /* 0..112 */
                        const int32_t tube_k = 256 - ((dr_mid * dr_mid * 96) / (112 * 112));
                        arc_col = g3_scale565(arc_col, tube_k);

                        /* Subtle luminous boost within +-34 deg of the live Sun Jewel */
                        int32_t da = (int32_t)((ang - sun_ang_q16) & 0xFFFFu);
                        if (da > 32768) { da -= 65536; }
                        const int32_t ada = g3_abs(da);
                        if (ada < 6200) {
                            const int32_t boost = ((6200 - ada) * (6200 - ada)) / (6200 * 6200 / 115);
                            arc_col = g3_add565(arc_col, 0xFEA6u, (unsigned)boost, false);
                        }
                        px_col = g3_blend565(px_col, arc_col, (unsigned)rcov, false);
                    }
                } else if (r16 >= tick_in16 - 12 && r16 <= tick_out16 + 12) {
                    /* 48 Outer Bezel Micro-Ticks (4 major cardinals at 6A/12P/6P/12A) */
                    const uint32_t scaled = ang * 48u + 32768u;
                    const uint32_t k = (scaled >> 16) % 48u;
                    const int32_t frac = (int32_t)(scaled & 0xFFFFu) - 32768;
                    /* Tangential distance to tick centerline in Q4 px */
                    const int32_t dt16 = (int32_t)(((int64_t)g3_abs(frac) * (r16 >> 4)) / 31291);

                    int32_t t_rin, t_rout, t_hw;
                    uint16_t t_col;
                    if ((k % 12u) == 0u) {
                        /* Major cardinal ticks: 6A(24), 12P(36), 6P(0), 12A(12) */
                        t_rin = 208 * 16;
                        t_rout = 224 * 16;
                        t_hw = 19;
                        t_col = 0xFF77u; /* luminous champagne-ivory */
                    } else if ((k % 6u) == 0u) {
                        /* Intermediate 45-deg ticks: 3A, 9A, 3P, 9P */
                        t_rin = 209 * 16;
                        t_rout = 220 * 16;
                        t_hw = 14;
                        t_col = 0x9D59u; /* brushed silver-cyan */
                    } else {
                        /* Fine 7.5-deg micro-ticks */
                        t_rin = 210 * 16;
                        t_rout = 217 * 16;
                        t_hw = 11;
                        t_col = 0x4AAEu; /* machined slate-titanium */
                    }

                    if (dt16 < t_hw + 8 && r16 >= t_rin - 8 && r16 <= t_rout + 8) {
                        int32_t tcov = (dt16 <= t_hw - 4) ? 255
                                       : g3_clamp(((t_hw + 8 - dt16) * 255) / 12, 0, 255);
                        if (r16 < t_rin + 4) {
                            tcov = (tcov * g3_clamp(((r16 - (t_rin - 8)) * 255) / 12, 0, 255)) >> 8;
                        } else if (r16 > t_rout - 4) {
                            tcov = (tcov * g3_clamp(((t_rout + 8 - r16) * 255) / 12, 0, 255)) >> 8;
                        }
                        if (tcov > 0) {
                            px_col = g3_blend565(px_col, t_col, (unsigned)tcov, false);
                        }
                    }
                }
            }

            if (strength >= 255) {
                row[x] = swap_bytes ? g3_bswap(px_col) : px_col;
            } else {
                row[x] = g3_blend565(row[x], px_col, (unsigned)strength, swap_bytes);
            }
        }
    }

    /* ---- PASS 2: Live Glowing Golden Sun Jewel on the 24h perimeter arc ---- */
    {
        const int32_t sx16 = G3_CX16 + ((arc_mid16 * g3_cos_q15(sun_ang_q16)) >> 15);
        const int32_t sy16 = G3_CY16 + ((arc_mid16 * g3_sin_q15(sun_ang_q16)) >> 15);
        const int sy_i = sy16 >> 4;
        if (sy_i + 24 >= ry0 && sy_i - 24 < ry1) {
            /* Subtle dark recess ring behind jewel for crisp daylight contrast */
            const int sx_i = sx16 >> 4;
            int jy0 = g3_max(sy_i - 10, ry0);
            int jy1 = g3_min(sy_i + 10, ry1 - 1);
            int jx0 = g3_max(sx_i - 10, 0);
            int jx1 = g3_min(sx_i + 10, G3_W - 1);

            /* Warm additive outer radial halo */
            g3_glow_orb_clipped(dst, y0, ry1, swap_bytes, sx16, sy16, 22,
                                0xFD00u, (uint8_t)((160 * strength) >> 8),
                                0, G3_W - 1, 0, G3_H - 1, G3_R_MAX);
            g3_glow_orb_clipped(dst, y0, ry1, swap_bytes, sx16, sy16, 12,
                                0xFEA4u, (uint8_t)((215 * strength) >> 8),
                                0, G3_W - 1, 0, G3_H - 1, G3_R_MAX);

            /* Sculpted golden jewel core (r = 6.5 px) */
            const int32_t jr16 = 104; /* 6.5 px */
            for (int y = jy0; y <= jy1; ++y) {
                const int32_t dy = y * 16 + 8 - sy16;
                uint16_t *row = dst + (size_t)(y - y0) * G3_W;
                for (int x = jx0; x <= jx1; ++x) {
                    const int32_t dx = x * 16 + 8 - sx16;
                    const int32_t d2 = dx * dx + dy * dy;
                    if (d2 >= (jr16 + 12) * (jr16 + 12)) {
                        continue;
                    }
                    const int32_t d = (int32_t)g3_isqrt((uint32_t)d2);
                    const int32_t cov = (d <= jr16 - 4) ? 255
                                        : g3_clamp(((jr16 + 12 - d) * 255) / 16, 0, 255);
                    if (cov <= 0) {
                        continue;
                    }
                    const int32_t t = g3_clamp((d * 255) / jr16, 0, 255);
                    const uint16_t jcol = g3_lerp565(0xFFFBu, 0xFC80u, t);
                    row[x] = g3_blend565(row[x], jcol, (unsigned)((cov * strength) >> 8), swap_bytes);
                }
            }
        }
    }

    /* ---- PASS 3: Sculpted Translucent Weather Emblem [248..344] x [148..232] ---- */
    atmos_draw_emblem_rows(dst, y0, ry1, swap_bytes, env, strength);
}

/* ==========================================================================
 * 2. CORTEX COMMAND — STATUS + CODEX DESK AEROSPACE TELEMETRY DECK
 * ========================================================================== */

typedef struct {
    uint16_t start_q16;
    uint16_t span_q16;
    uint16_t lit_core;
    uint16_t lit_hi;
    uint16_t glow_col;
    uint16_t unlit_fill;
    uint16_t unlit_rim;
    uint16_t tick_col;
} cortex_quad_t;

/* 4 quadrant gauges:
 *   Q0 Top-Left     200..255 deg: Emerald MEM
 *   Q1 Top-Right    285..340 deg: Electric Cyan CPU
 *   Q2 Bottom-Right  20.. 75 deg: Warm Amber PWR (pulses emerald if charging)
 *   Q3 Bottom-Left  105..160 deg: Electric Violet AGENT */
static const cortex_quad_t s_cortex_quads[4] = {
    { 36409u, 10012u, 0x1F51u, 0x8FF9u, 0x170Eu, 0x08E2u, 0x1206u, 0x1B8Au }, /* MEM   */
    { 51883u, 10012u, 0x0EFFu, 0xA7FFu, 0x059Du, 0x08C4u, 0x11EAu, 0x1B6Du }, /* CPU   */
    {  3641u, 10012u, 0xFD23u, 0xFF31u, 0xD3A1u, 0x20A1u, 0x4961u, 0x6A63u }, /* PWR   */
    { 19115u, 10012u, 0xAA9Fu, 0xE5DFu, 0x71DCu, 0x1865u, 0x38EBu, 0x5990u }, /* AGENT */
};

static void cortex_led_palette(uint8_t kind, uint16_t *core, uint16_t *halo, bool *lit)
{
    switch ((jr_g3_led_color_t)kind) {
    case JR_G3_LED_GREEN:
        *core = 0x1F51u; *halo = 0x170Eu; *lit = true;  break;
    case JR_G3_LED_CYAN:
        *core = 0x1F3Fu; *halo = 0x05DFu; *lit = true;  break;
    case JR_G3_LED_AMBER:
        *core = 0xFD64u; *halo = 0xD3A1u; *lit = true;  break;
    case JR_G3_LED_VIOLET:
        *core = 0xB31Fu; *halo = 0x71DCu; *lit = true;  break;
    case JR_G3_LED_RED:
        *core = 0xF9E9u; *halo = 0xB0C3u; *lit = true;  break;
    case JR_G3_LED_OFF:
    default:
        *core = 0x1947u; *halo = 0x0000u; *lit = false; break;
    }
}

/* Render the centered translucent glass mission card, header pill bar,
 * horizontal row hairlines, and 3 glowing status LED dots. */
static void cortex_draw_card_rows(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                  const jr_g3_cortex_t *env, int32_t strength)
{
    const int pad = 5;
    int ry0 = g3_max(JR_G3_CORTEX_CARD_Y0 - pad, y0);
    int ry1 = g3_min(JR_G3_CORTEX_CARD_Y1 + pad, y1 - 1);
    if (ry0 > ry1) {
        return;
    }
    const int rx0 = JR_G3_CORTEX_CARD_X0 - pad;
    const int rx1 = JR_G3_CORTEX_CARD_X1 + pad;

    const int32_t card_cx16 = (JR_G3_CORTEX_CARD_X0 + JR_G3_CORTEX_CARD_X1) * 8; /* 233*16 */
    const int32_t card_cy16 = (JR_G3_CORTEX_CARD_Y0 + JR_G3_CORTEX_CARD_Y1) * 8; /* 220*16 */
    const int32_t card_hx16 = (JR_G3_CORTEX_CARD_X1 - JR_G3_CORTEX_CARD_X0) * 8; /* 129*16 */
    const int32_t card_hy16 = (JR_G3_CORTEX_CARD_Y1 - JR_G3_CORTEX_CARD_Y0) * 8; /*  84*16 */
    const int32_t card_rad16 = 16 * 16;

    const int32_t hdr_cx16 = (JR_G3_CORTEX_HDR_X0 + JR_G3_CORTEX_HDR_X1) * 8;
    const int32_t hdr_cy16 = (JR_G3_CORTEX_HDR_Y0 + JR_G3_CORTEX_HDR_Y1) * 8;
    const int32_t hdr_hx16 = (JR_G3_CORTEX_HDR_X1 - JR_G3_CORTEX_HDR_X0) * 8;
    const int32_t hdr_hy16 = (JR_G3_CORTEX_HDR_Y1 - JR_G3_CORTEX_HDR_Y0) * 8;
    const int32_t hdr_rad16 = 10 * 16;

    const uint16_t bdr_left  = env->codex_active ? 0x267Eu : 0x25DCu; /* electric cyan */
    const uint16_t bdr_right = env->codex_active ? 0xA2DFu : 0x3B55u; /* electric violet / cobalt */
    const uint16_t hdr_fill  = env->codex_active ? 0x18A5u : 0x0906u;
    const uint16_t hdr_rim   = env->codex_active ? 0x59D3u : 0x22CFu;
    const uint16_t accent_bar = env->codex_active ? 0xB31Fu : 0x0EFFu;

    for (int y = ry0; y <= ry1; ++y) {
        const int32_t py16 = y * 16 + 8;
        const int32_t card_vt = g3_clamp(((y - JR_G3_CORTEX_CARD_Y0) * 255) / 168, 0, 255);
        const int32_t fill_r = env->codex_active ? (16 - ((8 * card_vt) >> 8)) : (9 - ((4 * card_vt) >> 8));
        const int32_t fill_g = env->codex_active ? (14 - ((6 * card_vt) >> 8)) : (18 - ((9 * card_vt) >> 8));
        const int32_t fill_b = env->codex_active ? (34 - ((14 * card_vt) >> 8)) : (32 - ((14 * card_vt) >> 8));
        const bool in_hdr_y = (y >= JR_G3_CORTEX_HDR_Y0 - 1 && y <= JR_G3_CORTEX_HDR_Y1 + 1);
        const bool is_div_y = (y == 225 || y == 259);
        const bool in_acc_y = (y >= 156 && y <= 176);

        uint16_t *row = dst + (size_t)(y - y0) * G3_W;
        for (int x = rx0; x <= rx1; ++x) {
            const int32_t px16 = x * 16 + 8;
            const int32_t sd = g3_sd_rrect16(px16, py16, card_cx16, card_cy16,
                                             card_hx16, card_hy16, card_rad16);
            if (sd >= 64) {
                continue;
            }
            const int32_t ht = g3_clamp(((x - JR_G3_CORTEX_CARD_X0) * 255) / 258, 0, 255);
            const uint16_t bdr_col = g3_lerp565(bdr_left, bdr_right, ht);

            if (sd > 8) {
                /* Soft outer luminous glass halo (3.5 px falloff) */
                const int32_t gf = g3_clamp(((64 - sd) * 255) / 56, 0, 255);
                const unsigned ga = (unsigned)((gf * gf * 62 * strength) >> 24);
                if (ga > 0u) {
                    row[x] = g3_add565(row[x], bdr_col, ga, swap_bytes);
                }
                continue;
            }

            /* Frosted dark translucent card interior (ordered-dithered for zero banding) */
            const int32_t inside_cov = (sd <= -8) ? 255 : g3_clamp(((8 - sd) * 255) / 16, 0, 255);
            uint16_t px = row[x];
            if (inside_cov > 0) {
                /* Subtle top-edge inner frosted glass sheen */
                uint16_t fc = g3_rgb565_dither(fill_r, fill_g, fill_b, x, y);
                if (y <= JR_G3_CORTEX_CARD_Y0 + 10) {
                    const int32_t sheen = ((JR_G3_CORTEX_CARD_Y0 + 10 - y) * 38) / 10;
                    fc = g3_add565(fc, bdr_col, (unsigned)sheen, false);
                }
                px = g3_blend565(px, fc, (unsigned)((inside_cov * 228 * strength) >> 16), swap_bytes);
            }

            /* Header pill bar inside card (x in [116..350], y in [148..184]) */
            if (in_hdr_y && x >= JR_G3_CORTEX_HDR_X0 - 1 && x <= JR_G3_CORTEX_HDR_X1 + 1) {
                const int32_t hsd = g3_sd_rrect16(px16, py16, hdr_cx16, hdr_cy16,
                                                  hdr_hx16, hdr_hy16, hdr_rad16);
                if (hsd < 8) {
                    const int32_t hcov = (hsd <= -8) ? 255 : g3_clamp(((8 - hsd) * 255) / 16, 0, 255);
                    px = g3_blend565(px, hdr_fill, (unsigned)((hcov * 215 * strength) >> 16), swap_bytes);
                    /* 1 px hairline inner border around header pill */
                    const int32_t hrim = 16 - g3_abs(hsd + 4);
                    if (hrim > 0) {
                        const unsigned ha = (unsigned)((hrim * 155 * strength) >> 12);
                        px = g3_add565(px, hdr_rim, ha > 255u ? 255u : ha, swap_bytes);
                    }
                }
                /* Crisp left glowing accent bar inside header pill */
                if (in_acc_y && x >= 123 && x <= 126) {
                    px = g3_blend565(px, accent_bar, (unsigned)((230 * strength) >> 8), swap_bytes);
                }
            }

            /* Faint horizontal row divider hairlines at y = 225 and y = 259 */
            if (is_div_y && x >= 118 && x <= 348) {
                const int32_t edge = g3_min(x - 118, 348 - x);
                const int32_t ef = edge < 24 ? (edge * 255) / 24 : 255;
                const unsigned da = (unsigned)((ef * 54 * strength) >> 16);
                px = g3_add565(px, bdr_col, da, swap_bytes);
            }

            /* Crisp 1 px luminous card border */
            const int32_t bdist = g3_abs(sd + 2);
            if (bdist < 14) {
                const int32_t bcov = ((14 - bdist) * 255) / 14;
                const unsigned ba = (unsigned)((bcov * 215 * strength) >> 16);
                px = g3_add565(px, bdr_col, ba, swap_bytes);
            }
            row[x] = px;
        }
    }

    /* 3 Glowing Status LED Dots at x = 128, y = 208 / 242 / 276 */
    static const int s_led_y[3] = {
        JR_G3_CORTEX_ROW0_Y,
        JR_G3_CORTEX_ROW1_Y,
        JR_G3_CORTEX_ROW2_Y,
    };
    const int32_t led_cx16 = JR_G3_CORTEX_LED_X * 16 + 8;
    for (int i = 0; i < 3; ++i) {
        const int ly = s_led_y[i];
        if (ly + 10 < y0 || ly - 10 >= y1) {
            continue;
        }
        uint16_t l_core, l_halo;
        bool l_lit;
        cortex_led_palette(env->row_led[i], &l_core, &l_halo, &l_lit);
        const int32_t led_cy16 = ly * 16 + 8;

        if (l_lit) {
            g3_glow_orb_clipped(dst, y0, y1, swap_bytes, led_cx16, led_cy16, 9,
                                l_halo, (uint8_t)((175 * strength) >> 8),
                                JR_G3_CORTEX_CARD_X0 + 2, JR_G3_CORTEX_CARD_X1 - 2,
                                JR_G3_CORTEX_CARD_Y0 + 2, JR_G3_CORTEX_CARD_Y1 - 2, 190);
        }
        const int dy0 = g3_max(ly - 5, y0);
        const int dy1 = g3_min(ly + 5, y1 - 1);
        const int dx0 = JR_G3_CORTEX_LED_X - 5;
        const int dx1 = JR_G3_CORTEX_LED_X + 5;
        const int32_t r_dot16 = 60; /* 3.75 px */
        for (int y = dy0; y <= dy1; ++y) {
            const int32_t dpy = y * 16 + 8 - led_cy16;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;
            for (int x = dx0; x <= dx1; ++x) {
                const int32_t dpx = x * 16 + 8 - led_cx16;
                const int32_t d2 = dpx * dpx + dpy * dpy;
                if (d2 >= (r_dot16 + 12) * (r_dot16 + 12)) {
                    continue;
                }
                const int32_t d = (int32_t)g3_isqrt((uint32_t)d2);
                const int32_t cov = (d <= r_dot16 - 4) ? 255
                                    : g3_clamp(((r_dot16 + 12 - d) * 255) / 16, 0, 255);
                if (cov <= 0) {
                    continue;
                }
                uint16_t dc = l_core;
                if (l_lit && d < 32) {
                    dc = g3_lerp565(0xFFFFu, l_core, (d * 255) / 32);
                }
                row[x] = g3_blend565(row[x], dc, (unsigned)((cov * strength) >> 8), swap_bytes);
            }
        }
    }
}

/* Render bottom 24-bar symmetric luminous cyan-to-violet sparkline strictly
 * inside x in [140, 326], y in [324, 354]. */
static void cortex_draw_sparkline_rows(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                       const jr_g3_cortex_t *env, int32_t strength)
{
    int ry0 = g3_max(JR_G3_CORTEX_SPARK_Y0, y0);
    int ry1 = g3_min(JR_G3_CORTEX_SPARK_Y1, y1 - 1);
    if (ry0 > ry1) {
        return;
    }

    int32_t band_sum = 0;
    for (int i = 0; i < 16; ++i) {
        band_sum += (int32_t)env->bands[i];
    }
    const bool has_audio = (band_sum > 0);
    const uint32_t hb_phase = (env->now_ms * 26u) & 0xFFFFu;

    /* Precompute 24 symmetric bar half-heights (in Q4 px, range 24..224 -> 1.5..14 px) */
    int16_t bar_h16[JR_G3_CORTEX_SPARK_BARS];
    uint16_t bar_col[JR_G3_CORTEX_SPARK_BARS];
    int16_t bar_cx[JR_G3_CORTEX_SPARK_BARS];

    for (int k = 0; k < JR_G3_CORTEX_SPARK_BARS; ++k) {
        bar_cx[k] = (int16_t)(144 + (k * 178 + 11) / 23); /* 144..322 */
        /* Smooth electric cyan -> azure -> electric violet across 24 bars */
        const int32_t ct = (k * 255) / 23;
        bar_col[k] = (ct < 128)
            ? g3_lerp565(0x0EFFu, 0x455Fu, ct * 2)
            : g3_lerp565(0x455Fu, 0xB2DFu, (ct - 128) * 2);

        /* Symmetric distance from center (0 at k=11,12 -> 11 at k=0,23) */
        const int32_t sym = (k < 12) ? (11 - k) : (k - 12);
        int32_t level;
        if (has_audio) {
            const int32_t idx = (sym * 15) / 11;
            const int32_t nxt = idx < 15 ? idx + 1 : 15;
            level = ((int32_t)env->bands[idx] * 2 + (int32_t)env->bands[nxt]) / 3;
        } else {
            /* Gentle symmetric aerospace heartbeat wave when audio is silent */
            const int32_t w1 = g3_sin_q15((hb_phase - (uint32_t)sym * 4800u) & 0xFFFFu);
            const int32_t w2 = g3_sin_q15(((hb_phase >> 1) + (uint32_t)sym * 3200u) & 0xFFFFu);
            const int32_t env_tap = 256 - sym * 16; /* gentle taper toward edges */
            level = 42 + (((w1 * 34 + w2 * 18) >> 15) * env_tap >> 8);
            /* Subtle traveling pulse crest */
            const int32_t crest = (int32_t)((env->now_ms / 90u) % 24u);
            const int32_t dcrest = g3_abs(sym - (crest < 12 ? crest : 23 - crest));
            if (dcrest < 3) {
                level += (3 - dcrest) * 26;
            }
            level = g3_clamp(level, 18, 175);
        }
        bar_h16[k] = (int16_t)(28 + ((level * 188) / 255)); /* 1.75..13.5 px */
    }

    const int32_t y_mid16 = 339 * 16 + 8; /* center of [324..354] */

    for (int y = ry0; y <= ry1; ++y) {
        const int32_t py16 = y * 16 + 8;
        const int32_t dy16 = g3_abs(py16 - y_mid16);
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;

        /* Subtle 1 px horizontal baseline at y = 339 */
        if (y == 339) {
            for (int x = JR_G3_CORTEX_SPARK_X0 + 2; x <= JR_G3_CORTEX_SPARK_X1 - 2; ++x) {
                const int32_t edge = g3_min(x - (JR_G3_CORTEX_SPARK_X0 + 2),
                                            (JR_G3_CORTEX_SPARK_X1 - 2) - x);
                const int32_t ef = edge < 20 ? (edge * 255) / 20 : 255;
                const int32_t ct = ((x - JR_G3_CORTEX_SPARK_X0) * 255) / 186;
                const uint16_t lc = g3_lerp565(0x05DFu, 0x829Fu, ct);
                row[x] = g3_add565(row[x], lc, (unsigned)((ef * 52 * strength) >> 16), swap_bytes);
            }
        }

        for (int k = 0; k < JR_G3_CORTEX_SPARK_BARS; ++k) {
            const int32_t h16 = (int32_t)bar_h16[k];
            if (dy16 > h16 + 24) {
                continue;
            }
            const int32_t cx16 = (int32_t)bar_cx[k] * 16 + 8;
            const int xlo = g3_max((int)bar_cx[k] - 3, JR_G3_CORTEX_SPARK_X0);
            const int xhi = g3_min((int)bar_cx[k] + 3, JR_G3_CORTEX_SPARK_X1);
            const uint16_t col = bar_col[k];

            for (int x = xlo; x <= xhi; ++x) {
                const int32_t dx16 = g3_abs(x * 16 + 8 - cx16);
                /* Capsule SDF for each bar (half-width = 2.0 px = 32 Q4, rounded tip) */
                const int32_t qy = g3_max(dy16 - (h16 - 24), 0);
                const int32_t sd = (qy > 0)
                    ? ((int32_t)g3_isqrt((uint32_t)(dx16 * dx16 + qy * qy)) - 28)
                    : (dx16 - 28);
                if (sd >= 28) {
                    continue;
                }
                if (sd > 6) {
                    /* Soft additive luminous bar aura */
                    const int32_t gf = ((28 - sd) * 255) / 22;
                    const unsigned ga = (unsigned)((gf * gf * 78 * strength) >> 24);
                    row[x] = g3_add565(row[x], col, ga, swap_bytes);
                } else {
                    const int32_t cov = (sd <= -6) ? 255 : g3_clamp(((6 - sd) * 255) / 12, 0, 255);
                    /* Brighter luminous center spine fading to rich hue at tips */
                    const int32_t spine = g3_clamp(((h16 - dy16) * 255) / (h16 + 1), 0, 255);
                    const uint16_t bcol = g3_lerp565(col, 0xEFFFu, (spine * 145) >> 8);
                    row[x] = g3_blend565(row[x], bcol, (unsigned)((cov * strength) >> 8), swap_bytes);
                }
            }
        }
    }
}

void jr_g3_cortex_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                       const jr_g3_cortex_t *env)
{
    if (dst == NULL || env == NULL || nrows <= 0 || env->strength == 0u) {
        return;
    }
    int ry0 = y0 < 0 ? 0 : y0;
    int ry1 = y0 + nrows;
    if (ry1 > G3_H) {
        ry1 = G3_H;
    }
    if (ry0 >= ry1) {
        return;
    }

    const int32_t strength = (int32_t)env->strength;
    const int32_t r_max16 = G3_R_MAX * 16;
    const int32_t r_max16_sq = r_max16 * r_max16;

    /* Clamp quadrant telemetry percentages to 0..100 and compute Q8 lit cells (0..4608) */
    const int32_t pcts[4] = {
        g3_clamp((int32_t)env->mem_pct,   0, 100),
        g3_clamp((int32_t)env->cpu_pct,   0, 100),
        g3_clamp((int32_t)env->batt_pct,  0, 100),
        g3_clamp((int32_t)env->agent_pct, 0, 100),
    };
    int32_t lit_q8[4];
    for (int q = 0; q < 4; ++q) {
        lit_q8[q] = (pcts[q] * JR_G3_CORTEX_GAUGE_CELLS * 256) / 100;
    }

    /* Live emerald charging pulse for Q2 (PWR) when charging == true */
    int32_t chg_pulse = 0;
    if (env->charging) {
        const int32_t s = g3_sin_q15((env->now_ms * 34u) & 0xFFFFu);
        chg_pulse = 140 + ((s * 115) >> 15); /* 25..255 emerald pulse */
    }
    const bool low_batt = (!env->charging && pcts[2] < 20);

    /* Radial gauge geometry constants in Q4 px */
    const int32_t g_mid16 = ((JR_G3_CORTEX_GAUGE_R_IN + JR_G3_CORTEX_GAUGE_R_OUT) * 16) >> 1; /* 209*16 = 3344 */
    const int32_t g_hr16  = ((JR_G3_CORTEX_GAUGE_R_OUT - JR_G3_CORTEX_GAUGE_R_IN) * 16) >> 1; /*  15*16 = 240  */
    const int32_t t_in16  = JR_G3_CORTEX_TICK_R_IN * 16;  /* 182*16 = 2912 */
    const int32_t t_out16 = JR_G3_CORTEX_TICK_R_OUT * 16; /* 190*16 = 3040 */

    /* ---- PASS 1: Deep OLED Field + Radar Crosshair Reticle + 4 Chamfered Gauges ---- */
    for (int y = ry0; y < ry1; ++y) {
        const int32_t dy16 = y * 16 + 8 - G3_CY16;
        const int32_t dy_sq = dy16 * dy16;
        if (dy_sq >= r_max16_sq) {
            continue;
        }
        const int32_t chord16 = (int32_t)g3_isqrt((uint32_t)(r_max16_sq - dy_sq));
        int xlo = (G3_CX16 - chord16 - 8) >> 4;
        int xhi = (G3_CX16 + chord16 + 8) >> 4;
        if (xlo < 0) { xlo = 0; }
        if (xhi >= G3_W) { xhi = G3_W - 1; }

        const int32_t ady16 = g3_abs(dy16);
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;

        for (int x = xlo; x <= xhi; ++x) {
            const int32_t dx16 = x * 16 + 8 - G3_CX16;
            const int32_t d2 = dx16 * dx16 + dy_sq;
            if (d2 > r_max16_sq) {
                continue;
            }

            uint16_t px_col = 0x0000u;

            /* 1a. Subtle obsidian aerospace radial depth field inside r < 182 */
            if (d2 < (182 * 16) * (182 * 16)) {
                const int32_t fall = (int32_t)(((int64_t)((182 * 16) * (182 * 16) - d2) * 256) /
                                               ((182 * 16) * (182 * 16)));
                int32_t br = ((env->codex_active ? 10 : 5) * fall) >> 8;
                int32_t bg = ((env->codex_active ? 11 : 13) * fall) >> 8;
                int32_t bb = ((env->codex_active ? 28 : 24) * fall) >> 8;
                px_col = g3_rgb565_dither(br, bg, bb, x, y);
            }

            /* 1b. Inner Hairline Radar Crosshair Reticle (r in [120, 178])
             *     Cleanly faded out inside card box [104..362] x [136..304] & sparkline */
            if (d2 >= (119 * 16) * (119 * 16) && d2 <= (180 * 16) * (180 * 16)) {
                /* Distance outside the center card box (0 inside card) */
                const int32_t ox = g3_max(JR_G3_CORTEX_CARD_X0 - x, x - JR_G3_CORTEX_CARD_X1);
                const int32_t oy = g3_max(JR_G3_CORTEX_CARD_Y0 - y, y - JR_G3_CORTEX_CARD_Y1);
                const int32_t card_clear = g3_max(ox, oy);
                if (card_clear > 0) {
                    const int32_t cf = card_clear >= 8 ? 255 : (card_clear * 255) / 8;
                    const int32_t r16 = (int32_t)g3_isqrt((uint32_t)d2);
                    int32_t ret_a = 0;

                    /* Concentric hairline rings at r = 148 and r = 168 */
                    const int32_t dr148 = g3_abs(r16 - 148 * 16);
                    const int32_t dr168 = g3_abs(r16 - 168 * 16);
                    if (dr148 < 14) {
                        ret_a = g3_max(ret_a, ((14 - dr148) * 64) / 14);
                    }
                    if (dr168 < 14) {
                        ret_a = g3_max(ret_a, ((14 - dr168) * 76) / 14);
                    }

                    /* 4 subtle cardinal hairlines (0/90/180/270 deg, r in [120, 178]) */
                    const int32_t adx16 = g3_abs(dx16);
                    const bool clear_spark = !(y >= JR_G3_CORTEX_SPARK_Y0 - 2 &&
                                               y <= JR_G3_CORTEX_SPARK_Y1 + 4 &&
                                               x >= JR_G3_CORTEX_SPARK_X0 - 4 &&
                                               x <= JR_G3_CORTEX_SPARK_X1 + 4);
                    if (clear_spark && r16 >= 120 * 16 && r16 <= 178 * 16) {
                        const int32_t min_axis = g3_min(adx16, ady16);
                        if (min_axis < 12) {
                            ret_a = g3_max(ret_a, ((12 - min_axis) * 68) / 12);
                        }
                    }

                    if (ret_a > 0) {
                        const uint16_t ret_col = env->codex_active ? 0x4A98u : 0x1B8Eu;
                        px_col = g3_add565(px_col, ret_col, (unsigned)((ret_a * cf) >> 8), false);
                    }
                }
            }

            /* 1c. 4 Chamfered Segmented LED-Bar Gauges (r in [190..228]) & Micro-Ticks (r in [181..191]) */
            if (d2 >= (180 * 16) * (180 * 16) && d2 <= (229 * 16) * (229 * 16)) {
                const int32_t r16 = (int32_t)g3_isqrt((uint32_t)d2);
                const uint32_t ang = g3_atan2_q16(dy16, dx16);

                /* Identify which quadrant (if any) owns this angle (with ~2 deg bloom margin) */
                int q_hit = -1;
                int32_t rel_ang = 0;
                for (int q = 0; q < 4; ++q) {
                    const uint32_t start = s_cortex_quads[q].start_q16;
                    const uint32_t span = s_cortex_quads[q].span_q16;
                    int32_t da = (int32_t)((ang - start) & 0xFFFFu);
                    if (da > 32768) { da -= 65536; }
                    if (da >= -360 && da <= (int32_t)span + 360) {
                        q_hit = q;
                        rel_ang = da;
                        break;
                    }
                }

                if (q_hit >= 0) {
                    const cortex_quad_t *qd = &s_cortex_quads[q_hit];
                    /* Inner micro-ticks: 19 ticks along r in [182, 190] */
                    if (r16 >= t_in16 - 24 && r16 <= t_out16 + 12 &&
                        rel_ang >= -120 && rel_ang <= (int32_t)qd->span_q16 + 120) {
                        const int32_t step = (int32_t)qd->span_q16; /* 18 intervals */
                        const int32_t tk = g3_clamp((rel_ang * JR_G3_CORTEX_GAUGE_CELLS + (step >> 1)) / step,
                                                    0, JR_G3_CORTEX_GAUGE_CELLS);
                        const int32_t t_ang = (tk * step) / JR_G3_CORTEX_GAUGE_CELLS;
                        const int32_t da = g3_abs(rel_ang - t_ang);
                        const int32_t dt16 = (int32_t)(((int64_t)da * (r16 >> 4)) / 652);
                        const bool major = ((tk % 6) == 0);
                        const int32_t r0 = major ? (181 * 16) : (184 * 16);
                        const int32_t r1 = 190 * 16;
                        const int32_t hw = major ? 15 : 10;
                        if (dt16 < hw + 8 && r16 >= r0 - 8 && r16 <= r1 + 8) {
                            int32_t cov = (dt16 <= hw - 4) ? 255
                                          : g3_clamp(((hw + 8 - dt16) * 255) / 12, 0, 255);
                            if (r16 < r0 + 4) {
                                cov = (cov * g3_clamp(((r16 - (r0 - 8)) * 255) / 12, 0, 255)) >> 8;
                            } else if (r16 > r1 - 4) {
                                cov = (cov * g3_clamp(((r1 + 8 - r16) * 255) / 12, 0, 255)) >> 8;
                            }
                            if (cov > 0) {
                                uint16_t tcol = major ? qd->lit_core : qd->tick_col;
                                if (q_hit == 2 && env->charging) {
                                    tcol = g3_lerp565(tcol, 0x1F51u, chg_pulse);
                                }
                                px_col = g3_blend565(px_col, tcol,
                                                     (unsigned)((cov * (major ? 210 : 155)) >> 8), false);
                            }
                        }
                    }

                    /* 18 Chamfered Segmented LED-Bar Cells along r in [194, 224] (+4px glow) */
                    if (r16 >= (JR_G3_CORTEX_GAUGE_R_IN - 4) * 16 &&
                        r16 <= (JR_G3_CORTEX_GAUGE_R_OUT + 4) * 16) {
                        const int32_t span = (int32_t)qd->span_q16;
                        const int32_t cell_idx = g3_clamp((rel_ang * JR_G3_CORTEX_GAUGE_CELLS) / span,
                                                          0, JR_G3_CORTEX_GAUGE_CELLS - 1);
                        const int32_t cell_mid_ang = ((cell_idx * 2 + 1) * span) /
                                                     (JR_G3_CORTEX_GAUGE_CELLS * 2);
                        const int32_t da = g3_abs(rel_ang - cell_mid_ang);
                        /* Linear tangential offset in Q4 px: da * r16 / 10430 */
                        const int32_t dt16 = (int32_t)(((int64_t)da * r16) / 10430);
                        /* Tangential half-width of one cell (~2.15 deg at radius r16 -> ~7.8 px wide) */
                        const int32_t ht16 = (int32_t)(((int64_t)206 * r16) / 10430);
                        const int32_t dr16 = g3_abs(r16 - g_mid16);

                        /* Chamfered rectangle SDF in local (dr16, dt16), chamfer = 44 Q4 (~2.75 px) */
                        const int32_t d_box = g3_max(dr16 - g_hr16, dt16 - ht16);
                        const int32_t d_diag = ((dr16 + dt16 - (g_hr16 + ht16 - 44)) * 181) >> 8;
                        const int32_t sd = g3_max(d_box, d_diag);

                        if (sd < 64) {
                            /* Lit fraction 0..255 for this cell */
                            const int32_t c_lit = g3_clamp(lit_q8[q_hit] - cell_idx * 256, 0, 255);

                            uint16_t lit_core = qd->lit_core;
                            uint16_t lit_hi   = qd->lit_hi;
                            uint16_t glow_col = qd->glow_col;
                            if (q_hit == 2) {
                                if (env->charging) {
                                    lit_core = g3_lerp565(lit_core, 0x1F51u, chg_pulse);
                                    lit_hi   = g3_lerp565(lit_hi,   0x9FFAu, chg_pulse);
                                    glow_col = g3_lerp565(glow_col, 0x170Eu, chg_pulse);
                                } else if (low_batt) {
                                    lit_core = 0xFA65u; /* alert amber-coral */
                                    lit_hi   = 0xFDACu;
                                    glow_col = 0xC1C2u;
                                }
                            }

                            if (sd > 6) {
                                /* Soft additive OLED bloom around lit LED cells */
                                if (c_lit > 0) {
                                    const int32_t gf = ((64 - sd) * 255) / 58;
                                    const unsigned ga = (unsigned)((gf * gf * 115 * c_lit) >> 24);
                                    px_col = g3_add565(px_col, glow_col, ga, false);
                                }
                            } else {
                                const int32_t cov = (sd <= -6) ? 255
                                                    : g3_clamp(((6 - sd) * 255) / 12, 0, 255);
                                /* Unlit dark framed glass cell */
                                const bool on_rim = (sd > -20);
                                uint16_t cell_col = on_rim ? qd->unlit_rim : qd->unlit_fill;

                                if (c_lit > 0) {
                                    /* Radial luminous LED gradient (hot specular inner core -> rich saturated edges) */
                                    const int32_t rad_hot = g3_clamp(255 - (dr16 * 255) / (g_hr16 + 1), 0, 255);
                                    uint16_t on_col = g3_lerp565(lit_core, lit_hi, (rad_hot * 165) >> 8);
                                    if (on_rim) {
                                        on_col = g3_lerp565(on_col, 0xFFFFu, 55);
                                    }
                                    cell_col = g3_lerp565(cell_col, on_col, c_lit);
                                }
                                px_col = g3_blend565(px_col, cell_col, (unsigned)cov, false);
                            }
                        }
                    }
                }
            }

            if (strength >= 255) {
                row[x] = swap_bytes ? g3_bswap(px_col) : px_col;
            } else {
                row[x] = g3_blend565(row[x], px_col, (unsigned)strength, swap_bytes);
            }
        }
    }

    /* ---- PASS 2: Centered Translucent Glass Mission Card + 3 Glowing LEDs ---- */
    cortex_draw_card_rows(dst, y0, ry1, swap_bytes, env, strength);

    /* ---- PASS 3: Bottom 24-Bar Live Sparkline Waveform [140..326] x [324..354] ---- */
    cortex_draw_sparkline_rows(dst, y0, ry1, swap_bytes, env, strength);
}
