/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_glance.c — the rain warning and the morning briefing, pure.
 * Registered from test_core.c's main() like the mood and soak suites.
 */
#include "unity.h"
#include "jr_core/glance.h"
#include <string.h>

/* An afternoon that turns: probabilities per hour from midnight, the real
 * Open-Meteo answer for Fort Lauderdale on 2026-09-05 (36 hours). */
static const uint8_t k_day[JR_RAIN_HOURS] = {
     9,  9, 10,  6,  3,  4,  3,  2,  8, 24, 31, 32,
    13, 11, 16, 22, 42, 41, 40, 57, 68, 58, 50, 32,
    19, 13, 10,  8,  5,  3,  6,  7,  6, 10, 10, 12,
};

static void test_rain_warns_once_for_the_first_wet_hour(void)
{
    jr_rain_warn_t st;
    jr_rain_warn_init(&st);
    /* 17:00 -> the window is 18,19,20 = 40,57,68: hour 20 is 3 h ahead. */
    TEST_ASSERT_EQUAL_INT(3, jr_rain_warning_step(&st, k_day, JR_RAIN_HOURS, 17));
    /* The next ten-minute fetch, same hour: silent. */
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, k_day, JR_RAIN_HOURS, 17));
    /* 18:00, 19:00: still wet ahead, still one warning only. */
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, k_day, JR_RAIN_HOURS, 18));
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, k_day, JR_RAIN_HOURS, 19));
}

static void test_rain_rearms_only_after_the_window_dries(void)
{
    jr_rain_warn_t st;
    jr_rain_warn_init(&st);
    TEST_ASSERT_EQUAL_INT(3, jr_rain_warning_step(&st, k_day, JR_RAIN_HOURS, 17));
    /* 21:00 -> 22,23,24 = 50,32,19: under WARN but not under CLEAR: latched. */
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, k_day, JR_RAIN_HOURS, 21));
    TEST_ASSERT_FALSE(st.armed);
    /* 23:00 -> 24,25,26 = 19,13,10: dry, re-armed, nothing to say. */
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, k_day, JR_RAIN_HOURS, 23));
    TEST_ASSERT_TRUE(st.armed);
    /* A fresh front tomorrow warns again. */
    uint8_t wet[JR_RAIN_HOURS];
    memcpy(wet, k_day, sizeof wet);
    wet[25] = 90;
    TEST_ASSERT_EQUAL_INT(2, jr_rain_warning_step(&st, wet, JR_RAIN_HOURS, 23));
}

static void test_rain_ignores_unknown_hours_and_the_edge_of_the_data(void)
{
    jr_rain_warn_t st;
    jr_rain_warn_init(&st);
    uint8_t pp[JR_RAIN_HOURS];
    memset(pp, JR_RAIN_PP_UNKNOWN, sizeof pp);
    /* All unknown: never warns, never clears, stays armed. */
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, pp, JR_RAIN_HOURS, 12));
    TEST_ASSERT_TRUE(st.armed);
    /* A known dry hour beside unknown ones is still dry. */
    pp[14] = 5;
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, pp, JR_RAIN_HOURS, 12));
    /* Short data: 23:00 with only 24 hours known looks at nothing. */
    memcpy(pp, k_day, sizeof pp);
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, pp, 24, 23));
    /* Garbage in: no crash, no warning. */
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, NULL, JR_RAIN_HOURS, 12));
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, pp, JR_RAIN_HOURS, 24));
    /* A warning already booked does not fire again from a second step with
     * 255 in the window (the latch holds through unknown data). */
    jr_rain_warn_init(&st);
    TEST_ASSERT_EQUAL_INT(3, jr_rain_warning_step(&st, k_day, JR_RAIN_HOURS, 17));
    memset(pp, JR_RAIN_PP_UNKNOWN, sizeof pp);
    TEST_ASSERT_EQUAL_INT(0, jr_rain_warning_step(&st, pp, JR_RAIN_HOURS, 17));
    TEST_ASSERT_FALSE(st.armed);
}

