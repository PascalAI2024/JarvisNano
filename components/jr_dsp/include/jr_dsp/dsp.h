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

/* NOTE: the Phase-0 jr_vad_* stub and jr_dsp_resample_linear stub were removed
 * on 2026-07-18. Both were superseded and had ZERO production callers — their
 * only callers were host tests asserting stub behaviour, which is a false
 * signal, not coverage. The real implementations live where the data does:
 *   - VAD: adaptive floor tracking in main.c (see /api/diag/vadlog); the L4
 *     strategy is selected via jr_vad_mode_t in jr_core/session.h.
 *   - 24k->16k rate conversion: downsample_24to16_4lane() in jr_audio.c, which
 *     must preserve the 4-lane TDM interleave — a generic resampler cannot.
 * Re-add a pure primitive here only when something pure actually needs it. */

#ifdef __cplusplus
}
#endif

#endif /* JR_DSP_DSP_H */
