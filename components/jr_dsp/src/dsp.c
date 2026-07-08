/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_dsp/dsp.c — pure DSP math. Links only the C standard library.
 */
#include "jr_dsp/dsp.h"
#include <math.h>

float jr_dsp_rms(const int16_t *samples, size_t n)
{
    if (samples == NULL || n == 0) {
        return 0.0f;
    }
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double s = (double)samples[i];
        acc += s * s;
    }
    return (float)sqrt(acc / (double)n);
}

void jr_vad_init(jr_vad_t *v)
{
    if (v == NULL) {
        return;
    }
    v->noise_floor = 0.0f;
    v->snr_ratio = 3.0f;
    v->attack = 0.25f;   /* fast-attack  */
    v->release = 0.02f;  /* slow-decay   */
    v->in_speech = false;
}

bool jr_vad_update(jr_vad_t *v, float rms)
{
    /* Phase-0 stub: no noise-floor tracking yet, decision is "any energy".
     * Phase 1 replaces this with EMA floor + snr_ratio + hysteresis. */
    if (v == NULL) {
        return false;
    }
    v->in_speech = (rms > 0.0f);
    return v->in_speech;
}

size_t jr_dsp_resample_linear(const int16_t *in, size_t in_n, uint32_t in_rate,
                              int16_t *out, size_t out_cap, uint32_t out_rate)
{
    /* Phase-0 stub: nearest-sample decimation/duplication so the signature is
     * exercisable and length math is correct. Phase 1 swaps in true linear
     * interpolation (gl_resample_pcm16_linear). */
    if (in == NULL || out == NULL || in_n == 0 || in_rate == 0 || out_rate == 0) {
        return 0;
    }
    const size_t out_n_full = (size_t)(((uint64_t)in_n * out_rate) / in_rate);
    const size_t out_n = (out_n_full < out_cap) ? out_n_full : out_cap;
    for (size_t i = 0; i < out_n; ++i) {
        const size_t src = (size_t)(((uint64_t)i * in_rate) / out_rate);
        out[i] = in[(src < in_n) ? src : (in_n - 1)];
    }
    return out_n;
}
