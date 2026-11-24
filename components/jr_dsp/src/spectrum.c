/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_dsp/spectrum.c — fixed-point 64-pt radix-2 FFT + 16-band spectrum.
 *
 * Pure integer C with lookup tables in .rodata (0 B BSS, 0 heap allocs,
 * 0 floats). Intended for real-time audio visualization at 16 kHz or 24 kHz.
 */
#include "jr_dsp/spectrum.h"

#include <string.h>

#define JR_SPEC_FFT_N      64U
#define JR_SPEC_GATE_RMS   120U
#define JR_SPEC_GATE_MSQ   (JR_SPEC_GATE_RMS * JR_SPEC_GATE_RMS)
#define JR_SPEC_ENV_FLOOR  1200U
#define JR_SPEC_BAND_FLOOR 48U

/* 6-bit bit-reversal permutation for 64-point radix-2 DIT FFT. */
static const uint8_t s_bitrev[JR_SPEC_FFT_N] = {
     0, 32, 16, 48,  8, 40, 24, 56,  4, 36, 20, 52, 12, 44, 28, 60,
     2, 34, 18, 50, 10, 42, 26, 58,  6, 38, 22, 54, 14, 46, 30, 62,
     1, 33, 17, 49,  9, 41, 25, 57,  5, 37, 21, 53, 13, 45, 29, 61,
     3, 35, 19, 51, 11, 43, 27, 59,  7, 39, 23, 55, 15, 47, 31, 63
};

/* Periodic 64-point Hann window in Q15 (0..32767). */
static const int16_t s_hann_q15[JR_SPEC_FFT_N] = {
        0,    79,   315,   705,  1247,  1935,  2761,  3719,
     4799,  5990,  7281,  8660, 10114, 11628, 13187, 14778,
    16384, 17989, 19580, 21139, 22653, 24107, 25486, 26777,
    27968, 29048, 30006, 30832, 31520, 32062, 32452, 32688,
    32767, 32688, 32452, 32062, 31520, 30832, 30006, 29048,
    27968, 26777, 25486, 24107, 22653, 21139, 19580, 17989,
    16384, 14778, 13187, 11628, 10114,  8660,  7281,  5990,
     4799,  3719,  2761,  1935,  1247,   705,   315,    79
};

/* Q15 twiddle table: W_64^k = cos(2*pi*k/64) - j*sin(2*pi*k/64), k = 0..31. */
static const int16_t s_tw_cos_q15[JR_SPEC_FFT_N / 2U] = {
     32767,  32609,  32137,  31356,  30273,  28898,  27245,  25329,
     23170,  20787,  18204,  15446,  12539,   9512,   6393,   3212,
         0,  -3212,  -6393,  -9512, -12539, -15446, -18204, -20787,
    -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609
};

static const int16_t s_tw_sin_q15[JR_SPEC_FFT_N / 2U] = {
         0,  -3212,  -6393,  -9512, -12539, -15446, -18204, -20787,
    -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
    -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
    -23170, -20787, -18204, -15446, -12539,  -9512,  -6393,  -3212
};

/* Log-spaced mapping of FFT bins 1..31 into 16 perceptual bands:
 * bands 0..7 = 1 bin each, bands 8..10 = 2 bins, bands 11..13 = 3 bins,
 * bands 14..15 = 4 bins (total = 31 bins, covering 1..31 exactly). */
static const uint8_t s_band_start[JR_SPECTRUM_BANDS] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 13, 15, 18, 21, 24, 28
};

static const uint8_t s_band_count[JR_SPECTRUM_BANDS] = {
    1, 1, 1, 1, 1, 1, 1, 1, 2,  2,  2,  3,  3,  3,  4,  4
};

/* Gentle perceptual high-frequency tilt (Q8: 256 = 1.0x .. 388 = ~1.5x). */
static const uint16_t s_band_tilt_q8[JR_SPECTRUM_BANDS] = {
    256, 260, 264, 270, 276, 282, 290, 298,
    308, 318, 328, 340, 352, 364, 376, 388
};

static uint32_t isqrt64(uint64_t x)
{
    uint64_t res = 0u;
    uint64_t bit = (uint64_t)1u << 62;
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)res;
}

