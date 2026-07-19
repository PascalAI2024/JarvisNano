/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_core.c — Phase-1 Unity host suite for the PURE core.
 *
 * Runs on a laptop with NO ESP-IDF, NO hardware, NO live Gemini quota. Proves
 * the full L3 session state machine (spec: phase1-core-state-machine.md):
 *   - the happy-path spine + every death route + the UserStop terminus;
 *   - the 6 critical invariants v5 exists to guarantee (v4 failed exactly here);
 *   - the §4.11 zombie / "Live-but-silent" unreachability proof, encoded as
 *     tests (every non-final state's armed deadline forces a transition OUT);
 *   - the T01-T25 acceptance matrix.
 * Plus the original Phase-0 tests (pure DSP math, the fake-clock port seam).
 *
 * The reducer is fed a fake clock so every deadline is driven with NO
 * wall-clock waits. The transition function performs ZERO I/O — it returns an
 * ordered command list; the tests assert (next_state, command list).
 */
#include "unity.h"
#include "jr_core/session.h"
#include "jr_core/turn_policy.h"
#include "jr_core/monitors.h"
#include "jr_core/snapshot.h"
#include "jr_dsp/dsp.h"
#include "fake_clock.h"
#include <math.h>
#include <string.h>

/* Run-3 L2 transport suite, defined in host/test_transport.c; registered inside
 * this file's UNITY_BEGIN/END so the whole suite is one jr_host_tests binary. */
extern void transport_tests_run(void);

/* Capstone: the integrated orchestrator-pump + adversarial self-heal soak,
 * defined in host/test_soak.c; registered inside this file's UNITY_BEGIN/END. */
extern void soak_tests_run(void);

/* ---- shared fixtures ---- */
static jr_clock_t g_clk;

void setUp(void)
{
    fake_clock_reset();
    g_clk = fake_clock_make();
}
void tearDown(void) {}

/* Run one transition against the shared fake clock. */
static jr_outcome_t step(jr_session_t s, jr_event_t e)
{
    return jr_transition(s, e, g_clk);
}

/* A manual-VAD (PTT) session pinned at a given phase. */
static jr_session_t sess_at(jr_state_t phase)
{
    jr_session_t s = jr_session_init(JR_VAD_MANUAL_LOCAL_RMS);
    s.phase = phase;
    return s;
}
/* An auto-VAD (server) session pinned at a given phase. */
static jr_session_t sess_at_auto(jr_state_t phase)
{
    jr_session_t s = jr_session_init(JR_VAD_SERVER);
    s.phase = phase;
    return s;
}

/* --- command-list helpers --- */
static int has_cmd(const jr_cmd_list_t *L, jr_cmd_kind_t c)
{
    for (size_t i = 0; i < L->count; ++i) {
        if (L->cmds[i].kind == c) {
            return 1;
        }
    }
    return 0;
}
static const jr_command_t *find_cmd(const jr_cmd_list_t *L, jr_cmd_kind_t c)
{
    for (size_t i = 0; i < L->count; ++i) {
        if (L->cmds[i].kind == c) {
            return &L->cmds[i];
        }
    }
    return NULL;
}

/* ===================================================================== *
 *  ORIGINAL 11 (kept green; adapted to the SessionState API)            *
 * ===================================================================== */

/* Idle + UserStart advances to Connecting and emits Connect. */
static void test_idle_connect_advances(void)
{
    jr_outcome_t r = step(sess_at(JR_ST_IDLE), jr_event(JR_EV_USER_START));
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING, r.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_CONNECT));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_ARM_KEEPALIVE));
}

/* Full happy-path forward spine Idle -> ... -> Speaking -> Listening. */
static void test_happy_path_spine(void)
{
    jr_session_t s = sess_at(JR_ST_IDLE);

    s = step(s, jr_event(JR_EV_USER_START)).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING, s.phase);
    s = step(s, jr_event(JR_EV_CONNECTED)).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_HANDSHAKING, s.phase);
    s = step(s, jr_event(JR_EV_SETUP_COMPLETE)).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, s.phase);
    s = step(s, jr_event(JR_EV_SPEECH_ENDED)).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_THINKING, s.phase);
    s = step(s, jr_event(JR_EV_SERVER_AUDIO_CHUNK)).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_SPEAKING, s.phase);
    s = step(s, jr_event(JR_EV_SERVER_TURN_COMPLETE)).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, s.phase);
}

/* Involuntary death routes to Reconnecting (NOT Idle) + tears down. */
static void test_death_routes_to_reconnecting(void)
{
    jr_outcome_t r = step(sess_at(JR_ST_SPEAKING), jr_event(JR_EV_TRANSPORT_CLOSED));
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, r.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_FLUSH_PLAYBACK_RING));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_CLOSE_TRANSPORT));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_SCHEDULE_BACKOFF));
}

/* Only UserStop reaches Idle — via Draining. Death from the same source state
 * must NOT reach Idle (death != UserStop; disjoint targets). */
static void test_user_stop_routes_to_idle(void)
{
    jr_outcome_t drain = step(sess_at(JR_ST_SPEAKING), jr_event(JR_EV_USER_STOP));
    TEST_ASSERT_EQUAL_INT(JR_ST_DRAINING, drain.next.phase);
    jr_outcome_t idle = step(drain.next, jr_event(JR_EV_TRANSPORT_CLOSED));
    TEST_ASSERT_EQUAL_INT(JR_ST_IDLE, idle.next.phase);

    jr_outcome_t dead = step(sess_at(JR_ST_SPEAKING), jr_event(JR_EV_UPLINK_DEAD));
    TEST_ASSERT_NOT_EQUAL(JR_ST_IDLE, dead.next.phase);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, dead.next.phase);
}

/* Illegal pairs are inert: identity (no state change), flagged, no effectful
 * command (only an EmitDiag(illegal) log). */
static void test_unknown_pair_is_identity(void)
{
    jr_outcome_t r = step(sess_at(JR_ST_IDLE), jr_event(JR_EV_SERVER_TURN_COMPLETE));
    TEST_ASSERT_TRUE(r.illegal);
    TEST_ASSERT_EQUAL_INT(JR_ST_IDLE, r.next.phase);
    TEST_ASSERT_FALSE(has_cmd(&r.cmds, JR_CMD_CONNECT));
    TEST_ASSERT_FALSE(has_cmd(&r.cmds, JR_CMD_PUBLISH_SNAPSHOT));
}

static void test_state_predicates(void)
{
    TEST_ASSERT_TRUE(jr_state_is_live(JR_ST_SPEAKING));
    TEST_ASSERT_FALSE(jr_state_is_live(JR_ST_IDLE));
    TEST_ASSERT_EQUAL_STRING("Listening", jr_state_name(JR_ST_LISTENING));
    TEST_ASSERT_EQUAL_STRING("BargeDetected", jr_event_name(JR_EV_BARGE_DETECTED));
    TEST_ASSERT_EQUAL_STRING("MuteDacNow", jr_cmd_name(JR_CMD_MUTE_DAC_NOW));
}

/* ---- pure DSP math (unchanged) ---- */
static void test_rms_known_buffer(void)
{
    int16_t buf[64];
    for (int i = 0; i < 64; ++i) { buf[i] = 1000; }
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1000.0f, jr_dsp_rms(buf, 64));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jr_dsp_rms(buf, 0));
}
static void test_rms_square_wave(void)
{
    int16_t buf[100];
    for (int i = 0; i < 100; ++i) { buf[i] = (i % 2) ? 32767 : -32767; }
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 32767.0f, jr_dsp_rms(buf, 100));
}
static void test_fake_clock(void)
{
    fake_clock_reset();
    jr_clock_t clk = fake_clock_make();
    TEST_ASSERT_EQUAL_UINT64(0, jr_clock_now_ms(&clk));
    fake_clock_advance(1500);
    TEST_ASSERT_EQUAL_UINT64(1500, jr_clock_now_ms(&clk));
}

/* ===================================================================== *
 *  7.1 L3 transition tests — T01..T11                                   *
 * ===================================================================== */

