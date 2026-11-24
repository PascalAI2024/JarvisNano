/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_glass3_screens.c — Standalone host verification for Glass 3.0 data
 * screens (ATMOSPHAERA & CORTEX COMMAND).
 *
 * Verifies:
 *   1. Exact byte-for-byte strip-invariance (full 466-row vs 16-row and 12-row
 *      strips) across day/night/rain/storm/snow/fog/codex/charging/fade states.
 *   2. Exact swap_bytes=false vs swap_bytes=true byte-swap equivalence across
 *      every pixel of the 466x466 frame (both on black and prefilled canvases).
 *   3. Strict non-overlap safe-zone invariants (emblem isolation vs temp hero,
 *      horizon clearance below y=264, 24h Sun Jewel cardinal tracking, 4-quadrant
 *      gauge isolation, LED dot placement, sparkline bounding box, OLED corner clip).
 *   4. Optional PPM visual specimen export when G3_DUMP_PPM_DIR is set.
 *
 * Compiles cleanly with zero warnings under:
 *   cc -std=c11 -O2 -Wall -Wextra -Werror \
 *      host/test_glass3_screens.c components/jr_display/src/glass3_screens.c \
 *      -Icomponents/jr_display/src -o test_glass3_screens
 */
#include "glass3_screens.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W JR_G3_PANEL_W
#define H JR_G3_PANEL_H
#define FRAME_PX ((size_t)W * (size_t)H)

static int g_checks = 0;
static int g_fails = 0;

static void check(bool cond, const char *msg)
{
    ++g_checks;
    if (!cond) {
        ++g_fails;
        fprintf(stderr, "FAIL: %s\n", msg);
    }
}

static inline uint16_t bswap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static uint16_t s_full[FRAME_PX];
static uint16_t s_strip[FRAME_PX];
static uint16_t s_ref[FRAME_PX];
static uint16_t s_cmp[FRAME_PX];

static void fill_pattern(uint16_t *buf, bool swap_bytes)
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint16_t c = (uint16_t)((((x * 3) & 31) << 11) |
                                    (((y * 5) & 63) << 5) |
                                    (((x + y) & 31)));
            buf[(size_t)y * W + (size_t)x] = swap_bytes ? bswap16(c) : c;
        }
    }
}

static void render_atmos_strips(uint16_t *dst, int strip_h, bool swap_bytes,
                                const jr_g3_atmosphaera_t *env)
{
    for (int y0 = 0; y0 < H; y0 += strip_h) {
        int n = (y0 + strip_h <= H) ? strip_h : (H - y0);
        jr_g3_atmosphaera_rows(dst + (size_t)y0 * W, y0, n, swap_bytes, env);
    }
}

static void render_cortex_strips(uint16_t *dst, int strip_h, bool swap_bytes,
                                 const jr_g3_cortex_t *env)
{
    for (int y0 = 0; y0 < H; y0 += strip_h) {
        int n = (y0 + strip_h <= H) ? strip_h : (H - y0);
        jr_g3_cortex_rows(dst + (size_t)y0 * W, y0, n, swap_bytes, env);
    }
}

static bool frames_equal(const uint16_t *a, const uint16_t *b, int *out_x, int *out_y)
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            size_t idx = (size_t)y * W + (size_t)x;
            if (a[idx] != b[idx]) {
                if (out_x) { *out_x = x; }
                if (out_y) { *out_y = y; }
                return false;
            }
        }
    }
    return true;
}

static bool frames_bswap_equal(const uint16_t *native_buf, const uint16_t *swapped_buf)
{
    for (size_t i = 0; i < FRAME_PX; ++i) {
        if (bswap16(native_buf[i]) != swapped_buf[i]) {
            return false;
        }
    }
    return true;
}

static int luma565(uint16_t c)
{
    int r = ((c >> 11) & 31) << 3;
    int g = ((c >> 5) & 63) << 2;
    int b = (c & 31) << 3;
    return (r * 3 + g * 6 + b) / 10;
}

