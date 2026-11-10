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
#include "jr_display/tilt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;   /* empty is not pass: the summary names the count */

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
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

/* The listening halo: LISTEN paints sparse points strictly inside the free
 * r185-194 band, and IDLE paints nothing with the battery absent. The halo's
 * presence carries exactly one meaning: listening; louder input brightens it.
 * Whole-frame and strip rendering must remain identical. */
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
    CHECK(painted > 200, "listen halo painted only %zu px", painted);
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

/* Privacy is persistent hardware state, not a transient caption. It owns the
 * two-pixel gap between the battery and choice bands. */
static void test_privacy_ring_is_persistent_and_bounded(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *clear = calloc(px, sizeof *clear);
    uint16_t *muted = calloc(px, sizeof *muted);
    if (!clear || !muted) {
        printf("FAIL %s: alloc\n", __func__);
        g_failures++;
        free(clear); free(muted);
        return;
    }
    hud_env_t env = { .face = HUD_FACE_IDLE, .amp = 0, .batt_pct = 0xFF,
                      .charging = false, .privacy_muted = false,
                      .ox = 0, .oy = 0 };
    hud_overlay_frame(clear, 0, HUD_H, 1000, false, &env);
    env.privacy_muted = true;
    hud_overlay_frame(muted, 0, HUD_H, 1000, false, &env);

    size_t painted = 0, out_of_band = 0;
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            if (clear[(size_t)y * HUD_W + x] ==
                muted[(size_t)y * HUD_W + x]) {
                continue;
            }
            painted++;
            const long dx = x - 232, dy = y - 232;
            const long r2 = dx * dx + dy * dy;
            if (r2 < 220L * 220L || r2 > 223L * 223L) {
                out_of_band++;
            }
        }
    }
    CHECK(painted > 500, "privacy ring painted only %zu px", painted);
    CHECK(out_of_band == 0, "privacy ring left r220-223 (%zu px)",
          out_of_band);

    memset(clear, 0, px * sizeof *clear);
    memset(muted, 0, px * sizeof *muted);
    env.batt_pct = 100;
    env.privacy_muted = false;
    hud_overlay_frame(clear, 0, HUD_H, 1000, false, &env);
    env.privacy_muted = true;
    hud_overlay_frame(muted, 0, HUD_H, 1000, false, &env);
    size_t battery_overwritten = 0;
    for (size_t i = 0; i < px; ++i) {
        if (clear[i] != 0 && clear[i] != muted[i]) {
            battery_overwritten++;
        }
    }
    CHECK(battery_overwritten == 0,
          "privacy ring overwrote %zu battery pixels", battery_overwritten);
    free(clear); free(muted);
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

static void test_charging_rim_animates_inside_battery_band(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *a = calloc(px, sizeof *a);
    uint16_t *b = calloc(px, sizeof *b);
    CHECK(a != NULL && b != NULL, "alloc");
    if (a == NULL || b == NULL) {
        free(a); free(b);
        return;
    }
    const hud_env_t env = {
        .face = HUD_FACE_IDLE, .amp = 0, .batt_pct = 74,
        .charging = true, .privacy_muted = false, .ox = 0, .oy = 0
    };
    hud_overlay_frame(a, 0, HUD_H, 100U, false, &env);
    hud_overlay_frame(b, 0, HUD_H, 900U, false, &env);
    size_t changed = 0, escaped = 0;
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const size_t i = (size_t)y * HUD_W + x;
            if (a[i] == b[i]) continue;
            changed++;
            const int dx = x - 232, dy = y - 232;
            const int r2 = dx * dx + dy * dy;
            if (r2 < 215 * 215 || r2 > 221 * 221) escaped++;
        }
    }
    CHECK(changed > 0, "charging rim did not animate");
    CHECK(escaped == 0, "charging animation escaped battery band: %zu",
          escaped);
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

    hud_overlay_choices(whole, 0, HUD_H, false, c, HUD_CHOICE_MAX, 1, 255);

    int nstrips = 0, ragged = 0;
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        int nrows = (y + STRIP_ROWS <= HUD_H) ? STRIP_ROWS : (HUD_H - y);
        if (nrows != STRIP_ROWS) {
            ragged = nrows;
        }
        hud_overlay_choices(strips + (size_t)y * HUD_W, y, nrows, false,
                            c, HUD_CHOICE_MAX, 1, 255);
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

/* The ask's exit fade runs THROUGH hud_overlay_choices with a decaying
 * strength: 0 (and below) must paint nothing, and a mid strength must paint
 * dimmer than settled — never brighter — for both the selected and the
 * unselected arcs. */
static void test_choices_strength_gate(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }

    hud_choice_t c[HUD_CHOICE_MAX];
    t_fill_choices(c, HUD_CHOICE_MAX);

    hud_overlay_choices(fb, 0, HUD_H, false, c, HUD_CHOICE_MAX, 1, 0);
    hud_overlay_choices(fb, 0, HUD_H, false, c, HUD_CHOICE_MAX, 1, -9);
    size_t painted = 0;
    for (size_t i = 0; i < px; i++) if (fb[i] != 0) painted++;
    CHECK(painted == 0, "strength<=0 painted %zu px", painted);

    hud_overlay_choices(fb, 0, HUD_H, false, c, HUD_CHOICE_MAX, 1, 128);
    uint16_t *full = calloc(px, sizeof *full);
    if (!full) { printf("FAIL %s: alloc\n", __func__); g_failures++; free(fb); return; }
    hud_overlay_choices(full, 0, HUD_H, false, c, HUD_CHOICE_MAX, 1, 255);
    size_t mid = 0, brighter = 0;
    for (size_t i = 0; i < px; i++) {
        if (fb[i] == 0) continue;
        mid++;
        /* per-channel: the faded pixel may never exceed the settled one */
        const int rm = (fb[i] >> 11) & 31, rf = (full[i] >> 11) & 31;
        const int gm = (fb[i] >> 5) & 63,  gf = (full[i] >> 5) & 63;
        const int bm = fb[i] & 31,         bf = full[i] & 31;
        if (rm > rf || gm > gf || bm > bf) brighter++;
    }
    CHECK(mid > 5000, "strength=128 painted only %zu px", mid);
    CHECK(brighter == 0, "%zu px brighter at strength 128 than settled",
          brighter);
    free(fb); free(full);
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
    hud_overlay_choices(buf + GUARD, 197, NROWS, true, c, HUD_CHOICE_MAX, 0, 255);

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
    hud_overlay_choices(fb, 0, HUD_H, false, c, HUD_CHOICE_MAX, 2, 255);

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
    hud_overlay_choices(arc, 0, HUD_H, false, c, HUD_CHOICE_MAX, 0, 255);

    static const int pcts[] = { 5, 50, 100 };
    for (size_t k = 0; k < sizeof pcts / sizeof pcts[0]; k++) {
        for (int err = 0; err < 2; err++) {
            memset(rim, 0, px * sizeof *rim);
            hud_env_t env = { .face = err ? HUD_FACE_ERROR : HUD_FACE_IDLE,
                              .amp = 0, .batt_pct = (uint8_t)pcts[k],
                              .charging = false, .privacy_muted = true,
                              .ox = 0, .oy = 0 };
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
        hud_overlay_choices(fb, 0, HUD_H, false, solo, HUD_CHOICE_MAX, -1, 255);

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
    hud_overlay_ripple(fb, 0, HUD_H, false, 233, 233, HUD_RIPPLE_MS, HUD_RIPPLE_ACCEPT);
    hud_overlay_ripple(fb, 0, HUD_H, false, 233, 233, 5000u, HUD_RIPPLE_ACCEPT);
    size_t stale = 0;
    for (size_t i = 0; i < px; i++) if (fb[i]) stale++;
    CHECK(stale == 0, "expired ripple painted %zu px", stale);

    /* mid-animation, tap well on-glass */
    const uint32_t age = 200;
    const int cx = 300, cy = 180;
    const int rad = HUD_RIPPLE_R0 +
        (int)(((HUD_RIPPLE_R1 - HUD_RIPPLE_R0) * age) / HUD_RIPPLE_MS);
    hud_overlay_ripple(fb, 0, HUD_H, false, cx, cy, age, HUD_RIPPLE_ACCEPT);

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

    hud_overlay_ripple(whole, 0, HUD_H, false, 150, 340, 137u, HUD_RIPPLE_ACCEPT);
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        int nrows = (y + STRIP_ROWS <= HUD_H) ? STRIP_ROWS : (HUD_H - y);
        hud_overlay_ripple(strips + (size_t)y * HUD_W, y, nrows, false,
                           150, 340, 137u, HUD_RIPPLE_ACCEPT);
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
        hud_overlay_ripple(fb, 0, HUD_H, false, 40, 40, ages[k], HUD_RIPPLE_ACCEPT);
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

/* The rim's alarm rule is shared with the POWER focal arc via
 * HUD_BATT_LOW_PCT: red below it, never while charging. */
static void test_battery_rim_alarm_rule(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *a = calloc(px, sizeof *a);
    uint16_t *b = calloc(px, sizeof *b);
    if (!a || !b) { printf("FAIL %s: alloc\n", __func__); g_failures++; free(a); free(b); return; }

    hud_env_t env = { .face = HUD_FACE_IDLE, .amp = 0, .batt_pct = 8,
                      .charging = false, .ox = 0, .oy = 0 };
    hud_overlay_frame(a, 0, HUD_H, 500, false, &env);
    env.charging = true;
    hud_overlay_frame(b, 0, HUD_H, 500, false, &env);

    size_t red_a = 0, red_b = 0;
    for (int y = 0; y < HUD_H; y++) {
        for (int x = 0; x < HUD_W; x++) {
            int dx = x - 232, dy = y - 232;
            int r2 = dx * dx + dy * dy;
            if (r2 < 215 * 215 || r2 > 221 * 221) continue;
            uint16_t pa = a[y * HUD_W + x], pb = b[y * HUD_W + x];
            /* The rim is shade(ramp, 200): red 24/31, green 12/63 — a red
             * dominant channel with the green well under half of it. */
            if (((pa >> 11) & 0x1F) >= 16 && ((pa >> 5) & 0x3F) < 20) red_a++;
            if (((pb >> 11) & 0x1F) >= 16 && ((pb >> 5) & 0x3F) < 20) red_b++;
        }
    }
    CHECK(red_a > 0, "8%% on battery drew no red rim");
    CHECK(red_b == 0, "8%% on the charger drew %zu red rim px", red_b);
    free(a); free(b);
}

/* FOUR WATCHES, ONE CONTRACT (2026-09-02). Each style must paint a region
 * only it paints, and JARVIS must be the watch that shipped: four FNV-1a
 * checksums of the whole frame, taken from the tree before the styles
 * existed (scratch harness, 2026-09-02), so a stray pixel in the original
 * fails here. Colours: lume cream is the diver's alone; the digits are cyan
 * in a box no hand of the JARVIS watch reaches (x < 120 at 20:34); the
 * minimal watch has no gold and nothing beyond r=178. */
static uint32_t fnv_frame(const uint16_t *fb, size_t px)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < px; ++i) {
        h ^= fb[i];
        h *= 16777619u;
    }
    return h;
}

