/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/src/session.c — the full L3 transition table (Phase 1).
 *
 * PURE. Includes only its own header, two jr_ports headers (types + clock), and
 * the C standard library. No IDF, no driver, no network header — the host build
 * proves it. The reducer NEVER performs I/O: it returns an ordered command list
 * the single owner task executes later.
 *
 * Structure:
 *   - reconnect_policy()  §5.1 capped-exp backoff + park (used inside DEATH)
 *   - death()             §4.0 the ONE shared involuntary-death handler
 *   - a shared death pre-check that routes all 5 death events from all 5 death
 *     source states BEFORE the per-state switch (resolves the §4.0/§4.2 tension
 *     in favour of the load-bearing §4.0 routing rule + T06 + critical inv. #1)
 *   - jr_transition()     §4.1-4.10 the per-state table for every other event
 *
 * Illegal (state,event) pairs return an inert identity (next == input, no
 * effectful command) plus one EmitDiag(illegal) and outcome.illegal=true. A
 * device DEBUG build may define JR_ASSERT_ILLEGAL to hard-assert instead; the
 * host suite leaves it undefined so the inert-drop policy is itself testable.
 */
#include "jr_core/session.h"

#include <string.h>
#ifdef JR_ASSERT_ILLEGAL
#include <assert.h>
#endif

/* Enforce the contract counts at compile time (spec §8). */
_Static_assert(JR_ST__COUNT == 11, "8 states, Live x4 substates -> 11 configs");
_Static_assert(JR_EV__COUNT == 25, "25 typed events");
_Static_assert(JR_CMD__COUNT == 32, "32 typed commands");

/* ---------------------- command builders (readable rows) ----------------- */

static jr_command_t mk(jr_cmd_kind_t k)
{
    jr_command_t c;
    memset(&c, 0, sizeof c);
    c.kind = k;
    return c;
}

static void push(jr_cmd_list_t *L, jr_command_t c)
{
    if (L->count < JR_MAX_CMDS) {
        L->cmds[L->count++] = c;
    }
}

static jr_command_t mk_connect(uint32_t tok)
{
    jr_command_t c = mk(JR_CMD_CONNECT);
    c.resumption_token = tok;
    return c;
}
static jr_command_t mk_arm_ka(uint32_t deadline_ms)
{
    jr_command_t c = mk(JR_CMD_ARM_KEEPALIVE);
    c.deadline_ms = deadline_ms;
    return c;
}
static jr_command_t mk_arm_noreply(uint32_t timeout_ms)
{
    jr_command_t c = mk(JR_CMD_ARM_NO_REPLY_WATCHDOG);
    c.timeout_ms = timeout_ms;
    return c;
}
static jr_command_t mk_arm_cap(uint32_t timeout_ms)
{
    jr_command_t c = mk(JR_CMD_ARM_CAPTURE_PAUSE_TIMER);
    c.timeout_ms = timeout_ms;
    return c;
}
static jr_command_t mk_backoff(uint32_t delay_ms)
{
    jr_command_t c = mk(JR_CMD_SCHEDULE_BACKOFF);
    c.delay_ms = delay_ms;
    return c;
}
static jr_command_t mk_close(bool graceful)
{
    jr_command_t c = mk(JR_CMD_CLOSE_TRANSPORT);
    c.graceful = graceful;
    return c;
}
static jr_command_t mk_feed(const jr_pcm_t *pcm, size_t n)
{
    jr_command_t c = mk(JR_CMD_FEED_PLAYBACK);
    c.pcm = pcm;
    c.pcm_len = n;
    return c;
}
static jr_command_t mk_dispatch(const jr_event_t *e, uint32_t gen)
{
    jr_command_t c = mk(JR_CMD_DISPATCH_TOOL_CALL);
    c.call_id = e->call_id;
    c.call_id_text = e->call_id_text;
    c.tool_name = e->tool_name;
    c.tool_args = e->tool_args;
    c.session_gen = gen;
    return c;
}
static jr_command_t mk_tool_response(const jr_event_t *e)
{
    jr_command_t c = mk(JR_CMD_SEND_TOOL_RESPONSE);
    c.call_id = e->call_id;
    c.call_id_text = e->call_id_text;
    c.tool_name = e->tool_name;
    c.tool_response = e->tool_response;
    c.session_gen = e->session_gen;
    return c;
}
static jr_command_t mk_cancel_tool(const jr_event_t *e)
{
    jr_command_t c = mk(JR_CMD_CANCEL_TOOL_CALL);
    c.call_id = e->call_id;
    c.call_id_text = e->call_id_text;
    return c;
}
static jr_command_t mk_arm_ask(uint32_t timeout_ms)
{
    jr_command_t c = mk(JR_CMD_ARM_ASK_TIMER);
    c.timeout_ms = timeout_ms;
    return c;
}
/* PresentChoices carries the ask_user tool call verbatim; L6 parses the
 * question + labels out of tool_args and renders the arcs. */
static jr_command_t mk_present_choices(const jr_event_t *e)
{
    jr_command_t c = mk(JR_CMD_PRESENT_CHOICES);
    c.call_id = e->call_id;
    c.call_id_text = e->call_id_text;
    c.tool_name = e->tool_name;
    c.tool_args = e->tool_args;
    return c;
}
/* Re-present uses the ask payload remembered in SessionState (a stray tap
 * carries none of its own). It MUST carry tool_name/tool_args as well as the
 * ids: L6 re-parses the question and the labels out of tool_args on every
 * PresentChoices, so a payload with only ids renders empty arcs (or dereferences
 * NULL in the parser). These are the same BORROWED pointers enter_asking
 * captured — see the lifetime contract in session.h. */
static jr_command_t mk_represent_choices(const jr_session_t *s)
{
    jr_command_t c = mk(JR_CMD_PRESENT_CHOICES);
    c.call_id = s->ask_call_id;
    c.call_id_text = s->ask_call_id_text;
    c.tool_name = s->ask_tool_name;
    c.tool_args = s->ask_tool_args;
    return c;
}
/* The functionResponse. index == JR_CHOICE_NONE / text == NULL is the
 * "no answer" form every abandon path MUST send, so the model is never left
 * waiting on a tool call that will never be answered. */
static jr_command_t mk_choice_result(const jr_session_t *s, uint8_t index,
                                     const char *text)
{
    jr_command_t c = mk(JR_CMD_SEND_CHOICE_RESULT);
    c.call_id = s->ask_call_id;
    c.call_id_text = s->ask_call_id_text;
    c.choice_index = index;
    c.choice_text = text;
    return c;
}
static jr_command_t mk_diag(const char *reason, jr_event_kind_t et, jr_error_kind_t ek)
{
    jr_command_t c = mk(JR_CMD_EMIT_DIAG);
    c.reason = reason;
    c.event_type = et;
    c.error_kind = ek;
    return c;
}

/* ---------------------- ReconnectPolicy (§5.1) --------------------------- */

jr_reconnect_decision_t jr_reconnect_policy(uint16_t fail_count,
                                            uint64_t last_success_uptime_ms,
                                            jr_error_kind_t error_kind)
{
    jr_reconnect_decision_t r;

    if (error_kind == JR_ERRK_QUOTA) {
        r.delay_ms = JR_QUOTA_COOLOFF_MS;   /* long forced cool-off; quota-safe */
        r.should_park = true;
        return r;
    }
    if (error_kind == JR_ERRK_AUTH || error_kind == JR_ERRK_PROTOCOL) {
        r.delay_ms = 0;                      /* Fatal-adjacent park             */
        r.should_park = true;
        return r;
    }

    /* A healthy session (>= 15 s uptime) earns a clean slate: effective 0. */
    uint16_t effective = (last_success_uptime_ms >= JR_HEALTHY_UPTIME_MS)
                             ? 0
                             : fail_count;
    if (effective >= JR_PARK_AFTER) {
        r.delay_ms = 0;                      /* storm -> park for a human tap   */
        r.should_park = true;
        return r;
    }

    uint32_t delay = JR_BASE_MS << effective;  /* effective in [0,5] -> no overflow */
    if (delay > JR_CAP_MS) {
        delay = JR_CAP_MS;
    }
    r.delay_ms = delay;                      /* 1000,2000,4000,8000,16000,16000 */
    r.should_park = false;
    return r;
}

/* ---------------------- DEATH (§4.0) — the one shared handler ------------- */

static jr_outcome_t death(jr_session_t s, jr_error_kind_t ek,
                          jr_event_kind_t trigger, uint64_t now)
{
    jr_outcome_t o;
    memset(&o, 0, sizeof o);
    jr_cmd_list_t *L = &o.cmds;

    uint64_t uptime = now - s.last_success_ts;
    jr_reconnect_decision_t p = jr_reconnect_policy(s.fail_count, uptime, ek);

    push(L, mk(JR_CMD_MUTE_DAC_NOW));
    push(L, mk(JR_CMD_FLUSH_PLAYBACK_RING));
    push(L, mk(JR_CMD_PAUSE_CAPTURE));
    push(L, mk_close(false));                /* involuntary close (not graceful)*/
    push(L, mk(JR_CMD_DISARM_KEEPALIVE));
    push(L, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
    push(L, mk(JR_CMD_DISARM_CAPTURE_PAUSE_TIMER));
    /* Only when dying OUT of Asking — keeps the ordinary death fan-out at 10
     * commands (JR_MAX_CMDS is 12) while still guaranteeing the ask timer can
     * never fire into a reconnected session and address a dead tool call. */
    if (s.phase == JR_ST_ASKING) {
        push(L, mk(JR_CMD_DISARM_ASK_TIMER));
        /* ...and take the arcs OFF the glass. Without this the last arc-related
         * instruction L6 ever received is a PresentChoices, so the device shows
         * three tappable choices belonging to a dead session while the machine
         * sits in Reconnecting. A tap on them can answer nothing. */
        push(L, mk(JR_CMD_DISMISS_CHOICES));
    }
    push(L, mk_diag("death", trigger, ek));  /* load-bearing: carries error_kind*/

    s.fail_count = (uint16_t)(s.fail_count + 1);
    s.ask_call_id = 0;                       /* the ask dies with the session   */
    s.ask_call_id_text = NULL;
    s.ask_tool_name = NULL;
    s.ask_tool_args = NULL;
    /* No SendActivityEnd here: the socket is being closed involuntarily, so
     * there is nothing to send it on. The window closes with the session. */
    s.manual_activity_open = false;

    if (p.should_park) {
        if (p.delay_ms > 0) {
            push(L, mk_backoff(p.delay_ms)); /* quota cool-off timer            */
        }
        s.phase = JR_ST_BACKOFF;             /* parked / throttled              */
    } else {
        push(L, mk_backoff(p.delay_ms));
        s.phase = JR_ST_RECONNECTING;        /* auto-retry                      */
    }

    o.next = s;
    return o;                                /* finalize() appends PublishSnapshot */
}

static bool is_death_event(jr_event_kind_t k)
{
    return k == JR_EV_TRANSPORT_CLOSED || k == JR_EV_SERVER_ERROR ||
           k == JR_EV_STALE_DEADLINE || k == JR_EV_UPLINK_DEAD ||
           k == JR_EV_SERVER_GO_AWAY;
}

static bool is_death_source(jr_state_t st)
{
    return st == JR_ST_CONNECTING || st == JR_ST_HANDSHAKING ||
           st == JR_ST_LISTENING || st == JR_ST_THINKING ||
           st == JR_ST_SPEAKING ||
           /* Asking is a Live substate on a live socket: if the transport dies
            * under it the machine MUST route to DEATH like any other Live
            * substate. Omitting it would strand the machine in Asking with
            * arcs on screen and no session behind them. The spurious-kill risk
            * (no server frame arrives while a human deliberates) is handled by
            * the longer JR_ASK_DEADLINE_MS keepalive, not by exempting the
            * state from DEATH. */
           st == JR_ST_ASKING;
}

/* ---------------------- small transition constructors -------------------- */

/* A clean no-op self-transition (same phase, no commands, legal). */
static jr_outcome_t noop(jr_session_t s)
{
    jr_outcome_t o;
    memset(&o, 0, sizeof o);
    o.next = s;
    return o;
}

/* Illegal (state,event) pair: inert identity + one EmitDiag(illegal). */
static jr_outcome_t illegal_drop(jr_session_t s, jr_event_kind_t ev)
{
#ifdef JR_ASSERT_ILLEGAL
    assert(0 && "illegal (state,event) pair");
#endif
    jr_outcome_t o;
    memset(&o, 0, sizeof o);
    o.next = s;                              /* NO state change                 */
    o.illegal = true;
    push(&o.cmds, mk_diag("illegal", ev, JR_ERRK_UNKNOWN));
    return o;                                /* no PublishSnapshot for a drop   */
}

/* Enter Connecting: bump session_gen and arm the connect keepalive. Every
 * Idle/Reconnecting/Backoff/Fatal -> Connecting edge routes through here. */
static jr_outcome_t enter_connecting(jr_session_t s, uint32_t connect_token,
                                     bool cancel_backoff, bool reset_fail)
{
    jr_outcome_t o;
    memset(&o, 0, sizeof o);
    jr_cmd_list_t *L = &o.cmds;

    if (cancel_backoff) {
        push(L, mk(JR_CMD_CANCEL_BACKOFF));
    }
    s.session_gen += 1;                      /* stamps every async job          */
    if (reset_fail) {
        s.fail_count = 0;
    }
    s.phase = JR_ST_CONNECTING;
    push(L, mk_connect(connect_token));
    push(L, mk_arm_ka(JR_CONNECT_DEADLINE_MS));

    o.next = s;
    return o;
}

/* -> Idle with a clean reset (the UserStop / drain terminus). */
static jr_outcome_t to_idle_reset(jr_session_t s, bool cancel_backoff,
                                  bool close_transport, bool disarm_keepalive)
{
    jr_outcome_t o;
    memset(&o, 0, sizeof o);
    jr_cmd_list_t *L = &o.cmds;

    if (cancel_backoff) {
        push(L, mk(JR_CMD_CANCEL_BACKOFF));
    }
    if (close_transport) {
        push(L, mk_close(true));             /* graceful — the user asked       */
    }
    if (disarm_keepalive) {
        push(L, mk(JR_CMD_DISARM_KEEPALIVE));
    }
    s.phase = JR_ST_IDLE;
    s.fail_count = 0;                        /* reset fail_count                */
    s.resumption_token = 0;                  /* clear resumption_token          */
    s.ask_call_id = 0;                       /* no ask survives a park          */
    s.ask_call_id_text = NULL;
    s.ask_tool_name = NULL;
    s.ask_tool_args = NULL;
    s.manual_activity_open = false;
    o.next = s;
    return o;
}

/* ---------------------- Asking (§4.11 STATE-05/06) ----------------------- */

/* Enter Live.Asking from any Live substate. THE contract, in order:
 *   1. the arcs go up (PresentChoices);
 *   2. the no-reply watchdog is DISARMED — it is machine-scale (20 s) and this
 *      state waits on a human;
 *   3. the human-scale ask timer is armed instead;
 *   4. the capture-pause timer is disarmed so we do not spray audioStreamEnd
 *      once a second for the whole deliberation;
 *   5. the keepalive is stretched past the ask timer so StaleDeadline cannot
 *      pre-empt it and route to DEATH.
 * Capture is deliberately left RUNNING: the user may answer out loud, and the
 * mic must stay hot for barge-in. */
static jr_outcome_t enter_asking(jr_session_t s, const jr_event_t *e)
{
    jr_outcome_t o;
    memset(&o, 0, sizeof o);
    jr_cmd_list_t *L = &o.cmds;

    /* RE-ASK: the model asked a second question while the first ask was still
     * open. Adopting the new call without answering the old one strands the
     * model on a functionCall that will never be answered — it blocks on it
     * forever and the session goes silent. Answer the OUTGOING call first
     * (JR_CHOICE_NONE == "no answer"), then adopt the new one. Guarded on a
     * live ask so a first entry from Listening/Thinking/Speaking emits nothing
     * extra. */
    if (s.phase == JR_ST_ASKING && s.ask_call_id != 0) {
        push(L, mk_choice_result(&s, JR_CHOICE_NONE, NULL));
    }

    s.ask_call_id = e->call_id;
    s.ask_call_id_text = e->call_id_text;    /* BORROWED — see session.h        */
    s.ask_tool_name = e->tool_name;          /* BORROWED — for the re-present   */
    s.ask_tool_args = e->tool_args;          /* BORROWED — for the re-present   */
    s.phase = JR_ST_ASKING;

    push(L, mk_present_choices(e));
    push(L, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
    push(L, mk(JR_CMD_DISARM_CAPTURE_PAUSE_TIMER));
    push(L, mk_arm_ask(JR_ASK_TIMEOUT_MS));
    push(L, mk_arm_ka(JR_ASK_DEADLINE_MS));

    o.next = s;
    return o;
}

/* Every voluntary exit from Asking: take the arcs off the glass, kill the ask
 * timer, put the keepalive back on the normal Live deadline, and forget the
 * pending call. DismissChoices is EXPLICIT and unconditional — L6 must never
 * have to infer teardown from a phase field. */
static void leave_asking(jr_session_t *s, jr_cmd_list_t *L)
{
    push(L, mk(JR_CMD_DISMISS_CHOICES));
    push(L, mk(JR_CMD_DISARM_ASK_TIMER));
    push(L, mk_arm_ka(JR_LIVE_DEADLINE_MS));
    s->ask_call_id = 0;
    s->ask_call_id_text = NULL;
    s->ask_tool_name = NULL;
    s->ask_tool_args = NULL;
}

/* Close a manual-VAD activity window IF one is open. Manual (PTT) mode opens
 * the window on SpeechStarted and the model waits for its activityEnd; leaving
 * Asking without closing it leaves the model listening to a turn that never
 * ends. The `manual_activity_open` gate means a tap-and-leave that never saw
 * SpeechStarted emits nothing — a bare activityEnd is itself a protocol error. */
static void close_manual_activity(jr_session_t *s, jr_cmd_list_t *L)
{
    if (s->vad_mode == JR_VAD_MANUAL_LOCAL_RMS && s->manual_activity_open) {
        push(L, mk(JR_CMD_SEND_ACTIVITY_END));
        s->manual_activity_open = false;
    }
}

/* Append the implicit PublishSnapshot to any state-changing / command-emitting
 * transition (spec §0). Illegal drops and pure no-ops publish nothing. */
static jr_outcome_t finalize(jr_outcome_t o, jr_state_t prev_phase)
{
    if (!o.illegal && (o.next.phase != prev_phase || o.cmds.count > 0)) {
        jr_command_t c = mk(JR_CMD_PUBLISH_SNAPSHOT);
        c.snapshot_phase = o.next.phase;
        c.fail_count = o.next.fail_count;
        push(&o.cmds, c);
    }
    return o;
}

/* ======================================================================== *
 *  The reducer                                                             *
 * ======================================================================== */

jr_outcome_t jr_transition(jr_session_t s, jr_event_t e, jr_clock_t clk)
{
    const jr_state_t prev = s.phase;
    const jr_event_kind_t ev = e.kind;
    const uint64_t now = jr_clock_now_ms(&clk);
    const bool manual = (s.vad_mode == JR_VAD_MANUAL_LOCAL_RMS);

    jr_outcome_t o;

    /* ---- §4.0 shared DEATH routing: every death event in any death-source
     *      state -> DEATH (Reconnecting, or Backoff when should_park). NEVER
     *      Idle. This is the structural cure for "doesn't talk unless I type"
     *      and is checked BEFORE the per-state table (critical invariant #1). */
    if (is_death_source(s.phase) && is_death_event(ev)) {
        if (ev == JR_EV_SERVER_GO_AWAY && e.resumption_token != 0) {
            s.resumption_token = e.resumption_token;  /* retain resume handle    */
        }
        jr_error_kind_t ek = (ev == JR_EV_SERVER_ERROR) ? e.error_kind
                                                        : JR_ERRK_TRANSIENT;
        return finalize(death(s, ek, ev, now), prev);
    }

    switch (s.phase) {

    /* ----------------------------- §4.1 Idle ---------------------------- */
    case JR_ST_IDLE:
        switch (ev) {
        case JR_EV_USER_START:
        case JR_EV_TAP:
            s.resumption_token = 0;          /* fresh start, no token           */
            o = enter_connecting(s, 0, /*cancel*/ false, /*reset*/ true);
            break;
        case JR_EV_USER_STOP:                /* idempotent no-op                */
        case JR_EV_TRANSPORT_CLOSED:         /* defensive no-op; already closed */
            o = noop(s);
            break;
        default:
            o = illegal_drop(s, ev);
            break;
        }
        break;

    /* -------------------------- §4.2 Connecting ------------------------- */
    case JR_ST_CONNECTING:
        switch (ev) {
        case JR_EV_CONNECTED:
            s.phase = JR_ST_HANDSHAKING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_SEND_SETUP));
            push(&o.cmds, mk_arm_ka(JR_HANDSHAKE_DEADLINE_MS));
            break;
        case JR_EV_HEARTBEAT:
            o = noop(s);
            push(&o.cmds, mk_arm_ka(JR_CONNECT_DEADLINE_MS));
            break;
        case JR_EV_USER_STOP:
            o = to_idle_reset(s, false, /*close*/ true, /*disarm_ka*/ true);
            break;
        case JR_EV_USER_START:
        case JR_EV_TAP:                      /* already dialing                 */
            o = noop(s);
            break;
        case JR_EV_SETUP_COMPLETE:           /* setup before socket-open        */
        default:
            o = illegal_drop(s, ev);
            break;
        }
        break;

    /* ------------------------- §4.3 Handshaking ------------------------- */
    case JR_ST_HANDSHAKING:
        switch (ev) {
        case JR_EV_SETUP_COMPLETE:
            if (e.resumption_token != 0) {
                s.resumption_token = e.resumption_token;
            }
            s.last_success_ts = now;         /* healthy-uptime anchor           */
            s.phase = JR_ST_LISTENING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_START_CAPTURE));
            push(&o.cmds, mk(JR_CMD_UNMUTE_DAC));
            push(&o.cmds, mk_arm_ka(JR_LIVE_DEADLINE_MS));
            push(&o.cmds, mk_arm_cap(JR_CAPTURE_PAUSE_MS));
            break;
        case JR_EV_HEARTBEAT:
            o = noop(s);
            push(&o.cmds, mk_arm_ka(JR_HANDSHAKE_DEADLINE_MS));
            break;
        case JR_EV_USER_STOP:
            o = to_idle_reset(s, false, true, true);
            break;
        case JR_EV_USER_START:
        case JR_EV_TAP:
            o = noop(s);
            break;
        default:
            o = illegal_drop(s, ev);
            break;
        }
        break;

    /* ------------------------ §4.4 Live.Listening ----------------------- */
    case JR_ST_LISTENING:
        switch (ev) {
        case JR_EV_SPEECH_STARTED:
            o = noop(s);
            if (manual) {
                push(&o.cmds, mk(JR_CMD_SEND_ACTIVITY_START));
                o.next.manual_activity_open = true;   /* window opens here      */
            }
            push(&o.cmds, mk(JR_CMD_DISARM_CAPTURE_PAUSE_TIMER));
            break;
        case JR_EV_SPEECH_ENDED:
            s.phase = JR_ST_THINKING;
            s.manual_activity_open = false;           /* ...and closes here     */
            o = noop(s);
            push(&o.cmds, manual ? mk(JR_CMD_SEND_ACTIVITY_END)
                                 : mk(JR_CMD_SEND_AUDIO_STREAM_END));
            push(&o.cmds, mk_arm_noreply(JR_NOREPLY_MS));
            push(&o.cmds, mk(JR_CMD_DISARM_CAPTURE_PAUSE_TIMER));
            break;
        case JR_EV_SERVER_AUDIO_CHUNK:
            s.phase = JR_ST_SPEAKING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_UNMUTE_DAC));
            push(&o.cmds, mk_feed(e.pcm, e.pcm_len));
            push(&o.cmds, mk_arm_ka(JR_LIVE_DEADLINE_MS));
            break;
        case JR_EV_SERVER_TOOL_CALL:
            if (e.is_cancellation) {
                o = noop(s);
                push(&o.cmds, mk_cancel_tool(&e));
            } else {
                s.phase = JR_ST_THINKING;
                o = noop(s);
                push(&o.cmds, mk_dispatch(&e, s.session_gen));
                push(&o.cmds, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
            }
            break;
        case JR_EV_TOOL_RESULT_READY:
            if (jr_session_gen_is_stale(s.session_gen, e.session_gen)) {
                o = noop(s);
            } else {
                s.phase = JR_ST_THINKING;
                o = noop(s);
                push(&o.cmds, mk_tool_response(&e));
                push(&o.cmds, mk_arm_noreply(JR_NOREPLY_MS));
            }
            break;
        case JR_EV_ASK_OPENED:               /* ask_user tool call -> arcs      */
            o = enter_asking(s, &e);
            break;
        case JR_EV_SERVER_INTERRUPTED:       /* benign; nothing playing         */
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_FLUSH_PLAYBACK_RING));
            push(&o.cmds, mk(JR_CMD_MUTE_DAC_NOW));
            break;
        case JR_EV_SERVER_TURN_COMPLETE:     /* benign boundary                 */
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
            break;
        case JR_EV_HEARTBEAT:
            o = noop(s);
            push(&o.cmds, mk_arm_ka(JR_LIVE_DEADLINE_MS));
            break;
        case JR_EV_CAPTURE_PAUSE_ELAPSED:    /* auto-VAD keepalive              */
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_SEND_AUDIO_STREAM_END));
            push(&o.cmds, mk_arm_cap(JR_CAPTURE_PAUSE_MS));
            break;
        case JR_EV_USER_STOP:
            s.phase = JR_ST_DRAINING;
            s.manual_activity_open = false;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_SEND_ACTIVITY_END));
            push(&o.cmds, mk_close(true));
            push(&o.cmds, mk(JR_CMD_PAUSE_CAPTURE));
            push(&o.cmds, mk_arm_ka(JR_DRAIN_DEADLINE_MS));
            break;
        case JR_EV_USER_START:
        case JR_EV_TAP:
            o = noop(s);
            break;
        default:                             /* Barge/NoReply here are illegal  */
            o = illegal_drop(s, ev);
            break;
        }
        break;

    /* ------------------------- §4.5 Live.Thinking ----------------------- */
    case JR_ST_THINKING:
        switch (ev) {
        case JR_EV_SERVER_AUDIO_CHUNK:
            s.phase = JR_ST_SPEAKING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_UNMUTE_DAC));
            push(&o.cmds, mk_feed(e.pcm, e.pcm_len));
            push(&o.cmds, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
            push(&o.cmds, mk_arm_ka(JR_LIVE_DEADLINE_MS));
            break;
        case JR_EV_NO_REPLY_TIMEOUT:         /* the 20s -> Listening + resume   */
            s.phase = JR_ST_LISTENING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
            push(&o.cmds, mk(JR_CMD_START_CAPTURE));
            push(&o.cmds, mk_arm_cap(JR_CAPTURE_PAUSE_MS));
            push(&o.cmds, mk_diag("no_reply_resume", ev, JR_ERRK_UNKNOWN));
            break;
        case JR_EV_SERVER_INTERRUPTED:       /* interrupted-without-turnComplete*/
            s.phase = JR_ST_LISTENING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
            push(&o.cmds, mk(JR_CMD_FLUSH_PLAYBACK_RING));
            push(&o.cmds, mk(JR_CMD_MUTE_DAC_NOW));
            push(&o.cmds, mk(JR_CMD_START_CAPTURE));
            break;
        case JR_EV_SERVER_TOOL_CALL:
            o = noop(s);
            if (e.is_cancellation) {
                push(&o.cmds, mk_cancel_tool(&e));
                push(&o.cmds, mk_arm_noreply(JR_NOREPLY_MS));
            } else {
                push(&o.cmds, mk_dispatch(&e, s.session_gen));
                push(&o.cmds, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
            }
            break;
        case JR_EV_TOOL_RESULT_READY:
            o = noop(s);
            if (!jr_session_gen_is_stale(s.session_gen, e.session_gen)) {
                push(&o.cmds, mk_tool_response(&e));
                push(&o.cmds, mk_arm_noreply(JR_NOREPLY_MS));
            }
            break;
        case JR_EV_SERVER_TURN_COMPLETE:     /* text-only / empty turn          */
            s.phase = JR_ST_LISTENING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
            push(&o.cmds, mk(JR_CMD_START_CAPTURE));
            push(&o.cmds, mk_arm_cap(JR_CAPTURE_PAUSE_MS));
            break;
        case JR_EV_ASK_OPENED:               /* the usual emitter: the ask_user
                                              * tool call was dispatched from
                                              * Thinking and L6 recognised it   */
            o = enter_asking(s, &e);
            break;
        case JR_EV_SPEECH_STARTED:           /* user re-engaged                 */
            s.phase = JR_ST_LISTENING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
            if (manual) {
                push(&o.cmds, mk(JR_CMD_SEND_ACTIVITY_START));
                o.next.manual_activity_open = true;
            }
            break;
        case JR_EV_HEARTBEAT:
            o = noop(s);
            push(&o.cmds, mk_arm_ka(JR_LIVE_DEADLINE_MS));
            break;
        case JR_EV_CAPTURE_PAUSE_ELAPSED:
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_SEND_AUDIO_STREAM_END));
            push(&o.cmds, mk_arm_cap(JR_CAPTURE_PAUSE_MS));
            break;
        case JR_EV_USER_STOP:
            s.phase = JR_ST_DRAINING;
            o = noop(s);
            push(&o.cmds, mk_close(true));
            push(&o.cmds, mk(JR_CMD_PAUSE_CAPTURE));
            push(&o.cmds, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));
            push(&o.cmds, mk_arm_ka(JR_DRAIN_DEADLINE_MS));
            break;
        case JR_EV_USER_START:
        case JR_EV_TAP:
            o = noop(s);
            break;
        default:                             /* Barge/SpeechEnded illegal here  */
            o = illegal_drop(s, ev);
            break;
        }
        break;

    /* ------------------------- §4.6 Live.Speaking ----------------------- */
    case JR_ST_SPEAKING:
        switch (ev) {
        case JR_EV_SERVER_AUDIO_CHUNK:
            o = noop(s);
            push(&o.cmds, mk_feed(e.pcm, e.pcm_len));
            push(&o.cmds, mk_arm_ka(JR_LIVE_DEADLINE_MS));
            break;
        case JR_EV_BARGE_DETECTED:           /* THE barge row — exact 3-prefix  */
            s.phase = JR_ST_LISTENING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_MUTE_DAC_NOW));
            push(&o.cmds, mk(JR_CMD_FLUSH_PLAYBACK_RING));
            push(&o.cmds, mk(JR_CMD_SEND_ACTIVITY_START));
            o.next.manual_activity_open = manual; /* the barge opens the window */
            push(&o.cmds, mk_arm_cap(JR_CAPTURE_PAUSE_MS));
            push(&o.cmds, mk_diag("barge", ev, JR_ERRK_UNKNOWN));
            break;
        case JR_EV_SERVER_TURN_COMPLETE:     /* ring drains tail; DAC unmuted   */
            s.phase = JR_ST_LISTENING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_START_CAPTURE));
            push(&o.cmds, mk_arm_cap(JR_CAPTURE_PAUSE_MS));
            break;
        case JR_EV_SERVER_INTERRUPTED:       /* server self-interrupt -> flush  */
            s.phase = JR_ST_LISTENING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_MUTE_DAC_NOW));
            push(&o.cmds, mk(JR_CMD_FLUSH_PLAYBACK_RING));
            push(&o.cmds, mk(JR_CMD_START_CAPTURE));
            break;
        case JR_EV_SERVER_TOOL_CALL:
            o = noop(s);                     /* audio continues                 */
            if (e.is_cancellation) {
                push(&o.cmds, mk_cancel_tool(&e));
            } else {
                push(&o.cmds, mk_dispatch(&e, s.session_gen));
            }
            break;
        case JR_EV_TOOL_RESULT_READY:
            o = noop(s);
            if (!jr_session_gen_is_stale(s.session_gen, e.session_gen)) {
                push(&o.cmds, mk_tool_response(&e));
            }
            break;
        case JR_EV_HEARTBEAT:
            o = noop(s);
            push(&o.cmds, mk_arm_ka(JR_LIVE_DEADLINE_MS));
            break;
        case JR_EV_ASK_OPENED:               /* arcs go up while the model is
                                              * still speaking the question —
                                              * audio keeps flowing, no mute    */
            o = enter_asking(s, &e);
            break;
        case JR_EV_SPEECH_STARTED:           /* NOT a barge (zero-self-barge)   */
        case JR_EV_SPEECH_ENDED:             /* user pause during model speech  */
            o = noop(s);
            break;
        case JR_EV_USER_STOP:
            s.phase = JR_ST_DRAINING;
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_MUTE_DAC_NOW));
            push(&o.cmds, mk(JR_CMD_FLUSH_PLAYBACK_RING));
            push(&o.cmds, mk_close(true));
            push(&o.cmds, mk(JR_CMD_PAUSE_CAPTURE));
            push(&o.cmds, mk_arm_ka(JR_DRAIN_DEADLINE_MS));
            break;
        case JR_EV_USER_START:
        case JR_EV_TAP:
            o = noop(s);
            break;
        default:                             /* NoReply illegal (disarmed)      */
            o = illegal_drop(s, ev);
            break;
        }
        break;

    /* -------------------- §4.11 Live.Asking (STATE-05/06) ---------------- *
     * Waiting on a HUMAN, not on the server. Two rules dominate every row:
     *   - the no-reply watchdog stays DISARMED for the whole state (it is
     *     20 s; deliberation is not a fault) and is only re-armed on an exit
     *     that leaves the model owing us a reply;
     *   - every exit sends a SendChoiceResult, even the abandon paths, so the
     *     model is never left blocked on an unanswered tool call.
     * Death events never reach here — the shared §4.0 pre-check owns them. */
    case JR_ST_ASKING:
        switch (ev) {
        case JR_EV_CHOICE_PICKED:            /* THE row: a finger landed        */
            o = noop(s);
            push(&o.cmds, mk_choice_result(&s, e.choice_index, e.choice_text));
            /* The human may have started talking while deciding and then tapped
             * instead. That activity window is still open — close it or the
             * model waits on a turn that never ends. */
            close_manual_activity(&s, &o.cmds);
            leave_asking(&s, &o.cmds);
            push(&o.cmds, mk_arm_noreply(JR_NOREPLY_MS)); /* model owes a reply */
            s.phase = JR_ST_THINKING;
            o.next = s;
            break;

        case JR_EV_ASK_TIMEOUT:              /* nobody answered in 120 s        */
            o = noop(s);
            push(&o.cmds, mk_choice_result(&s, JR_CHOICE_NONE, NULL));
            close_manual_activity(&s, &o.cmds);
            leave_asking(&s, &o.cmds);
            /* Back to Listening, NOT Thinking: we hand the floor back to the
             * user. No no-reply watchdog here — it is illegal in Listening. */
            push(&o.cmds, mk(JR_CMD_START_CAPTURE));
            push(&o.cmds, mk_arm_cap(JR_CAPTURE_PAUSE_MS));
            push(&o.cmds, mk_diag("ask_timeout", ev, JR_ERRK_UNKNOWN));
            s.phase = JR_ST_LISTENING;
            o.next = s;
            break;

        case JR_EV_NO_REPLY_TIMEOUT:
            /* A late fire from a watchdog we disarmed on entry. It must be
             * INERT: this is exactly the 21-second-deliberation teardown the
             * plan calls out. Never route to DEATH, never change phase. */
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_DISARM_NO_REPLY_WATCHDOG));  /* belt+braces */
            push(&o.cmds, mk_diag("noreply_ignored_in_asking", ev, JR_ERRK_UNKNOWN));
            break;

        case JR_EV_SPEECH_ENDED:
            /* The human answered OUT LOUD instead of tapping. Abandon the arcs,
             * close the tool call so the model is not stranded, and commit the
             * spoken turn exactly like Listening + SpeechEnded does. */
            o = noop(s);
            push(&o.cmds, mk_choice_result(&s, JR_CHOICE_NONE, NULL));
            /* Unconditional, exactly like Listening's SpeechEnded row: this IS
             * the turn boundary, and in auto-VAD it is the audioStreamEnd that
             * commits the turn. */
            push(&o.cmds, manual ? mk(JR_CMD_SEND_ACTIVITY_END)
                                 : mk(JR_CMD_SEND_AUDIO_STREAM_END));
            s.manual_activity_open = false;
            leave_asking(&s, &o.cmds);
            push(&o.cmds, mk_arm_noreply(JR_NOREPLY_MS));
            s.phase = JR_ST_THINKING;
            o.next = s;
            break;

        case JR_EV_SPEECH_STARTED:           /* mic is hot by design            */
            o = noop(s);
            if (manual) {
                push(&o.cmds, mk(JR_CMD_SEND_ACTIVITY_START));
                o.next.manual_activity_open = true;  /* MUST be closed on exit  */
            }
            break;

        case JR_EV_SERVER_AUDIO_CHUNK:       /* model speaks the question aloud
                                              * while the arcs are up           */
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_UNMUTE_DAC));
            push(&o.cmds, mk_feed(e.pcm, e.pcm_len));
            push(&o.cmds, mk_arm_ka(JR_ASK_DEADLINE_MS));
            break;

        case JR_EV_SERVER_TURN_COMPLETE:     /* question finished; still waiting*/
            o = noop(s);
            push(&o.cmds, mk_arm_ka(JR_ASK_DEADLINE_MS));
            break;

        case JR_EV_SERVER_INTERRUPTED:
            o = noop(s);
            push(&o.cmds, mk(JR_CMD_FLUSH_PLAYBACK_RING));
            push(&o.cmds, mk(JR_CMD_MUTE_DAC_NOW));
            break;

        case JR_EV_HEARTBEAT:
            o = noop(s);
            push(&o.cmds, mk_arm_ka(JR_ASK_DEADLINE_MS)); /* NOT the 45 s one   */
            break;

        case JR_EV_SERVER_TOOL_CALL:
            /* A cancellation of the ASK'S OWN call id is the server revoking
             * the question: tear the ask down like the timeout row, but send
             * NO SendChoiceResult — a functionResponse for a withdrawn id is
             * a protocol error. The floor goes back to the user. */
            if (e.is_cancellation && s.ask_call_id != 0 &&
                e.call_id == s.ask_call_id) {
                o = noop(s);
                close_manual_activity(&s, &o.cmds);
                leave_asking(&s, &o.cmds);
                push(&o.cmds, mk(JR_CMD_START_CAPTURE));
                push(&o.cmds, mk_arm_cap(JR_CAPTURE_PAUSE_MS));
                push(&o.cmds, mk_diag("ask_cancelled_by_server", ev,
                                      JR_ERRK_UNKNOWN));
                s.phase = JR_ST_LISTENING;
                o.next = s;
                break;
            }
            /* Otherwise: an unrelated tool, mid-ask. */
            o = noop(s);
            if (e.is_cancellation) {
                push(&o.cmds, mk_cancel_tool(&e));
            } else {
                push(&o.cmds, mk_dispatch(&e, s.session_gen));
            }
            break;

        case JR_EV_TOOL_RESULT_READY:
            o = noop(s);
            if (!jr_session_gen_is_stale(s.session_gen, e.session_gen)) {
                push(&o.cmds, mk_tool_response(&e));
            }
            break;

        case JR_EV_ASK_OPENED:               /* the model re-asked — re-arm the
                                              * whole ask around the NEW call   */
            o = enter_asking(s, &e);
            break;

        case JR_EV_TAP:                      /* a tap that missed every arc     */
        case JR_EV_USER_START:
            o = noop(s);
            push(&o.cmds, mk_represent_choices(&s));  /* redraw; idempotent     */
            break;

        case JR_EV_USER_STOP:                /* drains exactly like Listening   */
            o = noop(s);
            push(&o.cmds, mk_choice_result(&s, JR_CHOICE_NONE, NULL));
            close_manual_activity(&s, &o.cmds);  /* the leak Listening never had */
            push(&o.cmds, mk(JR_CMD_DISMISS_CHOICES));
            push(&o.cmds, mk(JR_CMD_DISARM_ASK_TIMER));
            push(&o.cmds, mk_close(true));
            push(&o.cmds, mk(JR_CMD_PAUSE_CAPTURE));
            push(&o.cmds, mk_arm_ka(JR_DRAIN_DEADLINE_MS));
            s.ask_call_id = 0;
            s.ask_call_id_text = NULL;
            s.ask_tool_name = NULL;
            s.ask_tool_args = NULL;
            s.phase = JR_ST_DRAINING;
            o.next = s;
            break;

        case JR_EV_CAPTURE_PAUSE_ELAPSED:    /* late fire; disarmed on entry    */
            o = noop(s);                     /* inert — do NOT re-arm           */
            break;

        default:
            o = illegal_drop(s, ev);
            break;
        }
        break;

    /* --------------------------- §4.7 Draining -------------------------- */
    case JR_ST_DRAINING:
        switch (ev) {
        case JR_EV_TRANSPORT_CLOSED:         /* the ONLY non-UserStop -> Idle   */
        case JR_EV_SERVER_ERROR:             /* any close during drain -> Idle  */
        case JR_EV_STALE_DEADLINE:
        case JR_EV_UPLINK_DEAD:
        case JR_EV_SERVER_GO_AWAY:
            o = to_idle_reset(s, false, /*close*/ false, /*disarm_ka*/ true);
            break;
        default:                             /* ignore all late frames          */
            o = noop(s);
            break;
        }
        break;

    /* ------------------------- §4.8 Reconnecting ------------------------ */
    case JR_ST_RECONNECTING:
        switch (ev) {
        case JR_EV_BACKOFF_ELAPSED:          /* auto-redial, no tap required    */
            o = enter_connecting(s, s.resumption_token, false, /*reset*/ false);
            break;
        case JR_EV_USER_START:               /* human accelerates the retry     */
        case JR_EV_TAP:
            o = enter_connecting(s, s.resumption_token, /*cancel*/ true, false);
            break;
        case JR_EV_USER_STOP:
            o = to_idle_reset(s, /*cancel*/ true, false, /*disarm_ka*/ true);
            break;
        default:                             /* residual frames -> inert stale  */
            o = noop(s);
            push(&o.cmds, mk_diag("stale", ev, JR_ERRK_UNKNOWN));
            break;
        }
        break;

    /* --------------------------- §4.9 Backoff --------------------------- */
    case JR_ST_BACKOFF:
        switch (ev) {
        case JR_EV_BACKOFF_ELAPSED:          /* quota cool-off timer fired      */
            o = enter_connecting(s, s.resumption_token, false, /*reset*/ false);
            break;
        case JR_EV_USER_START:               /* park-for-tap exit               */
        case JR_EV_TAP:
            s.resumption_token = 0;          /* fresh start on manual restart   */
            o = enter_connecting(s, 0, /*cancel*/ true, /*reset*/ true);
            break;
        case JR_EV_USER_STOP:
            o = to_idle_reset(s, /*cancel*/ true, false, /*disarm_ka*/ false);
            break;
        default:                             /* inert while parked              */
            o = noop(s);
            push(&o.cmds, mk_diag("stale", ev, JR_ERRK_UNKNOWN));
            break;
        }
        break;

    /* ---------------------------- §4.10 Fatal --------------------------- */
    case JR_ST_FATAL:
        switch (ev) {
        case JR_EV_USER_STOP:
            o = to_idle_reset(s, false, false, false);
            break;
        case JR_EV_USER_START:               /* manual recovery attempt         */
        case JR_EV_TAP:
            s.resumption_token = 0;
            o = enter_connecting(s, 0, false, /*reset*/ true);
            break;
        default:
            o = illegal_drop(s, ev);
            break;
        }
        break;

    default:
        o = illegal_drop(s, ev);
        break;
    }

    return finalize(o, prev);
}