static bool dump_ppm(const char *dir, const char *name, const uint16_t *fb)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < FRAME_PX; ++i) {
        uint16_t c = fb[i];
        uint8_t rgb[3] = {
            (uint8_t)((((c >> 11) & 31) * 255) / 31),
            (uint8_t)((((c >> 5) & 63) * 255) / 63),
            (uint8_t)(((c & 31) * 255) / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

/* ---- 1. ATMOSPHAERA tests ------------------------------------------------ */

static void test_atmosphaera_strip_and_swap(void)
{
    const jr_g3_atmosphaera_t cases[] = {
        /* 0: Sunny noon warm */
        { .hour = 12, .minute = 0,  .weather_code = JR_G3_WX_CLEAR,  .is_day = true,  .temp_f = 82, .rain_pct = 0,  .now_ms = 1200,  .strength = 255 },
        /* 1: Clear midnight cool */
        { .hour = 0,  .minute = 30, .weather_code = JR_G3_WX_CLEAR,  .is_day = false, .temp_f = 58, .rain_pct = 0,  .now_ms = 4500,  .strength = 255 },
        /* 2: Morning partly cloudy */
        { .hour = 8,  .minute = 15, .weather_code = JR_G3_WX_PARTLY, .is_day = true,  .temp_f = 72, .rain_pct = 15, .now_ms = 8100,  .strength = 255 },
        /* 3: Afternoon overcast clouds */
        { .hour = 15, .minute = 45, .weather_code = JR_G3_WX_CLOUDS, .is_day = true,  .temp_f = 64, .rain_pct = 30, .now_ms = 9900,  .strength = 255 },
        /* 4: Pre-dawn fog (WMO 45) */
        { .hour = 4,  .minute = 20, .weather_code = 45,              .is_day = false, .temp_f = 52, .rain_pct = 10, .now_ms = 12345, .strength = 255 },
        /* 5: Sunset rain (WMO 63) */
        { .hour = 17, .minute = 30, .weather_code = 63,              .is_day = true,  .temp_f = 66, .rain_pct = 85, .now_ms = 23456, .strength = 255 },
        /* 6: Evening storm (WMO 95) */
        { .hour = 21, .minute = 40, .weather_code = 95,              .is_day = false, .temp_f = 70, .rain_pct = 95, .now_ms = 34567, .strength = 255 },
        /* 7: Winter snow (WMO 73) */
        { .hour = 10, .minute = 0,  .weather_code = 73,              .is_day = true,  .temp_f = 28, .rain_pct = 70, .now_ms = 45678, .strength = 255 },
        /* 8: Partial fade over non-black canvas */
        { .hour = 14, .minute = 10, .weather_code = JR_G3_WX_RAIN,   .is_day = true,  .temp_f = 75, .rain_pct = 60, .now_ms = 56789, .strength = 128 },
    };

    const size_t n_cases = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n_cases; ++i) {
        const bool prefill = (cases[i].strength < 255);

        /* Full frame vs 16-row strips (swap_bytes = false) */
        if (prefill) {
            fill_pattern(s_full, false);
            fill_pattern(s_strip, false);
        } else {
            memset(s_full, 0, sizeof(s_full));
            memset(s_strip, 0, sizeof(s_strip));
        }
        jr_g3_atmosphaera_rows(s_full, 0, H, false, &cases[i]);
        render_atmos_strips(s_strip, 16, false, &cases[i]);
        int bx = -1, by = -1;
        char msg[128];
        snprintf(msg, sizeof(msg), "atmos case %zu: 16-row strip == full (native)", i);
        check(frames_equal(s_full, s_strip, &bx, &by), msg);

        /* Full frame vs 12-row strips (swap_bytes = true) */
        if (prefill) {
            fill_pattern(s_cmp, true);
            fill_pattern(s_strip, true);
        } else {
            memset(s_cmp, 0, sizeof(s_cmp));
            memset(s_strip, 0, sizeof(s_strip));
        }
        jr_g3_atmosphaera_rows(s_cmp, 0, H, true, &cases[i]);
        render_atmos_strips(s_strip, 12, true, &cases[i]);
        snprintf(msg, sizeof(msg), "atmos case %zu: 12-row strip == full (swapped)", i);
        check(frames_equal(s_cmp, s_strip, &bx, &by), msg);

        /* Exact swap_bytes equivalence */
        snprintf(msg, sizeof(msg), "atmos case %zu: swap_bytes exact bswap16 equivalence", i);
        check(frames_bswap_equal(s_full, s_cmp), msg);

        /* Pure OLED black outside r > 232 (corners) when starting on black */
        if (!prefill) {
            bool corners_black = (s_full[0] == 0u && s_full[W - 1] == 0u &&
                                  s_full[(size_t)(H - 1) * W] == 0u &&
                                  s_full[(size_t)(H - 1) * W + (W - 1)] == 0u);
            snprintf(msg, sizeof(msg), "atmos case %zu: OLED corners remain true black", i);
            check(corners_black, msg);
        }
    }

    /* Strength == 0 must be a strict no-op */
    jr_g3_atmosphaera_t zero_env = cases[0];
    zero_env.strength = 0;
    fill_pattern(s_ref, false);
    memcpy(s_cmp, s_ref, sizeof(s_ref));
    jr_g3_atmosphaera_rows(s_cmp, 0, H, false, &zero_env);
    check(frames_equal(s_ref, s_cmp, NULL, NULL), "atmos strength=0 modifies 0 pixels");
}

static void test_atmosphaera_safe_zones_and_sun(void)
{
    /* 1. Emblem strict isolation: changing weather_code must NEVER touch ANY pixel
     *    outside [JR_G3_ATMOS_EMBLEM_X0..X1] x [JR_G3_ATMOS_EMBLEM_Y0..Y1], and
     *    specifically ZERO pixels inside the Temp Hero safe zone [96..242] x [142..244]! */
    jr_g3_atmosphaera_t e_clear = {
        .hour = 12, .minute = 0, .weather_code = JR_G3_WX_CLEAR,
        .is_day = true, .temp_f = 76, .rain_pct = 0, .now_ms = 2000, .strength = 255,
    };
    jr_g3_atmosphaera_t e_storm = e_clear;
    e_storm.weather_code = JR_G3_WX_STORM;
    e_storm.rain_pct = 90;

    memset(s_ref, 0, sizeof(s_ref));
    memset(s_cmp, 0, sizeof(s_cmp));
    jr_g3_atmosphaera_rows(s_ref, 0, H, false, &e_clear);
    jr_g3_atmosphaera_rows(s_cmp, 0, H, false, &e_storm);

    int diff_inside_emblem = 0;
    int diff_outside_emblem = 0;
    int diff_in_temp_zone = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (s_ref[(size_t)y * W + (size_t)x] != s_cmp[(size_t)y * W + (size_t)x]) {
                bool in_emblem = (x >= JR_G3_ATMOS_EMBLEM_X0 && x <= JR_G3_ATMOS_EMBLEM_X1 &&
                                  y >= JR_G3_ATMOS_EMBLEM_Y0 && y <= JR_G3_ATMOS_EMBLEM_Y1);
                bool in_temp = (x >= JR_G3_ATMOS_TEMP_X0 && x <= JR_G3_ATMOS_TEMP_X1 &&
                                y >= JR_G3_ATMOS_TEMP_Y0 && y <= JR_G3_ATMOS_TEMP_Y1);
                if (in_emblem) { ++diff_inside_emblem; }
                else { ++diff_outside_emblem; }
                if (in_temp) { ++diff_in_temp_zone; }
            }
        }
    }
    check(diff_inside_emblem > 400, "atmos emblem visibly renders inside [248..344]x[148..232]");
    check(diff_outside_emblem == 0, "atmos emblem leaks 0 pixels outside its clip box");
    check(diff_in_temp_zone == 0, "atmos emblem overlaps 0 pixels of temp hero safe zone");

    /* 2. Planetary horizon clears completely below y = 264:
     *    Changing temp_f from warm (82) to cool (50) tints ONLY the horizon (y >= 266)! */
    jr_g3_atmosphaera_t e_warm = e_clear;
    jr_g3_atmosphaera_t e_cool = e_clear;
    e_warm.temp_f = 82;
    e_cool.temp_f = 50;
    memset(s_ref, 0, sizeof(s_ref));
    memset(s_cmp, 0, sizeof(s_cmp));
    jr_g3_atmosphaera_rows(s_ref, 0, H, false, &e_warm);
    jr_g3_atmosphaera_rows(s_cmp, 0, H, false, &e_cool);

    int hor_diff_above_265 = 0;
    int hor_diff_below_266 = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (s_ref[(size_t)y * W + (size_t)x] != s_cmp[(size_t)y * W + (size_t)x]) {
                if (y <= JR_G3_ATMOS_HORIZON_CLEAR_Y) {
                    ++hor_diff_above_265;
                } else {
                    ++hor_diff_below_266;
                }
            }
        }
    }
    check(hor_diff_above_265 == 0, "atmos horizon clears y <= 264 with 0 pixels");
    check(hor_diff_below_266 > 500, "atmos horizon visibly renders illuminated arc below y >= 266");
    /* Crest at (233, 270) should be notably luminous */
    check(luma565(s_ref[270 * W + 233]) > 140, "atmos horizon crest at (233, 270) is luminous");

    /* 3. Live Sun Jewel cardinal positioning: 6A=left, 12P=top, 6P=right, 12A=bottom */
    struct { uint8_t hr; int ex; int ey; const char *label; } cardinals[4] = {
        {  6,  36, 233, "atmos sun jewel at 6A (9 o'clock)" },
        { 12, 233,  36, "atmos sun jewel at 12P (12 o'clock)" },
        { 18, 430, 233, "atmos sun jewel at 6P (3 o'clock)" },
        {  0, 233, 430, "atmos sun jewel at 12A (6 o'clock)" },
    };
    for (int i = 0; i < 4; ++i) {
        jr_g3_atmosphaera_t ec = e_clear;
        ec.hour = cardinals[i].hr;
        ec.minute = 0;
        memset(s_full, 0, sizeof(s_full));
        jr_g3_atmosphaera_rows(s_full, 0, H, false, &ec);
        int lum = luma565(s_full[(size_t)cardinals[i].ey * W + (size_t)cardinals[i].ex]);
        check(lum > 220, cardinals[i].label);
    }
}

