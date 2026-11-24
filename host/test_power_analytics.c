/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_power_analytics.c — Unit tests for jr_power/analytics (pure C11,
 * zero ESP-IDF dependencies).
 */
#include "unity.h"
#include "jr_power/analytics.h"

#include <string.h>

static jr_power_sample_t make_sample(uint32_t uptime_s, int8_t pct, uint16_t mv,
                                     bool usb, bool charging,
                                     int8_t chip_c, int8_t pmic_c)
{
    jr_power_sample_t s = {
        .uptime_s = uptime_s,
        .pct      = pct,
        .mv       = mv,
        .usb      = usb,
        .charging = charging,
        .chip_c   = chip_c,
        .pmic_c   = pmic_c,
    };
    return s;
}

static void test_analytics_init_and_null_safety(void)
{
    jr_power_analytics_ctx_t ctx;
    jr_power_analytics_t out;

    jr_power_analytics_init(&ctx);
    jr_power_analytics_snapshot(&ctx, false, &out);

    TEST_ASSERT_FALSE(out.rate_valid);
    TEST_ASSERT_EQUAL_INT16(0, out.rate_pct_x10_hr);
    TEST_ASSERT_EQUAL_INT16(0, out.mv_per_hr);
    TEST_ASSERT_EQUAL_INT32(-1, out.eta_empty_min);
    TEST_ASSERT_EQUAL_INT32(-1, out.eta_full_min);
    TEST_ASSERT_EQUAL_UINT16(0u, out.session_min_mv);
    TEST_ASSERT_EQUAL_UINT16(0u, out.session_max_mv);
    TEST_ASSERT_EQUAL_UINT16(0u, out.sag_mv);
    TEST_ASSERT_EQUAL_INT8(-128, out.chip_c_max);
    TEST_ASSERT_EQUAL_INT8(-128, out.pmic_c_max);
    TEST_ASSERT_EQUAL_UINT32(0u, out.time_on_bat_s);
    TEST_ASSERT_EQUAL_UINT32(0u, out.time_on_usb_s);
    TEST_ASSERT_EQUAL_UINT16(0u, out.sample_count);

    /* NULL arguments must not crash or corrupt output. */
    jr_power_analytics_init(NULL);
    jr_power_sample_t s = make_sample(10u, 80, 4000u, false, false, 35, 36);
    jr_power_analytics_push(NULL, &s, false);
    jr_power_analytics_push(&ctx, NULL, false);
    jr_power_analytics_snapshot(&ctx, false, NULL);
    jr_power_analytics_snapshot(NULL, false, &out);
    TEST_ASSERT_FALSE(out.rate_valid);
    TEST_ASSERT_EQUAL_INT32(-1, out.eta_empty_min);
    TEST_ASSERT_EQUAL_INT32(-1, out.eta_full_min);
    TEST_ASSERT_EQUAL_INT8(-128, out.pmic_c_max);
}

