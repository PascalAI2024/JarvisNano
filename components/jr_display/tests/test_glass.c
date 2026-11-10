/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Property tests for the glass primitives (glass.h). No pixel hashes: every
 * assertion is a geometric or invariance property - areas against pi*r^2,
 * monotone edges, containment, byte-order symmetry, metric identities.
 *
 *     cd components/jr_display/tests && cmake -B build && cmake --build build \
 *       --target jr_display_glass_tests && ./build/jr_display_glass_tests
 */
#include "jr_display/glass.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks; /* empty is not pass: the summary names the count */

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define W GLASS_PANEL_PX
#define PX(v) ((int32_t)lround((v) * 16.0))
#define PI 3.14159265358979323846
#define CX PX(233.0)
#define CY PX(233.0)

/* ---- a shape, drawn through one entry point ------------------------------- */

enum { K_DISC, K_RING, K_ARC, K_GLOW };
typedef struct {
    int kind;
    int32_t cx, cy, r0, r1; /* disc: r0 is the radius; others: r0 in, r1 out */
    int32_t start, sweep;
    bool caps;
    int32_t glow_w;
    uint8_t peak;
} shape_t;

static void draw_row(uint16_t *row, int y, int lo, int hi, const shape_t *s,
                     uint16_t color, uint8_t alpha, bool swap)
{
    switch (s->kind) {
    case K_DISC:
        glass_disc_row(row, y, lo, hi, s->cx, s->cy, s->r0, color, alpha, swap);
        break;
    case K_RING:
        glass_ring_row(row, y, lo, hi, s->cx, s->cy, s->r0, s->r1, s->start, s->sweep,
                       color, alpha, swap);
        break;
    case K_ARC:
        glass_arc_row(row, y, lo, hi, s->cx, s->cy, s->r0, s->r1, s->start, s->sweep,
                      color, alpha, swap);
        break;
    default:
        glass_glow_row(row, y, lo, hi, s->cx, s->cy, s->r0, s->r1, s->start, s->sweep,
                       s->caps, s->glow_w, s->peak, color, alpha, swap);
        break;
    }
}

static shape_t disc(int32_t cx, int32_t cy, int32_t r)
{
    shape_t s = {K_DISC, cx, cy, r, 0, 0, 0, false, 0, 0};
    return s;
}
static shape_t ring(int32_t cx, int32_t cy, int32_t ri, int32_t ro, int32_t st, int32_t sw)
{
    shape_t s = {K_RING, cx, cy, ri, ro, st, sw, false, 0, 0};
    return s;
}
static shape_t arc(int32_t cx, int32_t cy, int32_t ri, int32_t ro, int32_t st, int32_t sw)
{
    shape_t s = {K_ARC, cx, cy, ri, ro, st, sw, true, 0, 0};
    return s;
}
static shape_t glow(int32_t cx, int32_t cy, int32_t ri, int32_t ro, int32_t st, int32_t sw,
                    bool caps, int32_t gw, uint8_t peak)
{
    shape_t s = {K_GLOW, cx, cy, ri, ro, st, sw, caps, gw, peak};
    return s;
}

/* The whole panel, white on black, one 6-bit green coverage byte per pixel. */
static uint8_t g_img[W * W];

static void render_green(const shape_t *s)
{
    static uint16_t row[W];
    for (int y = 0; y < W; y++) {
        memset(row, 0, sizeof row);
        draw_row(row, y, 0, W - 1, s, 0xFFFF, 255, false);
        for (int x = 0; x < W; x++) {
            g_img[y * W + x] = (uint8_t)((row[x] >> 5) & 63);
        }
    }
}

/* Area in px^2 (the green channel is coverage * 63). */
static double area_px2(const shape_t *s)
{
    render_green(s);
    long total = 0;
    for (int i = 0; i < W * W; i++) {
        total += g_img[i];
    }
    return (double)total / 63.0;
}

static bool same_picture(const shape_t *a, const shape_t *b)
{
    static uint16_t ra[W], rb[W];
    for (int y = 0; y < W; y++) {
        memset(ra, 0, sizeof ra);
        memset(rb, 0, sizeof rb);
        draw_row(ra, y, 0, W - 1, a, 0xFFFF, 255, false);
        draw_row(rb, y, 0, W - 1, b, 0xFFFF, 255, false);
        if (memcmp(ra, rb, sizeof ra) != 0) {
            return false;
        }
    }
    return true;
}

static bool writes_nothing(const shape_t *s)
{
    static uint16_t row[W];
    for (int y = 0; y < W; y++) {
        for (int x = 0; x < W; x++) {
            row[x] = 0x1234;
        }
        draw_row(row, y, 0, W - 1, s, 0xFFFF, 255, false);
        for (int x = 0; x < W; x++) {
            if (row[x] != 0x1234) {
                return false;
            }
        }
    }
    return true;
}

static double rel_err(double got, double want) { return (got - want) / want; }

/* ---- trig ------------------------------------------------------------------ */

static void test_trig(void)
{
    CHECK(glass_sin_q15(0) == 0, "sin 0");
    CHECK(glass_sin_q15(16384) == 32767, "sin quarter turn");
    CHECK(glass_cos_q15(0) == 32767, "cos 0");
    CHECK(glass_sin_q15(GLASS_Q16_12_OCLOCK) == -32767, "12 o'clock points up (y down)");
    CHECK(glass_cos_q15(32768) == -32767, "cos half turn");
    CHECK(glass_sin_q15(65536 + 1000) == glass_sin_q15(1000), "wraps by a turn");
    long worst = 0;
    int rising = 1;
    for (uint32_t a = 0; a < 65536; a += 13) {
        const long s = glass_sin_q15(a), c = glass_cos_q15(a);
        const long err = labs(s * s + c * c - 32767L * 32767L);
        if (err > worst) worst = err;
        if (a > 0 && a < 16384 && glass_sin_q15(a) < glass_sin_q15(a - 13)) rising = 0;
    }
    CHECK(worst < 32767L * 32767L / 1000, "s^2 + c^2 drifts by %ld", worst);
    CHECK(rising, "sine must rise through the first quarter");
}

