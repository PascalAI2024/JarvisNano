/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_pace.c - when the voice loop may skip its tick sleep.
 */
#include "unity.h"
#include "jr_core/pace.h"

static void test_pace_on_time_loop_sleeps(void)
{
    /* The codec read blocked for most of the frame; little work besides. */
    TEST_ASSERT_FALSE(jr_pace_skip_sleep(18000u, 14000u, 0u));
}

static void test_pace_instant_read_skips(void)
{
    TEST_ASSERT_TRUE(jr_pace_skip_sleep(300u, 20000u, 0u));
    TEST_ASSERT_TRUE(jr_pace_skip_sleep(JR_PACE_NO_WAIT_US - 1u, 0u, 0u));
    TEST_ASSERT_FALSE(jr_pace_skip_sleep(JR_PACE_NO_WAIT_US, 1000u, 0u));
}

static void test_pace_long_work_skips_even_with_a_slow_read(void)
{
    TEST_ASSERT_TRUE(jr_pace_skip_sleep(5000u, JR_PACE_FRAME_US, 0u));
    TEST_ASSERT_FALSE(jr_pace_skip_sleep(5000u, JR_PACE_FRAME_US - 1u, 0u));
}

static void test_pace_unknown_read_judges_on_work_alone(void)
{
    TEST_ASSERT_FALSE(jr_pace_skip_sleep(JR_PACE_READ_UNKNOWN, 100u, 0u));
    TEST_ASSERT_TRUE(jr_pace_skip_sleep(JR_PACE_READ_UNKNOWN,
                                        JR_PACE_FRAME_US + 5u, 0u));
}

static void test_pace_never_skips_more_than_the_bound_in_a_row(void)
{
    /* Permanently behind: skip, skip, skip, sleep, and the cycle repeats. */
    uint32_t skips = 0;
    uint32_t slept = 0;
    uint32_t longest = 0;
    for (int i = 0; i < 400; ++i) {
        if (jr_pace_skip_sleep(0u, 50000u, skips)) {
            ++skips;
            if (skips > longest) longest = skips;
        } else {
            skips = 0;
            ++slept;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(JR_PACE_MAX_SKIPS, longest);
    TEST_ASSERT_EQUAL_UINT32(100u, slept);          /* every fourth frame */
}

void pace_tests_run(void)
{
    RUN_TEST(test_pace_on_time_loop_sleeps);
    RUN_TEST(test_pace_instant_read_skips);
    RUN_TEST(test_pace_long_work_skips_even_with_a_slow_read);
    RUN_TEST(test_pace_unknown_read_judges_on_work_alone);
    RUN_TEST(test_pace_never_skips_more_than_the_bound_in_a_row);
}