/* T01 — transportClosed in Speaking -> Reconnecting + the full teardown. */
static void test_T01_transportClosed_in_Speaking(void)
{
    jr_event_t e = jr_event(JR_EV_TRANSPORT_CLOSED);
    e.close_kind = JR_CLOSE_ABRUPT;
    jr_outcome_t r = step(sess_at(JR_ST_SPEAKING), e);

    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, r.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_MUTE_DAC_NOW));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_FLUSH_PLAYBACK_RING));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_PAUSE_CAPTURE));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_CLOSE_TRANSPORT));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_DISARM_KEEPALIVE));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_DISARM_NO_REPLY_WATCHDOG));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_DISARM_CAPTURE_PAUSE_TIMER));
    const jr_command_t *sb = find_cmd(&r.cmds, JR_CMD_SCHEDULE_BACKOFF);
    TEST_ASSERT_NOT_NULL(sb);
    TEST_ASSERT_EQUAL_UINT32(1000, sb->delay_ms);
}

/* T02 — noReply 20s in Thinking -> Listening + [Disarm, StartCapture, ArmCap]. */
static void test_T02_noReply_resume(void)
{
    jr_session_t s = sess_at(JR_ST_THINKING);
    fake_clock_advance(20000);
    jr_outcome_t r = step(s, jr_event(JR_EV_NO_REPLY_TIMEOUT));

    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, r.next.phase);
    TEST_ASSERT_EQUAL_INT(JR_CMD_DISARM_NO_REPLY_WATCHDOG, r.cmds.cmds[0].kind);
    TEST_ASSERT_EQUAL_INT(JR_CMD_START_CAPTURE,            r.cmds.cmds[1].kind);
    TEST_ASSERT_EQUAL_INT(JR_CMD_ARM_CAPTURE_PAUSE_TIMER,  r.cmds.cmds[2].kind);
}

/* T03 — dead uplink in Listening -> Reconnecting via DEATH. */
static void test_T03_uplinkDead(void)
{
    jr_event_t e = jr_event(JR_EV_UPLINK_DEAD);
    e.consecutive_tx_failures = 25;
    jr_outcome_t r = step(sess_at(JR_ST_LISTENING), e);

    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, r.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_CLOSE_TRANSPORT));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_PAUSE_CAPTURE));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_SCHEDULE_BACKOFF));
}

/* T04 — quota error parks in Backoff with a ~45s cool-off (NOT Reconnecting). */
static void test_T04_quota_parks(void)
{
    jr_event_t e = jr_event(JR_EV_SERVER_ERROR);
    e.error_kind = JR_ERRK_QUOTA;
    jr_outcome_t r = step(sess_at(JR_ST_LISTENING), e);

    TEST_ASSERT_EQUAL_INT(JR_ST_BACKOFF, r.next.phase);
    TEST_ASSERT_NOT_EQUAL(JR_ST_RECONNECTING, r.next.phase);
    const jr_command_t *sb = find_cmd(&r.cmds, JR_CMD_SCHEDULE_BACKOFF);
    TEST_ASSERT_NOT_NULL(sb);
    TEST_ASSERT_EQUAL_UINT32(45000, sb->delay_ms);
}

/* T05 — stale keepalive deadline in Listening -> Reconnecting. */
static void test_T05_staleDeadline(void)
{
    jr_event_t e = jr_event(JR_EV_STALE_DEADLINE);
    e.age_ms = 45000;
    jr_outcome_t r = step(sess_at(JR_ST_LISTENING), e);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, r.next.phase);
}

/* T06 — death-routing matrix: EVERY death event x EVERY death source state ends
 * in Reconnecting/Backoff, NEVER Idle. (Critical invariant #1.) */
static void test_T06_death_routing_matrix(void)
{
    const jr_event_kind_t deaths[] = {
        JR_EV_TRANSPORT_CLOSED, JR_EV_SERVER_ERROR, JR_EV_STALE_DEADLINE,
        JR_EV_UPLINK_DEAD, JR_EV_SERVER_GO_AWAY,
    };
    const jr_state_t states[] = {
        JR_ST_CONNECTING, JR_ST_HANDSHAKING, JR_ST_LISTENING,
        JR_ST_THINKING, JR_ST_SPEAKING,
    };
    for (size_t d = 0; d < 5; ++d) {
        for (size_t st = 0; st < 5; ++st) {
            jr_event_t e = jr_event(deaths[d]);
            if (deaths[d] == JR_EV_SERVER_ERROR) {
                e.error_kind = JR_ERRK_TRANSIENT;
            }
            jr_outcome_t r = step(sess_at(states[st]), e);
            TEST_ASSERT_NOT_EQUAL(JR_ST_IDLE, r.next.phase);
            TEST_ASSERT_TRUE(r.next.phase == JR_ST_RECONNECTING ||
                             r.next.phase == JR_ST_BACKOFF);
        }
    }
}

/* T07 — UserStop is the ONLY path to Idle (via Draining); death never is.
 * (Critical invariant #2.) */
static void test_T07_userStop_only_path_to_Idle(void)
{
    const jr_state_t live[] = { JR_ST_LISTENING, JR_ST_THINKING, JR_ST_SPEAKING };
    for (size_t i = 0; i < 3; ++i) {
        jr_outcome_t drain = step(sess_at(live[i]), jr_event(JR_EV_USER_STOP));
        TEST_ASSERT_EQUAL_INT(JR_ST_DRAINING, drain.next.phase);
        jr_outcome_t idle = step(drain.next, jr_event(JR_EV_TRANSPORT_CLOSED));
        TEST_ASSERT_EQUAL_INT(JR_ST_IDLE, idle.next.phase);

        /* a death straight from the live state never reaches Idle */
        jr_outcome_t dead = step(sess_at(live[i]), jr_event(JR_EV_TRANSPORT_CLOSED));
        TEST_ASSERT_NOT_EQUAL(JR_ST_IDLE, dead.next.phase);
    }
}

/* T08 — a storm of quick deaths parks once fail_count reaches PARK_AFTER (=6),
 * i.e. the 7th consecutive quick death parks with no timer; Tap exits, fc=0.
 * (Reconciled with T09: six scheduled delays precede the park.) */
static void test_T08_storm_parks(void)
{
    jr_session_t s = sess_at(JR_ST_LISTENING);
    for (int i = 0; i < 6; ++i) {
        jr_outcome_t r = step(s, jr_event(JR_EV_TRANSPORT_CLOSED));
        TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, r.next.phase);
        s = r.next;                 /* carry fail_count */
        s.phase = JR_ST_LISTENING;  /* reconnect that dies again (uptime < 15s) */
    }
    jr_outcome_t parked = step(s, jr_event(JR_EV_TRANSPORT_CLOSED));
    TEST_ASSERT_EQUAL_INT(JR_ST_BACKOFF, parked.next.phase);
    TEST_ASSERT_FALSE(has_cmd(&parked.cmds, JR_CMD_SCHEDULE_BACKOFF)); /* no timer */

    jr_outcome_t resumed = step(parked.next, jr_event(JR_EV_TAP));
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING, resumed.next.phase);
    TEST_ASSERT_EQUAL_UINT16(0, resumed.next.fail_count);
}

/* T09 — capped exponential backoff: 1000, 2000, 4000, 8000, 16000, 16000. */
static void test_T09_capped_exponential(void)
{
    const uint32_t expect[6] = { 1000, 2000, 4000, 8000, 16000, 16000 };
    jr_session_t s = sess_at(JR_ST_LISTENING);
    for (int i = 0; i < 6; ++i) {
        jr_outcome_t r = step(s, jr_event(JR_EV_TRANSPORT_CLOSED));
        TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, r.next.phase);
        const jr_command_t *sb = find_cmd(&r.cmds, JR_CMD_SCHEDULE_BACKOFF);
        TEST_ASSERT_NOT_NULL(sb);
        TEST_ASSERT_EQUAL_UINT32(expect[i], sb->delay_ms);
        s = r.next;
        s.phase = JR_ST_LISTENING;
    }
}

/* T10 — a healthy (>=15s) session resets the backoff delay to 1000. */
static void test_T10_healthy_uptime_resets(void)
{
    /* first quick death -> delay 1000, fail_count -> 1 */
    jr_outcome_t r1 = step(sess_at(JR_ST_LISTENING), jr_event(JR_EV_TRANSPORT_CLOSED));
    const jr_command_t *sb1 = find_cmd(&r1.cmds, JR_CMD_SCHEDULE_BACKOFF);
    TEST_ASSERT_NOT_NULL(sb1);
    TEST_ASSERT_EQUAL_UINT32(1000, sb1->delay_ms);

    /* reconnect, reach Live, stamp last_success_ts = now (0) */
    jr_session_t hs = r1.next;         /* fail_count == 1 */
    hs.phase = JR_ST_HANDSHAKING;
    jr_outcome_t live = step(hs, jr_event(JR_EV_SETUP_COMPLETE));
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, live.next.phase);

    /* healthy uptime >= 15s, then die again -> delay resets to 1000 (fc still 1) */
    fake_clock_advance(15000);
    jr_outcome_t r2 = step(live.next, jr_event(JR_EV_TRANSPORT_CLOSED));
    const jr_command_t *sb2 = find_cmd(&r2.cmds, JR_CMD_SCHEDULE_BACKOFF);
    TEST_ASSERT_NOT_NULL(sb2);
    TEST_ASSERT_EQUAL_UINT32(1000, sb2->delay_ms);
}