static void test_watch_jarvis_is_pixel_identical(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }
    static const struct { int hh, mm, ss; uint32_t fnv; size_t lit; } pins[] = {
        { 10,  8, 30, 0x708eb205u, 1176 },
        {  3,  0, 30, 0x74835b5eu, 1005 },
        {  0, 30,  0, 0x5f6c77f1u, 1040 },
        { 23, 59, 59, 0x1a665dccu,  875 },
    };
    for (size_t i = 0; i < sizeof pins / sizeof *pins; ++i) {
        for (int via_style = 0; via_style < 2; ++via_style) {
            memset(fb, 0, px * sizeof *fb);
            if (via_style) {
                hud_overlay_clock_style(fb, 0, HUD_H, false, pins[i].hh,
                                        pins[i].mm, pins[i].ss, 255, 0);
            } else {
                hud_overlay_clock(fb, 0, HUD_H, false, pins[i].hh,
                                  pins[i].mm, pins[i].ss, 255);
            }
            size_t lit = 0;
            for (size_t k = 0; k < px; ++k) {
                if (fb[k]) lit++;
            }
            const uint32_t h = fnv_frame(fb, px);
            CHECK(lit == pins[i].lit, "%02d:%02d:%02d via_style=%d lit %zu != %zu",
                  pins[i].hh, pins[i].mm, pins[i].ss, via_style, lit, pins[i].lit);
            CHECK(h == pins[i].fnv, "%02d:%02d:%02d via_style=%d fnv 0x%08x != 0x%08x",
                  pins[i].hh, pins[i].mm, pins[i].ss, via_style, h, pins[i].fnv);
        }
    }
    free(fb);
}

static void render_style_strips(uint16_t *fb, int hh, int mm, int ss, int style)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    memset(fb, 0, px * sizeof *fb);
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int n = y + STRIP_ROWS > HUD_H ? HUD_H - y : STRIP_ROWS;
        hud_overlay_clock_style(fb + (size_t)y * HUD_W, y, n, false,
                                hh, mm, ss, 255, style);
    }
}

static void test_watch_styles_paint_their_own_regions(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    uint16_t *whole = calloc(px, sizeof *whole);
    if (!fb || !whole) { printf("FAIL %s: alloc\n", __func__); g_failures++; free(fb); free(whole); return; }
    const uint16_t cream = (uint16_t)(((232 & 0xF8) << 8) | ((226 & 0xFC) << 3) | (200 >> 3));
    const uint16_t ivory = (uint16_t)(((245 & 0xF8) << 8) | ((242 & 0xFC) << 3) | (232 >> 3));
    const uint16_t cyan  = (uint16_t)(((0 & 0xF8) << 8) | ((229 & 0xFC) << 3) | (255 >> 3));
    const uint16_t gold  = (uint16_t)(((214 & 0xF8) << 8) | ((178 & 0xFC) << 3) | (62 >> 3));

    for (int style = 1; style <= 7; ++style) {
        render_style_strips(fb, 6, 30, 15, style);   /* hands away from 12 */
        size_t tri = 0, cream_any = 0, lit = 0, iv = 0, cy = 0, au = 0, far = 0;
        for (int y = 0; y < HUD_H; ++y) {
            for (int x = 0; x < HUD_W; ++x) {
                const uint16_t p = fb[(size_t)y * HUD_W + x];
                if (p == 0) continue;
                lit++;
                const int dx = x - 232, dy = y - 232;
                const int r2 = dx * dx + dy * dy;
                if (r2 > 216 * 216) far++;
                if (p == cream) {
                    cream_any++;
                    if (x >= 205 && x <= 259 && y >= 52 && y <= 78) tri++;
                }
                if (p == ivory) iv++;
                if (p == cyan) cy++;
                if (p == gold) au++;
            }
        }
        CHECK(far == 0, "style %d paints %zu px beyond r216", style, far);
        CHECK(lit > 400, "style %d lit only %zu px", style, lit);
        switch (style) {
        case 1:
            CHECK(tri > 150, "DIVER: 12 o'clock triangle only %zu cream px", tri);
            CHECK(cream_any > 1200, "DIVER: only %zu cream px", cream_any);
            break;
        case 2:
            CHECK(au > 300, "DRESS: only %zu gold px (batons + dauphine)", au);
            CHECK(cream_any == 0, "DRESS painted %zu cream px", cream_any);
            break;
        case 3:
            CHECK(cream_any > 200, "PILOT: only %zu lume px", cream_any);
            break;
        case 4:
            CHECK(iv > 100000, "MINIMAL: only %zu ivory px (the dial is drawn)", iv);
            CHECK(cream_any == 0, "MINIMAL painted %zu cream px", cream_any);
            break;
        case 6:
            CHECK(cream_any > 200, "SKELETON: only %zu lume px", cream_any);
            break;
        case 7:
            CHECK(lit > 1000, "CELESTIAL: lit only %zu px", lit);
            break;
        default:
            CHECK(cy > 300, "FUTURE: only %zu cyan px", cy);
            break;
        }
        /* strip rendering must equal the whole-frame render */
        memset(whole, 0, px * sizeof *whole);
        hud_overlay_clock_style(whole, 0, HUD_H, false, 6, 30, 15, 255, style);
        CHECK(memcmp(whole, fb, px * sizeof *fb) == 0,
              "style %d: strips differ from the whole frame", style);
    }
    /* The per-frame cache follows its inputs: a new time on the same style,
     * rendered in strips right after a whole frame at the old time, equals
     * a whole frame at the new time and differs from the old one. */
    for (int style = 1; style <= 7; ++style) {
        hud_overlay_clock_style(whole, 0, HUD_H, false, 6, 30, 15, 255, style);
        render_style_strips(fb, 10, 8, 45, style);
        CHECK(memcmp(whole, fb, px * sizeof *fb) != 0,
              "style %d: 10:08:45 rendered as 6:30:15 (stale frame cache)", style);
        memset(whole, 0, px * sizeof *whole);
        hud_overlay_clock_style(whole, 0, HUD_H, false, 10, 8, 45, 255, style);
        CHECK(memcmp(whole, fb, px * sizeof *fb) == 0,
              "style %d at 10:08:45: strips differ from the whole frame", style);
    }
    /* strength 0 paints nothing in every style */
    for (int style = 0; style < 8; ++style) {
        memset(fb, 0, px * sizeof *fb);
        hud_overlay_clock_style(fb, 0, HUD_H, false, 6, 30, 15, 0, style);
        size_t any = 0;
        for (size_t k = 0; k < px; ++k) if (fb[k]) any++;
        CHECK(any == 0, "style %d paints %zu px at strength 0", style, any);
    }
    free(fb); free(whole);
}

/* The sixty ticks of the two procedural rims, pinned. The strip-vs-whole
 * memcmp cannot tell a tick skipped rightly from one skipped wrongly (both
 * paths skip it), so the band r >= 196 — MINIMAL's ticks at 196-208 and
 * FUTURE's at 198-210, where no hand reaches — is checksummed against the
 * renderer as it was before tick_in_strip (2026-09-05). */
static void test_watch_rim_ticks_are_pinned(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }
    static const struct { int style; uint32_t fnv; size_t lit; } pins[] = {
        { 4, 0x5d8ecf5eu, 23932 },
        { 5, 0xb7417481u,  1259 },
    };
    for (size_t i = 0; i < sizeof pins / sizeof *pins; ++i) {
        render_style_strips(fb, 6, 30, 15, pins[i].style);
        uint32_t h = 2166136261u;
        size_t lit = 0;
        for (int y = 0; y < HUD_H; ++y) {
            for (int x = 0; x < HUD_W; ++x) {
                const int dx = x - 232, dy = y - 232;
                if (dx * dx + dy * dy < 196 * 196) continue;
                const uint16_t p = fb[(size_t)y * HUD_W + x];
                h ^= p; h *= 16777619u;
                if (p) lit++;
            }
        }
        CHECK(h == pins[i].fnv && lit == pins[i].lit,
              "style %d rim: fnv 0x%08x lit %zu, pinned 0x%08x / %zu",
              pins[i].style, h, lit, pins[i].fnv, pins[i].lit);
    }
    free(fb);
}

