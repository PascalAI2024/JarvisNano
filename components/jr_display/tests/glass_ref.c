/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * TEST ONLY. The slow reference for the glass shape primitives: a verbatim
 * copy of glass_draw.c as it stood before the performance pass (per-pixel
 * coverage everywhere, geometry rebuilt per row). The optimised code in
 * src/glass_draw.c must reproduce its output; test_glass.c compares them.
 * Do not "fix" this file; it is the definition of the pixels.
 */
#include "jr_display/glass.h"

/* ---- small helpers -------------------------------------------------------- */

static inline int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }
static inline int32_t imin32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t imax32(int32_t a, int32_t b) { return a > b ? a : b; }

/* floor(sqrt(n)), digit by digit. */
static uint32_t isqrt_u32(uint32_t n)
{
    uint32_t res = 0, bit = 1u << 30;
    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

/* Distance in 1/64 px from a squared distance in 1/16 px units. */
static inline uint32_t dist64(uint32_t d2)
{
    if (d2 > 0x0FFFFFFFu) {
        d2 = 0x0FFFFFFFu;
    }
    return isqrt_u32(d2 << 4);
}

/* Coverage of a disc of radius r16 at distance d64. */
static inline int edge_cov(int32_t r16, uint32_t d64)
{
    const int32_t v = 128 + (r16 * 4 - (int32_t)d64) * 4;
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* First / last pixel whose centre is within T (1/16 px) of cx. */
static inline int x_lo(int32_t cx, int32_t t) { return (int)((cx - t - 8 + 15) >> 4); }
static inline int x_hi(int32_t cx, int32_t t) { return (int)((cx + t - 8) >> 4); }

typedef struct {
    uint16_t color;  /* native */
    uint16_t panel;  /* as stored in the row */
    unsigned alpha;
    bool swap;
} paint_t;

static void paint_init(paint_t *p, uint16_t color, uint8_t alpha, bool swap)
{
    p->color = color;
    p->panel = swap ? glass_bswap(color) : color;
    p->alpha = alpha;
    p->swap = swap;
}

/* Clamp the row request. False means nothing to draw. */
static bool row_clip(int y, int *lo, int *hi)
{
    if (y < 0 || y >= GLASS_PANEL_PX) {
        return false;
    }
    if (*lo < 0) *lo = 0;
    if (*hi > GLASS_PANEL_PX - 1) *hi = GLASS_PANEL_PX - 1;
    return *lo <= *hi;
}

static inline void put(uint16_t *p, const paint_t *c, unsigned cov)
{
    const unsigned a = (cov * (c->alpha + 1u)) >> 8;
    if (a == 0u) {
        return;
    }
    *p = (a >= 255u) ? c->panel : glass_blend565(*p, c->color, a, c->swap);
}

static void fill_run(uint16_t *row, int xa, int xb, const paint_t *c)
{
    if (c->alpha == 0u) {
        return;
    }
    if (c->alpha >= 255u) {
        for (int x = xa; x <= xb; x++) {
            row[x] = c->panel;
        }
    } else {
        for (int x = xa; x <= xb; x++) {
            row[x] = glass_blend565(row[x], c->color, c->alpha, c->swap);
        }
    }
}

/* Coverage of a disc of radius r16 for a pixel centre offset (dx, dy). */
static int disc_cov(int32_t dx, int32_t dy, int32_t r16)
{
    if (r16 <= 0) {
        return 0;
    }
    const uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
    if (r16 > 16 && d2 <= (uint32_t)((r16 - 16) * (r16 - 16))) {
        return 255;
    }
    if (d2 >= (uint32_t)((r16 + 16) * (r16 + 16))) {
        return 0;
    }
    return edge_cov(r16, dist64(d2));
}

/* ---- disc ----------------------------------------------------------------- */

void glassref_disc_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    int32_t cx16, int32_t cy16, int32_t r16,
                    uint16_t color565, uint8_t alpha, bool swap_bytes)
{
    if (row == NULL || r16 <= 0 || alpha == 0u || !row_clip(y, &clip_lo, &clip_hi)) {
        return;
    }
    paint_t c;
    paint_init(&c, color565, alpha, swap_bytes);
    const int32_t dy = y * 16 + 8 - cy16, ady = iabs32(dy);
    if (r16 + 16 <= ady) {
        return;
    }
    const int32_t ho = (int32_t)isqrt_u32((uint32_t)((r16 + 16) * (r16 + 16) - dy * dy));
    const int xa = (int)imax32(clip_lo, x_lo(cx16, ho));
    const int xb = (int)imin32(clip_hi, x_hi(cx16, ho));
    int sa = 1, sb = 0; /* solid run, empty by default */
    if (r16 - 16 > ady) {
        const int32_t si = (int32_t)isqrt_u32((uint32_t)((r16 - 16) * (r16 - 16) - dy * dy));
        sa = x_lo(cx16, si);
        sb = x_hi(cx16, si);
    }
    for (int x = xa; x <= xb; x++) {
        if (x >= sa && x <= sb) {
            const int e = (int)imin32(sb, xb);
            fill_run(row, x, e, &c);
            x = e;
            continue;
        }
        put(&row[x], &c, (unsigned)disc_cov(x * 16 + 8 - cx16, dy, r16));
    }
}

/* ---- ring / arc geometry -------------------------------------------------- */

typedef struct {
    int32_t cx, cy, ri, ro;
    int32_t ri2, ro2;               /* r squared                                */
    int32_t ri_m2, ri_p2;           /* (ri-16)^2 or -1, (ri+16)^2 or 0          */
    int32_t ro_m2, ro_p2;           /* (ro-16)^2 or -1, (ro+16)^2               */
    int32_t sx, sy, ex, ey;         /* start / end unit directions, Q15         */
    int wedge;                      /* 0 full ring, 1 span <= half turn, 2 more */
    bool caps;
    int32_t capr;                   /* cap radius                               */
    int32_t ax, ay, bx, by;         /* cap centres (absolute)                   */
} geo_t;

static bool geo_build(geo_t *g, int32_t cx, int32_t cy, int32_t ri, int32_t ro,
                      int32_t start, int32_t sweep, bool caps)
{
    if (sweep <= 0 || ro <= 0 || ro <= ri) {
        return false;
    }
    if (ri < 0) {
        ri = 0;
    }
    g->cx = cx;
    g->cy = cy;
    g->ri = ri;
    g->ro = ro;
    g->ri2 = ri * ri;
    g->ro2 = ro * ro;
    g->ri_m2 = ri > 16 ? (ri - 16) * (ri - 16) : -1;
    g->ri_p2 = ri > 0 ? (ri + 16) * (ri + 16) : 0;
    g->ro_m2 = ro > 16 ? (ro - 16) * (ro - 16) : -1;
    g->ro_p2 = (ro + 16) * (ro + 16);
    g->wedge = 0;
    g->caps = false;
    g->capr = 0;
    g->sx = g->sy = g->ex = g->ey = 0;
    g->ax = g->ay = g->bx = g->by = 0;
    if (sweep < GLASS_TURN_Q16) {
        const uint32_t s = (uint32_t)start & 0xFFFFu;
        const uint32_t e = (s + (uint32_t)sweep) & 0xFFFFu;
        g->sx = glass_cos_q15(s);
        g->sy = glass_sin_q15(s);
        g->ex = glass_cos_q15(e);
        g->ey = glass_sin_q15(e);
        g->wedge = sweep <= GLASS_TURN_Q16 / 2 ? 1 : 2;
        if (caps) {
            const int32_t rc2 = ri + ro; /* twice the centre-line radius */
            g->caps = true;
            g->capr = (ro - ri) / 2;
            g->ax = cx + ((g->sx * rc2 + 32768) >> 16);
            g->ay = cy + ((g->sy * rc2 + 32768) >> 16);
            g->bx = cx + ((g->ex * rc2 + 32768) >> 16);
            g->by = cy + ((g->ey * rc2 + 32768) >> 16);
        }
    }
    return true;
}

static inline int radial_cov(const geo_t *g, int32_t d2)
{
    if (d2 >= g->ro_p2 || d2 <= g->ri_m2) {
        return 0;
    }
    if (d2 <= g->ro_m2 && d2 >= g->ri_p2) {
        return 255;
    }
    const uint32_t d64 = dist64((uint32_t)d2);
    const int co = edge_cov(g->ro, d64);
    const int ci = g->ri > 0 ? edge_cov(g->ri, d64) : 0;
    return co > ci ? co - ci : 0;
}

/* Anti-aliased membership of the angular span for a pixel offset (dx, dy). */
static inline int wedge_cov(const geo_t *g, int32_t dx, int32_t dy)
{
    int32_t cs = 128 + ((g->sx * dy - g->sy * dx) >> 11); /* 1/256 px from the start line */
    int32_t ce = 128 + ((dx * g->ey - dy * g->ex) >> 11); /* ... from the end line        */
    cs = cs < 0 ? 0 : (cs > 255 ? 255 : cs);
    ce = ce < 0 ? 0 : (ce > 255 ? 255 : ce);
    if (g->wedge == 1) {
        const int32_t v = cs + ce - 255; /* both half planes */
        return v < 0 ? 0 : v;
    }
    const int32_t v = cs + ce; /* either half plane */
    return v > 255 ? 255 : v;
}

static int shape_cov(const geo_t *g, int32_t px, int32_t py)
{
    const int32_t dx = px - g->cx, dy = py - g->cy;
    int cov = radial_cov(g, dx * dx + dy * dy);
    if (g->wedge != 0 && cov != 0) {
        cov = (cov * wedge_cov(g, dx, dy) + 127) / 255;
    }
    if (g->caps) {
        /* Union by max: where a cap meets the body both are at full coverage
         * and there is nothing to double, and along the shared tangent edge
         * max keeps the single-edge coverage a probabilistic union would
         * darken. */
        cov = (int)imax32(cov, disc_cov(px - g->ax, py - g->ay, g->capr));
        cov = (int)imax32(cov, disc_cov(px - g->bx, py - g->by, g->capr));
    }
    return cov;
}

static void ring_like_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                          const geo_t *g, uint16_t color565, uint8_t alpha, bool swap_bytes)
{
    if (row == NULL || alpha == 0u || !row_clip(y, &clip_lo, &clip_hi)) {
        return;
    }
    paint_t c;
    paint_init(&c, color565, alpha, swap_bytes);
    const int32_t dy = y * 16 + 8 - g->cy, ady = iabs32(dy);
    if (g->ro + 16 <= ady) {
        return;
    }
    const int32_t ho = (int32_t)isqrt_u32((uint32_t)(g->ro_p2 - dy * dy));
    const int xa = (int)imax32(clip_lo, x_lo(g->cx, ho));
    const int xb = (int)imin32(clip_hi, x_hi(g->cx, ho));
    /* The hole: every pixel within he of the centre is empty. */
    int32_t he = -1;
    if (g->ri - 16 > ady) {
        he = (int32_t)isqrt_u32((uint32_t)(g->ri_m2 - dy * dy));
    }
    /* The solid body (full rings only): (si, so] from the centre. */
    const bool fast = g->wedge == 0 && !g->caps;
    int32_t so = -1, si = -1;
    if (fast && g->ro - 16 > ady) {
        so = (int32_t)isqrt_u32((uint32_t)(g->ro_m2 - dy * dy));
        if (g->ri > 0 && g->ri + 16 > ady) {
            si = (int32_t)isqrt_u32((uint32_t)(g->ri_p2 - dy * dy));
        }
    }
    for (int x = xa; x <= xb; x++) {
        const int32_t pdx = x * 16 + 8 - g->cx;
        const int32_t adx = iabs32(pdx);
        if (he >= 0 && adx <= he) {
            x = x_hi(g->cx, he); /* the loop increment steps past it */
            continue;
        }
        if (so >= 0 && adx <= so && adx > si) {
            int e = x_hi(g->cx, so);
            if (pdx < 0 && si >= 0) {
                e = (int)imin32(e, x_lo(g->cx, si) - 1);
            }
            e = (int)imin32(e, xb);
            fill_run(row, x, e, &c);
            x = e;
            continue;
        }
        put(&row[x], &c, (unsigned)shape_cov(g, x * 16 + 8, y * 16 + 8));
    }
}

