/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * showpiece_gfx.c — Wave 4 Showpiece OLED Graphics Engine.
 *
 * Pure C11, integer-only, zero-libm, zero-heap, 100% strip-invariant.
 *   1. Real-Time 3D Rotating Geodesic Hologram Engine (18 vertices, 42 edges)
 *   2. WATCH:ORRERY (J2000 astronomical planetarium + live synodic Moon phase)
 *   3. WATCH:REACTOR (32-bar radial audio spectrum + 3D holo + HH:MM + pip)
 */
#include "showpiece_gfx.h"

#include <string.h>

#include "jr_display/glass.h"

#define SP_W      HUD_W
#define SP_H      HUD_H
#define SP_CX16   (233 * 16)   /* pixel x center is 16*x + 8 -> 232.5 px = 3728 */
#define SP_CY16   (233 * 16)
#define SP_CX2    465          /* 2 * 232.5 */
#define SP_CY2    465

/* ---- fast integer helpers ----------------------------------------------- */

static inline int sp_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int sp_abs(int v)
{
    return v < 0 ? -v : v;
}

static uint32_t sp_isqrt(uint32_t n)
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

static inline uint16_t sp_rgb565(int r, int g, int b)
{
    r = sp_clamp(r, 0, 255);
    g = sp_clamp(g, 0, 255);
    b = sp_clamp(b, 0, 255);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Saturating additive OLED light blend: adds `over` scaled by `a/255` onto
 * panel-order pixel `under`. Produces realistic luminous intersections. */
static inline uint16_t sp_add565(uint16_t under, uint16_t over, unsigned a,
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

/* Lerp two native RGB565 colors by t in 0..255. */
static inline uint16_t sp_lerp565(uint16_t c0, uint16_t c1, int t)
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

/* True if pixel (x, y) lies inside circle of radius `r_max` about (232.5, 232.5). */
static inline bool sp_in_circle(int x, int y, int r_max)
{
    const int dx2 = 2 * x - SP_CX2;
    const int dy2 = 2 * y - SP_CY2;
    const int lim = 2 * r_max;
    return dx2 * dx2 + dy2 * dy2 <= lim * lim;
}

/* Draw a soft additive OLED glow orb (quadratic falloff) clipped to [y0, y1)
 * and circle r <= clip_r. Center is in Q4 px (16*x + 8). */
static void sp_glow_orb_rows(uint16_t *dst, int y0, int y1, bool swap_bytes,
                             int cx16, int cy16, int r_px,
                             uint16_t color, uint8_t peak_a, int clip_r)
{
    if (r_px <= 0 || peak_a == 0u) {
        return;
    }
    const int cy_i = cy16 >> 4;
    const int cx_i = cx16 >> 4;
    int ry0 = cy_i - r_px - 1;
    int ry1 = cy_i + r_px + 1;
    if (ry0 < y0) { ry0 = y0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry0 > ry1) {
        return;
    }
    int rx0 = cx_i - r_px - 1;
    int rx1 = cx_i + r_px + 1;
    if (rx0 < 0) { rx0 = 0; }
    if (rx1 >= SP_W) { rx1 = SP_W - 1; }
    const int32_t r16 = r_px * 16;
    const int32_t r16_sq = r16 * r16;
    const int lim2 = 2 * clip_r;
    const int lim2_sq = lim2 * lim2;

    for (int y = ry0; y <= ry1; ++y) {
        const int dy2 = 2 * y - SP_CY2;
        const int dy2_sq = dy2 * dy2;
        if (dy2_sq > lim2_sq) {
            continue;
        }
        const int32_t py16 = y * 16 + 8 - cy16;
        const int32_t py_sq = py16 * py16;
        if (py_sq >= r16_sq) {
            continue;
        }
        uint16_t *row = dst + (size_t)(y - y0) * SP_W;
        for (int x = rx0; x <= rx1; ++x) {
            const int dx2 = 2 * x - SP_CX2;
            if (dx2 * dx2 + dy2_sq > lim2_sq) {
                continue;
            }
            const int32_t px16 = x * 16 + 8 - cx16;
            const int32_t d2 = px16 * px16 + py_sq;
            if (d2 >= r16_sq) {
                continue;
            }
            /* smooth quadratic falloff: (1 - d^2/r^2)^2 */
            const uint32_t t = (uint32_t)(((int64_t)(r16_sq - d2) * 255) / r16_sq);
            const unsigned a = (unsigned)((t * t * peak_a) >> 16);
            if (a > 0u) {
                row[x] = sp_add565(row[x], color, a, swap_bytes);
            }
        }
    }
}

/* ==========================================================================
 * SHOWPIECE #3 (SHARED): REAL-TIME 3D ROTATING GEODESIC HOLOGRAM ENGINE
 *
 * Outer geodesic icosahedron (12 vertices, 30 edges) + inner counter-rotating
 * octahedron (6 vertices, 12 edges) = 18 vertices, 42 edges in flash .rodata.
 * ========================================================================== */

#define HOLO_VERTS  18
#define HOLO_EDGES  42
#define ICO_A       2153   /* 4096 / sqrt(1 + phi^2) */
#define ICO_B       3484   /* 4096 * phi / sqrt(1 + phi^2) */
#define OCT_C       2294   /* 0.56 * 4096 */

static const int16_t s_holo_verts[HOLO_VERTS][3] = {
    /* 0..11: unit icosahedron (Q12) */
    {     0,  ICO_A,  ICO_B },
    {     0, -ICO_A,  ICO_B },
    {     0,  ICO_A, -ICO_B },
    {     0, -ICO_A, -ICO_B },
    {  ICO_A,  ICO_B,     0 },
    { -ICO_A,  ICO_B,     0 },
    {  ICO_A, -ICO_B,     0 },
    { -ICO_A, -ICO_B,     0 },
    {  ICO_B,     0,  ICO_A },
    {  ICO_B,     0, -ICO_A },
    { -ICO_B,     0,  ICO_A },
    { -ICO_B,     0, -ICO_A },
    /* 12..17: inner octahedron crystal (Q12) */
    {  OCT_C,      0,      0 },
    { -OCT_C,      0,      0 },
    {      0,  OCT_C,      0 },
    {      0, -OCT_C,      0 },
    {      0,      0,  OCT_C },
    {      0,      0, -OCT_C },
};

static const uint8_t s_holo_edges[HOLO_EDGES][2] = {
    /* 30 icosahedron edges */
    {  0,  1 }, {  0,  4 }, {  0,  5 }, {  0,  8 }, {  0, 10 },
    {  1,  6 }, {  1,  7 }, {  1,  8 }, {  1, 10 },
    {  2,  3 }, {  2,  4 }, {  2,  5 }, {  2,  9 }, {  2, 11 },
    {  3,  6 }, {  3,  7 }, {  3,  9 }, {  3, 11 },
    {  4,  5 }, {  4,  8 }, {  4,  9 },
    {  5, 10 }, {  5, 11 },
    {  6,  7 }, {  6,  8 }, {  6,  9 },
    {  7, 10 }, {  7, 11 },
    {  8,  9 }, { 10, 11 },
    /* 12 inner octahedron edges */
    { 12, 14 }, { 12, 15 }, { 12, 16 }, { 12, 17 },
    { 13, 14 }, { 13, 15 }, { 13, 16 }, { 13, 17 },
    { 14, 16 }, { 14, 17 }, { 15, 16 }, { 15, 17 },
};

typedef struct {
    int16_t sx16;    /* projected x in Q4 px */
    int16_t sy16;    /* projected y in Q4 px */
    uint8_t depth;   /* 0 (far) .. 255 (near) */
    uint8_t energy;  /* audio excitation 0..255 */
} holo_pv_t;

typedef struct {
    int16_t y_min, y_max;  /* inclusive pixel row bounds */
    int16_t x0, y0, x1, y1;/* endpoints in Q4 px */
    int16_t nx, ny;        /* Q12 unit perpendicular normal */
    int16_t tx, ty;        /* Q12 unit tangent */
    int16_t len16;         /* segment length in Q4 px */
    uint16_t color;        /* native RGB565 */
    uint8_t alpha;         /* 0..255 */
} holo_pe_t;

typedef struct {
    bool     valid;
    uint32_t now_ms;
    int      max_r;
    int      mode;
    int      tilt_x, tilt_y;
    uint8_t  amp;
    int      strength;
    uint8_t  bands[16];
    uint16_t near_col, mid_col, far_col, inner_col, core_col;
    holo_pv_t v[HOLO_VERTS];
    holo_pe_t e[HOLO_EDGES];
} holo_cache_t;

static holo_cache_t s_holo_cache;

static void holo_rotate_pt(int32_t x, int32_t y, int32_t z,
                           int32_t sy, int32_t cy,
                           int32_t sp, int32_t cp,
                           int32_t sr, int32_t cr,
                           int32_t *ox, int32_t *oy, int32_t *oz)
{
    /* Yaw around Y */
    const int32_t x1 = (int32_t)(((int64_t)x * cy + (int64_t)z * sy) >> 15);
    const int32_t z1 = (int32_t)(((int64_t)-x * sy + (int64_t)z * cy) >> 15);
    /* Pitch around X */
    const int32_t y2 = (int32_t)(((int64_t)y * cp - (int64_t)z1 * sp) >> 15);
    const int32_t z2 = (int32_t)(((int64_t)y * sp + (int64_t)z1 * cp) >> 15);
    /* Roll around Z */
    const int32_t x3 = (int32_t)(((int64_t)x1 * cr - (int64_t)y2 * sr) >> 15);
    const int32_t y3 = (int32_t)(((int64_t)x1 * sr + (int64_t)y2 * cr) >> 15);
    *ox = x3;
    *oy = y3;
    *oz = z2;
}

static void holo_prepare(holo_cache_t *c, uint32_t now_ms, int max_r,
                         jr_holo_mode_t mode, int tilt_x, int tilt_y,
                         const uint8_t bands[16], uint8_t amp, int strength)
{
    uint8_t zero_bands[16] = { 0 };
    const uint8_t *b = bands ? bands : zero_bands;

    if (c->valid && c->now_ms == now_ms && c->max_r == max_r &&
        c->mode == (int)mode && c->tilt_x == tilt_x && c->tilt_y == tilt_y &&
        c->amp == amp && c->strength == strength &&
        memcmp(c->bands, b, 16) == 0) {
        return;
    }

    c->valid = true;
    c->now_ms = now_ms;
    c->max_r = max_r;
    c->mode = (int)mode;
    c->tilt_x = tilt_x;
    c->tilt_y = tilt_y;
    c->amp = amp;
    c->strength = strength;
    memcpy(c->bands, b, 16);

    /* State-aware OLED palettes + angular velocities */
    uint32_t yaw_speed = 5u;   /* Q16 turns per ~13 s */
    uint32_t pitch_speed = 3u;
    switch (mode) {
    case JR_HOLO_SPEAK:
        c->near_col  = 0xFFFFu;  /* ice-white */
        c->mid_col   = 0x07FFu;  /* neon cyan */
        c->far_col   = 0x0276u;  /* deep electric sapphire */
        c->inner_col = 0x5FFFu;  /* bright aqua core */
        c->core_col  = 0xFFFFu;
        yaw_speed = 9u;
        pitch_speed = 5u;
        break;
    case JR_HOLO_LISTEN:
        c->near_col  = 0x7FFEu;  /* bright turquoise */
        c->mid_col   = 0x07F8u;  /* electric turquoise */
        c->far_col   = 0x02B3u;  /* deep teal */
        c->inner_col = 0x47FDu;
        c->core_col  = 0xCFFFu;
        yaw_speed = 7u;
        pitch_speed = 4u;
        break;
    case JR_HOLO_THINK:
        c->near_col  = 0x47FFu;  /* electric cyan */
        c->mid_col   = 0x05FFu;
        c->far_col   = 0x419Au;  /* deep indigo-violet */
        c->inner_col = 0xC47Fu;  /* vibrant violet-indigo inner crystal */
        c->core_col  = 0xE6FFu;
        yaw_speed = 14u;
        pitch_speed = 10u;
        break;
    case JR_HOLO_MUTED:
        c->near_col  = 0xFFEBu;  /* champagne-gold highlight */
        c->mid_col   = 0xFD66u;  /* warm luxury gold */
        c->far_col   = 0x6241u;  /* deep bronze-gold */
        c->inner_col = 0xFEA8u;  /* soft warm gold inner crystal */
        c->core_col  = 0xFFF2u;
        yaw_speed = 3u;
        pitch_speed = 2u;
        break;
    case JR_HOLO_REACTOR:
        c->near_col  = 0xEFFFu;  /* ice-white cyan */
        c->mid_col   = 0x07FFu;  /* electric cyan */
        c->far_col   = 0x0215u;  /* deep cobalt */
        c->inner_col = 0x67FFu;  /* hot plasma cyan */
        c->core_col  = 0xFFFFu;
        yaw_speed = 8u;
        pitch_speed = 5u;
        break;
    case JR_HOLO_IDLE:
    default:
        c->near_col  = 0x57FFu;  /* cool crystalline cyan */
        c->mid_col   = 0x059Fu;  /* cyan-cobalt */
        c->far_col   = 0x0194u;  /* deep sapphire */
        c->inner_col = 0x26FFu;  /* cobalt-cyan crystal */
        c->core_col  = 0xBFFFu;
        yaw_speed = 4u;
        pitch_speed = 2u;
        break;
    }

    int bass = ((int)b[0] + b[1] + b[2] + b[3]) >> 2;
    int mid_e = ((int)b[4] + b[5] + b[6] + b[7] + b[8] + b[9]) / 6;
    if (bass == 0 && amp > 0u) {
        bass = (int)amp;
        mid_e = (int)(amp * 3u / 4u);
    }

    /* Smooth rotation + QMI8658 IMU tilt response */
    const uint32_t yaw   = (now_ms * yaw_speed) & 0xFFFFu;
    const uint32_t pitch = ((now_ms * pitch_speed) + (uint32_t)(tilt_y * 28)) & 0xFFFFu;
    const uint32_t roll  = ((now_ms * 1u) + (uint32_t)(tilt_x * 28)) & 0xFFFFu;

    const uint32_t iyaw   = (uint32_t)(-((int32_t)yaw * 3 / 2)) & 0xFFFFu;
    const uint32_t ipitch = (pitch + (pitch >> 2) + 12288u) & 0xFFFFu;
    const uint32_t iroll  = (uint32_t)(-(int32_t)roll) & 0xFFFFu;

    const int32_t sy = glass_sin_q15(yaw),   cy = glass_cos_q15(yaw);
    const int32_t sp = glass_sin_q15(pitch), cp = glass_cos_q15(pitch);
    const int32_t sr = glass_sin_q15(roll),  cr = glass_cos_q15(roll);

    const int32_t isy = glass_sin_q15(iyaw),   icy = glass_cos_q15(iyaw);
    const int32_t isp = glass_sin_q15(ipitch), icp = glass_cos_q15(ipitch);
    const int32_t isr = glass_sin_q15(iroll),  icr = glass_cos_q15(iroll);

    /* Keep max projected vertex radius comfortably inside (max_r - 5) px so
     * vertex halos never spill outside r <= max_r. */
    const int safe_r = max_r > 10 ? max_r - 5 : max_r;
    const int32_t z_cam = 13500; /* ~3.3 * 4096 */
    /* Perspective scale: at z=+4096 (front), (z_cam - z) = 9404.
     * At base radius 0.72 * safe_r and max audio burst 1.18x, max front extent
     * stays <= safe_r. */
    const int32_t base_scale = (safe_r * 16 * 7600) / 4096;

    /* Subtle breathing on LISTEN/IDLE */
    const int breath = (glass_sin_q15((now_ms * 14u) & 0xFFFFu) * 140) >> 15; /* +-140 in Q12 */

    for (int i = 0; i < HOLO_VERTS; ++i) {
        const bool inner = (i >= 12);
        int32_t vx = s_holo_verts[i][0];
        int32_t vy = s_holo_verts[i][1];
        int32_t vz = s_holo_verts[i][2];

        /* Radial modulation from live 16-band spectrum + breathing */
        int v_energy = inner ? bass : (int)b[(i * 16) / 12];
        if (!inner && v_energy == 0 && mid_e > 0) {
            v_energy = (mid_e * (2 + (i & 1))) / 3;
        }
        int32_t rad_q12 = 4096 + breath;
        if (mode == JR_HOLO_SPEAK) {
            rad_q12 += (v_energy * 820) >> 8;
        } else if (mode == JR_HOLO_LISTEN || mode == JR_HOLO_REACTOR) {
            rad_q12 += (v_energy * 640) >> 8;
        } else if (mode == JR_HOLO_THINK) {
            rad_q12 += (v_energy * 360) >> 8;
        }
        if (rad_q12 > 4800) {
            rad_q12 = 4800;
        }
        vx = (vx * rad_q12) >> 12;
        vy = (vy * rad_q12) >> 12;
        vz = (vz * rad_q12) >> 12;

        int32_t rx, ry, rz;
        if (!inner) {
            holo_rotate_pt(vx, vy, vz, sy, cy, sp, cp, sr, cr, &rx, &ry, &rz);
        } else {
            holo_rotate_pt(vx, vy, vz, isy, icy, isp, icp, isr, icr, &rx, &ry, &rz);
        }

        int32_t denom = z_cam - rz;
        if (denom < 4096) {
            denom = 4096;
        }
        int32_t sx16 = SP_CX16 + (int32_t)(((int64_t)rx * base_scale) / denom);
        int32_t sy16 = SP_CY16 + (int32_t)(((int64_t)ry * base_scale) / denom);

        /* Hard radial clamp in Q4 to guarantee vertex stays inside safe_r */
        int32_t dx = sx16 - SP_CX16;
        int32_t dy = sy16 - SP_CY16;
        const int32_t max_q4 = safe_r * 16;
        const int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
        if (d2 > (int64_t)max_q4 * max_q4) {
            const uint32_t d = sp_isqrt((uint32_t)d2);
            if (d > 0u) {
                sx16 = SP_CX16 + (int32_t)(((int64_t)dx * max_q4) / (int64_t)d);
                sy16 = SP_CY16 + (int32_t)(((int64_t)dy * max_q4) / (int64_t)d);
            }
        }

        int depth = ((rz + 4200) * 255) / 8400;
        c->v[i].sx16 = (int16_t)sx16;
        c->v[i].sy16 = (int16_t)sy16;
        c->v[i].depth = (uint8_t)sp_clamp(depth, 0, 255);
        c->v[i].energy = (uint8_t)sp_clamp(v_energy, 0, 255);
    }

    for (int i = 0; i < HOLO_EDGES; ++i) {
        const bool inner = (i >= 30);
        const holo_pv_t *a = &c->v[s_holo_edges[i][0]];
        const holo_pv_t *b_v = &c->v[s_holo_edges[i][1]];
        holo_pe_t *pe = &c->e[i];

        pe->x0 = a->sx16;
        pe->y0 = a->sy16;
        pe->x1 = b_v->sx16;
        pe->y1 = b_v->sy16;

        const int32_t dx = (int32_t)pe->x1 - pe->x0;
        const int32_t dy = (int32_t)pe->y1 - pe->y0;
        const uint32_t len = sp_isqrt((uint32_t)(dx * dx + dy * dy));
        pe->len16 = (int16_t)(len > 32000u ? 32000u : len);
        if (len > 0u) {
            pe->tx = (int16_t)((dx * 4096) / (int32_t)len);
            pe->ty = (int16_t)((dy * 4096) / (int32_t)len);
            pe->nx = (int16_t)((-dy * 4096) / (int32_t)len);
            pe->ny = (int16_t)((dx * 4096) / (int32_t)len);
        } else {
            pe->tx = 4096; pe->ty = 0; pe->nx = 0; pe->ny = 4096;
        }

        const int ylo = (pe->y0 < pe->y1 ? pe->y0 : pe->y1) - 24;
        const int yhi = (pe->y0 > pe->y1 ? pe->y0 : pe->y1) + 24;
        pe->y_min = (int16_t)sp_clamp(ylo >> 4, 0, SP_H - 1);
        pe->y_max = (int16_t)sp_clamp((yhi + 15) >> 4, 0, SP_H - 1);

        const int d_avg = ((int)a->depth + b_v->depth) >> 1;
        const int e_avg = ((int)a->energy + b_v->energy) >> 1;
        if (inner) {
            pe->color = sp_lerp565(c->far_col, c->inner_col, 90 + ((d_avg * 165) >> 8));
        } else if (d_avg >= 128) {
            pe->color = sp_lerp565(c->mid_col, c->near_col, (d_avg - 128) * 2);
        } else {
            pe->color = sp_lerp565(c->far_col, c->mid_col, d_avg * 2);
        }

        int base_a = inner ? (75 + ((d_avg * 130) >> 8))
                           : (58 + ((d_avg * 175) >> 8));
        if (e_avg > 0) {
            base_a += (e_avg * 45) >> 8;
        }
        base_a = (base_a * strength) >> 8;
        pe->alpha = (uint8_t)sp_clamp(base_a, 0, 255);
    }
}

/* Rasterize one anti-aliased additive 3D edge onto rows [y0, y1), strictly
 * clipped to circle r <= clip_r. Zero divides per pixel. */
static void holo_draw_edge(uint16_t *dst, int y0, int y1, bool swap_bytes,
                           const holo_pe_t *pe, int clip_r)
{
    if (pe->alpha == 0u || pe->len16 <= 0) {
        return;
    }
    int ry0 = pe->y_min;
    int ry1 = pe->y_max;
    if (ry0 < y0) { ry0 = y0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry0 > ry1) {
        return;
    }

    const int32_t x_min_q4 = (pe->x0 < pe->x1 ? pe->x0 : pe->x1) - 24;
    const int32_t x_max_q4 = (pe->x0 > pe->x1 ? pe->x0 : pe->x1) + 24;
    const int32_t dx = (int32_t)pe->x1 - pe->x0;
    const int32_t dy = (int32_t)pe->y1 - pe->y0;
    const int32_t half_w = 20; /* 1.25 px anti-aliased half-width in Q4 */
    const int lim2 = 2 * clip_r;
    const int lim2_sq = lim2 * lim2;

    for (int y = ry0; y <= ry1; ++y) {
        const int dy2 = 2 * y - SP_CY2;
        const int dy2_sq = dy2 * dy2;
        if (dy2_sq > lim2_sq) {
            continue;
        }
        const int32_t py16 = y * 16 + 8;
        int xlo, xhi;
        if (sp_abs(dy) < 16) {
            xlo = x_min_q4 >> 4;
            xhi = (x_max_q4 + 15) >> 4;
        } else {
            /* Intersect [py16 - 24, py16 + 24] with segment line */
            const int32_t xa = pe->x0 + (int32_t)(((int64_t)(py16 - 24 - pe->y0) * dx) / dy);
            const int32_t xb = pe->x0 + (int32_t)(((int64_t)(py16 + 24 - pe->y0) * dx) / dy);
            int32_t lo_q4 = xa < xb ? xa : xb;
            int32_t hi_q4 = xa > xb ? xa : xb;
            lo_q4 -= 24;
            hi_q4 += 24;
            if (lo_q4 < x_min_q4) { lo_q4 = x_min_q4; }
            if (hi_q4 > x_max_q4) { hi_q4 = x_max_q4; }
            xlo = lo_q4 >> 4;
            xhi = (hi_q4 + 15) >> 4;
        }
        if (xlo < 0) { xlo = 0; }
        if (xhi >= SP_W) { xhi = SP_W - 1; }

        uint16_t *row = dst + (size_t)(y - y0) * SP_W;
        const int32_t dpy = py16 - pe->y0;
        for (int x = xlo; x <= xhi; ++x) {
            const int dx2 = 2 * x - SP_CX2;
            if (dx2 * dx2 + dy2_sq > lim2_sq) {
                continue;
            }
            const int32_t dpx = x * 16 + 8 - pe->x0;
            /* Perpendicular distance in Q4 */
            const int32_t perp = sp_abs((dpx * pe->nx + dpy * pe->ny) >> 12);
            if (perp >= half_w) {
                continue;
            }
            /* Tangential projection in Q4 (clamp smoothly at endpoints) */
            const int32_t tang = (dpx * pe->tx + dpy * pe->ty) >> 12;
            int cov = ((half_w - perp) * 255) / half_w;
            if (tang < 0) {
                if (tang <= -half_w) { continue; }
                cov = (cov * (half_w + tang)) / half_w;
            } else if (tang > pe->len16) {
                const int32_t over = tang - pe->len16;
                if (over >= half_w) { continue; }
                cov = (cov * (half_w - over)) / half_w;
            }
            const unsigned a = (unsigned)((cov * pe->alpha) >> 8);
            if (a > 0u) {
                row[x] = sp_add565(row[x], pe->color, a, swap_bytes);
            }
        }
    }
}

void jr_showpiece_holo_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                            uint32_t now_ms, int max_r, jr_holo_mode_t mode,
                            int tilt_x, int tilt_y,
                            const uint8_t bands[16], uint8_t amp, int strength)
{
    if (dst == NULL || nrows <= 0 || strength <= 0 || max_r <= 4) {
        return;
    }
    const int y1 = y0 + nrows;
    if (y1 <= 232 - max_r || y0 > 233 + max_r) {
        return;
    }
    if (strength > 255) {
        strength = 255;
    }

    holo_prepare(&s_holo_cache, now_ms, max_r, mode, tilt_x, tilt_y, bands, amp, strength);
    const holo_cache_t *c = &s_holo_cache;

    /* 1. Depth-cued anti-aliased wireframe edges */
    for (int i = 0; i < HOLO_EDGES; ++i) {
        holo_draw_edge(dst, y0, y1, swap_bytes, &c->e[i], max_r);
    }

    /* 2. Glowing depth-cued vertex orbs (near = bright ice-white + halo) */
    for (int i = 0; i < HOLO_VERTS; ++i) {
        const holo_pv_t *v = &c->v[i];
        const bool inner = (i >= 12);
        const int halo_r = v->depth > 150 ? 5 : (v->depth > 80 ? 4 : 3);
        const uint16_t halo_col = inner ? c->inner_col
            : sp_lerp565(c->far_col, c->near_col, v->depth);
        const int halo_a = ((85 + ((v->depth * 145) >> 8) + (v->energy >> 2)) * strength) >> 8;
        sp_glow_orb_rows(dst, y0, y1, swap_bytes, v->sx16, v->sy16, halo_r,
                         halo_col, (uint8_t)sp_clamp(halo_a, 0, 255), max_r);
        if (v->depth >= 96) {
            const int core_a = ((110 + (((v->depth - 96) * 145) / 159)) * strength) >> 8;
            sp_glow_orb_rows(dst, y0, y1, swap_bytes, v->sx16, v->sy16, 2,
                             c->core_col, (uint8_t)sp_clamp(core_a, 0, 255), max_r);
        }
    }
}

/* ==========================================================================
 * SHOWPIECE #1: WATCH:ORRERY (J2000 EPHEMERIS + PLANETARIUM + MOON PHASE)
 * ========================================================================== */

/* Exact measured golden orbital track radii in dial_orrery.eaf (center 232.5) */
static const int16_t s_orrery_orbit_r[JR_ORRERY_PLANETS] = { 53, 79, 104, 130, 154, 183 };
static const int8_t  s_orrery_planet_r[JR_ORRERY_PLANETS] = { 4, 5, 6, 5, 7, 6 };
static const int8_t  s_orrery_parallax[JR_ORRERY_PLANETS] = { 2, 3, 4, 4, 5, 6 };

/* J2000 mean heliocentric longitudes (Q16 turns = deg * 65536 / 360) and
 * sidereal orbital periods in seconds (NASA JPL Standish J2000 elements). */
static const uint32_t s_j2000_l0_q16[JR_ORRERY_PLANETS] = {
    45921u, /* Mercury: 252.25 deg */
    33128u, /* Venus:   181.98 deg */
    18288u, /* Earth:   100.46 deg */
    64708u, /* Mars:    355.45 deg */
     6262u, /* Jupiter:  34.40 deg */
     9091u, /* Saturn:   49.94 deg */
};

static const int64_t s_orbit_period_s[JR_ORRERY_PLANETS] = {
      7600530LL, /* Mercury:    87.9691 d */
     19414149LL, /* Venus:     224.7008 d */
     31558150LL, /* Earth:     365.25636 d */
     59355037LL, /* Mars:      686.9796 d */
    374335690LL, /* Jupiter:  4332.589 d */
    929596608LL, /* Saturn:  10759.22 d */
};

/* Planet RGB palettes: base color + soft OLED halo color */
static const uint8_t s_planet_rgb[JR_ORRERY_PLANETS][3] = {
    { 216, 208, 198 }, /* Mercury: warm silver-pearl */
    { 246, 214, 148 }, /* Venus:   champagne-gold */
    {  46, 184, 255 }, /* Earth:   glowing sapphire-turquoise */
    { 244,  96,  68 }, /* Mars:    glowing ruby-coral */
    { 236, 180, 112 }, /* Jupiter: warm ochre-amber */
    { 242, 218, 146 }, /* Saturn:  pale gold */
};

static const uint16_t s_planet_halo565[JR_ORRERY_PLANETS] = {
    0xCE59u, /* Mercury silver-pearl */
    0xFE8Eu, /* Venus champagne */
    0x25DFu, /* Earth sapphire-turquoise */
    0xF2E8u, /* Mars ruby-coral */
    0xE58Du, /* Jupiter ochre-amber */
    0xF690u, /* Saturn pale gold */
};

void jr_orrery_ephemeris(uint16_t year, uint8_t month, uint8_t day,
                         int hh, int mm, int ss, jr_orrery_eph_t *out)
{
    if (out == NULL) {
        return;
    }
    if (year < 1970u || month < 1u || month > 12u || day < 1u || day > 31u) {
        year = 2026u;
        month = 11u;
        day = 23u;
    }
    hh = ((hh % 24) + 24) % 24;
    mm = ((mm % 60) + 60) % 60;
    ss = ((ss % 60) + 60) % 60;

    /* Fliegel-Van Flandern integer Gregorian -> Julian Day Number */
    const int a = (14 - (int)month) / 12;
    const int y = (int)year + 4800 - a;
    const int m = (int)month + 12 * a - 3;
    const int32_t jdn = (int32_t)day + (153 * m + 2) / 5 + 365 * y +
                        y / 4 - y / 100 + y / 400 - 32045;

    /* Seconds since J2000.0 (JD 2451545.0 = 2000-01-01 12:00 UTC) */
    const int64_t t_sec = (int64_t)(jdn - 2451545) * 86400LL +
                          (int64_t)(hh - 12) * 3600LL +
                          (int64_t)mm * 60LL + (int64_t)ss;

    for (int p = 0; p < JR_ORRERY_PLANETS; ++p) {
        const int64_t P = s_orbit_period_s[p];
        int64_t rem = t_sec % P;
        if (rem < 0) {
            rem += P;
        }
        const uint32_t delta_q16 = (uint32_t)((rem * 65536LL) / P);
        const uint32_t lon_q16 = (s_j2000_l0_q16[p] + delta_q16) & 0xFFFFu;
        /* Prograde astronomical north-pole view: counter-clockwise on dial
         * (screen y grows down, so subtract from 65536). */
        out->planet_q16[p] = (65536u - lon_q16) & 0xFFFFu;
    }

    /* Synodic Moon phase: 29.530588853 days = 2,551,443 s from New Moon epoch
     * JD 2451550.1 (5.1 days = 440,640 s after J2000.0). */
    const int64_t synodic_s = 2551443LL;
    int64_t mrem = (t_sec - 440640LL) % synodic_s;
    if (mrem < 0) {
        mrem += synodic_s;
    }
    out->moon_phase_q16 = (uint32_t)((mrem * 65536LL) / synodic_s);
    /* Earth's orbiting Moon position around Earth */
    out->earth_moon_q16 = (out->planet_q16[2] + (65536u - out->moon_phase_q16)) & 0xFFFFu;
}

/* Draw a 3D shaded jewel orb (dark terminator + top-left IMU specular highlight)
 * onto rows [y0, y1). Center (cx16, cy16) in Q4 px. */
static void orrery_planet_sphere(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                 int cx16, int cy16, int r_px,
                                 const uint8_t rgb[3], bool jupiter_band,
                                 int lx_q15, int ly_q15, int strength)
{
    const int cy_i = cy16 >> 4;
    const int cx_i = cx16 >> 4;
    int ry0 = cy_i - r_px - 1;
    int ry1 = cy_i + r_px + 1;
    if (ry0 < y0) { ry0 = y0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry0 > ry1) {
        return;
    }
    int rx0 = cx_i - r_px - 1;
    int rx1 = cx_i + r_px + 1;
    if (rx0 < 0) { rx0 = 0; }
    if (rx1 >= SP_W) { rx1 = SP_W - 1; }

    const int32_t r16 = r_px * 16;
    const int32_t r_outer = r16 + 10;
    const int32_t r_outer_sq = r_outer * r_outer;
    const int32_t lz_q15 = 22937; /* ~0.70 */
    /* Specular half-vector (normalized approx toward light + viewer (0,0,1)) */
    const int32_t hx_q15 = lx_q15 / 2;
    const int32_t hy_q15 = ly_q15 / 2;
    const int32_t hz_q15 = (lz_q15 + 32768) / 2;

    for (int y = ry0; y <= ry1; ++y) {
        const int32_t dy16 = y * 16 + 8 - cy16;
        const int32_t dy_sq = dy16 * dy16;
        if (dy_sq >= r_outer_sq) {
            continue;
        }
        uint16_t *row = dst + (size_t)(y - y0) * SP_W;
        for (int x = rx0; x <= rx1; ++x) {
            const int32_t dx16 = x * 16 + 8 - cx16;
            const int32_t d2 = dx16 * dx16 + dy_sq;
            if (d2 >= r_outer_sq) {
                continue;
            }
            const int32_t d = (int32_t)sp_isqrt((uint32_t)d2);
            int cov = 255;
            if (d > r16 - 6) {
                cov = ((r_outer - d) * 255) / 16;
                cov = sp_clamp(cov, 0, 255);
            }
            if (cov <= 0) {
                continue;
            }
            /* Sphere normal in Q15 */
            const int32_t nx = (dx16 * 32768) / (r16 > 0 ? r16 : 16);
            const int32_t ny = (dy16 * 32768) / (r16 > 0 ? r16 : 16);
            const int64_t nz2 = (int64_t)32768 * 32768 - (int64_t)nx * nx - (int64_t)ny * ny;
            const int32_t nz = nz2 > 0 ? (int32_t)sp_isqrt((uint32_t)nz2) : 0;

            /* Lambertian diffuse with rich dark terminator (ambient 54/256) */
            int32_t ndl = (int32_t)(((int64_t)nx * lx_q15 + (int64_t)ny * ly_q15 +
                                     (int64_t)nz * lz_q15) >> 15);
            if (ndl < 0) { ndl = 0; }
            int shade = 52 + ((ndl * 224) >> 15); /* 52..276 */

            /* Specular highlight (sharp jewel glint) */
            int32_t ndh = (int32_t)(((int64_t)nx * hx_q15 + (int64_t)ny * hy_q15 +
                                     (int64_t)nz * hz_q15) >> 15);
            int spec = 0;
            if (ndh > 22000) {
                const int s1 = (ndh - 22000) >> 6; /* 0..168 */
                spec = (s1 * s1) >> 7;
                if (spec > 220) { spec = 220; }
            }

            int br = rgb[0], bg = rgb[1], bb = rgb[2];
            if (jupiter_band && sp_abs(dy16) <= 22) {
                /* Subtle cream equatorial cloud belt on Jupiter */
                br = sp_clamp(br + 24, 0, 255);
                bg = sp_clamp(bg + 26, 0, 255);
                bb = sp_clamp(bb + 34, 0, 255);
            }

            const int cr = ((br * shade) >> 8) + spec;
            const int cg = ((bg * shade) >> 8) + spec;
            const int cb = ((bb * shade) >> 8) + spec;
            const uint16_t col = sp_rgb565(cr, cg, cb);
            const unsigned a = (unsigned)((cov * strength) >> 8);
            row[x] = glass_blend565(row[x], col, a, swap_bytes);
        }
    }
}

/* Draw half of Saturn's tilted golden elliptical ring (`front` selects front
 * vs back half so it wraps in true 3D around Saturn's sphere!). */
static void orrery_saturn_ring(uint16_t *dst, int y0, int y1, bool swap_bytes,
                               int cx16, int cy16, bool front, int strength)
{
    /* Tilted 24 deg: cos(24)=30000, sin(24)=13328 in Q15 */
    const int32_t c_t = 30000;
    const int32_t s_t = 13328;
    const int cy_i = cy16 >> 4;
    const int cx_i = cx16 >> 4;
    int ry0 = cy_i - 10;
    int ry1 = cy_i + 10;
    if (ry0 < y0) { ry0 = y0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry0 > ry1) {
        return;
    }
    int rx0 = cx_i - 15;
    int rx1 = cx_i + 15;
    if (rx0 < 0) { rx0 = 0; }
    if (rx1 >= SP_W) { rx1 = SP_W - 1; }

    /* Ellipse semi-axes: a = 12.5 px (200 Q4), b = 4.2 px (67 Q4).
     * Ring band normalized radius u_norm in [0.68, 1.04]. */
    const uint16_t ring_hi = 0xFEEFu; /* warm champagne gold */
    const uint16_t ring_lo = 0xC4A7u; /* antique rose-gold */

    for (int y = ry0; y <= ry1; ++y) {
        const int32_t dy = y * 16 + 8 - cy16;
        uint16_t *row = dst + (size_t)(y - y0) * SP_W;
        for (int x = rx0; x <= rx1; ++x) {
            const int32_t dx = x * 16 + 8 - cx16;
            /* Rotate into ring plane */
            const int32_t u = (int32_t)(((int64_t)dx * c_t + (int64_t)dy * s_t) >> 15);
            const int32_t v = (int32_t)(((int64_t)-dx * s_t + (int64_t)dy * c_t) >> 15);
            if (front ? (v < 0) : (v >= 0)) {
                continue;
            }
            /* Normalized ellipse distance^2 in Q8 (1.0 -> 256^2 = 65536) */
            const int32_t un = (u * 256) / 200;
            const int32_t vn = (v * 256) / 67;
            const int32_t e2 = un * un + vn * vn;
            if (e2 < 31000 || e2 > 76000) {
                continue;
            }
            const int32_t e = (int32_t)sp_isqrt((uint32_t)e2); /* ~176..275 */
            /* Peak band between 198..252 with smooth AA edges */
            int cov;
            if (e < 198) {
                cov = ((e - 176) * 255) / 22;
            } else if (e > 252) {
                cov = ((275 - e) * 255) / 23;
            } else {
                cov = 255;
            }
            cov = sp_clamp(cov, 0, 255);
            if (cov <= 0) {
                continue;
            }
            /* Subtle Cassini-style brightness gradient */
            const uint16_t col = e > 224 ? ring_hi : ring_lo;
            const unsigned a = (unsigned)((cov * (front ? 235 : 175) * strength) >> 16);
            row[x] = glass_blend565(row[x], col, a, swap_bytes);
        }
    }
}

/* Accurate Live Synodic Moon Phase Disc (r=13 at cx=233, cy=398) inside a thin
 * rose-gold ring, rendering the true curved lunar terminator. */
#define ORRERY_MOON_CX    233
#define ORRERY_MOON_CY    398
#define ORRERY_MOON_R     13

static void orrery_moon_phase_rows(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                   uint32_t phase_q16, int strength)
{
    int ry0 = ORRERY_MOON_CY - ORRERY_MOON_R - 3;
    int ry1 = ORRERY_MOON_CY + ORRERY_MOON_R + 3;
    if (ry0 < y0) { ry0 = y0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry0 > ry1) {
        return;
    }

    const int32_t cx16 = ORRERY_MOON_CX * 16;
    const int32_t cy16 = ORRERY_MOON_CY * 16;
    const int32_t r_disc = ORRERY_MOON_R * 16;
    const int32_t r_ring_out = ORRERY_MOON_R * 16 + 24; /* +1.5 px rose-gold bezel */
    const int32_t cos_phi = glass_cos_q15(phase_q16);
    const bool waxing = (phase_q16 < 32768u);

    const uint16_t bezel_col = 0xE58Fu; /* brushed rose-gold bezel */
    const uint16_t dark_col  = 0x10C4u; /* subtle midnight earthshine */
    const uint16_t lit_col   = 0xEF5Bu; /* warm lunar ivory-silver */
    const uint16_t mare_col  = 0xBDD6u; /* subtle lunar maria */

    /* Soft moonlight halo around the moon medallion proportional to illumination */
    const int illum = (32768 - cos_phi) >> 8; /* 0 (new) .. 256 (full) */
    if (illum > 24) {
        sp_glow_orb_rows(dst, y0, y1, swap_bytes, cx16, cy16, ORRERY_MOON_R + 4,
                         0x9DFDu, (uint8_t)(((illum * 50) >> 8) * strength >> 8), 214);
    }

    const int rx0 = ORRERY_MOON_CX - ORRERY_MOON_R - 3;
    const int rx1 = ORRERY_MOON_CX + ORRERY_MOON_R + 3;
    for (int y = ry0; y <= ry1; ++y) {
        const int32_t dy16 = y * 16 + 8 - cy16;
        const int32_t dy_sq = dy16 * dy16;
        uint16_t *row = dst + (size_t)(y - y0) * SP_W;

        const int32_t chord_sq = r_disc * r_disc - dy_sq;
        const int32_t half_w = chord_sq > 0 ? (int32_t)sp_isqrt((uint32_t)chord_sq) : 0;
        /* Curved lunar terminator x-position on this scanline in Q4 */
        const int32_t term_x = (int32_t)(((int64_t)half_w * cos_phi) >> 15);

        for (int x = rx0; x <= rx1; ++x) {
            const int32_t dx16 = x * 16 + 8 - cx16;
            const int32_t d = (int32_t)sp_isqrt((uint32_t)(dx16 * dx16 + dy_sq));
            if (d > r_ring_out + 8) {
                continue;
            }
            /* Inside lunar disc */
            if (d < r_disc + 4) {
                /* Signed distance from curved terminator (>0 = sunlit side) */
                int32_t lit_dist = waxing ? (dx16 - term_x) : (-term_x - dx16);
                /* Smooth 2 px anti-aliased terminator edge */
                int lit_f = sp_clamp(((lit_dist + 16) * 255) / 32, 0, 255);

                /* Subtle smooth lunar maria & spherical limb shading */
                uint16_t surf = lit_col;
                const int32_t m1 = (dx16 + 46) * (dx16 + 46) + (dy16 + 38) * (dy16 + 38);
                const int32_t m2 = (dx16 - 52) * (dx16 - 52) + (dy16 - 22) * (dy16 - 22);
                const int32_t m3 = (dx16 + 18) * (dx16 + 18) + (dy16 - 58) * (dy16 - 58);
                int mare_w = 0;
                if (m1 < 72 * 72) {
                    mare_w += ((72 * 72 - m1) * 140) / (72 * 72);
                }
                if (m2 < 60 * 60) {
                    mare_w += ((60 * 60 - m2) * 120) / (60 * 60);
                }
                if (m3 < 52 * 52) {
                    mare_w += ((52 * 52 - m3) * 110) / (52 * 52);
                }
                if (mare_w > 0) {
                    surf = sp_lerp565(lit_col, mare_col, sp_clamp(mare_w, 0, 210));
                }
                /* Gentle spherical limb darkening near lunar rim */
                if (d > r_disc - 48) {
                    const int limb = sp_clamp(((d - (r_disc - 48)) * 90) / 48, 0, 90);
                    surf = sp_lerp565(surf, dark_col, limb);
                }
                const uint16_t col = sp_lerp565(dark_col, surf, lit_f);
                const int cov = d > r_disc - 8 ? sp_clamp(((r_disc + 4 - d) * 255) / 12, 0, 255) : 255;
                row[x] = glass_blend565(row[x], col, (unsigned)((cov * strength) >> 8), swap_bytes);
            }
            /* Thin rose-gold bezel ring at r_disc..r_ring_out */
            if (d >= r_disc - 6 && d <= r_ring_out + 8) {
                int rcov;
                if (d < r_disc + 4) {
                    rcov = ((d - (r_disc - 6)) * 255) / 10;
                } else if (d > r_ring_out - 4) {
                    rcov = ((r_ring_out + 8 - d) * 255) / 12;
                } else {
                    rcov = 255;
                }
                rcov = sp_clamp(rcov, 0, 255);
                if (rcov > 0) {
                    row[x] = glass_blend565(row[x], bezel_col,
                                            (unsigned)((rcov * 215 * strength) >> 16), swap_bytes);
                }
            }
        }
    }
}

void jr_showpiece_orrery_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                              uint32_t now_ms, uint16_t year, uint8_t month, uint8_t day,
                              int hh, int mm, int ss,
                              int tilt_x, int tilt_y,
                              const hud_light_t *light, bool alive, int strength)
{
    if (dst == NULL || nrows <= 0 || strength <= 0) {
        return;
    }
    const int y1 = y0 + nrows;
    if (strength > 255) {
        strength = 255;
    }

    jr_orrery_eph_t eph;
    jr_orrery_ephemeris(year, month, day, hh, mm, ss, &eph);

    const int32_t lx_q15 = light ? light->bevel_lx : -19661;
    const int32_t ly_q15 = light ? light->bevel_ly : -26214;
    const int tx = alive ? tilt_x : 0;
    const int ty = alive ? tilt_y : 0;

    /* 1. Subtle glowing central sun corona shimmer around rose-gold medallion (r=26..34) */
    if (y0 <= 233 + 36 && y1 >= 232 - 36) {
        const int shimmer = alive
            ? (180 + ((glass_sin_q15((now_ms * 20u) & 0xFFFFu) * 55) >> 15))
            : 170;
        const int32_t r_in = 25 * 16;
        const int32_t r_out = 35 * 16;
        const int ry0 = y0 < 232 - 36 ? 232 - 36 : y0;
        const int ry1 = (y1 - 1) > 233 + 36 ? 233 + 36 : (y1 - 1);
        for (int y = ry0; y <= ry1; ++y) {
            const int32_t dy = y * 16 + 8 - SP_CY16;
            const int32_t dy2 = dy * dy;
            if (dy2 >= r_out * r_out) {
                continue;
            }
            uint16_t *row = dst + (size_t)(y - y0) * SP_W;
            for (int x = 232 - 36; x <= 233 + 36; ++x) {
                const int32_t dx = x * 16 + 8 - SP_CX16;
                const int32_t d2 = dx * dx + dy2;
                if (d2 < r_in * r_in || d2 >= r_out * r_out) {
                    continue;
                }
                const int32_t d = (int32_t)sp_isqrt((uint32_t)d2);
                const int peak_d = 28 * 16;
                int fall = d < peak_d ? ((d - r_in) * 255) / (peak_d - r_in)
                                      : ((r_out - d) * 255) / (r_out - peak_d);
                fall = sp_clamp(fall, 0, 255);
                const unsigned a = (unsigned)((fall * fall * shimmer * strength) >> 24);
                if (a > 0u) {
                    row[x] = sp_add565(row[x], 0xFDA5u, a, swap_bytes);
                }
            }
        }
    }

    /* 2. All 6 planets snapped to measured golden radii with 28-deg fading trails,
     *    3D IMU parallax, radial OLED halos, shaded spheres, Earth's Moon & Saturn's ring! */
    for (int p = 0; p < JR_ORRERY_PLANETS; ++p) {
        const int orbit_r = s_orrery_orbit_r[p];
        const int pr = s_orrery_planet_r[p];
        const int32_t ox16 = SP_CX16 + ((tx * s_orrery_parallax[p] * 16) >> 8);
        const int32_t oy16 = SP_CY16 + ((ty * s_orrery_parallax[p] * 16) >> 8);
        const uint32_t ang = eph.planet_q16[p];

        /* 2a. 28-degree smooth fading luminous orbital trail arc behind the planet */
        /* Prograde motion is decreasing screen angle, so trail sits at ang + k*step */
        const int trail_dots = sp_clamp(orbit_r / 4, 14, 44);
        const uint32_t trail_span_q16 = 5097u; /* 28 deg */
        for (int k = 1; k <= trail_dots; ++k) {
            const uint32_t ta = (ang + (trail_span_q16 * (uint32_t)k) / (uint32_t)trail_dots) & 0xFFFFu;
            const int32_t tx16 = ox16 + ((orbit_r * 16 * glass_cos_q15(ta)) >> 15);
            const int32_t ty16 = oy16 + ((orbit_r * 16 * glass_sin_q15(ta)) >> 15);
            const int ty_i = ty16 >> 4;
            if (ty_i + 3 < y0 || ty_i - 3 >= y1) {
                continue;
            }
            const int inv = trail_dots + 1 - k;
            const int ta_peak = (((inv * inv * 95) / (trail_dots * trail_dots)) * strength) >> 8;
            sp_glow_orb_rows(dst, y0, y1, swap_bytes, tx16, ty16, 2,
                             s_planet_halo565[p], (uint8_t)sp_clamp(ta_peak, 0, 255), 214);
        }

        /* 2b. Planet center */
        const int32_t px16 = ox16 + ((orbit_r * 16 * glass_cos_q15(ang)) >> 15);
        const int32_t py16 = oy16 + ((orbit_r * 16 * glass_sin_q15(ang)) >> 15);
        const int py_i = py16 >> 4;
        const int reach = (p == 2) ? 18 : ((p == 5) ? 16 : (pr + 6));
        if (py_i + reach < y0 || py_i - reach >= y1) {
            continue;
        }

        /* Soft radial OLED halo */
        sp_glow_orb_rows(dst, y0, y1, swap_bytes, px16, py16, pr + 5,
                         s_planet_halo565[p], (uint8_t)((135 * strength) >> 8), 214);

        /* Saturn back ring half behind planet */
        if (p == 5) {
            orrery_saturn_ring(dst, y0, y1, swap_bytes, px16, py16, false, strength);
        }

        /* Shaded 3D jewel planet orb */
        orrery_planet_sphere(dst, y0, y1, swap_bytes, px16, py16, pr,
                             s_planet_rgb[p], (p == 4), lx_q15, ly_q15, strength);

        /* Saturn front ring half in front of planet */
        if (p == 5) {
            orrery_saturn_ring(dst, y0, y1, swap_bytes, px16, py16, true, strength);
        }

        /* Earth's tiny orbiting silver Moon at r=12 px */
        if (p == 2) {
            const int32_t mx16 = px16 + ((12 * 16 * glass_cos_q15(eph.earth_moon_q16)) >> 15);
            const int32_t my16 = py16 + ((12 * 16 * glass_sin_q15(eph.earth_moon_q16)) >> 15);
            static const uint8_t moon_rgb[3] = { 226, 232, 242 };
            sp_glow_orb_rows(dst, y0, y1, swap_bytes, mx16, my16, 4,
                             0xCE7Fu, (uint8_t)((110 * strength) >> 8), 214);
            orrery_planet_sphere(dst, y0, y1, swap_bytes, mx16, my16, 2,
                                 moon_rgb, false, lx_q15, ly_q15, strength);
        }
    }

    /* 3. Accurate Live Synodic Moon Phase Disc at 6 o'clock (cx=233, cy=398, r=13) */
    orrery_moon_phase_rows(dst, y0, y1, swap_bytes, eph.moon_phase_q16, strength);
}