/* The outline is a rim. An outlined hand (DIVER, PILOT) on a
 * mid-grey dial (66 per channel) leaves a band of pixels darker than the
 * dial along both flanks and the caps — the 0.875 px rim of W_DARK at 86 %
 * covers at least half of one pixel per row and side, and that pixel sums
 * under 150 across its channels. A hand whose flanks stay bright (steel
 * or white seconds, cyan FUTURE) darkens nothing. Two angles, so both a
 * steep and a shallow hand carry it. */
static void test_outlined_hands_keep_their_rim(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }
    static const struct { int style, hand; bool outlined; } kinds[] = {
        { 1, 0, true }, { 1, 1, true }, { 3, 0, true }, { 3, 1, true },
        { 1, 2, false }, { 3, 2, false }, { 5, 0, false }, { 5, 1, false },
    };
    static const int degs[] = { 33, 100 };
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; ++k) {
        for (size_t d = 0; d < sizeof degs / sizeof *degs; ++d) {
            const uint32_t a16 = (uint32_t)(((uint64_t)degs[d] * 65536u) / 360u) + (192u << 8);
            for (size_t i = 0; i < px; ++i) fb[i] = 0x4208;
            hud_watch_hand(fb, 0, HUD_H, false, kinds[k].style, kinds[k].hand, a16, false);
            size_t rim = 0, lit = 0;
            for (size_t i = 0; i < px; ++i) {
                const uint16_t p = fb[i];
                if (p == 0x4208) continue;
                lit++;
                const int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                const int sum = ((r << 3) | (r >> 2)) + ((g << 2) | (g >> 4)) + ((b << 3) | (b >> 2));
                if (sum < 150) rim++;
            }
            CHECK(lit > 100, "style %d hand %d at %d deg lit %zu px", kinds[k].style, kinds[k].hand, degs[d], lit);
            if (kinds[k].outlined) {
                CHECK(rim > 120, "style %d hand %d at %d deg: only %zu rim px of %zu",
                      kinds[k].style, kinds[k].hand, degs[d], rim, lit);
                CHECK(rim < lit / 2, "style %d hand %d at %d deg: the rim (%zu of %zu) swallowed the body",
                      kinds[k].style, kinds[k].hand, degs[d], rim, lit);
            } else {
                CHECK(rim == 0, "style %d hand %d at %d deg grew a rim of %zu px",
                      kinds[k].style, kinds[k].hand, degs[d], rim);
            }
        }
    }
    free(fb);
}

/* No combing, at any angle: every row a hand touches is ONE contiguous run
 * of pixels. Dot-stamped hands (the first cut) left one-pixel teeth along
 * both edges at most angles; a scanline polygon cannot. The sweep is the
 * hand alone, without its shadow (the shadow is a second object two pixels
 * away, legitimately separate at the tip). */
static void test_watch_hands_never_comb(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = calloc(px, sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }
    static const int kinds[][2] = { {1,0},{1,1},{1,2},{2,0},{2,1},{3,0},{3,1},{3,2},{4,0},{4,1},{5,0},{5,1},{5,2} };
    size_t angles_checked = 0, rows_checked = 0;
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; ++k) {
        for (int deg = 0; deg < 360; ++deg) {
            const uint32_t a16 = (uint32_t)(((uint64_t)deg * 65536u) / 360u) + (192u << 8);
            memset(fb, 0, px * sizeof *fb);
            hud_watch_hand(fb, 0, HUD_H, false, kinds[k][0], kinds[k][1], a16, false);
            size_t lit = 0;
            int bad_row = -1;
            for (int y = 0; y < HUD_H && bad_row < 0; ++y) {
                const uint16_t *row = fb + (size_t)y * HUD_W;
                int runs = 0; bool in = false;
                for (int x = 0; x < HUD_W; ++x) {
                    const bool on = row[x] != 0;
                    if (on) lit++;
                    if (on && !in) runs++;
                    in = on;
                }
                if (runs > 1) bad_row = y;
                if (runs == 1) rows_checked++;
            }
            CHECK(lit > 20, "style %d hand %d at %d deg lit only %zu px", kinds[k][0], kinds[k][1], deg, lit);
            CHECK(bad_row < 0, "style %d hand %d at %d deg combs on row %d", kinds[k][0], kinds[k][1], deg, bad_row);
            angles_checked++;
        }
    }
    CHECK(angles_checked == 13u * 360u, "swept %zu angle/hand pairs", angles_checked);
    CHECK(rows_checked > 100000, "only %zu hand rows checked", rows_checked);
    free(fb);
}

/* Over a baked dial the hands BLEND: pixels the hands do not touch keep the
 * art, nothing outside r214 changes, and the edges are mixed rather than
 * hard. The dial is a flat mid grey here; the art worker's is not, and the
 * property is the same. */
static void test_watch_hands_blend_and_spare_the_dial(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) { printf("FAIL %s: alloc\n", __func__); g_failures++; return; }
    const uint16_t grey = 0x4208;
    for (int style = 1; style <= 7; ++style) {
        if (style == 4) continue;                   /* MINIMAL draws its own dial */
        for (size_t k = 0; k < px; ++k) fb[k] = grey;
        const hud_watch_t w = { .hh = 10, .mm = 8, .ss = 30, .ms = 500,
                                .batt_pct = 83, .wx_valid = true, .temp_f = 77,
                                .dial_baked = true };
        for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
            const int n = y + STRIP_ROWS > HUD_H ? HUD_H - y : STRIP_ROWS;
            hud_overlay_watch(fb + (size_t)y * HUD_W, y, n, false, &w, 255, style);
        }
        size_t changed_in = 0, changed_out = 0, mixed = 0, kept = 0;
        for (int y = 0; y < HUD_H; ++y) {
            for (int x = 0; x < HUD_W; ++x) {
                const uint16_t p = fb[(size_t)y * HUD_W + x];
                const int dx = x - 232, dy = y - 232;
                const bool inside = dx * dx + dy * dy <= 214 * 214;
                if (p == grey) { if (inside) kept++; continue; }
                if (inside) changed_in++; else changed_out++;
                /* a mixed pixel is neither the grey nor a solid hand colour:
                 * grey's channels partly survive in it */
                const int r = (p >> 11) & 31, g = (p >> 5) & 63;
                if (r != 8 && g != 16 && r > 2 && g > 4 && r < 28 && g < 58) mixed++;
            }
        }
        CHECK(changed_out == 0, "style %d touched %zu px outside r214", style, changed_out);
        CHECK(changed_in > 300 && changed_in < 40000, "style %d changed %zu px inside", style, changed_in);
        CHECK(kept > 100000, "style %d kept only %zu dial px", style, kept);
        CHECK(mixed > 100, "style %d has only %zu anti-aliased edge px", style, mixed);
    }
    free(fb);
}

/* ===== THE LIGHT ON THE HANDS (2026-11-09) =================================
 * Geometry never moves; only the light does. The rest light must be exactly
 * the one the watch shipped with, and a tilt must swing it smoothly. */
static bool light_eq(const hud_light_t *a, const hud_light_t *b)
{
    return a->shadow_dx == b->shadow_dx && a->shadow_dy == b->shadow_dy &&
           a->bevel_lx == b->bevel_lx && a->bevel_ly == b->bevel_ly &&
           a->glint_x == b->glint_x && a->glint_y == b->glint_y;
}

static int light_abs(int v) { return v < 0 ? -v : v; }

static void test_light_deadband_is_today(void)
{
    hud_light_t rest, l;
    hud_light_solve(0, 0, &rest);
    CHECK(rest.shadow_dx == 32 && rest.shadow_dy == 48 &&
          rest.bevel_lx == -19661 && rest.bevel_ly == -26214 &&
          rest.glint_x == -16 && rest.glint_y == -16,
          "rest light is not today's constants: %d,%d %d,%d %d,%d",
          rest.shadow_dx, rest.shadow_dy, rest.bevel_lx, rest.bevel_ly,
          rest.glint_x, rest.glint_y);
    int inside = 0;
    for (int x = -HUD_LIGHT_DEADBAND_MG; x <= HUD_LIGHT_DEADBAND_MG; ++x) {
        for (int y = -HUD_LIGHT_DEADBAND_MG; y <= HUD_LIGHT_DEADBAND_MG; ++y) {
            if (x * x + y * y >= HUD_LIGHT_DEADBAND_MG * HUD_LIGHT_DEADBAND_MG) continue;
            hud_light_solve(x, y, &l);
            inside++;
            CHECK(light_eq(&l, &rest), "(%d,%d) inside the dead band is not rest", x, y);
        }
    }
    CHECK(inside > 20000, "only %d dead-band points checked", inside);
}

/* Continuity: nothing steps at the dead-band edge, and nothing jumps
 * anywhere on the way up. Shadow in Q4 px, bevel in Q15, glint in 1/16. */