/* ---------------------- builders + name tables --------------------------- */

jr_event_t jr_event(jr_event_kind_t kind)
{
    jr_event_t e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    return e;
}

jr_session_t jr_session_init(jr_vad_mode_t vad_mode)
{
    jr_session_t s;
    memset(&s, 0, sizeof s);
    s.phase = JR_ST_IDLE;
    s.vad_mode = vad_mode;
    return s;
}

bool jr_state_is_live(jr_state_t state)
{
    return state == JR_ST_LISTENING ||
           state == JR_ST_THINKING ||
           state == JR_ST_SPEAKING ||
           /* Asking is a Live substate: socket up, mic hot, DAC available.
            * jr_orch_is_talking_capable() and jr_orch_is_zombie() both depend
            * on this — Asking arms the keepalive, so it is never a zombie. */
           state == JR_ST_ASKING;
}

const char *jr_state_name(jr_state_t state)
{
    switch (state) {
    case JR_ST_IDLE:         return "Idle";
    case JR_ST_CONNECTING:   return "Connecting";
    case JR_ST_HANDSHAKING:  return "Handshaking";
    case JR_ST_LISTENING:    return "Listening";
    case JR_ST_THINKING:     return "Thinking";
    case JR_ST_SPEAKING:     return "Speaking";
    case JR_ST_ASKING:       return "Asking";
    case JR_ST_DRAINING:     return "Draining";
    case JR_ST_RECONNECTING: return "Reconnecting";
    case JR_ST_BACKOFF:      return "Backoff";
    case JR_ST_FATAL:        return "Fatal";
    default:                 return "?";
    }
}

