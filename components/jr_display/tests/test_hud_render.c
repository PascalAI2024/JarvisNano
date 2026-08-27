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

/* The listening ring: LISTEN paints a full band strictly inside the free
 * r185-194 band (measured negative space — nothing else may live there), and
 * IDLE paints nothing at all with the battery absent, so the ring's presence
 * carries exactly one meaning: listening. Louder input only brightens; it
 * never moves the band. Strip invariance is structural (ov_ring_row is
 * row-analytic) but pinned here anyway because this ring is a STATE signal —
 * a seam would read as a glitch in the device's attention. */
static void test_listen_ring_band_and_exclusivity(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    uint16_t *strips = calloc(px, sizeof *strips);
    if (!fb || !strips) {
        printf("FAIL %s: alloc\n", __func__); g_failures++;
        free(fb); free(strips); return;
    }

    hud_env_t env = { .face = HUD_FACE_LISTEN, .amp = 0, .batt_pct = 0xFF,
                      .charging = false, .ox = 0, .oy = 0 };
    hud_overlay_frame(fb, 0, HUD_H, 1000, false, &env);
    size_t painted = 0, out_of_band = 0;
    for (int y = 0; y < HUD_H; y++) {
        for (int x = 0; x < HUD_W; x++) {
            if (fb[(size_t)y * HUD_W + x] == 0) continue;
            painted++;
            const long dx = x - 232, dy = y - 232;
            const long r2 = dx * dx + dy * dy;
            if (r2 < 185L * 185L || r2 > 194L * 194L) out_of_band++;
        }
    }
    CHECK(painted > 1000, "listen ring painted only %zu px", painted);
    CHECK(out_of_band == 0, "listen ring left the r185-194 band (%zu px)",
          out_of_band);

    /* strip invariance at the same instant */
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        int nrows = (y + STRIP_ROWS <= HUD_H) ? STRIP_ROWS : (HUD_H - y);
        hud_overlay_frame(strips + (size_t)y * HUD_W, y, nrows, 1000,
                          false, &env);
    }
    size_t diffs = 0;
    for (size_t i = 0; i < px; i++) if (fb[i] != strips[i]) diffs++;
    CHECK(diffs == 0, "listen ring strip/whole mismatch at %zu px", diffs);

    /* IDLE with no cell paints NOTHING — absence is the other half of the
     * signal. */
    memset(fb, 0, px * sizeof *fb);
    env.face = HUD_FACE_IDLE;
    hud_overlay_frame(fb, 0, HUD_H, 1000, false, &env);
    size_t idle_painted = 0;
    for (size_t i = 0; i < px; i++) if (fb[i] != 0) idle_painted++;
    CHECK(idle_painted == 0, "idle painted %zu px — ring exclusivity broken",
          idle_painted);
    free(fb); free(strips);
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

    /* and the absent case must leave the rim radius untouched. The rim rides
     * the INNER half of the outer free band (r215-220, see the band split in
     * hud_render.c) — probe there, not at some radius the gauge never uses, or
     * the check passes for the wrong reason. */
    size_t rim = 0;
    for (int y = 0; y < HUD_H; y++) {
        for (int x = 0; x < HUD_W; x++) {
            int dx = x - 232, dy = y - 232;
            int r2 = dx * dx + dy * dy;
            if (r2 >= 215 * 215 && r2 <= 221 * 221 && a[y * HUD_W + x] != 0) rim++;
        }
    }
    CHECK(rim == 0, "absent battery still painted %zu rim px", rim);
    free(a); free(b);
}

static void test_tilt_offset_is_disabled(void)
{
    /* Parallax is deliberately off: only this overlay can move, the baked .eaf
     * face cannot, so any offset is misregistration rather than depth. It must
     * return 0,0 for EVERY input — including a flat, still device, which on
     * this board reads roll ~= 178 deg (gz ~= -1 g face-up) and previously
     * produced a permanent -10 px shift. */
    const float cases[][2] = {
        {0.0f, 0.0f}, {177.7f, 1.3f}, {-177.7f, -1.3f},
        {30.0f, 30.0f}, {-30.0f, -30.0f}, {1000.0f, -1000.0f},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int8_t ox = 99, oy = 99;
        hud_tilt_offset(cases[i][0], cases[i][1], &ox, &oy);
        CHECK(ox == 0 && oy == 0,
              "tilt must not deflect (roll=%.1f pitch=%.1f -> %d,%d)",
              (double)cases[i][0], (double)cases[i][1], ox, oy);
    }
}

/* ---- choice arcs (STATE-05/06) --------------------------------------- */

/* Test-local integer sine, the same Bhaskara approximation hud_render.c uses,
 * so these tests need neither libm nor a private header — and so a point
 * placed "at angle a" here is placed with the renderer's own convention. */
static int t_sin(int a)
{
    a &= 255;
    int x = a & 127;
    if (x > 63) {
        x = 127 - x;
    }
    int32_t p = (int32_t)x * (128 - x);
    int32_t s = (p * (25451 + ((7373 * p) >> 12))) >> 12;
    if (s > 32767) {
        s = 32767;
    }
    return (a & 128) ? (int)-s : (int)s;
}
static int t_cos(int a) { return t_sin(a + 64); }

static void t_point(int a, int r, int *x, int *y)
{
    *x = 232 + ((t_cos(a) * r) >> 15);
    *y = 232 + ((t_sin(a) * r) >> 15);
}

/* Mid-angle of a possibly-wrapping span — the safest place to aim a tap. */
static int t_span_mid(const hud_choice_t *c)
{
    return (c->a0 + (((c->a1 - c->a0) & 255) / 2)) & 255;
}

static void t_fill_choices(hud_choice_t *c, int n)
{
    static const char *labels[HUD_CHOICE_MAX] = { "yes", "no", "later" };
    memset(c, 0, (size_t)n * sizeof *c);
    hud_choice_layout(n, c);
    for (int i = 0; i < n; i++) {
        c[i].label = labels[i];
    }
}

/* THE load-bearing test: the arcs are composited during the panel flush, one
 * 12-row DMA strip at a time, with a ragged 10-row tail. */
static void test_choices_strip_invariance(void)
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
    for (size_t i = 0; i < px; i++) { whole[i] = strips[i] = (uint16_t)(i * 11u); }

    hud_choice_t c[HUD_CHOICE_MAX];
    t_fill_choices(c, HUD_CHOICE_MAX);

    hud_overlay_choices(whole, 0, HUD_H, false, c, HUD_CHOICE_MAX, 1);

    int nstrips = 0, ragged = 0;
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        int nrows = (y + STRIP_ROWS <= HUD_H) ? STRIP_ROWS : (HUD_H - y);
        if (nrows != STRIP_ROWS) {
            ragged = nrows;
        }
        hud_overlay_choices(strips + (size_t)y * HUD_W, y, nrows, false,
                            c, HUD_CHOICE_MAX, 1);
        nstrips++;
    }
    CHECK(ragged == 10, "expected a ragged final strip of 10 rows, got %d", ragged);
    CHECK(nstrips == 39, "expected 39 strips for 466 rows, got %d", nstrips);

    size_t diffs = 0; int fx = -1, fy = -1;
    for (size_t i = 0; i < px; i++) {
        if (whole[i] != strips[i]) {
            if (!diffs) { fy = (int)(i / HUD_W); fx = (int)(i % HUD_W); }
            diffs++;
        }
    }
    CHECK(diffs == 0, "choice strip/whole mismatch at %zu px (first x=%d y=%d)",
          diffs, fx, fy);

    /* and it must actually have drawn something, or the test is vacuous */
    size_t painted = 0;
    for (size_t i = 0; i < px; i++) {
        if (whole[i] != (uint16_t)(i * 11u)) {
            painted++;
        }
    }
    CHECK(painted > 5000, "arcs painted only %zu px — too thin to tap", painted);

    free(whole); free(strips);
}