static uint32_t isqrt32(uint32_t x)
{
    uint32_t res = 0u;
    uint32_t bit = 1u << 30;
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

static void spec_decay_all(jr_spectrum_t *s)
{
    for (size_t b = 0u; b < JR_SPECTRUM_BANDS; ++b) {
        /* 65% old (166/256 = 0.6484); integer truncation guarantees exact 0. */
        s->bands[b] = (uint8_t)(((uint32_t)s->bands[b] * 166u) >> 8);
    }
    if (s->peak_env > JR_SPEC_ENV_FLOOR) {
        uint32_t drop = (uint32_t)(s->peak_env - JR_SPEC_ENV_FLOOR) >> 5;
        if (drop == 0u) {
            drop = 1u;
        }
        s->peak_env = (uint16_t)(s->peak_env - drop);
    } else {
        s->peak_env = JR_SPEC_ENV_FLOOR;
    }
}

void jr_spectrum_reset(jr_spectrum_t *s)
{
    if (s == NULL) {
        return;
    }
    memset(s->bands, 0, sizeof(s->bands));
    s->peak_env = JR_SPEC_ENV_FLOOR;
}

void jr_spectrum_init(jr_spectrum_t *s)
{
    jr_spectrum_reset(s);
}

void jr_spectrum_feed(jr_spectrum_t *s, const int16_t *pcm, size_t n_samples)
{
    if (s == NULL) {
        return;
    }
    if (pcm == NULL || n_samples < JR_SPEC_FFT_N) {
        spec_decay_all(s);
        return;
    }

    int32_t x[JR_SPEC_FFT_N];
    if (n_samples >= (JR_SPEC_FFT_N * 2U)) {
        /* 2:1 boxcar low-pass decimation centered in the chunk:
         * 24 kHz -> 12 kHz (187.5 Hz/bin, bins 1..31 = 188 Hz..5.8 kHz);
         * 16 kHz ->  8 kHz (125.0 Hz/bin, bins 1..31 = 125 Hz..3.9 kHz). */
        const size_t base = (n_samples - (JR_SPEC_FFT_N * 2U)) >> 1;
        for (size_t i = 0u; i < JR_SPEC_FFT_N; ++i) {
            const size_t p = base + (i << 1);
            x[i] = ((int32_t)pcm[p] + (int32_t)pcm[p + 1u]) >> 1;
        }
    } else {
        const size_t base = (n_samples - JR_SPEC_FFT_N) >> 1;
        for (size_t i = 0u; i < JR_SPEC_FFT_N; ++i) {
            x[i] = (int32_t)pcm[base + i];
        }
    }

    /* Remove DC offset and evaluate RMS noise gate on the analysis window. */
    int32_t sum = 0;
    for (size_t i = 0u; i < JR_SPEC_FFT_N; ++i) {
        sum += x[i];
    }
    const int32_t mean = sum / (int32_t)JR_SPEC_FFT_N;
    uint64_t sum_sq = 0u;
    for (size_t i = 0u; i < JR_SPEC_FFT_N; ++i) {
        int32_t v = x[i] - mean;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        x[i] = v;
        sum_sq += (uint64_t)((int64_t)v * (int64_t)v);
    }
    if ((sum_sq >> 6) < JR_SPEC_GATE_MSQ) {
        spec_decay_all(s);
        return;
    }

    /* Apply Hann window and load in bit-reversed order. */
    int32_t re[JR_SPEC_FFT_N];
    int32_t im[JR_SPEC_FFT_N];
    for (size_t i = 0u; i < JR_SPEC_FFT_N; ++i) {
        const size_t dst = s_bitrev[i];
        re[dst] = (int32_t)(((int64_t)x[i] * (int64_t)s_hann_q15[i]) >> 15);
        im[dst] = 0;
    }

    /* 64-point radix-2 Cooley-Tukey DIT FFT (6 butterfly stages). */
    for (size_t len = 2u; len <= JR_SPEC_FFT_N; len <<= 1) {
        const size_t half = len >> 1;
        const size_t step = JR_SPEC_FFT_N / len;
        for (size_t start = 0u; start < JR_SPEC_FFT_N; start += len) {
            for (size_t k = 0u; k < half; ++k) {
                const size_t idx1 = start + k;
                const size_t idx2 = idx1 + half;
                int32_t tr;
                int32_t ti;
                if (k == 0u) {
                    tr = re[idx2];
                    ti = im[idx2];
                } else {
                    const size_t tw = k * step;
                    const int64_t wr = (int64_t)s_tw_cos_q15[tw];
                    const int64_t wi = (int64_t)s_tw_sin_q15[tw];
                    const int64_t r2 = (int64_t)re[idx2];
                    const int64_t i2 = (int64_t)im[idx2];
                    tr = (int32_t)((r2 * wr - i2 * wi) >> 15);
                    ti = (int32_t)((r2 * wi + i2 * wr) >> 15);
                }
                const int32_t ur = re[idx1];
                const int32_t ui = im[idx1];
                re[idx1] = ur + tr;
                im[idx1] = ui + ti;
                re[idx2] = ur - tr;
                im[idx2] = ui - ti;
            }
        }
    }

    /* Magnitude of bins 1..31, normalized so full-scale sine ~ 32767. */
    uint32_t bin_mag[JR_SPEC_FFT_N / 2U];
    bin_mag[0] = 0u;
    for (size_t k = 1u; k < (JR_SPEC_FFT_N / 2U); ++k) {
        const uint64_t r2 = (uint64_t)((int64_t)re[k] * (int64_t)re[k]);
        const uint64_t i2 = (uint64_t)((int64_t)im[k] * (int64_t)im[k]);
        bin_mag[k] = isqrt64(r2 + i2) >> 4;
    }

    /* Aggregate bins 1..31 into 16 perceptual bands with mild HF tilt. */
    uint32_t band_mag[JR_SPECTRUM_BANDS];
    uint32_t frame_max = 0u;
    for (size_t b = 0u; b < JR_SPECTRUM_BANDS; ++b) {
        const size_t start = s_band_start[b];
        const size_t count = s_band_count[b];
        uint32_t acc = 0u;
        for (size_t k = 0u; k < count; ++k) {
            acc += bin_mag[start + k];
        }
        uint32_t avg = acc / (uint32_t)count;
        avg = (avg * (uint32_t)s_band_tilt_q8[b]) >> 8;
        if (avg > JR_SPEC_BAND_FLOOR) {
            avg -= JR_SPEC_BAND_FLOOR;
        } else {
            avg = 0u;
        }
        band_mag[b] = avg;
        if (avg > frame_max) {
            frame_max = avg;
        }
    }

    /* Adaptive slow AGC envelope: instant upward peak track, slow release. */
    uint32_t env = s->peak_env;
    if (env < JR_SPEC_ENV_FLOOR) {
        env = JR_SPEC_ENV_FLOOR;
    }
    if (frame_max > env) {
        env = frame_max;
    } else {
        const uint32_t target_env = frame_max > JR_SPEC_ENV_FLOOR
                                        ? frame_max
                                        : JR_SPEC_ENV_FLOOR;
        uint32_t drop = (env - target_env) >> 5;
        if (drop == 0u && env > target_env) {
            drop = 1u;
        }
        env -= drop;
    }
    if (env > 65535u) {
        env = 65535u;
    }
    s->peak_env = (uint16_t)env;

    /* Sqrt perceptual compression (0..255) + fast attack / smooth decay. */
    for (size_t b = 0u; b < JR_SPECTRUM_BANDS; ++b) {
        const uint32_t v = band_mag[b];
        uint32_t target = 0u;
        if (v > 0u) {
            const uint32_t norm_q16 = (v >= env) ? 65535u : ((v << 16) / env);
            target = isqrt32(norm_q16);
            if (target > 255u) {
                target = 255u;
            }
        }
        const uint32_t prev = s->bands[b];
        uint32_t next;
        if (target > prev) {
            /* Fast attack: ~70% new + 30% old. */
            next = (target * 179u + prev * 77u + 128u) >> 8;
            if (next > 255u) {
                next = 255u;
            }
        } else {
            /* Smooth decay: ~65% old + 35% new. */
            next = (prev * 166u + target * 90u) >> 8;
        }
        s->bands[b] = (uint8_t)next;
    }
}
