/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_soak.c — the Phase-1 CAPSTONE: the integrated orchestrator-pump +
 * adversarial self-heal soak.
 *
 * The other 73 host tests prove every EDGE in isolation (each transition row,
 * each monitor arm/fire, the framer's would-block survival, the parser's nasty
 * sequences). THIS file proves the SYSTEM: that the L3 machine (session.c) + the
 * 6 resilience monitors (monitors.c) + the real L2 Gemini transport
 * (gemini_live.c: framer + parser) COMPOSE, behind their ports, into the pure
 * single-writer pump (orchestrator.c) — and that the ASSEMBLED core, driven by a
 * fake clock and a scriptable fake ws-transport, ALWAYS self-heals to a
 * talking-capable state and can NEVER settle into v4's "doesn't talk unless I
 * type" permanent silence.
 *
 * Wiring (all ESP-IDF-free, all deterministic):
 *   - jr_orch_t                 owns the SessionState + monitors + backoff timer
 *                               + observability snapshot (the pure pump);
 *   - jr_gemini_client_t        the REAL L2 transport (framer + parser) over the
 *                               fake ws-transport — the would-block flood, the
 *                               split-brain soft-fails, the goAway/error/heartbeat
 *                               frames all flow through production transport code;
 *   - soak_io()                 the injected jr_orch_io_t: poll_inbound pumps the
 *                               client + maps its events to jr_event_t; exec runs
 *                               the transport/DAC side-effects and scripts the
 *                               server's responses on the fake socket.
 *
 * THE SELF-HEAL INVARIANT (asserted continuously): after ANY induced fault and a
 * bounded amount of fast-forwarded time, the pump returns to a talking-capable
 * config (Listening/Thinking/Speaking) OR a legitimate rest (Backoff-parked,
 * recoverable by a tap). It is NEVER a zombie (a non-final state with no armed
 * deadline), never Live against a dead socket, never Idle without a UserStop.
 */
#include "unity.h"
#include "jr_core/orchestrator.h"
#include "jr_core/session.h"
#include "jr_transport/gemini_live.h"
#include "fake_ws_transport.h"
#include "fake_clock.h"

#include <string.h>
#include <stdio.h>

/* ===================================================================== *
 *  scripted server frames (static literals: they outlive every recv)    *
 * ===================================================================== */
static const char *FRAME_SETUP_COMPLETE = "{\"setupComplete\":{}}";
static const char *FRAME_AUDIO =
    "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":"
    "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAAAAAAA\"}}]}}}";
static const char *FRAME_TURN_DONE  = "{\"serverContent\":{\"turnComplete\":true}}";
static const char *FRAME_HEARTBEAT  =
    "{\"sessionResumptionUpdate\":{\"newHandle\":\"h9\",\"resumable\":true}}";
static const char *FRAME_GOAWAY     = "{\"goAway\":{\"timeLeft\":\"30s\"}}";
static const char *FRAME_ERR_QUOTA  = "{\"error\":{\"code\":429,\"message\":\"quota\"}}";
static const char *FRAME_ERR_TRANS  = "{\"error\":{\"code\":503,\"message\":\"backend\"}}";

/* ===================================================================== *
 *  the host "device": a real transport over the fake ws + the pump's io *
 * ===================================================================== */
#define SOAK_INQ_CAP 128

typedef struct {
    fake_ws_t          ws;
    jr_gemini_client_t client;
    jr_clock_t         clk;

    /* mapped inbound event queue (rich transport events + synthetic ws signals) */
    jr_event_t inq[SOAK_INQ_CAP];
    size_t     inq_head, inq_count;

    /* fault control */
    int      fail_next_connect;   /* >0: the next Connect delivers a drop instead */
    uint32_t last_resumable_token;/* last usable handle seen (for goAway resume)   */

    /* observed side effects */
    bool     dac_muted;
    bool     capturing;
    bool     socket_open;
    uint32_t playback_fed;
    uint32_t last_connect_token;  /* token the last Connect carried (resume proof) */
    uint32_t connect_count;

    jr_pcm_t dummy_pcm[8];        /* a valid non-null pointer for FeedPlayback     */
} soak_dev_t;

/* fresh clock each call — all fake clocks read the one shared counter */
static uint64_t soak_now(void)
{
    jr_clock_t c = fake_clock_make();
    return jr_clock_now_ms(&c);
}

static void soak_inq_push(soak_dev_t *d, jr_event_t e)
{
    if (d->inq_count >= SOAK_INQ_CAP) {
        return;                    /* bounded; never reached in these scenarios */
    }
    size_t tail = (d->inq_head + d->inq_count) % SOAK_INQ_CAP;
    d->inq[tail] = e;
    d->inq_count++;
}

