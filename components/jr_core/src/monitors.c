/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/src/monitors.c — the stateful resilience collaborators (spec §5).
 *
 * PURE. Includes only its own header (which pulls jr_core/session.h for the
 * event vocabulary) and libc. No IDF, no timer, no globals. Each monitor is a
 * value struct the single owner task arms/polls; poll() returns the event the
 * L3 machine consumes — the monitor NEVER acts.
 */
#include "jr_core/monitors.h"

#include <string.h>

static jr_monitor_poll_t no_fire(void)
{
    jr_monitor_poll_t r;
    memset(&r, 0, sizeof r);
    r.fired = false;
    return r;
}

static jr_monitor_poll_t fire(jr_event_kind_t kind, uint32_t age_ms, uint32_t tx_fail)
{
    jr_monitor_poll_t r;
    memset(&r, 0, sizeof r);
    r.fired = true;
    r.event = jr_event(kind);
    r.event.age_ms = age_ms;
    r.event.consecutive_tx_failures = tx_fail;
    return r;
}

/* ---------------------- 5.2 KeepaliveMonitor ----------------------------- */

void jr_keepalive_arm(jr_keepalive_monitor_t *m, uint64_t now, uint32_t deadline_ms)
{
    if (m == NULL) {
        return;
    }
    m->armed = true;
    m->last_rx_ts = now;
    m->deadline_ms = deadline_ms;
}

void jr_keepalive_on_traffic(jr_keepalive_monitor_t *m, uint64_t now)
{
    if (m == NULL || !m->armed) {
        return;
    }
    m->last_rx_ts = now;             /* every server frame re-arms the deadline */
}

void jr_keepalive_disarm(jr_keepalive_monitor_t *m)
{
    if (m != NULL) {
        m->armed = false;
    }
}

jr_monitor_poll_t jr_keepalive_poll(const jr_keepalive_monitor_t *m, uint64_t now)
{
    if (m == NULL || !m->armed) {
        return no_fire();
    }
    if (now - m->last_rx_ts > m->deadline_ms) {
        return fire(JR_EV_STALE_DEADLINE, (uint32_t)(now - m->last_rx_ts), 0);
    }
    return no_fire();
}

/* ---------------------- 5.3 DeadUplinkMonitor ---------------------------- */

void jr_dead_uplink_arm(jr_dead_uplink_monitor_t *m, uint32_t threshold)
{
    if (m == NULL) {
        return;
    }
    m->armed = true;
    m->consecutive_fails = 0;
    m->threshold = (threshold == 0) ? JR_TX_FAIL_RESUME : threshold;
}

void jr_dead_uplink_disarm(jr_dead_uplink_monitor_t *m)
{
    if (m != NULL) {
        m->armed = false;
        m->consecutive_fails = 0;
    }
}

void jr_dead_uplink_on_tx(jr_dead_uplink_monitor_t *m, jr_err_t tx_result)
{
    if (m == NULL || !m->armed) {
        return;
    }
    if (jr_err_is_backpressure(tx_result)) {
        return;                       /* would-block is NOT a tx failure (§5.6) */
    }
    if (tx_result == JR_OK) {
        m->consecutive_fails = 0;     /* a good send re-arms the counter        */
    } else {
        m->consecutive_fails++;       /* a real failure (FAIL/CLOSED/TIMEOUT)   */
    }
}

jr_monitor_poll_t jr_dead_uplink_poll(const jr_dead_uplink_monitor_t *m)
{
    if (m == NULL || !m->armed) {
        return no_fire();
    }
    if (m->consecutive_fails >= m->threshold) {
        return fire(JR_EV_UPLINK_DEAD, 0, m->consecutive_fails);
    }
    return no_fire();
}

/* ---------------------- 5.4 NoReplyWatchdog ------------------------------ */

void jr_no_reply_arm(jr_no_reply_watchdog_t *m, uint64_t now, uint32_t timeout_ms)
{
    if (m == NULL) {
        return;
    }
    m->armed = true;
    m->entered_ts = now;
    m->timeout_ms = timeout_ms;
}

void jr_no_reply_disarm(jr_no_reply_watchdog_t *m)
{
    if (m != NULL) {
        m->armed = false;
    }
}

jr_monitor_poll_t jr_no_reply_poll(const jr_no_reply_watchdog_t *m, uint64_t now)
{
    if (m == NULL || !m->armed) {
        return no_fire();
    }
    if (now - m->entered_ts > m->timeout_ms) {
        return fire(JR_EV_NO_REPLY_TIMEOUT, (uint32_t)(now - m->entered_ts), 0);
    }
    return no_fire();
}

/* ---------------------- 5.5 CapturePauseKeepalive ------------------------ */

void jr_capture_pause_arm(jr_capture_pause_monitor_t *m, uint64_t now,
                          uint32_t timeout_ms, bool auto_vad)
{
    if (m == NULL) {
        return;
    }
    m->armed = true;
    m->auto_vad = auto_vad;
    m->last_capture_ts = now;
    m->timeout_ms = timeout_ms;
}

void jr_capture_pause_on_capture(jr_capture_pause_monitor_t *m, uint64_t now)
{
    if (m == NULL || !m->armed) {
        return;
    }
    m->last_capture_ts = now;         /* a captured frame re-arms the pause timer */
}

void jr_capture_pause_disarm(jr_capture_pause_monitor_t *m)
{
    if (m != NULL) {
        m->armed = false;
    }
}

jr_monitor_poll_t jr_capture_pause_poll(const jr_capture_pause_monitor_t *m, uint64_t now)
{
    if (m == NULL || !m->armed || !m->auto_vad) {
        return no_fire();             /* never fires in manual mode (spec §5.5) */
    }
    if (now - m->last_capture_ts > m->timeout_ms) {
        return fire(JR_EV_CAPTURE_PAUSE_ELAPSED, (uint32_t)(now - m->last_capture_ts), 0);
    }
    return no_fire();
}

/* ---------------------- 5.6 Backpressure (bounded, drop-newest) ---------- */

void jr_bp_init(jr_bp_ring_t *r, size_t cap)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof *r);
    r->cap = (cap > JR_BP_CAP) ? JR_BP_CAP : cap;
}

bool jr_bp_push(jr_bp_ring_t *r, uint32_t v)
{
    if (r == NULL) {
        return false;
    }
    if (r->len < r->cap) {
        r->slots[r->len++] = v;       /* keep oldest */
        return true;
    }
    r->drops++;                       /* drop newest — bounded memory, no DEATH */
    return false;
}

bool jr_bp_pop(jr_bp_ring_t *r, uint32_t *out)
{
    if (r == NULL || r->len == 0) {
        return false;
    }
    if (out != NULL) {
        *out = r->slots[0];
    }
    for (size_t i = 1; i < r->len; ++i) {
        r->slots[i - 1] = r->slots[i];
    }
    r->len--;
    return true;
}
