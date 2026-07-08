/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/snapshot.h — the observability StateSnapshot (spec §0 / §3.5 / #7).
 *
 * The typed snapshot the machine publishes via the implicit PublishSnapshot
 * command on every state-changing transition. This is the structured-
 * observability requirement (v4 failure class #7) satisfied by construction:
 * every transition is visible the first time it fires, with no bespoke counter.
 *
 * PURE: a value struct + a fold reducer that folds one transition outcome into
 * the running snapshot. No I/O. The HTTP/JSON serialization of this struct is a
 * DEVICE-phase concern (the /api/gemini/live + /api/debug/gain seam) — NOT here.
 */
#ifndef JR_CORE_SNAPSHOT_H
#define JR_CORE_SNAPSHOT_H

#include <stdint.h>
#include <stddef.h>

#include "jr_core/session.h"     /* jr_state_t, jr_event_t, jr_outcome_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Last-N transition-reason ring (a bounded history for the diag view). */
#define JR_SNAP_RING 8

typedef struct {
    jr_state_t      phase;            /* current phase (== last next.phase)      */
    jr_event_kind_t last_reason;      /* the last transition's triggering event  */
    jr_error_kind_t last_error_kind;  /* error_kind of the last death (if any)   */

    uint32_t        transitions;      /* count of state-CHANGING transitions     */
    uint32_t        deaths;           /* count of involuntary deaths (DEATH)     */
    uint32_t        reconnects;       /* count of auto-redials (Reconn/Backoff-> */
                                      /* Connecting, no user tap)                */
    uint16_t        fail_count;       /* mirror of the session's fail_count       */

    jr_event_kind_t reason_ring[JR_SNAP_RING]; /* last-N triggering events       */
    size_t          ring_head;        /* next write index (mod JR_SNAP_RING)     */
    size_t          ring_count;       /* number of valid entries (<= RING)       */
} jr_state_snapshot_t;

/* Zeroed snapshot anchored at Idle. */
void jr_snapshot_init(jr_state_snapshot_t *snap);

/* Fold one transition into the snapshot. `prev_phase` is the phase BEFORE the
 * transition; `e` the event that drove it; `o` the outcome jr_transition
 * returned. Pure — mutates only *snap. Illegal drops fold nothing (they are not
 * transitions). */
void jr_snapshot_fold(jr_state_snapshot_t *snap, jr_state_t prev_phase,
                      jr_event_t e, const jr_outcome_t *o);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_SNAPSHOT_H */