static jr_error_kind_t map_gem_err(jr_gemini_error_kind_t k)
{
    switch (k) {
    case JR_GEMINI_ERRK_QUOTA:     return JR_ERRK_QUOTA;
    case JR_GEMINI_ERRK_AUTH:      return JR_ERRK_AUTH;
    case JR_GEMINI_ERRK_PROTOCOL:  return JR_ERRK_PROTOCOL;
    case JR_GEMINI_ERRK_TRANSIENT: return JR_ERRK_TRANSIENT;
    default:                       return JR_ERRK_UNKNOWN;
    }
}

/* Map ONE rich transport event to the L3 event vocabulary and enqueue it. The
 * parser owns the pcm/tool-arg buffers only for the callback's duration (they are
 * freed right after in jr_gemini_pump_rx), so we retain NO parser pointers:
 * audio points at a dev-owned dummy buffer, tool strings are literals, tokens are
 * plain uint32s. */
static void soak_rich_cb(void *u, const jr_gemini_event_t *ge)
{
    soak_dev_t *d = (soak_dev_t *)u;
    jr_event_t e = jr_event(JR_EV_HEARTBEAT);   /* default: any frame is liveness */
    switch (ge->kind) {
    case JR_GEV_SETUP_COMPLETE:
        e = jr_event(JR_EV_SETUP_COMPLETE);
        e.resumption_token = d->last_resumable_token;
        break;
    case JR_GEV_AUDIO_CHUNK:
        e = jr_event(JR_EV_SERVER_AUDIO_CHUNK);
        e.pcm = d->dummy_pcm;
        e.pcm_len = ge->pcm_len;
        e.sample_rate = ge->sample_rate;
        break;
    case JR_GEV_INTERRUPTED:
        e = jr_event(JR_EV_SERVER_INTERRUPTED);
        break;
    case JR_GEV_TURN_COMPLETE:
    case JR_GEV_GENERATION_COMPLETE:
        e = jr_event(JR_EV_SERVER_TURN_COMPLETE);
        break;
    case JR_GEV_TOOL_CALL:
        e = jr_event(JR_EV_SERVER_TOOL_CALL);
        e.call_id = ge->call_id;
        e.tool_name = "tool";
        e.tool_args = "{}";
        break;
    case JR_GEV_TOOL_CANCEL:
        e = jr_event(JR_EV_SERVER_TOOL_CALL);
        e.is_cancellation = true;
        e.call_id = ge->call_id;
        break;
    case JR_GEV_GO_AWAY:
        e = jr_event(JR_EV_SERVER_GO_AWAY);
        e.resumption_token = d->last_resumable_token;
        break;
    case JR_GEV_RESUMPTION_UPDATE:
        if (ge->resumable && ge->resumption_token) {
            d->last_resumable_token = ge->resumption_token;
        }
        e = jr_event(JR_EV_HEARTBEAT);
        e.resumption_token = ge->resumption_token;
        break;
    case JR_GEV_ERROR:
        e = jr_event(JR_EV_SERVER_ERROR);
        e.error_kind = map_gem_err(ge->error_kind);
        break;
    case JR_GEV_TEXT:
    case JR_GEV_UNKNOWN:
    default:
        e = jr_event(JR_EV_HEARTBEAT);
        break;
    }
    soak_inq_push(d, e);
}

/* poll_inbound: hand back one queued mapped event; when the queue is empty, pump
 * ONE ws frame through the real parser (its rich_cb enqueues the mapped events),
 * then hand back the first. Returns false only when both are dry. */
static bool soak_poll(void *ctx, jr_event_t *out)
{
    soak_dev_t *d = (soak_dev_t *)ctx;
    if (d->inq_count == 0) {
        jr_gemini_pump_rx(&d->client);        /* may enqueue 0..N via soak_rich_cb */
    }
    if (d->inq_count == 0) {
        return false;
    }
    *out = d->inq[d->inq_head];
    d->inq_head = (d->inq_head + 1) % SOAK_INQ_CAP;
    d->inq_count--;
    return true;
}

/* exec: run one externally-visible command. Transport commands script the fake
 * server's responses (the handshake / close); DAC + capture commands record the
 * observed side effects. */