static void test_choices_respect_bounds(void)
{
    enum { GUARD = 64, NROWS = 12 };
    const size_t body = (size_t)NROWS * HUD_W;
    uint16_t *buf = malloc((body + 2 * GUARD) * sizeof *buf);
    if (!buf) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }
    for (size_t i = 0; i < body + 2 * GUARD; i++) buf[i] = 0xA5A5;

    hud_choice_t c[HUD_CHOICE_MAX];
    t_fill_choices(c, HUD_CHOICE_MAX);

    /* y0 deliberately NOT strip-aligned */
    hud_overlay_choices(buf + GUARD, 197, NROWS, true, c, HUD_CHOICE_MAX, 0);

    size_t head = 0, tail = 0;
    for (int i = 0; i < GUARD; i++) {
        if (buf[i] != 0xA5A5) head++;
        if (buf[GUARD + body + i] != 0xA5A5) tail++;
    }
    CHECK(head == 0, "choices wrote %zu px before the buffer", head);
    CHECK(tail == 0, "choices wrote %zu px past the buffer", tail);
    free(buf);
}

/* The baked face owns every radius except three narrow bands, and the panel
 * owns everything past its own edge. Assert BOTH real limits, not looser
 * round numbers:
 *
 *   r < 215     the baked bezel ticks (r200-214). An earlier version of this
 *               test rejected only r < 210, so up to 5 px of arc could sit on
 *               top of the ticks and still pass.
 *   r > 232.5   off the glass. The frame is 466 px about a HALF-UNIT centre;
 *               an earlier version allowed r240 and 16% of the arc pixels were
 *               being hard-clipped by the framebuffer rather than drawn.
 *
 * Measured in DOUBLED coordinates about the true centre (2x-465, 2y-465) so
 * the half pixel does not make the tolerance lopsided — measuring from the
 * integer centre 232 is 0.5 px tight on one side and 0.5 px slack on the
 * other, which is exactly the size of the error being hunted. */
static void test_choices_stay_in_free_band(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }

    hud_choice_t c[HUD_CHOICE_MAX];
    t_fill_choices(c, HUD_CHOICE_MAX);
    hud_overlay_choices(fb, 0, HUD_H, false, c, HUD_CHOICE_MAX, 2);

    /* doubled radii: r215 -> 430, the glass edge r232.5 -> 465 */
    const long r_near2 = 430L * 430L;
    const long r_far2  = 465L * 465L;

    size_t inside = 0, too_close = 0, too_far = 0, edge = 0;
    for (int y = 0; y < HUD_H; y++) {
        for (int x = 0; x < HUD_W; x++) {
            if (fb[(size_t)y * HUD_W + x] == 0) {
                continue;
            }
            const long dx = 2L * x - (HUD_W - 1), dy = 2L * y - (HUD_H - 1);
            const long r2 = dx * dx + dy * dy;
            if (r2 < r_near2)     too_close++;
            else if (r2 > r_far2) too_far++;
            else                  inside++;
            if (x == 0 || y == 0 || x == HUD_W - 1 || y == HUD_H - 1) {
                edge++;
            }
        }
    }
    CHECK(too_close == 0, "%zu arc px inside r215 — over the bezel ticks", too_close);
    CHECK(too_far == 0, "%zu arc px outside r232.5 — off the glass", too_far);
    /* a pixel on row/column 0 or 465 is the framebuffer clipping the arc, which
     * is what off-glass drawing looks like from inside the buffer */
    CHECK(edge == 0, "%zu arc px on the framebuffer border — arc is clipped", edge);
    CHECK(inside > 5000, "only %zu arc px in the free band", inside);
    free(fb);
}

/* The battery rim and the choice arcs are BOTH live while a question is on
 * screen — hud_overlay_frame draws the gauge unconditionally, every frame — and
 * they share the outer free band. They used to overlap at r225-229 vs r217-236,
 * so a charge sweep ran straight through all three arcs and whichever drew
 * second won. The band is split; prove the split holds at every charge level,
 * and in the ERROR state, whose red ring rides the same radius. */
static void test_battery_and_choices_do_not_overlap(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *rim = calloc(px, sizeof *rim);
    uint16_t *arc = calloc(px, sizeof *arc);
    if (!rim || !arc) {
        printf("FAIL %s: alloc\n", __func__); g_failures++; free(rim); free(arc); return;
    }

    hud_choice_t c[HUD_CHOICE_MAX];
    t_fill_choices(c, HUD_CHOICE_MAX);
    hud_overlay_choices(arc, 0, HUD_H, false, c, HUD_CHOICE_MAX, 0);

    static const int pcts[] = { 5, 50, 100 };
    for (size_t k = 0; k < sizeof pcts / sizeof pcts[0]; k++) {
        for (int err = 0; err < 2; err++) {
            memset(rim, 0, px * sizeof *rim);
            hud_env_t env = { .face = err ? HUD_FACE_ERROR : HUD_FACE_IDLE,
                              .amp = 0, .batt_pct = (uint8_t)pcts[k],
                              .charging = false, .ox = 0, .oy = 0 };
            hud_overlay_frame(rim, 0, HUD_H, 500, false, &env);

            size_t clash = 0;
            int cx = -1, cy = -1;
            for (int y = 0; y < HUD_H; y++) {
                for (int x = 0; x < HUD_W; x++) {
                    const size_t i = (size_t)y * HUD_W + x;
                    if (rim[i] && arc[i]) {
                        if (!clash) { cx = x; cy = y; }
                        clash++;
                    }
                }
            }
            CHECK(clash == 0,
                  "batt %d%% err=%d: %zu px collide with the arcs (first %d,%d)",
                  pcts[k], err, clash, cx, cy);
        }
    }
    free(rim); free(arc);
}