/* T11 — self-heal without a tap: Reconnecting -> ... -> Live.Listening driven
 * only by monitor/transport events; no UserStart anywhere. (Critical: the
 * "talks without typing" proof.) */
static void test_T11_self_heal_without_tap(void)
{
    jr_session_t s = sess_at(JR_ST_RECONNECTING);
    uint32_t gen0 = s.session_gen;

    jr_outcome_t c = step(s, jr_event(JR_EV_BACKOFF_ELAPSED));
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING, c.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&c.cmds, JR_CMD_CONNECT));
    TEST_ASSERT_EQUAL_UINT32(gen0 + 1, c.next.session_gen); /* a real new session */

    jr_outcome_t h = step(c.next, jr_event(JR_EV_CONNECTED));
    TEST_ASSERT_EQUAL_INT(JR_ST_HANDSHAKING, h.next.phase);
    jr_outcome_t l = step(h.next, jr_event(JR_EV_SETUP_COMPLETE));
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, l.next.phase);
}

/* ===================================================================== *
 *  7.2 fake-transport Gemini-sequence tests — T12..T18                  *
 * ===================================================================== */

/* T12 — interrupted WITHOUT a preceding generationComplete (Thinking). */
static void test_T12_interrupted_without_generationComplete(void)
{
    jr_outcome_t r = step(sess_at(JR_ST_THINKING), jr_event(JR_EV_SERVER_INTERRUPTED));
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, r.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_DISARM_NO_REPLY_WATCHDOG));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_FLUSH_PLAYBACK_RING));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_MUTE_DAC_NOW));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_START_CAPTURE));
}

/* T13 — server self-interrupt while Speaking -> flush + resume listening. */
static void test_T13_interrupted_in_speaking(void)
{
    jr_outcome_t r = step(sess_at(JR_ST_SPEAKING), jr_event(JR_EV_SERVER_INTERRUPTED));
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, r.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_MUTE_DAC_NOW));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_FLUSH_PLAYBACK_RING));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_START_CAPTURE));
}

/* T14 — toolCall is dispatched generation-tagged with the current session_gen. */
static void test_T14_toolCall_generation_tagged(void)
{
    jr_session_t s = sess_at(JR_ST_THINKING);
    s.session_gen = 7;
    jr_event_t e = jr_event(JR_EV_SERVER_TOOL_CALL);
    e.call_id = 42;
    e.call_id_text = "call_42";
    e.tool_name = "get_weather";
    e.tool_args = "{}";
    jr_outcome_t r = step(s, e);

    TEST_ASSERT_EQUAL_INT(JR_ST_THINKING, r.next.phase);
    const jr_command_t *d = find_cmd(&r.cmds, JR_CMD_DISPATCH_TOOL_CALL);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT32(7, d->session_gen);
    TEST_ASSERT_EQUAL_UINT32(42, d->call_id);
    TEST_ASSERT_EQUAL_STRING("call_42", d->call_id_text);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_DISARM_NO_REPLY_WATCHDOG));
    TEST_ASSERT_FALSE(has_cmd(&r.cmds, JR_CMD_ARM_NO_REPLY_WATCHDOG));
}

static void test_tool_result_rearms_reply_watchdog(void)
{
    jr_session_t s = sess_at(JR_ST_THINKING);
    s.session_gen = 7;
    jr_event_t e = jr_event(JR_EV_TOOL_RESULT_READY);
    e.call_id = 42;
    e.call_id_text = "call_42";
    e.tool_name = "current_time";
    e.tool_response = "{\"result\":\"12:34\"}";
    e.session_gen = 7;
    jr_outcome_t r = step(s, e);

    const jr_command_t *send = find_cmd(&r.cmds, JR_CMD_SEND_TOOL_RESPONSE);
    TEST_ASSERT_NOT_NULL(send);
    TEST_ASSERT_EQUAL_STRING("call_42", send->call_id_text);
    TEST_ASSERT_EQUAL_STRING("current_time", send->tool_name);
    TEST_ASSERT_EQUAL_STRING("{\"result\":\"12:34\"}", send->tool_response);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_ARM_NO_REPLY_WATCHDOG));

    e.session_gen = 6;
    r = step(s, e);
    TEST_ASSERT_FALSE(has_cmd(&r.cmds, JR_CMD_SEND_TOOL_RESPONSE));
    TEST_ASSERT_FALSE(has_cmd(&r.cmds, JR_CMD_ARM_NO_REPLY_WATCHDOG));
}

/* T15 — toolCallCancellation cancels by id; a result stamped with a stale gen
 * is dropped by the generation-tagging predicate. */
static void test_T15_toolCancel_and_stale_gen_drop(void)
{
    jr_session_t s = sess_at(JR_ST_THINKING);
    s.session_gen = 7;
    jr_event_t cancel = jr_event(JR_EV_SERVER_TOOL_CALL);
    cancel.is_cancellation = true;
    cancel.call_id = 42;
    cancel.call_id_text = "call_42";
    jr_outcome_t r = step(s, cancel);

    const jr_command_t *c = find_cmd(&r.cmds, JR_CMD_CANCEL_TOOL_CALL);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT32(42, c->call_id);
    TEST_ASSERT_EQUAL_STRING("call_42", c->call_id_text);

    /* a result tagged gen = N-1 arrives after the machine is at gen N -> stale */
    TEST_ASSERT_TRUE(jr_session_gen_is_stale(7, 6));
    TEST_ASSERT_FALSE(jr_session_gen_is_stale(7, 7));
}

/* T16 — goAway carries a resumption token; the auto-reconnect Connect uses it. */
static void test_T16_goAway_resumes_with_token(void)
{
    jr_event_t go = jr_event(JR_EV_SERVER_GO_AWAY);
    go.resumption_token = 0xABCD;
    jr_outcome_t r = step(sess_at(JR_ST_SPEAKING), go);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, r.next.phase);
    TEST_ASSERT_EQUAL_UINT32(0xABCD, r.next.resumption_token);

    jr_outcome_t c = step(r.next, jr_event(JR_EV_BACKOFF_ELAPSED));
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING, c.next.phase);
    const jr_command_t *conn = find_cmd(&c.cmds, JR_CMD_CONNECT);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_UINT32(0xABCD, conn->resumption_token);
}

/* T17 — a server chunk before any turn boundary opens Speaking. */
static void test_T17_serverChunk_before_boundary(void)
{
    jr_outcome_t r = step(sess_at(JR_ST_LISTENING), jr_event(JR_EV_SERVER_AUDIO_CHUNK));
    TEST_ASSERT_EQUAL_INT(JR_ST_SPEAKING, r.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_UNMUTE_DAC));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_FEED_PLAYBACK));
}

/* T18 — a dead uplink is NOT masked by downlink heartbeats (split-brain cure). */
static void test_T18_split_brain_dead_uplink(void)
{
    jr_session_t s = sess_at(JR_ST_LISTENING);
    s = step(s, jr_event(JR_EV_HEARTBEAT)).next;   /* downlink alive */
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, s.phase);
    s = step(s, jr_event(JR_EV_HEARTBEAT)).next;
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, s.phase);

    jr_event_t ud = jr_event(JR_EV_UPLINK_DEAD);
    ud.consecutive_tx_failures = 25;
    jr_outcome_t r = step(s, ud);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, r.next.phase);
}

/* ===================================================================== *
 *  7.3 L4 adaptive-VAD / barge tests — T19..T23 (REAL signal math)      *
 *                                                                       *
 *  Synthesized deterministic PCM (no binary WAV assets): a constant-fill *
 *  frame has RMS == fill value, so jr_dsp_rms(frame) reproduces the      *
 *  target level exactly and every threshold crossing is deterministic.   *
 *  The fake clock advances one 32 ms frame per eval so the cold-start +  *
 *  barge-guard windows are driven with no wall-clock wait.               *
 * ===================================================================== */

#define FRAME_N 512

