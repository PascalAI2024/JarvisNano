/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/src/orchestrator.c — the pure single-writer pump (Phase 1 capstone).
 *
 * PURE. Includes only its own header (which pulls jr_core/session.h,
 * jr_core/monitors.h, jr_core/snapshot.h) + libc. No IDF, no driver/network/
 * model header, no globals, no wall-clock. Every effect goes through the
 * injected jr_clock_t + jr_orch_io_t. The host build proves the seam: a stray
 * driver include fails there.
 *
 * The pump is the ONE writer of SessionState. It composes:
 *   - jr_transition()  (session.c)  — decides next state + command list;
 *   - the 6 monitors   (monitors.c) — emit the deadline/uplink events;
 *   - jr_snapshot_fold (snapshot.c) — records every transition for observability.
 * The reducer stays pure (returns commands, never acts). The monitors stay pure
 * (return events, never act). The pump is the single place they meet.
 */
#include "jr_core/orchestrator.h"

#include <string.h>

/* Hard cap on fixpoint iterations per step. Inputs at a fixed `now` are finite
 * (a bounded inbox + a bounded reconnect cascade) and no monitor re-fires after
 * being serviced at the same `now` (arming stamps last_rx/fire_at == now, so age
 * 0 <= deadline), so the loop always terminates well under this. The cap is a
 * belt-and-suspenders guard, never reached in practice. */
#define JR_ORCH_FIXPOINT_CAP 512u

/* ---------------------- command dispatch -------------------------------- */

/* Forward one externally-visible command to the injected I/O port. */
static void orch_exec(jr_orch_t *app, const jr_command_t *cmd)
{
    if (app->io.exec != NULL) {
        app->io.exec(app->io.ctx, cmd);
    }
    app->cmds_exec++;
}

/* Execute the ordered command list jr_transition returned. Timer/monitor/backoff
 * commands mutate the monitors the orchestrator OWNS; everything else is an
 * external effect forwarded to the I/O port. START/PAUSE capture additionally
 * arm/disarm the DeadUplinkMonitor (capture running == uplink active), which the
 * command set does not model explicitly. */
static void orch_exec_cmds(jr_orch_t *app, const jr_cmd_list_t *L, uint64_t now)
{
    for (size_t i = 0; i < L->count; ++i) {
        const jr_command_t *c = &L->cmds[i];
        switch (c->kind) {
        /* --- owned timers / monitors (internal; never reach the I/O port) --- */
        case JR_CMD_ARM_KEEPALIVE:
            jr_keepalive_arm(&app->keepalive, now, c->deadline_ms);
            break;
        case JR_CMD_DISARM_KEEPALIVE:
            jr_keepalive_disarm(&app->keepalive);
            break;
        case JR_CMD_ARM_NO_REPLY_WATCHDOG:
            jr_no_reply_arm(&app->no_reply, now, c->timeout_ms);
            break;
        case JR_CMD_DISARM_NO_REPLY_WATCHDOG:
            jr_no_reply_disarm(&app->no_reply);
            break;
        case JR_CMD_ARM_CAPTURE_PAUSE_TIMER:
            jr_capture_pause_arm(&app->capture_pause, now, c->timeout_ms,
                                 app->session.vad_mode == JR_VAD_SERVER);
            break;
        case JR_CMD_DISARM_CAPTURE_PAUSE_TIMER:
            jr_capture_pause_disarm(&app->capture_pause);
            break;
        /* The ask timer is OWNED here, exactly like the backoff timer. It must
         * NEVER reach the I/O port: the device's exec() has no case for it and
         * would silently swallow it, leaving Asking with no bounding deadline at
         * all. This case IS the implementation of JR_EV_ASK_TIMEOUT. */
        case JR_CMD_ARM_ASK_TIMER:
            app->ask_timer.armed = true;
            app->ask_timer.armed_ts = now;
            app->ask_timer.timeout_ms = c->timeout_ms;
            break;
        case JR_CMD_DISARM_ASK_TIMER:
            app->ask_timer.armed = false;
            break;

        case JR_CMD_SCHEDULE_BACKOFF:
            app->backoff.armed = true;
            app->backoff.fire_at = now + c->delay_ms;
            break;
        case JR_CMD_CANCEL_BACKOFF:
            app->backoff.armed = false;
            break;

        /* --- capture lane: also (dis)arm the DeadUplinkMonitor --- */
        case JR_CMD_START_CAPTURE:
            jr_dead_uplink_arm(&app->dead_uplink, JR_TX_FAIL_RESUME);
            orch_exec(app, c);
            break;
        case JR_CMD_PAUSE_CAPTURE:
            jr_dead_uplink_disarm(&app->dead_uplink);
            orch_exec(app, c);
            break;

        /* --- everything else is an external effect --- */
        default:
            orch_exec(app, c);
            break;
        }
    }
}

