/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass shapes: anti-aliased discs, rings, arcs with round caps, and glows,
 * one row at a time. Integer-only; see glass.h for the contract.
 *
 * Coverage comes from distance: an edge at radius R gives
 * 128 + (R - d) * 256 (clamped to 0..255) for a pixel whose centre is d away,
 * a one-pixel linear ramp that preserves area to well under 1%. d is taken
 * with an integer square root, but only for pixels inside an edge band: a
 * band is entered by comparing squared distances, so a ring's solid body, its
 * hole and everything outside cost two multiplies and a compare.
 *
 * Cost model (the shell draws 466 rows a frame, each through several of
 * these): a row is never walked pixel by pixel when it can be described. The
 * solid interior of a ring or arc, the span a glow is at full strength, and
 * the pixels that can be touched at all are each an interval in x on one row
 * (a circle and a half plane both cut a row in an interval), found in closed
 * form. Runs are filled; only the few edge pixels pay for a coverage
 * computation, and that computation is the same one the original per-pixel
 * walk used (tests/glass_ref.c keeps that walk as the reference), so the
 * pixels are the same ones. Geometry that does not change between rows (the
 * trig, the cap centres, the vertical extent) is prepared once and kept in a
 * small cache; see geo_get.
 *
 * Units: positions and radii 1/16 px, root distances 1/64 px, angles Q16 turns.
 */
#include "jr_display/glass.h"

/* ---- trig ----------------------------------------------------------------- */

/* The spatial shell's Q15 quarter sine (jr_display.c s_sp_sin), copied: this
 * file must not include jr_display.c. 130 bytes. */
static const int16_t k_sin[65] = {
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

int32_t glass_sin_q15(uint32_t angle_q16)
{
    uint32_t t = angle_q16 & 0xFFFFu;
    const bool neg = t >= 0x8000u;
    t &= 0x7FFFu;
    if (t > 0x4000u) {
        t = 0x8000u - t; /* sin(pi - x) = sin(x) */
    }
    const uint32_t i = t >> 8, f = t & 0xFFu;
    int32_t v = k_sin[i < 64u ? i : 64u];
    if (i < 64u) {
        v += ((int32_t)(k_sin[i + 1u] - k_sin[i]) * (int32_t)f) >> 8;
    }
    return neg ? -v : v;
}

int32_t glass_cos_q15(uint32_t angle_q16)
{
    return glass_sin_q15(angle_q16 + 0x4000u);
}

/* ---- small helpers -------------------------------------------------------- */

static inline int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }
static inline int32_t imin32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t imax32(int32_t a, int32_t b) { return a > b ? a : b; }

/* floor(sqrt(n)), digit by digit, starting at the leading digit rather than
 * walking down to it. */
static uint32_t isqrt_u32(uint32_t n)
{
    if (n == 0u) {
        return 0u;
    }
    uint32_t res = 0, bit = 1u << ((31u - (uint32_t)__builtin_clz(n)) & ~1u);
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
    unsigned wo, wu; /* glass_blend565 weights for `alpha` (valid when 0 < alpha < 255) */
    bool swap;
} paint_t;