/* ---- blend ----------------------------------------------------------------- */

static void test_blend(void)
{
    int exact = 1, mono = 1, swapped = 1;
    for (uint32_t u = 0; u < 65536; u += 97) {
        for (uint32_t o = 0; o < 65536; o += 4099) {
            if (glass_blend565((uint16_t)u, (uint16_t)o, 0, false) != u) exact = 0;
            if (glass_blend565((uint16_t)u, (uint16_t)o, 255, false) != o) exact = 0;
            for (unsigned a = 1; a < 255; a += 29) {
                const uint16_t n = glass_blend565((uint16_t)u, (uint16_t)o, a, false);
                const uint16_t s = glass_blend565(glass_bswap((uint16_t)u), (uint16_t)o, a, true);
                if (s != glass_bswap(n)) swapped = 0;
                /* every channel lies between its two inputs */
                for (int sh = 0; sh < 3; sh++) {
                    const int pos[3] = {11, 5, 0}, bits[3] = {5, 6, 5};
                    const int m = (1 << bits[sh]) - 1;
                    const int cu = (u >> pos[sh]) & m, co = (o >> pos[sh]) & m, cn = (n >> pos[sh]) & m;
                    if (cn < (cu < co ? cu : co) || cn > (cu > co ? cu : co)) mono = 0;
                }
            }
        }
    }
    CHECK(exact, "alpha 0 must return under and alpha 255 over, bit-exactly");
    CHECK(mono, "a blend channel left the range of its inputs");
    CHECK(swapped, "swap_bytes blend differs from the swapped native blend");
}

/* ---- areas ----------------------------------------------------------------- */

static void test_disc_area(void)
{
    const double radii[] = {4.25, 8.0, 20.5, 64.0, 150.0, 228.0};
    const double offs[][2] = {{0, 0}, {0.3125, 0.1875}, {0.6875, 0.4375}, {0.5, 0.5}};
    for (size_t i = 0; i < sizeof radii / sizeof radii[0]; i++) {
        for (size_t j = 0; j < 4; j++) {
            const shape_t s = disc(PX(233.0 + offs[j][0]), PX(233.0 + offs[j][1]), PX(radii[i]));
            const double want = PI * radii[i] * radii[i];
            const double got = area_px2(&s);
            CHECK(fabs(rel_err(got, want)) < (radii[i] < 8 ? 0.015 : 0.01), "disc r=%.2f off=%zu: %.2f vs %.2f (%+.2f%%)",
                  radii[i], j, got, want, 100 * rel_err(got, want));
        }
    }
}

static void test_ring_area(void)
{
    const double rr[][2] = {{100, 110}, {30, 31.5}, {5, 60}, {200, 214}, {118, 123.5}};
    for (size_t i = 0; i < sizeof rr / sizeof rr[0]; i++) {
        const shape_t s = ring(PX(233.3125), PX(232.6875), PX(rr[i][0]), PX(rr[i][1]), 0, 65536);
        const double want = PI * (rr[i][1] * rr[i][1] - rr[i][0] * rr[i][0]);
        const double got = area_px2(&s);
        CHECK(fabs(rel_err(got, want)) < 0.01, "ring %.1f-%.1f: %.2f vs %.2f (%+.2f%%)",
              rr[i][0], rr[i][1], got, want, 100 * rel_err(got, want));
    }
    /* A span is its fraction of the annulus. */
    const int32_t sweeps[] = {4096, 16384, 30000, 32768, 40000, 60000};
    for (size_t i = 0; i < sizeof sweeps / sizeof sweeps[0]; i++) {
        const shape_t s = ring(CX, CY, PX(100), PX(110), 12345, sweeps[i]);
        const double want = PI * (110.0 * 110 - 100.0 * 100) * sweeps[i] / 65536.0;
        const double got = area_px2(&s);
        CHECK(fabs(rel_err(got, want)) < 0.01, "span %d: %.2f vs %.2f (%+.2f%%)",
              (int)sweeps[i], got, want, 100 * rel_err(got, want));
    }
}

/* ---- edges ------------------------------------------------------------------ */

static void test_edge_monotone(void)
{
    /* Across the right edge of a disc, coverage never rises going outward. */
    const shape_t d = disc(PX(233.37), PX(233.0), PX(60.4));
    render_green(&d);
    int prev = 63, ok = 1, steps = 0;
    for (int x = 233; x < 233 + 70; x++) {
        const int v = g_img[233 * W + x];
        if (v > prev) ok = 0;
        if (v != prev) steps++;
        prev = v;
    }
    CHECK(ok, "disc coverage rose moving outward across its edge");
    CHECK(prev == 0, "coverage never reached 0 outside the disc");
    /* The same through a ring's two edges. */
    const shape_t r = ring(CX, CY, PX(50.3), PX(61.6), 0, 65536);
    render_green(&r);
    ok = 1;
    prev = 0;
    for (int x = 233; x < 233 + 52; x++) {
        const int v = g_img[233 * W + x];
        if (v < prev) ok = 0; /* inner edge: rising */
        prev = v;
    }
    CHECK(ok, "ring inner edge must rise outward");
    CHECK(steps >= 2, "an edge pixel should be partial, saw %d steps", steps);
    /* And a fixed pixel only gains coverage as the radius grows. */
    int mono = 1, last = 0;
    for (int r16 = PX(59.0); r16 <= PX(61.0); r16++) {
        const shape_t g = disc(PX(233.0), PX(233.0), r16);
        uint16_t row[W];
        memset(row, 0, sizeof row);
        draw_row(row, 233, 0, W - 1, &g, 0xFFFF, 255, false);
        const int v = (row[233 + 60] >> 5) & 63;
        if (v < last) mono = 0;
        last = v;
    }
    CHECK(mono, "pixel coverage fell while the radius grew");
    CHECK(last == 63, "edge pixel should end covered");
}

