/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the four-mood rest ladder. No ESP-IDF.
 */
#include "unity.h"
#include "jr_core/mood.h"

static void test_mood_starts_awake(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = 100 };
    jr_mood_out_t o = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, o.mood);
    TEST_ASSERT_EQUAL_UINT8(100, o.brightness);
    TEST_ASSERT_TRUE(o.voice_armed);
    TEST_ASSERT_FALSE(o.clock_on);
}

static void test_mood_still_climbs_ambient_whisper_dream(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = {0};

    in.now_ms = JR_MOOD_AMBIENT_MS - 1;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);

    in.now_ms = JR_MOOD_AMBIENT_MS;
    jr_mood_out_t amb = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_AMBIENT, amb.mood);
    TEST_ASSERT_TRUE(amb.clock_on);
    TEST_ASSERT_TRUE(amb.voice_armed);
    TEST_ASSERT_EQUAL_UINT8(48, amb.brightness);
    TEST_ASSERT_TRUE(amb.changed);

    in.now_ms = JR_MOOD_WHISPER_MS;
    jr_mood_out_t wh = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, wh.mood);
    TEST_ASSERT_FALSE(wh.voice_armed);
    TEST_ASSERT_EQUAL_UINT8(22, wh.brightness);

    in.now_ms = JR_MOOD_DREAM_MS;
    jr_mood_out_t dr = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, dr.mood);
    TEST_ASSERT_FALSE(dr.voice_armed);
    TEST_ASSERT_EQUAL_UINT8(8, dr.brightness);
}

static void test_mood_motion_wakes(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = JR_MOOD_DREAM_MS };
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &in).mood);

    in.now_ms = JR_MOOD_DREAM_MS + 50;
    in.moving = true;
    jr_mood_out_t o = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, o.mood);
    TEST_ASSERT_TRUE(o.voice_armed);
    TEST_ASSERT_TRUE(o.changed);
}

static void test_mood_busy_holds_awake(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = {
        .now_ms = JR_MOOD_DREAM_MS,
        .user_busy = true,
    };
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
}

static void test_mood_face_down_is_dream(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = 200, .face_down = true };
    jr_mood_out_t o = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, o.mood);
    TEST_ASSERT_FALSE(o.voice_armed);
}

/* Sleep is due ten minutes into DREAM and not a tick sooner, by either road
 * (face-down, or still); a poke or a move cancels it. Both roads are walked
 * so a rule that only counted from boot, or only from face-down, fails. */
static void test_mood_sleep_is_due_ten_minutes_into_dream(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = 1000, .face_down = true };
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &in).mood);
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, 1000));
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, 1000 + JR_MOOD_SLEEP_MS - 1));
    TEST_ASSERT_TRUE(jr_mood_sleep_due(&s, 1000 + JR_MOOD_SLEEP_MS));
    /* Lifted: awake, and the clock starts over. */
    in.face_down = false;
    in.moving = true;
    in.now_ms = 1000 + JR_MOOD_SLEEP_MS;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, in.now_ms + JR_MOOD_SLEEP_MS));

    /* The still road: DREAM at 15 min, sleep due at 25. */
    jr_mood_reset(&s, 0);
    jr_mood_in_t still = { .now_ms = JR_MOOD_DREAM_MS };
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &still).mood);
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, JR_MOOD_DREAM_MS + JR_MOOD_SLEEP_MS - 1));
    TEST_ASSERT_TRUE(jr_mood_sleep_due(&s, JR_MOOD_DREAM_MS + JR_MOOD_SLEEP_MS));
    /* WHISPER is not DREAM: a device that has rested 5 min never sleeps. */
    jr_mood_reset(&s, 0);
    still.now_ms = JR_MOOD_WHISPER_MS;
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, jr_mood_step(&s, &still).mood);
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, JR_MOOD_WHISPER_MS + 2 * JR_MOOD_SLEEP_MS));
    TEST_ASSERT_FALSE(jr_mood_sleep_due(NULL, 0));
}

/* The saver ladder is the same ladder, four times faster, and it is an
 * INPUT: the moment the cell is charging again the normal waits apply. */