static void paint_init(paint_t *p, uint16_t color, uint8_t alpha, bool swap)
{
    p->color = color;
    p->panel = swap ? glass_bswap(color) : color;
    p->alpha = alpha;
    p->wo = alpha < 255u ? ((unsigned)alpha * 64u + 127u) / 255u : 64u;
    p->wu = 64u - p->wo;
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

/* glass_blend565 with the weights already worked out: the same arithmetic,
 * for a run of pixels that all use the same alpha. */
static inline uint16_t blend_w(uint16_t under, uint16_t over, unsigned wo, unsigned wu, bool swap)
{
    const uint32_t u = swap ? glass_bswap(under) : under;
    const uint32_t rb = ((u & 0xF81Fu) * wu + (uint32_t)(over & 0xF81Fu) * wo + 0x10020u) >> 6;
    const uint32_t g = ((u & 0x07E0u) * wu + (uint32_t)(over & 0x07E0u) * wo + (32u << 5)) >> 6;
    const uint16_t out = (uint16_t)((rb & 0xF81Fu) | (g & 0x07E0u));
    return swap ? glass_bswap(out) : out;
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
            row[x] = blend_w(row[x], c->color, c->wo, c->wu, c->swap);
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

void glass_disc_row(uint16_t *row, int y, int clip_lo, int clip_hi,
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

/* ---- row intervals ---------------------------------------------------------
 *
 * On one row a disc, an annulus' chord and a half plane each select an
 * interval of pixels. A span is such an interval, inclusive; a set holds the
 * handful (at most six) one row ever needs. */

typedef struct {
    int lo, hi;
} span_t;

#define SPAN_INF 1000000

static const span_t k_span_all = {-SPAN_INF, SPAN_INF};
static const span_t k_span_none = {1, 0};

typedef struct {
    span_t s[6];
    int n;
} spanset_t;

static inline span_t span_isect(span_t a, span_t b)
{
    span_t r = {a.lo > b.lo ? a.lo : b.lo, a.hi < b.hi ? a.hi : b.hi};
    return r;
}

static inline void ss_add(spanset_t *r, span_t s)
{
    if (s.lo <= s.hi && r->n < 6) {
        r->s[r->n++] = s;
    }
}

/* Is x in the set? If so *end is the last pixel of a span holding it. */
static inline bool ss_find(const spanset_t *r, int x, int *end)
{
    bool in = false;
    int e = x;
    for (int i = 0; i < r->n; i++) {
        if (r->s[i].lo <= x && x <= r->s[i].hi) {
            in = true;
            if (r->s[i].hi > e) {
                e = r->s[i].hi;
            }
        }
    }
    *end = e;
    return in;
}

/* First pixel after x that starts a span, or SPAN_INF. */
static inline int ss_next(const spanset_t *r, int x)
{
    int best = SPAN_INF;
    for (int i = 0; i < r->n; i++) {
        if (r->s[i].lo > x && r->s[i].lo < best) {
            best = r->s[i].lo;
        }
    }
    return best;
}

static void ss_isect(spanset_t *out, const spanset_t *a, const spanset_t *b)
{
    for (int i = 0; i < a->n; i++) {
        for (int j = 0; j < b->n; j++) {
            ss_add(out, span_isect(a->s[i], b->s[j]));
        }
    }
}

/* Pixels x with a * (16x + 8 - cx) + b >= thr, a half plane cut by the row.
 * Everything stays in 32 bits: the same products the per-pixel code formed
 * (a is a Q15 direction, b a Q15 direction times a row offset). */
static span_t halfplane(int32_t a, int32_t b, int32_t thr, int32_t cx)
{
    const int32_t r = thr - b - a * (8 - cx); /* a * 16 * x >= r */
    if (a == 0) {
        return r <= 0 ? k_span_all : k_span_none;
    }
    const int32_t d = a * 16;
    int32_t q = r / d;
    const int32_t m = r % d;
    span_t s;
    if (a > 0) {                       /* x >= ceil(r / d) */
        if (m != 0 && r > 0) {
            q++;
        }
        s.lo = q;
        s.hi = SPAN_INF;
    } else {                           /* x <= floor(r / d) */
        if (m != 0 && r > 0) {
            q--;
        }
        s.lo = -SPAN_INF;
        s.hi = q;
    }
    return s;
}

/* Pixels whose centre lies within reach (1/16 px, exclusive) of (cdx, cdy) on
 * the row at y16: the bounding interval of a disc's nonzero pixels. */
static span_t disc_span(int32_t cdx, int32_t cdy, int32_t reach, int32_t y16)
{
    const int32_t dy = y16 - cdy;
    if (reach <= 0 || iabs32(dy) >= reach) {
        return k_span_none;
    }
    const int32_t h = (int32_t)isqrt_u32((uint32_t)(reach * reach - dy * dy));
    span_t s = {x_lo(cdx, h), x_hi(cdx, h)};
    return s;
}

/* The radial pieces of a row: pixels with so >= |dx| > si (si < 0: no hole;
 * so < 0: none), as at most two spans. */
static void radial_pieces(spanset_t *out, int32_t cx, int32_t so, int32_t si)
{
    if (so < 0) {
        return;
    }
    if (si < 0) {
        span_t s = {x_lo(cx, so), x_hi(cx, so)};
        ss_add(out, s);
        return;
    }
    span_t l = {x_lo(cx, so), x_lo(cx, si) - 1};
    span_t r = {x_hi(cx, si) + 1, x_hi(cx, so)};
    ss_add(out, l);
    ss_add(out, r);
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
    int32_t ymin, ymax;             /* rows (1/16 px) the shape can touch, before any AA margin */
    /* Edge windows, in the units of n = 16 * d2 (the square of d64): below lo
     * the edge is at full coverage, from hi up it is at none, and only n in
     * [lo, hi) needs the root. inv is 2^20 / (2 * 4r), the slope that turns
     * n - (4r)^2 into a first guess at the root (see near_sqrt). One window per
     * edge: outer, inner, and the round caps. */
    uint32_t lo_o, hi_o, lo_i, hi_i, lo_c, hi_c;
    int32_t inv_o, inv_i, inv_c;
    /* The same for the glow of one width, filled by glow_prep. */
    int32_t gw16;
    bool g_fast_o, g_fast_i;
    uint32_t g_hi_o, g_lo_i, g_lo_c;
    int32_t g_a_o, g_inv_o, g_a_i, g_inv_i, g_a_c, g_inv_c;
} geo_t;

/* floor(sqrt(n)) for an n known to lie near a^2: the tangent gives a guess
 * within a step or two (inv = 2^20 / 2a, a in 1/64 px) and two compares settle
 * it, so the digit loop is not run for the few edge pixels of a row. Exact for
 * any guess; a bad one only costs more steps. The caller keeps |n - a^2| small
 * enough that delta * inv fits 31 bits. */
static inline uint32_t near_sqrt(uint32_t n, uint32_t a, int32_t inv)
{
    const int32_t delta = (int32_t)(n - a * a);
    int32_t k = (int32_t)a + (delta * inv >> 20);
    if (k < 0) {
        k = 0;
    }
    uint32_t r = (uint32_t)k;
    while (r * r > n) {
        r--;
    }
    while ((r + 1u) * (r + 1u) <= n) {
        r++;
    }
    return r;
}

/* n = 16 * d2 as dist64 forms it. */
static inline uint32_t n16(int32_t d2)
{
    uint32_t d = (uint32_t)d2;
    if (d > 0x0FFFFFFFu) {
        d = 0x0FFFFFFFu;
    }
    return d << 4;
}

/* The window of an edge of radius r16. A pixel covers 128 + 4 * (4r - d64)
 * clamped to 0..255, so it is full once d64 <= 4r - 32 (n < (4r - 31)^2) and
 * empty once d64 >= 4r + 32 (n >= (4r + 32)^2). */
static void edge_window(int32_t r16, uint32_t *lo, uint32_t *hi, int32_t *inv)
{
    const int32_t r4 = r16 * 4;
    *lo = r4 > 31 ? (uint32_t)((r4 - 31) * (r4 - 31)) : 0u;
    *hi = (uint32_t)((r4 + 32) * (r4 + 32));
    *inv = r4 > 0 ? (int32_t)((1 << 20) / (2 * r4)) : 0;
}

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
    g->ymin = cy - ro;
    g->ymax = cy + ro;
    g->gw16 = 0;
    edge_window(ro, &g->lo_o, &g->hi_o, &g->inv_o);
    edge_window(ri, &g->lo_i, &g->hi_i, &g->inv_i);
    g->lo_c = g->hi_c = 0u;
    g->inv_c = 0;
    if (sweep < GLASS_TURN_Q16) {
        const uint32_t s = (uint32_t)start & 0xFFFFu;
        const uint32_t e = (s + (uint32_t)sweep) & 0xFFFFu;
        g->sx = glass_cos_q15(s);
        g->sy = glass_sin_q15(s);
        g->ex = glass_cos_q15(e);
        g->ey = glass_sin_q15(e);
        g->wedge = sweep <= GLASS_TURN_Q16 / 2 ? 1 : 2;
        /* The sector's rows: its four corners, and the extreme of the outer
         * circle if the span crosses 6 or 12 o'clock. */
        const int32_t ys[4] = {(g->sy * ri) >> 15, (g->sy * ro) >> 15,
                               (g->ey * ri) >> 15, (g->ey * ro) >> 15};
        int32_t lo = cy + ys[0], hi = lo;
        for (int i = 1; i < 4; i++) {
            lo = imin32(lo, cy + ys[i]);
            hi = imax32(hi, cy + ys[i]);
        }
        if ((((uint32_t)0x4000u - s) & 0xFFFFu) <= (uint32_t)sweep) {
            hi = cy + ro;
        }
        if ((((uint32_t)0xC000u - s) & 0xFFFFu) <= (uint32_t)sweep) {
            lo = cy - ro;
        }
        g->ymin = lo;
        g->ymax = hi;
        if (caps) {
            const int32_t rc2 = ri + ro; /* twice the centre-line radius */
            g->caps = true;
            g->capr = (ro - ri) / 2;
            edge_window(g->capr, &g->lo_c, &g->hi_c, &g->inv_c);
            g->ax = cx + ((g->sx * rc2 + 32768) >> 16);
            g->ay = cy + ((g->sy * rc2 + 32768) >> 16);
            g->bx = cx + ((g->ex * rc2 + 32768) >> 16);
            g->by = cy + ((g->ey * rc2 + 32768) >> 16);
            g->ymin = imin32(g->ymin, imin32(g->ay, g->by) - g->capr);
            g->ymax = imax32(g->ymax, imax32(g->ay, g->by) + g->capr);
        }
    }
    return true;
}

/* ---- prepared geometry cache -----------------------------------------------
 *
 * A strip is 12 rows and every primitive is called once per row, so the same
 * trig, cap centres and vertical extent would be worked out 466 times a frame
 * per primitive. They are kept here, keyed by the call's parameters, and a hit
 * costs a few compares. Sixteen slots hold a whole screen (WEATHER draws
 * twelve arcs); past that the slots are reused round-robin and the only
 * penalty is the rebuild the cache saves.
 *
 * ONE RENDER TASK DRAWS (gfx_render, the only task that composes strips), so
 * the cache is plain static state with no lock. It is never trusted for
 * safety: an entry holds numbers only, no pointers, and every use is clamped
 * to the row and the clip. Concurrent callers would at worst draw one wrong
 * frame, not fault. Host tests are single threaded. */

#define GEO_SLOTS 16

typedef struct {
    int32_t cx, cy, ri, ro, start, sweep;
    bool caps, valid;
    geo_t g;
} geo_slot_t;

static geo_slot_t s_geo[GEO_SLOTS];
static unsigned s_geo_hint;   /* where the last lookup hit: calls repeat in order */
static unsigned s_geo_victim; /* next slot to overwrite */

static geo_t *geo_get(int32_t cx, int32_t cy, int32_t ri, int32_t ro,
                            int32_t start, int32_t sweep, bool caps)
{
    if (sweep <= 0 || ro <= 0 || ro <= ri) {
        return NULL;
    }
    for (unsigned i = 0; i < GEO_SLOTS; i++) {
        const unsigned k = (s_geo_hint + 1u + i) % GEO_SLOTS;
        geo_slot_t *e = &s_geo[k];
        if (e->valid && e->start == start && e->sweep == sweep && e->ro == ro && e->ri == ri &&
            e->cx == cx && e->cy == cy && e->caps == caps) {
            s_geo_hint = k;
            return &e->g;
        }
    }
    geo_slot_t *e = &s_geo[s_geo_victim];
    s_geo_hint = s_geo_victim;
    s_geo_victim = (s_geo_victim + 1u) % GEO_SLOTS;
    e->valid = false;
    if (!geo_build(&e->g, cx, cy, ri, ro, start, sweep, caps)) {
        return NULL;
    }
    e->cx = cx;
    e->cy = cy;
    e->ri = ri;
    e->ro = ro;
    e->start = start;
    e->sweep = sweep;
    e->caps = caps;
    e->valid = true;
    return &e->g;
}

/* Can row y (centre 16y + 8) hold any pixel of the shape widened by `margin`
 * (1/16 px)? The margin covers the one-pixel anti-aliasing ramps. */
static inline bool row_in_extent(const geo_t *g, int32_t y16, int32_t margin)
{
    return y16 >= g->ymin - margin && y16 <= g->ymax + margin;
}

#define EDGE_MARGIN 48 /* 3 px: a coverage ramp is half a pixel to each side */

static inline int radial_cov(const geo_t *g, int32_t d2)
{
    if (d2 >= g->ro_p2 || d2 <= g->ri_m2) {
        return 0;
    }
    if (d2 <= g->ro_m2 && d2 >= g->ri_p2) {
        return 255;
    }
    /* In an edge band. The root is only needed inside the half-pixel ramp of
     * an edge: outside it the edge's coverage is already 0 or 255. */
    const uint32_t n = n16(d2);
    uint32_t d64 = 0;
    bool have = false;
    int co, ci = 0;
    if (n >= g->hi_o) {
        co = 0;
    } else if (n < g->lo_o) {
        co = 255;
    } else {
        d64 = near_sqrt(n, (uint32_t)g->ro * 4u, g->inv_o);
        have = true;
        co = edge_cov(g->ro, d64);
    }
    if (g->ri > 0 && n < g->hi_i) {
        if (n < g->lo_i) {
            ci = 255;
        } else {
            if (!have) {
                d64 = near_sqrt(n, (uint32_t)g->ri * 4u, g->inv_i);
            }
            ci = edge_cov(g->ri, d64);
        }
    }
    return co > ci ? co - ci : 0;
}

/* disc_cov() for the round caps, with the edge window worked out once. */
static inline int cap_cov(const geo_t *g, int32_t dx, int32_t dy)
{
    if (g->capr <= 0) {
        return 0;
    }
    const uint32_t n = n16(dx * dx + dy * dy);
    if (n < g->lo_c) {
        return 255;
    }
    if (n >= g->hi_c) {
        return 0;
    }
    return edge_cov(g->capr, near_sqrt(n, (uint32_t)g->capr * 4u, g->inv_c));
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
        cov = (int)imax32(cov, cap_cov(g, px - g->ax, py - g->ay));
        cov = (int)imax32(cov, cap_cov(g, px - g->bx, py - g->by));
    }
    return cov;
}

/* wedge_cov() thresholds in the units of the half-plane products: the line
 * distance in 1/256 px is 128 + (t >> 11), so 255 (full) needs t >= 127 * 2048
 * and anything above 0 needs t >= -127 * 2048. */
#define WEDGE_FULL_T (127 * 2048)
#define WEDGE_ANY_T (-127 * 2048)

/* The pixels of the row inside the start line's and end line's half planes
 * (wedge_cov's two products reaching `thr`): both for a span up to a half turn,
 * either for more. */
static void wedge_spans(const geo_t *g, int32_t dy, int32_t thr, spanset_t *out)
{
    const span_t ss = halfplane(-g->sy, g->sx * dy, thr, g->cx);
    const span_t se = halfplane(g->ey, -dy * g->ex, thr, g->cx);
    if (g->wedge == 1) {
        ss_add(out, span_isect(ss, se));
    } else {
        ss_add(out, ss);
        ss_add(out, se);
    }
}

/* The pixels of the row the stroke (ring, arc or its caps) covers at exactly
 * 255: the radial body (si, so] from the centre where the wedge is also at
 * full. Either half plane at full is enough for a span over a half turn. */
static void stroke_solid(const geo_t *g, int32_t dy, int32_t ady, spanset_t *solid)
{
    int32_t so = -1, si = -1;
    if (g->ro - 16 > ady) {
        so = (int32_t)isqrt_u32((uint32_t)(g->ro_m2 - dy * dy));
        if (g->ri > 0 && g->ri + 16 > ady) {
            si = (int32_t)isqrt_u32((uint32_t)(g->ri_p2 - dy * dy));
        }
    }
    spanset_t body = {.n = 0};
    radial_pieces(&body, g->cx, so, si);
    if (g->wedge == 0) {
        *solid = body;
    } else {
        spanset_t full = {.n = 0};
        wedge_spans(g, dy, WEDGE_FULL_T, &full);
        ss_isect(solid, &body, &full);
    }
}

/* A recorder for glass_ring_record(): the same row walk as the painter, but
 * each fill and each edge pixel is written to a byte stream instead of the
 * row. Per row the stream is a list of records:
 *   0, x0 lo, x0 hi, x1 lo, x1 hi         a solid run x0..x1 (fill_run)
 *   n, x0 lo, x0 hi, cov * n              n edge pixels from x0 (put)
 * and the buffer starts with GLASS_PANEL_PX + 1 uint16 row offsets. */
typedef struct {
    uint8_t *buf;     /* NULL: only count */
    size_t cap, pos;
    size_t span;      /* pos of the open edge span's count byte, 0 = none */
    unsigned span_n;  /* pixels in the open span */
    int span_next;    /* x the open span would take next */
} rec_t;

static void rec_byte(rec_t *r, unsigned v)
{
    if (r->buf != NULL && r->pos < r->cap) {
        r->buf[r->pos] = (uint8_t)v;
    }
    r->pos++;
}

static void rec_u16(rec_t *r, int v)
{
    rec_byte(r, (unsigned)v & 0xFFu);
    rec_byte(r, ((unsigned)v >> 8) & 0xFFu);
}

static void rec_flush(rec_t *r) { r->span = 0; }

static void rec_run(rec_t *r, int xa, int xb)
{
    rec_flush(r);
    rec_byte(r, 0u);
    rec_u16(r, xa);
    rec_u16(r, xb);
}

static void rec_edge(rec_t *r, int x, unsigned cov)
{
    if (r->span == 0u || r->span_next != x || r->span_n == 255u) {
        rec_flush(r);
        r->span = r->pos;
        r->span_n = 0u;
        rec_byte(r, 0u);          /* the count, patched below */
        rec_u16(r, x);
    }
    rec_byte(r, cov);
    r->span_n++;
    r->span_next = x + 1;
    if (r->buf != NULL && r->span < r->cap) {
        r->buf[r->span] = (uint8_t)r->span_n;
    }
}

static inline __attribute__((always_inline)) void
ring_like_row_t(uint16_t *row, int y, int clip_lo, int clip_hi, const geo_t *g,
                uint16_t color565, uint8_t alpha, bool swap_bytes, rec_t *rec)
{
    if (rec == NULL && (row == NULL || alpha == 0u)) {
        return;
    }
    if (!row_clip(y, &clip_lo, &clip_hi)) {
        return;
    }
    const int32_t y16 = y * 16 + 8;
    if (!row_in_extent(g, y16, EDGE_MARGIN)) {
        return;
    }
    paint_t c;
    paint_init(&c, color565, alpha, swap_bytes);
    const int32_t dy = y16 - g->cy, ady = iabs32(dy);
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
    /* Solid: the pixels the stroke covers at 255. Maybe: the pixels that
     * can be above zero at all. */
    spanset_t solid = {.n = 0}, maybe = {.n = 0};
    stroke_solid(g, dy, ady, &solid);
    if (g->wedge == 0) {
        ss_add(&maybe, k_span_all);
    } else {
        /* Wherever the wedge is above zero, plus the cap discs' own reach. */
        wedge_spans(g, dy, WEDGE_ANY_T, &maybe);
        if (g->caps && g->capr > 0) {
            ss_add(&maybe, disc_span(g->ax, g->ay, g->capr + 16, y16));
            ss_add(&maybe, disc_span(g->bx, g->by, g->capr + 16, y16));
        }
    }
    int x = xa, e;
    while (x <= xb) {
        const int32_t pdx = x * 16 + 8 - g->cx;
        if (he >= 0 && iabs32(pdx) <= he) {
            x = x_hi(g->cx, he) + 1;
            continue;
        }
        if (ss_find(&solid, x, &e)) {
            e = (int)imin32(e, xb);
            if (rec != NULL) {
                rec_run(rec, x, e);
            } else {
                fill_run(row, x, e, &c);
            }
            x = e + 1;
            continue;
        }
        if (!ss_find(&maybe, x, &e)) {
            x = ss_next(&maybe, x);
            continue;
        }
        /* A run of edge pixels: up to the end of the maybe span, the next solid
         * run or the hole, so the sets are not searched for each of them. */
        int lim = (int)imin32(e, xb);
        const int ns = ss_next(&solid, x);
        if (ns <= lim) {
            lim = ns - 1;
        }
        if (he >= 0) {
            const int hl = x_lo(g->cx, he);
            if (hl > x && hl <= lim) {
                lim = hl - 1;
            }
        }
        for (; x <= lim; x++) {
            if (rec != NULL) {
                rec_edge(rec, x, (unsigned)shape_cov(g, x * 16 + 8, y16));
            } else {
                put(&row[x], &c, (unsigned)shape_cov(g, x * 16 + 8, y16));
            }
        }
    }
}

static void ring_like_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                          const geo_t *g, uint16_t color565, uint8_t alpha, bool swap_bytes)
{
    ring_like_row_t(row, y, clip_lo, clip_hi, g, color565, alpha, swap_bytes, NULL);
}

