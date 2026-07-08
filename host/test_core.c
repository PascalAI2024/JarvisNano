/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_core.c — Phase-0 Unity tests for the PURE core.
 *
 * Runs on a laptop with NO ESP-IDF. Proves: (1) the L3 transition function is
 * a real state machine (the happy-path spine advances, death routes to
 * Reconnecting, UserStop routes to Idle); (2) the pure DSP math is correct
 * (rms of a known buffer, resample length); (3) the port-fake seam works (fake
 * clock). This is the load-bearing Phase-0 win — the v4 lifecycle-race crash
 * class becomes catchable here, before a 15-20 min device build.
 */
#include "unity.h"
#include "jr_core/session.h"
#include "jr_core/turn_policy.h"
#include "jr_dsp/dsp.h"
#include "fake_clock.h"
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

/* --- helper: does a result contain a given command? --- */
static int has_cmd(const jr_transition_result_t *r, jr_cmd_t c)
{
    for (size_t i = 0; i < r->cmd_count; ++i) {
        if (r->cmds[i] == c) {
            return 1;
        }
    }
    return 0;
}

/* ===================== L3 state machine ===================== */

/* The headline acceptance assertion: Idle + Connect advances. */
static void test_idle_connect_advances(void)
{
    jr_transition_result_t r = jr_transition(JR_ST_IDLE, JR_EV_CONNECT);
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING, r.next);
    TEST_ASSERT_TRUE(has_cmd(&r, JR_CMD_OPEN_TRANSPORT));
}

/* Full happy-path forward spine Idle -> ... -> Speaking -> Listening. */
static void test_happy_path_spine(void)
{
    jr_state_t s = JR_ST_IDLE;

    s = jr_transition(s, JR_EV_CONNECT).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING, s);
    s = jr_transition(s, JR_EV_TRANSPORT_OPEN).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_HANDSHAKING, s);
    s = jr_transition(s, JR_EV_SETUP_COMPLETE).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, s);
    s = jr_transition(s, JR_EV_SPEECH_END).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_THINKING, s);
    s = jr_transition(s, JR_EV_AUDIO_CHUNK).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_SPEAKING, s);
    s = jr_transition(s, JR_EV_TURN_COMPLETE).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, s);
}

/* Involuntary death routes to Reconnecting (NOT Idle) + flushes playback. */
static void test_death_routes_to_reconnecting(void)
{
    jr_transition_result_t r = jr_transition(JR_ST_SPEAKING, JR_EV_TRANSPORT_CLOSED);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, r.next);
    TEST_ASSERT_TRUE(has_cmd(&r, JR_CMD_FLUSH_PLAYBACK));
    TEST_ASSERT_TRUE(has_cmd(&r, JR_CMD_RECONNECT));
}

/* Only UserStop routes to Idle — the structural cure for "doesn't talk unless
 * I type": death != UserStop. */
static void test_user_stop_routes_to_idle(void)
{
    jr_transition_result_t stop = jr_transition(JR_ST_SPEAKING, JR_EV_USER_STOP);
    TEST_ASSERT_EQUAL_INT(JR_ST_IDLE, stop.next);

    /* Same source state, a death event, must NOT reach Idle. */
    jr_transition_result_t dead = jr_transition(JR_ST_SPEAKING, JR_EV_UPLINK_DEAD);
    TEST_ASSERT_NOT_EQUAL(JR_ST_IDLE, dead.next);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, dead.next);
}

/* Unknown pairs are identity in Phase 0 (no zombie transitions). */
static void test_unknown_pair_is_identity(void)
{
    jr_transition_result_t r = jr_transition(JR_ST_IDLE, JR_EV_TURN_COMPLETE);
    TEST_ASSERT_EQUAL_INT(JR_ST_IDLE, r.next);
    TEST_ASSERT_EQUAL_size_t(0, r.cmd_count);
}

static void test_state_predicates(void)
{
    TEST_ASSERT_TRUE(jr_state_is_live(JR_ST_SPEAKING));
    TEST_ASSERT_FALSE(jr_state_is_live(JR_ST_IDLE));
    TEST_ASSERT_EQUAL_STRING("Listening", jr_state_name(JR_ST_LISTENING));
}

/* ===================== Pure DSP math ===================== */

/* rms of a constant +1000 buffer is exactly 1000. */
static void test_rms_known_buffer(void)
{
    int16_t buf[64];
    for (int i = 0; i < 64; ++i) {
        buf[i] = 1000;
    }
    float rms = jr_dsp_rms(buf, 64);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1000.0f, rms);

    /* Empty buffer is 0, not NaN. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jr_dsp_rms(buf, 0));
}

/* rms of a full-scale square wave (+/-32767) is ~32767. */
static void test_rms_square_wave(void)
{
    int16_t buf[100];
    for (int i = 0; i < 100; ++i) {
        buf[i] = (i % 2) ? 32767 : -32767;
    }
    float rms = jr_dsp_rms(buf, 100);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 32767.0f, rms);
}

/* 24k -> 16k downsample yields 2/3 the samples. */
static void test_resample_length(void)
{
    int16_t in[768];
    int16_t out[600];
    for (int i = 0; i < 768; ++i) {
        in[i] = (int16_t)i;
    }
    size_t n = jr_dsp_resample_linear(in, 768, 24000, out, 600, 16000);
    TEST_ASSERT_EQUAL_size_t(512, n);   /* 768 * 16000 / 24000 = 512 */
}

static void test_vad_stub(void)
{
    jr_vad_t v;
    jr_vad_init(&v);
    TEST_ASSERT_TRUE(jr_vad_update(&v, 500.0f));   /* energy -> speech */
    TEST_ASSERT_FALSE(jr_vad_update(&v, 0.0f));     /* silence         */
}

/* ===================== Port-fake seam ===================== */

static void test_fake_clock(void)
{
    fake_clock_reset();
    jr_clock_t clk = fake_clock_make();
    TEST_ASSERT_EQUAL_UINT64(0, jr_clock_now_ms(&clk));
    fake_clock_advance(1500);
    TEST_ASSERT_EQUAL_UINT64(1500, jr_clock_now_ms(&clk));
}

int main(void)
{
    UNITY_BEGIN();
    /* L3 */
    RUN_TEST(test_idle_connect_advances);
    RUN_TEST(test_happy_path_spine);
    RUN_TEST(test_death_routes_to_reconnecting);
    RUN_TEST(test_user_stop_routes_to_idle);
    RUN_TEST(test_unknown_pair_is_identity);
    RUN_TEST(test_state_predicates);
    /* DSP */
    RUN_TEST(test_rms_known_buffer);
    RUN_TEST(test_rms_square_wave);
    RUN_TEST(test_resample_length);
    RUN_TEST(test_vad_stub);
    /* Ports */
    RUN_TEST(test_fake_clock);
    return UNITY_END();
}