static void soak_exec(void *ctx, const jr_command_t *cmd)
{
    soak_dev_t *d = (soak_dev_t *)ctx;
    switch (cmd->kind) {
    case JR_CMD_CONNECT:
        d->connect_count++;
        d->last_connect_token = cmd->resumption_token;
        if (d->fail_next_connect > 0) {
            d->fail_next_connect--;           /* this reconnect attempt drops */
            d->ws.state = JR_WS_CLOSED;
            d->socket_open = false;
            soak_inq_push(d, jr_event(JR_EV_TRANSPORT_CLOSED));
        } else {
            d->ws.state = JR_WS_OPEN;
            d->socket_open = true;
            soak_inq_push(d, jr_event(JR_EV_CONNECTED)); /* ws-open signal */
        }
        break;
    case JR_CMD_SEND_SETUP:
        if (d->socket_open) {
            fake_ws_push_inbox(&d->ws, FRAME_SETUP_COMPLETE); /* server acks setup */
        }
        break;
    case JR_CMD_CLOSE_TRANSPORT:
        d->ws.state = JR_WS_CLOSED;
        d->ws.inbox_head = 0;
        d->ws.inbox_count = 0;                /* socket gone: drop in-flight frames */
        d->socket_open = false;
        d->inq_head = 0;
        d->inq_count = 0;                     /* drop mapped-but-undrained events   */
        d->ws.close_calls++;
        break;
    case JR_CMD_MUTE_DAC_NOW:      d->dac_muted = true;  break;
    case JR_CMD_UNMUTE_DAC:        d->dac_muted = false; break;
    case JR_CMD_FLUSH_PLAYBACK_RING:                     break;
    case JR_CMD_FEED_PLAYBACK:     d->playback_fed++;    break;
    case JR_CMD_START_CAPTURE:     d->capturing = true;  break;
    case JR_CMD_PAUSE_CAPTURE:     d->capturing = false; break;
    case JR_CMD_SEND_ACTIVITY_START:
    case JR_CMD_SEND_ACTIVITY_END:
    case JR_CMD_SEND_AUDIO_STREAM_END:
    case JR_CMD_SEND_TEXT:
    case JR_CMD_SEND_AUDIO:
        /* exercise the REAL framer's send path for control/turn frames */
        (void)jr_gemini_send_frame(&d->client, "{\"c\":1}", 7);
        break;
    default:
        /* DispatchToolCall / CancelToolCall / EmitDiag / PublishSnapshot: no-op */
        break;
    }
}

static void soak_dev_init(soak_dev_t *d, jr_orch_t *app, jr_vad_mode_t vad)
{
    memset(d, 0, sizeof *d);
    d->clk = fake_clock_make();
    fake_ws_init(&d->ws);
    d->ws.state = JR_WS_CLOSED;               /* not connected until Connect */

    jr_gemini_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.vad_mode = vad;
    jr_gemini_client_init(&d->client, fake_ws_make(&d->ws), d->clk, &cfg);
    jr_gemini_client_set_event_cb(&d->client, soak_rich_cb, d);

    jr_orch_io_t io;
    io.ctx = d;
    io.poll_inbound = soak_poll;
    io.exec = soak_exec;
    jr_orch_init(app, fake_clock_make(), io, vad);
}

/* boot Idle -> ... -> Live.Listening driven ONLY by UserStart + the transport
 * cascade (no hand-forced states). One inject + one fixpoint step is enough:
 * Connect -> Connected -> SendSetup -> setupComplete -> Listening all settle at
 * the same tick. */
static void soak_boot_to_listening(soak_dev_t *d, jr_orch_t *app)
{
    (void)d;
    jr_orch_inject(app, jr_event(JR_EV_USER_START), soak_now());
    jr_orch_step(app, soak_now());
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, jr_orch_phase(app));
}

static void soak_to_speaking(soak_dev_t *d, jr_orch_t *app)
{
    fake_ws_push_inbox(&d->ws, FRAME_AUDIO);
    jr_orch_step(app, soak_now());
    TEST_ASSERT_EQUAL_INT(JR_ST_SPEAKING, jr_orch_phase(app));
}

/* ===================================================================== *
 *  soak instrumentation + the self-heal recovery driver                 *
 * ===================================================================== */
typedef struct {
    uint32_t turns;
    uint32_t faults;
    uint32_t max_silent;        /* max consecutive non-talking-capable ticks */
    uint32_t silent_run;        /* current run                               */
    uint32_t permanent_silence; /* MUST stay 0                               */
    uint32_t zombie_seen;       /* MUST stay 0                               */
} soak_stats_t;

static void soak_tick_account(jr_orch_t *app, soak_stats_t *st)
{
    if (jr_orch_is_zombie(app)) {
        st->zombie_seen++;
    }
    if (jr_orch_is_talking_capable(app)) {
        st->silent_run = 0;
    } else {
        st->silent_run++;
        if (st->silent_run > st->max_silent) {
            st->max_silent = st->silent_run;
        }
    }
}

/* Drive the assembled core back to a talking-capable state after a fault, within
 * a bounded amount of fast-forwarded time. Reconnecting auto-advances on its
 * backoff timer; a parked Backoff needs a user tap (proving recoverability). The
 * self-heal invariant (no zombie, bounded silence) is checked every tick. */