/* ---- 2. CORTEX COMMAND tests --------------------------------------------- */

static void test_cortex_strip_and_swap(void)
{
    jr_g3_cortex_t cases[4];
    memset(cases, 0, sizeof(cases));

    /* Case 0: Nominal telemetry, silent audio -> heartbeat sparkline */
    cases[0].mem_pct = 74;
    cases[0].cpu_pct = 48;
    cases[0].batt_pct = 82;
    cases[0].agent_pct = 65;
    cases[0].charging = false;
    cases[0].codex_active = false;
    cases[0].row_led[0] = JR_G3_LED_GREEN;
    cases[0].row_led[1] = JR_G3_LED_CYAN;
    cases[0].row_led[2] = JR_G3_LED_AMBER;
    cases[0].now_ms = 1800;
    cases[0].strength = 255;

    /* Case 1: Codex active + charging emerald pulse + live 16-band spectrum */
    cases[1].mem_pct = 88;
    cases[1].cpu_pct = 92;
    cases[1].batt_pct = 54;
    cases[1].agent_pct = 78;
    cases[1].charging = true;
    cases[1].codex_active = true;
    cases[1].row_led[0] = JR_G3_LED_VIOLET;
    cases[1].row_led[1] = JR_G3_LED_CYAN;
    cases[1].row_led[2] = JR_G3_LED_GREEN;
    for (int b = 0; b < 16; ++b) {
        cases[1].bands[b] = (uint8_t)(220 - b * 11);
    }
    cases[1].now_ms = 5120;
    cases[1].strength = 255;

    /* Case 2: Low battery alert + edge 0% / 100% gauges */
    cases[2].mem_pct = 100;
    cases[2].cpu_pct = 0;
    cases[2].batt_pct = 12;
    cases[2].agent_pct = 100;
    cases[2].charging = false;
    cases[2].codex_active = false;
    cases[2].row_led[0] = JR_G3_LED_RED;
    cases[2].row_led[1] = JR_G3_LED_AMBER;
    cases[2].row_led[2] = JR_G3_LED_OFF;
    cases[2].now_ms = 9000;
    cases[2].strength = 255;

    /* Case 3: Partial fade over non-black canvas */
    cases[3] = cases[1];
    cases[3].strength = 160;

    for (size_t i = 0; i < 4; ++i) {
        const bool prefill = (cases[i].strength < 255);

        if (prefill) {
            fill_pattern(s_full, false);
            fill_pattern(s_strip, false);
        } else {
            memset(s_full, 0, sizeof(s_full));
            memset(s_strip, 0, sizeof(s_strip));
        }
        jr_g3_cortex_rows(s_full, 0, H, false, &cases[i]);
        render_cortex_strips(s_strip, 16, false, &cases[i]);
        char msg[128];
        snprintf(msg, sizeof(msg), "cortex case %zu: 16-row strip == full (native)", i);
        check(frames_equal(s_full, s_strip, NULL, NULL), msg);

        if (prefill) {
            fill_pattern(s_cmp, true);
            fill_pattern(s_strip, true);
        } else {
            memset(s_cmp, 0, sizeof(s_cmp));
            memset(s_strip, 0, sizeof(s_strip));
        }
        jr_g3_cortex_rows(s_cmp, 0, H, true, &cases[i]);
        render_cortex_strips(s_strip, 12, true, &cases[i]);
        snprintf(msg, sizeof(msg), "cortex case %zu: 12-row strip == full (swapped)", i);
        check(frames_equal(s_cmp, s_strip, NULL, NULL), msg);

        snprintf(msg, sizeof(msg), "cortex case %zu: swap_bytes exact bswap16 equivalence", i);
        check(frames_bswap_equal(s_full, s_cmp), msg);

        if (!prefill) {
            bool corners_black = (s_full[0] == 0u && s_full[W - 1] == 0u &&
                                  s_full[(size_t)(H - 1) * W] == 0u &&
                                  s_full[(size_t)(H - 1) * W + (W - 1)] == 0u);
            snprintf(msg, sizeof(msg), "cortex case %zu: OLED corners remain true black", i);
            check(corners_black, msg);
        }
    }

    /* Strength == 0 must be a strict no-op */
    jr_g3_cortex_t z = cases[0];
    z.strength = 0;
    fill_pattern(s_ref, false);
    memcpy(s_cmp, s_ref, sizeof(s_ref));
    jr_g3_cortex_rows(s_cmp, 0, H, false, &z);
    check(frames_equal(s_ref, s_cmp, NULL, NULL), "cortex strength=0 modifies 0 pixels");
}

