/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Render every interface cue to a WAV with the SAME generator the firmware
 * runs (components/jr_core/src/cue.c) and print what it measured.
 *
 *   cmake -S host -B host/build && cmake --build host/build --target jr_render_cues
 *   host/build/jr_render_cues <out-dir> [owner-volume 10..100]
 *
 * Output: <out-dir>/cue-<kind>.wav (24 kHz, mono, 16-bit) and one line per cue:
 * duration, peak in dBFS, first and last sample, largest sample-to-sample step.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jr_core/cue.h"

static void put16(FILE *f, unsigned v) { fputc(v & 255u, f); fputc((v >> 8) & 255u, f); }
static void put32(FILE *f, unsigned long v)
{
    put16(f, (unsigned)(v & 0xFFFFu));
    put16(f, (unsigned)((v >> 16) & 0xFFFFu));
}

static int write_wav(const char *path, const int16_t *pcm, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return -1;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36ul + (unsigned long)n * 2ul);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);                         /* PCM */
    put16(f, 1);                         /* mono */
    put32(f, JR_CUE_SAMPLE_RATE_HZ);
    put32(f, JR_CUE_SAMPLE_RATE_HZ * 2u);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, (unsigned long)n * 2ul);
    for (size_t i = 0; i < n; ++i) put16(f, (unsigned)(uint16_t)pcm[i]);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <out-dir> [owner-volume]\n", argv[0]);
        return 2;
    }
    const int volume = argc > 2 ? atoi(argv[2]) : 100;
    static const char *const names[JR_CUE_KIND_COUNT] =
        { "tick", "confirm", "dismiss", "limit" };
    static int16_t pcm[JR_CUE_MAX_SAMPLES];

    printf("owner volume %d\n", volume);
    printf("%-8s %8s %8s %9s %6s %6s %9s\n",
           "cue", "ms", "samples", "peak dBFS", "first", "last", "max step");
    for (int k = 0; k < JR_CUE_KIND_COUNT; ++k) {
        const uint16_t amp = jr_cue_amplitude_permille((jr_cue_kind_t)k, volume);
        const size_t n = jr_cue_render((jr_cue_kind_t)k, amp, pcm, JR_CUE_MAX_SAMPLES);
        if (n == 0u) {
            fprintf(stderr, "render failed for %s\n", names[k]);
            return 1;
        }
        int peak = 0, step = 0;
        for (size_t i = 0; i < n; ++i) {
            if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
            if (i > 0u && abs(pcm[i] - pcm[i - 1u]) > step) step = abs(pcm[i] - pcm[i - 1u]);
        }
        char path[512];
        snprintf(path, sizeof path, "%s/cue-%s.wav", argv[1], names[k]);
        if (write_wav(path, pcm, n) != 0) {
            fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }
        printf("%-8s %8.1f %8zu %9.2f %6d %6d %9d\n", names[k],
               1000.0 * (double)n / (double)JR_CUE_SAMPLE_RATE_HZ, n,
               20.0 * log10((double)peak / 32768.0), pcm[0], pcm[n - 1u], step);
    }
    /* For scale: the completion chime plays at 8 % of full scale. */
    printf("reference: chime 8%% of full scale = %.2f dBFS\n",
           20.0 * log10(0.08));
    return 0;
}