/* ==========================================================================
 * SHOWPIECE #2: WATCH:REACTOR (32-BAR RADIAL SPECTRUM + 3D HOLO + HH:MM)
 *
 * Strictly confined inside dial_reactor.eaf's recessed chamber (r <= 122).
 * ========================================================================== */

#define REACTOR_R_CHAMBER  122
#define REACTOR_BARS       32
#define REACTOR_BAR_R0     82
#define REACTOR_BAR_R1_MAX 119
#define REACTOR_HOLO_R     72
#define REACTOR_PIP_R      120

typedef struct {
    int16_t y_min, y_max;
    int16_t cos_q15, sin_q15;
    int16_t r_end16;   /* outer radius of bar in Q4 px */
    uint8_t level;     /* 0..255 */
} reactor_bar_t;

typedef struct {
    bool     valid;
    uint32_t now_ms;
    int      ss, ms;
    bool     still, alive;
    uint8_t  bands[16];
    uint8_t  amp;
    int      energy;
    reactor_bar_t bar[REACTOR_BARS];
} reactor_cache_t;

static reactor_cache_t s_reactor_cache;

static void reactor_prepare_bars(reactor_cache_t *rc, uint32_t now_ms,
                                 int ss, int ms, bool still, bool alive,
                                 const uint8_t bands[16], uint8_t amp)
{
    uint8_t zero[16] = { 0 };
    const uint8_t *b = bands ? bands : zero;

    if (rc->valid && rc->now_ms == now_ms && rc->ss == ss && rc->ms == ms &&
        rc->still == still && rc->alive == alive && rc->amp == amp &&
        memcmp(rc->bands, b, 16) == 0) {
        return;
    }

    rc->valid = true;
    rc->now_ms = now_ms;
    rc->ss = ss;
    rc->ms = ms;
    rc->still = still;
    rc->alive = alive;
    rc->amp = amp;
    memcpy(rc->bands, b, 16);

    int sum = 0;
    for (int i = 0; i < 16; ++i) {
        sum += b[i];
    }
    const bool has_spec = (sum > 0);
    rc->energy = has_spec ? (sum >> 4) : (int)amp;

    /* Live seconds phase in Q16 turns (12 o'clock clockwise) */
    const uint32_t sec_q16 = (GLASS_Q16_12_OCLOCK +
                              (uint32_t)(((int64_t)(ss * 1000 + ms) * 65536LL) / 60000LL)) & 0xFFFFu;
    const uint32_t breath_q16 = (now_ms * 22u) & 0xFFFFu;

    for (int k = 0; k < REACTOR_BARS; ++k) {
        const uint32_t ang = (GLASS_Q16_12_OCLOCK + (uint32_t)k * 2048u) & 0xFFFFu;
        const int32_t ca = glass_cos_q15(ang);
        const int32_t sa = glass_sin_q15(ang);

        int val = 0;
        if (has_spec) {
            /* Mirror 16 live bands around the 32-bar ring symmetrically */
            const int idx = k < 16 ? k : (31 - k);
            const int next_idx = idx < 15 ? idx + 1 : 15;
            val = (k & 1) ? (((int)b[idx] * 2 + b[next_idx]) / 3) : (int)b[idx];
        } else {
            /* Mesmerizing harmonic breathing wave + sweeping plasma crest synced to seconds */
            const int w1 = glass_sin_q15(((uint32_t)k * 4096u + breath_q16) & 0xFFFFu);
            const int w2 = glass_sin_q15(((uint32_t)k * 6144u - (breath_q16 >> 1)) & 0xFFFFu);
            val = 36 + (((w1 * 22) + (w2 * 14)) >> 15);
            if (!still) {
                /* Sweeping plasma highlight crest chasing the seconds pip */
                int diff = (int)((ang - sec_q16) & 0xFFFFu);
                if (diff > 32768) { diff -= 65536; }
                const int ad = sp_abs(diff);
                if (ad < 8192) {
                    const int boost = ((8192 - ad) * (8192 - ad)) >> 19; /* up to ~128 */
                    val += boost;
                }
            }
            if (amp > 0u) {
                val += (int)(amp >> 2);
            }
            val = sp_clamp(val, 14, 210);
        }

        const int len_px = 4 + ((val * (REACTOR_BAR_R1_MAX - REACTOR_BAR_R0 - 4)) / 255);
        const int32_t r1_16 = (REACTOR_BAR_R0 + len_px) * 16;
        const int32_t r0_16 = REACTOR_BAR_R0 * 16;

        const int32_t y_a = SP_CY16 + ((r0_16 * sa) >> 15);
        const int32_t y_b = SP_CY16 + ((r1_16 * sa) >> 15);
        const int ylo = (y_a < y_b ? y_a : y_b) - 48;
        const int yhi = (y_a > y_b ? y_a : y_b) + 48;

        rc->bar[k].cos_q15 = (int16_t)ca;
        rc->bar[k].sin_q15 = (int16_t)sa;
        rc->bar[k].r_end16 = (int16_t)r1_16;
        rc->bar[k].level = (uint8_t)sp_clamp(val, 0, 255);
        rc->bar[k].y_min = (int16_t)sp_clamp(ylo >> 4, 0, SP_H - 1);
        rc->bar[k].y_max = (int16_t)sp_clamp((yhi + 15) >> 4, 0, SP_H - 1);
    }
}