/* n evenly spaced, non-overlapping spans that leave 12 o'clock clear. */
static void test_choice_layout_spans(void)
{
    for (int n = 1; n <= HUD_CHOICE_MAX; n++) {
        hud_choice_t c[HUD_CHOICE_MAX];
        memset(c, 0, sizeof c);
        hud_choice_layout(n, c);

        /* every angle belongs to at most one span, and 12 o'clock to none */
        int hits[256];
        memset(hits, 0, sizeof hits);
        for (int a = 0; a < 256; a++) {
            for (int i = 0; i < n; i++) {
                const int a0 = c[i].a0, a1 = c[i].a1;
                const bool in = (a0 <= a1) ? (a >= a0 && a <= a1)
                                           : (a >= a0 || a <= a1);
                if (in) {
                    hits[a]++;
                }
            }
        }
        int overlaps = 0, covered = 0;
        for (int a = 0; a < 256; a++) {
            if (hits[a] > 1) overlaps++;
            if (hits[a]) covered++;
        }
        CHECK(overlaps == 0, "n=%d: %d angles claimed by two arcs", n, overlaps);

        /* the reserved 12 o'clock gap (~1.2 rad == ~48 Q8) must be clear */
        for (int d = -20; d <= 20; d++) {
            const int a = (192 + d) & 255;
            CHECK(hits[a] == 0, "n=%d: angle %d sits in the question gap", n, a);
        }
        /* ...and the arcs must still fill most of the rest */
        CHECK(covered >= 256 - 48 - n * 2 - 6,
              "n=%d: arcs cover only %d/256 units", n, covered);

        /* spans must be usefully wide, not slivers */
        for (int i = 0; i < n; i++) {
            const int w = (c[i].a1 - c[i].a0) & 255;
            CHECK(w >= 20 && w <= 256 - 48,
                  "n=%d arc %d has width %d", n, i, w);
        }
    }
}

static void test_choice_hit(void)
{
    hud_choice_t c[HUD_CHOICE_MAX];
    t_fill_choices(c, HUD_CHOICE_MAX);

    int idx = 99;
    /* dead centre never answers */
    CHECK(!hud_choice_hit(c, HUD_CHOICE_MAX, 232, 232, &idx),
          "centre tap answered a question");
    CHECK(idx == -1, "miss must report -1, got %d", idx);

    /* a tap at r=100 sits inside the face even if its angle is on an arc */
    for (int i = 0; i < HUD_CHOICE_MAX; i++) {
        int x, y;
        t_point(t_span_mid(&c[i]), 100, &x, &y);
        CHECK(!hud_choice_hit(c, HUD_CHOICE_MAX, x, y, &idx),
              "r=100 tap on arc %d's bearing answered it", i);
    }

    /* the middle of each arc hits that arc, at the arc's own radius */
    for (int i = 0; i < HUD_CHOICE_MAX; i++) {
        int x, y;
        t_point(t_span_mid(&c[i]), 226, &x, &y);
        idx = 99;
        CHECK(hud_choice_hit(c, HUD_CHOICE_MAX, x, y, &idx),
              "arc %d not hit at its own midpoint (%d,%d)", i, x, y);
        CHECK(idx == i, "tap on arc %d reported %d", i, idx);
    }

    /* the 12 o'clock question gap answers nothing */
    for (int d = -16; d <= 16; d += 8) {
        int x, y;
        t_point((192 + d) & 255, 226, &x, &y);
        CHECK(!hud_choice_hit(c, HUD_CHOICE_MAX, x, y, &idx),
              "tap in the question gap (a=%d) answered", (192 + d) & 255);
    }

    /* a NULL slot is unused and must never hit */
    hud_choice_t blanked[HUD_CHOICE_MAX];
    memcpy(blanked, c, sizeof blanked);
    blanked[1].label = NULL;
    {
        int x, y;
        t_point(t_span_mid(&blanked[1]), 226, &x, &y);
        CHECK(!hud_choice_hit(blanked, HUD_CHOICE_MAX, x, y, &idx),
              "a NULL-label slot was hit");
    }

    /* a WRAPPING span [240..20] must be hit on BOTH sides of 0, and must not
     * bleed outside itself. Probes are inset a few units from the edges so the
     * LUT-step flooring in the angle conversion cannot flip the expectation. */
    hud_choice_t wrap[1] = { { "wrap", 240, 20 } };
    static const int inside[]  = { 244, 250, 255, 0, 6, 16 };
    static const int outside[] = { 30, 64, 128, 192, 230 };
    for (size_t i = 0; i < sizeof inside / sizeof inside[0]; i++) {
        int x, y;
        t_point(inside[i], 226, &x, &y);
        idx = 99;
        CHECK(hud_choice_hit(wrap, 1, x, y, &idx) && idx == 0,
              "wrapping span missed at a=%d (%d,%d)", inside[i], x, y);
    }
    for (size_t i = 0; i < sizeof outside / sizeof outside[0]; i++) {
        int x, y;
        t_point(outside[i], 226, &x, &y);
        CHECK(!hud_choice_hit(wrap, 1, x, y, &idx),
              "wrapping span over-claimed at a=%d", outside[i]);
    }
}

/* A tap on the face must never answer a question.
 *
 * The first hit test gated only on a MINIMUM radius (110) and then tested the
 * bearing, so the whole annulus from r111 to the corner of the framebuffer
 * answered: a poke at the baked face's outer ring (r155-179) or its bezel
 * ticks (r200-214) sent a wrong answer to Gemini. Sweep every bearing at every
 * radius the baked art occupies and require silence. */
static void test_choice_hit_rejects_face_and_bezel(void)
{
    hud_choice_t c[HUD_CHOICE_MAX];
    t_fill_choices(c, HUD_CHOICE_MAX);

    /* r214 is the outermost bezel tick; r215 is the first free pixel. Sample
     * across the whole occupied face, not just below the old gate. */
    static const int face_r[] = { 10, 60, 94, 110, 111, 130, 150, 165, 179,
                                  190, 200, 207, 214 };
    size_t answered = 0;
    int bad_r = -1, bad_a = -1, bad_idx = -1;
    for (size_t k = 0; k < sizeof face_r / sizeof face_r[0]; k++) {
        for (int a = 0; a < 256; a++) {
            int x, y, idx = 99;
            t_point(a, face_r[k], &x, &y);
            if (hud_choice_hit(c, HUD_CHOICE_MAX, x, y, &idx)) {
                if (!answered) { bad_r = face_r[k]; bad_a = a; bad_idx = idx; }
                answered++;
            }
        }
    }
    CHECK(answered == 0,
          "%zu taps on the baked face answered (first r=%d a=%d -> arc %d)",
          answered, bad_r, bad_a, bad_idx);

    /* ...and a tap well past the outward slop is not a tap on an arc either */
    size_t far_answered = 0;
    for (int a = 0; a < 256; a++) {
        int x, y, idx = 99;
        t_point(a, 300, &x, &y);
        if (x < 0 || x >= HUD_W || y < 0 || y >= HUD_H) {
            continue;               /* off-buffer; the driver cannot report it */
        }
        if (hud_choice_hit(c, HUD_CHOICE_MAX, x, y, &idx)) {
            far_answered++;
        }
    }
    CHECK(far_answered == 0, "%zu taps at r=300 answered", far_answered);
}

/* What you see is what you tap — IN BOTH DIRECTIONS.
 *
 * The renderer decides membership with cross products against the span's
 * boundary rays; the hit test decides it from an integer atan2 plus a radial
 * band. Those are independent code paths, and a user cannot tell them apart.
 *
 *   FORWARD   every painted px of arc i must hit-test to i. Catches a bearing
 *             that lights one arc and answers another.
 *   CONVERSE  every px of the visible disc that hit-tests to i must lie within
 *             T_HIT_SLOP px of arc i's painted set. Catches the opposite and
 *             far nastier failure: a target much LARGER than the graphic, i.e.
 *             a tap that answers a question with nothing under the finger.
 *             The forward direction alone cannot see it — it was green while
 *             102,115 px answered versus 20,954 px painted (4.9x).
 *
 * T_HIT_SLOP has to cover the deliberate inward tap tolerance (8 px, so a
 * finger landing short of a bezel-hugging arc still counts) plus the ~5.7 px
 * that one Q8 angular step spans at this radius, where the hit test's floored
 * atan2 and the renderer's cross products can legitimately disagree. */