size_t glass_ring_record(uint8_t *buf, size_t cap, int32_t cx16, int32_t cy16,
                         int32_t r_in16, int32_t r_out16)
{
    const geo_t *g = geo_get(cx16, cy16, r_in16, r_out16, 0, GLASS_TURN_Q16, false);
    const size_t head = (size_t)(GLASS_PANEL_PX + 1) * 2u;
    if (g == NULL || (buf != NULL && cap < head)) {
        return 0u;
    }
    rec_t r = {.buf = buf, .cap = cap, .pos = head};
    uint16_t scratch = 0u;        /* the recorder never touches the row */
    for (int y = 0; y < GLASS_PANEL_PX; y++) {
        if (buf != NULL) {
            ((uint16_t *)(void *)buf)[y] = (uint16_t)r.pos;
        }
        rec_flush(&r);
        ring_like_row_t(&scratch, y, 0, GLASS_PANEL_PX - 1, g, 0u, 255u, false, &r);
    }
    if (buf != NULL) {
        ((uint16_t *)(void *)buf)[GLASS_PANEL_PX] = (uint16_t)r.pos;
    }
    if (r.pos > 0xFFFFu || (buf != NULL && r.pos > cap)) {
        return 0u;
    }
    return r.pos;
}

void glass_ring_replay_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                           const uint8_t *rec, uint16_t color565, uint8_t alpha,
                           bool swap_bytes)
{
    if (row == NULL || rec == NULL || alpha == 0u || !row_clip(y, &clip_lo, &clip_hi)) {
        return;
    }
    const uint16_t *tab = (const uint16_t *)(const void *)rec;
    size_t p = tab[y];
    const size_t end = tab[y + 1];
    paint_t c;
    paint_init(&c, color565, alpha, swap_bytes);
    while (p < end) {
        const unsigned n = rec[p];
        const int x0 = (int)rec[p + 1] | ((int)rec[p + 2] << 8);
        if (n == 0u) {
            const int x1 = (int)rec[p + 3] | ((int)rec[p + 4] << 8);
            p += 5u;
            const int a = x0 > clip_lo ? x0 : clip_lo;
            const int b = x1 < clip_hi ? x1 : clip_hi;
            if (a <= b) {
                fill_run(row, a, b, &c);
            }
        } else {
            p += 3u;
            for (unsigned i = 0; i < n; i++) {
                const int x = x0 + (int)i;
                if (x >= clip_lo && x <= clip_hi) {
                    put(&row[x], &c, rec[p + i]);
                }
            }
            p += n;
        }
    }
}

