/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/src/snapshot.c — the observability StateSnapshot fold reducer.
 *
 * PURE. Only its own header (which pulls jr_core/session.h) + libc. No I/O.
 */
#include "jr_core/snapshot.h"

#include <string.h>

void jr_snapshot_init(jr_state_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }
    memset(snap, 0, sizeof *snap);
    snap->phase = JR_ST_IDLE;
    snap->last_reason = JR_EV__COUNT;      /* sentinel: no transition yet */
    snap->last_error_kind = JR_ERRK_UNKNOWN;
}

/* Does the outcome carry an EmitDiag whose reason matches `reason`? The reasons
 * are string literals emitted by the reducer ("death", "barge", ...). */
static bool has_diag_reason(const jr_cmd_list_t *L, const char *reason)
{
    for (size_t i = 0; i < L->count; ++i) {
        const jr_command_t *c = &L->cmds[i];
        if (c->kind == JR_CMD_EMIT_DIAG && c->reason != NULL &&
            strcmp(c->reason, reason) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_cmd(const jr_cmd_list_t *L, jr_cmd_kind_t k)
{
    for (size_t i = 0; i < L->count; ++i) {
        if (L->cmds[i].kind == k) {
            return true;
        }
    }
    return false;
}

void jr_snapshot_fold(jr_state_snapshot_t *snap, jr_state_t prev_phase,
                      jr_event_t e, const jr_outcome_t *o)
{
    if (snap == NULL || o == NULL) {
        return;
    }
    if (o->illegal) {
        return;                            /* an inert drop is not a transition */
    }

    const jr_state_t next = o->next.phase;

    snap->phase = next;
    snap->last_reason = e.kind;
    snap->fail_count = o->next.fail_count;

    /* an involuntary death: the shared DEATH handler stamps EmitDiag("death"). */
    if (has_diag_reason(&o->cmds, "death")) {
        snap->deaths++;
        snap->last_error_kind =
            (e.kind == JR_EV_SERVER_ERROR) ? e.error_kind : JR_ERRK_TRANSIENT;
    }

    /* an auto-redial: entering Connecting FROM Reconnecting/Backoff (no user
     * tap needed) — the "talks without typing" recovery, counted for the HUD. */
    if (has_cmd(&o->cmds, JR_CMD_CONNECT) &&
        (prev_phase == JR_ST_RECONNECTING || prev_phase == JR_ST_BACKOFF)) {
        snap->reconnects++;
    }

    /* a state-changing transition bumps the counter + pushes into the ring. */
    if (next != prev_phase) {
        snap->transitions++;
        snap->reason_ring[snap->ring_head] = e.kind;
        snap->ring_head = (snap->ring_head + 1) % JR_SNAP_RING;
        if (snap->ring_count < JR_SNAP_RING) {
            snap->ring_count++;
        }
    }
}
