/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Glass bench: renders the STATUS and WEATHER compositions row by row into a
 * 466 px row buffer, the way the compositor does (one call per primitive per
 * row, a round clip at r=228), and reports nanoseconds per frame (median over
 * many frames) with a per-primitive breakdown, for the shipped primitives and
 * for the slow reference (tests/glass_ref.c) in the same run. It also renders
 * each scene once with both and counts the pixels that differ.
 *
 * HOST numbers: x86 under -O2, in Docker. They rank the work; they are not
 * device milliseconds. Not a suite:
 *
 *   cmake -B build && cmake --build build --target glass_bench
 *   ./build/glass_bench
 */
#include "../glass_ref.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N GLASS_PANEL_PX

typedef struct {
    void (*ring)(uint16_t *, int, int, int, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                 uint16_t, uint8_t, bool);
    void (*arc)(uint16_t *, int, int, int, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                uint16_t, uint8_t, bool);
    void (*glow)(uint16_t *, int, int, int, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                 bool, int32_t, uint8_t, uint16_t, uint8_t, bool);
} impl_t;

static const impl_t IMPL_NEW = {glass_ring_row, glass_arc_row, glass_glow_row};
static const impl_t IMPL_REF = {glassref_ring_row, glassref_arc_row, glassref_glow_row};

enum { M_BG = 1, M_RING = 2, M_GLOW = 4, M_ARC = 8, M_TEXT = 16, M_ALL = 31 };

#define C_INK 0xF79Eu
#define C_GREY 0x8C71u
#define C_CYAN 0x07FFu
#define C_RAIN 0x049Fu
#define SWAP true

typedef struct {
    const char *s;
    int len;
    glass_font_id_t font;
    int x0, y_top, tracking, y_lo, y_hi;
    uint16_t color;
    uint8_t alpha;
} txt_t;

static void txt_at(txt_t *t, glass_font_id_t font, const char *s, int x0, int y_top, int tracking,
                   uint16_t color)
{
    const glass_metrics_t m = glass_font_metrics(font);
    t->s = s;
    t->len = (int)strlen(s);
    t->font = font;
    t->x0 = x0;
    t->y_top = y_top;
    t->tracking = tracking;
    t->y_lo = y_top + m.ink_top;
    t->y_hi = y_top + m.ink_bottom;
    t->color = color;
    t->alpha = 255;
}

static void txt_centred(txt_t *t, glass_font_id_t font, const char *s, int cx, int y_top,
                        int tracking, uint16_t color)
{
    txt_at(t, font, s, cx - glass_text_width(font, s, strlen(s), tracking) / 2, y_top, tracking,
           color);
}

static void round_clip(int y, int *lo, int *hi)
{
    const int dy = 2 * y + 1 - 466; /* 2 * (y + 0.5 - 233) */
    const int r2 = 4 * 228 * 228 - dy * dy;
    if (r2 <= 0) {
        *lo = 1;
        *hi = 0;
        return;
    }
    int h = 0;
    while ((h + 1) * (h + 1) <= r2) h++; /* half chord in half-pixels */
    *lo = (466 - h) / 2;
    *hi = (466 + h) / 2 - 1;
    if (*lo < 0) *lo = 0;
    if (*hi > N - 1) *hi = N - 1;
}

typedef struct {
    int n_txt;
    txt_t txt[12];
    bool status;
    int32_t sweep;
    int n_arc;
    int32_t ri[12], ro[12], start[12], sweepw[12];
    uint8_t aw[12];
    char pct_text[8];
} scene_t;

static void scene_status(scene_t *sc, int pct)
{
    memset(sc, 0, sizeof *sc);
    sc->status = true;
    sc->sweep = (int32_t)(((int64_t)pct * GLASS_TURN_Q16) / 100);
    snprintf(sc->pct_text, sizeof sc->pct_text, "%d", pct);
    const char *pb = sc->pct_text;
    const int cyr = 233 - 10, hero_top = cyr - 59 / 2 - 2, base = hero_top + 59;
    const int hw = glass_text_width(GLASS_FONT_HERO, pb, strlen(pb), 0);
    const int pw = glass_text_width(GLASS_FONT_VALUE, "%", 1, 0);
    const int x0 = 233 - (hw + 5 + pw) / 2;
    int n = 0;
    txt_centred(&sc->txt[n++], GLASS_FONT_LABEL, "BATTERY", 233, hero_top - 26, 3, C_GREY);
    txt_at(&sc->txt[n++], GLASS_FONT_HERO, pb, x0, hero_top, 0, C_INK);
    txt_at(&sc->txt[n++], GLASS_FONT_VALUE, "%", x0 + hw + 5, base - 24, 0, C_CYAN);
    txt_centred(&sc->txt[n++], GLASS_FONT_TEXT, "FULL . ON USB", 233, base + 16, 1, C_GREY);
    static const char *const names[3] = {"WIFI", "LINK", "TOOLS"};
    static const char *const vals[3] = {"-58", "ON", "ON"};
    for (int i = 0; i < 3; i++) {
        const int cxi = 233 + (i - 1) * 56;
        txt_centred(&sc->txt[n++], GLASS_FONT_LABEL, names[i], cxi, 352, 2, C_GREY);
        txt_centred(&sc->txt[n++], GLASS_FONT_TEXT, vals[i], cxi, 367, 0, C_INK);
    }
    sc->n_txt = n;
}

