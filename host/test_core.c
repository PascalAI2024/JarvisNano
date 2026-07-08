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
#include "jr_dsp/dsp.h"
#include "fake_clock.h"
#include <math.h>
#include <string.h>

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
static void test_resample_length(void)
{
    int16_t in[768], out[600];
    for (int i = 0; i < 768; ++i) { in[i] = (int16_t)i; }
    size_t n = jr_dsp_resample_linear(in, 768, 24000, out, 600, 16000);
    TEST_ASSERT_EQUAL_size_t(512, n);
}
static void test_vad_stub(void)
{
    jr_vad_t v;
    jr_vad_init(&v);
    TEST_ASSERT_TRUE(jr_vad_update(&v, 500.0f));
    TEST_ASSERT_FALSE(jr_vad_update(&v, 0.0f));
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
    e.tool_name = "get_weather";
    e.tool_args = "{}";
    jr_outcome_t r = step(s, e);

    TEST_ASSERT_EQUAL_INT(JR_ST_THINKING, r.next.phase);
    const jr_command_t *d = find_cmd(&r.cmds, JR_CMD_DISPATCH_TOOL_CALL);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT32(7, d->session_gen);
    TEST_ASSERT_EQUAL_UINT32(42, d->call_id);
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
    jr_outcome_t r = step(s, cancel);

    const jr_command_t *c = find_cmd(&r.cmds, JR_CMD_CANCEL_TOOL_CALL);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT32(42, c->call_id);

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
 *  7.3 VAD / barge tests — T19..T23                                     *
 *  This run: the L3/state-level + current TurnPolicy-stub guarantees     *
 *  (zero-self-barge at L3, the barge command contract, turn-boundary     *
 *  contract, playback-adaptive gate, cold-start safety). The full WAV-   *
 *  fixture signal-math assertions land with the L4 adaptive VAD next run.*
 * ===================================================================== */

/* T19 — zero self-barge: raw SpeechStarted edges during Speaking never barge
 * (L3 §4.6), and the TurnPolicy peak-hold guard suppresses the echo-tail
 * transient (mic spikes while playback already dropped). */
static void test_T19_zero_self_barge(void)
{
    /* L3: 10 raw speech edges over the model's turn -> stays Speaking, no barge */
    jr_session_t s = sess_at(JR_ST_SPEAKING);
    for (int i = 0; i < 10; ++i) {
        jr_outcome_t r = step(s, jr_event(JR_EV_SPEECH_STARTED));
        TEST_ASSERT_EQUAL_INT(JR_ST_SPEAKING, r.next.phase);
        TEST_ASSERT_FALSE(has_cmd(&r.cmds, JR_CMD_MUTE_DAC_NOW));
        TEST_ASSERT_FALSE(has_cmd(&r.cmds, JR_CMD_SEND_ACTIVITY_START));
        s = r.next;
    }

    /* L4 stub: peak-hold covers the 60-100ms echo-tail lag */
    jr_turn_policy_t p;
    jr_turn_policy_init(&p);
    jr_turn_policy_eval(&p, 100.0f, 1000.0f, true);          /* peak attacks high */
    jr_turn_decision_t d = jr_turn_policy_eval(&p, 736.0f, 141.0f, true);
    TEST_ASSERT_FALSE(d.is_barge);                            /* echo tail suppressed */
}

/* T20 — THE barge row: BargeDetected in Speaking -> Listening emitting EXACTLY
 * [MuteDacNow, FlushPlaybackRing, SendActivityStart] in that order. (Critical
 * invariant #3.) */
static void test_T20_barge_command_contract(void)
{
    jr_event_t e = jr_event(JR_EV_BARGE_DETECTED);
    e.rms = 736.0f;
    e.playback_peak = 141.0f;
    jr_outcome_t r = step(sess_at(JR_ST_SPEAKING), e);

    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, r.next.phase);
    TEST_ASSERT_EQUAL_INT(JR_CMD_MUTE_DAC_NOW,        r.cmds.cmds[0].kind);
    TEST_ASSERT_EQUAL_INT(JR_CMD_FLUSH_PLAYBACK_RING, r.cmds.cmds[1].kind);
    TEST_ASSERT_EQUAL_INT(JR_CMD_SEND_ACTIVITY_START, r.cmds.cmds[2].kind);
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_ARM_CAPTURE_PAUSE_TIMER));
    TEST_ASSERT_TRUE(has_cmd(&r.cmds, JR_CMD_EMIT_DIAG));
}

