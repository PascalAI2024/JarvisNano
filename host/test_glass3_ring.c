/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_glass3_ring.c — Standalone host tests for Glass 3.0 Ring & Voice-Wave Engine.
 *
 * Pure integer C11, zero-libm verification of:
 *   1. Exact byte-for-byte strip-invariance (full 466-row vs 16-row & 12-row strips)
 *      across all 3 renderers and multiple telemetry/voice states.
 *   2. Strict radial & text safe-zone compliance (zero pixel bleed into protected zones).
 *   3. Exact swap_bytes=false vs swap_bytes=true bit-for-bit byte-swap equivalence.
 *   4. Guard-band bounds, NULL safety, strength<=0 gating, and telemetry reactivity.
 *   5. Optional `--dump-ppm <dir>` visual smoke rendering for human/agent inspection.
 */
#include "glass3_ring.h"
#include "jr_display/glass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_PX  ((size_t)HUD_W * (size_t)HUD_H)
#define CX2       465
#define CY2       465

static int g_checks = 0;
static int g_failures = 0;

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

static int r2_sq_at(int x, int y)
{
    const int dx2 = 2 * x - CX2;
    const int dy2 = 2 * y - CY2;
    return dx2 * dx2 + dy2 * dy2;
}

static uint32_t frame_luma_sum(const uint16_t *fb)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < PANEL_PX; ++i) {
        const uint16_t p = fb[i];
        const uint32_t r = ((p >> 11) & 31u) << 3;
        const uint32_t g = ((p >> 5) & 63u) << 2;
        const uint32_t b = (p & 31u) << 3;
        sum += r + g + b;
    }
    return sum;
}

static void seed_canvas(uint16_t *fb, bool swap_bytes)
{
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const uint16_t r = (uint16_t)(((x * 7 + y * 3) >> 6) & 7u);
            const uint16_t g = (uint16_t)(((x * 5 + y * 9) >> 6) & 15u);
            const uint16_t b = (uint16_t)(((x * 11 + y * 5) >> 6) & 7u);
            const uint16_t nat = (uint16_t)((r << 11) | (g << 5) | b);
            fb[(size_t)y * HUD_W + (size_t)x] = swap_bytes ? glass_bswap(nat) : nat;
        }
    }
}

static size_t count_diffs(const uint16_t *a, const uint16_t *b,
                          int *first_x, int *first_y)
{
    size_t diffs = 0;
    for (size_t i = 0; i < PANEL_PX; ++i) {
        if (a[i] != b[i]) {
            if (diffs == 0 && first_x && first_y) {
                *first_y = (int)(i / HUD_W);
                *first_x = (int)(i % HUD_W);
            }
            diffs++;
        }
    }
    return diffs;
}

static size_t count_swap_mismatches(const uint16_t *nat, const uint16_t *swp,
                                    int *first_x, int *first_y)
{
    size_t diffs = 0;
    for (size_t i = 0; i < PANEL_PX; ++i) {
        if (glass_bswap(nat[i]) != swp[i]) {
            if (diffs == 0 && first_x && first_y) {
                *first_y = (int)(i / HUD_W);
                *first_x = (int)(i % HUD_W);
            }
            diffs++;
        }
    }
    return diffs;
}

/* ---- 1. Strip invariance (full 466 vs 16-row and 12-row strips) ---------- */

