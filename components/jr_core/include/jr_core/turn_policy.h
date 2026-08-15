/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/turn_policy.h — L4 Turn-taking / VAD policy (CORE strategy).
 *
 * "Is it a turn / is it a barge?" as a swappable strategy the state machine
 * consults. Phase-1 REAL implementation (spec §6, decisions/2026-07-07-adaptive-vad):
 *
 *   - an adaptive noise-floor EMA tracker (NOT fixed RMS constants) that FREEZES
 *     during user speech AND during playback (echo would poison the floor);
 *   - SNR + hysteresis speech/silence decision (separate speak-on vs silence-off
 *     thresholds — the 85/40 seed pattern) so inter-word pauses never read as
 *     end-of-turn;
 *   - a 4-frame / 128 ms peak-HELD-playback barge gate
 *       effective_gate = max(adaptive_floor, BARGE_RATIO * peak_held)
 *     with a ~500 ms guard window after playback starts, so the 60-100 ms echo
 *     tail (mic spikes while the DAC level already dropped) never self-barges;
 *   - a ~500 ms cold-start window (floor unconverged) that suppresses both the
 *     first speech commit and any barge.
 *
 * PURE: depends only on jr_dsp (jr_dsp_rms), the injected jr_clock_t (every
 * time-window is driven by the fake clock in host tests — no wall clock), and
 * the C standard library. No IDF, no globals. It is a reducer over injected
 * state: consume one PCM capture frame + the known playback reference level +
 * playback_active + the L3 substate, return the decision + the edge event the
 * L3 machine consumes.
 */
#ifndef JR_CORE_TURN_POLICY_H
#define JR_CORE_TURN_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "jr_dsp/dsp.h"          /* jr_dsp_rms — the only DSP dependency */
#include "jr_ports/jr_types.h"   /* jr_pcm_t */
#include "jr_ports/clock.h"      /* jr_clock_t — the injected time source */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Adaptive-VAD shape parameters — v4 tuned numbers as ADAPTIVE SEEDS,
 *      NOT hardcoded thresholds (spec §6.3 / calibration.md). The floor tracks
 *      the room at runtime; these only set the SNR margins + gate shape. --- */
#define JR_TP_FRAME_MS          32u     /* 512-sample @ 16 kHz esp-sr AEC frame  */
#define JR_TP_PEAK_FRAMES       4       /* barge peak-hold = 4 frames / 128 ms   */
#define JR_TP_MIN_SPEECH_FRAMES 8       /* ~240-256 ms min-speech onset          */
#define JR_TP_HANGOVER_FRAMES   22      /* ~704 ms field-tuned end-turn hangover */
/* 4 frames (128 ms) sustained over-threshold to fire a barge. v4 used 2, but
 * v5's AEC throws brief residual spikes (onset re-convergence, loud syllables)
 * that a 2-frame latch caught as false self-barges; real user talk-over is
 * easily >128 ms, so 4 filters the transients without losing genuine barges. */
#define JR_TP_BARGE_LATCH       4
/* Absolute barge floor (post-6x-gain RMS). The VAD onset (floor*snr_on=85) is
 * too low for barge: between audio chunks the ratio term decays and the ~150
 * residual echo cleared 85 and self-barged. v4 used a dedicated ~180 floor over
 * a 24-162 residual; v5's residual is ~117-151, so 250 sits above it and below
 * the user's ~400-800 talk-over. The gate is max(ratio*peak_playback, this). */
#define JR_TP_BARGE_FLOOR       250.0f
/* v4's 500 ms guard after playback starts (covers the mic-gain-drop +
 * cold-AEC onset spike). Longer hurts responsiveness — you couldn't interrupt
 * in the first second — so transient spikes past this window are filtered by
 * the 4-frame latch instead. */
#define JR_TP_BARGE_GUARD_MS    500u
#define JR_TP_COLD_START_MS     500u    /* first ~500 ms: floor unconverged      */

/* Substate the policy needs to know (subset of the L3 phase). Speaking flips on
 * the barge gate + freezes floor adaptation; Listening/Thinking run the
 * speech/silence turn logic. */
typedef enum {
    JR_TP_LISTENING = 0,
    JR_TP_THINKING,
    JR_TP_SPEAKING,
} jr_tp_substate_t;

/* The edge event the owner forwards to the L3 machine (maps 1:1 onto
 * JR_EV_SPEECH_STARTED / JR_EV_SPEECH_ENDED / JR_EV_BARGE_DETECTED). Kept as an
 * independent enum so turn_policy.h does not couple to session.h. */
