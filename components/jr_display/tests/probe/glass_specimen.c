/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Glass specimen: renders two scenes through the row API exactly as the
 * compositor would (12-row strips, one row call per primitive, an explicit
 * round clip at r=228) and writes PPMs for eyes-on-the-glass review.
 *
 *   glass_status.ppm   a STATUS-like screen: swept ring with round cap and
 *                      glow, BATTERY / 100 % / FULL . ON USB
 *   glass_charset.ppm  every atlas's charset in rows
 *   glass_arcs.ppm     arcs at several sweeps, widths and start angles
 *
 * Build and run (from components/jr_display/tests, see CMakeLists.txt):
 *
 *   cmake -B build && cmake --build build --target glass_specimen
 *   ./build/glass_specimen [output_dir]
 *
 * Prints how many pixels each scene's primitives changed: the proxy for the
 * per-frame cost of the screen. NOT part of any suite: the assert is a human
 * (or an agent) reading the picture.
 */
#include "jr_display/glass.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N GLASS_PANEL_PX
#define STRIP 12
#define PX(v) ((int32_t)lround((v) * 16.0))

static uint16_t g_frame[N * N];
static long g_touched;
static long g_calls;

#define C_GOLD 0xFEA0u
#define C_GOLD_DIM 0x3160u /* the track: gold at roughly a fifth */
#define C_WHITE 0xFFFFu
#define C_LABEL 0xAD55u    /* soft grey */

/* Count what a primitive changed in the row it was handed. */
#define TRACK(row, call)                                                       \
    do {                                                                       \
        uint16_t before_[N];                                                   \
        memcpy(before_, (row), sizeof before_);                                \
        call;                                                                  \
        for (int i_ = 0; i_ < N; i_++) g_touched += (row)[i_] != before_[i_];  \
        g_calls++;                                                             \
    } while (0)

static void round_clip(int y, int *lo, int *hi)
{
    const double dy = (y + 0.5) - 233.0;
    const double r2 = 228.0 * 228.0 - dy * dy;
    if (r2 <= 0) {
        *lo = 1;
        *hi = 0;
        return;
    }
    const int h = (int)floor(sqrt(r2));
    *lo = 233 - h;
    *hi = 232 + h;
}

static void centered(uint16_t *row, int y, int lo, int hi, glass_font_id_t f, const char *s,
                     int cx, int y_top, int tracking, uint16_t col)
{
    const int w = glass_text_width(f, s, strlen(s), tracking);
    TRACK(row, glass_text_row(row, y, lo, hi, f, s, strlen(s), cx - w / 2, y_top, tracking, col, 255, false));
}

static void status_row(uint16_t *row, int y, int lo, int hi)
{
    const int32_t cx = PX(233), cy = PX(233), ri = PX(113), ro = PX(123);
    const int32_t sweep = (int32_t)(65536 * 0.80);
    /* track, then the glow behind the swept arc, then the arc itself */
    TRACK(row, glass_ring_row(row, y, lo, hi, cx, cy, ri, ro, 0, 65536, C_GOLD_DIM, 255, false));
    TRACK(row, glass_glow_row(row, y, lo, hi, cx, cy, ri, ro, GLASS_Q16_12_OCLOCK, sweep, true, PX(16), 150,
                              C_GOLD, 255, false));
    TRACK(row, glass_arc_row(row, y, lo, hi, cx, cy, ri, ro, GLASS_Q16_12_OCLOCK, sweep, C_GOLD, 255, false));

    const glass_metrics_t hero = glass_font_metrics(GLASS_FONT_HERO);
    const glass_metrics_t val = glass_font_metrics(GLASS_FONT_VALUE);
    const int hero_top = 233 - hero.cap_h / 2 - 2;
    const int base = hero_top + hero.cap_h;
    const int hw = glass_text_width(GLASS_FONT_HERO, "100", 3, 0);
    const int pw = glass_text_width(GLASS_FONT_VALUE, "%", 1, 0);
    const int x0 = 233 - (hw + 5 + pw) / 2;
    centered(row, y, lo, hi, GLASS_FONT_LABEL, "BATTERY", 233, hero_top - 26, 3, C_LABEL);
    TRACK(row, glass_text_row(row, y, lo, hi, GLASS_FONT_HERO, "100", 3, x0, hero_top, 0, C_WHITE, 255, false));
    TRACK(row, glass_text_row(row, y, lo, hi, GLASS_FONT_VALUE, "%", 1, x0 + hw + 5, base - val.cap_h, 0, C_GOLD, 255, false));
    centered(row, y, lo, hi, GLASS_FONT_LABEL, "FULL \xC2\xB7 ON USB", 233, base + 22, 3, C_LABEL);
}