static void test_chrono_strip_invariance(void)
{
    uint16_t *whole = malloc(PANEL_PX * sizeof *whole);
    uint16_t *s16   = malloc(PANEL_PX * sizeof *s16);
    uint16_t *s12   = malloc(PANEL_PX * sizeof *s12);
    CHECK(whole && s16 && s12, "alloc failed");
    if (!whole || !s16 || !s12) {
        free(whole); free(s16); free(s12);
        return;
    }

    const jr_g3_chrono_env_t envs[5] = {
        /* Ready state */
        { .batt_pct = 82, .charging = false, .wifi_bars = 4, .amp = 95,
          .muted = false, .codex_active = false, .voice_busy = false },
        /* Codex active + charging */
        { .batt_pct = 64, .charging = true, .wifi_bars = 3, .amp = 180,
          .muted = false, .codex_active = true, .voice_busy = false },
        /* Privacy muted + low battery */
        { .batt_pct = 12, .charging = false, .wifi_bars = 1, .amp = 220,
          .muted = true, .codex_active = false, .voice_busy = false },
        /* Voice busy + unknown batt + no wifi */
        { .batt_pct = 0xFF, .charging = false, .wifi_bars = 0, .amp = 250,
          .muted = false, .codex_active = false, .voice_busy = true },
        /* Edge max values */
        { .batt_pct = 100, .charging = false, .wifi_bars = 9, .amp = 0,
          .muted = false, .codex_active = false, .voice_busy = false },
    };

    for (int i = 0; i < 5; ++i) {
        const bool swp = (i & 1) != 0;
        const uint32_t t = 1234u + (uint32_t)i * 977u;
        seed_canvas(whole, swp);
        memcpy(s16, whole, PANEL_PX * sizeof *whole);
        memcpy(s12, whole, PANEL_PX * sizeof *whole);

        jr_g3_chrono_arc_rows(whole, 0, HUD_H, swp, t, &envs[i], 255);
        for (int y = 0; y < HUD_H; y += 16) {
            const int nr = (y + 16 <= HUD_H) ? 16 : (HUD_H - y);
            jr_g3_chrono_arc_rows(s16 + (size_t)y * HUD_W, y, nr, swp, t, &envs[i], 255);
        }
        for (int y = 0; y < HUD_H; y += 12) {
            const int nr = (y + 12 <= HUD_H) ? 12 : (HUD_H - y);
            jr_g3_chrono_arc_rows(s12 + (size_t)y * HUD_W, y, nr, swp, t, &envs[i], 255);
        }

        int fx = -1, fy = -1;
        size_t d16 = count_diffs(whole, s16, &fx, &fy);
        CHECK(d16 == 0, "chrono case %d 16-row strip diff=%zu first=(%d,%d)", i, d16, fx, fy);
        size_t d12 = count_diffs(whole, s12, &fx, &fy);
        CHECK(d12 == 0, "chrono case %d 12-row strip diff=%zu first=(%d,%d)", i, d12, fx, fy);
    }

    /* NULL env strip invariance */
    memset(whole, 0, PANEL_PX * sizeof *whole);
    memset(s16, 0, PANEL_PX * sizeof *s16);
    jr_g3_chrono_arc_rows(whole, 0, HUD_H, false, 500u, NULL, 200);
    for (int y = 0; y < HUD_H; y += 16) {
        const int nr = (y + 16 <= HUD_H) ? 16 : (HUD_H - y);
        jr_g3_chrono_arc_rows(s16 + (size_t)y * HUD_W, y, nr, false, 500u, NULL, 200);
    }
    int fx = -1, fy = -1;
    CHECK(count_diffs(whole, s16, &fx, &fy) == 0, "chrono NULL env strip mismatch at (%d,%d)", fx, fy);

    free(whole);
    free(s16);
    free(s12);
}

static void test_aetherium_strip_invariance(void)
{
    uint16_t *whole = malloc(PANEL_PX * sizeof *whole);
    uint16_t *s16   = malloc(PANEL_PX * sizeof *s16);
    uint16_t *s12   = malloc(PANEL_PX * sizeof *s12);
    CHECK(whole && s16 && s12, "alloc failed");
    if (!whole || !s16 || !s12) {
        free(whole); free(s16); free(s12);
        return;
    }

    const uint8_t live_bands[16] = {
        220, 245, 210, 195, 175, 230, 160, 140,
        185, 215, 150, 125, 190, 165, 110, 85
    };
    const jr_g3_aeth_mode_t modes[3] = {
        JR_G3_AETH_LISTEN, JR_G3_AETH_THINK, JR_G3_AETH_SPEAK
    };

    for (int m = 0; m < 3; ++m) {
        const bool swp = (m == 1);
        const uint32_t t = 2400u + (uint32_t)m * 1111u;
        const uint8_t *b = (m == 0) ? NULL : live_bands;
        const uint8_t amp = (m == 0) ? 45u : 210u;

        seed_canvas(whole, swp);
        memcpy(s16, whole, PANEL_PX * sizeof *whole);
        memcpy(s12, whole, PANEL_PX * sizeof *whole);

        jr_g3_aetherium_rows(whole, 0, HUD_H, swp, t, modes[m], b, amp, 255);
        for (int y = 0; y < HUD_H; y += 16) {
            const int nr = (y + 16 <= HUD_H) ? 16 : (HUD_H - y);
            jr_g3_aetherium_rows(s16 + (size_t)y * HUD_W, y, nr, swp, t, modes[m], b, amp, 255);
        }
        for (int y = 0; y < HUD_H; y += 12) {
            const int nr = (y + 12 <= HUD_H) ? 12 : (HUD_H - y);
            jr_g3_aetherium_rows(s12 + (size_t)y * HUD_W, y, nr, swp, t, modes[m], b, amp, 255);
        }

        int fx = -1, fy = -1;
        size_t d16 = count_diffs(whole, s16, &fx, &fy);
        CHECK(d16 == 0, "aetherium mode %d 16-row strip diff=%zu first=(%d,%d)", m, d16, fx, fy);
        size_t d12 = count_diffs(whole, s12, &fx, &fy);
        CHECK(d12 == 0, "aetherium mode %d 12-row strip diff=%zu first=(%d,%d)", m, d12, fx, fy);
    }

    free(whole);
    free(s16);
    free(s12);
}