static void test_ring_orientation_and_ends(void)
{
    /* 3 o'clock start, quarter turn clockwise = lower right quadrant. */
    const shape_t q = ring(CX, CY, PX(90), PX(100), 0, 16384);
    render_green(&q);
    CHECK(g_img[233 * W + 233 + 95] >= 60 || g_img[234 * W + 233 + 95] >= 60, "3 o'clock end not lit");
    CHECK(g_img[(233 + 95) * W + 233] >= 60 || g_img[(233 + 95) * W + 232] >= 60, "6 o'clock end not lit");
    CHECK(g_img[(233 + 67) * W + 233 + 67] >= 60, "lower right is inside the span");
    CHECK(g_img[(233 - 67) * W + 233 + 67] == 0, "upper right must be outside the span");
    CHECK(g_img[(233 + 67) * W + 233 - 67] == 0, "lower left must be outside the span");
    CHECK(g_img[(233 - 95) * W + 233] == 0 && g_img[233 * W + 233 - 95] == 0, "12 and 9 o'clock must be dark");
    /* 12 o'clock is 49152: a small sweep from there lights the top. */
    const shape_t t = ring(CX, CY, PX(90), PX(100), GLASS_Q16_12_OCLOCK, 8192);
    render_green(&t);
    CHECK(g_img[145 * W + 269] >= 60, "pixel at 1:30 not lit");
    CHECK(g_img[145 * W + 197] == 0, "pixel at 10:30 must be dark");
    /* Flat ends are clean: no coverage beyond a pixel past the cut line. */
    int stray = 0;
    render_green(&q);
    for (int x = 233 + 80; x < 233 + 105; x++) {
        if (g_img[(233 - 2) * W + x] != 0) stray++; /* 2 px above the cut */
        if (g_img[(233 + 2) * W + x] < 60 && x > 233 + 91 && x < 233 + 99) stray++;
    }
    CHECK(stray == 0, "cut line is ragged (%d stray px)", stray);
}

static void test_spans_degenerate(void)
{
    const shape_t zr = ring(CX, CY, PX(90), PX(100), 1000, 0);
    const shape_t za = arc(CX, CY, PX(90), PX(100), 1000, 0);
    const shape_t zg = glow(CX, CY, PX(90), PX(100), 1000, 0, true, PX(10), 255);
    const shape_t nr = ring(CX, CY, PX(90), PX(100), 1000, -5);
    const shape_t na = arc(CX, CY, PX(90), PX(100), 1000, -40000);
    CHECK(writes_nothing(&zr), "zero-sweep ring wrote");
    CHECK(writes_nothing(&za), "zero-sweep arc wrote");
    CHECK(writes_nothing(&zg), "zero-sweep glow wrote");
    CHECK(writes_nothing(&nr), "negative-sweep ring wrote");
    CHECK(writes_nothing(&na), "negative-sweep arc wrote");
    const shape_t full = ring(CX, CY, PX(90), PX(100), 777, 65536);
    const shape_t over = ring(CX, CY, PX(90), PX(100), 777, 99999);
    const shape_t afull = arc(CX, CY, PX(90), PX(100), 777, 65536);
    const shape_t aover = arc(CX, CY, PX(90), PX(100), 12345, 70000);
    const shape_t norm = ring(CX, CY, PX(90), PX(100), 0, 65536);
    CHECK(same_picture(&full, &over), "sweep > 65536 must equal the full ring");
    CHECK(same_picture(&full, &afull), "full-sweep arc must equal the ring");
    CHECK(same_picture(&aover, &norm), "full-sweep arc must ignore its start");
    const shape_t gf = glow(CX, CY, PX(90), PX(100), 5, 65536, true, PX(10), 200);
    const shape_t go = glow(CX, CY, PX(90), PX(100), 9, 80000, false, PX(10), 200);
    CHECK(same_picture(&gf, &go), "full-sweep glow must not depend on caps or start");
}

static void test_arc_growth_and_caps(void)
{
    /* Coverage only ever grows with sweep, and never jumps. */
    const int32_t r0 = PX(100), r1 = PX(110);
    long worst_step = 0, total_step = 0;
    int mono = 1, n = 0;
    double prev_area = 0;
    for (int32_t sweep = 256; sweep <= 65536; sweep += 256) {
        const shape_t s = arc(CX, CY, r0, r1, GLASS_Q16_12_OCLOCK, sweep);
        const double a = area_px2(&s);
        if (n > 0) {
            if (a < prev_area - 0.5) mono = 0;
            const long step = (long)((a - prev_area) * 100);
            if (step > worst_step) worst_step = step;
            total_step += step;
        }
        prev_area = a;
        n++;
    }
    CHECK(mono, "arc area fell as the sweep grew");
    const double mean = (double)total_step / (n - 1);
    CHECK(worst_step < 1.6 * mean, "arc area jumped by %.1f (mean step %.1f): end cap?",
          worst_step / 100.0, mean / 100.0);
    /* Same through the 0 -> 1 turn wrap at 65535. */
    const shape_t a1 = arc(CX, CY, r0, r1, 20000, 65535);
    const shape_t a2 = ring(CX, CY, r0, r1, 0, 65536);
    CHECK(fabs(rel_err(area_px2(&a1), area_px2(&a2))) < 0.01, "an almost-full arc should be almost the ring");

    /* Round caps add one disc of the stroke's diameter. */
    const double widths[] = {4, 10, 24};
    for (size_t i = 0; i < 3; i++) {
        const int32_t ri = PX(100.0 - widths[i] / 2), ro = PX(100.0 + widths[i] / 2);
        const shape_t flat = ring(CX, CY, ri, ro, 10000, 22000);
        const shape_t cap = arc(CX, CY, ri, ro, 10000, 22000);
        const double add = area_px2(&cap) - area_px2(&flat);
        const double want = PI * (widths[i] / 2) * (widths[i] / 2);
        CHECK(fabs(rel_err(add, want)) < 0.04, "caps w=%.0f add %.2f, want %.2f (%+.1f%%)",
              widths[i], add, want, 100 * rel_err(add, want));
    }
}