void glass_ring_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                    int32_t start_q16, int32_t sweep_q16,
                    uint16_t color565, uint8_t alpha, bool swap_bytes)
{
    const geo_t *g = geo_get(cx16, cy16, r_in16, r_out16, start_q16, sweep_q16, false);
    if (g != NULL) {
        ring_like_row(row, y, clip_lo, clip_hi, g, color565, alpha, swap_bytes);
    }
}

void glass_arc_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                   int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                   int32_t start_q16, int32_t sweep_q16,
                   uint16_t color565, uint8_t alpha, bool swap_bytes)
{
    const geo_t *g = geo_get(cx16, cy16, r_in16, r_out16, start_q16, sweep_q16, true);
    if (g != NULL) {
        ring_like_row(row, y, clip_lo, clip_hi, g, color565, alpha, swap_bytes);
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

/* What the glow of one width needs beyond the shape: the same windows as the
 * edges, over the falloff band (t64 in (0, w64)) either side of the stroke. */
static void glow_prep(geo_t *g, int32_t w16)
{
    if (g->gw16 == w16) {
        return;
    }
    const int32_t w64 = w16 * 4;
    g->gw16 = w16;
    /* Outer side: d64 in [4ro, 4ro + w64); empty once d64 >= 4ro + w64. */
    g->g_fast_o = w64 <= 1024;
    g->g_hi_o = (uint32_t)((g->ro * 4 + w64) * (g->ro * 4 + w64));
    g->g_a_o = g->ro * 4 + w64 / 2;
    g->g_inv_o = (1 << 20) / (2 * g->g_a_o);
    /* Inner side: d64 in (4ri - w64, 4ri]; empty once d64 <= 4ri - w64. */
    g->g_a_i = g->ri * 4 - w64 / 2;
    g->g_fast_i = g->g_a_i > 0 && w64 <= 1024;
    g->g_lo_i = g->ri * 4 - w64 + 1 > 0
                    ? (uint32_t)((g->ri * 4 - w64 + 1) * (g->ri * 4 - w64 + 1)) : 0u;
    g->g_inv_i = g->g_fast_i ? (1 << 20) / (2 * g->g_a_i) : 0;
    /* Cap side: d64 in (4capr, 4capr + w64); at or under 4capr is full. */
    g->g_lo_c = (uint32_t)((g->capr * 4 + 1) * (g->capr * 4 + 1));
    g->g_a_c = g->capr * 4 + w64 / 2;
    g->g_inv_c = (1 << 20) / (2 * g->g_a_c);
}

/* How far outside the shape a pixel centre is, in 1/64 px, for the glow's
 * falloff: <= 0 is full strength (peak), >= w16 * 4 is dark. glow_prep(g, w16)
 * has run. */
static int32_t glow_t64(const geo_t *g, int32_t w16, int32_t px, int32_t py)
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
            return 0;
        }
        const uint32_t n = n16(d2);
        if (d2 > g->ro2) {
            if (n >= g->g_hi_o) {
                return w64;
            }
            const uint32_t d64 = g->g_fast_o ? near_sqrt(n, (uint32_t)g->g_a_o, g->g_inv_o)
                                             : isqrt_u32(n);
            t64 = (int32_t)d64 - g->ro * 4;
        } else {
            if (n < g->g_lo_i) {
                return w64;
            }
            const uint32_t d64 = g->g_fast_i ? near_sqrt(n, (uint32_t)g->g_a_i, g->g_inv_i)
                                             : isqrt_u32(n);
            t64 = g->ri * 4 - (int32_t)d64;
        }
    } else if (g->caps) {
        const int32_t da = (px - g->ax) * (px - g->ax) + (py - g->ay) * (py - g->ay);
        const int32_t db = (px - g->bx) * (px - g->bx) + (py - g->by) * (py - g->by);
        const int32_t m = imin32(da, db);
        const int32_t reach = g->capr + w16;
        if (m >= reach * reach) {
            return w64;
        }
        const uint32_t n = n16(m);
        if (n < g->g_lo_c) {
            return 0;
        }
        const uint32_t d64 = w64 <= 1024 ? near_sqrt(n, (uint32_t)g->g_a_c, g->g_inv_c)
                                         : isqrt_u32(n);
        t64 = (int32_t)d64 - g->capr * 4;
    } else {
        const int32_t m = imin32(seg_d2(g, dx, dy, g->sx, g->sy), seg_d2(g, dx, dy, g->ex, g->ey));
        if (m >= w16 * w16) {
            return w64;
        }
        t64 = (int32_t)dist64((uint32_t)m);
    }
    return t64;
}