static void test_scrub_strip_invariance(void)
{
    uint16_t *whole = malloc(PANEL_PX * sizeof *whole);
    uint16_t *s16   = malloc(PANEL_PX * sizeof *s16);
    uint16_t *s12   = malloc(PANEL_PX * sizeof *s12);
    CHECK(whole && s16 && s12, "alloc failed");
    if (!whole || !s16 || !s12) {
        free(whole); free(s16); free(s12);
        return;
    }

    const uint8_t pcts[5] = { 0, 1, 50, 73, 100 };
    const jr_g3_scrub_kind_t kinds[3] = {
        JR_G3_SCRUB_VOLUME, JR_G3_SCRUB_BRIGHTNESS, JR_G3_SCRUB_TIMER
    };

    for (int i = 0; i < 5; ++i) {
        const jr_g3_scrub_kind_t k = kinds[i % 3];
        const bool swp = (i & 1) != 0;
        const uint32_t t = 777u + (uint32_t)i * 533u;

        seed_canvas(whole, swp);
        memcpy(s16, whole, PANEL_PX * sizeof *whole);
        memcpy(s12, whole, PANEL_PX * sizeof *whole);

        jr_g3_bezel_scrub_rows(whole, 0, HUD_H, swp, t, pcts[i], k, 255);
        for (int y = 0; y < HUD_H; y += 16) {
            const int nr = (y + 16 <= HUD_H) ? 16 : (HUD_H - y);
            jr_g3_bezel_scrub_rows(s16 + (size_t)y * HUD_W, y, nr, swp, t, pcts[i], k, 255);
        }
        for (int y = 0; y < HUD_H; y += 12) {
            const int nr = (y + 12 <= HUD_H) ? 12 : (HUD_H - y);
            jr_g3_bezel_scrub_rows(s12 + (size_t)y * HUD_W, y, nr, swp, t, pcts[i], k, 255);
        }

        int fx = -1, fy = -1;
        size_t d16 = count_diffs(whole, s16, &fx, &fy);
        CHECK(d16 == 0, "scrub pct=%u 16-row strip diff=%zu first=(%d,%d)", pcts[i], d16, fx, fy);
        size_t d12 = count_diffs(whole, s12, &fx, &fy);
        CHECK(d12 == 0, "scrub pct=%u 12-row strip diff=%zu first=(%d,%d)", pcts[i], d12, fx, fy);
    }

    free(whole);
    free(s16);
    free(s12);
}

/* ---- 2. Strict radial & text safe-zone compliance ------------------------ */