#define T_HIT_SLOP 14

static void test_choice_render_and_hit_agree(void)
{
    hud_choice_t c[HUD_CHOICE_MAX];
    t_fill_choices(c, HUD_CHOICE_MAX);

    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    uint8_t *mask = calloc(px * HUD_CHOICE_MAX, 1);
    if (!fb || !mask) {
        printf("FAIL %s: alloc\n", __func__); g_failures++; free(fb); free(mask); return;
    }

    size_t agree = 0, disagree = 0, painted = 0;
    int bad_x = -1, bad_y = -1, bad_i = -1, bad_got = -1;
    for (int i = 0; i < HUD_CHOICE_MAX; i++) {
        hud_choice_t solo[HUD_CHOICE_MAX];
        memcpy(solo, c, sizeof solo);
        for (int j = 0; j < HUD_CHOICE_MAX; j++) {
            if (j != i) {
                solo[j].label = NULL;   /* render arc i by itself */
            }
        }
        memset(fb, 0, px * sizeof *fb);
        hud_overlay_choices(fb, 0, HUD_H, false, solo, HUD_CHOICE_MAX, -1);

        for (int y = 0; y < HUD_H; y++) {
            for (int x = 0; x < HUD_W; x++) {
                const size_t p = (size_t)y * HUD_W + x;
                if (fb[p] == 0) {
                    continue;
                }
                mask[(size_t)i * px + p] = 1;
                painted++;
                int idx = -1;
                hud_choice_hit(c, HUD_CHOICE_MAX, x, y, &idx);
                if (idx == i) {
                    agree++;
                } else {
                    if (!disagree) {
                        bad_x = x; bad_y = y; bad_i = i; bad_got = idx;
                    }
                    disagree++;
                }
            }
        }
    }
    CHECK(agree > 5000, "only %zu arc px to check", agree);
    CHECK(disagree == 0,
          "%zu lit px answer the wrong arc (first: arc %d px %d,%d -> %d)",
          disagree, bad_i, bad_x, bad_y, bad_got);

    /* ---- the converse: nothing answers that is not under a painted arc ---- */
    size_t answering = 0, orphan = 0;
    int worst = 0, ox = -1, oy = -1, oi = -1;
    for (int y = 0; y < HUD_H; y++) {
        for (int x = 0; x < HUD_W; x++) {
            /* the visible disc only — off-glass pixels cannot be tapped */
            const long ddx = 2L * x - (HUD_W - 1), ddy = 2L * y - (HUD_H - 1);
            if (ddx * ddx + ddy * ddy > 465L * 465L) {
                continue;
            }
            int idx = -1;
            if (!hud_choice_hit(c, HUD_CHOICE_MAX, x, y, &idx) || idx < 0) {
                continue;
            }
            answering++;
            /* nearest painted px of the arc this pixel claims to be */
            int best = T_HIT_SLOP + 1;
            for (int j = -T_HIT_SLOP; j <= T_HIT_SLOP && best > 0; j++) {
                const int yy = y + j;
                if (yy < 0 || yy >= HUD_H) continue;
                for (int i2 = -T_HIT_SLOP; i2 <= T_HIT_SLOP; i2++) {
                    const int xx = x + i2;
                    if (xx < 0 || xx >= HUD_W) continue;
                    if (!mask[(size_t)idx * px + (size_t)yy * HUD_W + xx]) continue;
                    const int d2 = i2 * i2 + j * j;
                    int d = 0;
                    while ((d + 1) * (d + 1) <= d2) d++;   /* floor(sqrt(d2)) */
                    if (d < best) best = d;
                }
            }
            if (best > worst) { worst = best; ox = x; oy = y; oi = idx; }
            if (best > T_HIT_SLOP) {
                orphan++;
            }
        }
    }
    printf("      [agree] painted=%zu answering=%zu (%.2fx) worst-gap=%d px\n",
           painted, answering, painted ? (double)answering / (double)painted : 0.0,
           worst);
    CHECK(orphan == 0,
          "%zu px answer an arc with no arc under them (worst %d px at %d,%d -> arc %d)",
          orphan, worst, ox, oy, oi);
    /* Belt and braces on the same defect, stated globally as the reviewer
     * measured it. The per-pixel orphan check above is the sharp one; this is
     * the blunt instrument that stays meaningful even if the slop constants
     * move, because the bound is DERIVED, not guessed:
     *
     *   painted   arcs span r223..231, 9 px of radius.
     *   answering the band plus slop is r215..255, clipped by the glass at
     *             r232.5, so 17.5 px of radius reach the user's finger.
     *
     * 17.5/9 = 1.95, and the hit test's floored atan2 adds a fraction of a Q8
     * step at each arc end, so ~2.0 is the designed figure. 2.5 leaves room for
     * a slop tweak; 4.9x — the unbounded annulus that handed the baked face's
     * rings and bezel ticks to whichever arc shared their bearing — does not
     * fit under it, and neither does anything else that forgets the outer
     * bound. If this fires, re-derive it rather than raising it. */
    CHECK((double)answering <= (double)painted * 2.5,
          "tap target is %.2fx the painted arcs — too big to be what you see",
          painted ? (double)answering / (double)painted : 0.0);

    free(fb); free(mask);
}

/* ---- ask text helpers (the STATE-05/06 text layer's geometry) --------- */

static void test_wrap2_short_fits_one_line(void)
{
    char l1[25], l2[25];
    memset(l1, 'X', sizeof l1);
    memset(l2, 'X', sizeof l2);
    hud_wrap2("hello", 24, l1, l2);
    CHECK(strcmp(l1, "hello") == 0, "l1='%s'", l1);
    CHECK(l2[0] == '\0', "l2 should be empty, got '%s'", l2);
}

static void test_wrap2_splits_at_space(void)
{
    char l1[25], l2[25];
    /* 29 chars; the break must land on the last space at or before col 24,
     * and the break space must be consumed, not carried onto either line. */
    hud_wrap2("have you considered rebooting", 24, l1, l2);
    CHECK(strcmp(l1, "have you considered") == 0, "l1='%s'", l1);
    CHECK(strcmp(l2, "rebooting") == 0, "l2='%s'", l2);
}

static void test_wrap2_hard_splits_long_word(void)
{
    char l1[25], l2[25];
    /* 48 chars, no spaces: nothing to break on, so 24/24 hard split. */
    const char *s = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv";
    hud_wrap2(s, 24, l1, l2);
    CHECK(strlen(l1) == 24 && strncmp(l1, s, 24) == 0, "l1='%s'", l1);
    CHECK(strlen(l2) == 24 && strcmp(l2, s + 24) == 0, "l2='%s'", l2);
}