static void test_light_is_continuous(void)
{
    hud_light_t rest, a, b;
    hud_light_solve(0, 0, &rest);
    for (int m = 79; m <= 81; ++m) {            /* the edge: still rest */
        hud_light_solve(m, 0, &a);
        CHECK(light_eq(&a, &rest), "edge mag %d moved off rest", m);
    }
    int worst_sh = 0, worst_bv = 0, worst_gl = 0, pairs = 0;
    for (int dir = 0; dir < 8; ++dir) {
        static const int ux[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
        static const int uy[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        int px = 0, py = 0;
        for (int m = 60; m <= 1200; ++m) {
            const int x = ux[dir] * m * (ux[dir] && uy[dir] ? 707 : 1000) / 1000;
            const int y = uy[dir] * m * (ux[dir] && uy[dir] ? 707 : 1000) / 1000;
            hud_light_solve(x, y, &b);
            if (m > 60) {
                hud_light_solve(px, py, &a);
                int d = light_abs(a.shadow_dx - b.shadow_dx);
                if (light_abs(a.shadow_dy - b.shadow_dy) > d) d = light_abs(a.shadow_dy - b.shadow_dy);
                if (d > worst_sh) worst_sh = d;
                d = light_abs(a.bevel_lx - b.bevel_lx);
                if (light_abs(a.bevel_ly - b.bevel_ly) > d) d = light_abs(a.bevel_ly - b.bevel_ly);
                if (d > worst_bv) worst_bv = d;
                d = light_abs(a.glint_x - b.glint_x);
                if (light_abs(a.glint_y - b.glint_y) > d) d = light_abs(a.glint_y - b.glint_y);
                if (d > worst_gl) worst_gl = d;
                pairs++;
            }
            px = x; py = y;
        }
    }
    CHECK(pairs > 9000, "only %d adjacent pairs", pairs);
    CHECK(worst_sh <= 2, "shadow jumped %d Q4 between 1 mg steps", worst_sh);
    CHECK(worst_gl <= 1, "glint jumped %d between 1 mg steps", worst_gl);
    CHECK(worst_bv <= 800, "bevel vector jumped %d (Q15) between 1 mg steps", worst_bv);
}

static void test_light_shadow_falls_downhill_and_clamps(void)
{
    hud_light_t l;
    /* UP vector toward 12 o'clock (y is down on the glass): the device
     * stands on edge, the shadow falls to 6 o'clock, 6 px long */
    hud_light_solve(0, -1000, &l);
    CHECK(l.shadow_dx == 0 && l.shadow_dy == 96, "12 up: shadow %d,%d", l.shadow_dx, l.shadow_dy);
    hud_light_solve(0, 1000, &l);
    CHECK(l.shadow_dx == 0 && l.shadow_dy == -96, "6 up: shadow %d,%d", l.shadow_dx, l.shadow_dy);
    hud_light_solve(1000, 0, &l);
    CHECK(l.shadow_dx == -96 && l.shadow_dy == 0, "3 up: shadow %d,%d", l.shadow_dx, l.shadow_dy);
    hud_light_solve(-1000, 0, &l);
    CHECK(l.shadow_dx == 96 && l.shadow_dy == 0, "9 up: shadow %d,%d", l.shadow_dx, l.shadow_dy);

    /* length grows with tilt once past the ramp, and never beyond 6 px */
    static const int ux[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int uy[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    for (int dir = 0; dir < 8; ++dir) {
        long prev = -1;
        for (int m = 400; m <= 1200; m += 8) {
            const int s = (ux[dir] && uy[dir]) ? 707 : 1000;
            hud_light_solve(ux[dir] * m * s / 1000, uy[dir] * m * s / 1000, &l);
            const long len2 = (long)l.shadow_dx * l.shadow_dx + (long)l.shadow_dy * l.shadow_dy;
            CHECK(len2 <= 97L * 97L, "dir %d m %d shadow %ld > 6 px", dir, m, len2);
            if (m <= 1000 && dir % 2 == 0) {
                CHECK(len2 >= prev, "dir %d: shadow shrank at %d mg", dir, m);
                prev = len2;
            }
        }
    }
    /* the whole input square, corners included */
    for (int x = -1500; x <= 1500; x += 25) {
        for (int y = -1500; y <= 1500; y += 25) {
            hud_light_solve(x, y, &l);
            CHECK(light_abs(l.shadow_dx) <= 97 && light_abs(l.shadow_dy) <= 97,
                  "(%d,%d) shadow %d,%d beyond 6 px", x, y, l.shadow_dx, l.shadow_dy);
            CHECK(light_abs(l.glint_x) <= 24 && light_abs(l.glint_y) <= 24,
                  "(%d,%d) glint %d,%d leaves the hub", x, y, l.glint_x, l.glint_y);
        }
    }
}

static void test_light_bevel_stays_normalised_and_mirrors(void)
{
    hud_light_t l, m;
    int tilted = 0;
    for (int x = -1200; x <= 1200; x += 20) {
        for (int y = -1200; y <= 1200; y += 20) {
            hud_light_solve(x, y, &l);
            const long n2 = (long)l.bevel_lx * l.bevel_lx + (long)l.bevel_ly * l.bevel_ly;
            const long one = 32768L * 32768L;
            CHECK(n2 <= one + one / 50, "(%d,%d) bevel vector too long (%ld)", x, y, n2);
            if (x * x + y * y >= 400 * 400) {
                tilted++;
                CHECK(n2 >= one - one / 50, "(%d,%d) bevel vector not unit (%ld)", x, y, n2);
                hud_light_solve(-x, -y, &m);        /* point reflection */
                CHECK(m.shadow_dx == -l.shadow_dx && m.shadow_dy == -l.shadow_dy &&
                      m.bevel_lx == -l.bevel_lx && m.bevel_ly == -l.bevel_ly &&
                      m.glint_x == -l.glint_x && m.glint_y == -l.glint_y,
                      "(%d,%d) is not mirrored by (%d,%d)", x, y, -x, -y);
            }
        }
    }
    CHECK(tilted > 8000, "only %d tilted points checked", tilted);
    /* the light goes where the shadow does not: standing on edge, 12 up,
     * the bevel light comes from the top (negative y on the glass) */
    hud_light_solve(0, -1000, &l);
    CHECK(l.bevel_lx == 0 && l.bevel_ly == -32768, "12 up: bevel %d,%d", l.bevel_lx, l.bevel_ly);
    CHECK(l.glint_x == 0 && l.glint_y < -16, "12 up: glint %d,%d", l.glint_x, l.glint_y);
}

static void test_light_filter_converges_without_overshoot(void)
{
    static const int targets[] = { 1000, -1200, 80, 0, -1 };
    for (size_t t = 0; t < sizeof targets / sizeof *targets; ++t) {
        for (int dt = 10; dt <= 100; dt += 30) {
            for (int from = -1; from <= 1; from += 2) {
                int cur = from * 700 * 256;
                int prev_gap = light_abs(targets[t] * 256 - cur);
                int steps = 0;
                while (cur != targets[t] * 256 && steps < 4000) {
                    const int next = hud_light_ease(cur, targets[t], dt);
                    const int before = targets[t] * 256 - cur;
                    const int after = targets[t] * 256 - next;
                    CHECK(light_abs(after) <= prev_gap, "target %d dt %d: gap grew", targets[t], dt);
                    CHECK((before >= 0 && after >= 0) || (before <= 0 && after <= 0),
                          "target %d dt %d: overshot (%d -> %d)", targets[t], dt, cur, next);
                    prev_gap = light_abs(after);
                    cur = next;
                    steps++;
                }
                CHECK(cur == targets[t] * 256, "target %d dt %d did not arrive in %d steps",
                      targets[t], dt, steps);
            }
        }
    }
    /* about one time constant to cover 63 %, on the 42 ms awake cadence */
    int cur = 0;
    for (int i = 0; i < 6; ++i) cur = hud_light_ease(cur, 1000, 42);   /* 252 ms */
    CHECK(cur / 256 > 540 && cur / 256 < 720,
          "after ~250 ms the filter is at %d mg, expected ~630", cur / 256);
}

static void render_watch_light(uint16_t *fb, int style, const hud_light_t *lt, bool shadow)
{
    /* a mid grey dial: a shadow over black is invisible, over grey it is not */
    for (size_t k = 0; k < (size_t)HUD_W * HUD_H; ++k) fb[k] = 0x4208;
    const hud_watch_t w = { .hh = 10, .mm = 10, .ss = 30, .ms = 0,
                            .batt_pct = 83, .wx_valid = true, .temp_f = 77,
                            .dial_baked = false, .shadow = shadow, .light = lt };
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int n = y + STRIP_ROWS > HUD_H ? HUD_H - y : STRIP_ROWS;
        hud_overlay_watch(fb + (size_t)y * HUD_W, y, n, false, &w, 255, style);
    }
}

static void test_watch_light_in_the_frame(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *a = calloc(px, sizeof *a), *b = calloc(px, sizeof *b);
    if (!a || !b) { printf("FAIL %s: alloc\n", __func__); g_failures++; free(a); free(b); return; }
    hud_light_t rest, tilt;
    hud_light_solve(0, 0, &rest);
    hud_light_solve(707, -707, &tilt);            /* 45 degrees, 6 o'clock side */
    for (int style = 1; style <= 7; ++style) {
        render_watch_light(a, style, NULL, true);
        render_watch_light(b, style, &rest, true);
        CHECK(memcmp(a, b, px * sizeof *a) == 0, "style %d: rest light != NULL light", style);
        render_watch_light(b, style, &tilt, true);
        const bool differs = memcmp(a, b, px * sizeof *a) != 0;
        size_t mask_diff = 0;                    /* touched-vs-dial changed */
        size_t px_diff_on = 0;                   /* pixels that differ at all */
        for (size_t k = 0; k < px; ++k) {
            if ((a[k] != 0x4208) != (b[k] != 0x4208)) mask_diff++;
            if (a[k] != b[k]) px_diff_on++;
        }
        if (style == 5) {
            /* FUTURE casts no shadow, but its bevel tone follows the light:
             * the footprint is the same up to edge pixels whose tone rounds
             * onto the dial, only the flank tones move */
            CHECK(mask_diff <= 60, "FUTURE footprint moved by %zu px under tilt", mask_diff);
            printf("note: FUTURE frame %s under tilt (bevel tone only, footprint moved %zu px)\n",
                   differs ? "differs" : "is identical", mask_diff);
        } else {
            CHECK(differs, "style %d: a strong tilt left the frame unchanged", style);
        }
        /* with shadows off (the resting cadence) only tones follow the light */
        render_watch_light(a, style, NULL, false);
        render_watch_light(b, style, &tilt, false);
        mask_diff = 0;
        size_t px_diff_off = 0;
        for (size_t k = 0; k < px; ++k) {
            if ((a[k] != 0x4208) != (b[k] != 0x4208)) mask_diff++;
            if (a[k] != b[k]) px_diff_off++;
        }
        if (style != 5) {
            /* the shadow itself is what moved: far more pixels change with
             * shadows on than the tone shift alone changes with them off */
            CHECK(px_diff_on > px_diff_off + 150,
                  "style %d: shadow moved only %zu px beyond tone (%zu on, %zu off)",
                  style, px_diff_on - px_diff_off, px_diff_on, px_diff_off);
        }
        CHECK(mask_diff <= 100, "style %d: light moved the footprint with shadows off (%zu px)",
              style, mask_diff);
    }
    free(a); free(b);
}

/* ===== THE LIVE ORNAMENTS (2026-11-10): SKELETON balance wheel and
 * CELESTIAL stars. Their own block, at the end, so a merge with another
 * builder is a pure append. ========================================== */
#include <math.h>
#include "balance_wheel_data.h"
#include "celestial_stars_data.h"

#define LO_SENTINEL 0xA5A5u

static uint16_t *lo_frame(uint16_t fill)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) { printf("FAIL lo_frame: alloc\n"); g_failures++; return NULL; }
    for (size_t k = 0; k < px; ++k) fb[k] = fill;
    return fb;
}

static void lo_blit_strips(uint16_t *fb, bool swap, int32_t ang)
{
    for (int y = 0; y < HUD_H; y += 12) {
        const int n = y + 12 > HUD_H ? HUD_H - y : 12;
        hud_balance_blit(fb + (size_t)y * HUD_W, y, n, swap, ang);
    }
}

static int lo_ch_diff(uint16_t a, uint16_t b)
{
    int d = abs(((a >> 11) & 31) - ((b >> 11) & 31));
    int e = abs(((a >> 5) & 63) / 2 - ((b >> 5) & 63) / 2);
    int f = abs((a & 31) - (b & 31));
    return d > e ? (d > f ? d : f) : (e > f ? e : f);
}

/* The swing: bounded, periodic (1 s), zero at the top of the second, peak at
 * a quarter period, eased (the extremes are flat, the crossings are steep). */
static void test_balance_swing_is_bounded_periodic_and_eased(void)
{
    int lo = 0, hi = 0;
    for (int t = 0; t < 120000; t += 7) {
        const int ss = (t / 1000) % 60, ms = t % 1000;
        const int32_t a = hud_balance_angle_q16(ss, ms);
        if (a < lo) lo = a;
        if (a > hi) hi = a;
        CHECK(a >= -HUD_BALANCE_SWING_Q16 && a <= HUD_BALANCE_SWING_Q16,
              "swing %d out of +-%d at %d ms", (int)a, HUD_BALANCE_SWING_Q16, t);
        const int t2 = (t + 1000) % 60000;
        CHECK(hud_balance_angle_q16(t2 / 1000, t2 % 1000) == a,
              "swing is not periodic at %d ms", t);
    }
    CHECK(hi >= HUD_BALANCE_SWING_Q16 - 40 && lo <= -HUD_BALANCE_SWING_Q16 + 40,
          "swing never reaches its ends (%d .. %d)", lo, hi);
    CHECK(hud_balance_angle_q16(7, 0) == 0, "not at rest at the top of a second");
    const int top = hud_balance_angle_q16(7, 250), up = hud_balance_angle_q16(7, 240);
    const int zero = hud_balance_angle_q16(7, 10);
    CHECK(abs(top - up) * 10 < abs(zero), "not eased: %d per 10 ms at the end, %d at the crossing",
          abs(top - up), abs(zero));
    CHECK(hud_balance_angle_q16(7, 750) < 0 && hud_balance_angle_q16(7, 250) > 0,
          "the two half-swings do not go opposite ways");
    CHECK(hud_balance_angle_q16(-5, -9) == hud_balance_angle_q16(55, 0), "negative clock not folded");
    CHECK(hud_balance_angle_q16(7, 5000) == hud_balance_angle_q16(7, 999), "ms not clamped");
}

/* The rotation: the centre maps to the centre, and a rim point of the sprite
 * lands where an independent floating-point rotation says, at 0, +45 and -45
 * degrees. The whole footprint is also compared with a nearest-neighbour
 * reference built with libm. */
static void test_balance_rotation_maps_centre_and_rim(void)
{
    const int cx = JR_BALANCE_DIAL_CX, cy = JR_BALANCE_DIAL_CY;
    const int32_t angs[3] = { 0, HUD_BALANCE_SWING_Q16, -HUD_BALANCE_SWING_Q16 };
    uint16_t centre[3] = { 0 };
    /* the rim point: the opaque sprite pixel farthest from the sprite centre */
    int bx = 0, by = 0, bd = -1;
    for (int y = 0; y < JR_BALANCE_SIZE; ++y)
        for (int x = 0; x < JR_BALANCE_SIZE; ++x)
            if (jr_balance_alpha[y * JR_BALANCE_SIZE + x] == 255) {
                const int d = (x - JR_BALANCE_C) * (x - JR_BALANCE_C) + (y - JR_BALANCE_C) * (y - JR_BALANCE_C);
                if (d > bd) { bd = d; bx = x; by = y; }
            }
    CHECK(bd > 60 * 60, "no opaque rim pixel found (r^2 %d)", bd);
    for (int i = 0; i < 3; ++i) {
        uint16_t *fb = lo_frame(LO_SENTINEL);
        if (!fb) return;
        lo_blit_strips(fb, false, angs[i]);
        centre[i] = fb[(size_t)cy * HUD_W + cx];
        CHECK(centre[i] != LO_SENTINEL, "angle %d: the centre was not drawn", (int)angs[i]);
        /* the rim point */
        const double th = (double)angs[i] * 2.0 * M_PI / 65536.0;
        const double rx = bx - JR_BALANCE_C, ry = by - JR_BALANCE_C;
        const double ex = cx + rx * cos(th) - ry * sin(th);
        const double ey = cy + rx * sin(th) + ry * cos(th);
        bool hit = false;
        for (int dy = -1; dy <= 1 && !hit; ++dy)
            for (int dx = -1; dx <= 1 && !hit; ++dx) {
                const int x = (int)floor(ex + 0.5) + dx, y = (int)floor(ey + 0.5) + dy;
                if (fb[(size_t)y * HUD_W + x] != LO_SENTINEL &&
                    (x - ex) * (x - ex) + (y - ey) * (y - ey) < 1.6)
                    hit = true;
            }
        CHECK(hit, "angle %d: the rim point is not at (%.1f, %.1f)", (int)angs[i], ex, ey);
        /* the footprint against a nearest-neighbour reference */
        size_t agree = 0, total = 0;
        const double c = cos(-th), s = sin(-th);
        for (int y = cy - JR_BALANCE_R - 2; y <= cy + JR_BALANCE_R + 2; ++y)
            for (int x = cx - JR_BALANCE_R - 2; x <= cx + JR_BALANCE_R + 2; ++x) {
                const double dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > (double)JR_BALANCE_R * JR_BALANCE_R) continue;
                /* drawn when the bilinear alpha at the source point is not
                 * negligible (the blit skips under 4 of 255) */
                const double u = JR_BALANCE_C + dx * c - dy * s, v = JR_BALANCE_C + dx * s + dy * c;
                const int sx = (int)floor(u), sy = (int)floor(v);
                const double fx = u - sx, fy = v - sy;
                const double al = (1 - fx) * (1 - fy) * jr_balance_alpha[sy * JR_BALANCE_SIZE + sx] +
                                  fx * (1 - fy) * jr_balance_alpha[sy * JR_BALANCE_SIZE + sx + 1] +
                                  (1 - fx) * fy * jr_balance_alpha[(sy + 1) * JR_BALANCE_SIZE + sx] +
                                  fx * fy * jr_balance_alpha[(sy + 1) * JR_BALANCE_SIZE + sx + 1];
                if (al > 1.0 && al < 12.0) continue;      /* the threshold band */
                const bool want = al >= 12.0;
                const bool got = fb[(size_t)y * HUD_W + x] != LO_SENTINEL;
                total++;
                if (want == got) agree++;
            }
        CHECK(agree * 1000 >= total * 990, "angle %d: footprint agrees on %zu of %zu px",
              (int)angs[i], agree, total);
        free(fb);
    }
    CHECK(lo_ch_diff(centre[0], centre[1]) <= 3 && lo_ch_diff(centre[0], centre[2]) <= 3,
          "the jewel moves: %04x %04x %04x", centre[0], centre[1], centre[2]);
}