void jr_showpiece_reactor_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                               uint32_t now_ms, int hh, int mm, int ss, int ms,
                               bool still, bool alive, int tilt_x, int tilt_y,
                               const uint8_t bands[16], uint8_t amp, int strength)
{
    if (dst == NULL || nrows <= 0 || strength <= 0) {
        return;
    }
    const int y1 = y0 + nrows;
    if (y1 <= 232 - REACTOR_R_CHAMBER || y0 > 233 + REACTOR_R_CHAMBER) {
        return;
    }
    if (strength > 255) {
        strength = 255;
    }

    hh = ((hh % 24) + 24) % 24;
    mm = ((mm % 60) + 60) % 60;
    ss = ((ss % 60) + 60) % 60;
    ms = sp_clamp(ms, 0, 999);

    reactor_prepare_bars(&s_reactor_cache, now_ms, ss, ms, still, alive, bands, amp);
    const reactor_cache_t *rc = &s_reactor_cache;

    /* 1. Subtle recessed core ambient cobalt-cyan glow strictly inside r <= 118 */
    {
        const int amb_a = ((28 + (rc->energy >> 3)) * strength) >> 8;
        sp_glow_orb_rows(dst, y0, y1, swap_bytes, SP_CX16, SP_CY16, 118,
                         0x0255u, (uint8_t)sp_clamp(amb_a, 0, 72), REACTOR_R_CHAMBER);
    }

    /* 2. 32-Bar Radial Electric-Cyan Audio Spectrum (r = 82..119) */
    {
        const int32_t r0_16 = REACTOR_BAR_R0 * 16;
        const int32_t half_w16 = 34; /* ~2.1 px half-width -> crisp 4.2 px wide radial bars */
        const uint16_t col_base = 0x027Bu; /* deep electric cobalt */
        const uint16_t col_mid  = 0x07FFu; /* neon electric cyan */
        const uint16_t col_tip  = 0xEFFFu; /* bright ice-white tip */

        for (int k = 0; k < REACTOR_BARS; ++k) {
            const reactor_bar_t *rb = &rc->bar[k];
            int ry0 = rb->y_min;
            int ry1 = rb->y_max;
            if (ry0 < y0) { ry0 = y0; }
            if (ry1 >= y1) { ry1 = y1 - 1; }
            if (ry0 > ry1) {
                continue;
            }
            const int32_t ca = rb->cos_q15;
            const int32_t sa = rb->sin_q15;
            const int32_t xa = SP_CX16 + ((r0_16 * ca) >> 15);
            const int32_t xb = SP_CX16 + ((rb->r_end16 * ca) >> 15);
            int xlo = ((xa < xb ? xa : xb) - 48) >> 4;
            int xhi = ((xa > xb ? xa : xb) + 64) >> 4;
            if (xlo < 0) { xlo = 0; }
            if (xhi >= SP_W) { xhi = SP_W - 1; }

            const int32_t bar_len = rb->r_end16 - r0_16;
            for (int y = ry0; y <= ry1; ++y) {
                const int32_t dy = y * 16 + 8 - SP_CY16;
                uint16_t *row = dst + (size_t)(y - y0) * SP_W;
                for (int x = xlo; x <= xhi; ++x) {
                    if (!sp_in_circle(x, y, REACTOR_R_CHAMBER)) {
                        continue;
                    }
                    const int32_t dx = x * 16 + 8 - SP_CX16;
                    /* Projection along bar axis (radial distance) and perpendicular */
                    const int32_t rad = (int32_t)(((int64_t)dx * ca + (int64_t)dy * sa) >> 15);
                    if (rad < r0_16 - 8 || rad > rb->r_end16 + 8) {
                        continue;
                    }
                    const int32_t perp = sp_abs((int32_t)(((int64_t)-dx * sa + (int64_t)dy * ca) >> 15));
                    if (perp >= half_w16) {
                        continue;
                    }

                    int cov = ((half_w16 - perp) * 255) / 14;
                    if (cov > 255) { cov = 255; }
                    if (rad < r0_16) {
                        cov = (cov * (rad - (r0_16 - 8))) >> 3;
                    } else if (rad > rb->r_end16) {
                        cov = (cov * ((rb->r_end16 + 8) - rad)) >> 3;
                    }
                    if (cov <= 0) {
                        continue;
                    }

                    /* 3-stop OLED radial gradient: cobalt base -> cyan core -> ice-white tip */
                    uint16_t col;
                    if (rb->r_end16 - rad <= 56) {
                        const int tf = sp_clamp(((56 - (rb->r_end16 - rad)) * 255) / 56, 0, 255);
                        col = sp_lerp565(col_mid, col_tip, tf);
                    } else {
                        const int bf = bar_len > 0 ? sp_clamp(((rad - r0_16) * 255) / (bar_len > 192 ? 192 : bar_len), 0, 255) : 255;
                        col = sp_lerp565(col_base, col_mid, bf);
                    }
                    const unsigned a = (unsigned)((cov * strength) >> 8);
                    row[x] = glass_blend565(row[x], col, a, swap_bytes);
                }
            }
        }
    }

    /* 3. Thin glowing inner retaining ring at r=76 separating 3D Holo from Spectrum */
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = dst + (size_t)(y - y0) * SP_W;
        glass_ring_row(row, y, 232 - 80, 233 + 80, SP_CX16, SP_CY16,
                       75 * 16 + 8, 77 * 16, 0, GLASS_TURN_Q16,
                       0x04B9u, (uint8_t)((120 * strength) >> 8), swap_bytes);
    }

    /* 4. Real-Time 3D Rotating Geodesic Hologram Sphere in the dead center (r <= 72) */
    jr_showpiece_holo_rows(dst, y0, nrows, swap_bytes,
                           still ? 0u : now_ms, REACTOR_HOLO_R, JR_HOLO_REACTOR,
                           alive ? tilt_x : 0, alive ? tilt_y : 0,
                           bands, amp, strength);

    /* 5. Crisp Luminous Digital Time Readout (HH:MM) with subtle dark backing pill */
    {
        char time_str[6];
        time_str[0] = (char)('0' + (hh / 10));
        time_str[1] = (char)('0' + (hh % 10));
        time_str[2] = ':';
        time_str[3] = (char)('0' + (mm / 10));
        time_str[4] = (char)('0' + (mm % 10));
        time_str[5] = '\0';

        const glass_metrics_t mv = glass_font_metrics(GLASS_FONT_VALUE);
        const int track = 2;
        const int tw = glass_text_width(GLASS_FONT_VALUE, time_str, 5u, track);
        const int tx = (SP_W - tw) / 2 + track / 2;
        const int ty = (SP_H - mv.cap_h) / 2;

        /* Subtle dark elliptical backing halo so digits read sharply over 3D lattice */
        const int rx_pill = tw / 2 + 12;
        const int ry_pill = mv.cap_h / 2 + 8;
        int py0 = 233 - ry_pill;
        int py1 = 233 + ry_pill;
        if (py0 < y0) { py0 = y0; }
        if (py1 >= y1) { py1 = y1 - 1; }
        for (int y = py0; y <= py1; ++y) {
            const int dy = y - 233;
            const int dy_n = (dy * 256) / ry_pill;
            const int rem = 256 * 256 - dy_n * dy_n;
            if (rem <= 0) {
                continue;
            }
            const int half_x = (rx_pill * (int)sp_isqrt((uint32_t)rem)) >> 8;
            const int xlo = sp_clamp(233 - half_x, 0, SP_W - 1);
            const int xhi = sp_clamp(233 + half_x, 0, SP_W - 1);
            uint16_t *row = dst + (size_t)(y - y0) * SP_W;
            for (int x = xlo; x <= xhi; ++x) {
                const int dx_n = ((x - 233) * 256) / rx_pill;
                const int e2 = dx_n * dx_n + dy_n * dy_n;
                if (e2 >= 65536) {
                    continue;
                }
                const int fade = (65536 - e2) >> 8; /* 0..256 */
                const unsigned da = (unsigned)((fade * 175 * strength) >> 16);
                row[x] = glass_blend565(row[x], 0x0000u, da, swap_bytes);
            }
        }

        /* Cyan subtle shadow + crisp ice-white HH:MM digits */
        const int text_y0 = ty + mv.ink_top - 1;
        const int text_y1 = ty + mv.ink_bottom + 1;
        for (int y = y0; y < y1; ++y) {
            if (y < text_y0 || y > text_y1) {
                continue;
            }
            uint16_t *row = dst + (size_t)(y - y0) * SP_W;
            glass_text_row(row, y, 232 - 70, 233 + 70, GLASS_FONT_VALUE,
                           time_str, 5u, tx, ty + 1, track,
                           0x07FFu, (uint8_t)((120 * strength) >> 8), swap_bytes);
            glass_text_row(row, y, 232 - 70, 233 + 70, GLASS_FONT_VALUE,
                           time_str, 5u, tx, ty, track,
                           0xF7FFu, (uint8_t)strength, swap_bytes);
        }
    }

    /* 6. Sweeping electric-cyan seconds pip orbiting the r=120 steel retaining ring */
    if (!still) {
        const uint32_t sec_q16 = (GLASS_Q16_12_OCLOCK +
                                  (uint32_t)(((int64_t)(ss * 1000 + ms) * 65536LL) / 60000LL)) & 0xFFFFu;
        /* Tiny 6-dot fading cyan plasma wake behind pip */
        for (int k = 1; k <= 6; ++k) {
            const uint32_t wa = (sec_q16 - (uint32_t)k * 360u) & 0xFFFFu;
            const int32_t wx16 = SP_CX16 + ((REACTOR_PIP_R * 16 * glass_cos_q15(wa)) >> 15);
            const int32_t wy16 = SP_CY16 + ((REACTOR_PIP_R * 16 * glass_sin_q15(wa)) >> 15);
            const int a = (((7 - k) * 28) * strength) >> 8;
            sp_glow_orb_rows(dst, y0, y1, swap_bytes, wx16, wy16, 2,
                             0x07FFu, (uint8_t)sp_clamp(a, 0, 255), REACTOR_R_CHAMBER);
        }
        /* Bright electric-cyan halo + ice-white pip core (clipped strictly to r <= 122) */
        const int32_t px16 = SP_CX16 + ((REACTOR_PIP_R * 16 * glass_cos_q15(sec_q16)) >> 15);
        const int32_t py16 = SP_CY16 + ((REACTOR_PIP_R * 16 * glass_sin_q15(sec_q16)) >> 15);
        sp_glow_orb_rows(dst, y0, y1, swap_bytes, px16, py16, 5,
                         0x07FFu, (uint8_t)((235 * strength) >> 8), REACTOR_R_CHAMBER);
        sp_glow_orb_rows(dst, y0, y1, swap_bytes, px16, py16, 2,
                         0xFFFFu, (uint8_t)strength, REACTOR_R_CHAMBER);
    }
}