static void test_chrono_safe_zones_and_telemetry(void)
{
    uint16_t *fb = calloc(PANEL_PX, sizeof *fb);
    uint16_t *fb2 = calloc(PANEL_PX, sizeof *fb2);
    CHECK(fb && fb2, "alloc failed");
    if (!fb || !fb2) {
        free(fb); free(fb2);
        return;
    }

    const jr_g3_chrono_env_t active = {
        .batt_pct = 75, .charging = false, .wifi_bars = 4, .amp = 190,
        .muted = false, .codex_active = true, .voice_busy = false
    };
    jr_g3_chrono_arc_rows(fb, 0, HUD_H, false, 1800u, &active, 255);

    size_t core_bleed = 0;
    size_t gap_bleed = 0;
    size_t outer_bleed = 0;
    size_t ripple_px = 0;
    size_t bezel_px = 0;
    size_t cap_top_px = 0, cap_right_px = 0, cap_bl_px = 0, cap_tl_px = 0;
    int max_ripple_ch = 0;

    const int r106_sq = (2 * JR_G3_CHRONO_RIPPLE_R_MIN) * (2 * JR_G3_CHRONO_RIPPLE_R_MIN);
    const int r168_sq = (2 * JR_G3_CHRONO_RIPPLE_R_MAX) * (2 * JR_G3_CHRONO_RIPPLE_R_MAX);
    const int r186_sq = (2 * JR_G3_CHRONO_BEZEL_R_MIN)  * (2 * JR_G3_CHRONO_BEZEL_R_MIN);
    const int r207_sq = (2 * 207) * (2 * 207);
    const int r212_sq = (2 * 212) * (2 * 212);
    const int r226_sq = (2 * 226) * (2 * 226);
    const int r231_sq = (2 * JR_G3_CHRONO_BEZEL_R_MAX)  * (2 * JR_G3_CHRONO_BEZEL_R_MAX);

    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const uint16_t p = fb[(size_t)y * HUD_W + (size_t)x];
            if (p == 0u) {
                continue;
            }
            const int r2 = r2_sq_at(x, y);
            if (r2 < r106_sq) {
                core_bleed++;
            } else if (r2 > r168_sq && r2 < r186_sq) {
                gap_bleed++;
            } else if (r2 > r231_sq) {
                outer_bleed++;
            } else if (r2 >= r106_sq && r2 <= r168_sq) {
                ripple_px++;
                const int r8 = ((p >> 11) & 31) << 3;
                const int g8 = ((p >> 5) & 63) << 2;
                const int b8 = (p & 31) << 3;
                if (r8 > max_ripple_ch) { max_ripple_ch = r8; }
                if (g8 > max_ripple_ch) { max_ripple_ch = g8; }
                if (b8 > max_ripple_ch) { max_ripple_ch = b8; }
            } else if (r2 >= r186_sq && r2 <= r207_sq) {
                bezel_px++;
            } else if (r2 >= r212_sq && r2 <= r226_sq) {
                if (y < 60 && x > 140 && x < 326) { cap_top_px++; }
                if (x > 380 && y > 70 && y < 395) { cap_right_px++; }
                if (x < 160 && y > 220)           { cap_bl_px++; }
                if (x < 160 && y < 190 && y > 30) { cap_tl_px++; }
            }
        }
    }

    CHECK(core_bleed == 0,  "CHRONO-ARC bled %zu px into r < 106", core_bleed);
    CHECK(gap_bleed == 0,   "CHRONO-ARC bled %zu px into r in (168, 186)", gap_bleed);
    CHECK(outer_bleed == 0, "CHRONO-ARC bled %zu px into r > 231", outer_bleed);
    CHECK(ripple_px > 500,  "CHRONO-ARC sonar ripples too sparse: %zu px", ripple_px);
    CHECK(max_ripple_ch <= 28, "CHRONO-ARC sonar ripples too bright: max_ch=%d (want whisper-soft)", max_ripple_ch);
    CHECK(bezel_px > 4000,  "CHRONO-ARC micrometer bezel too sparse: %zu px", bezel_px);
    CHECK(cap_top_px > 1200,   "CHRONO-ARC top AI capsule sparse: %zu px", cap_top_px);
    CHECK(cap_right_px > 2000, "CHRONO-ARC right battery capsule sparse: %zu px", cap_right_px);
    CHECK(cap_bl_px > 1500,    "CHRONO-ARC bottom-left mic capsule sparse: %zu px", cap_bl_px);
    CHECK(cap_tl_px > 800,     "CHRONO-ARC top-left wifi capsule sparse: %zu px", cap_tl_px);

    /* Verify telemetry changes output (muted vs live, wifi_bars 4 vs 0, charging vs normal) */
    jr_g3_chrono_env_t muted = active;
    muted.muted = true;
    memset(fb2, 0, PANEL_PX * sizeof *fb2);
    jr_g3_chrono_arc_rows(fb2, 0, HUD_H, false, 1800u, &muted, 255);
    CHECK(count_diffs(fb, fb2, NULL, NULL) > 2000, "CHRONO-ARC muted state did not visibly update");

    jr_g3_chrono_env_t low_wifi = active;
    low_wifi.wifi_bars = 0;
    memset(fb2, 0, PANEL_PX * sizeof *fb2);
    jr_g3_chrono_arc_rows(fb2, 0, HUD_H, false, 1800u, &low_wifi, 255);
    CHECK(frame_luma_sum(fb) > frame_luma_sum(fb2), "CHRONO-ARC wifi_bars=4 should be brighter than 0");

    free(fb);
    free(fb2);
}

