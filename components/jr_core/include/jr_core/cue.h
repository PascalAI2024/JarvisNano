/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_cue — quiet interface sounds. Pure: no drivers, no FreeRTOS.
 *
 * Four generated cues (24 kHz mono, nothing stored in flash):
 *   TICK     the screen changed          ~20 ms, 2 kHz, fast decay
 *   CONFIRM  a control went on           two rising notes, ~90 ms
 *   DISMISS  a control went off / closed one falling note, ~70 ms
 *   LIMIT    a slider or swipe ran out   one dull low note, ~50 ms
 *
 * jr_cue_allow() is the single gate. A cue is silent unless every rule holds:
 * the owner's ui_sounds setting is on, the touch was physical, the speaker is
 * not muted by the owner, no voice session has the microphone open, the mood
 * is AWAKE or AMBIENT, no update is running, and (ticks only) 120 ms have
 * passed since the last tick.
 */
#ifndef JR_CORE_CUE_H
#define JR_CORE_CUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jr_core/mood.h"
#include "jr_core/session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JR_CUE_SAMPLE_RATE_HZ   24000u
/* The existing completion chime plays at 8 % of full scale (4 % in WHISPER).
 * No cue peaks above 5 %, whatever the volume setting. */
#define JR_CUE_MAX_PERMILLE     50u
#define JR_CUE_TICK_MIN_GAP_MS  120u
/* Largest cue, in samples (CONFIRM, 90 ms): sizes a caller's scratch buffer. */
#define JR_CUE_MAX_SAMPLES      (JR_CUE_SAMPLE_RATE_HZ * 90u / 1000u)

typedef enum {
    JR_CUE_TICK = 0,
    JR_CUE_CONFIRM,
    JR_CUE_DISMISS,
    JR_CUE_LIMIT,
    JR_CUE_KIND_COUNT,
} jr_cue_kind_t;

typedef enum {
    JR_CUE_SOURCE_PHYSICAL = 0,   /* a finger on the glass */
    JR_CUE_SOURCE_SYNTHETIC,      /* injected over HTTP: never makes a sound */
} jr_cue_source_t;

typedef struct {
    bool ui_sounds;               /* the owner's setting */
    bool owner_muted;             /* privacy pause or flip latch */
    jr_state_t phase;             /* the voice session phase */
    jr_mood_t mood;
    bool ota_active;
    uint32_t now_ms;
} jr_cue_ctx_t;

typedef struct {
    uint32_t last_tick_ms;
    bool tick_seen;
} jr_cue_gate_t;

/* True while the microphone is open to the model or the model is answering:
 * a cue now is either an echo the model hears or noise over its voice. */
bool jr_cue_phase_blocks(jr_state_t phase);

/* The gate. On true the caller plays the cue; the tick clock is updated here,
 * so a tick that is allowed and then dropped by a busy speaker still counts. */
bool jr_cue_allow(jr_cue_gate_t *gate, const jr_cue_ctx_t *ctx,
                  jr_cue_kind_t kind, jr_cue_source_t source);

/* Amplitude, in permille of full scale, for an owner volume of 10..100. Never
 * above JR_CUE_MAX_PERMILLE. */
uint16_t jr_cue_amplitude_permille(jr_cue_kind_t kind, int owner_volume);

/* Samples one cue renders to (0 for an invalid kind). */
size_t jr_cue_samples(jr_cue_kind_t kind);

/* Render a cue at `amp_permille` into `out`. Returns the sample count, or 0
 * when `cap` is too small or the kind is invalid. Every cue starts and ends at
 * exactly zero. */
size_t jr_cue_render(jr_cue_kind_t kind, uint16_t amp_permille,
                     int16_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_CUE_H */