static int pixel_at(const shape_t *s, int x, int y, uint8_t alpha)
{
    uint16_t row[W];
    memset(row, 0, sizeof row);
    draw_row(row, y, 0, W - 1, s, 0xFFFF, alpha, false);
    return row[x];
}

static void test_cap_seam(void)
{
    /* Sample the centre line from before the start cap to past the end cap at
     * half alpha: every sample is the same pixel value. A seam or a double
     * blend where the cap meets the body would show as a different value. */
    const int32_t ri = PX(95), ro = PX(105);
    const int32_t start = 8000, sweep = 20000;
    const shape_t s = arc(CX, CY, ri, ro, start, sweep);
    int distinct = 0, first = -1, samples = 0;
    const double cap_turns = 5.0 / 100.0 / (2 * PI);
    for (double t = start / 65536.0 - cap_turns * 0.6; t <= (start + sweep) / 65536.0 + cap_turns * 0.6; t += 1.0 / 4000) {
        const int x = (int)floor(233.0 + 100.0 * cos(2 * PI * t));
        const int y = (int)floor(233.0 + 100.0 * sin(2 * PI * t));
        const int v = pixel_at(&s, x, y, 128);
        if (first < 0) first = v;
        if (v != first) distinct++;
        samples++;
    }
    CHECK(samples > 1000 && first > 0, "seam test sampled nothing (%d, first %d)", samples, first);
    CHECK(distinct == 0, "%d of %d centre-line samples differ at alpha 128: seam or double blend", distinct, samples);
    /* Full alpha: nothing exceeds white either way. */
    render_green(&s);
    int over = 0;
    for (int i = 0; i < W * W; i++) over += g_img[i] > 63;
    CHECK(over == 0, "coverage above full");
}

/* ---- glow ------------------------------------------------------------------- */

static void test_glow(void)
{
    const int32_t ri = PX(100), ro = PX(110), gw = PX(12);
    const shape_t g = glow(CX, CY, ri, ro, 0, 65536, false, gw, 200);
    render_green(&g);
    /* Outward from the stroke edge along the 3 o'clock row. */
    const int y = 233;
    int last = g_img[y * W + 233 + 109], ok = 1, hit_peak = last;
    for (int x = 233 + 110; x < 233 + 110 + 14; x++) {
        const int v = g_img[y * W + x];
        if (v > last + 2) ok = 0; /* 2 levels of dither slack */
        last = v;
    }
    CHECK(ok, "glow rose moving away from the stroke");
    CHECK(last == 0, "glow must be gone past its width (%d)", last);
    const int peak6 = 200 * 63 / 255;
    CHECK(abs(hit_peak - peak6) <= 3, "glow at the stroke is %d, peak is %d", hit_peak, peak6);
    CHECK(g_img[y * W + 233 + 110 + 1] >= peak6 * 70 / 100, "glow just outside the edge is already weak (%d)",
          g_img[y * W + 233 + 111]);
    const int mid = g_img[y * W + 233 + 110 + 6];
    CHECK(mid > 0 && mid < peak6 * 45 / 100, "quadratic fall: halfway is %d of peak %d", mid, peak6);
    /* Inward from the inner edge. */
    last = g_img[y * W + 233 + 100];
    ok = 1;
    for (int x = 233 + 99; x > 233 + 99 - 14; x--) {
        const int v = g_img[y * W + x];
        if (v > last + 2) ok = 0;
        last = v;
    }
    CHECK(ok && last == 0, "inner glow should fall to 0 (ok=%d last=%d)", ok, last);
    CHECK(g_img[233 * W + 233] == 0, "the middle of the ring must stay dark");
    /* Never overflows or darkens: over white nothing changes; over grey only brightens. */
    int dark = 0, changed = 0;
    static uint16_t row[W];
    for (int yy = 0; yy < W; yy++) {
        for (int x = 0; x < W; x++) row[x] = 0xFFFF;
        draw_row(row, yy, 0, W - 1, &g, 0xFFFF, 255, false);
        for (int x = 0; x < W; x++) changed += row[x] != 0xFFFF;
        for (int x = 0; x < W; x++) row[x] = 0x7BEF;
        draw_row(row, yy, 0, W - 1, &g, 0xFEA0, 255, false);
        for (int x = 0; x < W; x++) {
            const uint16_t v = row[x];
            if ((v >> 11) < (0x7BEF >> 11) || ((v >> 5) & 63) < ((0x7BEF >> 5) & 63) || (v & 31) < (0x7BEF & 31)) dark++;
        }
    }
    CHECK(changed == 0, "glow over white changed %d pixels (channel overflow)", changed);
    CHECK(dark == 0, "glow darkened %d pixels of a grey ground", dark);
    /* A swept glow with caps is as smooth as the ring's: compare the sharpest
     * neighbour step. */
    const shape_t gr = glow(CX, CY, ri, ro, 0, 65536, true, gw, 220);
    const shape_t ga = glow(CX, CY, ri, ro, GLASS_Q16_12_OCLOCK, 40000, true, gw, 220);
    const shape_t gf = glow(CX, CY, ri, ro, GLASS_Q16_12_OCLOCK, 40000, false, gw, 220);
    int worst_ring = 0, worst_arc = 0, worst_flat = 0;
    for (int pass = 0; pass < 3; pass++) {
        render_green(pass == 0 ? &gr : (pass == 1 ? &ga : &gf));
        int worst = 0;
        for (int yy = 1; yy < W - 1; yy++) {
            for (int x = 1; x < W - 1; x++) {
                const int v = g_img[yy * W + x];
                const int dx = abs(v - g_img[yy * W + x + 1]), dy = abs(v - g_img[(yy + 1) * W + x]);
                if (dx > worst) worst = dx;
                if (dy > worst) worst = dy;
            }
        }
        if (pass == 0) worst_ring = worst; else if (pass == 1) worst_arc = worst; else worst_flat = worst;
    }
    CHECK(worst_arc <= worst_ring + 2, "capped arc glow steps by %d, the ring's sharpest is %d", worst_arc, worst_ring);
    CHECK(worst_flat <= worst_ring + 2, "flat-ended glow steps by %d, the ring's sharpest is %d", worst_flat, worst_ring);
    /* The glow follows the span: nothing lit in the gap's far side. */
    render_green(&ga);
    const double a = area_px2(&ga), b = area_px2(&gr);
    CHECK(a < b * 0.75 && a > b * 0.5, "swept glow area %.0f vs full %.0f", a, b);
}

