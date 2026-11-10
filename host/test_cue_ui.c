/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the interface cues: the gate and the generator. No ESP-IDF.
 */
#include "unity.h"
#include "jr_core/cue.h"
#include <stdlib.h>

#define PEAK_CAP_LSB (32767 * (int)JR_CUE_MAX_PERMILLE / 1000 + 1)

static jr_cue_ctx_t calm(void)
{
    jr_cue_ctx_t c = {
        .ui_sounds = true, .owner_muted = false, .phase = JR_ST_IDLE,
        .mood = JR_MOOD_AWAKE, .ota_active = false, .now_ms = 10000u,
    };
    return c;
}

/* ---- the gate ---- */

static void test_gate_allows_every_kind_when_calm_and_physical(void)
{
    for (int k = 0; k < JR_CUE_KIND_COUNT; ++k) {
        jr_cue_gate_t g = {0};
        jr_cue_ctx_t c = calm();
        TEST_ASSERT_TRUE_MESSAGE(
            jr_cue_allow(&g, &c, (jr_cue_kind_t)k, JR_CUE_SOURCE_PHYSICAL),
            "a calm, unmuted, physical cue plays");
    }
}

static void test_gate_synthetic_input_never_sounds(void)
{
    for (int k = 0; k < JR_CUE_KIND_COUNT; ++k) {
        jr_cue_gate_t g = {0};
        jr_cue_ctx_t c = calm();
        TEST_ASSERT_FALSE(
            jr_cue_allow(&g, &c, (jr_cue_kind_t)k, JR_CUE_SOURCE_SYNTHETIC));
        TEST_ASSERT_FALSE_MESSAGE(g.tick_seen,
            "a refused synthetic tick must not start the tick clock");
    }
}

static void test_gate_owner_mute_silences_every_kind(void)
{
    for (int k = 0; k < JR_CUE_KIND_COUNT; ++k) {
        jr_cue_gate_t g = {0};
        jr_cue_ctx_t c = calm();
        c.owner_muted = true;
        TEST_ASSERT_FALSE(
            jr_cue_allow(&g, &c, (jr_cue_kind_t)k, JR_CUE_SOURCE_PHYSICAL));
    }
}

static void test_gate_setting_off_silences_every_kind(void)
{
    for (int k = 0; k < JR_CUE_KIND_COUNT; ++k) {
        jr_cue_gate_t g = {0};
        jr_cue_ctx_t c = calm();
        c.ui_sounds = false;
        TEST_ASSERT_FALSE(
            jr_cue_allow(&g, &c, (jr_cue_kind_t)k, JR_CUE_SOURCE_PHYSICAL));
    }
}

static void test_gate_open_microphone_and_speech_block(void)
{
    const jr_state_t blocked[] = { JR_ST_LISTENING, JR_ST_THINKING,
                                   JR_ST_SPEAKING, JR_ST_ASKING };
    const jr_state_t open[] = { JR_ST_IDLE, JR_ST_CONNECTING,
                                JR_ST_HANDSHAKING, JR_ST_DRAINING,
                                JR_ST_RECONNECTING, JR_ST_BACKOFF,
                                JR_ST_FATAL };
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; ++i) {
        jr_cue_gate_t g = {0};
        jr_cue_ctx_t c = calm();
        c.phase = blocked[i];
        TEST_ASSERT_FALSE(
            jr_cue_allow(&g, &c, JR_CUE_CONFIRM, JR_CUE_SOURCE_PHYSICAL));
    }
    for (size_t i = 0; i < sizeof open / sizeof open[0]; ++i) {
        jr_cue_gate_t g = {0};
        jr_cue_ctx_t c = calm();
        c.phase = open[i];
        TEST_ASSERT_TRUE(
            jr_cue_allow(&g, &c, JR_CUE_CONFIRM, JR_CUE_SOURCE_PHYSICAL));
    }
}

static void test_gate_rest_moods_and_ota_block(void)
{
    const jr_mood_t moods[] = { JR_MOOD_AWAKE, JR_MOOD_AMBIENT,
                                JR_MOOD_WHISPER, JR_MOOD_DREAM };
    const bool plays[] = { true, true, false, false };
    for (int i = 0; i < 4; ++i) {
        jr_cue_gate_t g = {0};
        jr_cue_ctx_t c = calm();
        c.mood = moods[i];
        TEST_ASSERT_EQUAL(plays[i],
            jr_cue_allow(&g, &c, JR_CUE_DISMISS, JR_CUE_SOURCE_PHYSICAL));
    }
    jr_cue_gate_t g = {0};
    jr_cue_ctx_t c = calm();
    c.ota_active = true;
    TEST_ASSERT_FALSE(jr_cue_allow(&g, &c, JR_CUE_LIMIT, JR_CUE_SOURCE_PHYSICAL));
}

