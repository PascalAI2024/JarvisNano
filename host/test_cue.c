/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the interface cues: what a tone may do to the mute state.
 * No ESP-IDF.
 */
#include "unity.h"
#include "jr_core/dac_mute.h"

/* Idle device: DAC muted by the last session end, owner has NOT muted. The
 * tone must be audible, and the DAC must be muted again once it has played. */
static void test_cue_on_idle_muted_dac_plays_then_restores_mute(void)
{
    jr_dac_mute_t m = { .muted = true, .restore = false };
    TEST_ASSERT_TRUE(jr_dac_mute_cue_begin(&m, false));
    TEST_ASSERT_FALSE_MESSAGE(m.muted, "the DAC is open while the tone plays");
    TEST_ASSERT_FALSE_MESSAGE(jr_dac_mute_cue_settle(&m, true),
                              "no restore while samples are still sounding");
    TEST_ASSERT_FALSE(m.muted);
    TEST_ASSERT_TRUE(jr_dac_mute_cue_settle(&m, false));
    TEST_ASSERT_TRUE_MESSAGE(m.muted, "a cue must not leave the DAC unmuted");
    TEST_ASSERT_FALSE(m.restore);
}

/* The owner has muted: the cue is refused and nothing moves. */
static void test_cue_while_owner_muted_is_silent_and_stays_muted(void)
{
    jr_dac_mute_t m = { .muted = true, .restore = false };
    TEST_ASSERT_FALSE(jr_dac_mute_cue_begin(&m, true));
    TEST_ASSERT_TRUE(m.muted);
    TEST_ASSERT_FALSE(m.restore);
    TEST_ASSERT_FALSE(jr_dac_mute_cue_settle(&m, false));
    TEST_ASSERT_TRUE(m.muted);
}

/* Unmuted device (live session): the cue plays and the DAC stays unmuted. */
static void test_cue_on_unmuted_dac_plays_and_stays_unmuted(void)
{
    jr_dac_mute_t m = { .muted = false, .restore = false };
    TEST_ASSERT_TRUE(jr_dac_mute_cue_begin(&m, false));
    TEST_ASSERT_FALSE(m.restore);
    TEST_ASSERT_FALSE(jr_dac_mute_cue_settle(&m, false));
    TEST_ASSERT_FALSE_MESSAGE(m.muted, "a cue must not mute a live session");
}

/* A session starts while the tone is sounding: its unmute is real. */
static void test_real_unmute_during_cue_cancels_the_restore(void)
{
    jr_dac_mute_t m = { .muted = true, .restore = false };
    TEST_ASSERT_TRUE(jr_dac_mute_cue_begin(&m, false));
    jr_dac_mute_unmute(&m);
    TEST_ASSERT_FALSE(jr_dac_mute_cue_settle(&m, false));
    TEST_ASSERT_FALSE(m.muted);
}

/* A fast-kill during the tone: muted, and no stale restore flag left behind. */
static void test_real_mute_during_cue_leaves_no_stale_restore(void)
{
    jr_dac_mute_t m = { .muted = true, .restore = false };
    TEST_ASSERT_TRUE(jr_dac_mute_cue_begin(&m, false));
    jr_dac_mute_mute(&m);
    TEST_ASSERT_TRUE(m.muted);
    TEST_ASSERT_FALSE(m.restore);
    jr_dac_mute_unmute(&m);   /* later real unmute stays unmuted */
    TEST_ASSERT_FALSE(jr_dac_mute_cue_settle(&m, false));
    TEST_ASSERT_FALSE(m.muted);
}

/* Two cues back to back on a muted DAC: the second must not record the
 * first's temporary unmute as the owner's state. */
static void test_second_cue_keeps_the_original_mute_owed(void)
{
    jr_dac_mute_t m = { .muted = true, .restore = false };
    TEST_ASSERT_TRUE(jr_dac_mute_cue_begin(&m, false));
    TEST_ASSERT_TRUE(jr_dac_mute_cue_begin(&m, false));
    TEST_ASSERT_TRUE_MESSAGE(m.restore,
        "the debt survives a second begin (jr_audio drops overlapping cues, "
        "so this is belt and braces)");
    TEST_ASSERT_TRUE(jr_dac_mute_cue_settle(&m, false));
    TEST_ASSERT_TRUE(m.muted);
}

void cue_tests_run(void)
{
    RUN_TEST(test_cue_on_idle_muted_dac_plays_then_restores_mute);
    RUN_TEST(test_cue_while_owner_muted_is_silent_and_stays_muted);
    RUN_TEST(test_cue_on_unmuted_dac_plays_and_stays_unmuted);
    RUN_TEST(test_real_unmute_during_cue_cancels_the_restore);
    RUN_TEST(test_real_mute_during_cue_leaves_no_stale_restore);
    RUN_TEST(test_second_cue_keeps_the_original_mute_owed);
}
