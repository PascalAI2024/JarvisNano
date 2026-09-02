/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/display.h — Display port (L5 presenter side).
 *
 * The core publishes an immutable domain snapshot; the L5 Presenter (a
 * separate task + separate failure domain) maps it to baked AAF segments +
 * amplitude buckets. The core knows nothing of emote_gfx, RGB565, or strips.
 * There is NO runtime CPU framebuffer (hardware.md non-negotiable #5) — the
 * port speaks in domain intent, not pixels.
 */
#ifndef JR_PORTS_DISPLAY_H
#define JR_PORTS_DISPLAY_H

#include "jr_ports/jr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Coarse presentation intent derived from the L3 StateSnapshot. The Presenter
 * owns the state->baked-segment mapping; this is the neutral vocabulary. */
typedef enum {
    JR_FACE_IDLE,
    JR_FACE_LISTENING,
    JR_FACE_THINKING,
    JR_FACE_SPEAKING,
    JR_FACE_ERROR,
    /* 2026-09-02: states that used to borrow a face. Appended after ERROR so
     * the numbers the cockpit and the demo reel already report do not move. */
    JR_FACE_RESTING,    /* WHISPER/DREAM: the iris asleep, breathing slowly */
    JR_FACE_MUTED,      /* privacy: the closed iris held still, in gold */
    JR_FACE_LINKING,    /* connecting/reconnecting: the Wi-Fi orbit */
    JR_FACE_COUNT       /* bound, not a face: clip caches and clamps use it */
} jr_face_t;

typedef struct jr_display {
    void *ctx;
    /* Blank/idle fill — the Phase-0 boot target and the failure-mode signal. */
    void (*blank)(void *ctx);
    /* Present a face intent; amplitude 0..255 drives the reactive waveform
     * bucket (ignored for non-reactive faces). */
    void (*present)(void *ctx, jr_face_t face, uint8_t amplitude);
} jr_display_t;

static inline void jr_display_blank(const jr_display_t *d) { d->blank(d->ctx); }
static inline void jr_display_present(const jr_display_t *d, jr_face_t f, uint8_t a)
{
    d->present(d->ctx, f, a);
}

#ifdef __cplusplus
}
#endif

#endif /* JR_PORTS_DISPLAY_H */
