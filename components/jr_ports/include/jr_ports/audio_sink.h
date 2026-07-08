/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/audio_sink.h — AudioSink port (L0 playback side).
 *
 * The core pushes 24 kHz mono PCM (native Gemini downlink rate). mute_now() is
 * the ONE deliberate low-latency exception in the architecture: a synchronous
 * fast-kill DAC mute callable from the capture task to interrupt playback on a
 * barge (~79ms), authorized by the state machine (architecture.md §Resilience,
 * hardware.md echo-tail notes). It must not block.
 */
#ifndef JR_PORTS_AUDIO_SINK_H
#define JR_PORTS_AUDIO_SINK_H

#include "jr_ports/jr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jr_audio_sink {
    void *ctx;
    /* Queue 24 kHz mono PCM for playback. Returns samples accepted (>=0) or a
     * negative jr_err_t. A full ring returns fewer than requested (drop-newest
     * backpressure) — never blocks the caller. */
    int (*write)(void *ctx, const jr_pcm_t *frame, size_t samples);
    /* Synchronous fast-kill: mute the DAC and flush the playback ring NOW.
     * Safe to call from the capture task. Must not block. */
    void (*mute_now)(void *ctx);
} jr_audio_sink_t;

static inline int jr_audio_sink_write(const jr_audio_sink_t *s,
                                      const jr_pcm_t *frame, size_t samples)
{
    return s->write(s->ctx, frame, samples);
}

static inline void jr_audio_sink_mute_now(const jr_audio_sink_t *s)
{
    s->mute_now(s->ctx);
}

#ifdef __cplusplus
}
#endif

#endif /* JR_PORTS_AUDIO_SINK_H */