static void test_gate_tick_rate_limit_is_120_ms(void)
{
    jr_cue_gate_t g = {0};
    jr_cue_ctx_t c = calm();
    TEST_ASSERT_TRUE(jr_cue_allow(&g, &c, JR_CUE_TICK, JR_CUE_SOURCE_PHYSICAL));
    c.now_ms += 119u;
    TEST_ASSERT_FALSE(jr_cue_allow(&g, &c, JR_CUE_TICK, JR_CUE_SOURCE_PHYSICAL));
    /* A refused tick does not push the window out. */
    c.now_ms += 1u;
    TEST_ASSERT_TRUE(jr_cue_allow(&g, &c, JR_CUE_TICK, JR_CUE_SOURCE_PHYSICAL));
    /* Other cues are not rate limited and do not reset the tick clock. */
    c.now_ms += 10u;
    TEST_ASSERT_TRUE(jr_cue_allow(&g, &c, JR_CUE_CONFIRM, JR_CUE_SOURCE_PHYSICAL));
    TEST_ASSERT_TRUE(jr_cue_allow(&g, &c, JR_CUE_CONFIRM, JR_CUE_SOURCE_PHYSICAL));
    c.now_ms += 110u;
    TEST_ASSERT_TRUE(jr_cue_allow(&g, &c, JR_CUE_TICK, JR_CUE_SOURCE_PHYSICAL));
}

static void test_gate_tick_rate_limit_survives_clock_wrap(void)
{
    jr_cue_gate_t g = {0};
    jr_cue_ctx_t c = calm();
    c.now_ms = 0xFFFFFFF0u;
    TEST_ASSERT_TRUE(jr_cue_allow(&g, &c, JR_CUE_TICK, JR_CUE_SOURCE_PHYSICAL));
    c.now_ms = 0x00000010u;   /* 32 ms later across the wrap */
    TEST_ASSERT_FALSE(jr_cue_allow(&g, &c, JR_CUE_TICK, JR_CUE_SOURCE_PHYSICAL));
    c.now_ms = 0x00000080u;   /* 144 ms later */
    TEST_ASSERT_TRUE(jr_cue_allow(&g, &c, JR_CUE_TICK, JR_CUE_SOURCE_PHYSICAL));
}

static void test_gate_rejects_invalid_kind(void)
{
    jr_cue_gate_t g = {0};
    jr_cue_ctx_t c = calm();
    TEST_ASSERT_FALSE(jr_cue_allow(&g, &c, JR_CUE_KIND_COUNT,
                                   JR_CUE_SOURCE_PHYSICAL));
}

/* ---- level ---- */

static void test_amplitude_is_quiet_and_below_the_chime(void)
{
    /* The chime plays at 8 % = 80 permille. */
    for (int k = 0; k < JR_CUE_KIND_COUNT; ++k) {
        for (int v = 0; v <= 120; v += 5) {
            const uint16_t a = jr_cue_amplitude_permille((jr_cue_kind_t)k, v);
            TEST_ASSERT_TRUE(a >= 1u);
            TEST_ASSERT_TRUE(a <= JR_CUE_MAX_PERMILLE);
            TEST_ASSERT_TRUE(a < 80u);
        }
        TEST_ASSERT_TRUE(jr_cue_amplitude_permille((jr_cue_kind_t)k, 10) <=
                         jr_cue_amplitude_permille((jr_cue_kind_t)k, 100));
    }
    TEST_ASSERT_TRUE(jr_cue_amplitude_permille(JR_CUE_TICK, 100) <
                     jr_cue_amplitude_permille(JR_CUE_CONFIRM, 100));
}

/* ---- the generator ---- */

static int16_t buf[JR_CUE_MAX_SAMPLES];

static void test_render_lengths_match_the_brief(void)
{
    TEST_ASSERT_EQUAL_UINT(480, jr_cue_samples(JR_CUE_TICK));      /* 20 ms */
    TEST_ASSERT_EQUAL_UINT(2160, jr_cue_samples(JR_CUE_CONFIRM));  /* 90 ms */
    TEST_ASSERT_EQUAL_UINT(1680, jr_cue_samples(JR_CUE_DISMISS));  /* 70 ms */
    TEST_ASSERT_EQUAL_UINT(1200, jr_cue_samples(JR_CUE_LIMIT));    /* 50 ms */
    TEST_ASSERT_EQUAL_UINT(0, jr_cue_samples(JR_CUE_KIND_COUNT));
    for (int k = 0; k < JR_CUE_KIND_COUNT; ++k) {
        TEST_ASSERT_TRUE(jr_cue_samples((jr_cue_kind_t)k) <= JR_CUE_MAX_SAMPLES);
    }
}