/* 4x4 Bayer thresholds, 0..15. */
static const uint8_t k_bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

/* What a light of weight `a` adds to each channel, at 8 bits. */
typedef struct {
    uint32_t r, g, b;
} glow_add_t;

static inline glow_add_t glow_add_for(uint16_t col, unsigned a)
{
    const uint32_t cr = col >> 11, cg = (col >> 5) & 63u, cb = col & 31u;
    const uint32_t r8 = (cr << 3) | (cr >> 2), g8 = (cg << 2) | (cg >> 4), b8 = (cb << 3) | (cb >> 2);
    glow_add_t t = {(r8 * a + 127u) / 255u, (g8 * a + 127u) / 255u, (b8 * a + 127u) / 255u};
    return t;
}

/* The glow's addend for a pixel t64 (1/64 px) outside the stroke: falloff,
 * alpha, then the colour's share, packed 10 bits a channel. 0 is no light. */
static uint32_t glow_pack(uint16_t color, uint8_t peak, unsigned alpha, uint32_t inv, int32_t w64,
                          int32_t t64)
{
    unsigned cov = peak;
    if (t64 > 0) {
        const uint32_t u = (((uint32_t)(w64 - t64)) * inv) >> 16; /* 0..256 */
        cov = (unsigned)(((uint32_t)peak * u * u) >> 16);
    }
    const unsigned a = (cov * (alpha + 1u)) >> 8;
    if (a == 0u) {
        return 0u;
    }
    const glow_add_t t = glow_add_for(color, a);
    return (t.r << 20) | (t.g << 10) | t.b;
}

