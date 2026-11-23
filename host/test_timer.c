/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_timer.c — unit tests for jr_core/timer (pure C, no ESP-IDF).
 */
#include "unity.h"
#include "jr_core/timer.h"
#include <string.h>

static void test_timer_init_and_empty_queries(void)
{
    jr_timer_bank_t b;
    jr_timer_init(&b);
    TEST_ASSERT_FALSE(jr_timer_any_active(&b));
    TEST_ASSERT_FALSE(jr_timer_any_ringing(&b));
    TEST_ASSERT_EQUAL_UINT(0u, jr_timer_active_count(&b));

    jr_timer_view_t v;
    TEST_ASSERT_FALSE(jr_timer_nearest(&b, 1000u, &v));
    TEST_ASSERT_FALSE(v.active);
    TEST_ASSERT_FALSE(v.ringing);
    TEST_ASSERT_EQUAL_UINT(0u, v.rem_s);
    TEST_ASSERT_EQUAL_UINT(0u, v.pct_left);
}

static void test_timer_validation_and_label_normalization(void)
{
    jr_timer_bank_t b;
    jr_timer_init(&b);
    char lbl[JR_TIMER_LABEL_CAP];
    uint32_t sec = 0u;

    /* Too short / too long rejected */
    TEST_ASSERT_FALSE(jr_timer_set(&b, 1000u, 0u, "tea", lbl, sizeof lbl, &sec));
    TEST_ASSERT_FALSE(jr_timer_set(&b, 1000u, 4u, "tea", lbl, sizeof lbl, &sec));
    TEST_ASSERT_FALSE(jr_timer_set(&b, 1000u, 43201u, "long", lbl, sizeof lbl, &sec));

    /* Boundary 5 s with NULL/empty label defaults to TIMER */
    TEST_ASSERT_TRUE(jr_timer_set(&b, 1000u, 5u, NULL, lbl, sizeof lbl, &sec));
    TEST_ASSERT_EQUAL_STRING("TIMER", lbl);
    TEST_ASSERT_EQUAL_UINT32(5u, sec);

    /* Whitespace trim, uppercase, quotes stripped, 16-char cap */
    TEST_ASSERT_TRUE(jr_timer_set(&b, 1000u, 43200u, "  \"pasta   water\" extra long  ",
                                  lbl, sizeof lbl, &sec));
    TEST_ASSERT_EQUAL_STRING("PASTA WATER EXTR", lbl);
    TEST_ASSERT_EQUAL_UINT32(43200u, sec);
}

static void test_timer_pomodoro_focus_and_break_defaults(void)
{
    jr_timer_bank_t b;
    jr_timer_init(&b);
    char lbl[JR_TIMER_LABEL_CAP];
    uint32_t sec = 0u;

    /* "focus" with 0 s -> 25 min (1500 s) */
    TEST_ASSERT_TRUE(jr_timer_set(&b, 5000u, 0u, "focus", lbl, sizeof lbl, &sec));
    TEST_ASSERT_EQUAL_STRING("FOCUS", lbl);
    TEST_ASSERT_EQUAL_UINT32(1500u, sec);

    /* "break" with 0 s -> 5 min (300 s) */
    TEST_ASSERT_TRUE(jr_timer_set(&b, 5000u, 0u, "  Break ", lbl, sizeof lbl, &sec));
    TEST_ASSERT_EQUAL_STRING("BREAK", lbl);
    TEST_ASSERT_EQUAL_UINT32(300u, sec);

    /* Explicit duration overrides Pomodoro default */
    TEST_ASSERT_TRUE(jr_timer_set(&b, 5000u, 600u, "focus", lbl, sizeof lbl, &sec));
    TEST_ASSERT_EQUAL_STRING("FOCUS", lbl);
    TEST_ASSERT_EQUAL_UINT32(600u, sec);

    jr_timer_view_t v;
    TEST_ASSERT_TRUE(jr_timer_nearest(&b, 5000u, &v));
    TEST_ASSERT_EQUAL_STRING("BREAK", v.label);
    TEST_ASSERT_EQUAL_UINT16(300u, v.rem_s);
    TEST_ASSERT_EQUAL_UINT8(100u, v.pct_left);
}