static void soak_recover(soak_dev_t *d, jr_orch_t *app, soak_stats_t *st)
{
    unsigned budget = 0;
    while (!jr_orch_is_talking_capable(app) && budget < 4000u) {
        budget++;
        soak_tick_account(app, st);                 /* count the tick that BEGINS silent */
        jr_state_t p = jr_orch_phase(app);

        if (app->backoff.armed) {
            uint64_t fa = app->backoff.fire_at;
            uint64_t now = soak_now();
            if (fa > now) {
                fake_clock_advance(fa - now + 1);   /* jump to the backoff deadline */
            }
        } else if (p == JR_ST_BACKOFF || p == JR_ST_FATAL) {
            jr_orch_inject(app, jr_event(JR_EV_TAP), soak_now()); /* park-for-tap exit */
        } else {
            fake_clock_advance(50);                 /* nudge Connecting/Handshaking */
        }
        jr_orch_step(app, soak_now());

        TEST_ASSERT_FALSE(jr_orch_is_zombie(app));  /* continuous zombie proof */
    }
    if (!jr_orch_is_talking_capable(app)) {
        st->permanent_silence++;                    /* the failure we prove == 0 */
    }
    (void)d;
}

/* ===================================================================== *
 *  the adversarial fault injectors (each leaves the core recoverable)   *
 * ===================================================================== */

/* Transport drop: the socket dies. A synthetic TransportClosed -> DEATH. */
static void fault_transport_drop(soak_dev_t *d, jr_orch_t *app)
{
    d->ws.state = JR_WS_CLOSED;
    d->ws.inbox_head = 0;
    d->ws.inbox_count = 0;
    d->socket_open = false;
    jr_orch_inject(app, jr_event(JR_EV_TRANSPORT_CLOSED), soak_now());
}

/* Heartbeat gap: no server frame past the live deadline -> KeepaliveMonitor
 * fires StaleDeadline -> DEATH (the stale-idle-session cure). */
static void fault_heartbeat_gap(soak_dev_t *d, jr_orch_t *app)
{
    (void)d;
    fake_clock_advance(JR_LIVE_DEADLINE_MS + 1000);
    jr_orch_step(app, soak_now());
}

/* Would-block FLOOD: a starved uplink. The REAL framer buffers/drops-newest
 * (bounded memory) and KEEPS the connection — no DEATH, the machine stays Live. */
static void fault_wouldblock_flood(soak_dev_t *d, jr_orch_t *app)
{
    d->ws.send_mode = FWS_WOULD_BLOCK;
    for (int i = 0; i < 64; ++i) {
        jr_err_t r = jr_gemini_send_frame(&d->client, "micframe", 8);
        jr_orch_report_tx(app, r);              /* would-block: ignored (backpressure) */
    }
    d->ws.send_mode = FWS_OK;
    (void)jr_gemini_flush(&d->client);
    jr_orch_step(app, soak_now());              /* still Live */
}

/* Split-brain: the uplink is dead (soft-fails) while the downlink is alive
 * (heartbeats). The DeadUplinkMonitor fires at TX_FAIL_RESUME; the
 * KeepaliveMonitor does NOT (its clock is re-armed every inbound frame). */
static void fault_split_brain(soak_dev_t *d, jr_orch_t *app)
{
    d->ws.send_mode = FWS_SOFT_FAIL;
    for (unsigned i = 0; i < JR_TX_FAIL_RESUME; ++i) {
        (void)jr_gemini_send_frame(&d->client, "mic", 3); /* soft-fail: counts */
        jr_orch_report_tx(app, JR_ERR_FAIL);              /* feed the owned monitor */
        fake_ws_push_inbox(&d->ws, FRAME_HEARTBEAT);      /* downlink alive */
        fake_clock_advance(15);
        if (i == JR_TX_FAIL_RESUME - 1u) {
            /* at the moment the uplink is declared dead, the downlink keepalive
             * is still armed and NOT stale — the split-brain is real. */
            TEST_ASSERT_TRUE(app->keepalive.armed);
            TEST_ASSERT_FALSE(jr_keepalive_poll(&app->keepalive, soak_now()).fired);
        }
        jr_orch_step(app, soak_now());          /* fires UplinkDead on the 25th */
    }
    d->ws.send_mode = FWS_OK;
}

/* goAway: a graceful server disconnect carrying a resumption handle -> DEATH,
 * retaining the token so the reconnect resumes the session. */
static void fault_goaway(soak_dev_t *d, jr_orch_t *app)
{
    fake_ws_push_inbox(&d->ws, FRAME_HEARTBEAT);  /* a usable handle first */
    jr_orch_step(app, soak_now());
    fake_ws_push_inbox(&d->ws, FRAME_GOAWAY);
    jr_orch_step(app, soak_now());
}

/* Quota error: parks in Backoff with the ~45 s cool-off timer. */
static void fault_quota(soak_dev_t *d, jr_orch_t *app)
{
    fake_ws_push_inbox(&d->ws, FRAME_ERR_QUOTA);
    jr_orch_step(app, soak_now());
}