static void test_aetherium_safe_zones_and_reactivity(void)
{
    uint16_t *fb_loud = calloc(PANEL_PX, sizeof *fb_loud);
    uint16_t *fb_quiet = calloc(PANEL_PX, sizeof *fb_quiet);
    CHECK(fb_loud && fb_quiet, "alloc failed");
    if (!fb_loud || !fb_quiet) {
        free(fb_loud); free(fb_quiet);
        return;
    }

    const uint8_t loud_bands[16] = {
        250, 240, 230, 220, 210, 235, 245, 220,
        200, 195, 215, 230, 210, 185, 170, 160
    };
    jr_g3_aetherium_rows(fb_loud, 0, HUD_H, false, 3125u, JR_G3_AETH_SPEAK, loud_bands, 240, 255);
    jr_g3_aetherium_rows(fb_quiet, 0, HUD_H, false, 3125u, JR_G3_AETH_LISTEN, NULL, 0, 255);

    size_t center_badge_bleed = 0;
    size_t inner_other_bleed = 0;
    size_t outer_bleed = 0;
    size_t ring_px = 0;
    size_t tick_px = 0;
    size_t ribbon_px = 0;

    const int r160_sq = (2 * JR_G3_AETH_RING_R_MIN) * (2 * JR_G3_AETH_RING_R_MIN);
    const int r218_sq = (2 * 218) * (2 * 218);
    const int r231_sq = (2 * JR_G3_AETH_RING_R_MAX) * (2 * JR_G3_AETH_RING_R_MAX);

    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const uint16_t p = fb_loud[(size_t)y * HUD_W + (size_t)x];
            if (p == 0u) {
                continue;
            }
            const int r2 = r2_sq_at(x, y);
            const bool in_ribbon = (x >= JR_G3_AETH_RIBBON_X0 && x <= JR_G3_AETH_RIBBON_X1 &&
                                    y >= JR_G3_AETH_RIBBON_Y0 && y <= JR_G3_AETH_RIBBON_Y1);
            if (r2 > r231_sq) {
                outer_bleed++;
            } else if (r2 < r160_sq) {
                if (in_ribbon) {
                    ribbon_px++;
                } else {
                    if (y >= 165 && y <= 275) {
                        center_badge_bleed++;
                    }
                    inner_other_bleed++;
                }
            } else {
                ring_px++;
                if (r2 >= r218_sq) {
                    tick_px++;
                }
            }
        }
    }

    CHECK(outer_bleed == 0, "AETHERIUM bled %zu px into r > 231", outer_bleed);
    CHECK(center_badge_bleed == 0, "AETHERIUM bled %zu px into center badge/core y=[165,275]", center_badge_bleed);
    CHECK(inner_other_bleed == 0, "AETHERIUM bled %zu px into r < 160 outside vocal ribbon", inner_other_bleed);
    CHECK(ring_px > 25000, "AETHERIUM harmonic ring too sparse: %zu px", ring_px);
    CHECK(tick_px > 2000,  "AETHERIUM outer tick ring + cardinals too sparse: %zu px", tick_px);
    CHECK(ribbon_px > 3500, "AETHERIUM vocal waveform ribbon too sparse: %zu px", ribbon_px);

    /* Loud SPEAK must emit substantially higher luminance than quiet LISTEN */
    const uint32_t luma_loud = frame_luma_sum(fb_loud);
    const uint32_t luma_quiet = frame_luma_sum(fb_quiet);
    CHECK(luma_loud > luma_quiet + 200000u,
          "AETHERIUM loud (%u) not significantly brighter than quiet (%u)", luma_loud, luma_quiet);

    /* Cardinal dots at 12/3/6/9 (r=223) must be bright */
    CHECK(fb_loud[(size_t)(233 - 223) * HUD_W + 233] != 0u, "AETHERIUM 12 o'clock cardinal dot dark");
    CHECK(fb_loud[(size_t)233 * HUD_W + (233 + 223)] != 0u, "AETHERIUM 3 o'clock cardinal dot dark");
    CHECK(fb_loud[(size_t)(233 + 223) * HUD_W + 233] != 0u, "AETHERIUM 6 o'clock cardinal dot dark");
    CHECK(fb_loud[(size_t)233 * HUD_W + (233 - 223)] != 0u, "AETHERIUM 9 o'clock cardinal dot dark");

    free(fb_loud);
    free(fb_quiet);
}

