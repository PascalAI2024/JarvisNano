/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/src/turn_policy.c — L4 TurnPolicy (Phase 0 skeleton).
 *
 * PURE. Only jr_dsp + libc. No IDF.
 */
#include "jr_core/turn_policy.h"

void jr_turn_policy_init(jr_turn_policy_t *p)
{
    if (p == NULL) {
        return;
    }
    jr_vad_init(&p->vad);
    p->playback_peak = 0.0f;
}

jr_turn_decision_t jr_turn_policy_eval(jr_turn_policy_t *p,
                                       float capture_rms,
                                       float playback_rms,
                                       bool playback_active)
{
    jr_turn_decision_t d = { false, true, false };
    if (p == NULL) {
        return d;
    }

    /* Phase-0 stub: fast-attack / slow-decay peak hold on the playback level so
     * the barge gate has an echo-tail guard, and a placeholder speech decision
     * via jr_vad. Phase 1 replaces with the real adaptive floor + the
     * max(adaptive-floor, ratio% * peak-held-playback) gate. */
    if (playback_rms > p->playback_peak) {
        p->playback_peak = playback_rms;               /* fast attack */
    } else {
        p->playback_peak *= 0.90f;                     /* slow decay  */
    }

    d.is_speech = jr_vad_update(&p->vad, capture_rms);
    d.is_silence = !d.is_speech;
    d.is_barge = d.is_speech && playback_active &&
                 (capture_rms > p->playback_peak);
    return d;
}