static void test_wrap2_truncates_overflow(void)
{
    char l1[25], l2[25];
    /* 71 chars — more than 2*24, so line 2 must hard-truncate at 24 and
     * still terminate. The break still lands on a word boundary. */
    const char *s =
        "would you like the long answer or the short answer to this question sir";
    hud_wrap2(s, 24, l1, l2);
    CHECK(strcmp(l1, "would you like the long") == 0, "l1='%s'", l1);
    CHECK(strlen(l2) == 24, "overflow should fill line 2 to 24, got %zu",
          strlen(l2));
    CHECK(strncmp(l2, "answer or the short answ", 24) == 0, "l2='%s'", l2);
}

static void test_wrap2_preserves_48_char_sentence(void)
{
    char l1[25], l2[25];
    /* exactly 2*24 chars with a space at col 24: nothing may be lost — the
     * two lines rejoined across the consumed break space must equal the
     * original text. */
    const char *s = "this question is exactly forty eight chars long!";
    hud_wrap2(s, 24, l1, l2);
    char joined[64];
    snprintf(joined, sizeof joined, "%s %s", l1, l2);
    CHECK(strcmp(joined, s) == 0, "lost characters: '%s' + '%s'", l1, l2);
    CHECK(strlen(l1) <= 24 && strlen(l2) <= 24, "line overflow");
}

static void test_wrap2_terminates_and_empty(void)
{
    char l1[25], l2[25];
    memset(l1, 'X', sizeof l1);
    memset(l2, 'X', sizeof l2);
    hud_wrap2("", 24, l1, l2);
    CHECK(l1[0] == '\0', "empty text: l1 not terminated empty");
    CHECK(l2[0] == '\0', "empty text: l2 not terminated empty");

    /* termination must hold even when both lines fill completely */
    memset(l1, 'X', sizeof l1);
    memset(l2, 'X', sizeof l2);
    hud_wrap2("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz", 24,
              l1, l2);
    CHECK(l1[24] == '\0', "l1 not NUL-terminated at max_cols");
    CHECK(l2[24] == '\0', "l2 not NUL-terminated at max_cols");
}

/* Every label anchor from the real 3-choice layout must land fully on-screen
 * (every box corner inside the framebuffer) with the box CENTRE within
 * r=215+24 of the true centre — the clamped-box equivalent of "no text past
 * the glass", since the drawn text hugs the anchor. The bottom arc's label
 * must actually sit in the 6 o'clock region, not just somewhere legal. */
static void test_label_anchor_on_screen(void)
{
    hud_choice_t c[HUD_CHOICE_MAX];
    t_fill_choices(c, HUD_CHOICE_MAX);

    const int r = 205, w = 60, h = 7;
    int bottom_y = -1;
    for (int i = 0; i < HUD_CHOICE_MAX; i++) {
        int x = -1, y = -1;
        hud_choice_label_anchor(&c[i], r, w, h, &x, &y);
        CHECK(x >= 0 && y >= 0 && x + w <= HUD_W && y + h <= HUD_H,
              "arc %d anchor box off-screen: x=%d y=%d", i, x, y);
        /* box centre in doubled coords about the true centre (232.5, 232.5) */
        const long cx2 = 2L * x + w - (HUD_W - 1);
        const long cy2 = 2L * y + h - (HUD_H - 1);
        CHECK(cx2 * cx2 + cy2 * cy2 <= (2L * (215 + 24)) * (2L * (215 + 24)),
              "arc %d label centre strays past r239", i);
        if (y > bottom_y) {
            bottom_y = y;
        }
    }
    CHECK(bottom_y > 300, "no label in the 6 o'clock region (max y=%d)",
          bottom_y);
}

/* The square clamp is not the glass. Near-horizontal arcs — BOTH n=2 arcs and
 * the n=3 side arcs — put the r=205 anchor centre ~200 px out, where a
 * square-clamped box's corners ran under the arc annulus (r>=223) from ~8
 * label chars and off the r=232.5 glass edge from ~10. Options may legally be
 * 19 chars, so prove the radial clamp at exactly that width: every corner of
 * every box inside HUD_LABEL_R_SAFE (verifier bound 2*R+1 in doubled coords)
 * and strictly inside the r=223 arc band — the farthest point of an
 * axis-aligned box from the centre is a corner, so corner checks cover the
 * whole box. The clamp must slide boxes along the chord, not teleport them:
 * the n=2 labels must stay on their own sides of the screen. */
static void test_label_anchor_radial_clamp_19_chars(void)
{
    const int w = 6 * 19, h = 7;
    for (int n = 2; n <= HUD_CHOICE_MAX; n++) {
        hud_choice_t c[HUD_CHOICE_MAX];
        memset(c, 0, sizeof c);
        hud_choice_layout(n, c);
        for (int i = 0; i < n; i++) {
            int x = -1, y = -1;
            hud_choice_label_anchor(&c[i], 205, w, h, &x, &y);
            CHECK(x >= 0 && y >= 0 && x + w <= HUD_W && y + h <= HUD_H,
                  "n=%d arc %d box off-screen: x=%d y=%d", n, i, x, y);
            const int cxs[2] = { x, x + w - 1 };
            const int cys[2] = { y, y + h - 1 };
            for (int a = 0; a < 2; a++) {
                for (int b = 0; b < 2; b++) {
                    const long dx = 2L * cxs[a] - (HUD_W - 1);
                    const long dy = 2L * cys[b] - (HUD_H - 1);
                    const long r2 = dx * dx + dy * dy;
                    CHECK(r2 <= (2L * HUD_LABEL_R_SAFE + 1) *
                                (2L * HUD_LABEL_R_SAFE + 1),
                          "n=%d arc %d corner (%d,%d) past r%d",
                          n, i, cxs[a], cys[b], HUD_LABEL_R_SAFE);
                    CHECK(r2 < (2L * 223) * (2L * 223),
                          "n=%d arc %d corner (%d,%d) touches the arc band",
                          n, i, cxs[a], cys[b]);
                }
            }
            if (n == 2) {
                /* arc 0 is the right-side arc, arc 1 the left-side one */
                const int cx = x + w / 2;
                CHECK(i == 0 ? cx > 233 : cx < 233,
                      "n=2 arc %d label crossed to the wrong side (cx=%d)",
                      i, cx);
            }
        }
    }
}

/* Lifted verbatim from jr_display.c native_darken() — the reference the fold
 * must match. Do not "simplify" it: the point is comparing hud_dim565 against
 * the real per-channel transform, not against a re-model of it. */
static uint16_t ref_native_darken(uint16_t pixel)
{
    uint16_t r = (pixel >> 11) & 0x1fU;
    uint16_t g = (pixel >> 5) & 0x3fU;
    uint16_t b = pixel & 0x1fU;
    return (uint16_t)(((r >> 2) << 11) | ((g >> 2) << 5) | (b >> 2));
}

/* hud_dim565 folds jr_display.c's per-pixel dim chain — panel_native (bswap
 * if swapped), native_darken, panel_order_color (bswap back) — into one
 * masked shift. The green channel spans both bytes in the swapped layout, so
 * nothing short of EXHAUSTIVE equality is proof: all 65536 values, both byte
 * orders. */