/* Which inbound event kinds are downlink server frames — every one is a liveness
 * tick that re-arms the KeepaliveMonitor (spec §5.2: "any server frame"). A
 * TransportClosed/ServerError routes to DEATH anyway, so it is not a liveness
 * tick; a synthetic Connected is a ws-level signal handled by the reducer's
 * ArmKeepalive, not a downlink frame. */
static bool is_downlink_frame(jr_event_kind_t k)
{
    switch (k) {
    case JR_EV_SETUP_COMPLETE:
    case JR_EV_SERVER_AUDIO_CHUNK:
    case JR_EV_SERVER_INTERRUPTED:
    case JR_EV_SERVER_TURN_COMPLETE:
    case JR_EV_SERVER_TOOL_CALL:
    case JR_EV_SERVER_GO_AWAY:
    case JR_EV_HEARTBEAT:
        return true;
    default:
        return false;
    }
}

/* Feed ONE event through the single-writer pipeline: keepalive liveness tick ->
 * jr_transition -> snapshot fold -> execute commands. The ONLY place
 * SessionState is written. */
static void orch_process(jr_orch_t *app, jr_event_t e, uint64_t now)
{
    if (is_downlink_frame(e.kind)) {
        jr_keepalive_on_traffic(&app->keepalive, now);
    }

    const jr_state_t prev = app->session.phase;
    jr_outcome_t o = jr_transition(app->session, e, app->clock);

    jr_snapshot_fold(&app->snap, prev, e, &o);
    app->session = o.next;                 /* single writer */
    app->events_fed++;

    orch_exec_cmds(app, &o.cmds, now);
}

/* Poll the owned monitors in a fixed priority order; process the FIRST that
 * fires and return true (the fixpoint loop re-polls after the state change).
 * Keepalive is polled before capture-pause so a stale deadline (death) wins over
 * an auto-VAD keepalive when both are past due. */
