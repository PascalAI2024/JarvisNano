/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_hud_render.c — host tests for the procedural HUD renderer.
 *
 * hud_render.c is the render path for the JarvisNano OS design (procedural,
 * not baked — a 4-mood x 4-state clip matrix does not fit flash or PSRAM; see
 * docs/JARVISNANO_OS_PLAN.md). Its header claims host-compilability; this
 * proves it, and pins the behaviours the presenter depends on.
 *
 * The load-bearing test is strip invariance. jr_display.c blits in 12-row
 * internal DMA strips (466 = 38*12 + 10, so the final strip is RAGGED at 10
 * rows). Any y-dependent state that leaks across calls — or any off-by-one in
 * the ragged tail — shows up on glass as a seam and nowhere else.
 */
#include "jr_display/hud_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: ", __func__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define STRIP_ROWS 12 /* must match JR_DISPLAY_STRIP_ROWS in jr_display.c */

/* Render the whole frame in one call, then again in 12-row strips from the
 * SAME state, and require the two to be pixel-identical. */
static void test_strip_invariance(void)
{
    hud_t h;
    hud_init(&h, 0, false);
    hud_set(&h, HUD_FACE_SPEAK, 200);
    /* advance past the boot bloom into a reactive, animated face */
    for (uint32_t t = 0; t <= HUD_BOOT_MS + 500; t += 40) {
        hud_tick(&h, t);
    }

    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *whole = malloc(px * sizeof *whole);
    uint16_t *strips = malloc(px * sizeof *strips);
    if (!whole || !strips) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        free(whole);
        free(strips);
        return;
    }
    memset(whole, 0xAB, px * sizeof *whole);
    memset(strips, 0xCD, px * sizeof *strips);

    hud_render_rows(&h, whole, 0, HUD_H);

    int nstrips = 0, ragged = 0;
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        int nrows = (y + STRIP_ROWS <= HUD_H) ? STRIP_ROWS : (HUD_H - y);
        if (nrows != STRIP_ROWS) {
            ragged = nrows;
        }
        hud_render_rows(&h, strips + (size_t)y * HUD_W, y, nrows);
        nstrips++;
    }
    CHECK(ragged == 10, "expected a ragged final strip of 10 rows, got %d", ragged);
    CHECK(nstrips == 39, "expected 39 strips for 466 rows, got %d", nstrips);

    size_t diffs = 0;
    int first_y = -1, first_x = -1;
    for (size_t i = 0; i < px; i++) {
        if (whole[i] != strips[i]) {
            if (!diffs) {
                first_y = (int)(i / HUD_W);
                first_x = (int)(i % HUD_W);
            }
            diffs++;
        }
    }
    CHECK(diffs == 0,
          "strip rendering diverges from whole-frame at %zu px (first x=%d y=%d)",
          diffs, first_x, first_y);

    free(whole);
    free(strips);
}

/* hud_render_rows must write exactly nrows*HUD_W pixels — not one more. */
static void test_render_respects_bounds(void)
{
    hud_t h;
    hud_init(&h, 0, false);
    for (uint32_t t = 0; t <= HUD_BOOT_MS + 200; t += 40) {
        hud_tick(&h, t);
    }

    enum { GUARD = 64, NROWS = 12 };
    const size_t body = (size_t)NROWS * HUD_W;
    uint16_t *buf = malloc((body + 2 * GUARD) * sizeof *buf);
    if (!buf) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        return;
    }
    for (size_t i = 0; i < body + 2 * GUARD; i++) {
        buf[i] = 0xA5A5;
    }

    /* render into the middle, at a y0 that is NOT strip-aligned */
    hud_render_rows(&h, buf + GUARD, 197, NROWS);

    size_t head_clobber = 0, tail_clobber = 0;
    for (int i = 0; i < GUARD; i++) {
        if (buf[i] != 0xA5A5) {
            head_clobber++;
        }
        if (buf[GUARD + body + i] != 0xA5A5) {
            tail_clobber++;
        }
    }
    CHECK(head_clobber == 0, "wrote %zu px before the buffer", head_clobber);
    CHECK(tail_clobber == 0, "wrote %zu px past the buffer", tail_clobber);

    free(buf);
}

/* The bloom owns the stage for HUD_BOOT_MS; requests are held, then honored. */
static void test_boot_bloom_gates_face(void)
{
    hud_t h;
    hud_init(&h, 0, false);
    CHECK(h.cur == HUD_FACE_BOOT, "should init to BOOT, got %d", (int)h.cur);

    hud_set(&h, HUD_FACE_SPEAK, 90);
    hud_tick(&h, HUD_BOOT_MS - 1);
    CHECK(!h.boot_done, "bloom ended early");
    CHECK(h.cur == HUD_FACE_BOOT, "face changed during bloom (%d)", (int)h.cur);

    hud_tick(&h, HUD_BOOT_MS);
    CHECK(h.boot_done, "bloom did not complete at HUD_BOOT_MS");
    CHECK(h.cur == HUD_FACE_SPEAK, "pending face not honored, got %d", (int)h.cur);
    CHECK(h.prev == HUD_FACE_BOOT, "crossfade should start from BOOT");
}