void glassref_ring_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                    int32_t start_q16, int32_t sweep_q16,
                    uint16_t color565, uint8_t alpha, bool swap_bytes)
{
    geo_t g;
    if (geo_build(&g, cx16, cy16, r_in16, r_out16, start_q16, sweep_q16, false)) {
        ring_like_row(row, y, clip_lo, clip_hi, &g, color565, alpha, swap_bytes);
    }
}

void glassref_arc_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                   int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                   int32_t start_q16, int32_t sweep_q16,
                   uint16_t color565, uint8_t alpha, bool swap_bytes)
{
    geo_t g;
    if (geo_build(&g, cx16, cy16, r_in16, r_out16, start_q16, sweep_q16, true)) {
        ring_like_row(row, y, clip_lo, clip_hi, &g, color565, alpha, swap_bytes);
    }
}

/* ---- glow ------------------------------------------------------------------ */

/* Squared distance from offset (dx, dy) to the radial segment [ri, ro] along
 * the unit direction (ux, uy). */
static int32_t seg_d2(const geo_t *g, int32_t dx, int32_t dy, int32_t ux, int32_t uy)
{
    int32_t proj = (dx * ux + dy * uy) >> 15;
    proj = proj < g->ri ? g->ri : (proj > g->ro ? g->ro : proj);
    const int32_t ex = dx - ((ux * proj) >> 15);
    const int32_t ey = dy - ((uy * proj) >> 15);
    return ex * ex + ey * ey;
}

