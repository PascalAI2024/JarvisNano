/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/audio_source.h — AudioSource port (L0 capture side).
 *
 * The core pulls already-demuxed, already-downsampled 16 kHz mono mic frames.
 * TDM demux (mic = lane 0, ref = lane 2), AEC, and the mandatory 24->16
 * downsample all live behind this port in the L1 adapter — the core never
 * sees a lane or a sample rate mismatch (hardware.md non-negotiables #1-3).
 */
#ifndef JR_PORTS_AUDIO_SOURCE_H
#define JR_PORTS_AUDIO_SOURCE_H

#include "jr_ports/jr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jr_audio_source {
    void *ctx;
    /* Read up to max_samples of 16 kHz mono PCM into frame.
     * Returns samples read (>=0), or a negative jr_err_t on failure. */
    int (*read)(void *ctx, jr_pcm_t *frame, size_t max_samples);
} jr_audio_source_t;

static inline int jr_audio_source_read(const jr_audio_source_t *s,
                                       jr_pcm_t *frame, size_t max_samples)
{
    return s->read(s->ctx, frame, max_samples);
}

#ifdef __cplusplus
}
#endif

#endif /* JR_PORTS_AUDIO_SOURCE_H */