/* Nothing outside the bounding disc, any angle, either byte order; strips of
 * 12 rows equal the whole frame; the byte-swapped render is the byte swap of
 * the plain one; the pixel cost is reported. */
static void test_balance_stays_in_its_disc_and_strips_agree(void)
{
    const int cx = JR_BALANCE_DIAL_CX, cy = JR_BALANCE_DIAL_CY;
    const size_t px = (size_t)HUD_W * HUD_H;
    size_t worst = 0;
    for (int a = -HUD_BALANCE_SWING_Q16; a <= HUD_BALANCE_SWING_Q16; a += 1531) {
        uint16_t *whole = lo_frame(LO_SENTINEL), *strips = lo_frame(LO_SENTINEL);
        uint16_t *sw = lo_frame(0xA5A5u);
        if (!whole || !strips || !sw) return;
        const int visited = hud_balance_blit(whole, 0, HUD_H, false, a);
        lo_blit_strips(strips, false, a);
        CHECK(memcmp(whole, strips, px * sizeof *whole) == 0, "angle %d: strips differ from the whole frame", a);
        lo_blit_strips(sw, true, a);
        size_t changed = 0, outside = 0, swap_bad = 0;
        for (int y = 0; y < HUD_H; ++y)
            for (int x = 0; x < HUD_W; ++x) {
                const uint16_t p = whole[(size_t)y * HUD_W + x];
                if (p != LO_SENTINEL) {
                    changed++;
                    const int dx = x - cx, dy = y - cy;
                    if (dx * dx + dy * dy > (JR_BALANCE_R + 1) * (JR_BALANCE_R + 1)) outside++;
                }
                const uint16_t q = sw[(size_t)y * HUD_W + x];
                if ((uint16_t)((q >> 8) | (q << 8)) != p) swap_bad++;
            }
        CHECK(outside == 0, "angle %d: %zu px outside the wheel disc", a, outside);
        CHECK(swap_bad == 0, "angle %d: %zu px differ between byte orders", a, swap_bad);
        CHECK(changed > 6000 && changed < 15500, "angle %d: wheel drew %zu px", a, changed);
        CHECK(visited > 14500 && visited < 16500, "angle %d: visited %d px", a, visited);
        if ((size_t)visited > worst) worst = (size_t)visited;
        free(whole); free(strips); free(sw);
    }
    /* a strip that does not meet the disc paints and visits nothing */
    uint16_t row[HUD_W * 12];
    for (size_t k = 0; k < sizeof row / sizeof *row; ++k) row[k] = LO_SENTINEL;
    CHECK(hud_balance_blit(row, 0, 12, false, 0) == 0, "a strip above the disc visited pixels");
    CHECK(hud_balance_blit(NULL, 0, 12, false, 0) == 0 && hud_balance_blit(row, 0, 0, false, 0) == 0,
          "NULL / empty strip");
    printf("note: balance wheel visits %zu px per frame at most (box %d x %d)\n",
           worst, 2 * (JR_BALANCE_R + 1) + 1, 2 * (JR_BALANCE_R + 1) + 1);
}