typedef enum {
    JR_TP_EV_NONE = 0,
    JR_TP_EV_SPEECH_STARTED,
    JR_TP_EV_SPEECH_ENDED,
    JR_TP_EV_BARGE_DETECTED,
} jr_tp_event_t;

typedef struct {
    bool          is_speech;   /* latched speech (held through inter-word pauses) */
    bool          is_silence;  /* == !is_speech                                   */
    bool          is_barge;    /* adaptive gate crossed during Speaking           */
    jr_tp_event_t event;       /* the edge event for L3, or JR_TP_EV_NONE         */
    float         rms;         /* capture RMS used for this decision               */
    float         noise_floor; /* the tracked floor at this frame (observability) */
    float         barge_gate;  /* effective barge gate this frame (0 when not Speaking) */
    float         peak_play;   /* peak-held playback ref driving the gate         */
} jr_turn_decision_t;

typedef struct {
    /* --- adaptive noise-floor tracker (spec §6.1) --- */
    float    noise_floor;      /* EMA-tracked ambient RMS                        */
    bool     seeded;           /* floor seeded on the first frame               */
    /* SNR margins + EMA weights (seeded, runtime-tunable on device) */
    float    snr_on;           /* onset  threshold = floor * snr_on  (~2.2)     */
    float    snr_off;          /* offset threshold = floor * snr_off (~1.2, <on)*/
    float    floor_up;         /* slow-attack EMA weight when floor rises        */
    float    floor_down;       /* EMA weight when floor falls                    */
    float    barge_ratio;      /* BARGE_RATIO_PCT as a fraction (~0.10)          */

    /* --- turn (speech/silence) latch + run counters (spec §6.1) --- */
    bool     in_speech;        /* hysteresis latch                              */
    int      speech_run;       /* consecutive over-onset frames (reset on commit)*/
    int      silence_run;      /* consecutive under-offset frames               */

    /* --- barge peak-hold ring + latch (spec §6.2) --- */
    float    peak_hold[JR_TP_PEAK_FRAMES]; /* recent playback reference levels   */
    int      peak_idx;
    int      barge_run;        /* consecutive over-gate frames                   */
    bool     barge_latched;    /* rising-edge de-dup for the barge event         */

    /* --- time windows (driven by the injected clock) --- */
    bool     started;          /* first eval stamped start_ts                    */
    uint64_t start_ts;         /* policy birth (cold-start window anchor)        */
    uint64_t guard_deadline_ts;/* barge suppressed until now > this              */
    bool     playback_was_active; /* rising-edge detect to arm the guard         */
} jr_turn_policy_t;

/* Zero + seed the shape parameters (floor unknown until the first frame). */
void jr_turn_policy_init(jr_turn_policy_t *p);

/* Reduce one capture frame.
 *   capture / capture_len : the mic PCM frame (RMS derived via jr_dsp_rms)
 *   playback_level        : the KNOWN playback reference level we drive the DAC
 *                           with this frame (0 when nothing is playing) — the
 *                           barge gate's peak-hold input, NOT the mic
 *   playback_active       : the DAC is driving (arms the guard, freezes floor)
 *   substate              : the L3 substate (Speaking => run the barge gate)
 *   clk                   : injected clock — cold-start + guard windows
 * Returns the decision + the edge event for L3. Pure; mutates only *p. */
jr_turn_decision_t jr_turn_policy_eval(jr_turn_policy_t *p,
                                       const jr_pcm_t *capture, size_t capture_len,
                                       float playback_level,
                                       bool playback_active,
                                       jr_tp_substate_t substate,
                                       jr_clock_t clk);

/* Same policy, but judging an RMS the caller already has.
 *
 * The device's capture frame is amplified ~6x for the uplink so the model can
 * hear the user; that gain scales the room's noise bed up with the voice, so a
 * VAD judging the amplified buffer reads a lived-in room as continuous speech.
 * This entry point lets the composition root feed the un-amplified (AEC-clean)
 * RMS instead, on the same threshold scale. */
jr_turn_decision_t jr_turn_policy_eval_rms(jr_turn_policy_t *p,
                                           float rms_in,
                                           float playback_level,
                                           bool playback_active,
                                           jr_tp_substate_t substate,
                                           jr_clock_t clk);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_TURN_POLICY_H */