static void scene_weather(scene_t *sc)
{
    memset(sc, 0, sizeof *sc);
    static const int pcts[12] = {0, 10, 40, 80, 100, 5, 60, 20, 0, 30, 90, 50};
    const int32_t rc = (113 * 16 + 123 * 16) / 2;
    for (int k = 0; k < 12; k++) {
        const int pct = pcts[k];
        int w, a, sw;
        if (pct < 10) {
            w = 32;
            a = 90;
            sw = 546;
        } else {
            w = 48 + (112 * pct) / 100;
            a = 100 + (155 * pct) / 100;
            sw = 3641;
        }
        const int32_t centre = (49152 + ((2 * k + 1) * 65536) / 24) & 0xFFFF;
        sc->ri[k] = rc - w / 2;
        sc->ro[k] = rc + (w + 1) / 2;
        sc->start[k] = (centre - sw / 2) & 0xFFFF;
        sc->sweepw[k] = sw;
        sc->aw[k] = (uint8_t)a;
    }
    sc->n_arc = 12;
    const int cyr = 233 - 10, hero_top = cyr - 59 / 2 - 2, base = hero_top + 59;
    const int hw = glass_text_width(GLASS_FONT_HERO, "72", 2, 0);
    const int dw = glass_text_width(GLASS_FONT_VALUE, "*", 1, 0);
    const int x0 = 233 - (hw + 4 + dw) / 2;
    int n = 0;
    txt_centred(&sc->txt[n++], GLASS_FONT_LABEL, "CLEAR SKY", 233, hero_top - 26, 3, C_CYAN);
    txt_at(&sc->txt[n++], GLASS_FONT_HERO, "72", x0, hero_top, 0, C_INK);
    txt_at(&sc->txt[n++], GLASS_FONT_VALUE, "*", x0 + hw + 4, hero_top, 0, C_CYAN);
    txt_centred(&sc->txt[n++], GLASS_FONT_TEXT, "H 86  L 76", 233, base + 16, 1, C_INK);
    txt_centred(&sc->txt[n++], GLASS_FONT_TEXT, "SUNSET 7:42", 233, base + 40, 1, C_GREY);
    txt_centred(&sc->txt[n++], GLASS_FONT_LABEL, "UPDATED 4M AGO", 233, 362, 2, C_GREY);
    sc->n_txt = n;
}

static uint16_t g_bg[N];

static void render_frame(const impl_t *im, const scene_t *sc, unsigned mask, uint16_t *sink)
{
    uint16_t row[N];
    const int32_t cx16 = 233 * 16, cy16 = (233 - 10) * 16;
    const int ring_y0 = 223 - 130, ring_y1 = 223 + 130;
    for (int y = 0; y < N; y++) {
        int lo, hi;
        round_clip(y, &lo, &hi);
        if (lo > hi) continue;
        memcpy(row, g_bg, sizeof row); /* the M_BG cost; always paid */
        if (mask != M_BG && y >= ring_y0 && y <= ring_y1) {
            if (sc->status) {
                if (mask & M_RING) {
                    im->ring(row, y, lo, hi, cx16, cy16, 113 * 16, 123 * 16,
                             GLASS_Q16_12_OCLOCK + sc->sweep, GLASS_TURN_Q16 - sc->sweep, C_CYAN,
                             44, SWAP);
                }
                if (sc->sweep > 0) {
                    if (mask & M_GLOW) {
                        im->glow(row, y, lo, hi, cx16, cy16, 113 * 16, 123 * 16,
                                 GLASS_Q16_12_OCLOCK, sc->sweep, true, 40, 140, C_CYAN, 255, SWAP);
                    }
                    if (mask & M_ARC) {
                        im->arc(row, y, lo, hi, cx16, cy16, 113 * 16, 123 * 16,
                                GLASS_Q16_12_OCLOCK, sc->sweep, C_CYAN, 255, SWAP);
                    }
                }
            } else if (mask & M_ARC) {
                for (int i = 0; i < sc->n_arc; i++) {
                    im->arc(row, y, lo, hi, cx16, cy16, sc->ri[i], sc->ro[i], sc->start[i],
                            sc->sweepw[i], C_RAIN, sc->aw[i], SWAP);
                }
            }
        }
        if (mask & M_TEXT) {
            for (int i = 0; i < sc->n_txt; i++) {
                const txt_t *t = &sc->txt[i];
                if (y >= t->y_lo && y <= t->y_hi) {
                    glass_text_row(row, y, lo, hi, t->font, t->s, (size_t)t->len, t->x0, t->y_top,
                                   t->tracking, t->color, t->alpha, SWAP);
                }
            }
        }
        if (sink) memcpy(sink + (size_t)y * N, row, sizeof row);
    }
}

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_ll(const void *a, const void *b)
{
    const long long x = *(const long long *)a, y = *(const long long *)b;
    return x < y ? -1 : (x > y);
}