static void test_dim565_matches_darken_chain(void)
{
    size_t bad_plain = 0, bad_swap = 0;
    unsigned first_plain = 0, first_swap = 0;
    for (uint32_t v = 0; v <= 0xFFFFu; v++) {
        const uint16_t p = (uint16_t)v;
        if (hud_dim565(p, false) != ref_native_darken(p)) {
            if (!bad_plain) first_plain = (unsigned)v;
            bad_plain++;
        }
        /* swapped path: memory -> native -> darken -> memory */
        const uint16_t nat = (uint16_t)((p >> 8) | (p << 8));
        const uint16_t dark = ref_native_darken(nat);
        const uint16_t want = (uint16_t)((dark >> 8) | (dark << 8));
        if (hud_dim565(p, true) != want) {
            if (!bad_swap) first_swap = (unsigned)v;
            bad_swap++;
        }
    }
    CHECK(bad_plain == 0, "%zu plain-path mismatches (first 0x%04x)",
          bad_plain, first_plain);
    CHECK(bad_swap == 0, "%zu swapped-path mismatches (first 0x%04x)",
          bad_swap, first_swap);
}

/* hud_fade565's endpoints, pinned exhaustively: k=0 is the identity and k=32
 * is exactly hud_dim565 (itself proven against the darken chain above) — all
 * 65536 values, both byte orders. Between the endpoints every channel must
 * fall monotonically as k rises, or the watch fade would shimmer. */
static void test_fade565_endpoints_and_monotonic(void)
{
    size_t bad_id = 0, bad_full = 0;
    for (uint32_t v = 0; v <= 0xFFFFu; v++) {
        const uint16_t p = (uint16_t)v;
        if (hud_fade565(p, false, 0) != p || hud_fade565(p, true, 0) != p) {
            bad_id++;
        }
        if (hud_fade565(p, false, 32) != hud_dim565(p, false) ||
            hud_fade565(p, true, 32) != hud_dim565(p, true)) {
            bad_full++;
        }
    }
    CHECK(bad_id == 0, "%zu identity (k=0) mismatches", bad_id);
    CHECK(bad_full == 0, "%zu full-dim (k=32) mismatches vs hud_dim565",
          bad_full);

    int pr = 31, pg = 63, pb = 31;
    for (int k = 1; k <= 32; k++) {
        const uint16_t v = hud_fade565(0xFFFFu, false, k);
        const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        CHECK(r <= pr && g <= pg && b <= pb,
              "channel rose at k=%d (r%d g%d b%d)", k, r, g, b);
        pr = r; pg = g; pb = b;
    }
}

/* hud_mix565's endpoints: m=0 must return `under` and m=32 must return
 * `over`, bit-exactly, both byte orders — for each operand paired against a
 * fixed witness sweeping the operand exhausts both positions. Midpoint
 * sanity: black mixed half toward white lands on the half-scale channels. */
static void test_mix565_endpoints(void)
{
    size_t bad = 0;
    for (uint32_t v = 0; v <= 0xFFFFu; v++) {
        const uint16_t p = (uint16_t)v;
        if (hud_mix565(p, 0x1234u, 0, false) != p ||
            hud_mix565(p, 0x1234u, 0, true) != p ||
            hud_mix565(0x1234u, p, 32, false) != p ||
            hud_mix565(0x1234u, p, 32, true) != p) {
            bad++;
        }
    }
    CHECK(bad == 0, "%zu endpoint mismatches", bad);

    const uint16_t half = hud_mix565(0x0000u, 0xFFFFu, 16, false);
    CHECK(((half >> 11) & 31) == 15 && ((half >> 5) & 63) == 31 &&
          (half & 31) == 15, "midpoint mix wrong (0x%04x)", half);
}

/* A wrapping span [240..20] crosses 3 o'clock; its midpoint is a=2, just past
 * 3 o'clock — NOT the complementary a=130 over by 9 o'clock. A naive
 * (a0+a1)/2 lands on the wrong side of the screen; the anchor must not. */
static void test_label_anchor_wrap_span(void)
{
    const hud_choice_t wrap = { "wrap", 240, 20 };
    int x = -1, y = -1;
    hud_choice_label_anchor(&wrap, 205, 24, 7, &x, &y);
    CHECK(x > 350, "wrap-span label on the complementary side: x=%d", x);
    CHECK(y + 3 > 200 && y + 3 < 266,
          "wrap-span label strayed vertically: y=%d", y);
}

/* ---- caption chip (STATE-04) + tap ripple (TRANS-05) ------------------ */

/* The caption's dim band trusts hud_glass_chord for its per-row extent, and
 * the comment in the coordinator spec claims 38 columns of scale-1 text
 * always fit on the chord at the caption rows. Prove it instead: for EVERY
 * possible line length at every row of both line layouts, the centred text
 * extent must sit inside the chord, and every text row inside the 386..425
 * band. Also pin the chord function's own edges. */
static void test_caption_text_fits_glass_chord(void)
{
    CHECK(hud_glass_chord(233) == 233, "chord at centre row should be 233");
    CHECK(hud_glass_chord(0) == 0, "row 0 misses the glass (dy=-233)");
    CHECK(hud_glass_chord(466) == 0, "row 466 misses the glass");
    CHECK(hud_glass_chord(-5) == 0 && hud_glass_chord(500) == 0,
          "off-frame rows must return 0");

    /* single line rows 403..409; two-line rows 396..402 and 408..414 */
    static const int bands[3][2] = { {403, 409}, {396, 402}, {408, 414} };
    for (int len = 1; len <= 38; len++) {
        const int x0 = (HUD_W - 6 * len) / 2;
        const int x1 = x0 + 6 * len - 1;
        for (size_t b = 0; b < 3; b++) {
            for (int y = bands[b][0]; y <= bands[b][1]; y++) {
                CHECK(y >= 386 && y <= 425,
                      "text row %d outside the 386..425 band", y);
                const int half = hud_glass_chord(y);
                CHECK(x0 >= 233 - half && x1 <= 233 + half,
                      "len=%d row=%d text [%d,%d] outside chord [%d,%d]",
                      len, y, x0, x1, 233 - half, 233 + half);
            }
        }
    }
}

/* Expiry, the ring annulus, and the glass clip, all at once. The innermost
 * painted pixel of a scanline-solved annulus may sit up to a pixel inside
 * r_in (isqrt flooring, same as ov_ring_row), hence the rad-2 inner bound. */
static void test_ripple_geometry(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }

    /* expired (== and >) paints nothing */
    hud_overlay_ripple(fb, 0, HUD_H, false, 233, 233, HUD_RIPPLE_MS);
    hud_overlay_ripple(fb, 0, HUD_H, false, 233, 233, 5000u);
    size_t stale = 0;
    for (size_t i = 0; i < px; i++) if (fb[i]) stale++;
    CHECK(stale == 0, "expired ripple painted %zu px", stale);

    /* mid-animation, tap well on-glass */
    const uint32_t age = 200;
    const int cx = 300, cy = 180;
    const int rad = HUD_RIPPLE_R0 +
        (int)(((HUD_RIPPLE_R1 - HUD_RIPPLE_R0) * age) / HUD_RIPPLE_MS);
    hud_overlay_ripple(fb, 0, HUD_H, false, cx, cy, age);

    size_t painted = 0, off_glass = 0, off_ring = 0;
    for (int y = 0; y < HUD_H; y++) {
        for (int x = 0; x < HUD_W; x++) {
            if (fb[(size_t)y * HUD_W + x] == 0) {
                continue;
            }
            painted++;
            const int gx = x - 233, gy = y - 233;
            if (gx * gx + gy * gy > 232 * 232) off_glass++;
            const int dx = x - cx, dy = y - cy;
            const int r2 = dx * dx + dy * dy;
            if (r2 < (rad - 2) * (rad - 2) || r2 > (rad + 1) * (rad + 1)) {
                off_ring++;
            }
        }
    }
    CHECK(painted > 200, "ripple painted only %zu px", painted);
    CHECK(off_glass == 0, "%zu ripple px off-glass", off_glass);
    CHECK(off_ring == 0, "%zu ripple px outside the age-%u annulus",
          off_ring, (unsigned)age);
    free(fb);
}