/* Every pixel of a halo has the same colour and alpha, so what it adds is a
 * function of t64 alone: tabulated once per (width, peak, colour, alpha) and
 * looked up per pixel. One table, one render task (see geo_get). */
#define GLOW_LUT_MAX 256

typedef struct {
    bool valid;
    int32_t w16;
    uint8_t peak, alpha;
    uint16_t color;
    uint32_t e[GLOW_LUT_MAX];
} glow_lut_t;

static glow_lut_t s_glow_lut;

static const glow_lut_t *glow_lut_get(int32_t w16, uint8_t peak, uint16_t color, uint8_t alpha,
                                      uint32_t inv)
{
    const int32_t w64 = w16 * 4;
    if (w64 > GLOW_LUT_MAX) {
        return NULL;
    }
    glow_lut_t *l = &s_glow_lut;
    if (!l->valid || l->w16 != w16 || l->peak != peak || l->color != color || l->alpha != alpha) {
        for (int32_t t = 0; t < w64; t++) {
            l->e[t] = glow_pack(color, peak, alpha, inv, w64, t);
        }
        l->w16 = w16;
        l->peak = peak;
        l->color = color;
        l->alpha = alpha;
        l->valid = true;
    }
    return l;
}

/* Add colour * a/255 to a panel-order pixel, saturating per channel. The sum
 * is kept at 8 bits per channel and quantised with an ordered dither, so a
 * soft ramp that spans only a few 5-bit levels does not band. */