/* In the watch: SKELETON on its baked dial swings only when the watch is
 * alive; at the top of a second (angle 0) alive or not renders identically,
 * a quarter second later they differ in the wheel; strips equal the whole
 * frame; the stand-in dial draws no wheel. */
static void test_balance_in_the_watch_follows_alive_and_strips(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *a = lo_frame(0x4208), *b = lo_frame(0x4208), *c = lo_frame(0x4208);
    if (!a || !b || !c) return;
    hud_watch_t w = { .hh = 3, .mm = 0, .ss = 0, .ms = 0, .batt_pct = -1, .dial_baked = true,
                      .shadow = true, .alive = true };
    hud_overlay_watch(a, 0, HUD_H, false, &w, 255, 6);
    w.alive = false;
    hud_overlay_watch(b, 0, HUD_H, false, &w, 255, 6);
    CHECK(memcmp(a, b, px * sizeof *a) == 0, "alive changes the wheel at angle 0");
    for (size_t k = 0; k < px; ++k) { a[k] = b[k] = 0x4208; }
    w.ms = 250; w.alive = true;
    hud_overlay_watch(a, 0, HUD_H, false, &w, 255, 6);
    w.alive = false;
    hud_overlay_watch(b, 0, HUD_H, false, &w, 255, 6);
    size_t diff = 0;
    for (size_t k = 0; k < px; ++k) if (a[k] != b[k]) diff++;
    CHECK(diff > 1500, "a swinging wheel differs from a resting one in only %zu px", diff);
    /* strips equal the whole frame while swinging */
    for (size_t k = 0; k < px; ++k) { a[k] = c[k] = 0x4208; }
    w.alive = true;
    hud_overlay_watch(a, 0, HUD_H, false, &w, 255, 6);
    for (int y = 0; y < HUD_H; y += 12) {
        const int n = y + 12 > HUD_H ? HUD_H - y : 12;
        hud_overlay_watch(c + (size_t)y * HUD_W, y, n, false, &w, 255, 6);
    }
    CHECK(memcmp(a, c, px * sizeof *a) == 0, "SKELETON strips differ from the whole frame while swinging");
    /* no baked dial, no recess, no wheel: the stand-in is the same alive or not */
    w.dial_baked = false;
    for (size_t k = 0; k < px; ++k) { a[k] = b[k] = 0x4208; }
    hud_overlay_watch(a, 0, HUD_H, false, &w, 255, 6);
    w.alive = false;
    hud_overlay_watch(b, 0, HUD_H, false, &w, 255, 6);
    CHECK(memcmp(a, b, px * sizeof *a) == 0, "the stand-in dial draws a wheel");
    free(a); free(b); free(c);
}

/* The star table: a few dozen, on the dial, away from the hub and the track,
 * apart from each other, on periods of 2-5 s that divide a minute. */
static void test_celestial_star_table_is_sane(void)
{
    CHECK(HUD_STAR_COUNT >= 24 && HUD_STAR_COUNT <= 40, "%d stars, want 24-40", HUD_STAR_COUNT);
    for (int i = 0; i < HUD_STAR_COUNT; ++i) {
        const hud_star_t *s = &s_stars[i];
        const int dx = s->x - 233, dy = s->y - 233, r2 = dx * dx + dy * dy;
        CHECK(r2 >= 30 * 30 && r2 <= 186 * 186, "star %d at r^2 %d is in the hub or the track", i, r2);
        CHECK(s->period_ms >= 2000 && s->period_ms <= 5000 && 60000 % s->period_ms == 0,
              "star %d period %u", i, (unsigned)s->period_ms);
        CHECK(s->strength >= 128, "star %d strength %u", i, (unsigned)s->strength);
        for (int j = i + 1; j < HUD_STAR_COUNT; ++j) {
            const int ex = s->x - s_stars[j].x, ey = s->y - s_stars[j].y;
            CHECK(ex * ex + ey * ey >= 16 * 16, "stars %d and %d are %d apart (squared)", i, j, ex * ex + ey * ey);
        }
    }
    /* each star is periodic in its own period, bounded, and they are not in step */
    int distinct = 0;
    int seen[HUD_STAR_COUNT];
    for (int i = 0; i < HUD_STAR_COUNT; ++i) {
        const int P = s_stars[i].period_ms;
        for (int t = 0; t < 60000; t += 211) {
            const int l = hud_celestial_star_level(i, (t / 1000) % 60, t % 1000);
            CHECK(l >= 0 && l <= 254, "star %d level %d", i, l);
            const int t2 = (t + P) % 60000;
            CHECK(hud_celestial_star_level(i, t2 / 1000, t2 % 1000) == l, "star %d not periodic in %d ms", i, P);
        }
        seen[i] = hud_celestial_star_level(i, 12, 0);
        bool dup = false;
        for (int j = 0; j < i; ++j) if (seen[j] == seen[i]) dup = true;
        if (!dup) distinct++;
    }
    CHECK(distinct >= HUD_STAR_COUNT * 2 / 3, "only %d distinct phases at one instant", distinct);
    CHECK(hud_celestial_star_level(-1, 0, 0) == 0 && hud_celestial_star_level(HUD_STAR_COUNT, 0, 0) == 0,
          "out-of-range star has a level");
}

/* The glint is subtle and local: additive and clamped, a few pixels per
 * star, nothing beyond r 190, strips equal the whole frame, the byte-swapped
 * render is the byte swap, and only an alive watch draws any. */