/* The header promises a request posted before restart survives it. */
static void test_restart_boot_preserves_pending_request(void)
{
    hud_t h;
    hud_init(&h, 0, false);
    hud_set(&h, HUD_FACE_LISTEN, 55);

    hud_restart_boot(&h, 1000);
    CHECK(!h.boot_done, "restart should re-arm the bloom");
    CHECK(h.cur == HUD_FACE_BOOT, "restart should show BOOT");

    hud_tick(&h, 1000 + HUD_BOOT_MS);
    CHECK(h.cur == HUD_FACE_LISTEN,
          "request posted before restart was lost, got %d", (int)h.cur);
}

static void test_set_encodes_mailbox_and_clamps(void)
{
    hud_t h;
    hud_init(&h, 0, false);

    hud_set(&h, HUD_FACE_THINK, 0x5A);
    CHECK(h.req == (((uint32_t)HUD_FACE_THINK << 8) | 0x5Au),
          "mailbox encoding wrong: 0x%08x", (unsigned)h.req);

    hud_set(&h, (hud_face_t)99, 7);
    CHECK(((h.req >> 8) & 0xFF) == (uint32_t)HUD_FACE_IDLE,
          "out-of-range face should clamp to IDLE, got %u",
          (unsigned)((h.req >> 8) & 0xFF));
}

/* dt is clamped to 100 ms so a stalled render task cannot fast-forward. */
static void test_tick_clamps_long_stall(void)
{
    hud_t stalled, stepped;
    hud_init(&stalled, 0, false);
    hud_init(&stepped, 0, false);

    hud_tick(&stalled, 5000); /* one enormous jump */
    hud_tick(&stepped, 100);  /* the clamped equivalent */

    CHECK(stalled.breath_phase == stepped.breath_phase,
          "stall not clamped: %d vs %d", stalled.breath_phase,
          stepped.breath_phase);
}

/* swap_bytes bakes panel byte order into the palette. */
static void test_swap_bytes_swaps_palette(void)
{
    hud_t plain, swapped;
    hud_init(&plain, 0, false);
    hud_init(&swapped, 0, true);

    size_t checked = 0, matched = 0;
    for (int c = 0; c < HUD_COL_COUNT; c++) {
        for (int l = 0; l < HUD_RAMP_LEVELS; l++) {
            uint16_t a = plain.ramp[c][l];
            uint16_t b = swapped.ramp[c][l];
            if (a == 0) {
                continue; /* black is symmetric — proves nothing */
            }
            checked++;
            if (b == (uint16_t)((a >> 8) | (a << 8))) {
                matched++;
            }
        }
    }
    CHECK(checked > 0, "palette was entirely zero");
    CHECK(matched == checked, "%zu/%zu palette entries byte-swapped", matched,
          checked);
}


/* ---- overlay mode ---------------------------------------------------- */

/* The overlay must obey the same strip contract as the full renderer: the HUD
 * is composited during the panel flush, one 12-row DMA strip at a time. */
static void test_overlay_strip_invariance(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *whole = malloc(px * sizeof *whole);
    uint16_t *strips = malloc(px * sizeof *strips);
    if (!whole || !strips) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        free(whole); free(strips);
        return;
    }
    /* seed both identically — the overlay composites OVER existing content */
    for (size_t i = 0; i < px; i++) { whole[i] = strips[i] = (uint16_t)(i * 7u); }

    const hud_env_t env = { .face = HUD_FACE_SPEAK, .amp = 190,
                            .batt_pct = 65, .charging = false,
                            .ox = 4, .oy = -3 };
    const uint32_t now = 123456;

    hud_overlay_frame(whole, 0, HUD_H, now, false, &env);
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        int nrows = (y + STRIP_ROWS <= HUD_H) ? STRIP_ROWS : (HUD_H - y);
        hud_overlay_frame(strips + (size_t)y * HUD_W, y, nrows, now, false, &env);
    }
    size_t diffs = 0; int fy = -1;
    for (size_t i = 0; i < px; i++) {
        if (whole[i] != strips[i]) { if (!diffs) fy = (int)(i / HUD_W); diffs++; }
    }
    CHECK(diffs == 0, "overlay strip/whole mismatch at %zu px (first row %d)",
          diffs, fy);
    free(whole); free(strips);
}