/* Same 12-row strip contract (with the ragged 10-row tail) as every other
 * overlay: full-frame vs strips must be byte-identical mid-animation. */
static void test_ripple_strip_invariance(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *whole = malloc(px * sizeof *whole);
    uint16_t *strips = malloc(px * sizeof *strips);
    if (!whole || !strips) {
        printf("FAIL %s: alloc\n", __func__); g_failures++;
        free(whole); free(strips); return;
    }
    for (size_t i = 0; i < px; i++) { whole[i] = strips[i] = (uint16_t)(i * 13u); }

    hud_overlay_ripple(whole, 0, HUD_H, false, 150, 340, 137u);
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        int nrows = (y + STRIP_ROWS <= HUD_H) ? STRIP_ROWS : (HUD_H - y);
        hud_overlay_ripple(strips + (size_t)y * HUD_W, y, nrows, false,
                           150, 340, 137u);
    }
    size_t diffs = 0, painted = 0;
    for (size_t i = 0; i < px; i++) {
        if (whole[i] != strips[i]) diffs++;
        if (whole[i] != (uint16_t)(i * 13u)) painted++;
    }
    CHECK(diffs == 0, "ripple strip/whole mismatch at %zu px", diffs);
    CHECK(painted > 100, "ripple painted only %zu px — vacuous", painted);
    free(whole); free(strips);
}

/* A corner tap sits at r~273, OFF the glass. Early the whole ring is outside
 * (must paint nothing); late the ring's inner edge crosses onto the glass
 * (must paint, on-glass only). Never a pixel outside r232 — the framebuffer
 * corner around the tap point is exactly where an unclipped ring would go. */
static void test_ripple_corner_tap_stays_on_glass(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }

    static const uint32_t ages[] = { 60, 200, 380 };
    for (size_t k = 0; k < sizeof ages / sizeof ages[0]; k++) {
        memset(fb, 0, px * sizeof *fb);
        hud_overlay_ripple(fb, 0, HUD_H, false, 40, 40, ages[k]);
        size_t painted = 0, off_glass = 0;
        for (int y = 0; y < HUD_H; y++) {
            for (int x = 0; x < HUD_W; x++) {
                if (fb[(size_t)y * HUD_W + x] == 0) continue;
                painted++;
                const int gx = x - 233, gy = y - 233;
                if (gx * gx + gy * gy > 232 * 232) off_glass++;
            }
        }
        CHECK(off_glass == 0, "age=%u: %zu corner-tap px off-glass",
              (unsigned)ages[k], off_glass);
        if (ages[k] == 60) {
            CHECK(painted == 0,
                  "age=60: ring is fully off-glass yet painted %zu px", painted);
        }
        if (ages[k] == 380) {
            CHECK(painted > 0,
                  "age=380: ring reaches the glass but painted nothing");
        }
    }
    free(fb);
}

/* ---- wake bloom (TRANS-01) -------------------------------------------- */

/* The wavefront expands monotonically with age (ease-out means later ==
 * farther), every painted pixel stays inside r<=152 about (232,232) — always
 * on the glass — and an expired age paints nothing at all, which is the
 * whole self-clearing contract. */
static void test_bloom_expands_expires_and_bounds(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }

    hud_overlay_bloom(fb, 0, HUD_H, false, HUD_BLOOM_MS);
    hud_overlay_bloom(fb, 0, HUD_H, false, HUD_BLOOM_MS + 1000u);
    size_t painted = 0;
    for (size_t i = 0; i < px; i++) if (fb[i] != 0) painted++;
    CHECK(painted == 0, "expired bloom painted %zu px", painted);

    long r2_max[2] = {0, 0};
    static const uint32_t ages[2] = {60u, 420u};
    for (int k = 0; k < 2; k++) {
        memset(fb, 0, px * sizeof *fb);
        hud_overlay_bloom(fb, 0, HUD_H, false, ages[k]);
        size_t n = 0;
        for (int y = 0; y < HUD_H; y++) {
            for (int x = 0; x < HUD_W; x++) {
                if (fb[(size_t)y * HUD_W + x] == 0) continue;
                n++;
                const long dx = x - 232, dy = y - 232;
                const long r2 = dx * dx + dy * dy;
                if (r2 > r2_max[k]) r2_max[k] = r2;
            }
        }
        CHECK(n > 200, "age=%u painted only %zu px", ages[k], n);
        CHECK(r2_max[k] <= 152L * 152L, "age=%u reached r^2=%ld past r152",
              ages[k], r2_max[k]);
    }
    CHECK(r2_max[0] < r2_max[1],
          "wavefront did not expand (r^2 %ld -> %ld)", r2_max[0], r2_max[1]);
    free(fb);
}

/* Same strip contract as every other overlay, checked mid-flight while both
 * the seed and the wavefront are live. */
static void test_bloom_strip_invariance(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *whole = malloc(px * sizeof *whole);
    uint16_t *strips = malloc(px * sizeof *strips);
    if (!whole || !strips) {
        printf("FAIL %s: alloc\n", __func__); g_failures++;
        free(whole); free(strips); return;
    }
    for (size_t i = 0; i < px; i++) { whole[i] = strips[i] = (uint16_t)(i * 17u); }
    hud_overlay_bloom(whole, 0, HUD_H, false, 180u);
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        int nrows = (y + STRIP_ROWS <= HUD_H) ? STRIP_ROWS : (HUD_H - y);
        hud_overlay_bloom(strips + (size_t)y * HUD_W, y, nrows, false, 180u);
    }
    size_t diffs = 0;
    for (size_t i = 0; i < px; i++) if (whole[i] != strips[i]) diffs++;
    CHECK(diffs == 0, "bloom strip/whole mismatch at %zu px", diffs);
    free(whole); free(strips);
}

/* ---- ambient watch face (UI-01) --------------------------------------- */

/* Hands are distinguishable by colour: the minute hand and hub are pure
 * white 0xffff, the seconds hand is gold (the only non-white colour with a
 * red component), the hour hand is cyan (no red). Hand pixels are separated
 * from the hub by radius: the hub ends at r=5, the hands start at r=20, so
 * r^2 > 100 is cleanly a hand. Direction paper-check baked in: hh=3 -> hour
 * RIGHT (a=0), mm=0 -> minute UP (a=192), ss=30 -> seconds DOWN (a=64),
 * mm=30 -> minute DOWN. */