static void test_celestial_twinkle_is_additive_small_and_strip_invariant(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    const uint16_t navy = 0x0841u;
    size_t worst_touched = 0;
    int worst_step = 0;
    for (int t = 0; t < 12000; t += 437) {
        const int ss = t / 1000, ms = t % 1000;
        uint16_t *whole = lo_frame(navy), *strips = lo_frame(navy), *sw = lo_frame(navy);
        if (!whole || !strips || !sw) return;
        for (size_t k = 0; k < px; ++k) sw[k] = (uint16_t)((navy >> 8) | (navy << 8));
        const int touched = hud_celestial_twinkle(whole, 0, HUD_H, false, ss, ms);
        for (int y = 0; y < HUD_H; y += 12) {
            const int n = y + 12 > HUD_H ? HUD_H - y : 12;
            hud_celestial_twinkle(strips + (size_t)y * HUD_W, y, n, false, ss, ms);
            hud_celestial_twinkle(sw + (size_t)y * HUD_W, y, n, true, ss, ms);
        }
        CHECK(memcmp(whole, strips, px * sizeof *whole) == 0, "t=%d: strips differ from the whole frame", t);
        size_t changed = 0, far = 0, swap_bad = 0;
        for (int y = 0; y < HUD_H; ++y)
            for (int x = 0; x < HUD_W; ++x) {
                const uint16_t p = whole[(size_t)y * HUD_W + x];
                const uint16_t q = sw[(size_t)y * HUD_W + x];
                if ((uint16_t)((q >> 8) | (q << 8)) != p) swap_bad++;
                if (p == navy) continue;
                changed++;
                const int dx = x - 233, dy = y - 233;
                if (dx * dx + dy * dy > 190 * 190) far++;
                /* additive: no channel went down, and no step is a blink */
                const int dr = (((p >> 11) & 31) - ((navy >> 11) & 31)) * 8;
                const int dg = (((p >> 5) & 63) - ((navy >> 5) & 63)) * 4;
                const int db = ((p & 31) - (navy & 31)) * 8;
                CHECK(dr >= 0 && dg >= 0 && db >= 0, "t=%d: a glint darkened a pixel", t);
                const int m = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
                if (m > worst_step) worst_step = m;
            }
        CHECK(far == 0, "t=%d: %zu glint px beyond r 190", t, far);
        CHECK(swap_bad == 0, "t=%d: byte orders differ", t);
        CHECK(changed <= (size_t)touched, "t=%d: changed %zu > touched %d", t, changed, touched);
        CHECK(touched <= HUD_STAR_COUNT * 13, "t=%d: %d px touched", t, touched);
        if ((size_t)touched > worst_touched) worst_touched = (size_t)touched;
        free(whole); free(strips); free(sw);
    }
    CHECK(worst_step <= 140, "the strongest glint adds %d to a channel: not subtle", worst_step);
    CHECK(worst_touched >= 20, "the stars never lit (%zu)", worst_touched);
    printf("note: celestial twinkle touches %zu px per frame at most (%d stars), strongest glint +%d per channel\n",
           worst_touched, HUD_STAR_COUNT, worst_step);
    /* in the watch: alive draws them, a resting watch holds the dial still */
    uint16_t *a = lo_frame(navy), *b = lo_frame(navy), *d = lo_frame(navy);
    if (!a || !b || !d) return;
    hud_watch_t w = { .hh = 1, .mm = 0, .ss = 4, .ms = 100, .batt_pct = -1, .dial_baked = true,
                      .shadow = true, .alive = true };
    hud_overlay_watch(a, 0, HUD_H, false, &w, 255, 7);
    for (int y = 0; y < HUD_H; y += 12) {
        const int n = y + 12 > HUD_H ? HUD_H - y : 12;
        hud_overlay_watch(d + (size_t)y * HUD_W, y, n, false, &w, 255, 7);
    }
    CHECK(memcmp(a, d, px * sizeof *a) == 0, "CELESTIAL strips differ from the whole frame");
    w.alive = false;
    hud_overlay_watch(b, 0, HUD_H, false, &w, 255, 7);
    size_t diff = 0;
    for (size_t k = 0; k < px; ++k) if (a[k] != b[k]) diff++;
    CHECK(diff >= 10, "an alive CELESTIAL differs from a resting one in only %zu px", diff);
    free(a); free(b); free(d);
}

/* ---- TILT PARALLAX (2026-11-10) -------------------------------------------
 * The shaper (tilt.h) is pure; the stars read its output through the watch. */

/* A step of tilt gives an offset that then decays to zero. */
static void test_tilt_step_decays_to_zero(void)
{
    jr_tilt_t t;
    jr_tilt_reset(&t);
    jr_tilt_step(&t, 0, 0, 42);                       /* seeds at rest */
    CHECK(t.x == 0 && t.y == 0, "the seed is not rest: %d,%d", t.x, t.y);
    int prev = 0, peak = 0, zero_at = -1;
    for (int ms = 42; ms <= 20000; ms += 42) {
        jr_tilt_step(&t, 150, -80, 42);               /* lean and hold */
        if (ms == 42) {
            CHECK(t.x > 50 && t.x < JR_TILT_FULL, "the first frame of a step reads %d", t.x);
            CHECK(t.y < -20, "y leans the other way: %d", t.y);
            peak = t.x;
        }
        CHECK(t.x <= prev || ms == 42, "x rose while decaying at %d ms (%d > %d)", ms, t.x, prev);
        prev = t.x;
        if (t.x == 0 && t.y == 0 && zero_at < 0) zero_at = ms;
    }
    CHECK(peak > 0 && zero_at > 0, "the step never came back to zero (peak %d)", peak);
    CHECK(zero_at < 12000, "re-centring took %d ms", zero_at);
    CHECK(t.x == 0 && t.y == 0, "still leaning after 20 s: %d,%d", t.x, t.y);
    /* and the way back is a lean the other way, then rest */
    jr_tilt_step(&t, 0, 0, 42);
    CHECK(t.x < 0, "returning to the old pose does not lean back: %d", t.x);
}

/* Noise inside the dead zone gives exactly zero. */
static void test_tilt_dead_zone_is_exactly_zero(void)
{
    jr_tilt_t t;
    jr_tilt_reset(&t);
    jr_tilt_step(&t, 300, -200, 42);
    enum { NOISE = 8 };                               /* inside the dead zone, with room for the centre */
    unsigned s = 12345u;
    int bad = 0;
    for (int i = 0; i < 5000; ++i) {
        s = s * 1664525u + 1013904223u;
        const int nx = (int)((s >> 16) % (2 * NOISE + 1)) - NOISE;
        s = s * 1664525u + 1013904223u;
        const int ny = (int)((s >> 16) % (2 * NOISE + 1)) - NOISE;
        jr_tilt_step(&t, 300 + nx, -200 + ny, 42);
        if (t.x != 0 || t.y != 0) bad++;
    }
    CHECK(bad == 0, "%d frames of +-%d mg noise moved the dial", bad, (int)NOISE);
    jr_tilt_step(&t, 300 + JR_TILT_DEAD_MG + 40, -200, 42);
    CHECK(t.x > 0, "a lean past the dead zone reads nothing");
}

/* The clamp holds, in both directions, however far the hand goes. */
static void test_tilt_clamp_holds(void)
{
    jr_tilt_t t;
    jr_tilt_reset(&t);
    jr_tilt_step(&t, 0, 0, 42);
    jr_tilt_step(&t, 4000, -4000, 42);
    CHECK(t.x == JR_TILT_FULL && t.y == -JR_TILT_FULL, "no clamp: %d,%d", t.x, t.y);
    jr_tilt_step(&t, -4000, 4000, 42);
    CHECK(t.x == -JR_TILT_FULL && t.y == JR_TILT_FULL, "no clamp the other way: %d,%d", t.x, t.y);
    jr_tilt_reset(&t);
    jr_tilt_step(&t, 0, 0, 1);
    jr_tilt_step(&t, JR_TILT_DEAD_MG + JR_TILT_CLAMP_MG / 2, 0, 1);
    CHECK(t.x > JR_TILT_FULL / 2 - 8 && t.x < JR_TILT_FULL / 2 + 8, "half a clamp reads %d", t.x);
}

/* Ten minutes of zero input: no drift, in any pose, at any frame cadence. */
static void test_tilt_no_drift_in_ten_minutes(void)
{
    static const int poses[][2] = { {0, 0}, {-1000, 37}, {512, -733}, {1199, 1199}, {-3, 4} };
    static const int dts[] = { 42, 83, 200, 500 };
    for (size_t p = 0; p < sizeof poses / sizeof poses[0]; ++p)
        for (size_t d = 0; d < sizeof dts / sizeof dts[0]; ++d) {
            jr_tilt_t t;
            jr_tilt_reset(&t);
            int nonzero = 0;
            jr_tilt_step(&t, poses[p][0], poses[p][1], dts[d]);
            for (int ms = 0; ms < 600000; ms += dts[d]) {
                jr_tilt_step(&t, poses[p][0], poses[p][1], dts[d]);
                if (t.x != 0 || t.y != 0) nonzero++;
            }
            CHECK(nonzero == 0, "pose %zu dt %d drifted on %d frames", p, dts[d], nonzero);
            CHECK(abs(t.cx_q8 - poses[p][0] * 256) < JR_TILT_DEAD_MG * 256,
                  "the centre wandered from the pose: %d vs %d", t.cx_q8, poses[p][0] * 256);
        }
}

/* A long gap (the dial was not on the glass) reseeds; the diag word round-trips. */
static void test_tilt_gap_reseeds_and_word_packs(void)
{
    jr_tilt_t t;
    jr_tilt_reset(&t);
    jr_tilt_step(&t, 0, 0, 42);
    jr_tilt_step(&t, 300, 0, 42);
    CHECK(t.x != 0, "no lean to forget");
    jr_tilt_step(&t, 300, 0, JR_TILT_GAP_MS + 1);
    CHECK(t.x == 0 && t.y == 0, "a long gap kept the old lean: %d,%d", t.x, t.y);
    jr_tilt_step(&t, 300, 0, 42);
    CHECK(t.x == 0, "the reseeded centre is not the pose it came back in");
    const int vals[] = { -256, -1, 0, 1, 255, 256 };
    for (size_t i = 0; i < 6; ++i)
        for (size_t j = 0; j < 6; ++j) {
            const uint32_t w = jr_tilt_pack(vals[i], vals[j]);
            CHECK(jr_tilt_unpack_x(w) == vals[i] && jr_tilt_unpack_y(w) == vals[j],
                  "tilt word lost %d,%d", vals[i], vals[j]);
        }
    CHECK(jr_tilt_pack(-1, 1) == 0x0001FFFFu, "tilt word layout drifted: 0x%08x", jr_tilt_pack(-1, 1));
}