#define FRAMES 301
typedef struct {
    long long med, min;
} stat_t;

/* Reference and shipped frames alternate, so a noisy neighbour on the host
 * lands on both; the median is the figure, the minimum shows the noise. */
static void measure_pair(const impl_t *im_new, const scene_t *sc, unsigned mask, stat_t *ref, stat_t *cur)
{
    long long tr[FRAMES], tn[FRAMES];
    for (int i = 0; i < 5; i++) {
        render_frame(&IMPL_REF, sc, mask, NULL);
        render_frame(im_new, sc, mask, NULL);
    }
    for (int i = 0; i < FRAMES; i++) {
        long long a = now_ns();
        render_frame(&IMPL_REF, sc, mask, NULL);
        tr[i] = now_ns() - a;
        a = now_ns();
        render_frame(im_new, sc, mask, NULL);
        tn[i] = now_ns() - a;
    }
    qsort(tr, FRAMES, sizeof tr[0], cmp_ll);
    qsort(tn, FRAMES, sizeof tn[0], cmp_ll);
    ref->med = tr[FRAMES / 2];
    ref->min = tr[0];
    cur->med = tn[FRAMES / 2];
    cur->min = tn[0];
}

static long diff_count(const impl_t *im_new, const scene_t *sc, long *max_step)
{
    static uint16_t a[N * N], b[N * N];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    render_frame(im_new, sc, M_ALL, a);
    render_frame(&IMPL_REF, sc, M_ALL, b);
    long n = 0;
    *max_step = 0;
    for (int i = 0; i < N * N; i++) {
        if (a[i] != b[i]) {
            n++;
            const int ca = (int)glass_bswap(a[i]), cb = (int)glass_bswap(b[i]);
            long d = abs((ca >> 11) - (cb >> 11));
            const long dg = abs(((ca >> 5) & 63) - ((cb >> 5) & 63));
            const long db = abs((ca & 31) - (cb & 31));
            if (dg > d) d = dg;
            if (db > d) d = db;
            if (d > *max_step) *max_step = d;
        }
    }
    return n;
}

static void report(const char *name, const scene_t *sc)
{
    static const struct {
        const char *n;
        unsigned m;
    } parts[] = {{"ring", M_RING}, {"glow", M_GLOW}, {"arc", M_ARC}, {"text", M_TEXT}, {"ALL", M_ALL}};
    stat_t bg_r, bg_n;
    measure_pair(&IMPL_NEW, sc, M_BG, &bg_r, &bg_n);
    printf("%-12s %10s %10s %7s | %10s %10s %7s   (ns/frame, row copy %lld removed)\n", name,
           "ref med", "new med", "x", "ref min", "new min", "x", bg_n.med);
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        if (sc->status == false && (parts[i].m == M_RING || parts[i].m == M_GLOW)) continue;
        stat_t r, w;
        measure_pair(&IMPL_NEW, sc, M_BG | parts[i].m, &r, &w);
        long long rm = r.med - bg_r.med, wm = w.med - bg_n.med, rn = r.min - bg_r.min,
                  wn = w.min - bg_n.min;
        if (rm < 0) rm = 0;
        if (wm < 1) wm = 1;
        if (rn < 0) rn = 0;
        if (wn < 1) wn = 1;
        printf("  %-10s %10lld %10lld %6.2fx | %10lld %10lld %6.2fx\n", parts[i].n, rm, wm,
               (double)rm / (double)wm, rn, wn, (double)rn / (double)wn);
    }
    long step;
    const long d = diff_count(&IMPL_NEW, sc, &step);
    printf("  pixels differing from the reference: %ld (largest channel step %ld)\n\n", d, step);
}

int main(void)
{
    for (int x = 0; x < N; x++) {
        g_bg[x] = glass_bswap((uint16_t)(((x >> 4) << 11) | (((x >> 3) & 63) << 5) | (31 - (x >> 4))));
    }
    scene_t sc;
    static const int pcts[3] = {100, 74, 5};
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof name, "STATUS %d%%", pcts[i]);
        scene_status(&sc, pcts[i]);
        report(name, &sc);
    }
    scene_weather(&sc);
    report("WEATHER", &sc);
    return 0;
}