static inline uint16_t add565(uint16_t under, const glow_add_t *t, int x, int y, bool swap)
{
    const uint32_t u = swap ? glass_bswap(under) : under;
    const uint32_t d = k_bayer[((unsigned)y & 3u) * 4u + ((unsigned)x & 3u)];
    uint32_t r = (((u >> 11) << 3) + t->r + (d >> 1)) >> 3;
    uint32_t g = ((((u >> 5) & 63u) << 2) + t->g + (d >> 2)) >> 2;
    uint32_t b = (((u & 31u) << 3) + t->b + (d >> 1)) >> 3;
    r = r > 31u ? 31u : r;
    g = g > 63u ? 63u : g;
    b = b > 31u ? 31u : b;
    const uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return swap ? glass_bswap(out) : out;
}

/* add565 over x0..x1 of one row for a constant addend: the four dither
 * columns' sums are worked out once and the byte order is a compile-time
 * constant in each loop. */
static inline void add_run_n(uint16_t *row, int x0, int x1, int y, const glow_add_t *t, bool swap)
{
    uint32_t ar[4], ag[4], ab[4];
    for (unsigned i = 0; i < 4u; i++) {
        const uint32_t d = k_bayer[((unsigned)y & 3u) * 4u + i];
        ar[i] = t->r + (d >> 1);
        ag[i] = t->g + (d >> 2);
        ab[i] = t->b + (d >> 1);
    }
    for (int x = x0; x <= x1; x++) {
        const unsigned c = (unsigned)x & 3u;
        const uint32_t u = swap ? glass_bswap(row[x]) : row[x];
        uint32_t r = (((u >> 11) << 3) + ar[c]) >> 3;
        uint32_t g = ((((u >> 5) & 63u) << 2) + ag[c]) >> 2;
        uint32_t b = (((u & 31u) << 3) + ab[c]) >> 3;
        r = r > 31u ? 31u : r;
        g = g > 63u ? 63u : g;
        b = b > 31u ? 31u : b;
        const uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
        row[x] = swap ? glass_bswap(out) : out;
    }
}

static void add_run(uint16_t *row, int x0, int x1, int y, const glow_add_t *t, bool swap)
{
    if (swap) {
        add_run_n(row, x0, x1, y, t, true);
    } else {
        add_run_n(row, x0, x1, y, t, false);
    }
}