/* Fill a frame so jr_dsp_rms(frame) == |level| (RMS of a constant == |const|). */
static void fill_rms(int16_t *buf, size_t n, float level)
{
    if (level < 0.0f) { level = 0.0f; }
    if (level > 32767.0f) { level = 32767.0f; }
    int16_t v = (int16_t)level;
    for (size_t i = 0; i < n; ++i) { buf[i] = v; }
}

/* Reduce one frame at the given capture RMS + known playback reference level,
 * then advance the fake clock one frame (32 ms). */
static jr_turn_decision_t tp_frame(jr_turn_policy_t *p, float cap_rms,
                                   float play_level, bool play_active,
                                   jr_tp_substate_t sub)
{
    int16_t buf[FRAME_N];
    fill_rms(buf, FRAME_N, cap_rms);
    jr_turn_decision_t d =
        jr_turn_policy_eval(p, buf, FRAME_N, play_level, play_active, sub, g_clk);
    fake_clock_advance(JR_TP_FRAME_MS);
    return d;
}

/* Warm the noise floor to `floor` and clear the 500 ms cold-start window by
 * feeding ambient Listening frames. 20 frames = 640 ms > cold-start. */
static void tp_warm(jr_turn_policy_t *p, float floor_level)
{
    for (int i = 0; i < 20; ++i) {
        tp_frame(p, floor_level, 0.0f, false, JR_TP_LISTENING);
    }
}

/* T19 — ZERO SELF-BARGE across a synthesized ~10-turn playback with the exact
 * calibration echo-tail transient (mic~=736 while the instantaneous playback
 * level already dropped to ~141). The 4-frame peak-hold retains the causing
 * high playback level, so the gate stays above the lagged echo — no frame
 * barges, guard window or not. */
static void test_T19_zero_self_barge(void)
{
    jr_turn_policy_t p;
    jr_turn_policy_init(&p);
    tp_warm(&p, 40.0f);                     /* floor -> ~40, cold-start cleared */

    const float PLAY_HI = 8177.0f;          /* loud model audio                 */
    const float PLAY_LO = 141.0f;           /* the "play dropped to 141" level   */
    const float ECHO_RATIO = 0.09f;         /* residual echo just under the 10% */
    const int   LAG = 2;                    /* 64 ms DAC/DMA echo lag           */

    /* build 10 back-to-back turns: 20 loud frames + 4 quiet frames each */
    #define NF (10 * 24)
    float play[NF];
    for (int t = 0; t < 10; ++t) {
        for (int f = 0; f < 24; ++f) {
            play[t * 24 + f] = (f < 20) ? PLAY_HI : PLAY_LO;
        }
    }

    bool saw_tail_transient = false;
    for (int i = 0; i < NF; ++i) {
        float echo = (i >= LAG ? ECHO_RATIO * play[i - LAG] : 0.0f) + 40.0f;
        if (echo > 700.0f && play[i] < 200.0f) {
            saw_tail_transient = true;      /* the mic~=736 while play~=141 case */
        }
        jr_turn_decision_t d = tp_frame(&p, echo, play[i], true, JR_TP_SPEAKING);
        TEST_ASSERT_FALSE(d.is_barge);
        TEST_ASSERT_NOT_EQUAL(JR_TP_EV_BARGE_DETECTED, d.event);
    }
    TEST_ASSERT_TRUE(saw_tail_transient);   /* we really exercised the transient */
    #undef NF
}

/* T20 — a GENUINE talk-over barge fires promptly (< 300 ms). Steady loud
 * playback: echo alone never crosses the gate; from frame K the user talks over
 * (capture well above the gate) and BargeDetected latches within 2 frames. */
static void test_T20_talkover_barge_prompt(void)
{
    jr_turn_policy_t p;
    jr_turn_policy_init(&p);
    tp_warm(&p, 40.0f);

    const float PLAY = 8177.0f;
    const float ECHO = 0.09f * PLAY + 40.0f;   /* ~776 — below the ~817 gate    */
    const float VOICE = 2600.0f;               /* talk-over, well above the gate */
    const int   K = 20;                        /* onset frame (past the guard)  */

    int barge_frame = -1;
    for (int i = 0; i < 40; ++i) {
        float cap = (i >= K) ? VOICE : ECHO;
        jr_turn_decision_t d = tp_frame(&p, cap, PLAY, true, JR_TP_SPEAKING);
        if (i < K) {
            TEST_ASSERT_FALSE(d.is_barge);     /* echo alone never self-barges  */
        }
        if (d.event == JR_TP_EV_BARGE_DETECTED && barge_frame < 0) {
            barge_frame = i;
        }
    }
    TEST_ASSERT_TRUE(barge_frame >= 0);                 /* it fired              */
    TEST_ASSERT_TRUE((barge_frame - K) <= 10);          /* within 300 ms (10 fr) */
}

static void test_speaking_echo_never_primes_next_user_turn(void)
{
    jr_turn_policy_t p;
    jr_turn_policy_init(&p);
    tp_warm(&p, 40.0f);

    for (int i = 0; i < 30; ++i) {
        jr_turn_decision_t d = tp_frame(&p, 700.0f, 8000.0f, true,
                                        JR_TP_SPEAKING);
        TEST_ASSERT_NOT_EQUAL(JR_TP_EV_SPEECH_STARTED, d.event);
        TEST_ASSERT_FALSE(p.in_speech);
    }

    for (int i = 0; i < 30; ++i) {
        jr_turn_decision_t d = tp_frame(&p, 60.0f, 0, false,
                                        JR_TP_LISTENING);
        TEST_ASSERT_EQUAL_INT(JR_TP_EV_NONE, d.event);
    }
}

/* T21 — the silence threshold is the subtle killer. A speech utterance with
 * inter-word pauses at ~93-117 RMS (ABOVE the offset threshold) then a true
 * end-of-turn pause at the floor: hysteresis holds through the inter-word
 * pauses, so EXACTLY ONE SpeechStarted and EXACTLY ONE SpeechEnded fire — the
 * SpeechEnded only at the real pause. */
static void test_T21_silence_threshold_regression(void)
{
    jr_turn_policy_t p;
    jr_turn_policy_init(&p);
    tp_warm(&p, 40.0f);                     /* floor 40 -> onset 88, offset 48   */

    int starts = 0, ends = 0, end_frame = -1, i = 0;
    /* word 1 (10 frames @120), inter-word pause (5 @100), word 2 (10 @120) */
    for (int k = 0; k < 10; ++k, ++i) { if (tp_frame(&p,120,0,false,JR_TP_LISTENING).event==JR_TP_EV_SPEECH_STARTED) starts++; }
    for (int k = 0; k < 5;  ++k, ++i) { if (tp_frame(&p,100,0,false,JR_TP_LISTENING).event==JR_TP_EV_SPEECH_ENDED)   { ends++; end_frame=i; } }
    for (int k = 0; k < 10; ++k, ++i) { jr_turn_decision_t d=tp_frame(&p,120,0,false,JR_TP_LISTENING); if(d.event==JR_TP_EV_SPEECH_STARTED) starts++; if(d.event==JR_TP_EV_SPEECH_ENDED){ends++;end_frame=i;} }
    /* true end-of-turn pause @40 (< offset 48) held past hangover */
    int pause_start = i;
    for (int k = 0; k < 30; ++k, ++i) { jr_turn_decision_t d=tp_frame(&p,40,0,false,JR_TP_LISTENING); if(d.event==JR_TP_EV_SPEECH_ENDED){ends++;end_frame=i;} if(d.event==JR_TP_EV_SPEECH_STARTED) starts++; }

    TEST_ASSERT_EQUAL_INT(1, starts);                 /* one turn opened         */
    TEST_ASSERT_EQUAL_INT(1, ends);                   /* committed exactly once  */
    TEST_ASSERT_TRUE(end_frame >= pause_start);       /* at the REAL pause only  */
}

/* T22 — the noise floor is PINNED (v4-proven fixed calibration), NOT adaptive.
 * The v5 adaptive floor was the regression: fed rising room noise it climbed
 * toward the user's speech level, driving the onset gate up until speech no
 * longer cleared it ("stuck listening, not responding"). Assert the floor holds
 * at 40 through a sustained loud sweep, so onset stays ~85 and a normal word
 * always commits — the behavior that worked a month ago. */