static void test_timer_max_four_concurrent_and_full_bank(void)
{
    jr_timer_bank_t b;
    jr_timer_init(&b);

    TEST_ASSERT_TRUE(jr_timer_set(&b, 0u, 60u, "one", NULL, 0u, NULL));
    TEST_ASSERT_TRUE(jr_timer_set(&b, 0u, 30u, "two", NULL, 0u, NULL));
    TEST_ASSERT_TRUE(jr_timer_set(&b, 0u, 90u, "three", NULL, 0u, NULL));
    TEST_ASSERT_TRUE(jr_timer_set(&b, 0u, 15u, "four", NULL, 0u, NULL));
    TEST_ASSERT_EQUAL_UINT(4u, jr_timer_active_count(&b));
    /* Fifth timer rejected while all 4 slots are occupied */
    TEST_ASSERT_FALSE(jr_timer_set(&b, 0u, 10u, "five", NULL, 0u, NULL));

    /* Nearest is "FOUR" (15 s) */
    jr_timer_view_t v;
    TEST_ASSERT_TRUE(jr_timer_nearest(&b, 5000u, &v));
    TEST_ASSERT_EQUAL_STRING("FOUR", v.label);
    TEST_ASSERT_EQUAL_UINT16(10u, v.rem_s);
    TEST_ASSERT_EQUAL_UINT8(66u, v.pct_left);
}

static void test_timer_expiry_chime_cadence_and_auto_stop(void)
{
    jr_timer_bank_t b;
    jr_timer_init(&b);
    TEST_ASSERT_TRUE(jr_timer_set(&b, 1000u, 10u, "tea", NULL, 0u, NULL));

    jr_timer_events_t ev;
    jr_timer_step(&b, 10999u, &ev);
    TEST_ASSERT_FALSE(ev.expired);
    TEST_ASSERT_FALSE(ev.chime);
    TEST_ASSERT_FALSE(jr_timer_any_ringing(&b));

    /* Expiry at 11000 ms triggers expired + immediate chime */
    jr_timer_step(&b, 11000u, &ev);
    TEST_ASSERT_TRUE(ev.expired);
    TEST_ASSERT_TRUE(ev.chime);
    TEST_ASSERT_EQUAL_STRING("TEA", ev.expired_label);
    TEST_ASSERT_TRUE(jr_timer_any_ringing(&b));

    jr_timer_view_t v;
    TEST_ASSERT_TRUE(jr_timer_nearest(&b, 11500u, &v));
    TEST_ASSERT_TRUE(v.active);
    TEST_ASSERT_TRUE(v.ringing);
    TEST_ASSERT_EQUAL_UINT16(0u, v.rem_s);
    TEST_ASSERT_EQUAL_UINT8(0u, v.pct_left);

    /* No chime before 4 s */
    jr_timer_step(&b, 14999u, &ev);
    TEST_ASSERT_FALSE(ev.expired);
    TEST_ASSERT_FALSE(ev.chime);

    /* Periodic chime at +4 s */
    jr_timer_step(&b, 15000u, &ev);
    TEST_ASSERT_FALSE(ev.expired);
    TEST_ASSERT_TRUE(ev.chime);

    /* Still ringing at +59.999 s */
    jr_timer_step(&b, 11000u + JR_TIMER_RING_AUTO_STOP_MS - 1u, &ev);
    TEST_ASSERT_FALSE(ev.auto_stopped);
    TEST_ASSERT_TRUE(jr_timer_any_ringing(&b));

    /* Auto-stops at +60 s */
    jr_timer_step(&b, 11000u + JR_TIMER_RING_AUTO_STOP_MS, &ev);
    TEST_ASSERT_TRUE(ev.auto_stopped);
    TEST_ASSERT_FALSE(jr_timer_any_ringing(&b));
    TEST_ASSERT_FALSE(jr_timer_any_active(&b));
}

