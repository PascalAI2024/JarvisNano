/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_mic_watch.c - the dead-microphone detector (PLAN N16.10). Pure:
 * frames are fed at the 32 ms capture cadence with a made-up clock.
 */
#include "unity.h"
#include "jr_core/mic_watch.h"

#define FRAME_MS 32u
#define DEAD_RMS 0.7f      /* what the dead lane read on the device */
#define LIVE_RMS 12.0f     /* a live room reads 8 to 28 */

/* Feed `ms` of constant level; returns how many reopens were requested. */
static int feed(jr_mic_watch_t *w, uint32_t *now, uint32_t ms, float rms,
                bool adc_open, bool meant_open)
{
    int fired = 0;
    for (uint32_t t = 0; t < ms; t += FRAME_MS) {
        *now += FRAME_MS;
        if (jr_mic_watch_frame(w, *now, rms, adc_open, meant_open)) fired++;
    }
    return fired;
}

static void test_mic_watch_dead_lane_fires_once_after_the_window(void)
{
    jr_mic_watch_t w;
    uint32_t now = 100000u;
    jr_mic_watch_init(&w);
    TEST_ASSERT_EQUAL_INT(0, feed(&w, &now, JR_MIC_DEAD_MS - 2u * FRAME_MS,
                                  DEAD_RMS, true, true));
    TEST_ASSERT_FALSE(w.dead);
    TEST_ASSERT_EQUAL_INT(1, feed(&w, &now, 4u * FRAME_MS, DEAD_RMS, true, true));
    TEST_ASSERT_TRUE(w.dead);
    TEST_ASSERT_EQUAL_UINT32(1u, w.recoveries);
}

static void test_mic_watch_a_live_room_never_fires(void)
{
    jr_mic_watch_t w;
    uint32_t now = 5u;
    jr_mic_watch_init(&w);
    TEST_ASSERT_EQUAL_INT(0, feed(&w, &now, 120000u, LIVE_RMS, true, true));
    TEST_ASSERT_FALSE(w.dead);
    TEST_ASSERT_EQUAL_UINT32(0u, w.recoveries);
    /* The quietest live room read 8: still above electrical silence. */
    TEST_ASSERT_EQUAL_INT(0, feed(&w, &now, 60000u, 8.0f, true, true));
}

static void test_mic_watch_a_live_frame_restarts_the_window(void)
{
    jr_mic_watch_t w;
    uint32_t now = 0u;
    jr_mic_watch_init(&w);
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_INT(0, feed(&w, &now, JR_MIC_DEAD_MS - 3u * FRAME_MS,
                                      DEAD_RMS, true, true));
        TEST_ASSERT_EQUAL_INT(0, feed(&w, &now, FRAME_MS, LIVE_RMS, true, true));
    }
    TEST_ASSERT_EQUAL_UINT32(0u, w.recoveries);
}

static void test_mic_watch_never_judges_a_muted_or_sleeping_microphone(void)
{
    jr_mic_watch_t w;
    uint32_t now = 0u;
    jr_mic_watch_init(&w);
    /* Muted / asleep: the microphone is not meant to be open. */
    TEST_ASSERT_EQUAL_INT(0, feed(&w, &now, 60000u, 0.0f, true, false));
    TEST_ASSERT_FALSE(w.dead);
    /* ADC closed. */
    TEST_ASSERT_EQUAL_INT(0, feed(&w, &now, 60000u, 0.0f, false, true));
    TEST_ASSERT_FALSE(w.dead);
    TEST_ASSERT_EQUAL_UINT32(0u, w.recoveries);
    /* Unmuting does not inherit the silence of the muted time. */
    TEST_ASSERT_EQUAL_INT(0, feed(&w, &now, JR_MIC_DEAD_MS - 2u * FRAME_MS,
                                  0.0f, true, true));
    TEST_ASSERT_EQUAL_UINT32(0u, w.recoveries);
}

static void test_mic_watch_a_mute_mid_window_clears_it(void)
{
    jr_mic_watch_t w;
    uint32_t now = 0u;
    jr_mic_watch_init(&w);
    (void)feed(&w, &now, JR_MIC_DEAD_MS - 2u * FRAME_MS, DEAD_RMS, true, true);
    (void)feed(&w, &now, FRAME_MS, DEAD_RMS, true, false);   /* muted */
    TEST_ASSERT_EQUAL_INT(0, feed(&w, &now, JR_MIC_DEAD_MS - 2u * FRAME_MS,
                                  DEAD_RMS, true, true));
    TEST_ASSERT_EQUAL_UINT32(0u, w.recoveries);
}

static void test_mic_watch_backoff_doubles_and_caps_so_it_cannot_loop(void)
{
    jr_mic_watch_t w;
    uint32_t now = 1000u;
    jr_mic_watch_init(&w);
    /* A lane that never comes back: the reopen does not help. */
    uint32_t fired_at[8];
    int n = 0;
    uint32_t end = now + 400000u;
    while (now < end && n < 8) {
        now += FRAME_MS;
        if (jr_mic_watch_frame(&w, now, DEAD_RMS, true, true)) fired_at[n++] = now;
    }
    TEST_ASSERT_TRUE(n >= 5);
    TEST_ASSERT_TRUE(fired_at[1] - fired_at[0] >= JR_MIC_BACKOFF_MIN_MS);
    TEST_ASSERT_TRUE(fired_at[2] - fired_at[1] >= 2u * JR_MIC_BACKOFF_MIN_MS);
    TEST_ASSERT_TRUE(fired_at[3] - fired_at[2] >= 4u * JR_MIC_BACKOFF_MIN_MS);
    for (int i = 4; i < n; ++i) {
        TEST_ASSERT_TRUE(fired_at[i] - fired_at[i - 1] >= JR_MIC_BACKOFF_MAX_MS);
        TEST_ASSERT_TRUE(fired_at[i] - fired_at[i - 1] <
                         JR_MIC_BACKOFF_MAX_MS + JR_MIC_DEAD_MS + FRAME_MS);
    }
    TEST_ASSERT_TRUE(w.dead);
}