/* T21 — turn-boundary regression: inter-word SpeechStarted does NOT end the
 * turn; exactly ONE SpeechEnded commits it; a second is illegal (no double). */
static void test_T21_silence_threshold_regression(void)
{
    jr_outcome_t a = step(sess_at(JR_ST_LISTENING), jr_event(JR_EV_SPEECH_STARTED));
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, a.next.phase); /* inter-word, no commit */

    jr_outcome_t b = step(a.next, jr_event(JR_EV_SPEECH_ENDED));
    TEST_ASSERT_EQUAL_INT(JR_ST_THINKING, b.next.phase);  /* the one real boundary */

    jr_outcome_t c = step(b.next, jr_event(JR_EV_SPEECH_ENDED));
    TEST_ASSERT_TRUE(c.illegal);                          /* no double-commit */
}

/* T22 — the barge gate adapts to the playback (echo) level: the same capture
 * crosses a quiet gate but not a loud one. (Room/echo scaling; the noise-floor
 * room-adaptation lands with the L4 tracker next run.) */
static void test_T22_gate_adapts_to_playback(void)
{
    jr_turn_policy_t lo;
    jr_turn_policy_init(&lo);
    jr_turn_policy_eval(&lo, 50.0f, 200.0f, true);         /* quiet playback */
    jr_turn_decision_t d_lo = jr_turn_policy_eval(&lo, 500.0f, 100.0f, true);
    TEST_ASSERT_TRUE(d_lo.is_barge);                       /* talk-over crosses */

    jr_turn_policy_t hi;
    jr_turn_policy_init(&hi);
    jr_turn_policy_eval(&hi, 50.0f, 2000.0f, true);        /* loud playback */
    jr_turn_decision_t d_hi = jr_turn_policy_eval(&hi, 500.0f, 300.0f, true);
    TEST_ASSERT_FALSE(d_hi.is_barge);                      /* same capture = echo */
}

/* T23 — cold-start safety at L3: a fresh Listening does not spuriously commit a
 * turn on a bare SpeechStarted; a cold-AEC SpeechStarted in Speaking is not a
 * self-barge. (The 500ms floor-unconverged guard window is L4, next run.) */
static void test_T23_cold_start_guard(void)
{
    jr_outcome_t l = step(sess_at(JR_ST_LISTENING), jr_event(JR_EV_SPEECH_STARTED));
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, l.next.phase); /* no spurious commit */

    jr_outcome_t sp = step(sess_at(JR_ST_SPEAKING), jr_event(JR_EV_SPEECH_STARTED));
    TEST_ASSERT_EQUAL_INT(JR_ST_SPEAKING, sp.next.phase); /* no cold-start barge */
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
    RUN_TEST(test_resample_length);
    RUN_TEST(test_vad_stub);
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
    RUN_TEST(test_T15_toolCancel_and_stale_gen_drop);
    RUN_TEST(test_T16_goAway_resumes_with_token);
    RUN_TEST(test_T17_serverChunk_before_boundary);
    RUN_TEST(test_T18_split_brain_dead_uplink);
    RUN_TEST(test_T19_zero_self_barge);
    RUN_TEST(test_T20_barge_command_contract);
    RUN_TEST(test_T21_silence_threshold_regression);
    RUN_TEST(test_T22_gate_adapts_to_playback);
    RUN_TEST(test_T23_cold_start_guard);
    RUN_TEST(test_T24_backpressure_bounded_drop_newest);
    RUN_TEST(test_T25_wouldblock_is_not_death);

    /* --- §4.11 zombie proof + protocol invariants + coverage --- */
    RUN_TEST(test_zombie_unreachable);
    RUN_TEST(test_manual_ptt_turn_boundaries);
    RUN_TEST(test_fatal_exits);
    RUN_TEST(test_reconnect_policy_direct);

    return UNITY_END();
}
