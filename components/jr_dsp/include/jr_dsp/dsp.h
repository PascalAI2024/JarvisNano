/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_dsp/dsp.h — pure DSP math library (part of the inner core).
 *
 * Zero hardware, zero IDF. Pure functions of numbers so the §8 on-hardware
 * tuning ordeal (11+ builds) becomes a host regression suite fed by recorded
 * WAV fixtures (architecture.md §Testability §4). Phase 0 = signatures +
 * trivial-but-correct bodies (rms) and honest stubs (adaptive VAD, resampler);
 * the real adaptive noise-floor tracker and linear resampler land in Phase 1.
 */
#ifndef JR_DSP_DSP_H
#define JR_DSP_DSP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Root-mean-square of a signed-16-bit PCM buffer. Returns 0 for an empty
 * buffer. Range 0..32768. The primitive every VAD/barge feature derives from. */
float jr_dsp_rms(const int16_t *samples, size_t n);

/* ---- Adaptive VAD (L4 TurnPolicy substrate) -----------------------------
 * NOT fixed RMS constants (decisions/2026-07-07-adaptive-vad): a tracked
 * noise floor + SNR/hysteresis. Phase 0 ships the state struct + a stub
 * decision so L4 can consume it; Phase 1 replaces the body with the real
 * exponential-moving noise-floor tracker. */
typedef struct {
    float noise_floor;   /* EMA-tracked ambient RMS */
    float snr_ratio;     /* speech threshold = noise_floor * snr_ratio */
    float attack;        /* EMA weight when rising (fast) */
    float release;       /* EMA weight when falling (slow) */
    bool  in_speech;     /* hysteresis latch */
} jr_vad_t;

/* Initialise with sane defaults (floor unknown, ratio ~3.0). */
void jr_vad_init(jr_vad_t *v);

/* Feed one frame's RMS; updates the noise floor and returns the current
 * speech decision. Phase-0 stub: floor stays 0, decision is rms > 0. */
bool jr_vad_update(jr_vad_t *v, float rms);

/* ---- Linear resampler (L1 rate-conversion substrate) --------------------
 * The mandatory 24<->16 rate conversion (hardware.md non-negotiable #2) as a
 * pure, host-testable function. Phase 0 = signature + nearest-sample stub;
 * Phase 1 = true linear interpolation harvested from gl_resample_pcm16_linear.
 * Returns the number of output samples written (<= out_cap). */
size_t jr_dsp_resample_linear(const int16_t *in, size_t in_n, uint32_t in_rate,
                              int16_t *out, size_t out_cap, uint32_t out_rate);

#ifdef __cplusplus
}
#endif

#endif /* JR_DSP_DSP_H */