static void test_T22_floor_is_pinned_not_adaptive(void)
{
    /* baseline: floor 40, onset ~85. A 120-RMS word commits + a 40 pause ends. */
    jr_turn_policy_t q;
    jr_turn_policy_init(&q);
    tp_warm(&q, 40.0f);
    int q_starts = 0, q_ends = 0;
    for (int k = 0; k < 12; ++k) if (tp_frame(&q,120,0,false,JR_TP_LISTENING).event==JR_TP_EV_SPEECH_STARTED) q_starts++;
    for (int k = 0; k < 30; ++k) if (tp_frame(&q,40, 0,false,JR_TP_LISTENING).event==JR_TP_EV_SPEECH_ENDED)   q_ends++;
    TEST_ASSERT_EQUAL_INT(1, q_starts);
    TEST_ASSERT_EQUAL_INT(1, q_ends);

    /* Anti-"stuck-listening": a sustained rising loud sweep must NOT drift the
     * floor — it stays pinned at 40 (the v5 adaptive floor would have climbed
     * toward ~130 and swallowed the next word). */
    jr_turn_policy_t s;
    jr_turn_policy_init(&s);
    tp_warm(&s, 40.0f);
    jr_turn_decision_t dz;
    memset(&dz, 0, sizeof dz);
    float lvl = 40.0f;
    for (int k = 0; k < 120; ++k) { lvl += 1.5f; dz = tp_frame(&s, lvl, 0, false, JR_TP_LISTENING); }
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 40.0f, dz.noise_floor);      /* pinned, did NOT track up */
}

/* The attached Waveshare's post-AEC idle bed is commonly in the low teens.
 * It must not become a ~29-RMS speech gate (13 * 2.2), which caused unattended
 * phantom turns on hardware. The calibrated floor stays at 40, while real
 * speech above the resulting ~88-RMS threshold still commits normally. */
static void test_quiet_hardware_floor_does_not_false_start(void)
{
    jr_turn_policy_t p;
    jr_turn_policy_init(&p);
    int false_starts = 0;
    jr_turn_decision_t quiet = {0};
    for (int i = 0; i < 40; ++i) {
        quiet = tp_frame(&p, 13.0f, 0, false, JR_TP_LISTENING);
        if (quiet.event == JR_TP_EV_SPEECH_STARTED) { false_starts++; }
    }
    TEST_ASSERT_EQUAL_INT(0, false_starts);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 40.0f, quiet.noise_floor);

    int real_starts = 0;
    for (int i = 0; i < 12; ++i) {
        if (tp_frame(&p, 110.0f, 0, false, JR_TP_LISTENING).event ==
            JR_TP_EV_SPEECH_STARTED) {
            real_starts++;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, real_starts);
}

/* T23 — cold-start guard: while the floor is unconverged (first ~500 ms), no
 * speech commit and no barge fire, even for over-threshold input; once the
 * window passes, a genuine word commits. */
static void test_T23_cold_start_guard(void)
{
    /* (A) speech-commit suppression during the first 500 ms. */
    jr_turn_policy_t p;
    jr_turn_policy_init(&p);
    tp_frame(&p, 40.0f, 0, false, JR_TP_LISTENING);   /* frame 0: seed floor ~40 */
    int early = 0;
    for (int i = 1; i < 16; ++i) {                    /* frames 1..15 : t < 500ms */
        if (tp_frame(&p, 120.0f, 0, false, JR_TP_LISTENING).event == JR_TP_EV_SPEECH_STARTED) early++;
    }
    TEST_ASSERT_EQUAL_INT(0, early);                  /* commit suppressed cold   */
    int late = 0;
    for (int i = 0; i < 15; ++i) {                    /* now t >= 500ms          */
        if (tp_frame(&p, 120.0f, 0, false, JR_TP_LISTENING).event == JR_TP_EV_SPEECH_STARTED) late++;
    }
    TEST_ASSERT_EQUAL_INT(1, late);                   /* commits once warm        */

    /* (B) barge suppression during the first 500 ms even for a massive talk-over. */
    jr_turn_policy_t b;
    jr_turn_policy_init(&b);
    for (int i = 0; i < 15; ++i) {                    /* all frames t < 500ms    */
        jr_turn_decision_t d = tp_frame(&b, 3000.0f, 2000.0f, true, JR_TP_SPEAKING);
        TEST_ASSERT_FALSE(d.is_barge);
    }
}

/* ===================================================================== *
 *  7.4 backpressure / would-block tests — T24..T25                      *
 * ===================================================================== */

/* Model of an adapter's bounded, drop-newest capture->sender queue (§5.6). */
typedef struct {
    int      buf[4];
    size_t   len;
    size_t   cap;
    unsigned drops;
} bp_ring;
static void bp_push(bp_ring *r, int v)
{
    if (r->len < r->cap) {
        r->buf[r->len++] = v;   /* keep oldest */
    } else {
        r->drops++;             /* drop newest */
    }
}

/* T24 — bounded, drop-newest: memory stays capped, oldest retained, newest
 * dropped, a drops counter increments, and a full queue never drives DEATH. */
static void test_T24_backpressure_bounded_drop_newest(void)
{
    bp_ring r;
    memset(&r, 0, sizeof r);
    r.cap = 4;
    for (int i = 0; i < 10; ++i) {
        bp_push(&r, i);
    }
    TEST_ASSERT_TRUE(r.len <= r.cap);          /* bounded memory */
    TEST_ASSERT_EQUAL_size_t(4, r.len);
    TEST_ASSERT_EQUAL_INT(0, r.buf[0]);        /* oldest retained */
    TEST_ASSERT_EQUAL_INT(3, r.buf[3]);
    TEST_ASSERT_EQUAL_UINT(6, r.drops);        /* newest 6 dropped */

    /* a saturated uplink is backpressure, never a death; no event reaches L3 */
    TEST_ASSERT_TRUE(jr_err_is_backpressure(JR_ERR_WOULD_BLOCK));
}

/* T25 — would-block (poll_write==0, errno==0) is backpressure, NOT death: the
 * framer drops one recoverable frame; the machine stays Live. There is no
 * would-block EVENT in the 21-event vocabulary, so it can never be delivered as
 * a death. */
static void test_T25_wouldblock_is_not_death(void)
{
    TEST_ASSERT_TRUE(jr_err_is_backpressure(JR_ERR_WOULD_BLOCK));
    TEST_ASSERT_FALSE(jr_err_is_backpressure(JR_ERR_FAIL));
    TEST_ASSERT_FALSE(jr_err_is_backpressure(JR_ERR_CLOSED));

    /* the framer swallowed the would-block; the reducer sees only benign frames
     * and stays Live (never Reconnecting). */
    jr_session_t s = sess_at(JR_ST_LISTENING);
    jr_outcome_t r = step(s, jr_event(JR_EV_HEARTBEAT));
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, r.next.phase);
    TEST_ASSERT_NOT_EQUAL(JR_ST_RECONNECTING, r.next.phase);
}

/* ===================================================================== *
 *  §4.11 zombie unreachability + protocol invariants (dedicated)        *
 * ===================================================================== */

/* §4.11 — for EVERY non-final state, feeding its armed-monitor deadline event
 * forces a transition OUT (forward progress). The only rest points are Idle,
 * Backoff, Fatal. (Critical invariant #6.) */
static void test_zombie_unreachable(void)
{
    /* Connecting keepalive(connect) -> StaleDeadline -> out */
    TEST_ASSERT_NOT_EQUAL(JR_ST_CONNECTING,
        step(sess_at(JR_ST_CONNECTING), jr_event(JR_EV_STALE_DEADLINE)).next.phase);
    /* Handshaking keepalive(handshake) -> StaleDeadline -> out */
    TEST_ASSERT_NOT_EQUAL(JR_ST_HANDSHAKING,
        step(sess_at(JR_ST_HANDSHAKING), jr_event(JR_EV_STALE_DEADLINE)).next.phase);
    /* Live.Listening keepalive/uplink -> out */
    TEST_ASSERT_NOT_EQUAL(JR_ST_LISTENING,
        step(sess_at(JR_ST_LISTENING), jr_event(JR_EV_STALE_DEADLINE)).next.phase);
    TEST_ASSERT_NOT_EQUAL(JR_ST_LISTENING,
        step(sess_at(JR_ST_LISTENING), jr_event(JR_EV_UPLINK_DEAD)).next.phase);
    /* Live.Thinking noReply/keepalive/uplink -> out */
    TEST_ASSERT_NOT_EQUAL(JR_ST_THINKING,
        step(sess_at(JR_ST_THINKING), jr_event(JR_EV_NO_REPLY_TIMEOUT)).next.phase);
    TEST_ASSERT_NOT_EQUAL(JR_ST_THINKING,
        step(sess_at(JR_ST_THINKING), jr_event(JR_EV_STALE_DEADLINE)).next.phase);
    TEST_ASSERT_NOT_EQUAL(JR_ST_THINKING,
        step(sess_at(JR_ST_THINKING), jr_event(JR_EV_UPLINK_DEAD)).next.phase);
    /* Live.Speaking keepalive/uplink -> out */
    TEST_ASSERT_NOT_EQUAL(JR_ST_SPEAKING,
        step(sess_at(JR_ST_SPEAKING), jr_event(JR_EV_STALE_DEADLINE)).next.phase);
    TEST_ASSERT_NOT_EQUAL(JR_ST_SPEAKING,
        step(sess_at(JR_ST_SPEAKING), jr_event(JR_EV_UPLINK_DEAD)).next.phase);
    /* Reconnecting backoff timer -> BackoffElapsed -> Connecting */
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING,
        step(sess_at(JR_ST_RECONNECTING), jr_event(JR_EV_BACKOFF_ELAPSED)).next.phase);
    /* Draining keepalive(drain) -> StaleDeadline -> Idle (forced) */
    TEST_ASSERT_EQUAL_INT(JR_ST_IDLE,
        step(sess_at(JR_ST_DRAINING), jr_event(JR_EV_STALE_DEADLINE)).next.phase);
}