/* Glow coverage 0..255 for a pixel centre. */
static int glow_cov(const geo_t *g, int32_t w16, uint32_t inv, int peak, int32_t px, int32_t py)
{
    const int32_t dx = px - g->cx, dy = py - g->cy;
    const int32_t d2 = dx * dx + dy * dy;
    const int32_t w64 = w16 * 4;
    int32_t t64; /* distance outside the shape, 1/64 px */
    bool in = true;
    if (g->wedge != 0) {
        const bool a = (g->sx * dy - g->sy * dx) >= 0;
        const bool b = (dx * g->ey - dy * g->ex) >= 0;
        in = g->wedge == 1 ? (a && b) : (a || b);
    }
    if (in) {
        if (d2 <= g->ro2 && d2 >= g->ri2) {
            return peak;
        }
        const int32_t d64 = (int32_t)dist64((uint32_t)d2);
        t64 = d64 > g->ro * 4 ? d64 - g->ro * 4 : g->ri * 4 - d64;
    } else if (g->caps) {
        const int32_t da = (px - g->ax) * (px - g->ax) + (py - g->ay) * (py - g->ay);
        const int32_t db = (px - g->bx) * (px - g->bx) + (py - g->by) * (py - g->by);
        const int32_t m = imin32(da, db);
        const int32_t reach = g->capr + w16;
        if (m >= reach * reach) {
            return 0;
        }
        t64 = (int32_t)dist64((uint32_t)m) - g->capr * 4;
    } else {
        const int32_t m = imin32(seg_d2(g, dx, dy, g->sx, g->sy), seg_d2(g, dx, dy, g->ex, g->ey));
        if (m >= w16 * w16) {
            return 0;
        }
        t64 = (int32_t)dist64((uint32_t)m);
    }
    if (t64 <= 0) {
        return peak;
    }
    if (t64 >= w64) {
        return 0;
    }
    const uint32_t u = (((uint32_t)(w64 - t64)) * inv) >> 16; /* 0..256 */
    return (int)(((uint32_t)peak * u * u) >> 16);
}

