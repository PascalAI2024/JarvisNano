/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/monitors.h — the stateful resilience collaborators (spec §5).
 *
 * These are the pure, Clock-injected monitors that EMIT the events the L3
 * machine already consumes (StaleDeadline / UplinkDead / NoReplyTimeout /
 * CapturePauseElapsed). None does I/O; none reaches for a timer. Each is driven
 * by the SAME injected clock as the reducer, so host tests advance a fake clock
 * past a deadline (asserts the event fires) and feed traffic before it (asserts
 * it re-arms and does NOT fire). This is the single-owner self-heal rule of
 * spec §5.7: ONE task arms/polls them; they only return events, they never act.
 *
 * The 6 resilience primitives of spec §8:
 *   1. ReconnectPolicy       — pure fn in session.c (jr_reconnect_policy)
 *   2. KeepaliveMonitor      — this file (staleness)
 *   3. DeadUplinkMonitor     — this file (split-brain cure)
 *   4. NoReplyWatchdog       — this file (Thinking > 20 s)
 *   5. CapturePauseKeepalive — this file (auto-VAD capture pause > 1 s)
 *   6. Backpressure          — this file (jr_bp_ring, drop-newest) + the
 *                              jr_err_is_backpressure classifier in session.h
 *
 * The connect / handshake / drain deadlines and the backoff timer of §5 are the
 * SAME KeepaliveMonitor (different deadline_ms) and the ScheduleBackoff command
 * the reducer already emits — not separate objects.
 */
#ifndef JR_CORE_MONITORS_H
#define JR_CORE_MONITORS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "jr_core/session.h"     /* jr_event_t, jr_event_kind_t, jr_err_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Optional event a monitor poll yields. `fired` false => `event` is unset. */
typedef struct {
    bool       fired;
    jr_event_t event;
} jr_monitor_poll_t;

/* ======================================================================== *
 *  5.2 KeepaliveMonitor — staleness (emits StaleDeadline{age})             *
 * ======================================================================== */
/* Per-state deadline (CONNECT/HANDSHAKE/LIVE/DRAIN); last_rx_ts is updated on
 * EVERY server frame including Heartbeat/sessionResumptionUpdate (v4 discarded
 * these — their absence is the perfect death signal). */
typedef struct {
    bool     armed;
    uint64_t last_rx_ts;
    uint32_t deadline_ms;
} jr_keepalive_monitor_t;

void jr_keepalive_arm(jr_keepalive_monitor_t *m, uint64_t now, uint32_t deadline_ms);
void jr_keepalive_on_traffic(jr_keepalive_monitor_t *m, uint64_t now); /* re-arm */
void jr_keepalive_disarm(jr_keepalive_monitor_t *m);
jr_monitor_poll_t jr_keepalive_poll(const jr_keepalive_monitor_t *m, uint64_t now);

/* ======================================================================== *
 *  5.3 DeadUplinkMonitor — split-brain cure (emits UplinkDead{n})          *
 * ======================================================================== */
/* Counts CONSECUTIVE tx failures; fires at TX_FAIL_RESUME (25). A would-block
 * is backpressure, NOT a tx failure (spec §5.6) — it does not increment. This
 * is independent of KeepaliveMonitor: downlink heartbeats resetting the stale
 * clock must NOT mask a dead uplink (observed tx_send_failures=175). */
typedef struct {
    bool     armed;
    uint32_t consecutive_fails;
    uint32_t threshold;
} jr_dead_uplink_monitor_t;

void jr_dead_uplink_arm(jr_dead_uplink_monitor_t *m, uint32_t threshold);
void jr_dead_uplink_disarm(jr_dead_uplink_monitor_t *m);
/* Feed one tx result. JR_OK re-arms (counter -> 0); a real failure increments;
 * JR_ERR_WOULD_BLOCK is ignored (backpressure, not a failure). */
void jr_dead_uplink_on_tx(jr_dead_uplink_monitor_t *m, jr_err_t tx_result);
jr_monitor_poll_t jr_dead_uplink_poll(const jr_dead_uplink_monitor_t *m);

/* ======================================================================== *
 *  5.4 NoReplyWatchdog — Thinking > 20 s (emits NoReplyTimeout{age})       *
 * ======================================================================== */
/* Armed on entry to Live.Thinking; disarmed on the first ServerAudioChunk or
 * any exit. 20 s in Thinking with no server audio => resume Listening. */
typedef struct {
    bool     armed;
    uint64_t entered_ts;
    uint32_t timeout_ms;
} jr_no_reply_watchdog_t;

void jr_no_reply_arm(jr_no_reply_watchdog_t *m, uint64_t now, uint32_t timeout_ms);
void jr_no_reply_disarm(jr_no_reply_watchdog_t *m);   /* first audio chunk / exit */
jr_monitor_poll_t jr_no_reply_poll(const jr_no_reply_watchdog_t *m, uint64_t now);

/* ======================================================================== *
 *  5.5 CapturePauseKeepalive — auto-VAD only (emits CapturePauseElapsed)   *
 * ======================================================================== */
/* A capture pause > CAPTURE_PAUSE_MS (1000) in auto-VAD => the machine emits
 * SendAudioStreamEnd. Never fires in manual mode. on_capture re-arms. */
typedef struct {
    bool     armed;
    bool     auto_vad;
    uint64_t last_capture_ts;
    uint32_t timeout_ms;
} jr_capture_pause_monitor_t;

void jr_capture_pause_arm(jr_capture_pause_monitor_t *m, uint64_t now,
                          uint32_t timeout_ms, bool auto_vad);
void jr_capture_pause_on_capture(jr_capture_pause_monitor_t *m, uint64_t now); /* re-arm */
void jr_capture_pause_disarm(jr_capture_pause_monitor_t *m);
jr_monitor_poll_t jr_capture_pause_poll(const jr_capture_pause_monitor_t *m, uint64_t now);

/* ======================================================================== *
 *  5.6 Backpressure — the bounded, drop-newest queue IS the primitive      *
 * ======================================================================== */
/* Fixed-capacity ring: keep OLDEST, drop NEWEST past capacity, count drops. A
 * saturated uplink is bounded memory + a drops counter, never a DEATH. Holds
 * opaque token slots (the adapter maps frames -> tokens). */
#define JR_BP_CAP 8
typedef struct {
    uint32_t slots[JR_BP_CAP];
    size_t   len;
    size_t   cap;      /* <= JR_BP_CAP */
    uint32_t drops;
} jr_bp_ring_t;

void   jr_bp_init(jr_bp_ring_t *r, size_t cap);
/* Returns true if the value was stored, false if dropped (newest-drop). */
bool   jr_bp_push(jr_bp_ring_t *r, uint32_t v);
bool   jr_bp_pop(jr_bp_ring_t *r, uint32_t *out);  /* oldest first (FIFO)       */

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_MONITORS_H */