static void line(uint16_t *row, int y, int lo, int hi, glass_font_id_t f, const char *s, int y_top, int tr,
                 uint16_t col)
{
    centered(row, y, lo, hi, f, s, 233, y_top, tr, col);
}

static void charset_row(uint16_t *row, int y, int lo, int hi)
{
    const char *punct = "0123456789 -_.:/%?!+,'()&*\xC2\xB0\xC2\xB7\xE2\x88\x92";
    line(row, y, lo, hi, GLASS_FONT_LABEL, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 70, 2, C_WHITE);
    line(row, y, lo, hi, GLASS_FONT_LABEL, punct, 90, 2, C_GOLD);
    line(row, y, lo, hi, GLASS_FONT_TEXT, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 112, 1, C_WHITE);
    line(row, y, lo, hi, GLASS_FONT_TEXT, punct, 138, 1, C_GOLD);
    line(row, y, lo, hi, GLASS_FONT_VALUE, "0123456789 %\xC2\xB0-\xE2\x88\x92+:./", 168, 0, C_GOLD);
    line(row, y, lo, hi, GLASS_FONT_VALUE, "ABCDEFGHIJKLM", 202, 0, C_WHITE);
    line(row, y, lo, hi, GLASS_FONT_VALUE, "NOPQRSTUVWXYZ", 236, 0, C_WHITE);
    line(row, y, lo, hi, GLASS_FONT_HERO, "0123456789", 280, 0, C_WHITE);
    line(row, y, lo, hi, GLASS_FONT_HERO, "-\xE2\x88\x92:", 350, 0, C_GOLD);
}

static void arc_with_glow(uint16_t *row, int y, int lo, int hi, double cx, double cy, double r, double w,
                          int32_t start, double sweep, int glow_px, uint8_t alpha)
{
    const int32_t ri = PX(r - w / 2), ro = PX(r + w / 2);
    TRACK(row, glass_glow_row(row, y, lo, hi, PX(cx), PX(cy), ri, ro, start, (int32_t)(65536 * sweep), true,
                              PX(glow_px), 140, C_GOLD, alpha, false));
    TRACK(row, glass_arc_row(row, y, lo, hi, PX(cx), PX(cy), ri, ro, start, (int32_t)(65536 * sweep), C_GOLD,
                             alpha, false));
}

static void arcs_row(uint16_t *row, int y, int lo, int hi)
{
    const double sweeps[] = {0.10, 0.25, 0.50, 0.75, 0.95};
    for (int i = 0; i < 5; i++) {
        arc_with_glow(row, y, lo, hi, 233 + (i - 2) * 70, 120, 22, 6, GLASS_Q16_12_OCLOCK, sweeps[i], 8, 255);
    }
    const double widths[] = {2, 4, 8, 12};
    for (int i = 0; i < 4; i++) {
        arc_with_glow(row, y, lo, hi, 233 + (i - 1.5) * 92, 232, 32, widths[i], 8000, 0.62, 8, 255);
    }
    arc_with_glow(row, y, lo, hi, 233 - 105, 345, 42, 10, 0, 0.30, 10, 255);
    arc_with_glow(row, y, lo, hi, 233, 345, 42, 10, 16384, 0.30, 10, 255);
    arc_with_glow(row, y, lo, hi, 233 + 105, 345, 42, 10, 32768, 0.30, 10, 128);
}

static int write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", N, N);
    for (int i = 0; i < N * N; i++) {
        const uint16_t v = g_frame[i];
        const uint8_t rgb[3] = {(uint8_t)(((v >> 11) << 3) | ((v >> 13) & 7)),
                                (uint8_t)((((v >> 5) & 63) << 2) | ((v >> 9) & 3)),
                                (uint8_t)(((v & 31) << 3) | ((v >> 2) & 7))};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

static int scene(const char *dir, const char *name, void (*rowfn)(uint16_t *, int, int, int))
{
    static uint16_t strip[STRIP * N];
    g_touched = 0;
    g_calls = 0;
    for (int y0 = 0; y0 < N; y0 += STRIP) {
        memset(strip, 0, sizeof strip);
        const int rows = (N - y0 < STRIP) ? N - y0 : STRIP;
        for (int r = 0; r < rows; r++) {
            int lo, hi;
            round_clip(y0 + r, &lo, &hi);
            if (lo <= hi) {
                rowfn(&strip[r * N], y0 + r, lo, hi);
            }
        }
        for (int i = 0; i < rows * N; i++) {
            g_frame[y0 * N + i] = strip[i];
        }
    }
    printf("%s: %ld primitive calls, %ld pixels changed (%.1f%% of the panel's %d)\n", name, g_calls,
           g_touched, 100.0 * (double)g_touched / (N * N), N * N);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    return write_ppm(path);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    return scene(dir, "glass_status", status_row) || scene(dir, "glass_charset", charset_row) ||
           scene(dir, "glass_arcs", arcs_row);
}