/* 4x4 Bayer thresholds, 0..15. */
static const uint8_t k_bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

/* Add colour * a/255 to a panel-order pixel, saturating per channel. The sum
 * is kept at 8 bits per channel and quantised with an ordered dither, so a
 * soft ramp that spans only a few 5-bit levels does not band. a=0 returns the
 * pixel untouched. */
static uint16_t add565(uint16_t under, uint16_t col, unsigned a, int x, int y, bool swap)
{
    if (a == 0u) {
        return under;
    }
    const uint32_t u = swap ? glass_bswap(under) : under;
    const uint32_t d = k_bayer[((unsigned)y & 3u) * 4u + ((unsigned)x & 3u)];
    const uint32_t cr = col >> 11, cg = (col >> 5) & 63u, cb = col & 31u;
    const uint32_t r8 = (cr << 3) | (cr >> 2), g8 = (cg << 2) | (cg >> 4), b8 = (cb << 3) | (cb >> 2);
    uint32_t r = (((u >> 11) << 3) + (r8 * a + 127u) / 255u + (d >> 1)) >> 3;
    uint32_t g = ((((u >> 5) & 63u) << 2) + (g8 * a + 127u) / 255u + (d >> 2)) >> 2;
    uint32_t b = (((u & 31u) << 3) + (b8 * a + 127u) / 255u + (d >> 1)) >> 3;
    r = r > 31u ? 31u : r;
    g = g > 63u ? 63u : g;
    b = b > 31u ? 31u : b;
    const uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return swap ? glass_bswap(out) : out;
}