/* ---- clip, rows, byte order -------------------------------------------------- */

#define GUARD 64
#define SENTINEL 0xA5A5

typedef void (*draw_any_fn)(uint16_t *row, int y, int lo, int hi, const void *ctx,
                            uint16_t color, uint8_t alpha, bool swap);

static void draw_shape_any(uint16_t *row, int y, int lo, int hi, const void *ctx, uint16_t c,
                           uint8_t a, bool sw)
{
    draw_row(row, y, lo, hi, (const shape_t *)ctx, c, a, sw);
}

typedef struct {
    glass_font_id_t font;
    const char *text;
    int x0, y_top, tracking;
} textctx_t;

static void draw_text_any(uint16_t *row, int y, int lo, int hi, const void *ctx, uint16_t c,
                          uint8_t a, bool sw)
{
    const textctx_t *t = (const textctx_t *)ctx;
    glass_text_row(row, y, lo, hi, t->font, t->text, strlen(t->text), t->x0, t->y_top,
                   t->tracking, c, a, sw);
}

/* Draw every row between the guards and check nothing outside [lo, hi] moved. */
static long g_painted; /* pixels the clip tests actually drew: the tests must bite */

static int clip_violations(draw_any_fn fn, const void *ctx, int lo, int hi)
{
    static uint16_t buf[W + 2 * GUARD];
    int bad = 0, painted = 0;
    const int elo = lo < 0 ? 0 : lo, ehi = hi > W - 1 ? W - 1 : hi;
    for (int y = -2; y < W + 2; y++) {
        for (size_t i = 0; i < sizeof buf / sizeof buf[0]; i++) buf[i] = SENTINEL;
        fn(buf + GUARD, y, lo, hi, ctx, 0x07E0, 255, false);
        for (int x = -GUARD; x < W + GUARD; x++) {
            const bool inside = y >= 0 && y < W && x >= elo && x <= ehi;
            const uint16_t v = buf[GUARD + x];
            if (!inside && v != SENTINEL) bad++;
            if (inside && v != SENTINEL) painted++;
        }
    }
    g_painted += painted;
    return bad;
}

static void test_clip(void)
{
    const shape_t shapes[] = {
        disc(CX, CY, PX(150)),
        ring(CX, CY, PX(100), PX(112), 0, 65536),
        ring(CX, CY, PX(100), PX(112), 4000, 30000),
        arc(CX, CY, PX(100), PX(112), 4000, 30000),
        arc(CX, CY, PX(0), PX(228), 4000, 50000),
        glow(CX, CY, PX(100), PX(112), 0, 65536, false, PX(16), 255),
        glow(CX, CY, PX(100), PX(112), 4000, 30000, true, PX(16), 255),
        disc(PX(10), PX(450), PX(40)), /* hangs off the panel corner */
    };
    const int clips[][2] = {{120, 300}, {-40, 500}, {233, 233}, {0, 465}, {300, 120}};
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        for (size_t j = 0; j < sizeof clips / sizeof clips[0]; j++) {
            const int bad = clip_violations(draw_shape_any, &shapes[i], clips[j][0], clips[j][1]);
            CHECK(bad == 0, "shape %zu clip [%d,%d]: %d stray writes", i, clips[j][0], clips[j][1], bad);
        }
    }
    const textctx_t texts[] = {
        {GLASS_FONT_LABEL, "BATTERY 100%", 150, 200, 3},
        {GLASS_FONT_HERO, "100", 90, 150, 0},
        {GLASS_FONT_VALUE, "12:30 -5", -20, 100, 2},
        {GLASS_FONT_TEXT, "FULL \xC2\xB7 ON USB", 420, 300, 1},
    };
    const int tclips[][2] = {{100, 300}, {-40, 500}, {0, 465}, {200, 200}};
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        for (size_t j = 0; j < sizeof tclips / sizeof tclips[0]; j++) {
            const int bad = clip_violations(draw_text_any, &texts[i], tclips[j][0], tclips[j][1]);
            CHECK(bad == 0, "text %zu clip [%d,%d]: %d stray writes", i,
                  tclips[j][0], tclips[j][1], bad);
        }
    }
    CHECK(g_painted > 100000, "the clip tests drew only %ld pixels: they are not exercising anything", g_painted);
}