static bool orch_poll_monitors(jr_orch_t *app, uint64_t now)
{
    jr_monitor_poll_t p;

    /* The ask timer is polled FIRST, ahead of the keepalive. By construction
     * JR_ASK_DEADLINE_MS > JR_ASK_TIMEOUT_MS, so the ask timer expires 5 s
     * earlier and the ordering is normally moot; it matters when the pump was
     * starved and both are past due at the same `now`. In that race the GRACEFUL
     * exit must win — AskTimeout answers the tool call, dismisses the arcs and
     * hands the floor back to the user, whereas StaleDeadline routes to DEATH
     * and tears the session down. Never trade a recoverable timeout for a
     * reconnect. */
    if (app->ask_timer.armed &&
        (now - app->ask_timer.armed_ts) > app->ask_timer.timeout_ms) {
        /* Disarm BEFORE feeding, exactly like the backoff timer below. The
         * reducer's own DisarmAskTimer is then a harmless idempotent repeat.
         * This is not belt-and-braces: if the timer were ever armed outside
         * Asking the reducer would illegal_drop the event and emit no disarm at
         * all, and a self-re-firing monitor turns jr_orch_step's fixpoint into a
         * 512-iteration spin on every single tick, forever. */
        app->ask_timer.armed = false;
        orch_process(app, jr_event(JR_EV_ASK_TIMEOUT), now);
        return true;
    }

    p = jr_keepalive_poll(&app->keepalive, now);
    if (p.fired) { orch_process(app, p.event, now); return true; }

    p = jr_dead_uplink_poll(&app->dead_uplink);
    if (p.fired) { orch_process(app, p.event, now); return true; }

    p = jr_no_reply_poll(&app->no_reply, now);
    if (p.fired) { orch_process(app, p.event, now); return true; }

    p = jr_capture_pause_poll(&app->capture_pause, now);
    if (p.fired) { orch_process(app, p.event, now); return true; }

    if (app->backoff.armed && now >= app->backoff.fire_at) {
        app->backoff.armed = false;
        orch_process(app, jr_event(JR_EV_BACKOFF_ELAPSED), now);
        return true;
    }
    return false;
}

/* ======================================================================== *
 *  Public API                                                              *
 * ======================================================================== */

void jr_orch_init(jr_orch_t *app, jr_clock_t clock, jr_orch_io_t io,
                  jr_vad_mode_t vad_mode)
{
    if (app == NULL) {
        return;
    }
    memset(app, 0, sizeof *app);
    app->clock = clock;
    app->io = io;
    app->session = jr_session_init(vad_mode);
    jr_snapshot_init(&app->snap);
    /* monitors + backoff start disarmed (memset) — Idle arms nothing. */
}

void jr_orch_step(jr_orch_t *app, uint64_t now)
{
    if (app == NULL) {
        return;
    }
    app->steps++;

    unsigned guard = 0;
    bool progressed = true;
    while (progressed && guard < JR_ORCH_FIXPOINT_CAP) {
        guard++;
        progressed = false;

        /* (1) drain inbound transport/parser events */
        if (app->io.poll_inbound != NULL) {
            jr_event_t ev;
            while (app->io.poll_inbound(app->io.ctx, &ev)) {
                orch_process(app, ev, now);
                progressed = true;
                if (guard >= JR_ORCH_FIXPOINT_CAP) {
                    break;
                }
            }
        }

        /* (2) poll/advance the monitors; feed one fired event */
        if (orch_poll_monitors(app, now)) {
            progressed = true;
        }
    }
    app->last_fixpoint = guard;
}

void jr_orch_inject(jr_orch_t *app, jr_event_t e, uint64_t now)
{
    if (app == NULL) {
        return;
    }
    orch_process(app, e, now);
}

void jr_orch_report_tx(jr_orch_t *app, jr_err_t tx_result)
{
    if (app == NULL) {
        return;
    }
    jr_dead_uplink_on_tx(&app->dead_uplink, tx_result);
}

const jr_state_snapshot_t *jr_orch_snapshot(const jr_orch_t *app)
{
    return &app->snap;
}

jr_state_t jr_orch_phase(const jr_orch_t *app)
{
    return app->session.phase;
}

bool jr_orch_is_talking_capable(const jr_orch_t *app)
{
    return jr_state_is_live(app->session.phase);
}

bool jr_orch_is_resting(const jr_orch_t *app)
{
    jr_state_t p = app->session.phase;
    return p == JR_ST_IDLE || p == JR_ST_BACKOFF || p == JR_ST_FATAL;
}

bool jr_orch_is_zombie(const jr_orch_t *app)
{
    if (jr_orch_is_resting(app)) {
        return false;                       /* a legitimate rest, not a zombie */
    }
    /* a non-rest state must carry an armed forward-progress deadline */
    bool armed = app->keepalive.armed || app->no_reply.armed ||
                 app->backoff.armed  || app->ask_timer.armed;
    return !armed;
}