static void test_briefing_is_once_a_morning_on_a_lift(void)
{
    jr_briefing_t st;
    jr_briefing_init(&st);
    /* 04:59 is night; 11:00 is no longer morning; no lift is no briefing. */
    TEST_ASSERT_FALSE(jr_briefing_due(&st, 4, 248, true));
    TEST_ASSERT_FALSE(jr_briefing_due(&st, 11, 248, true));
    TEST_ASSERT_FALSE(jr_briefing_due(&st, 7, 248, false));
    /* The first lift of the morning. */
    TEST_ASSERT_TRUE(jr_briefing_due(&st, 7, 248, true));
    /* The second lift the same morning, and the same day at 10:59. */
    TEST_ASSERT_FALSE(jr_briefing_due(&st, 7, 248, true));
    TEST_ASSERT_FALSE(jr_briefing_due(&st, 10, 248, true));
    /* Tomorrow. */
    TEST_ASSERT_TRUE(jr_briefing_due(&st, 5, 249, true));
    TEST_ASSERT_FALSE(jr_briefing_due(&st, 9, 249, true));
}

static void test_briefing_compose_says_only_what_the_device_holds(void)
{
    char out[512];
    jr_briefing_facts_t f = {
        .have_date = true, .wday = 6, .mday = 5, .mon = 8,
        .have_weather = true, .temp_f = 75, .hi_f = 88, .lo_f = 76,
        .condition = "light drizzle", .rain_in_h = 2,
        .tasks_done = 2, .task_title = "Find the CO5300 brightness registers",
        .battery_pct = 25, .on_usb = false,
    };
    const int n = jr_briefing_compose(out, sizeof out, &f);
    TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof out);
    TEST_ASSERT_NOT_NULL(strstr(out, "Saturday 5 September"));
    TEST_ASSERT_NOT_NULL(strstr(out, "75 degrees and light drizzle, high 88, low 76"));
    TEST_ASSERT_NOT_NULL(strstr(out, "rain likely in 2 hours"));
    TEST_ASSERT_NOT_NULL(strstr(out, "2 delegated tasks finished, the latest: Find the CO5300"));
    TEST_ASSERT_NOT_NULL(strstr(out, "Battery 25 percent"));

    /* Nothing known but the date: no weather sentence, no tasks, no battery
     * line at 80 %, nothing invented. */
    jr_briefing_facts_t bare = { .have_date = true, .wday = 1, .mday = 7, .mon = 8,
                                 .battery_pct = 80 };
    jr_briefing_compose(out, sizeof out, &bare);
    TEST_ASSERT_NOT_NULL(strstr(out, "Monday 7 September"));
    TEST_ASSERT_NULL(strstr(out, "degrees"));
    TEST_ASSERT_NULL(strstr(out, "task"));
    TEST_ASSERT_NULL(strstr(out, "Battery"));
    /* On USB a low cell is not worth a sentence. */
    jr_briefing_facts_t usb = { .battery_pct = 12, .on_usb = true };
    jr_briefing_compose(out, sizeof out, &usb);
    TEST_ASSERT_NULL(strstr(out, "Battery"));
    /* A short buffer is cut, terminated, and reported as such. */
    char tiny[24];
    const int t = jr_briefing_compose(tiny, sizeof tiny, &f);
    TEST_ASSERT_EQUAL_INT((int)sizeof tiny - 1, t);
    TEST_ASSERT_EQUAL_INT((int)sizeof tiny - 1, (int)strlen(tiny));
}

static void test_briefing_caption_fits_the_band_in_shell_glyphs(void)
{
    char cap[64];
    jr_briefing_facts_t f = {
        .have_weather = true, .temp_f = 75, .condition = "Moderate rain showers",
        .tasks_done = 12,
    };
    const int n = jr_briefing_caption(cap, sizeof cap, &f);
    TEST_ASSERT_EQUAL_STRING("GOOD MORNING 75* SHOWERS 12 DONE", cap);
    TEST_ASSERT_TRUE(n <= JR_BRIEF_CAPTION_GLYPHS);
    /* No degree sign ever: the shell prints '*'. */
    TEST_ASSERT_NULL(strchr(cap, (char)0xB0));
    jr_briefing_facts_t none = {0};
    jr_briefing_caption(cap, sizeof cap, &none);
    TEST_ASSERT_EQUAL_STRING("GOOD MORNING", cap);
}

void glance_tests_run(void)
{
    RUN_TEST(test_rain_warns_once_for_the_first_wet_hour);
    RUN_TEST(test_rain_rearms_only_after_the_window_dries);
    RUN_TEST(test_rain_ignores_unknown_hours_and_the_edge_of_the_data);
    RUN_TEST(test_briefing_is_once_a_morning_on_a_lift);
    RUN_TEST(test_briefing_compose_says_only_what_the_device_holds);
    RUN_TEST(test_briefing_caption_fits_the_band_in_shell_glyphs);
}