static uint16_t background(int x, int y)
{
    return (uint16_t)(((x * 7 + y * 3) & 31) << 11 | ((x + y * 5) & 63) << 5 | ((x * 3 + y) & 31));
}

static int swap_mismatch(draw_any_fn fn, const void *ctx, uint8_t alpha, uint16_t color)
{
    static uint16_t a[W], b[W];
    int bad = 0;
    for (int y = 0; y < W; y += 1) {
        for (int x = 0; x < W; x++) {
            a[x] = background(x, y);
            b[x] = glass_bswap(a[x]);
        }
        fn(a, y, 0, W - 1, ctx, color, alpha, false);
        fn(b, y, 0, W - 1, ctx, color, alpha, true);
        for (int x = 0; x < W; x++) bad += a[x] != glass_bswap(b[x]);
    }
    return bad;
}

static void test_byte_order(void)
{
    const shape_t shapes[] = {
        disc(PX(200.4), PX(210.7), PX(90)),
        ring(CX, CY, PX(100), PX(112), 0, 65536),
        ring(CX, CY, PX(100), PX(112), 9000, 40000),
        arc(CX, CY, PX(100), PX(112), 9000, 40000),
        glow(CX, CY, PX(100), PX(112), 9000, 40000, true, PX(14), 230),
    };
    const uint8_t alphas[] = {255, 140, 17};
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        for (size_t j = 0; j < 3; j++) {
            const int bad = swap_mismatch(draw_shape_any, &shapes[i], alphas[j], 0xFEA0);
            CHECK(bad == 0, "shape %zu alpha %u: %d pixels differ after un-swapping", i, alphas[j], bad);
        }
    }
    const textctx_t t = {GLASS_FONT_VALUE, "12:30 PM -5\xC2\xB0", 60, 200, 2};
    for (size_t j = 0; j < 3; j++) {
        const int bad = swap_mismatch(draw_text_any, &t, alphas[j], 0xFEA0);
        CHECK(bad == 0, "text alpha %u: %d pixels differ after un-swapping", alphas[j], bad);
    }
    const shape_t zero_alpha = disc(CX, CY, PX(50));
    CHECK(swap_mismatch(draw_shape_any, &zero_alpha, 0, 0xFFFF) == 0, "alpha 0 swap");
    static uint16_t row[W];
    int moved = 0;
    for (int y = 0; y < W; y++) {
        for (int x = 0; x < W; x++) row[x] = background(x, y);
        draw_row(row, y, 0, W - 1, &zero_alpha, 0xFFFF, 0, false);
        for (int x = 0; x < W; x++) moved += row[x] != background(x, y);
    }
    CHECK(moved == 0, "alpha 0 painted %d pixels", moved);
}

/* ---- text -------------------------------------------------------------------- */

static size_t utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 63));
    out[2] = (char)(0x80 | (cp & 63));
    return 3;
}

static void test_charsets(void)
{
    const char *five_by_seven = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:/%?!+,'()& *";
    for (int f = GLASS_FONT_LABEL; f <= GLASS_FONT_TEXT; f++) {
        int missing = 0;
        for (const char *p = five_by_seven; *p; p++) missing += glass_font_glyph((glass_font_id_t)f, (uint8_t)*p) == NULL;
        missing += glass_font_glyph((glass_font_id_t)f, 0xB0) == NULL;
        missing += glass_font_glyph((glass_font_id_t)f, 0xB7) == NULL;
        missing += glass_font_glyph((glass_font_id_t)f, 0x2212) == NULL;
        CHECK(missing == 0, "font %d lacks %d of the 5x7 set and degree, dot, minus", f, missing);
    }
    int missing = 0;
    for (const char *p = "0123456789 %-+:./ABCDEFGHIJKLMNOPQRSTUVWXYZ"; *p; p++) missing += glass_font_glyph(GLASS_FONT_VALUE, (uint8_t)*p) == NULL;
    missing += glass_font_glyph(GLASS_FONT_VALUE, 0xB0) == NULL;
    missing += glass_font_glyph(GLASS_FONT_VALUE, 0x2212) == NULL;
    CHECK(missing == 0, "VALUE lacks %d requested glyphs", missing);
    missing = 0;
    for (const char *p = "0123456789 -:"; *p; p++) missing += glass_font_glyph(GLASS_FONT_HERO, (uint8_t)*p) == NULL;
    missing += glass_font_glyph(GLASS_FONT_HERO, 0x2212) == NULL;
    CHECK(missing == 0, "HERO lacks %d requested glyphs", missing);
    CHECK(glass_font_glyph(GLASS_FONT_HERO, 'A') == NULL, "HERO is digits only");
    CHECK(glass_font_glyph(GLASS_FONT_LABEL, 'a') == glass_font_glyph(GLASS_FONT_LABEL, 'A'), "lowercase folds");
    CHECK(glass_font_glyph((glass_font_id_t)9, 'A') == NULL, "bad font id");
    /* Atlases are sorted: the lookup is a binary search. */
    for (int f = 0; f < GLASS_FONT_COUNT; f++) {
        const glass_font_t *ft = &glass_fonts[f];
        int sorted = 1;
        for (int i = 1; i < ft->n_glyphs; i++) sorted &= ft->glyphs[i - 1].cp < ft->glyphs[i].cp;
        CHECK(sorted, "font %d glyphs not sorted by code point", f);
    }
}