static void test_timer_cancel_by_label_nearest_all_and_dismiss(void)
{
    jr_timer_bank_t b;
    jr_timer_init(&b);
    TEST_ASSERT_TRUE(jr_timer_set(&b, 0u, 10u, "eggs", NULL, 0u, NULL));
    TEST_ASSERT_TRUE(jr_timer_set(&b, 0u, 20u, "tea", NULL, 0u, NULL));
    TEST_ASSERT_TRUE(jr_timer_set(&b, 0u, 30u, "pasta", NULL, 0u, NULL));

    /* Dismiss ringing only clears ringing, leaves running intact */
    jr_timer_events_t ev;
    jr_timer_step(&b, 10000u, &ev);
    TEST_ASSERT_TRUE(ev.expired);
    TEST_ASSERT_EQUAL_UINT(1u, jr_timer_dismiss_ringing(&b));
    TEST_ASSERT_FALSE(jr_timer_any_ringing(&b));
    TEST_ASSERT_EQUAL_UINT(2u, jr_timer_active_count(&b));

    /* Cancel by label (case-insensitive) */
    TEST_ASSERT_EQUAL_UINT(1u, jr_timer_cancel(&b, 11000u, "Pasta"));
    TEST_ASSERT_EQUAL_UINT(1u, jr_timer_active_count(&b));

    /* Cancel nearest when label is NULL/empty */
    TEST_ASSERT_TRUE(jr_timer_set(&b, 11000u, 100u, "cake", NULL, 0u, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, jr_timer_cancel(&b, 12000u, ""));
    jr_timer_view_t v;
    TEST_ASSERT_TRUE(jr_timer_nearest(&b, 12000u, &v));
    TEST_ASSERT_EQUAL_STRING("CAKE", v.label);

    /* Cancel all */
    TEST_ASSERT_TRUE(jr_timer_set(&b, 12000u, 50u, "soup", NULL, 0u, NULL));
    TEST_ASSERT_EQUAL_UINT(2u, jr_timer_cancel(&b, 12000u, "all"));
    TEST_ASSERT_FALSE(jr_timer_any_active(&b));
}

static void test_timer_survives_uint32_clock_wrap(void)
{
    jr_timer_bank_t b;
    jr_timer_init(&b);
    const uint32_t start = 0xFFFFE000u; /* 8192 ms before wrap */
    TEST_ASSERT_TRUE(jr_timer_set(&b, start, 10u, "wrap", NULL, 0u, NULL));

    jr_timer_view_t v;
    TEST_ASSERT_TRUE(jr_timer_nearest(&b, start + 5000u, &v));
    TEST_ASSERT_EQUAL_UINT16(5u, v.rem_s);
    TEST_ASSERT_EQUAL_UINT8(50u, v.pct_left);

    /* Crosses 0x00000000 boundary */
    jr_timer_events_t ev;
    jr_timer_step(&b, start + 9999u, &ev);
    TEST_ASSERT_FALSE(ev.expired);
    jr_timer_step(&b, start + 10000u, &ev);
    TEST_ASSERT_TRUE(ev.expired);
    TEST_ASSERT_TRUE(ev.chime);
    TEST_ASSERT_TRUE(jr_timer_any_ringing(&b));

    /* Chime +4 s after wrap */
    jr_timer_step(&b, start + 14000u, &ev);
    TEST_ASSERT_TRUE(ev.chime);

    /* Auto-stop +60 s after wrap */
    jr_timer_step(&b, start + 70000u, &ev);
    TEST_ASSERT_TRUE(ev.auto_stopped);
    TEST_ASSERT_FALSE(jr_timer_any_active(&b));
}

void timer_tests_run(void)
{
    RUN_TEST(test_timer_init_and_empty_queries);
    RUN_TEST(test_timer_validation_and_label_normalization);
    RUN_TEST(test_timer_pomodoro_focus_and_break_defaults);
    RUN_TEST(test_timer_max_four_concurrent_and_full_bank);
    RUN_TEST(test_timer_expiry_chime_cadence_and_auto_stop);
    RUN_TEST(test_timer_cancel_by_label_nearest_all_and_dismiss);
    RUN_TEST(test_timer_survives_uint32_clock_wrap);
}
