/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/turn_policy.h — L4 Turn-taking / VAD policy (CORE strategy).
 *
 * "Is it a turn / is it a barge?" as a swappable strategy the state machine
 * consults: TurnPolicy(audio_features) -> {isSpeech, isSilence, isBarge}.
 * Adaptive noise-floor + SNR/hysteresis, NOT fixed RMS constants
 * (decisions/2026-07-07-adaptive-vad). The two VAD modes (manual-local-RMS vs
 * server-VAD) are strategies INJECTED into L3, not states.
 *
 * PURE: depends only on jr_dsp (also pure) + the C standard library. Phase 0 =
 * the decision struct + a stub that wraps jr_vad + a peak-hold barge-gate
 * placeholder; Phase 1 fills in the adaptive floor + the 4-frame/128ms
 * peak-held-playback barge gate harvested from cap_gemini_live.c.
 */
#ifndef JR_CORE_TURN_POLICY_H
#define JR_CORE_TURN_POLICY_H

#include <stdbool.h>
#include "jr_dsp/dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool is_speech;
    bool is_silence;
    bool is_barge;   /* speech detected while playback is active */
} jr_turn_decision_t;

typedef struct {
    jr_vad_t vad;
    float    playback_peak;  /* peak-held recent playback level (echo guard) */
} jr_turn_policy_t;

void jr_turn_policy_init(jr_turn_policy_t *p);

/* Evaluate one capture frame's RMS against the adaptive floor and the
 * peak-held playback level. playback_active tells the gate whether we are in a
 * potential barge window. */
jr_turn_decision_t jr_turn_policy_eval(jr_turn_policy_t *p,
                                       float capture_rms,
                                       float playback_rms,
                                       bool playback_active);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_TURN_POLICY_H */