static void test_metrics(void)
{
    const int caps[] = {11, 15, 24, 59};
    for (int f = 0; f < GLASS_FONT_COUNT; f++) {
        const glass_metrics_t m = glass_font_metrics((glass_font_id_t)f);
        CHECK(m.cap_h == caps[f], "font %d cap height %d, designed %d", f, m.cap_h, caps[f]);
        CHECK(m.line_h > m.cap_h && m.ink_top <= 0 && m.ink_bottom >= m.cap_h, "font %d metrics inconsistent", f);
        /* Flat-topped, flat-bottomed capitals stand exactly on the cap box, so
         * every baseline in a line lines up. */
        const char *flat = f == GLASS_FONT_HERO ? "1" : "HELIZT";
        for (const char *p = flat; *p; p++) {
            const glass_glyph_t *g = glass_font_glyph((glass_font_id_t)f, (uint8_t)*p);
            CHECK(g && g->yo == 0 && g->yo + g->h == m.cap_h, "font %d '%c' is not cap-high (yo %d h %d cap %d)",
                  f, *p, g ? g->yo : -1, g ? g->h : -1, m.cap_h);
        }
        /* Round digits overshoot by at most one pixel. */
        const glass_glyph_t *o = glass_font_glyph((glass_font_id_t)f, '0');
        CHECK(o && o->yo >= -1 && o->yo + o->h <= m.cap_h + 1, "font %d '0' overshoots", f);
    }
    const glass_metrics_t none = glass_font_metrics((glass_font_id_t)9);
    CHECK(none.cap_h == 0 && none.line_h == 0, "unknown font must report zeros");
}

static int adv_of(glass_font_id_t f, uint32_t cp)
{
    const glass_glyph_t *g = glass_font_glyph(f, cp);
    return g ? g->adv : glass_font_metrics(f).space_adv;
}

static void test_text_width(void)
{
    const char *s = "BATTERY 100%";
    for (int f = GLASS_FONT_LABEL; f <= GLASS_FONT_VALUE; f++) {
        for (int tr = -1; tr <= 4; tr += 5) {
            int want = 0;
            for (const char *p = s; *p; p++) want += adv_of((glass_font_id_t)f, (uint8_t)*p);
            want += tr * ((int)strlen(s) - 1);
            CHECK(glass_text_width((glass_font_id_t)f, s, strlen(s), tr) == want, "font %d tracking %d width %d, want %d", f,
                  tr, glass_text_width((glass_font_id_t)f, s, strlen(s), tr), want);
        }
    }
    const char *u = "FULL \xC2\xB7 ON USB"; /* 13 characters, 14 bytes */
    int want = 0;
    for (const char *p = "FULL ON USB"; *p; p++) want += adv_of(GLASS_FONT_LABEL, (uint8_t)*p);
    want += adv_of(GLASS_FONT_LABEL, 0xB7) + adv_of(GLASS_FONT_LABEL, ' ') + 3 * 12;
    CHECK(glass_text_width(GLASS_FONT_LABEL, u, strlen(u), 3) == want, "UTF-8 width %d, want %d",
          glass_text_width(GLASS_FONT_LABEL, u, strlen(u), 3), want);
    const glass_metrics_t m = glass_font_metrics(GLASS_FONT_HERO);
    CHECK(glass_text_width(GLASS_FONT_HERO, "~", 1, 0) == m.space_adv, "unknown character must take a space's advance");
    CHECK(glass_text_width(GLASS_FONT_HERO, "A", 1, 0) == m.space_adv, "HERO has no letters");
    CHECK(glass_text_width(GLASS_FONT_HERO, "1~2", 3, 5) == adv_of(GLASS_FONT_HERO, '1') + m.space_adv + adv_of(GLASS_FONT_HERO, '2') + 10, "mixed unknown");
    CHECK(glass_text_width(GLASS_FONT_LABEL, "", 0, 5) == 0, "empty text has no width");
    CHECK(glass_text_width(GLASS_FONT_LABEL, "A", 1, 99) == adv_of(GLASS_FONT_LABEL, 'A'), "one glyph has no tracking");
    CHECK(glass_text_width(GLASS_FONT_LABEL, "AB\0CD", 5, 0) == adv_of(GLASS_FONT_LABEL, 'A') + adv_of(GLASS_FONT_LABEL, 'B'), "stops at NUL");
    CHECK(glass_text_width(GLASS_FONT_LABEL, "A\xC2", 2, 0) == adv_of(GLASS_FONT_LABEL, 'A') + m.space_adv * 0 + glass_font_metrics(GLASS_FONT_LABEL).space_adv, "truncated UTF-8 must not read past len");
    CHECK(glass_text_width((glass_font_id_t)9, "A", 1, 0) == 0, "bad font width");
}

/* Lit columns (white on black) of a text run, as groups. */
static int lit_groups(const textctx_t *t, int *first_cols, int max)
{
    static uint16_t row[W];
    static char lit[W];
    memset(lit, 0, sizeof lit);
    for (int y = 0; y < W; y++) {
        memset(row, 0, sizeof row);
        glass_text_row(row, y, 0, W - 1, t->font, t->text, strlen(t->text), t->x0, t->y_top, t->tracking,
                       0xFFFF, 255, false);
        for (int x = 0; x < W; x++) if (row[x]) lit[x] = 1;
    }
    int n = 0;
    for (int x = 0; x < W; x++) {
        if (lit[x] && (x == 0 || !lit[x - 1]) && n < max) first_cols[n++] = x;
    }
    return n;
}

static void test_tracking_moves_glyphs(void)
{
    textctx_t a = {GLASS_FONT_LABEL, "III", 40, 100, 0};
    textctx_t b = a;
    b.tracking = 7;
    int ca[8], cb[8];
    const int na = lit_groups(&a, ca, 8), nb = lit_groups(&b, cb, 8);
    CHECK(na == 3 && nb == 3, "expected three separate I stems, saw %d and %d", na, nb);
    if (na == 3 && nb == 3) {
        CHECK(cb[1] - ca[1] == 7 && cb[2] - ca[2] == 14 && cb[0] == ca[0], "tracking shifts: %d %d %d", cb[0] - ca[0], cb[1] - ca[1], cb[2] - ca[2]);
        CHECK(ca[1] - ca[0] == adv_of(GLASS_FONT_LABEL, 'I') && ca[2] - ca[1] == adv_of(GLASS_FONT_LABEL, 'I'), "pen advance");
    }
}