/* Manual-PTT protocol: audioStreamEnd must NEVER be emitted in manual mode
 * (the v4 stuck-in-THINKING bug); auto mode uses audioStreamEnd, not activityEnd.
 * (Command semantics from docs/reference/gemini-live-api-v5.md.) */
static void test_manual_ptt_turn_boundaries(void)
{
    jr_outcome_t m = step(sess_at(JR_ST_LISTENING), jr_event(JR_EV_SPEECH_ENDED));
    TEST_ASSERT_EQUAL_INT(JR_ST_THINKING, m.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&m.cmds, JR_CMD_SEND_ACTIVITY_END));
    TEST_ASSERT_FALSE(has_cmd(&m.cmds, JR_CMD_SEND_AUDIO_STREAM_END));

    jr_outcome_t a = step(sess_at_auto(JR_ST_LISTENING), jr_event(JR_EV_SPEECH_ENDED));
    TEST_ASSERT_EQUAL_INT(JR_ST_THINKING, a.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&a.cmds, JR_CMD_SEND_AUDIO_STREAM_END));
    TEST_ASSERT_FALSE(has_cmd(&a.cmds, JR_CMD_SEND_ACTIVITY_END));
}

/* Fatal exits (§4.10): UserStop -> Idle; UserStart/Tap -> Connecting; a stray
 * server frame is an inert illegal drop. */
static void test_fatal_exits(void)
{
    jr_outcome_t stop = step(sess_at(JR_ST_FATAL), jr_event(JR_EV_USER_STOP));
    TEST_ASSERT_EQUAL_INT(JR_ST_IDLE, stop.next.phase);

    jr_outcome_t start = step(sess_at(JR_ST_FATAL), jr_event(JR_EV_USER_START));
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING, start.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&start.cmds, JR_CMD_CONNECT));

    jr_outcome_t drop = step(sess_at(JR_ST_FATAL), jr_event(JR_EV_SERVER_AUDIO_CHUNK));
    TEST_ASSERT_TRUE(drop.illegal);
}

/* ReconnectPolicy §5.1 exercised directly (park-quota, storm, healthy reset). */
static void test_reconnect_policy_direct(void)
{
    jr_reconnect_decision_t q = jr_reconnect_policy(0, 0, JR_ERRK_QUOTA);
    TEST_ASSERT_TRUE(q.should_park);
    TEST_ASSERT_EQUAL_UINT32(45000, q.delay_ms);

    jr_reconnect_decision_t storm = jr_reconnect_policy(JR_PARK_AFTER, 0, JR_ERRK_TRANSIENT);
    TEST_ASSERT_TRUE(storm.should_park);
    TEST_ASSERT_EQUAL_UINT32(0, storm.delay_ms);

    jr_reconnect_decision_t healthy = jr_reconnect_policy(5, JR_HEALTHY_UPTIME_MS, JR_ERRK_TRANSIENT);
    TEST_ASSERT_FALSE(healthy.should_park);
    TEST_ASSERT_EQUAL_UINT32(1000, healthy.delay_ms); /* effective 0 despite fc=5 */
}

/* THE barge row (L3 §4.6, critical invariant #3): BargeDetected in Speaking ->
 * Listening emitting EXACTLY [MuteDacNow, FlushPlaybackRing, SendActivityStart]
 * in that order. (The L4 signal math that PRODUCES a BargeDetected is T19/T20;
 * this pins the command contract the event triggers.) */
static void test_barge_command_contract(void)
{
    jr_event_t e = jr_event(JR_EV_BARGE_DETECTED);
    e.rms = 2600.0f;
    e.playback_peak = 8177.0f;
    jr_outcome_t r = step(sess_at(JR_ST_SPEAKING), e);

    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, r.next.phase);
    TEST_ASSERT_EQUAL_INT(JR_CMD_MUTE_DAC_NOW,        r.cmds.cmds[0].kind);
    TEST_ASSERT_EQUAL_INT(JR_CMD_FLUSH_PLAYBACK_RING, r.cmds.cmds[1].kind);
    TEST_ASSERT_EQUAL_INT(JR_CMD_SEND_ACTIVITY_START, r.cmds.cmds[2].kind);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_ARM_CAPTURE_PAUSE_TIMER));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_EMIT_DIAG));
}

/* ===================================================================== *
 *  §5 resilience monitors — arm / re-arm / fire (fake-clock driven)     *
 * ===================================================================== */

/* 5.2 KeepaliveMonitor — fires StaleDeadline past the deadline; server traffic
 * (any frame incl. Heartbeat) before the deadline re-arms and it does NOT fire. */
static void test_keepalive_monitor_arm_rearm_fire(void)
{
    jr_keepalive_monitor_t m;
    memset(&m, 0, sizeof m);
    jr_keepalive_arm(&m, jr_clock_now_ms(&g_clk), JR_LIVE_DEADLINE_MS);   /* 45000 */

    fake_clock_advance(44000);
    TEST_ASSERT_FALSE(jr_keepalive_poll(&m, jr_clock_now_ms(&g_clk)).fired);
    jr_keepalive_on_traffic(&m, jr_clock_now_ms(&g_clk));                 /* re-arm */
    fake_clock_advance(44000);
    TEST_ASSERT_FALSE(jr_keepalive_poll(&m, jr_clock_now_ms(&g_clk)).fired); /* still alive */

    fake_clock_advance(2000);                                            /* age > 45000 */
    jr_monitor_poll_t p = jr_keepalive_poll(&m, jr_clock_now_ms(&g_clk));
    TEST_ASSERT_TRUE(p.fired);
    TEST_ASSERT_EQUAL_INT(JR_EV_STALE_DEADLINE, p.event.kind);
    TEST_ASSERT_TRUE(p.event.age_ms > JR_LIVE_DEADLINE_MS);

    jr_keepalive_disarm(&m);
    fake_clock_advance(100000);
    TEST_ASSERT_FALSE(jr_keepalive_poll(&m, jr_clock_now_ms(&g_clk)).fired); /* disarmed */
}

/* 5.3 DeadUplinkMonitor — fires UplinkDead at TX_FAIL_RESUME(25) consecutive tx
 * failures; a good send re-arms (resets); a would-block never counts (§5.6). */
