/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_dsp/spectrum.h — pure fixed-point 16-band audio spectrum analyzer.
 *
 * Zero hardware, zero IDF, zero floats, zero heap allocations.
 */
#ifndef JR_DSP_SPECTRUM_H
#define JR_DSP_SPECTRUM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_SPECTRUM_BANDS 16U

typedef struct {
    uint8_t bands[JR_SPECTRUM_BANDS]; /* smoothed 0..255 log-spaced energy */
    uint16_t peak_env;                /* adaptive slow AGC floor/ceiling */
} jr_spectrum_t;

void jr_spectrum_init(jr_spectrum_t *s);
void jr_spectrum_reset(jr_spectrum_t *s);
/* Feed mono int16 PCM samples (>= 64 samples; stride handles 16k or 24k).
 * Computes a 64-pt fixed-point radix-2 FFT with Hann window and updates
 * 16 smoothed bands (fast attack, smooth decay). Zero heap allocs, <150 us. */
void jr_spectrum_feed(jr_spectrum_t *s, const int16_t *pcm, size_t n_samples);

#ifdef __cplusplus
}
#endif
#endif