static void test_overlay_respects_bounds(void)
{
    enum { GUARD = 64, NROWS = 12 };
    const size_t body = (size_t)NROWS * HUD_W;
    uint16_t *buf = malloc((body + 2 * GUARD) * sizeof *buf);
    if (!buf) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }
    for (size_t i = 0; i < body + 2 * GUARD; i++) buf[i] = 0xA5A5;

    const hud_env_t env = { .face = HUD_FACE_LISTEN, .amp = 255,
                            .batt_pct = 100, .charging = true,
                            .ox = HUD_TILT_MAX, .oy = HUD_TILT_MAX };
    hud_overlay_frame(buf + GUARD, 197, NROWS, 999, true, &env);

    size_t head = 0, tail = 0;
    for (int i = 0; i < GUARD; i++) {
        if (buf[i] != 0xA5A5) head++;
        if (buf[GUARD + body + i] != 0xA5A5) tail++;
    }
    CHECK(head == 0, "overlay wrote %zu px before the buffer", head);
    CHECK(tail == 0, "overlay wrote %zu px past the buffer", tail);
    free(buf);
}

/* A USB-powered puck with no cell must show NO gauge — a full-looking or
 * empty-looking battery arc would both be lies. */
static void test_overlay_hides_absent_battery(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *a = calloc(px, sizeof *a);
    uint16_t *b = calloc(px, sizeof *b);
    if (!a || !b) { printf("FAIL %s: alloc\n", __func__); g_failures++; free(a); free(b); return; }

    hud_env_t env = { .face = HUD_FACE_IDLE, .amp = 0, .batt_pct = 0xFF,
                      .charging = false, .ox = 0, .oy = 0 };
    hud_overlay_frame(a, 0, HUD_H, 500, false, &env);   /* no battery */
    env.batt_pct = 80;
    hud_overlay_frame(b, 0, HUD_H, 500, false, &env);   /* with battery */

    size_t diffs = 0;
    for (size_t i = 0; i < px; i++) if (a[i] != b[i]) diffs++;
    CHECK(diffs > 0, "battery arc drew nothing at 80%% — gauge is dead");

    /* and the absent case must leave the rim radius untouched */
    size_t rim = 0;
    for (int y = 0; y < HUD_H; y++) {
        for (int x = 0; x < HUD_W; x++) {
            int dx = x - 232, dy = y - 232;
            int r2 = dx * dx + dy * dy;
            if (r2 >= 209 * 209 && r2 <= 215 * 215 && a[y * HUD_W + x] != 0) rim++;
        }
    }
    CHECK(rim == 0, "absent battery still painted %zu rim px", rim);
    free(a); free(b);
}

static void test_tilt_offset_clamps_and_signs(void)
{
    int8_t ox = 99, oy = 99;

    hud_tilt_offset(0.0f, 0.0f, &ox, &oy);
    CHECK(ox == 0 && oy == 0, "level should not deflect (got %d,%d)", ox, oy);

    hud_tilt_offset(1000.0f, 1000.0f, &ox, &oy);
    CHECK(ox >= -HUD_TILT_MAX && ox <= HUD_TILT_MAX &&
          oy >= -HUD_TILT_MAX && oy <= HUD_TILT_MAX,
          "extreme tilt escaped the clamp (%d,%d)", ox, oy);

    hud_tilt_offset(-1000.0f, -1000.0f, &ox, &oy);
    CHECK(ox >= -HUD_TILT_MAX && ox <= HUD_TILT_MAX &&
          oy >= -HUD_TILT_MAX && oy <= HUD_TILT_MAX,
          "extreme negative tilt escaped the clamp (%d,%d)", ox, oy);

    /* opposite rolls must deflect in opposite directions */
    int8_t lx = 0, ly = 0, rx = 0, ry = 0;
    hud_tilt_offset(-20.0f, 0.0f, &lx, &ly);
    hud_tilt_offset( 20.0f, 0.0f, &rx, &ry);
    CHECK(lx == -rx && lx != 0, "roll should be antisymmetric (%d vs %d)", lx, rx);
}

int main(void)
{
    test_strip_invariance();
    test_render_respects_bounds();
    test_boot_bloom_gates_face();
    test_restart_boot_preserves_pending_request();
    test_set_encodes_mailbox_and_clamps();
    test_tick_clamps_long_stall();
    test_swap_bytes_swaps_palette();
    test_overlay_strip_invariance();
    test_overlay_respects_bounds();
    test_overlay_hides_absent_battery();
    test_tilt_offset_clamps_and_signs();

    if (g_failures) {
        printf("hud_render tests FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("hud_render tests passed\n");
    return 0;
}