void glassref_glow_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                    int32_t start_q16, int32_t sweep_q16, bool round_caps,
                    int32_t width16, uint8_t peak,
                    uint16_t color565, uint8_t alpha, bool swap_bytes)
{
    geo_t g;
    if (row == NULL || width16 <= 0 || peak == 0u || alpha == 0u ||
        !row_clip(y, &clip_lo, &clip_hi) ||
        !geo_build(&g, cx16, cy16, r_in16, r_out16, start_q16, sweep_q16, round_caps)) {
        return;
    }
    const int32_t dy = y * 16 + 8 - g.cy, ady = iabs32(dy);
    const int32_t reach = g.ro + width16;
    if (reach <= ady) {
        return;
    }
    const int32_t ho = (int32_t)isqrt_u32((uint32_t)(reach * reach - dy * dy));
    const int xa = (int)imax32(clip_lo, x_lo(g.cx, ho));
    const int xb = (int)imin32(clip_hi, x_hi(g.cx, ho));
    int32_t he = -1; /* nothing within he of the centre is lit */
    const int32_t rr = g.ri - width16;
    if (rr > ady) {
        he = (int32_t)isqrt_u32((uint32_t)(rr * rr - dy * dy));
    }
    const uint32_t inv = (256u << 16) / (uint32_t)(width16 * 4);
    for (int x = xa; x <= xb; x++) {
        if (he >= 0 && iabs32(x * 16 + 8 - g.cx) <= he) {
            x = x_hi(g.cx, he);
            continue;
        }
        const int cov = glow_cov(&g, width16, inv, peak, x * 16 + 8, y * 16 + 8);
        const unsigned a = ((unsigned)cov * ((unsigned)alpha + 1u)) >> 8;
        if (a != 0u) {
            row[x] = add565(row[x], color565, a, x, y, swap_bytes);
        }
    }
}