const char *jr_event_name(jr_event_kind_t event)
{
    switch (event) {
    case JR_EV_CONNECTED:            return "Connected";
    case JR_EV_SETUP_COMPLETE:       return "SetupComplete";
    case JR_EV_SERVER_AUDIO_CHUNK:   return "ServerAudioChunk";
    case JR_EV_SERVER_INTERRUPTED:   return "ServerInterrupted";
    case JR_EV_SERVER_TURN_COMPLETE: return "ServerTurnComplete";
    case JR_EV_SERVER_TOOL_CALL:     return "ServerToolCall";
    case JR_EV_TOOL_RESULT_READY:    return "ToolResultReady";
    case JR_EV_SERVER_GO_AWAY:       return "ServerGoAway";
    case JR_EV_SERVER_ERROR:         return "ServerError";
    case JR_EV_TRANSPORT_CLOSED:     return "TransportClosed";
    case JR_EV_HEARTBEAT:            return "Heartbeat";
    case JR_EV_SPEECH_STARTED:       return "SpeechStarted";
    case JR_EV_SPEECH_ENDED:         return "SpeechEnded";
    case JR_EV_BARGE_DETECTED:       return "BargeDetected";
    case JR_EV_UPLINK_DEAD:          return "UplinkDead";
    case JR_EV_STALE_DEADLINE:       return "StaleDeadline";
    case JR_EV_NO_REPLY_TIMEOUT:     return "NoReplyTimeout";
    case JR_EV_BACKOFF_ELAPSED:      return "BackoffElapsed";
    case JR_EV_CAPTURE_PAUSE_ELAPSED:return "CapturePauseElapsed";
    case JR_EV_USER_START:           return "UserStart";
    case JR_EV_USER_STOP:            return "UserStop";
    case JR_EV_TAP:                  return "Tap";
    case JR_EV_ASK_OPENED:           return "AskOpened";
    case JR_EV_CHOICE_PICKED:        return "ChoicePicked";
    case JR_EV_ASK_TIMEOUT:          return "AskTimeout";
    default:                         return "?";
    }
}