static void test_render_edges_are_zero_and_peak_is_capped(void)
{
    for (int k = 0; k < JR_CUE_KIND_COUNT; ++k) {
        const size_t n = jr_cue_samples((jr_cue_kind_t)k);
        TEST_ASSERT_EQUAL_UINT(n, jr_cue_render((jr_cue_kind_t)k,
            JR_CUE_MAX_PERMILLE, buf, JR_CUE_MAX_SAMPLES));
        TEST_ASSERT_EQUAL_INT16(0, buf[0]);
        TEST_ASSERT_EQUAL_INT16(0, buf[n - 1u]);
        int peak = 0;
        for (size_t i = 0; i < n; ++i) {
            const int a = abs((int)buf[i]);
            if (a > peak) peak = a;
        }
        TEST_ASSERT_TRUE_MESSAGE(peak > 0, "a cue is not silent");
        TEST_ASSERT_TRUE_MESSAGE(peak <= PEAK_CAP_LSB,
                                 "no cue peaks above 5 % of full scale");
        /* The release is a raised cosine, so the last samples are tiny. */
        TEST_ASSERT_TRUE(abs((int)buf[n - 2u]) < 33);
        TEST_ASSERT_TRUE(abs((int)buf[1]) < 33);
    }
}

static void test_render_refuses_a_small_buffer_and_clamps_amplitude(void)
{
    TEST_ASSERT_EQUAL_UINT(0, jr_cue_render(JR_CUE_CONFIRM, 40, buf, 100));
    TEST_ASSERT_EQUAL_UINT(0, jr_cue_render(JR_CUE_KIND_COUNT, 40, buf,
                                            JR_CUE_MAX_SAMPLES));
    TEST_ASSERT_EQUAL_UINT(0, jr_cue_render(JR_CUE_TICK, 40, NULL,
                                            JR_CUE_MAX_SAMPLES));
    const size_t n = jr_cue_render(JR_CUE_LIMIT, 1000, buf, JR_CUE_MAX_SAMPLES);
    int peak = 0;
    for (size_t i = 0; i < n; ++i) {
        if (abs((int)buf[i]) > peak) peak = abs((int)buf[i]);
    }
    TEST_ASSERT_TRUE_MESSAGE(peak <= PEAK_CAP_LSB,
                             "an oversize amplitude request is clamped");
}

/* Sign changes: the rising cue speeds up, the falling one slows. */
static size_t crossings(const int16_t *x, size_t n)
{
    size_t c = 0;
    for (size_t i = 1; i < n; ++i) {
        if ((x[i - 1u] < 0) != (x[i] < 0)) ++c;
    }
    return c;
}

static void test_confirm_rises_and_dismiss_falls(void)
{
    size_t n = jr_cue_render(JR_CUE_CONFIRM, 40, buf, JR_CUE_MAX_SAMPLES);
    TEST_ASSERT_TRUE(crossings(buf + n / 2u, n / 2u) >
                     crossings(buf, n / 2u) * 14 / 10);
    n = jr_cue_render(JR_CUE_DISMISS, 35, buf, JR_CUE_MAX_SAMPLES);
    TEST_ASSERT_TRUE(crossings(buf + n / 2u, n / 2u) <
                     crossings(buf, n / 2u));
}

void cue_ui_tests_run(void)
{
    RUN_TEST(test_gate_allows_every_kind_when_calm_and_physical);
    RUN_TEST(test_gate_synthetic_input_never_sounds);
    RUN_TEST(test_gate_owner_mute_silences_every_kind);
    RUN_TEST(test_gate_setting_off_silences_every_kind);
    RUN_TEST(test_gate_open_microphone_and_speech_block);
    RUN_TEST(test_gate_rest_moods_and_ota_block);
    RUN_TEST(test_gate_tick_rate_limit_is_120_ms);
    RUN_TEST(test_gate_tick_rate_limit_survives_clock_wrap);
    RUN_TEST(test_gate_rejects_invalid_kind);
    RUN_TEST(test_amplitude_is_quiet_and_below_the_chime);
    RUN_TEST(test_render_lengths_match_the_brief);
    RUN_TEST(test_render_edges_are_zero_and_peak_is_capped);
    RUN_TEST(test_render_refuses_a_small_buffer_and_clamps_amplitude);
    RUN_TEST(test_confirm_rises_and_dismiss_falls);
}