void glass_glow_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                    int32_t start_q16, int32_t sweep_q16, bool round_caps,
                    int32_t width16, uint8_t peak,
                    uint16_t color565, uint8_t alpha, bool swap_bytes)
{
    if (row == NULL || width16 <= 0 || peak == 0u || alpha == 0u ||
        !row_clip(y, &clip_lo, &clip_hi)) {
        return;
    }
    geo_t *g = geo_get(cx16, cy16, r_in16, r_out16, start_q16, sweep_q16, round_caps);
    if (g == NULL) {
        return;
    }
    glow_prep(g, width16);
    const int32_t y16 = y * 16 + 8;
    if (!row_in_extent(g, y16, EDGE_MARGIN + width16)) {
        return;
    }
    const int32_t dy = y16 - g->cy, ady = iabs32(dy);
    const int32_t reach = g->ro + width16;
    if (reach <= ady) {
        return;
    }
    const int32_t ho = (int32_t)isqrt_u32((uint32_t)(reach * reach - dy * dy));
    const int xa = (int)imax32(clip_lo, x_lo(g->cx, ho));
    const int xb = (int)imin32(clip_hi, x_hi(g->cx, ho));
    int32_t he = -1; /* nothing within he of the centre is lit */
    const int32_t rr = g->ri - width16;
    if (rr > ady) {
        he = (int32_t)isqrt_u32((uint32_t)(rr * rr - dy * dy));
    }
    const int32_t w64 = width16 * 4;
    const uint32_t inv = (256u << 16) / (uint32_t)w64;
    const glow_lut_t *lut = glow_lut_get(width16, peak, color565, alpha, inv);

    /* The span at full strength: inside the shape's half planes and between
     * the radii. `maybe` is everything that can be lit at all: the inside of
     * the half planes, and what lies within reach of an end. */
    int32_t so = -1, si = -1;
    if (g->ro2 - dy * dy >= 0) {
        so = (int32_t)isqrt_u32((uint32_t)(g->ro2 - dy * dy));
    }
    if (g->ri2 - dy * dy > 0) {
        si = (int32_t)isqrt_u32((uint32_t)(g->ri2 - dy * dy));
    }
    spanset_t body = {.n = 0}, inside = {.n = 0}, peaks = {.n = 0}, maybe = {.n = 0};
    radial_pieces(&body, g->cx, so, si);
    if (g->wedge == 0) {
        peaks = body;
        ss_add(&maybe, k_span_all);
    } else {
        wedge_spans(g, dy, 0, &inside);
        ss_isect(&peaks, &body, &inside);
        maybe = inside;
        if (g->caps) {
            ss_add(&maybe, disc_span(g->ax, g->ay, g->capr + width16, y16));
            ss_add(&maybe, disc_span(g->bx, g->by, g->capr + width16, y16));
        } else {
            /* Outside the half planes the glow follows the two radial edges:
             * lit within width16 of an edge segment, so within width16 of its
             * line. t is that distance times the direction's length (about
             * 32768); the pad is for the table's rounding. */
            const int32_t band = (width16 + 4) * 33000;
            const span_t s_lo = halfplane(-g->sy, g->sx * dy, -band, g->cx);
            const span_t s_hi = halfplane(g->sy, -g->sx * dy, -band, g->cx);
            const span_t e_lo = halfplane(g->ey, -dy * g->ex, -band, g->cx);
            const span_t e_hi = halfplane(-g->ey, dy * g->ex, -band, g->cx);
            ss_add(&maybe, span_isect(s_lo, s_hi));
            ss_add(&maybe, span_isect(e_lo, e_hi));
        }
    }
    const unsigned a_peak = (((unsigned)peak) * ((unsigned)alpha + 1u)) >> 8;
    const glow_add_t add_peak = glow_add_for(color565, a_peak);
    int x = xa, e;
    while (x <= xb) {
        if (he >= 0 && iabs32(x * 16 + 8 - g->cx) <= he) {
            x = x_hi(g->cx, he) + 1;
            continue;
        }
        if (ss_find(&peaks, x, &e)) {
            e = (int)imin32(e, xb);
            if (a_peak != 0u) {
                add_run(row, x, e, y, &add_peak, swap_bytes);
            }
            x = e + 1;
            continue;
        }
        if (!ss_find(&maybe, x, &e)) {
            x = ss_next(&maybe, x);
            continue;
        }
        int lim = (int)imin32(e, xb);
        const int np = ss_next(&peaks, x);
        if (np <= lim) {
            lim = np - 1;
        }
        if (he >= 0) {
            const int hl = x_lo(g->cx, he);
            if (hl > x && hl <= lim) {
                lim = hl - 1;
            }
        }
        for (; x <= lim; x++) {
            const int32_t t64 = glow_t64(g, width16, x * 16 + 8, y16);
            if (t64 >= w64) {
                continue;
            }
            const uint32_t pk = lut != NULL ? lut->e[t64 > 0 ? t64 : 0]
                                            : glow_pack(color565, peak, alpha, inv, w64, t64);
            if (pk != 0u) {
                const glow_add_t t = {(pk >> 20) & 1023u, (pk >> 10) & 1023u, pk & 1023u};
                row[x] = add565(row[x], &t, x, y, swap_bytes);
            }
        }
    }
}

/* ---- motion: pure timeline maths ------------------------------------------ */

uint32_t glass_ease_out_cubic_q16(uint32_t t_ms, uint32_t dur_ms)
{
    if (dur_ms == 0u || t_ms >= dur_ms) {
        return GLASS_ONE_Q16;
    }
    const uint32_t x = (uint32_t)(((uint64_t)t_ms << 16) / dur_ms);
    const uint32_t u = GLASS_ONE_Q16 - x;
    const uint32_t u2 = (uint32_t)(((uint64_t)u * u) >> 16);
    const uint32_t u3 = (uint32_t)(((uint64_t)u2 * u) >> 16);
    return GLASS_ONE_Q16 - u3;
}

uint32_t glass_stagger_q16(uint32_t t_ms, uint32_t start_ms, uint32_t dur_ms)
{
    return t_ms <= start_ms ? 0u : glass_ease_out_cubic_q16(t_ms - start_ms, dur_ms);
}

int32_t glass_count_up(int32_t target, uint32_t frac_q16)
{
    if (frac_q16 >= GLASS_ONE_Q16) {
        return target;
    }
    const int64_t v = (int64_t)target * (int64_t)frac_q16;
    return (int32_t)((v + (v < 0 ? -32768 : 32768)) / GLASS_ONE_Q16);
}

int glass_rise_px(int rise_px, uint32_t frac_q16)
{
    if (frac_q16 >= GLASS_ONE_Q16) {
        return 0;
    }
    return (int)(((int64_t)rise_px * (int64_t)(GLASS_ONE_Q16 - frac_q16) + 32768) >> 16);
}

uint8_t glass_frac_alpha(uint8_t alpha, uint32_t frac_q16)
{
    if (frac_q16 >= GLASS_ONE_Q16) {
        return alpha;
    }
    return (uint8_t)(((uint32_t)alpha * frac_q16 + 32768u) >> 16);
}

bool glass_entry_animating(uint32_t t_ms, uint32_t total_ms)
{
    return t_ms < total_ms;
}