static void test_scrub_safe_zones_and_monotonicity(void)
{
    uint16_t *fb0   = calloc(PANEL_PX, sizeof *fb0);
    uint16_t *fb25  = calloc(PANEL_PX, sizeof *fb25);
    uint16_t *fb80  = calloc(PANEL_PX, sizeof *fb80);
    uint16_t *fb100 = calloc(PANEL_PX, sizeof *fb100);
    uint16_t *fb150 = calloc(PANEL_PX, sizeof *fb150);
    CHECK(fb0 && fb25 && fb80 && fb100 && fb150, "alloc failed");
    if (!fb0 || !fb25 || !fb80 || !fb100 || !fb150) {
        free(fb0); free(fb25); free(fb80); free(fb100); free(fb150);
        return;
    }

    jr_g3_bezel_scrub_rows(fb0,   0, HUD_H, false, 1000u,   0, JR_G3_SCRUB_VOLUME, 255);
    jr_g3_bezel_scrub_rows(fb25,  0, HUD_H, false, 1000u,  25, JR_G3_SCRUB_VOLUME, 255);
    jr_g3_bezel_scrub_rows(fb80,  0, HUD_H, false, 1000u,  80, JR_G3_SCRUB_VOLUME, 255);
    jr_g3_bezel_scrub_rows(fb100, 0, HUD_H, false, 1000u, 100, JR_G3_SCRUB_VOLUME, 255);
    jr_g3_bezel_scrub_rows(fb150, 0, HUD_H, false, 1000u, 150, JR_G3_SCRUB_VOLUME, 255);

    const int r204_sq = (2 * JR_G3_SCRUB_R_MIN) * (2 * JR_G3_SCRUB_R_MIN);
    const int r231_sq = (2 * JR_G3_SCRUB_R_MAX) * (2 * JR_G3_SCRUB_R_MAX);
    size_t inner_bleed = 0, outer_bleed = 0, dial_px = 0;

    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const uint16_t p = fb80[(size_t)y * HUD_W + (size_t)x];
            if (p == 0u) {
                continue;
            }
            const int r2 = r2_sq_at(x, y);
            if (r2 < r204_sq) {
                inner_bleed++;
            } else if (r2 > r231_sq) {
                outer_bleed++;
            } else {
                dial_px++;
            }
        }
    }

    CHECK(inner_bleed == 0, "SCRUB bled %zu px into r < 204", inner_bleed);
    CHECK(outer_bleed == 0, "SCRUB bled %zu px into r > 231", outer_bleed);
    CHECK(dial_px > 10000,  "SCRUB dial too sparse: %zu px", dial_px);

    const uint32_t l0 = frame_luma_sum(fb0);
    const uint32_t l25 = frame_luma_sum(fb25);
    const uint32_t l80 = frame_luma_sum(fb80);
    const uint32_t l100 = frame_luma_sum(fb100);
    CHECK(l0 > 0u && l25 > l0 && l80 > l25 && l100 > l80,
          "SCRUB luminance not monotonic: 0=%u 25=%u 80=%u 100=%u", l0, l25, l80, l100);
    CHECK(count_diffs(fb100, fb150, NULL, NULL) == 0,
          "SCRUB value_pct=150 did not clamp identically to 100");

    free(fb0); free(fb25); free(fb80); free(fb100); free(fb150);
}

/* ---- 3. Exact swap_bytes=false vs swap_bytes=true equivalence ------------ */