static void test_analytics_startup_noise_gating(void)
{
    jr_power_analytics_ctx_t ctx;
    jr_power_analytics_t out;
    jr_power_analytics_init(&ctx);

    /* Span < 180s even with big % and mV drop must stay invalid. */
    jr_power_sample_t s0 = make_sample(0u, 80, 4000u, false, false, 35, 38);
    jr_power_sample_t s1 = make_sample(150u, 75, 3950u, false, false, 36, 39);
    jr_power_analytics_push(&ctx, &s0, false);
    jr_power_analytics_push(&ctx, &s1, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_FALSE(out.rate_valid);
    TEST_ASSERT_EQUAL_INT32(-1, out.eta_empty_min);
    TEST_ASSERT_EQUAL_UINT16(2u, out.sample_count);

    /* Span >= 180s but |dpct| < 2 AND |dmv| < 15 must stay invalid. */
    jr_power_analytics_init(&ctx);
    jr_power_sample_t a0 = make_sample(100u, 80, 4000u, false, false, 35, 38);
    jr_power_sample_t a1 = make_sample(280u, 79, 3986u, false, false, 35, 38);
    jr_power_analytics_push(&ctx, &a0, false);
    jr_power_analytics_push(&ctx, &a1, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_FALSE(out.rate_valid);
    TEST_ASSERT_EQUAL_INT32(-1, out.eta_empty_min);

    /* Reaching |dmv| == 15 at span >= 180s unlocks mv_per_hr while keeping
     * eta_empty_min == -1 if dpct == 0. */
    jr_power_analytics_init(&ctx);
    jr_power_sample_t b0 = make_sample(100u, 80, 4000u, false, false, 35, 38);
    jr_power_sample_t b1 = make_sample(280u, 80, 3985u, false, false, 35, 38);
    jr_power_analytics_push(&ctx, &b0, false);
    jr_power_analytics_push(&ctx, &b1, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_TRUE(out.rate_valid);
    TEST_ASSERT_EQUAL_INT16(0, out.rate_pct_x10_hr);
    TEST_ASSERT_EQUAL_INT16(-300, out.mv_per_hr);
    TEST_ASSERT_EQUAL_INT32(-1, out.eta_empty_min);

    /* Reaching |dpct| == 2 at span == 180s unlocks rate & ETA. */
    jr_power_analytics_init(&ctx);
    jr_power_sample_t c0 = make_sample(0u, 80, 4000u, false, false, 35, 38);
    jr_power_sample_t c1 = make_sample(180u, 78, 3995u, false, false, 35, 38);
    jr_power_analytics_push(&ctx, &c0, false);
    jr_power_analytics_push(&ctx, &c1, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_TRUE(out.rate_valid);
    TEST_ASSERT_EQUAL_INT16(-400, out.rate_pct_x10_hr); /* -40.0 %/hr */
    TEST_ASSERT_EQUAL_INT32(117, out.eta_empty_min);   /* 78% / 40%/hr = 117m */
}

static void test_analytics_discharge_rate_and_eta(void)
{
    jr_power_analytics_ctx_t ctx;
    jr_power_analytics_t out;
    jr_power_analytics_init(&ctx);

    /* Linear discharge: 80% -> 75%, 4000 -> 3900 mV over 1800 s (0.5 hr).
     * Expected: -10.0 %/hr (-100), -200 mV/hr, ETA = 75 / 10 * 60 = 450 min. */
    for (uint32_t i = 0; i <= 10u; ++i) {
        const uint32_t t = i * 180u;
        const int8_t pct = (int8_t)(80 - (int)(i / 2u));
        const uint16_t mv = (uint16_t)(4000u - i * 10u);
        jr_power_sample_t s = make_sample(t, pct, mv, false, false, 40, 42);
        jr_power_analytics_push(&ctx, &s, false);
    }

    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_TRUE(out.rate_valid);
    TEST_ASSERT_EQUAL_INT16(-100, out.rate_pct_x10_hr);
    TEST_ASSERT_EQUAL_INT16(-200, out.mv_per_hr);
    TEST_ASSERT_EQUAL_INT32(450, out.eta_empty_min);
    TEST_ASSERT_EQUAL_INT32(-1, out.eta_full_min);
    TEST_ASSERT_EQUAL_UINT32(1800u, out.time_on_bat_s);
    TEST_ASSERT_EQUAL_UINT32(0u, out.time_on_usb_s);
    TEST_ASSERT_EQUAL_UINT16(11u, out.sample_count);

    /* Ultra-slow discharge clamps eta_empty_min to 5999. */
    jr_power_analytics_init(&ctx);
    jr_power_sample_t slow0 = make_sample(0u, 100, 4150u, false, false, 30, 32);
    jr_power_sample_t slow1 = make_sample(144000u, 98, 4120u, false, false, 30, 32);
    jr_power_analytics_push(&ctx, &slow0, false);
    jr_power_analytics_push(&ctx, &slow1, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_TRUE(out.rate_valid);
    TEST_ASSERT_EQUAL_INT32(5999, out.eta_empty_min);

    /* Very fast drain at 1% clamps to 1 min; 0% reports 0 min. */
    jr_power_analytics_init(&ctx);
    jr_power_sample_t fast0 = make_sample(0u, 21, 3500u, false, false, 45, 48);
    jr_power_sample_t fast1 = make_sample(180u, 1, 3300u, false, false, 45, 48);
    jr_power_analytics_push(&ctx, &fast0, false);
    jr_power_analytics_push(&ctx, &fast1, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_TRUE(out.rate_valid);
    TEST_ASSERT_EQUAL_INT32(1, out.eta_empty_min);

    jr_power_sample_t empty = make_sample(210u, 0, 3250u, false, false, 45, 48);
    jr_power_analytics_push(&ctx, &empty, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_EQUAL_INT32(0, out.eta_empty_min);
}

static void test_analytics_charge_care_vs_full_taper(void)
{
    jr_power_analytics_ctx_t ctx;
    jr_power_analytics_t out_care;
    jr_power_analytics_t out_full;

    /* Charging at +60.0 %/hr (+600): 40% -> 50% in 600 s (10 min). */
    jr_power_analytics_init(&ctx);
    jr_power_sample_t s0 = make_sample(0u, 40, 3800u, true, true, 42, 46);
    jr_power_sample_t s1 = make_sample(600u, 50, 3950u, true, true, 44, 49);
    jr_power_analytics_push(&ctx, &s0, true);
    jr_power_analytics_push(&ctx, &s1, true);

    /* Desk Care mode (target = 80%): linear 30% / 60%/hr = 30 min. */
    jr_power_analytics_snapshot(&ctx, true, &out_care);
    TEST_ASSERT_TRUE(out_care.rate_valid);
    TEST_ASSERT_EQUAL_INT16(600, out_care.rate_pct_x10_hr);
    TEST_ASSERT_EQUAL_INT16(900, out_care.mv_per_hr);
    TEST_ASSERT_EQUAL_INT32(-1, out_care.eta_empty_min);
    TEST_ASSERT_EQUAL_INT32(30, out_care.eta_full_min);
    TEST_ASSERT_EQUAL_UINT16(0u, out_care.sag_mv);

    /* Travel mode (target = 100%, CV taper ~1.15x above 80%):
     * 30% CC + 20% * 1.15 CV = 53% effective -> 53 min (> linear 50 min). */
    jr_power_analytics_snapshot(&ctx, false, &out_full);
    TEST_ASSERT_TRUE(out_full.rate_valid);
    TEST_ASSERT_EQUAL_INT32(53, out_full.eta_full_min);
    TEST_ASSERT_TRUE(out_full.eta_full_min > 50);

    /* At 85% while charging: Desk Care reports 0 min (already >=80%),
     * while Travel mode applies 1.15x taper to remaining 15% -> 17 min. */
    jr_power_analytics_init(&ctx);
    jr_power_sample_t h0 = make_sample(0u, 75, 4020u, true, true, 42, 46);
    jr_power_sample_t h1 = make_sample(600u, 85, 4120u, true, true, 43, 47);
    jr_power_analytics_push(&ctx, &h0, false);
    jr_power_analytics_push(&ctx, &h1, false);

    jr_power_analytics_snapshot(&ctx, true, &out_care);
    TEST_ASSERT_EQUAL_INT32(0, out_care.eta_full_min);

    jr_power_analytics_snapshot(&ctx, false, &out_full);
    TEST_ASSERT_EQUAL_INT32(17, out_full.eta_full_min);

    /* At 96% (>= 95%), no extra taper multiplier is added (already in CV). */
    jr_power_analytics_init(&ctx);
    jr_power_sample_t cv0 = make_sample(0u, 94, 4180u, true, true, 40, 43);
    jr_power_sample_t cv1 = make_sample(360u, 96, 4195u, true, true, 40, 43);
    jr_power_analytics_push(&ctx, &cv0, false);
    jr_power_analytics_push(&ctx, &cv1, false);
    jr_power_analytics_snapshot(&ctx, false, &out_full);
    TEST_ASSERT_EQUAL_INT16(200, out_full.rate_pct_x10_hr); /* +20.0 %/hr */
    TEST_ASSERT_EQUAL_INT32(12, out_full.eta_full_min);     /* 4% / 20%/hr = 12m */

    /* On USB but NOT charging (Care hold / complete): both ETAs must be -1. */
    jr_power_analytics_init(&ctx);
    jr_power_sample_t hold0 = make_sample(0u, 80, 4010u, true, false, 38, 40);
    jr_power_sample_t hold1 = make_sample(300u, 82, 4030u, true, false, 38, 40);
    jr_power_analytics_push(&ctx, &hold0, true);
    jr_power_analytics_push(&ctx, &hold1, true);
    jr_power_analytics_snapshot(&ctx, true, &out_care);
    TEST_ASSERT_EQUAL_INT32(-1, out_care.eta_empty_min);
    TEST_ASSERT_EQUAL_INT32(-1, out_care.eta_full_min);
}

static void test_analytics_edge_resets_preserve_cumulative_stats(void)
{
    jr_power_analytics_ctx_t ctx;
    jr_power_analytics_t out;
    jr_power_analytics_init(&ctx);

    /* Phase 1: 30 min on battery with high temps (chip=49, pmic=53). */
    jr_power_sample_t b0 = make_sample(0u, 80, 4000u, false, false, 45, 50);
    jr_power_sample_t b1 = make_sample(1800u, 75, 3900u, false, false, 49, 53);
    jr_power_analytics_push(&ctx, &b0, false);
    jr_power_analytics_push(&ctx, &b1, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_TRUE(out.rate_valid);
    TEST_ASSERT_EQUAL_UINT32(1800u, out.time_on_bat_s);
    TEST_ASSERT_EQUAL_UINT32(0u, out.time_on_usb_s);

    /* Phase 2: Plug into USB at t=1830 -> rolling window resets, cumulative
     * battery time (+30s) and peak temps survive! */
    jr_power_sample_t u0 = make_sample(1830u, 75, 4180u, true, true, 40, 44);
    jr_power_analytics_push(&ctx, &u0, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_FALSE(out.rate_valid);
    TEST_ASSERT_EQUAL_UINT16(1u, out.sample_count);
    TEST_ASSERT_EQUAL_UINT32(1830u, out.time_on_bat_s);
    TEST_ASSERT_EQUAL_UINT32(0u, out.time_on_usb_s);
    TEST_ASSERT_EQUAL_INT8(49, out.chip_c_max);
    TEST_ASSERT_EQUAL_INT8(53, out.pmic_c_max);
    TEST_ASSERT_EQUAL_UINT16(0u, out.sag_mv);

    /* Continue charging 10 min on USB. */
    jr_power_sample_t u1 = make_sample(2430u, 85, 4200u, true, true, 41, 45);
    jr_power_analytics_push(&ctx, &u1, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_TRUE(out.rate_valid);
    TEST_ASSERT_EQUAL_INT16(600, out.rate_pct_x10_hr);
    TEST_ASSERT_EQUAL_UINT32(600u, out.time_on_usb_s);

    /* Phase 3: Toggle care_mode while on USB -> window resets, USB time kept. */
    jr_power_sample_t u2 = make_sample(2460u, 85, 4200u, true, true, 41, 45);
    jr_power_analytics_push(&ctx, &u2, true);
    jr_power_analytics_snapshot(&ctx, true, &out);
    TEST_ASSERT_FALSE(out.rate_valid);
    TEST_ASSERT_EQUAL_UINT16(1u, out.sample_count);
    TEST_ASSERT_EQUAL_UINT32(630u, out.time_on_usb_s);

    /* Phase 4: Charging stops (e.g. Care hold) -> window resets again. */
    jr_power_sample_t u3 = make_sample(2490u, 85, 4000u, true, false, 40, 43);
    jr_power_analytics_push(&ctx, &u3, true);
    jr_power_analytics_snapshot(&ctx, true, &out);
    TEST_ASSERT_FALSE(out.rate_valid);
    TEST_ASSERT_EQUAL_UINT16(1u, out.sample_count);
    TEST_ASSERT_EQUAL_UINT32(660u, out.time_on_usb_s);

    /* Phase 5: Unplug USB -> battery session_min/max reset cleanly (4200 mV
     * from USB must NOT inflate battery sag_mv!). */
    jr_power_sample_t d0 = make_sample(2520u, 85, 3960u, false, false, 39, 41);
    jr_power_sample_t d1 = make_sample(2550u, 85, 3920u, false, false, 39, 41);
    jr_power_analytics_push(&ctx, &d0, true);
    jr_power_analytics_push(&ctx, &d1, true);
    jr_power_analytics_snapshot(&ctx, true, &out);
    TEST_ASSERT_EQUAL_UINT16(3920u, out.session_min_mv);
    TEST_ASSERT_EQUAL_UINT16(3960u, out.session_max_mv);
    TEST_ASSERT_EQUAL_UINT16(40u, out.sag_mv);
    TEST_ASSERT_EQUAL_UINT32(1860u, out.time_on_bat_s);
    TEST_ASSERT_EQUAL_UINT32(690u, out.time_on_usb_s);
    TEST_ASSERT_EQUAL_INT8(49, out.chip_c_max);
    TEST_ASSERT_EQUAL_INT8(53, out.pmic_c_max);
}

static void test_analytics_ring_wrap_beyond_capacity(void)
{
    jr_power_analytics_ctx_t ctx;
    jr_power_analytics_t out;
    jr_power_analytics_init(&ctx);

    /* Push 90 samples (capacity = 60) at 30 s cadence:
     * - Samples 0..29 (15 min): flat 90% (0 %/hr) — all evicted once 90 pushed.
     * - Samples 30..89 (59 intervals = 1770 s): drops 90% -> 80% (-10%).
     * Expected rate from rolling window [30..89]: -10 * 36000 / 1770 = -203. */
    for (uint32_t i = 0; i < 30u; ++i) {
        jr_power_sample_t s = make_sample(i * 30u, 90, 4050u, false, false, 38, 40);
        jr_power_analytics_push(&ctx, &s, false);
    }
    for (uint32_t i = 0; i < JR_POWER_ANALYTICS_CAP; ++i) {
        const uint32_t idx = 30u + i;
        const int8_t pct = (int8_t)(90 - (int)((i * 10u) / (JR_POWER_ANALYTICS_CAP - 1u)));
        const uint16_t mv = (uint16_t)(4050u - i * 2u);
        jr_power_sample_t s = make_sample(idx * 30u, pct, mv, false, false, 38, 40);
        jr_power_analytics_push(&ctx, &s, false);
    }

    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_EQUAL_UINT16(JR_POWER_ANALYTICS_CAP, out.sample_count);
    TEST_ASSERT_TRUE(out.rate_valid);
    /* Exact rolling rate across evicted-clean window: -10 * 36000 / 1770 = -203 */
    TEST_ASSERT_EQUAL_INT16(-203, out.rate_pct_x10_hr);
    /* Cumulative battery time spans all 89 intervals = 2670 s. */
    TEST_ASSERT_EQUAL_UINT32(89u * 30u, out.time_on_bat_s);
    TEST_ASSERT_EQUAL_UINT16(4050u - (JR_POWER_ANALYTICS_CAP - 1u) * 2u, out.session_min_mv);
    TEST_ASSERT_EQUAL_UINT16(4050u, out.session_max_mv);
    TEST_ASSERT_EQUAL_UINT16((JR_POWER_ANALYTICS_CAP - 1u) * 2u, out.sag_mv);
}

static void test_analytics_sag_mv_and_zero_voltage_guard(void)
{
    jr_power_analytics_ctx_t ctx;
    jr_power_analytics_t out;
    jr_power_analytics_init(&ctx);

    /* Single battery sample: sag_mv must be 0 (< 2 samples). */
    jr_power_sample_t s0 = make_sample(0u, 70, 4010u, false, false, 36, 39);
    jr_power_analytics_push(&ctx, &s0, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_EQUAL_UINT16(4010u, out.session_min_mv);
    TEST_ASSERT_EQUAL_UINT16(4010u, out.session_max_mv);
    TEST_ASSERT_EQUAL_UINT16(0u, out.sag_mv);

    /* Zero mV reading must NOT drag session_min_mv down to 0. */
    jr_power_sample_t bad = make_sample(30u, 70, 0u, false, false, 36, 39);
    jr_power_analytics_push(&ctx, &bad, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_EQUAL_UINT16(4010u, out.session_min_mv);
    TEST_ASSERT_EQUAL_UINT16(4010u, out.session_max_mv);
    TEST_ASSERT_EQUAL_UINT16(0u, out.sag_mv);

    /* Load dip to 3875 mV then partial recovery to 3950 mV -> sag = 135 mV. */
    jr_power_sample_t dip = make_sample(60u, 69, 3875u, false, false, 41, 44);
    jr_power_sample_t rec = make_sample(90u, 69, 3950u, false, false, 39, 42);
    jr_power_analytics_push(&ctx, &dip, false);
    jr_power_analytics_push(&ctx, &rec, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_EQUAL_UINT16(3875u, out.session_min_mv);
    TEST_ASSERT_EQUAL_UINT16(4010u, out.session_max_mv);
    TEST_ASSERT_EQUAL_UINT16(135u, out.sag_mv);
}

static void test_analytics_invalid_pmic_temp_ignored(void)
{
    jr_power_analytics_ctx_t ctx;
    jr_power_analytics_t out;
    jr_power_analytics_init(&ctx);

    /* Only invalid (-128) pmic temps -> pmic_c_max remains -128. */
    jr_power_sample_t s0 = make_sample(0u, 80, 4000u, false, false, 37, -128);
    jr_power_sample_t s1 = make_sample(30u, 80, 3995u, false, false, -128, -128);
    jr_power_analytics_push(&ctx, &s0, false);
    jr_power_analytics_push(&ctx, &s1, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_EQUAL_INT8(37, out.chip_c_max);
    TEST_ASSERT_EQUAL_INT8(-128, out.pmic_c_max);

    /* Valid negative pmic temp replaces -128 sentinel cleanly. */
    jr_power_sample_t cold = make_sample(60u, 80, 3990u, false, false, 35, -5);
    jr_power_analytics_push(&ctx, &cold, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_EQUAL_INT8(-5, out.pmic_c_max);

    /* Higher valid pmic temp updates peak; later -128 does not overwrite it. */
    jr_power_sample_t warm = make_sample(90u, 79, 3985u, false, false, 52, 57);
    jr_power_sample_t inv  = make_sample(120u, 79, 3980u, false, false, 40, -128);
    jr_power_analytics_push(&ctx, &warm, false);
    jr_power_analytics_push(&ctx, &inv, false);
    jr_power_analytics_snapshot(&ctx, false, &out);
    TEST_ASSERT_EQUAL_INT8(52, out.chip_c_max);
    TEST_ASSERT_EQUAL_INT8(57, out.pmic_c_max);
}

void power_analytics_tests_run(void)
{
    RUN_TEST(test_analytics_init_and_null_safety);
    RUN_TEST(test_analytics_startup_noise_gating);
    RUN_TEST(test_analytics_discharge_rate_and_eta);
    RUN_TEST(test_analytics_charge_care_vs_full_taper);
    RUN_TEST(test_analytics_edge_resets_preserve_cumulative_stats);
    RUN_TEST(test_analytics_ring_wrap_beyond_capacity);
    RUN_TEST(test_analytics_sag_mv_and_zero_voltage_guard);
    RUN_TEST(test_analytics_invalid_pmic_temp_ignored);
}