static void test_dead_uplink_monitor_arm_rearm_fire(void)
{
    jr_dead_uplink_monitor_t m;
    memset(&m, 0, sizeof m);
    jr_dead_uplink_arm(&m, JR_TX_FAIL_RESUME);

    for (int i = 0; i < 24; ++i) { jr_dead_uplink_on_tx(&m, JR_ERR_FAIL); }
    TEST_ASSERT_FALSE(jr_dead_uplink_poll(&m).fired);       /* 24 < 25 */
    jr_dead_uplink_on_tx(&m, JR_ERR_FAIL);                  /* the 25th */
    jr_monitor_poll_t p = jr_dead_uplink_poll(&m);
    TEST_ASSERT_TRUE(p.fired);
    TEST_ASSERT_EQUAL_INT(JR_EV_UPLINK_DEAD, p.event.kind);
    TEST_ASSERT_EQUAL_UINT32(25, p.event.consecutive_tx_failures);

    /* a good send mid-run re-arms (counter -> 0) so it does NOT fire */
    jr_dead_uplink_arm(&m, JR_TX_FAIL_RESUME);
    for (int i = 0; i < 24; ++i) { jr_dead_uplink_on_tx(&m, JR_ERR_FAIL); }
    jr_dead_uplink_on_tx(&m, JR_OK);                        /* success re-arms */
    for (int i = 0; i < 24; ++i) { jr_dead_uplink_on_tx(&m, JR_ERR_FAIL); }
    TEST_ASSERT_FALSE(jr_dead_uplink_poll(&m).fired);

    /* would-block is backpressure, not a tx failure: 25 of them never fire */
    jr_dead_uplink_arm(&m, JR_TX_FAIL_RESUME);
    for (int i = 0; i < 25; ++i) { jr_dead_uplink_on_tx(&m, JR_ERR_WOULD_BLOCK); }
    TEST_ASSERT_FALSE(jr_dead_uplink_poll(&m).fired);
}

/* 5.4 NoReplyWatchdog — fires NoReplyTimeout past 20 s in Thinking; disarm (the
 * first ServerAudioChunk / any exit) prevents the fire. */
static void test_no_reply_watchdog_arm_disarm_fire(void)
{
    jr_no_reply_watchdog_t m;
    memset(&m, 0, sizeof m);
    jr_no_reply_arm(&m, jr_clock_now_ms(&g_clk), JR_NOREPLY_MS);          /* 20000 */

    fake_clock_advance(19000);
    TEST_ASSERT_FALSE(jr_no_reply_poll(&m, jr_clock_now_ms(&g_clk)).fired);
    fake_clock_advance(2000);                                            /* > 20000 */
    jr_monitor_poll_t p = jr_no_reply_poll(&m, jr_clock_now_ms(&g_clk));
    TEST_ASSERT_TRUE(p.fired);
    TEST_ASSERT_EQUAL_INT(JR_EV_NO_REPLY_TIMEOUT, p.event.kind);

    /* disarmed (first chunk arrived) -> never fires */
    jr_no_reply_arm(&m, jr_clock_now_ms(&g_clk), JR_NOREPLY_MS);
    jr_no_reply_disarm(&m);
    fake_clock_advance(60000);
    TEST_ASSERT_FALSE(jr_no_reply_poll(&m, jr_clock_now_ms(&g_clk)).fired);
}

/* 5.5 CapturePauseKeepalive — auto-VAD only: fires CapturePauseElapsed past 1 s
 * of capture pause; on_capture re-arms; NEVER fires in manual mode. */
static void test_capture_pause_monitor_arm_rearm_fire(void)
{
    jr_capture_pause_monitor_t m;
    memset(&m, 0, sizeof m);
    jr_capture_pause_arm(&m, jr_clock_now_ms(&g_clk), JR_CAPTURE_PAUSE_MS, true); /* auto */

    fake_clock_advance(1001);                                            /* > 1000 */
    jr_monitor_poll_t p = jr_capture_pause_poll(&m, jr_clock_now_ms(&g_clk));
    TEST_ASSERT_TRUE(p.fired);
    TEST_ASSERT_EQUAL_INT(JR_EV_CAPTURE_PAUSE_ELAPSED, p.event.kind);

    /* a captured frame re-arms so a sub-second pause does not fire */
    jr_capture_pause_on_capture(&m, jr_clock_now_ms(&g_clk));
    fake_clock_advance(999);
    TEST_ASSERT_FALSE(jr_capture_pause_poll(&m, jr_clock_now_ms(&g_clk)).fired);

    /* manual mode never fires, however long the pause */
    jr_capture_pause_monitor_t manual;
    memset(&manual, 0, sizeof manual);
    jr_capture_pause_arm(&manual, jr_clock_now_ms(&g_clk), JR_CAPTURE_PAUSE_MS, false);
    fake_clock_advance(5000);
    TEST_ASSERT_FALSE(jr_capture_pause_poll(&manual, jr_clock_now_ms(&g_clk)).fired);
}

/* 5.6 Backpressure — the bounded, drop-newest ring primitive: capped memory,
 * oldest retained (FIFO), newest dropped, a drops counter, never a DEATH. */
static void test_bp_ring_primitive(void)
{
    jr_bp_ring_t r;
    jr_bp_init(&r, 4);
    for (uint32_t i = 0; i < 10; ++i) { jr_bp_push(&r, i); }
    TEST_ASSERT_TRUE(r.len <= r.cap);              /* bounded memory */
    TEST_ASSERT_EQUAL_size_t(4, r.len);
    TEST_ASSERT_EQUAL_UINT32(6, r.drops);          /* newest 6 dropped */

    uint32_t v;
    TEST_ASSERT_TRUE(jr_bp_pop(&r, &v));
    TEST_ASSERT_EQUAL_UINT32(0, v);                /* oldest retained (FIFO) */
    TEST_ASSERT_TRUE(jr_bp_pop(&r, &v));
    TEST_ASSERT_EQUAL_UINT32(1, v);

    /* a would-block is backpressure, not a death event in the L3 vocabulary */
    TEST_ASSERT_TRUE(jr_err_is_backpressure(JR_ERR_WOULD_BLOCK));
}

/* Integration: each monitor emits an event the L3 machine actually consumes,
 * with the right payload, driving the right transition. Proves the seam — the
 * monitors feed the SAME reducer, no inline timeout inside a task. */
static void test_monitors_feed_the_machine(void)
{
    /* KeepaliveMonitor StaleDeadline in Listening -> Reconnecting (DEATH). */
    jr_keepalive_monitor_t ka;
    memset(&ka, 0, sizeof ka);
    jr_keepalive_arm(&ka, jr_clock_now_ms(&g_clk), JR_LIVE_DEADLINE_MS);
    fake_clock_advance(JR_LIVE_DEADLINE_MS + 1);
    jr_monitor_poll_t sp = jr_keepalive_poll(&ka, jr_clock_now_ms(&g_clk));
    TEST_ASSERT_TRUE(sp.fired);
    jr_outcome_t ro = step(sess_at(JR_ST_LISTENING), sp.event);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, ro.next.phase);

    /* DeadUplinkMonitor UplinkDead in Listening -> Reconnecting (DEATH). */
    jr_dead_uplink_monitor_t du;
    memset(&du, 0, sizeof du);
    jr_dead_uplink_arm(&du, JR_TX_FAIL_RESUME);
    for (int i = 0; i < 25; ++i) { jr_dead_uplink_on_tx(&du, JR_ERR_FAIL); }
    jr_monitor_poll_t up = jr_dead_uplink_poll(&du);
    TEST_ASSERT_TRUE(up.fired);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, step(sess_at(JR_ST_LISTENING), up.event).next.phase);

    /* NoReplyWatchdog NoReplyTimeout in Thinking -> Listening + resume. */
    jr_no_reply_watchdog_t nr;
    memset(&nr, 0, sizeof nr);
    jr_no_reply_arm(&nr, jr_clock_now_ms(&g_clk), JR_NOREPLY_MS);
    fake_clock_advance(JR_NOREPLY_MS + 1);
    jr_monitor_poll_t nrp = jr_no_reply_poll(&nr, jr_clock_now_ms(&g_clk));
    TEST_ASSERT_TRUE(nrp.fired);
    jr_outcome_t nro = step(sess_at(JR_ST_THINKING), nrp.event);
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, nro.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&nro.cmds, JR_CMD_START_CAPTURE));

    /* CapturePauseKeepalive CapturePauseElapsed in auto-VAD Listening ->
     * stays Listening + SendAudioStreamEnd (the keepalive v4 never had). */
    jr_capture_pause_monitor_t cp;
    memset(&cp, 0, sizeof cp);
    jr_capture_pause_arm(&cp, jr_clock_now_ms(&g_clk), JR_CAPTURE_PAUSE_MS, true);
    fake_clock_advance(JR_CAPTURE_PAUSE_MS + 1);
    jr_monitor_poll_t cpp = jr_capture_pause_poll(&cp, jr_clock_now_ms(&g_clk));
    TEST_ASSERT_TRUE(cpp.fired);
    jr_outcome_t cpo = step(sess_at_auto(JR_ST_LISTENING), cpp.event);
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, cpo.next.phase);
    TEST_ASSERT_TRUE(has_cmd(&cpo.cmds, JR_CMD_SEND_AUDIO_STREAM_END));
}