static void test_swap_bytes_equivalence(void)
{
    uint16_t *nat = malloc(PANEL_PX * sizeof *nat);
    uint16_t *swp = malloc(PANEL_PX * sizeof *swp);
    CHECK(nat && swp, "alloc failed");
    if (!nat || !swp) {
        free(nat); free(swp);
        return;
    }

    const jr_g3_chrono_env_t env = {
        .batt_pct = 58, .charging = true, .wifi_bars = 3, .amp = 145,
        .muted = false, .codex_active = true, .voice_busy = false
    };
    const uint8_t bands[16] = {
        180, 210, 240, 190, 160, 220, 200, 150,
        175, 205, 190, 140, 120, 165, 130, 90
    };

    int fx = -1, fy = -1;

    /* 1. CHRONO-ARC */
    seed_canvas(nat, false);
    seed_canvas(swp, true);
    jr_g3_chrono_arc_rows(nat, 0, HUD_H, false, 4123u, &env, 230);
    jr_g3_chrono_arc_rows(swp, 0, HUD_H, true,  4123u, &env, 230);
    size_t m1 = count_swap_mismatches(nat, swp, &fx, &fy);
    CHECK(m1 == 0, "CHRONO-ARC swap_bytes mismatch=%zu first=(%d,%d)", m1, fx, fy);

    /* 2. AETHERIUM */
    seed_canvas(nat, false);
    seed_canvas(swp, true);
    jr_g3_aetherium_rows(nat, 0, HUD_H, false, 6789u, JR_G3_AETH_SPEAK, bands, 215, 245);
    jr_g3_aetherium_rows(swp, 0, HUD_H, true,  6789u, JR_G3_AETH_SPEAK, bands, 215, 245);
    size_t m2 = count_swap_mismatches(nat, swp, &fx, &fy);
    CHECK(m2 == 0, "AETHERIUM swap_bytes mismatch=%zu first=(%d,%d)", m2, fx, fy);

    /* 3. BEZEL SCRUB */
    seed_canvas(nat, false);
    seed_canvas(swp, true);
    jr_g3_bezel_scrub_rows(nat, 0, HUD_H, false, 2345u, 68, JR_G3_SCRUB_BRIGHTNESS, 250);
    jr_g3_bezel_scrub_rows(swp, 0, HUD_H, true,  2345u, 68, JR_G3_SCRUB_BRIGHTNESS, 250);
    size_t m3 = count_swap_mismatches(nat, swp, &fx, &fy);
    CHECK(m3 == 0, "BEZEL SCRUB swap_bytes mismatch=%zu first=(%d,%d)", m3, fx, fy);

    free(nat);
    free(swp);
}

/* ---- 4. Guard-band bounds & strength=0 / NULL safety --------------------- */

static void test_guard_bands_and_gates(void)
{
    enum { GUARD = 128, NROWS = 16 };
    const size_t body = (size_t)NROWS * HUD_W;
    uint16_t *buf = malloc((body + 2 * GUARD) * sizeof *buf);
    CHECK(buf != NULL, "alloc failed");
    if (!buf) {
        return;
    }

    const jr_g3_chrono_env_t env = {
        .batt_pct = 90, .charging = false, .wifi_bars = 4, .amp = 160,
        .muted = false, .codex_active = false, .voice_busy = true
    };
    const uint8_t bands[16] = { 200, 180, 160, 190, 210, 230, 170, 150, 140, 160, 180, 200, 190, 170, 150, 130 };

    /* strength == 0 must paint ZERO pixels */
    for (size_t i = 0; i < body + 2 * GUARD; ++i) {
        buf[i] = 0x5A5Au;
    }
    jr_g3_chrono_arc_rows(buf + GUARD, 197, NROWS, false, 1000u, &env, 0);
    jr_g3_aetherium_rows(buf + GUARD, 290, NROWS, false, 1000u, JR_G3_AETH_SPEAK, bands, 200, 0);
    jr_g3_bezel_scrub_rows(buf + GUARD, 20, NROWS, false, 1000u, 75, JR_G3_SCRUB_TIMER, 0);
    size_t touched = 0;
    for (size_t i = 0; i < body + 2 * GUARD; ++i) {
        if (buf[i] != 0x5A5Au) { touched++; }
    }
    CHECK(touched == 0, "strength=0 modified %zu pixels", touched);

    /* NULL / nrows<=0 safety */
    jr_g3_chrono_arc_rows(NULL, 0, 16, false, 0, &env, 255);
    jr_g3_aetherium_rows(NULL, 0, 16, false, 0, JR_G3_AETH_LISTEN, NULL, 0, 255);
    jr_g3_bezel_scrub_rows(NULL, 0, 16, false, 0, 50, JR_G3_SCRUB_VOLUME, 255);
    jr_g3_chrono_arc_rows(buf + GUARD, 0, 0, false, 0, &env, 255);
    jr_g3_aetherium_rows(buf + GUARD, 0, -4, false, 0, JR_G3_AETH_LISTEN, NULL, 0, 255);
    jr_g3_bezel_scrub_rows(buf + GUARD, 0, 0, false, 0, 50, JR_G3_SCRUB_VOLUME, 255);

    /* Active render on unaligned strip must never clobber guard sentinels */
    jr_g3_chrono_arc_rows(buf + GUARD, 197, NROWS, false, 1000u, &env, 255);
    jr_g3_aetherium_rows(buf + GUARD, 290, NROWS, false, 1000u, JR_G3_AETH_SPEAK, bands, 220, 255);
    jr_g3_bezel_scrub_rows(buf + GUARD, 15, NROWS, false, 1000u, 75, JR_G3_SCRUB_TIMER, 255);

    size_t head_bad = 0, tail_bad = 0;
    for (int i = 0; i < GUARD; ++i) {
        if (buf[i] != 0x5A5Au) { head_bad++; }
        if (buf[GUARD + body + (size_t)i] != 0x5A5Au) { tail_bad++; }
    }
    CHECK(head_bad == 0, "clobbered %zu guard px before strip", head_bad);
    CHECK(tail_bad == 0, "clobbered %zu guard px after strip", tail_bad);

    free(buf);
}