static void test_mood_saver_ladder_is_four_times_faster(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .saver = true };
    in.now_ms = JR_MOOD_AMBIENT_MS / JR_MOOD_SAVER_DIV - 1;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
    in.now_ms = JR_MOOD_AMBIENT_MS / JR_MOOD_SAVER_DIV;
    TEST_ASSERT_EQUAL(JR_MOOD_AMBIENT, jr_mood_step(&s, &in).mood);
    in.now_ms = JR_MOOD_WHISPER_MS / JR_MOOD_SAVER_DIV;
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, jr_mood_step(&s, &in).mood);
    in.now_ms = JR_MOOD_DREAM_MS / JR_MOOD_SAVER_DIV;
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &in).mood);
    const uint32_t dream_at = in.now_ms;
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, dream_at + JR_MOOD_SLEEP_MS / JR_MOOD_SAVER_DIV - 1));
    TEST_ASSERT_TRUE(jr_mood_sleep_due(&s, dream_at + JR_MOOD_SLEEP_MS / JR_MOOD_SAVER_DIV));
    /* Plugged in (saver off) the same stillness is not yet DREAM. */
    jr_mood_reset(&s, 0);
    in.saver = false;
    in.now_ms = JR_MOOD_DREAM_MS / JR_MOOD_SAVER_DIV;
    TEST_ASSERT_EQUAL(JR_MOOD_AMBIENT, jr_mood_step(&s, &in).mood);
}

/* QUIET (privacy mute): five seconds still and the glass is a WHISPER watch,
 * never AMBIENT (a dimmed listener with nothing to listen for). A pickup, a
 * busy phase, and face-down keep their meanings, and DREAM keeps its own
 * clock — mute must not be a shortcut to a black glass. Mutation-checked in
 * both directions: a ladder that ignores quiet fails the 5 s check; one that
 * jumps straight to DREAM fails the 1 minute check. */
static void test_mood_quiet_rests_as_a_watch_in_five_seconds(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = JR_MOOD_QUIET_MS - 1U, .quiet = true };
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
    in.now_ms = JR_MOOD_QUIET_MS;
    jr_mood_out_t o = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, o.mood);
    TEST_ASSERT_TRUE(o.clock_on);
    TEST_ASSERT_FALSE(o.voice_armed);
    TEST_ASSERT_EQUAL(22, o.brightness);
    in.now_ms = 60000U;
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, jr_mood_step(&s, &in).mood);
    in.now_ms = JR_MOOD_DREAM_MS;
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &in).mood);

    /* a pickup lights it, and it settles again in five seconds, not five minutes */
    in.now_ms = JR_MOOD_DREAM_MS + 1000U;
    in.moving = true;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
    in.moving = false;
    in.now_ms += JR_MOOD_QUIET_MS;
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, jr_mood_step(&s, &in).mood);

    /* answering a question outranks quiet; face-down still outranks both */
    in.user_busy = true;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
    in.user_busy = false;
    in.face_down = true;
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &in).mood);

    /* not quiet: the same five seconds is still AWAKE */
    jr_mood_reset(&s, 0);
    jr_mood_in_t loud = { .now_ms = JR_MOOD_QUIET_MS };
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &loud).mood);
}

static void test_mood_poke_awake_resets_still(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = JR_MOOD_DREAM_MS };
    (void)jr_mood_step(&s, &in);
    jr_mood_poke_awake(&s, JR_MOOD_DREAM_MS + 10);
    in.now_ms = JR_MOOD_DREAM_MS + 20;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
}

void mood_tests_run(void)
{
    RUN_TEST(test_mood_starts_awake);
    RUN_TEST(test_mood_still_climbs_ambient_whisper_dream);
    RUN_TEST(test_mood_motion_wakes);
    RUN_TEST(test_mood_busy_holds_awake);
    RUN_TEST(test_mood_face_down_is_dream);
    RUN_TEST(test_mood_quiet_rests_as_a_watch_in_five_seconds);
    RUN_TEST(test_mood_poke_awake_resets_still);
    RUN_TEST(test_mood_sleep_is_due_ten_minutes_into_dream);
    RUN_TEST(test_mood_saver_ladder_is_four_times_faster);
}