static void test_mic_watch_a_recovered_lane_clears_dead_and_resets_the_backoff(void)
{
    jr_mic_watch_t w;
    uint32_t now = 0u;
    jr_mic_watch_init(&w);
    TEST_ASSERT_EQUAL_INT(1, feed(&w, &now, JR_MIC_DEAD_MS + 2u * FRAME_MS,
                                  DEAD_RMS, true, true));
    TEST_ASSERT_TRUE(w.dead);
    (void)feed(&w, &now, FRAME_MS, LIVE_RMS, true, true);   /* reopen worked */
    TEST_ASSERT_FALSE(w.dead);
    TEST_ASSERT_EQUAL_UINT32(1u, w.recoveries);
    /* Dies again soon after: the gap is the minimum again, not doubled. */
    uint32_t t0 = now;
    int fired = 0;
    while (!fired && now - t0 < 60000u) {
        now += FRAME_MS;
        fired = jr_mic_watch_frame(&w, now, DEAD_RMS, true, true) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT(1, fired);
    TEST_ASSERT_EQUAL_UINT32(2u, w.recoveries);
}

static void test_mic_watch_the_clock_wraps(void)
{
    jr_mic_watch_t w;
    uint32_t now = 0xFFFFFFF0u - 2000u;
    jr_mic_watch_init(&w);
    TEST_ASSERT_EQUAL_INT(1, feed(&w, &now, JR_MIC_DEAD_MS + 2u * FRAME_MS,
                                  DEAD_RMS, true, true));
}

/* Meant-open as the capture caller computes it. */
static int feed_state(jr_mic_watch_t *w, uint32_t *now, uint32_t ms, float rms,
                      bool sleeping, bool speaking)
{
    return feed(w, now, ms, rms, true,
                jr_mic_watch_meant_open(true, sleeping, speaking));
}

static void test_mic_watch_meant_open_truth_table(void)
{
    TEST_ASSERT_TRUE(jr_mic_watch_meant_open(true, false, false));
    TEST_ASSERT_FALSE(jr_mic_watch_meant_open(false, false, false));   /* muted */
    TEST_ASSERT_FALSE(jr_mic_watch_meant_open(true, true, false));     /* asleep */
    TEST_ASSERT_FALSE(jr_mic_watch_meant_open(true, false, true));     /* speaking */
}

/* At the 6 dB speaking gain a pause inside a reply reads under 3.0 on a
 * healthy lane (rms 2.0): that must never request a reopen. */
static void test_mic_watch_a_quiet_pause_inside_a_reply_is_not_judged(void)
{
    jr_mic_watch_t w;
    uint32_t now = 0u;
    jr_mic_watch_init(&w);
    TEST_ASSERT_EQUAL_INT(0, feed_state(&w, &now, 3u * JR_MIC_DEAD_MS, 2.0f,
                                        false, true));
    TEST_ASSERT_FALSE(w.dead);
    TEST_ASSERT_EQUAL_UINT32(0u, w.recoveries);
}

/* Speaking does not blind it for good: the window starts when the reply ends,
 * and a lane that is still dead after the reply is still found. */
static void test_mic_watch_a_dead_lane_is_found_once_the_reply_ends(void)
{
    jr_mic_watch_t w;
    uint32_t now = 0u;
    jr_mic_watch_init(&w);
    TEST_ASSERT_EQUAL_INT(0, feed_state(&w, &now, 2u * JR_MIC_DEAD_MS, DEAD_RMS,
                                        false, true));
    /* The reply ended: the speaking time does not count towards the window. */
    TEST_ASSERT_EQUAL_INT(0, feed_state(&w, &now, JR_MIC_DEAD_MS - 2u * FRAME_MS,
                                        DEAD_RMS, false, false));
    TEST_ASSERT_EQUAL_INT(1, feed_state(&w, &now, 4u * FRAME_MS, DEAD_RMS,
                                        false, false));
    TEST_ASSERT_TRUE(w.dead);
}

void mic_watch_tests_run(void)
{
    RUN_TEST(test_mic_watch_meant_open_truth_table);
    RUN_TEST(test_mic_watch_a_quiet_pause_inside_a_reply_is_not_judged);
    RUN_TEST(test_mic_watch_a_dead_lane_is_found_once_the_reply_ends);
    RUN_TEST(test_mic_watch_dead_lane_fires_once_after_the_window);
    RUN_TEST(test_mic_watch_a_live_room_never_fires);
    RUN_TEST(test_mic_watch_a_live_frame_restarts_the_window);
    RUN_TEST(test_mic_watch_never_judges_a_muted_or_sleeping_microphone);
    RUN_TEST(test_mic_watch_a_mute_mid_window_clears_it);
    RUN_TEST(test_mic_watch_backoff_doubles_and_caps_so_it_cannot_loop);
    RUN_TEST(test_mic_watch_a_recovered_lane_clears_dead_and_resets_the_backoff);
    RUN_TEST(test_mic_watch_the_clock_wraps);
}