static void test_cortex_safe_zones_and_gauges(void)
{
    jr_g3_cortex_t base;
    memset(&base, 0, sizeof(base));
    base.mem_pct = 50;
    base.cpu_pct = 50;
    base.batt_pct = 50;
    base.agent_pct = 50;
    base.row_led[0] = JR_G3_LED_GREEN;
    base.row_led[1] = JR_G3_LED_CYAN;
    base.row_led[2] = JR_G3_LED_VIOLET;
    base.now_ms = 2400;
    base.strength = 255;

    memset(s_ref, 0, sizeof(s_ref));
    jr_g3_cortex_rows(s_ref, 0, H, false, &base);

    /* 1. Sparkline strict bounding box: modifying bands[16] changes ONLY [140..326] x [324..354] */
    jr_g3_cortex_t loud = base;
    for (int i = 0; i < 16; ++i) {
        loud.bands[i] = 240;
    }
    memset(s_cmp, 0, sizeof(s_cmp));
    jr_g3_cortex_rows(s_cmp, 0, H, false, &loud);

    int spark_in = 0, spark_out = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (s_ref[(size_t)y * W + (size_t)x] != s_cmp[(size_t)y * W + (size_t)x]) {
                bool in_box = (x >= JR_G3_CORTEX_SPARK_X0 && x <= JR_G3_CORTEX_SPARK_X1 &&
                               y >= JR_G3_CORTEX_SPARK_Y0 && y <= JR_G3_CORTEX_SPARK_Y1);
                if (in_box) { ++spark_in; }
                else { ++spark_out; }
            }
        }
    }
    check(spark_in > 300, "cortex sparkline visibly responds to live bands[16]");
    check(spark_out == 0, "cortex sparkline leaks 0 pixels outside [140..326]x[324..354]");

    /* 2. Quadrant gauge isolation: changing MEM (Top-Left) touches ONLY top-left outer annulus */
    jr_g3_cortex_t mem_hi = base;
    mem_hi.mem_pct = 100;
    memset(s_cmp, 0, sizeof(s_cmp));
    jr_g3_cortex_rows(s_cmp, 0, H, false, &mem_hi);

    int mem_tl_diff = 0, mem_card_diff = 0, mem_other_q_diff = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (s_ref[(size_t)y * W + (size_t)x] != s_cmp[(size_t)y * W + (size_t)x]) {
                if (x >= JR_G3_CORTEX_CARD_X0 && x <= JR_G3_CORTEX_CARD_X1 &&
                    y >= JR_G3_CORTEX_CARD_Y0 && y <= JR_G3_CORTEX_CARD_Y1) {
                    ++mem_card_diff;
                } else if (x < 233 && y < 233) {
                    ++mem_tl_diff;
                } else {
                    ++mem_other_q_diff;
                }
            }
        }
    }
    check(mem_tl_diff > 500, "cortex MEM gauge visibly illuminates top-left quadrant");
    check(mem_card_diff == 0, "cortex MEM gauge touches 0 pixels inside mission card");
    check(mem_other_q_diff == 0, "cortex MEM gauge touches 0 pixels in other quadrants");

    /* 3. Charging emerald pulse shifts PWR gauge (Bottom-Right) green upward */
    jr_g3_cortex_t chg = base;
    chg.charging = true;
    chg.now_ms = 482; /* near sine peak */
    memset(s_cmp, 0, sizeof(s_cmp));
    jr_g3_cortex_rows(s_cmp, 0, H, false, &chg);
    int br_diff = 0;
    for (int y = 233; y < H; ++y) {
        for (int x = 233; x < W; ++x) {
            if (s_ref[(size_t)y * W + (size_t)x] != s_cmp[(size_t)y * W + (size_t)x]) {
                ++br_diff;
            }
        }
    }
    check(br_diff > 400, "cortex charging flag visibly modulates bottom-right PWR gauge");

    /* 4. Glowing status LEDs at x=128, y=208/242/276 are bright, while text safe zone stays dark */
    check(luma565(s_ref[JR_G3_CORTEX_ROW0_Y * W + JR_G3_CORTEX_LED_X]) > 180,
          "cortex row 0 LED at (128, 208) is luminous");
    check(luma565(s_ref[JR_G3_CORTEX_ROW1_Y * W + JR_G3_CORTEX_LED_X]) > 180,
          "cortex row 1 LED at (128, 242) is luminous");
    check(luma565(s_ref[JR_G3_CORTEX_ROW2_Y * W + JR_G3_CORTEX_LED_X]) > 140,
          "cortex row 2 LED at (128, 276) is luminous");

    /* Text safe zone inside card (x in [148..344], y=208/242/276) must stay dark frosted glass */
    int max_text_bg_luma = 0;
    const int row_ys[3] = { JR_G3_CORTEX_ROW0_Y, JR_G3_CORTEX_ROW1_Y, JR_G3_CORTEX_ROW2_Y };
    for (int r = 0; r < 3; ++r) {
        for (int x = JR_G3_CORTEX_TEXT_X0 + 4; x <= JR_G3_CORTEX_TEXT_X1 - 6; ++x) {
            int l = luma565(s_ref[(size_t)row_ys[r] * W + (size_t)x]);
            if (l > max_text_bg_luma) {
                max_text_bg_luma = l;
            }
        }
    }
    check(max_text_bg_luma < 36, "cortex tabular text safe zone stays dark & unobstructed");
}

