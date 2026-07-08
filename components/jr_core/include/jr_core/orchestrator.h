/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/orchestrator.h — the PURE single-writer orchestrator pump (Phase 1
 * capstone). This is the object that COMPOSES the three green subsystems the
 * host suite already proves edge-by-edge:
 *
 *   1. the L3 session state machine   (session.c   — jr_transition)
 *   2. the 6 resilience monitors       (monitors.c  — keepalive / dead-uplink /
 *                                                     no-reply / capture-pause /
 *                                                     backoff timer / backpressure)
 *   3. the observability snapshot      (snapshot.c  — jr_snapshot_fold)
 *
 * ...behind their ports into ONE single-owner loop. `jr_orch_step()` is that
 * loop. On each tick it:
 *
 *   (a) DRAINS inbound transport/parser events from the injected I/O port;
 *   (b) FEEDS them — and any monitor-fired events — into `jr_transition`;
 *   (c) EXECUTES the returned commands: the timer/monitor/backoff/snapshot
 *       commands against the monitors it OWNS, the transport/DAC/tool commands
 *       against the injected I/O port;
 *   (d) POLLS/advances the owned monitors against the injected clock.
 *
 * It OWNS the SessionState; ports do the I/O. It is PURE: no ESP-IDF, no globals,
 * no wall-clock — every effect goes through the injected `jr_clock_t` + the
 * `jr_orch_io_t` port. On device the SAME pump is wrapped in a pinned FreeRTOS
 * task (device phase); here it is driven by a fake clock + a scriptable fake
 * ws-transport so an adversarial fault soak can PROVE the assembled core always
 * self-heals to a talking-capable state and can never go permanently silent
 * (spec §4.11 — the zombie proof, now for the whole system over time).
 *
 * Dependency direction: this file depends ONLY on jr_core + jr_ports (the same
 * pure island session.c lives on). It does NOT know about jr_transport — the
 * concrete Gemini client is wired in at the composition root (device main.c) or
 * the host harness (host/test_soak.c) through the abstract `jr_orch_io_t`.
 */
#ifndef JR_CORE_ORCHESTRATOR_H
#define JR_CORE_ORCHESTRATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "jr_core/session.h"
#include "jr_core/monitors.h"
#include "jr_core/snapshot.h"
#include "jr_ports/clock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== *
 *  The injected I/O port                                                   *
 * ======================================================================== *
 * The orchestrator OWNS the state machine, the monitors, the backoff timer,
 * and the observability snapshot. Only the OUTSIDE world is reached through
 * these two hooks — everything else is internal, so the pump stays a single
 * writer with no hidden globals.
 *
 *   poll_inbound: drain ONE pending inbound transport/parser event into *out.
 *                 Returns true if one was produced (a recv+parse happened),
 *                 false when the inbound queue is empty. The pump loops it until
 *                 it returns false each tick. On device this wraps the L2
 *                 `jr_gemini_pump_rx` + an event map; on host the fake produces
 *                 scripted/mapped events. Transport-level socket close is
 *                 surfaced here as a synthetic `JR_EV_TRANSPORT_CLOSED`.
 *
 *   exec:         execute ONE externally-visible command (Connect / SendSetup /
 *                 Send* / CloseTransport / capture / DAC / tools / diag /
 *                 snapshot). The timer/monitor/backoff commands NEVER reach
 *                 exec — the orchestrator applies those to the monitors it owns.
 */
typedef struct jr_orch_io {
    void *ctx;
    bool (*poll_inbound)(void *ctx, jr_event_t *out);
    void (*exec)(void *ctx, const jr_command_t *cmd);
} jr_orch_io_t;

/* One-shot backoff timer — the ReconnectPolicy delay the reducer schedules via
 * ScheduleBackoff, realized as an owned deadline that emits BackoffElapsed. It
 * lives inside the orchestrator so the single owner drives the auto-redial with
 * no external timer (the "talks without a tap" loop). */
typedef struct {
    bool     armed;
    uint64_t fire_at;   /* monotonic ms at which BackoffElapsed is emitted */
} jr_orch_backoff_t;

