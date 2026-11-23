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

/* Every tone that existed before the interface cues sounds under the owner
 * mute, and hands the DAC back muted; the owner mute is never touched. */
static void test_every_old_tone_plays_under_owner_mute_and_restores_the_dac(void)
{
    const jr_tone_kind_t old[] = { JR_TONE_OWNER_ACK, JR_TONE_DIAG_CHIRP,
                                   JR_TONE_SWEEP, JR_TONE_CHIME };
    for (size_t i = 0; i < sizeof old / sizeof old[0]; ++i) {
        jr_dac_mute_t m = { .muted = true, .restore = false };
        const bool owner_muted = true;   /* a tone never writes this */
        TEST_ASSERT_TRUE(jr_dac_mute_tone_begin(&m, owner_muted, old[i]));
        TEST_ASSERT_FALSE(m.muted);
        TEST_ASSERT_TRUE(jr_dac_mute_cue_settle(&m, false));
        TEST_ASSERT_TRUE_MESSAGE(m.muted, "the DAC is muted again after the tone");
        TEST_ASSERT_TRUE(owner_muted);
    }
}

/* Only the new interface cues are refused under the owner mute, and nothing
 * moves when they are. */
static void test_ui_cue_is_refused_under_owner_mute(void)
{
    jr_dac_mute_t m = { .muted = true, .restore = false };
    TEST_ASSERT_FALSE(jr_dac_mute_tone_begin(&m, true, JR_TONE_UI_CUE));
    TEST_ASSERT_TRUE(m.muted);
    TEST_ASSERT_FALSE(m.restore);
}

/* Timer alarm sounds even under owner privacy mute, then restores the DAC. */
static void test_alarm_tone_sounds_under_owner_mute_and_restores_dac(void)
{
    jr_dac_mute_t m = { .muted = true, .restore = false };
    TEST_ASSERT_TRUE(jr_dac_mute_sounds_under_owner_mute(JR_TONE_ALARM));
    TEST_ASSERT_TRUE(jr_dac_mute_tone_begin(&m, true, JR_TONE_ALARM));
    TEST_ASSERT_FALSE(m.muted);
    TEST_ASSERT_TRUE(m.restore);
    TEST_ASSERT_TRUE(jr_dac_mute_cue_settle(&m, false));
    TEST_ASSERT_TRUE(m.muted);
    TEST_ASSERT_FALSE(m.restore);
}

/* With the owner not muted every kind plays, and a live DAC stays live. */
static void test_every_tone_plays_when_owner_not_muted(void)
{
    for (int k = JR_TONE_OWNER_ACK; k <= JR_TONE_ALARM; ++k) {
        jr_dac_mute_t m = { .muted = false, .restore = false };
        TEST_ASSERT_TRUE(jr_dac_mute_tone_begin(&m, false, (jr_tone_kind_t)k));
        TEST_ASSERT_FALSE(jr_dac_mute_cue_settle(&m, false));
        TEST_ASSERT_FALSE(m.muted);
    }
}

void cue_tests_run(void)
{
    RUN_TEST(test_every_old_tone_plays_under_owner_mute_and_restores_the_dac);
    RUN_TEST(test_ui_cue_is_refused_under_owner_mute);
    RUN_TEST(test_alarm_tone_sounds_under_owner_mute_and_restores_dac);
    RUN_TEST(test_every_tone_plays_when_owner_not_muted);
    RUN_TEST(test_cue_on_idle_muted_dac_plays_then_restores_mute);
    RUN_TEST(test_cue_while_owner_muted_is_silent_and_stays_muted);
    RUN_TEST(test_cue_on_unmuted_dac_plays_and_stays_unmuted);
    RUN_TEST(test_real_unmute_during_cue_cancels_the_restore);
    RUN_TEST(test_real_mute_during_cue_leaves_no_stale_restore);
    RUN_TEST(test_second_cue_keeps_the_original_mute_owed);
}