static void test_star_is_degree(void)
{
    for (int f = GLASS_FONT_LABEL; f <= GLASS_FONT_VALUE; f++) {
        static uint16_t a[W], b[W];
        int diff = 0, lit = 0;
        for (int y = 0; y < 200; y++) {
            memset(a, 0, sizeof a);
            memset(b, 0, sizeof b);
            glass_text_row(a, y, 0, W - 1, (glass_font_id_t)f, "5*", 2, 30, 60, 0, 0xFFFF, 255, false);
            glass_text_row(b, y, 0, W - 1, (glass_font_id_t)f, "5\xC2\xB0", 3, 30, 60, 0, 0xFFFF, 255, false);
            diff += memcmp(a, b, sizeof a) != 0;
            for (int x = 0; x < W; x++) lit += a[x] != 0;
        }
        CHECK(diff == 0 && lit > 0, "font %d: '*' and the degree sign differ in %d rows (lit %d)", f, diff, lit);
    }
}

static void test_every_glyph_in_its_box(void)
{
    static uint16_t row[W];
    for (int f = 0; f < GLASS_FONT_COUNT; f++) {
        const glass_font_t *ft = &glass_fonts[f];
        int bad_box = 0, bad_count = 0, bad_trim = 0, blank = 0;
        for (int gi = 0; gi < ft->n_glyphs; gi++) {
            const glass_glyph_t *g = &ft->glyphs[gi];
            char s[4] = {0};
            const size_t n = utf8_encode(g->cp, s);
            const int x0 = 40, ytop = 80;
            int lit = 0, outside = 0;
            for (int y = 0; y < 200; y++) {
                memset(row, 0, sizeof row);
                glass_text_row(row, y, 0, W - 1, (glass_font_id_t)f, s, n, x0, ytop, 0, 0xFFFF, 255, false);
                for (int x = 0; x < W; x++) {
                    if (!row[x]) continue;
                    lit++;
                    if (x < x0 + g->xo || x >= x0 + g->xo + g->w || y < ytop + g->yo || y >= ytop + g->yo + g->h) outside++;
                }
            }
            /* alpha nibbles of the bitmap, and whether each edge of the box has ink */
            int nibbles = 0, top = 0, bottom = 0, left = 0, right = 0;
            for (int yy = 0; yy < g->h; yy++) {
                for (int xx = 0; xx < g->w; xx++) {
                    const unsigned idx = (unsigned)yy * g->w + (unsigned)xx;
                    const unsigned nib = (idx & 1) ? ft->bits[g->off + (idx >> 1)] & 15 : ft->bits[g->off + (idx >> 1)] >> 4;
                    if (!nib) continue;
                    nibbles++;
                    top |= yy == 0;
                    bottom |= yy == g->h - 1;
                    left |= xx == 0;
                    right |= xx == g->w - 1;
                }
            }
            if (g->w == 0) {
                blank += g->cp != ' ' || lit != 0;
            } else {
                bad_box += outside != 0;
                bad_count += lit != nibbles;
                bad_trim += !(top && bottom && left && right);
                blank += lit == 0;
            }
        }
        CHECK(bad_box == 0, "font %d: %d glyphs draw outside their declared box", f, bad_box);
        CHECK(bad_count == 0, "font %d: %d glyphs lit a different pixel count than their bitmap holds", f, bad_count);
        CHECK(bad_trim == 0, "font %d: %d glyph boxes are not trimmed to the ink", f, bad_trim);
        CHECK(blank == 0, "font %d: %d glyphs draw nothing (or a space draws)", f, blank);
    }
}

static void test_text_alpha_and_color(void)
{
    /* Half alpha text never exceeds the opaque picture, per channel, and is not equal to it. */
    static uint16_t full[W], half[W];
    int brighter = 0, differs = 0;
    for (int y = 0; y < 200; y++) {
        memset(full, 0, sizeof full);
        memset(half, 0, sizeof half);
        glass_text_row(full, y, 0, W - 1, GLASS_FONT_VALUE, "88", 2, 30, 60, 1, 0xFFE0, 255, false);
        glass_text_row(half, y, 0, W - 1, GLASS_FONT_VALUE, "88", 2, 30, 60, 1, 0xFFE0, 128, false);
        for (int x = 0; x < W; x++) {
            if ((half[x] >> 11) > (full[x] >> 11) || ((half[x] >> 5) & 63) > ((full[x] >> 5) & 63)) brighter++;
            differs += half[x] != full[x];
            if (full[x] && full[x] != 0xFFE0 && (full[x] & 31) != 0) brighter++; /* yellow has no blue */
        }
    }
    CHECK(brighter == 0, "alpha made text brighter than opaque (%d)", brighter);
    CHECK(differs > 100, "alpha 128 changed only %d pixels", differs);
}

int main(void)
{
    test_trig();
    test_blend();
    test_disc_area();
    test_ring_area();
    test_edge_monotone();
    test_ring_orientation_and_ends();
    test_spans_degenerate();
    test_arc_growth_and_caps();
    test_cap_seam();
    test_glow();
    test_clip();
    test_byte_order();
    test_charsets();
    test_metrics();
    test_text_width();
    test_tracking_moves_glyphs();
    test_star_is_degree();
    test_every_glyph_in_its_box();
    test_text_alpha_and_color();

    if (g_failures) {
        printf("%d failure(s) of %d checks\n", g_failures, g_checks);
        printf("glass tests FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("glass tests passed (%d checks)\n", g_checks);
    return 0;
}