/* Transient error: routes to Reconnecting (auto-heal). */
static void fault_transient(soak_dev_t *d, jr_orch_t *app)
{
    fake_ws_push_inbox(&d->ws, FRAME_ERR_TRANS);
    jr_orch_step(app, soak_now());
}

/* NoReply: 20 s in Thinking with no server audio -> back to Listening (resume).
 * NOT a death. */
static void fault_noreply(soak_dev_t *d, jr_orch_t *app)
{
    /* re-arm keepalive so only the 20 s no-reply watchdog trips, not the 45 s
     * stale deadline. */
    fake_ws_push_inbox(&d->ws, FRAME_HEARTBEAT);
    jr_orch_step(app, soak_now());

    jr_state_t p = jr_orch_phase(app);
    if (p == JR_ST_SPEAKING) {
        fake_ws_push_inbox(&d->ws, FRAME_TURN_DONE);
        jr_orch_step(app, soak_now());          /* -> Listening */
        p = jr_orch_phase(app);
    }
    if (p == JR_ST_LISTENING) {
        jr_orch_inject(app, jr_event(JR_EV_SPEECH_ENDED), soak_now()); /* -> Thinking */
    }
    /* if already Thinking, fall straight through */
    fake_clock_advance(JR_NOREPLY_MS + 500);
    jr_orch_step(app, soak_now());              /* NoReplyTimeout -> Listening */
}

/* ===================================================================== *
 *  NAMED adversarial fault tests (each asserts its self-heal outcome)   *
 * ===================================================================== */

/* The pump COMPOSES the happy path: Idle -> Connecting -> Handshaking ->
 * Listening, driven only by UserStart + the fake transport, with a real armed
 * keepalive and the capture/DAC side effects executed. */
static void test_soak_pump_boots_to_listening(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    TEST_ASSERT_EQUAL_INT(JR_ST_IDLE, jr_orch_phase(&app));

    soak_boot_to_listening(&d, &app);

    TEST_ASSERT_TRUE(jr_orch_is_talking_capable(&app));
    TEST_ASSERT_TRUE(app.keepalive.armed);       /* an armed forward-progress deadline */
    TEST_ASSERT_FALSE(jr_orch_is_zombie(&app));
    TEST_ASSERT_TRUE(d.capturing);               /* StartCapture executed */
    TEST_ASSERT_FALSE(d.dac_muted);              /* UnmuteDac executed    */
    TEST_ASSERT_TRUE(d.socket_open);
}

/* Fault: transport drop mid-Speaking -> reconnect -> resumes talking (no
 * permanent silence, no zombie). */
static void test_soak_transport_drop_midspeaking_resumes(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    soak_boot_to_listening(&d, &app);
    soak_to_speaking(&d, &app);

    fault_transport_drop(&d, &app);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, jr_orch_phase(&app));
    TEST_ASSERT_FALSE(d.socket_open);
    TEST_ASSERT_TRUE(app.backoff.armed);          /* an armed retry deadline */
    TEST_ASSERT_FALSE(jr_orch_is_zombie(&app));

    soak_stats_t st;
    memset(&st, 0, sizeof st);
    soak_recover(&d, &app, &st);

    TEST_ASSERT_TRUE(jr_orch_is_talking_capable(&app));
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, jr_orch_phase(&app));
    TEST_ASSERT_TRUE(d.socket_open);              /* reconnected socket */
    TEST_ASSERT_EQUAL_UINT32(0, st.permanent_silence);
    TEST_ASSERT_TRUE(jr_orch_snapshot(&app)->reconnects >= 1);
}

/* Fault: heartbeat gap while Live -> KeepaliveMonitor StaleDeadline -> reconnect. */
static void test_soak_heartbeat_gap_reconnects(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    soak_boot_to_listening(&d, &app);

    fault_heartbeat_gap(&d, &app);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, jr_orch_phase(&app));
    TEST_ASSERT_EQUAL_INT(JR_EV_STALE_DEADLINE, jr_orch_snapshot(&app)->last_reason);
    TEST_ASSERT_TRUE(jr_orch_snapshot(&app)->deaths >= 1);

    soak_stats_t st;
    memset(&st, 0, sizeof st);
    soak_recover(&d, &app, &st);
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, jr_orch_phase(&app));
    TEST_ASSERT_EQUAL_UINT32(0, st.permanent_silence);
}

/* Fault: would-block flood during Live -> connection survives (drop-newest,
 * bounded memory), no death, playback resumes. */