/* Depths come from the index alone; each depth slides by its own pixels. */
static void test_star_depths_and_shift(void)
{
    int count[3] = { 0, 0, 0 };
    for (int i = 0; i < HUD_STAR_COUNT; ++i) {
        const int d = hud_celestial_star_depth(i);
        CHECK(d >= 0 && d <= 2, "star %d depth %d", i, d);
        if (d < 0 || d > 2) return;
        count[d]++;
        int dx, dy;
        hud_celestial_star_shift(i, 0, 0, &dx, &dy);
        CHECK(dx == 0 && dy == 0, "star %d moves with no lean", i);
    }
    CHECK(count[0] >= 8 && count[1] >= 8 && count[2] >= 8, "depths %d/%d/%d are lopsided", count[0], count[1], count[2]);
    const int px[3] = { 6, 3, 1 };
    int seen[3] = { 0, 0, 0 };
    for (int i = 0; i < HUD_STAR_COUNT; ++i) {
        const int d = hud_celestial_star_depth(i);
        int dx, dy;
        hud_celestial_star_shift(i, 256, 0, &dx, &dy);
        if (dx == 0 && dy == 0) continue;               /* held by the glass or the hub */
        CHECK(dx == px[d] && dy == 0, "star %d (depth %d) slid %d,%d at full lean, wanted %d,0", i, d, dx, dy, px[d]);
        seen[d]++;
        hud_celestial_star_shift(i, -256, 256, &dx, &dy);
        if (dx != 0 || dy != 0) CHECK(dx == -px[d] && dy == px[d], "star %d (depth %d) at (-,+): %d,%d", i, d, dx, dy);
    }
    CHECK(seen[0] >= 6 && seen[1] >= 6 && seen[2] >= 6, "too few stars slide: %d/%d/%d", seen[0], seen[1], seen[2]);
    /* never outside the glass, never onto the hub, at any lean */
    int held = 0;
    for (int i = 0; i < HUD_STAR_COUNT; ++i)
        for (int tx = -256; tx <= 256; tx += 32)
            for (int ty = -256; ty <= 256; ty += 32) {
                int dx, dy;
                hud_celestial_star_shift(i, tx, ty, &dx, &dy);
                const int nx = s_stars[i].x + dx - 233, ny = s_stars[i].y + dy - 233;
                const int r2 = nx * nx + ny * ny;
                if (dx == 0 && dy == 0) { held++; continue; }
                CHECK(r2 <= HUD_STAR_GLASS_R * HUD_STAR_GLASS_R && r2 >= HUD_STAR_HUB_R * HUD_STAR_HUB_R,
                      "star %d left its ring at lean %d,%d", i, tx, ty);
            }
    printf("note: the glass/hub guard holds %d of %d star-lean pairs on this table\n", held, HUD_STAR_COUNT * 17 * 17);
}

/* The frame: lean (+max, 0) against 0. The glint at each star moved by its
 * own depth and nothing else in the frame changed. */
static void test_celestial_tilt_moves_only_the_glints(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    const uint16_t navy = 0x0841u;
    int best_t = 0, best_n = -1;
    for (int t = 0; t < 5000; t += 100) {               /* a moment with many stars lit */
        int n = 0;
        for (int i = 0; i < HUD_STAR_COUNT; ++i) if (hud_celestial_star_level(i, t / 1000, t % 1000) >= 12) n++;
        if (n > best_n) { best_n = n; best_t = t; }
    }
    CHECK(best_n >= 6, "no moment with six lit stars (best %d)", best_n);
    const int ss = best_t / 1000, ms = best_t % 1000;
    uint16_t *base = lo_frame(navy), *lean = lo_frame(navy), *want = lo_frame(navy), *zero = lo_frame(navy);
    if (!base || !lean || !want || !zero) return;
    hud_celestial_twinkle(base, 0, HUD_H, false, ss, ms);
    hud_celestial_twinkle_tilt(zero, 0, HUD_H, false, ss, ms, 0, 0);
    CHECK(memcmp(base, zero, px * sizeof *base) == 0, "tilt (0,0) is not the plain twinkle");
    CHECK(hud_celestial_twinkle_tilt(lean, 0, HUD_H, false, ss, ms, 256, 0) > 0, "nothing drawn under lean");
    int moved[3] = { 0, 0, 0 };
    int sh[HUD_STAR_COUNT], lit[HUD_STAR_COUNT];
    for (int i = 0; i < HUD_STAR_COUNT; ++i) {
        lit[i] = hud_celestial_star_level(i, ss, ms) >= 12;
        int dx, dy;
        hud_celestial_star_shift(i, 256, 0, &dx, &dy);
        sh[i] = dx;
        if (lit[i] && dx) moved[hud_celestial_star_depth(i)]++;
    }
    /* expected: each lit star's 5x5 window of the plain frame, moved by its own
     * shift. Windows must not touch for this to mean anything. */
    for (int i = 0; i < HUD_STAR_COUNT; ++i) {
        if (!lit[i]) continue;
        const int ix = s_stars[i].x, iy = s_stars[i].y;
        for (int j = i + 1; j < HUD_STAR_COUNT; ++j) {
            if (!lit[j]) continue;
            const int jx = s_stars[j].x, jy = s_stars[j].y;
            CHECK(abs(ix - jx) > 4 || abs(iy - jy) > 4, "plain windows of %d and %d collide", i, j);
            CHECK(abs(ix + sh[i] - jx - sh[j]) > 4 || abs(iy - jy) > 4, "moved windows of %d and %d collide", i, j);
        }
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                want[(size_t)(iy + dy) * HUD_W + ix + sh[i] + dx] = base[(size_t)(iy + dy) * HUD_W + ix + dx];
    }
    size_t bad = 0;
    for (size_t k = 0; k < px; ++k) if (want[k] != lean[k]) bad++;
    CHECK(bad == 0, "%zu px differ from the glints moved by their own depth", bad);
    CHECK(moved[0] >= 1 && moved[1] >= 1 && moved[2] >= 1,
          "this moment lit near %d / middle %d / far %d moved stars", moved[0], moved[1], moved[2]);
    uint16_t *strips = lo_frame(navy);
    if (strips) {
        for (int y = 0; y < HUD_H; y += 12) {
            const int n = y + 12 > HUD_H ? HUD_H - y : 12;
            hud_celestial_twinkle_tilt(strips + (size_t)y * HUD_W, y, n, false, ss, ms, 256, 0);
        }
        CHECK(memcmp(strips, lean, px * sizeof *strips) == 0, "strips differ from the whole frame under lean");
        free(strips);
    }
    /* in the watch: a lean moves CELESTIAL, a resting watch and any other dial ignore it */
    uint16_t *a = lo_frame(navy), *b = lo_frame(navy), *c = lo_frame(navy), *d = lo_frame(navy);
    if (a && b && c && d) {
        hud_watch_t w = { .hh = 1, .mm = 0, .ss = ss, .ms = ms, .batt_pct = -1, .dial_baked = true,
                          .shadow = true, .alive = true };
        hud_overlay_watch(a, 0, HUD_H, false, &w, 255, 7);
        w.tilt_x = 256;
        hud_overlay_watch(b, 0, HUD_H, false, &w, 255, 7);
        CHECK(memcmp(a, b, px * sizeof *a) != 0, "a lean did not move CELESTIAL");
        w.alive = false;
        hud_overlay_watch(c, 0, HUD_H, false, &w, 255, 7);
        w.tilt_x = 0;
        hud_overlay_watch(d, 0, HUD_H, false, &w, 255, 7);
        CHECK(memcmp(c, d, px * sizeof *c) == 0, "a resting CELESTIAL leaned");
        w.alive = true; w.tilt_x = 256;
        hud_overlay_watch(c, 0, HUD_H, false, &w, 255, 6);
        w.tilt_x = 0;
        hud_overlay_watch(d, 0, HUD_H, false, &w, 255, 6);
        CHECK(memcmp(c, d, px * sizeof *c) == 0, "a lean moved a dial that is not CELESTIAL");
    }
    free(a); free(b); free(c); free(d);
    free(base); free(lean); free(want); free(zero);
}

int main(void)
{
    test_light_deadband_is_today();
    test_light_is_continuous();
    test_light_shadow_falls_downhill_and_clamps();
    test_light_bevel_stays_normalised_and_mirrors();
    test_light_filter_converges_without_overshoot();
    test_watch_light_in_the_frame();
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
    test_charging_rim_animates_inside_battery_band();
    test_listen_ring_band_and_exclusivity();
    test_privacy_ring_is_persistent_and_bounded();
    test_tilt_offset_is_disabled();
    test_choices_strip_invariance();
    test_choices_strength_gate();
    test_choices_respect_bounds();
    test_choices_stay_in_free_band();
    test_battery_and_choices_do_not_overlap();
    test_battery_rim_alarm_rule();
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
    test_watch_jarvis_is_pixel_identical();
    test_watch_styles_paint_their_own_regions();
    test_watch_rim_ticks_are_pinned();
    test_outlined_hands_keep_their_rim();
    test_watch_hands_never_comb();
    test_watch_hands_blend_and_spare_the_dial();
    test_balance_swing_is_bounded_periodic_and_eased();
    test_balance_rotation_maps_centre_and_rim();
    test_balance_stays_in_its_disc_and_strips_agree();
    test_balance_in_the_watch_follows_alive_and_strips();
    test_celestial_star_table_is_sane();
    test_celestial_twinkle_is_additive_small_and_strip_invariant();
    test_tilt_step_decays_to_zero();
    test_tilt_dead_zone_is_exactly_zero();
    test_tilt_clamp_holds();
    test_tilt_no_drift_in_ten_minutes();
    test_tilt_gap_reseeds_and_word_packs();
    test_star_depths_and_shift();
    test_celestial_tilt_moves_only_the_glints();

    if (g_failures) {
        printf("%d failure(s) of %d checks\n", g_failures, g_checks);
        printf("hud_render tests FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("hud_render tests passed (%d checks)\n", g_checks);
    return 0;
}
