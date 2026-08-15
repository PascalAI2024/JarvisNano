/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/src/turn_policy.c — L4 TurnPolicy, the REAL adaptive VAD (Phase 1).
 *
 * PURE. Only jr_dsp (jr_dsp_rms), the injected clock, and libc. No IDF, no
 * globals. Implements spec §6:
 *   §6.1 adaptive noise-floor EMA (frozen during speech AND playback)
 *        + SNR/hysteresis speech/silence turn decision;
 *   §6.2 4-frame/128 ms peak-HELD-playback barge gate
 *        effective_gate = max(adaptive_floor, BARGE_RATIO * peak_held)
 *        with a 500 ms guard window;
 *   §6.3 the cold-start (floor unconverged) suppression window.
 */
#include "jr_core/turn_policy.h"

/* Hardware-calibrated lower bound from the proven v4 Waveshare path. The
 * post-AEC quiet bed can read in the low teens; treating that as the entire
 * room floor makes ordinary codec noise clear a 2.2x onset gate and opens a
 * phantom turn. A floor of 40 keeps onset at ~88 RMS while the EMA remains
 * free to adapt upward in louder rooms. */
#define JR_TP_MIN_FLOOR 40.0f

void jr_turn_policy_init(jr_turn_policy_t *p)
{
    if (p == NULL) {
        return;
    }
    p->noise_floor = 0.0f;
    p->seeded = false;

    /* v4-PROVEN FIXED calibration at a pinned 40 floor: onset 85 (snr_on 2.125),
     * offset 48 (snr_off 1.2 — MUST stay above the floor so a genuine silence at
     * the ambient floor crosses `rms < offset_thr` and ends the turn; collapsing
     * it to the floor would leave turns unable to end). The EMA weights are
     * ZEROED: the v5 adaptive floor was the regression — it re-seeded from one
     * frame on every LISTENING entry and could climb toward the user's ~130
     * speech level, pushing the onset gate up until the device stopped hearing
     * you ("stuck listening, not responding"). A pinned floor is what worked a
     * month ago; JR_TP_MIN_FLOOR is now the single knob if mic gain drifts. */
    p->snr_on = 2.125f;
    p->snr_off = 1.2f;
    p->floor_up = 0.0f;     /* pinned: no adaptive drift */
    p->floor_down = 0.0f;
    /* Barge proportional term = 30% of peak-held playback. v4 used 10% because
     * its AEC left only ~2% residual; v5's AEC leaks ~28% on loud syllables
     * (measured residual 598-924 at playback ~3300), so the gate must sit above
     * that to avoid self-barge. Cost is v4's known limit: interrupting DURING
     * the model's loudest moments needs a raised voice; normal-volume barge
     * works in pauses and quieter passages (where the ratio term is low and the
     * 250 floor governs). */
    p->barge_ratio = 0.30f;

    p->in_speech = false;
    p->speech_run = 0;
    p->silence_run = 0;

    for (int i = 0; i < JR_TP_PEAK_FRAMES; ++i) {
        p->peak_hold[i] = 0.0f;
    }
    p->peak_idx = 0;
    p->barge_run = 0;
    p->barge_latched = false;

    p->started = false;
    p->start_ts = 0;
    p->guard_deadline_ts = 0;
    p->playback_was_active = false;
}

static float peak_held(const jr_turn_policy_t *p)
{
    float m = 0.0f;
    for (int i = 0; i < JR_TP_PEAK_FRAMES; ++i) {
        if (p->peak_hold[i] > m) {
            m = p->peak_hold[i];
        }
    }
    return m;
}

/* Thin wrapper: the frame is used for exactly one thing — its RMS. Callers that
 * already hold a better-scaled RMS (see jr_turn_policy_eval_rms) should pass it
 * directly rather than handing over a buffer to be re-measured. */
jr_turn_decision_t jr_turn_policy_eval(jr_turn_policy_t *p,
                                       const jr_pcm_t *capture, size_t capture_len,
                                       float playback_level,
                                       bool playback_active,
                                       jr_tp_substate_t substate,
                                       jr_clock_t clk)
{
    return jr_turn_policy_eval_rms(p, jr_dsp_rms(capture, capture_len),
                                   playback_level, playback_active,
                                   substate, clk);
}