/* ===================================================================== *
 *  Observability StateSnapshot — the fold reducer                       *
 * ===================================================================== */

/* Step the machine AND fold the transition into the snapshot (mirrors the owner
 * task consuming the implicit PublishSnapshot on every transition). */
static jr_session_t step_fold(jr_state_snapshot_t *snap, jr_session_t s, jr_event_t e)
{
    jr_outcome_t o = step(s, e);
    jr_snapshot_fold(snap, s.phase, e, &o);
    return o.next;
}

/* A sequence of transitions yields the expected counters, last-reason, error
 * kind, and last-N reason ring. */
static void test_snapshot_counters_and_ring(void)
{
    jr_state_snapshot_t snap;
    jr_snapshot_init(&snap);
    TEST_ASSERT_EQUAL_INT(JR_ST_IDLE, snap.phase);

    jr_session_t s = sess_at(JR_ST_IDLE);
    s = step_fold(&snap, s, jr_event(JR_EV_USER_START));    /* -> Connecting  */
    s = step_fold(&snap, s, jr_event(JR_EV_CONNECTED));     /* -> Handshaking */
    s = step_fold(&snap, s, jr_event(JR_EV_SETUP_COMPLETE));/* -> Listening   */

    /* an involuntary death (quota) -> Backoff, counted as a death */
    jr_event_t q = jr_event(JR_EV_SERVER_ERROR);
    q.error_kind = JR_ERRK_QUOTA;
    s = step_fold(&snap, s, q);                             /* -> Backoff     */
    TEST_ASSERT_EQUAL_INT(JR_ST_BACKOFF, snap.phase);
    TEST_ASSERT_EQUAL_UINT32(1, snap.deaths);
    TEST_ASSERT_EQUAL_INT(JR_ERRK_QUOTA, snap.last_error_kind);

    /* the quota cool-off elapses -> auto-redial to Connecting (a reconnect) */
    s = step_fold(&snap, s, jr_event(JR_EV_BACKOFF_ELAPSED)); /* -> Connecting */
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING, snap.phase);
    TEST_ASSERT_EQUAL_UINT32(1, snap.reconnects);
    TEST_ASSERT_EQUAL_INT(JR_EV_BACKOFF_ELAPSED, snap.last_reason);

    /* 5 state-changing transitions so far; ring holds them in order */
    TEST_ASSERT_EQUAL_UINT32(5, snap.transitions);
    TEST_ASSERT_EQUAL_size_t(5, snap.ring_count);
    TEST_ASSERT_EQUAL_INT(JR_EV_USER_START,     snap.reason_ring[0]);
    TEST_ASSERT_EQUAL_INT(JR_EV_CONNECTED,      snap.reason_ring[1]);
    TEST_ASSERT_EQUAL_INT(JR_EV_SETUP_COMPLETE, snap.reason_ring[2]);
    TEST_ASSERT_EQUAL_INT(JR_EV_SERVER_ERROR,   snap.reason_ring[3]);
    TEST_ASSERT_EQUAL_INT(JR_EV_BACKOFF_ELAPSED,snap.reason_ring[4]);
}

/* An illegal (state,event) drop is not a transition: it folds nothing (no
 * counter bump, no ring push, phase unchanged). And the ring is bounded — it
 * keeps only the last N reasons, overwriting oldest. */
static void test_snapshot_illegal_and_ring_bound(void)
{
    jr_state_snapshot_t snap;
    jr_snapshot_init(&snap);

    /* illegal in Idle folds nothing */
    jr_outcome_t bad = step(sess_at(JR_ST_IDLE), jr_event(JR_EV_SERVER_TURN_COMPLETE));
    jr_snapshot_fold(&snap, JR_ST_IDLE, jr_event(JR_EV_SERVER_TURN_COMPLETE), &bad);
    TEST_ASSERT_EQUAL_UINT32(0, snap.transitions);
    TEST_ASSERT_EQUAL_size_t(0, snap.ring_count);

    /* overflow the ring: JR_SNAP_RING+3 heartbeat-driven no-op... use real
     * transitions by toggling Listening<->Speaking via chunk / turn-complete. */
    jr_session_t s = sess_at(JR_ST_LISTENING);
    for (int i = 0; i < JR_SNAP_RING + 3; ++i) {
        jr_event_t e = (i % 2 == 0) ? jr_event(JR_EV_SERVER_AUDIO_CHUNK)   /* ->Speaking */
                                    : jr_event(JR_EV_SERVER_TURN_COMPLETE);/* ->Listening */
        s = step_fold(&snap, s, e);
    }
    TEST_ASSERT_EQUAL_size_t(JR_SNAP_RING, snap.ring_count);   /* bounded */
    TEST_ASSERT_TRUE(snap.transitions >= (uint32_t)(JR_SNAP_RING + 3));
}

int main(void)
{
    UNITY_BEGIN();

    /* --- original 11 (kept green) --- */
    RUN_TEST(test_idle_connect_advances);
    RUN_TEST(test_happy_path_spine);
    RUN_TEST(test_death_routes_to_reconnecting);
    RUN_TEST(test_user_stop_routes_to_idle);
    RUN_TEST(test_unknown_pair_is_identity);
    RUN_TEST(test_state_predicates);
    RUN_TEST(test_rms_known_buffer);
    RUN_TEST(test_rms_square_wave);
    RUN_TEST(test_fake_clock);

    /* --- T01..T25 acceptance matrix --- */
    RUN_TEST(test_T01_transportClosed_in_Speaking);
    RUN_TEST(test_T02_noReply_resume);
    RUN_TEST(test_T03_uplinkDead);
    RUN_TEST(test_T04_quota_parks);
    RUN_TEST(test_T05_staleDeadline);
    RUN_TEST(test_T06_death_routing_matrix);
    RUN_TEST(test_T07_userStop_only_path_to_Idle);
    RUN_TEST(test_T08_storm_parks);
    RUN_TEST(test_T09_capped_exponential);
    RUN_TEST(test_T10_healthy_uptime_resets);
    RUN_TEST(test_T11_self_heal_without_tap);
    RUN_TEST(test_T12_interrupted_without_generationComplete);
    RUN_TEST(test_T13_interrupted_in_speaking);
    RUN_TEST(test_T14_toolCall_generation_tagged);
    RUN_TEST(test_tool_result_rearms_reply_watchdog);
    RUN_TEST(test_T15_toolCancel_and_stale_gen_drop);
    RUN_TEST(test_T16_goAway_resumes_with_token);
    RUN_TEST(test_T17_serverChunk_before_boundary);
    RUN_TEST(test_T18_split_brain_dead_uplink);
    RUN_TEST(test_T19_zero_self_barge);
    RUN_TEST(test_T20_talkover_barge_prompt);
    RUN_TEST(test_speaking_echo_never_primes_next_user_turn);
    RUN_TEST(test_T21_silence_threshold_regression);
    RUN_TEST(test_T22_floor_is_pinned_not_adaptive);
    RUN_TEST(test_quiet_hardware_floor_does_not_false_start);
    RUN_TEST(test_T23_cold_start_guard);
    RUN_TEST(test_T24_backpressure_bounded_drop_newest);
    RUN_TEST(test_T25_wouldblock_is_not_death);

    /* --- §4.11 zombie proof + protocol invariants + coverage --- */
    RUN_TEST(test_zombie_unreachable);
    RUN_TEST(test_manual_ptt_turn_boundaries);
    RUN_TEST(test_fatal_exits);
    RUN_TEST(test_reconnect_policy_direct);
    RUN_TEST(test_barge_command_contract);

    /* --- Run 2: §5 resilience monitors (arm/re-arm/fire) --- */
    RUN_TEST(test_keepalive_monitor_arm_rearm_fire);
    RUN_TEST(test_dead_uplink_monitor_arm_rearm_fire);
    RUN_TEST(test_no_reply_watchdog_arm_disarm_fire);
    RUN_TEST(test_capture_pause_monitor_arm_rearm_fire);
    RUN_TEST(test_bp_ring_primitive);
    RUN_TEST(test_monitors_feed_the_machine);

    /* --- Run 2: observability StateSnapshot fold reducer --- */
    RUN_TEST(test_snapshot_counters_and_ring);
    RUN_TEST(test_snapshot_illegal_and_ring_bound);

    /* --- Run 3: L2 Gemini transport (builders/framer/parser + fake ws) --- */
    transport_tests_run();

    /* --- Capstone: orchestrator pump + integrated adversarial self-heal soak --- */
    soak_tests_run();

    return UNITY_END();
}