/* ======================================================================== *
 *  The orchestrator — the single-owned composition                        *
 * ======================================================================== *
 * Transparent by design (like the composition root's jr_app): a device diag
 * endpoint and the host soak read the monitors + snapshot directly. Nothing
 * outside the pump MUTATES `session`.
 */
typedef struct jr_orch {
    jr_session_t               session;       /* THE single-owned state         */
    jr_clock_t                 clock;         /* the only injected time source  */
    jr_orch_io_t               io;            /* the injected I/O port          */

    /* the owned resilience monitors (spec §5) — armed by commands, polled by
     * the pump against `clock`. */
    jr_keepalive_monitor_t     keepalive;     /* staleness / connect deadlines  */
    jr_dead_uplink_monitor_t   dead_uplink;   /* split-brain cure               */
    jr_no_reply_watchdog_t     no_reply;      /* Thinking > 20 s                */
    jr_capture_pause_monitor_t capture_pause; /* auto-VAD keepalive             */
    jr_orch_backoff_t          backoff;       /* ScheduleBackoff -> BackoffElapsed */

    jr_state_snapshot_t        snap;          /* observability fold             */

    /* bounded, monotonic instrumentation (the soak reads these) */
    uint32_t                   steps;         /* jr_orch_step calls             */
    uint32_t                   events_fed;    /* events pushed into jr_transition */
    uint32_t                   cmds_exec;     /* external commands forwarded     */
    unsigned                   last_fixpoint; /* iterations the last step took   */
} jr_orch_t;

/* Build an orchestrator in Idle: monitors disarmed, snapshot anchored at Idle,
 * `vad_mode` seeded into the SessionState. */
void jr_orch_init(jr_orch_t *app, jr_clock_t clock, jr_orch_io_t io,
                  jr_vad_mode_t vad_mode);

/* THE PUMP. One single-writer tick at time `now` (which MUST equal the injected
 * clock's current value — the device owner computes it from the same clock).
 * Drives to a FIXPOINT at `now`: repeatedly (drain inbound events -> feed them +
 * any monitor-fired events into jr_transition -> execute the returned commands)
 * until no event remains and no monitor fires, then returns. Advancing the clock
 * between steps is what makes deadlines fire. Performs ZERO I/O of its own. */
void jr_orch_step(jr_orch_t *app, uint64_t now);

/* Inject one event that did NOT come from the transport: a UI/user event
 * (UserStart/UserStop/Tap) or an L4 TurnPolicy event (SpeechStarted/SpeechEnded/
 * BargeDetected). It runs through the SAME single-writer pipeline (feed ->
 * execute -> fold). On device this is called from the owner task (or the event
 * is enqueued to it) so the single-writer discipline holds. */
void jr_orch_inject(jr_orch_t *app, jr_event_t e, uint64_t now);

/* Report one uplink tx outcome to the owned DeadUplinkMonitor — the transport
 * tx path reporting its result to the single owner (spec §5.3). JR_OK resets the
 * run, a real failure counts, a would-block is ignored (backpressure, §5.6). */
void jr_orch_report_tx(jr_orch_t *app, jr_err_t tx_result);

/* ---- read-only views (device diag + host soak) ---- */
const jr_state_snapshot_t *jr_orch_snapshot(const jr_orch_t *app);
jr_state_t                 jr_orch_phase(const jr_orch_t *app);

/* ---- the self-heal health predicates the soak asserts ---- */

/* A talking-capable configuration: Live.Listening / Live.Thinking / Live.Speaking. */
bool jr_orch_is_talking_capable(const jr_orch_t *app);

/* A LEGITIMATE resting state: Idle, Backoff(parked), or Fatal — each observable
 * and user-exitable (spec §4.11). */
bool jr_orch_is_resting(const jr_orch_t *app);

/* THE zombie test: a non-final state carrying NO armed forward-progress deadline
 * (no keepalive, no no-reply watchdog, no backoff timer). If this is ever true
 * the system could sit "Live but silent" forever. The self-heal proof asserts it
 * is ALWAYS false — every non-rest state has a deadline that WILL fire and move
 * it. (dead-uplink is a counter, not a deadline, so it does not count as
 * forward-progress here.) */
bool jr_orch_is_zombie(const jr_orch_t *app);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_ORCHESTRATOR_H */