static void maybe_dump_specimens(void)
{
    const char *dir = getenv("G3_DUMP_PPM_DIR");
    if (dir == NULL || dir[0] == '\0') {
        return;
    }

    jr_g3_atmosphaera_t a_day = {
        .hour = 13, .minute = 30, .weather_code = JR_G3_WX_PARTLY,
        .is_day = true, .temp_f = 78, .rain_pct = 15, .now_ms = 1200, .strength = 255,
    };
    memset(s_full, 0, sizeof(s_full));
    jr_g3_atmosphaera_rows(s_full, 0, H, false, &a_day);
    dump_ppm(dir, "atmosphaera_day_partly.ppm", s_full);

    jr_g3_atmosphaera_t a_rain = {
        .hour = 21, .minute = 15, .weather_code = JR_G3_WX_STORM,
        .is_day = false, .temp_f = 62, .rain_pct = 90, .now_ms = 4200, .strength = 255,
    };
    memset(s_full, 0, sizeof(s_full));
    jr_g3_atmosphaera_rows(s_full, 0, H, false, &a_rain);
    dump_ppm(dir, "atmosphaera_night_storm.ppm", s_full);

    jr_g3_cortex_t c_codex;
    memset(&c_codex, 0, sizeof(c_codex));
    c_codex.mem_pct = 78;
    c_codex.cpu_pct = 64;
    c_codex.batt_pct = 86;
    c_codex.agent_pct = 72;
    c_codex.charging = true;
    c_codex.codex_active = true;
    c_codex.row_led[0] = JR_G3_LED_VIOLET;
    c_codex.row_led[1] = JR_G3_LED_CYAN;
    c_codex.row_led[2] = JR_G3_LED_GREEN;
    for (int i = 0; i < 16; ++i) {
        c_codex.bands[i] = (uint8_t)(210 - i * 10);
    }
    c_codex.now_ms = 500;
    c_codex.strength = 255;
    memset(s_full, 0, sizeof(s_full));
    jr_g3_cortex_rows(s_full, 0, H, false, &c_codex);
    dump_ppm(dir, "cortex_codex_charging.ppm", s_full);
}

int main(void)
{
    test_atmosphaera_strip_and_swap();
    test_atmosphaera_safe_zones_and_sun();
    test_cortex_strip_and_swap();
    test_cortex_safe_zones_and_gauges();
    maybe_dump_specimens();

    if (g_fails != 0) {
        fprintf(stderr, "glass3_screens: %d failure(s) of %d checks\n", g_fails, g_checks);
        return 1;
    }
    printf("glass3_screens passed (%d checks)\n", g_checks);
    return 0;
}