jr_turn_decision_t jr_turn_policy_eval_rms(jr_turn_policy_t *p,
                                           float rms_in,
                                           float playback_level,
                                           bool playback_active,
                                           jr_tp_substate_t substate,
                                           jr_clock_t clk)
{
    jr_turn_decision_t d = {
        .is_speech = false,
        .is_silence = true,
        .is_barge = false,
        .event = JR_TP_EV_NONE,
        .rms = 0.0f,
        .noise_floor = 0.0f,
    };
    if (p == NULL) {
        return d;
    }

    const uint64_t now = jr_clock_now_ms(&clk);
    const float rms = rms_in;
    d.rms = rms;
    const bool speaking = (substate == JR_TP_SPEAKING);

    /* --- time-window bookkeeping --------------------------------------- */
    if (!p->started) {
        p->started = true;
        p->start_ts = now;
        /* Pin the floor to the fixed minimum — do NOT seed from the first frame.
         * A breath, echo tail, or word landing on the LISTENING-entry frame
         * would bias the onset gate high and swallow the user's next turn. v4
         * used constants here, not a live seed. */
        p->noise_floor = JR_TP_MIN_FLOOR;
        p->seeded = true;
    }
    /* Guard arms on the rising edge of playback (cold-AEC convergence window). */
    if (playback_active && !p->playback_was_active) {
        p->guard_deadline_ts = now + JR_TP_BARGE_GUARD_MS;
    }
    p->playback_was_active = playback_active;

    const bool cold_start = (now - p->start_ts) < JR_TP_COLD_START_MS;

    /* --- barge peak-hold ring: the KNOWN playback reference this frame --- */
    p->peak_hold[p->peak_idx] = playback_level;
    p->peak_idx = (p->peak_idx + 1) % JR_TP_PEAK_FRAMES;

    /* --- adaptive noise-floor EMA (spec §6.1) --------------------------- *
     * Adapt ONLY when the frame looks like ambient noise (below onset) AND we
     * are neither latched in speech NOR playing back — echo / speech would
     * poison the floor. This freeze-during-playback rule is why the floor stays
     * honest while the DAC drives the echo reference. */
    float floor = (p->noise_floor > JR_TP_MIN_FLOOR) ? p->noise_floor : JR_TP_MIN_FLOOR;
    const float onset_thr = floor * p->snr_on;
    const float offset_thr = floor * p->snr_off;

    const bool looks_like_noise = (rms < onset_thr);
    if (looks_like_noise && !p->in_speech && !playback_active && !speaking) {
        const float w = (rms > p->noise_floor) ? p->floor_up : p->floor_down;
        p->noise_floor += w * (rms - p->noise_floor);
        if (p->noise_floor < JR_TP_MIN_FLOOR) {
            p->noise_floor = JR_TP_MIN_FLOOR;
        }
    }
    d.noise_floor = p->noise_floor;

    /* --- turn (speech/silence) decision — Listening / Thinking only ----- *
     * Runs the latch even under Speaking so state is consistent, but the L3
     * event is suppressed there (a bare SpeechStarted during Speaking is a
     * no-op — only the barge gate acts; spec §4.6 / §6.2). */
    if (speaking) {
        /* Speaking has its own echo-aware barge gate below. Carrying the normal
         * speech latch through model playback lets residual echo pre-latch a
         * phantom user turn, which then survives into Listening. */
        p->in_speech = false;
        p->speech_run = 0;
        p->silence_run = 0;
    } else if (!p->in_speech) {
        if (rms > onset_thr) {
            p->speech_run++;
        } else if (rms < offset_thr && p->speech_run > 0) {
            /* Match the field-proven VAD: genuine silence decays the onset
             * accumulator; the hysteresis dead-zone holds it across syllable
             * and inter-word dips. Requiring eight strictly consecutive loud
             * frames swallowed normal speech on the attached device. */
            p->speech_run--;
        }
        if (p->speech_run >= JR_TP_MIN_SPEECH_FRAMES && !cold_start) {
            p->in_speech = true;
            p->speech_run = 0;      /* accumulator reset on commit (spec §6.1) */
            p->silence_run = 0;
            d.event = JR_TP_EV_SPEECH_STARTED;
        }
    } else {
        if (rms < offset_thr) {
            p->silence_run++;
        } else {
            p->silence_run = 0;     /* inter-word pause: hysteresis holds       */
        }
        if (p->silence_run >= JR_TP_HANGOVER_FRAMES) {
            p->in_speech = false;
            p->silence_run = 0;
            d.event = JR_TP_EV_SPEECH_ENDED;
        }
    }
    d.is_speech = p->in_speech;
    d.is_silence = !p->in_speech;

    /* --- adaptive barge gate — Speaking only (spec §6.2) ---------------- */
    if (speaking) {
        const float held = peak_held(p);
        const float ratio_gate = p->barge_ratio * held;
        /* adaptive_floor_barge replaces the fixed floor of 80 (spec §6.2). That
         * 80 sat ~2x the ~40 ambient floor, i.e. at the speech-onset level — a
         * barge IS user speech, so it must clear the same onset bar. Tracking
         * floor*snr_on reproduces "80" adaptively without hugging the ambient
         * noise (which would self-trigger). */
        /* Barge floor is a dedicated absolute (JR_TP_BARGE_FLOOR), NOT the VAD
         * onset — a barge is louder speech OVER playback and must clear the
         * echo residual, which sits above the ambient onset gate. */
        float floor_barge = floor * p->snr_on;
        if (floor_barge < JR_TP_BARGE_FLOOR) {
            floor_barge = JR_TP_BARGE_FLOOR;
        }
        const float gate = (ratio_gate > floor_barge) ? ratio_gate : floor_barge;
        d.barge_gate = gate;        /* observability: what the barge had to beat */
        d.peak_play = held;
        const bool past_guard = (now > p->guard_deadline_ts) && !cold_start;

        if (past_guard && rms > gate) {
            p->barge_run++;
        } else {
            p->barge_run = 0;
        }

        if (p->barge_run >= JR_TP_BARGE_LATCH) {
            d.is_barge = true;
            if (!p->barge_latched) {        /* fire the event once per barge */
                p->barge_latched = true;
                d.event = JR_TP_EV_BARGE_DETECTED;
            }
        } else {
            d.is_barge = false;
            p->barge_latched = false;
        }
    } else {
        /* leaving Speaking clears the barge latch + run (fresh next turn) */
        p->barge_run = 0;
        p->barge_latched = false;
    }

    return d;
}