static void test_soak_wouldblock_flood_survives(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    soak_boot_to_listening(&d, &app);

    uint32_t drops_before = d.client.live.tx_drops;
    fault_wouldblock_flood(&d, &app);

    TEST_ASSERT_TRUE(jr_orch_is_talking_capable(&app));           /* never died */
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, jr_orch_phase(&app));
    TEST_ASSERT_TRUE(jr_gemini_txq_depth(&d.client) <= JR_GEMINI_TXQ_DEPTH); /* bounded */
    TEST_ASSERT_TRUE(d.client.live.tx_drops > drops_before);      /* drop-newest fired */
    TEST_ASSERT_EQUAL_UINT(0, d.ws.close_calls);                  /* never torn down */

    /* playback resumes cleanly afterwards */
    soak_to_speaking(&d, &app);
    TEST_ASSERT_TRUE(d.playback_fed > 0);
}

/* Fault: split-brain (uplink dead / downlink alive) -> DeadUplinkMonitor fires
 * at threshold -> reconnect (while keepalive stayed armed and fresh). */
static void test_soak_split_brain_uplink_dead_reconnects(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    soak_boot_to_listening(&d, &app);

    fault_split_brain(&d, &app);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, jr_orch_phase(&app));
    TEST_ASSERT_EQUAL_INT(JR_EV_UPLINK_DEAD, jr_orch_snapshot(&app)->last_reason);

    soak_stats_t st;
    memset(&st, 0, sizeof st);
    soak_recover(&d, &app, &st);
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, jr_orch_phase(&app));
    TEST_ASSERT_EQUAL_UINT32(0, st.permanent_silence);
}

/* Fault: goAway -> graceful reconnect using the resumption handle. */
static void test_soak_goaway_reconnects_with_token(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    soak_boot_to_listening(&d, &app);

    fault_goaway(&d, &app);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, jr_orch_phase(&app));
    TEST_ASSERT_NOT_EQUAL(0, app.session.resumption_token);   /* handle retained */
    uint32_t tok = app.session.resumption_token;

    soak_stats_t st;
    memset(&st, 0, sizeof st);
    soak_recover(&d, &app, &st);
    TEST_ASSERT_TRUE(jr_orch_is_talking_capable(&app));
    TEST_ASSERT_EQUAL_UINT32(tok, d.last_connect_token);       /* resume handle used */
    TEST_ASSERT_EQUAL_UINT32(0, st.permanent_silence);
}

/* Fault: NoReplyTimeout in Thinking -> back to Listening (resume, not a death). */
static void test_soak_noreply_thinking_to_listening(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    soak_boot_to_listening(&d, &app);

    jr_orch_inject(&app, jr_event(JR_EV_SPEECH_ENDED), soak_now());
    TEST_ASSERT_EQUAL_INT(JR_ST_THINKING, jr_orch_phase(&app));
    TEST_ASSERT_TRUE(app.no_reply.armed);

    fake_clock_advance(JR_NOREPLY_MS + 500);
    jr_orch_step(&app, soak_now());

    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, jr_orch_phase(&app));   /* resumed */
    TEST_ASSERT_EQUAL_UINT32(0, jr_orch_snapshot(&app)->deaths);   /* no death */
    TEST_ASSERT_FALSE(jr_orch_is_zombie(&app));
}

/* Fault: quota error -> Backoff park with a 45 s cool-off, and PROVE it is
 * recoverable (a subsequent UserStart re-arms and talks). */
static void test_soak_quota_backoff_recoverable(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    soak_boot_to_listening(&d, &app);

    uint64_t t0 = soak_now();
    fault_quota(&d, &app);
    TEST_ASSERT_EQUAL_INT(JR_ST_BACKOFF, jr_orch_phase(&app));
    TEST_ASSERT_TRUE(app.backoff.armed);                              /* cool-off timer */
    TEST_ASSERT_EQUAL_UINT64(t0 + JR_QUOTA_COOLOFF_MS, app.backoff.fire_at);
    TEST_ASSERT_EQUAL_INT(JR_ERRK_QUOTA, jr_orch_snapshot(&app)->last_error_kind);

    /* a user tap re-arms and talks (the park-for-tap exit) */
    jr_orch_inject(&app, jr_event(JR_EV_USER_START), soak_now());
    TEST_ASSERT_EQUAL_INT(JR_ST_CONNECTING, jr_orch_phase(&app));
    jr_orch_step(&app, soak_now());
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, jr_orch_phase(&app));
    TEST_ASSERT_EQUAL_UINT16(0, app.session.fail_count);             /* clean slate */
}

/* Fault: death STORM (rapid repeated drops with failing reconnects) -> backoff
 * caps per policy -> parks after N -> recoverable by a user tap. */