static void test_clock_hand_angles(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }

    hud_overlay_clock(fb, 0, HUD_H, false, 3, 0, 30, 255);
    size_t hour = 0, minute = 0, second = 0;
    size_t hour_bad = 0, min_bad = 0, sec_bad = 0;
    for (int y = 0; y < HUD_H; y++) {
        for (int x = 0; x < HUD_W; x++) {
            const uint16_t p = fb[(size_t)y * HUD_W + x];
            if (p == 0) continue;
            const int dx = x - 232, dy = y - 232;
            const int r2 = dx * dx + dy * dy;
            if (p == 0xffff) {                    /* minute hand + hub */
                if (r2 > 100) {
                    minute++;
                    if (y >= 233) min_bad++;
                }
            } else if ((p & 0xF800u) != 0) {      /* seconds hand: gold */
                second++;
                if (y <= 233) sec_bad++;
            } else {                              /* hour hand: cyan */
                hour++;
                if (x <= 233) hour_bad++;
            }
        }
    }
    CHECK(hour > 100, "hour hand painted only %zu px", hour);
    CHECK(minute > 100, "minute hand painted only %zu px", minute);
    CHECK(second > 50, "seconds hand painted only %zu px", second);
    CHECK(hour_bad == 0, "hh=3: %zu hour px not right of centre", hour_bad);
    CHECK(min_bad == 0, "mm=0: %zu minute px not above centre", min_bad);
    CHECK(sec_bad == 0, "ss=30: %zu seconds px not below centre", sec_bad);

    memset(fb, 0, px * sizeof *fb);
    hud_overlay_clock(fb, 0, HUD_H, false, 0, 30, 0, 255);
    size_t down_bad = 0;
    for (int y = 0; y < HUD_H; y++) {
        for (int x = 0; x < HUD_W; x++) {
            const uint16_t p = fb[(size_t)y * HUD_W + x];
            if (p != 0xffff) continue;
            const int dx = x - 232, dy = y - 232;
            if (dx * dx + dy * dy > 100 && y <= 233) down_bad++;
        }
    }
    CHECK(down_bad == 0, "mm=30: %zu minute px not below centre", down_bad);
    free(fb);
}

/* Everything inside r<=192 (measured from the overlay centre 232,232 — the
 * seconds tip at r190 plus dot spill, riding the free r185-194 band, clear of
 * the baked ticks at r200), and the same 12-row strip contract as every other
 * overlay. Strip invariance is checked at a MID fade strength on purpose:
 * transition frames must be as seam-free as settled ones. */
static void test_clock_bounds_and_strips(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *whole = malloc(px * sizeof *whole);
    uint16_t *strips = malloc(px * sizeof *strips);
    if (!whole || !strips) {
        printf("FAIL %s: alloc\n", __func__); g_failures++;
        free(whole); free(strips); return;
    }

    static const int times[][3] = {
        {3, 0, 15}, {10, 47, 33}, {6, 15, 0}, {23, 59, 59},
    };
    for (size_t k = 0; k < sizeof times / sizeof times[0]; k++) {
        memset(whole, 0, px * sizeof *whole);
        hud_overlay_clock(whole, 0, HUD_H, false,
                          times[k][0], times[k][1], times[k][2], 255);
        size_t out = 0, painted = 0;
        for (int y = 0; y < HUD_H; y++) {
            for (int x = 0; x < HUD_W; x++) {
                if (whole[(size_t)y * HUD_W + x] == 0) continue;
                painted++;
                const int dx = x - 232, dy = y - 232;
                if (dx * dx + dy * dy > 192 * 192) out++;
            }
        }
        CHECK(out == 0, "%02d:%02d:%02d painted %zu px past r192",
              times[k][0], times[k][1], times[k][2], out);
        CHECK(painted > 200, "%02d:%02d:%02d painted only %zu px",
              times[k][0], times[k][1], times[k][2], painted);
    }

    /* strip invariance at a representative time, mid-fade */
    for (size_t i = 0; i < px; i++) { whole[i] = strips[i] = (uint16_t)(i * 17u); }
    hud_overlay_clock(whole, 0, HUD_H, false, 10, 47, 33, 140);
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        int nrows = (y + STRIP_ROWS <= HUD_H) ? STRIP_ROWS : (HUD_H - y);
        hud_overlay_clock(strips + (size_t)y * HUD_W, y, nrows, false,
                          10, 47, 33, 140);
    }
    size_t diffs = 0;
    for (size_t i = 0; i < px; i++) if (whole[i] != strips[i]) diffs++;
    CHECK(diffs == 0, "clock strip/whole mismatch at %zu px", diffs);
    free(whole); free(strips);
}

/* The presenter's fade-out runs THROUGH hud_overlay_clock with a decaying
 * strength, so off == paints-nothing now lives at the strength gate: 0 (and
 * anything below) must paint nothing. A mid strength paints, and paints no
 * pure-white pixel — the minute hand scales down with everything else, hands
 * fading as one object. */
static void test_clock_strength_gate(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }

    hud_overlay_clock(fb, 0, HUD_H, false, 10, 47, 33, 0);
    hud_overlay_clock(fb, 0, HUD_H, false, 10, 47, 33, -7);
    size_t painted = 0;
    for (size_t i = 0; i < px; i++) if (fb[i] != 0) painted++;
    CHECK(painted == 0, "strength<=0 painted %zu px", painted);

    hud_overlay_clock(fb, 0, HUD_H, false, 10, 47, 33, 128);
    size_t mid = 0, whites = 0;
    for (size_t i = 0; i < px; i++) {
        if (fb[i] == 0) continue;
        mid++;
        if (fb[i] == 0xffff) whites++;
    }
    CHECK(mid > 200, "strength=128 painted only %zu px", mid);
    CHECK(whites == 0, "strength=128 left %zu full-white px", whites);
    free(fb);
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
    test_listen_ring_band_and_exclusivity();
    test_tilt_offset_is_disabled();
    test_choices_strip_invariance();
    test_choices_respect_bounds();
    test_choices_stay_in_free_band();
    test_battery_and_choices_do_not_overlap();
    test_choice_layout_spans();
    test_choice_hit();
    test_choice_hit_rejects_face_and_bezel();
    test_choice_render_and_hit_agree();
    test_wrap2_short_fits_one_line();
    test_wrap2_splits_at_space();
    test_wrap2_hard_splits_long_word();
    test_wrap2_truncates_overflow();
    test_wrap2_preserves_48_char_sentence();
    test_wrap2_terminates_and_empty();
    test_label_anchor_on_screen();
    test_label_anchor_wrap_span();
    test_label_anchor_radial_clamp_19_chars();
    test_dim565_matches_darken_chain();
    test_fade565_endpoints_and_monotonic();
    test_mix565_endpoints();
    test_caption_text_fits_glass_chord();
    test_ripple_geometry();
    test_ripple_strip_invariance();
    test_ripple_corner_tap_stays_on_glass();
    test_bloom_expands_expires_and_bounds();
    test_bloom_strip_invariance();
    test_clock_hand_angles();
    test_clock_bounds_and_strips();
    test_clock_strength_gate();

    if (g_failures) {
        printf("hud_render tests FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("hud_render tests passed\n");
    return 0;
}