const char *jr_cmd_name(jr_cmd_kind_t cmd)
{
    switch (cmd) {
    case JR_CMD_CONNECT:                  return "Connect";
    case JR_CMD_SEND_SETUP:               return "SendSetup";
    case JR_CMD_SEND_ACTIVITY_START:      return "SendActivityStart";
    case JR_CMD_SEND_ACTIVITY_END:        return "SendActivityEnd";
    case JR_CMD_SEND_AUDIO_STREAM_END:    return "SendAudioStreamEnd";
    case JR_CMD_SEND_TEXT:                return "SendText";
    case JR_CMD_SEND_AUDIO:               return "SendAudio";
    case JR_CMD_SEND_TOOL_RESPONSE:       return "SendToolResponse";
    case JR_CMD_CLOSE_TRANSPORT:          return "CloseTransport";
    case JR_CMD_START_CAPTURE:            return "StartCapture";
    case JR_CMD_PAUSE_CAPTURE:            return "PauseCapture";
    case JR_CMD_MUTE_DAC_NOW:             return "MuteDacNow";
    case JR_CMD_UNMUTE_DAC:               return "UnmuteDac";
    case JR_CMD_FLUSH_PLAYBACK_RING:      return "FlushPlaybackRing";
    case JR_CMD_FEED_PLAYBACK:            return "FeedPlayback";
    case JR_CMD_SCHEDULE_BACKOFF:         return "ScheduleBackoff";
    case JR_CMD_CANCEL_BACKOFF:           return "CancelBackoff";
    case JR_CMD_ARM_KEEPALIVE:            return "ArmKeepalive";
    case JR_CMD_DISARM_KEEPALIVE:         return "DisarmKeepalive";
    case JR_CMD_ARM_NO_REPLY_WATCHDOG:    return "ArmNoReplyWatchdog";
    case JR_CMD_DISARM_NO_REPLY_WATCHDOG: return "DisarmNoReplyWatchdog";
    case JR_CMD_ARM_CAPTURE_PAUSE_TIMER:  return "ArmCapturePauseTimer";
    case JR_CMD_DISARM_CAPTURE_PAUSE_TIMER: return "DisarmCapturePauseTimer";
    case JR_CMD_DISPATCH_TOOL_CALL:       return "DispatchToolCall";
    case JR_CMD_CANCEL_TOOL_CALL:         return "CancelToolCall";
    case JR_CMD_PUBLISH_SNAPSHOT:         return "PublishSnapshot";
    case JR_CMD_EMIT_DIAG:                return "EmitDiag";
    case JR_CMD_PRESENT_CHOICES:          return "PresentChoices";
    case JR_CMD_DISMISS_CHOICES:          return "DismissChoices";
    case JR_CMD_SEND_CHOICE_RESULT:       return "SendChoiceResult";
    case JR_CMD_ARM_ASK_TIMER:            return "ArmAskTimer";
    case JR_CMD_DISARM_ASK_TIMER:         return "DisarmAskTimer";
    default:                              return "?";
    }
}