static void test_soak_death_storm_parks_then_tap_recovers(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    soak_boot_to_listening(&d, &app);           /* clock frozen -> uptime stays < 15 s */

    /* first death, then keep retrying-and-dying via Tap (no clock advance) so the
     * fail_count storm is measured against a never-healthy uptime. */
    jr_orch_inject(&app, jr_event(JR_EV_TRANSPORT_CLOSED), soak_now());
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, jr_orch_phase(&app));

    int guard = 0;
    while (jr_orch_phase(&app) != JR_ST_BACKOFF && guard++ < 20) {
        d.fail_next_connect = 1;                /* the retried Connect drops again */
        jr_orch_inject(&app, jr_event(JR_EV_TAP), soak_now());  /* accelerate the retry */
        jr_orch_step(&app, soak_now());                          /* Connect -> drop -> DEATH */
        TEST_ASSERT_FALSE(jr_orch_is_zombie(&app));
    }

    TEST_ASSERT_EQUAL_INT(JR_ST_BACKOFF, jr_orch_phase(&app));   /* parked (storm) */
    TEST_ASSERT_FALSE(app.backoff.armed);                        /* parked: NO timer */
    TEST_ASSERT_TRUE(app.session.fail_count >= JR_PARK_AFTER);
    TEST_ASSERT_FALSE(jr_orch_is_zombie(&app));                  /* Backoff is a rest */

    /* a human tap un-parks and talks; the storm counter resets */
    d.fail_next_connect = 0;
    jr_orch_inject(&app, jr_event(JR_EV_TAP), soak_now());
    jr_orch_step(&app, soak_now());
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, jr_orch_phase(&app));
    TEST_ASSERT_EQUAL_UINT16(0, app.session.fail_count);
}

/* Fault: transient server error -> Reconnecting -> auto-heals to talking. */
static void test_soak_transient_error_reconnects(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    soak_boot_to_listening(&d, &app);

    fault_transient(&d, &app);
    TEST_ASSERT_EQUAL_INT(JR_ST_RECONNECTING, jr_orch_phase(&app));

    soak_stats_t st;
    memset(&st, 0, sizeof st);
    soak_recover(&d, &app, &st);
    TEST_ASSERT_EQUAL_INT(JR_ST_LISTENING, jr_orch_phase(&app));
    TEST_ASSERT_EQUAL_UINT32(0, st.permanent_silence);
}

/* ===================================================================== *
 *  THE SOAK — deterministic pseudo-random adversarial fault storm       *
 * ===================================================================== *
 * A deterministic in-C LCG (no wall clock, no libc rand) drives thousands of
 * simulated turns with randomly-induced faults from the matrix over fast-
 * forwarded sim-time. After EVERY step the self-heal invariant is asserted; the
 * permanent-silence count MUST be 0 and the max consecutive silent-ticks MUST
 * stay bounded.
 */
static uint32_t lcg_next(uint32_t *s)
{
    *s = (*s) * 1664525u + 1013904223u;   /* Numerical Recipes LCG (deterministic) */
    return *s;
}

/* A weighted fault table — cheap faults dominate so turns accumulate into the
 * thousands while the (time-expensive) stale/quota faults still recur. */
static const uint8_t FAULT_PICK[] = {
    0, 0, 0, 0, 0, 0,   /* transport drop        */
    1,                  /* heartbeat gap (stale) */
    2, 2, 2, 2, 2,      /* would-block flood     */
    3, 3, 3,            /* split-brain           */
    4, 4, 4,            /* goAway                */
    5,                  /* quota                 */
    6, 6, 6, 6,         /* transient error       */
    7, 7,               /* no-reply timeout      */
};
#define FAULT_PICK_N ((int)(sizeof(FAULT_PICK) / sizeof(FAULT_PICK[0])))

static void soak_inject_fault(soak_dev_t *d, jr_orch_t *app, uint32_t *seed)
{
    uint8_t which = FAULT_PICK[lcg_next(seed) % FAULT_PICK_N];
    switch (which) {
    case 0: fault_transport_drop(d, app);  break;
    case 1: fault_heartbeat_gap(d, app);   break;
    case 2: fault_wouldblock_flood(d, app);break;
    case 3: fault_split_brain(d, app);     break;
    case 4: fault_goaway(d, app);          break;
    case 5: fault_quota(d, app);           break;
    case 6: fault_transient(d, app);       break;
    default:fault_noreply(d, app);         break;
    }
}

/* Keep the conversation flowing (and keepalive fresh) with a server heartbeat +
 * an occasional full mic->model->done exchange. */
static void soak_normal_turn(soak_dev_t *d, jr_orch_t *app, uint32_t *seed)
{
    uint32_t adv = 80u + (lcg_next(seed) % 420u);   /* 80..499 ms */
    fake_clock_advance(adv);
    fake_ws_push_inbox(&d->ws, FRAME_HEARTBEAT);
    jr_orch_step(app, soak_now());

    if ((lcg_next(seed) % 3u) == 0u && jr_orch_phase(app) == JR_ST_LISTENING) {
        jr_orch_inject(app, jr_event(JR_EV_SPEECH_ENDED), soak_now());  /* -> Thinking */
        fake_clock_advance(60);
        fake_ws_push_inbox(&d->ws, FRAME_AUDIO);                        /* server audio */
        jr_orch_step(app, soak_now());                                  /* -> Speaking */
        fake_clock_advance(60);
        fake_ws_push_inbox(&d->ws, FRAME_TURN_DONE);                    /* turnComplete */
        jr_orch_step(app, soak_now());                                  /* -> Listening */
    }
}