/* ---- Optional PPM Visual Smoke Dump -------------------------------------- */

static void write_ppm(const char *path, const uint16_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", HUD_W, HUD_H);
    uint8_t row_rgb[HUD_W * 3];
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const uint16_t p = fb[(size_t)y * HUD_W + (size_t)x];
            const uint8_t r5 = (uint8_t)((p >> 11) & 31u);
            const uint8_t g6 = (uint8_t)((p >> 5) & 63u);
            const uint8_t b5 = (uint8_t)(p & 31u);
            row_rgb[x * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            row_rgb[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            row_rgb[x * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
        fwrite(row_rgb, 1, sizeof(row_rgb), f);
    }
    fclose(f);
}

static void dump_smoke_ppms(const char *dir)
{
    uint16_t *fb = calloc(PANEL_PX, sizeof *fb);
    if (!fb) {
        return;
    }
    char path[512];

    const jr_g3_chrono_env_t env = {
        .batt_pct = 78, .charging = false, .wifi_bars = 4, .amp = 175,
        .muted = false, .codex_active = true, .voice_busy = false
    };
    memset(fb, 0, PANEL_PX * sizeof *fb);
    jr_g3_chrono_arc_rows(fb, 0, HUD_H, false, 1800u, &env, 255);
    snprintf(path, sizeof(path), "%s/chrono_arc.ppm", dir);
    write_ppm(path, fb);

    const uint8_t bands[16] = {
        235, 250, 210, 185, 220, 240, 195, 160,
        205, 230, 175, 145, 190, 160, 120, 95
    };
    memset(fb, 0, PANEL_PX * sizeof *fb);
    jr_g3_aetherium_rows(fb, 0, HUD_H, false, 2400u, JR_G3_AETH_SPEAK, bands, 225, 255);
    snprintf(path, sizeof(path), "%s/aetherium_speak.ppm", dir);
    write_ppm(path, fb);

    memset(fb, 0, PANEL_PX * sizeof *fb);
    jr_g3_bezel_scrub_rows(fb, 0, HUD_H, false, 1200u, 72, JR_G3_SCRUB_VOLUME, 255);
    snprintf(path, sizeof(path), "%s/bezel_scrub.ppm", dir);
    write_ppm(path, fb);

    free(fb);
}

int main(int argc, char **argv)
{
    test_chrono_strip_invariance();
    test_aetherium_strip_invariance();
    test_scrub_strip_invariance();
    test_chrono_safe_zones_and_telemetry();
    test_aetherium_safe_zones_and_reactivity();
    test_scrub_safe_zones_and_monotonicity();
    test_swap_bytes_equivalence();
    test_guard_bands_and_gates();

    if (argc >= 3 && strcmp(argv[1], "--dump-ppm") == 0) {
        dump_smoke_ppms(argv[2]);
    }

    if (g_failures == 0) {
        printf("glass3_ring passed (%d checks)\n", g_checks);
        return 0;
    }
    printf("%d failure(s) of %d checks\n", g_failures, g_checks);
    return 1;
}