static void test_soak_randomized_selfheal(void)
{
    soak_dev_t d;
    jr_orch_t app;
    soak_stats_t st;
    memset(&st, 0, sizeof st);

    soak_dev_init(&d, &app, JR_VAD_MANUAL_LOCAL_RMS);
    soak_boot_to_listening(&d, &app);

    uint32_t seed = 0x1234BEEFu;                 /* deterministic — no wall clock */
    const uint64_t SOAK_MS = 1800000u;           /* >= 30 minutes of sim-time */
    const uint32_t MIN_TURNS = 3000u;            /* thousands of turns */
    const uint32_t MAX_TURNS = 200000u;          /* hard runtime cap */

    while ((st.turns < MIN_TURNS || soak_now() < SOAK_MS) && st.turns < MAX_TURNS) {
        st.turns++;

        bool talk = jr_orch_is_talking_capable(&app);
        uint32_t roll = lcg_next(&seed) % 100u;

        if (talk && roll < 22u) {                /* ~22% of live turns induce a fault */
            st.faults++;
            soak_inject_fault(&d, &app, &seed);
            soak_recover(&d, &app, &st);          /* prove it self-heals */
        } else if (talk) {
            soak_normal_turn(&d, &app, &seed);
        } else {
            soak_recover(&d, &app, &st);          /* defensive: heal anything unsettled */
        }

        /* the self-heal invariant, asserted every single turn */
        TEST_ASSERT_FALSE(jr_orch_is_zombie(&app));
        TEST_ASSERT_NOT_EQUAL(JR_ST_IDLE,     jr_orch_phase(&app)); /* no UserStop issued */
        TEST_ASSERT_NOT_EQUAL(JR_ST_DRAINING, jr_orch_phase(&app));
        TEST_ASSERT_NOT_EQUAL(JR_ST_FATAL,    jr_orch_phase(&app));
        TEST_ASSERT_TRUE(jr_orch_is_talking_capable(&app) || jr_orch_is_resting(&app));
        soak_tick_account(&app, &st);
    }

    /* THE PROOF */
    TEST_ASSERT_EQUAL_UINT32(0, st.permanent_silence);   /* never permanently silent */
    TEST_ASSERT_EQUAL_UINT32(0, st.zombie_seen);         /* never a zombie */
    TEST_ASSERT_TRUE(st.max_silent < 64u);               /* silent-ticks stayed bounded */
    TEST_ASSERT_TRUE(st.turns >= MIN_TURNS);             /* thousands of turns */
    TEST_ASSERT_TRUE(soak_now() >= SOAK_MS);             /* >= 30 min of sim-time */
    TEST_ASSERT_TRUE(jr_orch_snapshot(&app)->reconnects > 0); /* it truly reconnected */
    TEST_ASSERT_TRUE(jr_orch_is_talking_capable(&app) || jr_orch_is_resting(&app));

    printf("[SOAK] turns=%u faults=%u deaths=%u reconnects=%u "
           "permanent_silence=%u max_silent_ticks=%u sim_ms=%llu (%llu min) steps=%u\n",
           st.turns, st.faults,
           jr_orch_snapshot(&app)->deaths, jr_orch_snapshot(&app)->reconnects,
           st.permanent_silence, st.max_silent,
           (unsigned long long)soak_now(),
           (unsigned long long)(soak_now() / 60000u),
           app.steps);
}

/* ---- registration (called from test_core.c main, inside UNITY_BEGIN/END) ---- */
void soak_tests_run(void)
{
    /* named adversarial fault matrix */
    RUN_TEST(test_soak_pump_boots_to_listening);
    RUN_TEST(test_soak_transport_drop_midspeaking_resumes);
    RUN_TEST(test_soak_heartbeat_gap_reconnects);
    RUN_TEST(test_soak_wouldblock_flood_survives);
    RUN_TEST(test_soak_split_brain_uplink_dead_reconnects);
    RUN_TEST(test_soak_goaway_reconnects_with_token);
    RUN_TEST(test_soak_noreply_thinking_to_listening);
    RUN_TEST(test_soak_quota_backoff_recoverable);
    RUN_TEST(test_soak_death_storm_parks_then_tap_recovers);
    RUN_TEST(test_soak_transient_error_reconnects);
    /* the deterministic randomized soak */
    RUN_TEST(test_soak_randomized_selfheal);
}
