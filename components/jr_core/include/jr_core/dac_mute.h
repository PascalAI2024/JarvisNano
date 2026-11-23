/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_dac_mute — what a short tone may do to the speaker's mute state. Pure.
 *
 * jr_audio keeps one flag, s_muted, for "the DAC is muted". It is true at
 * boot, after every session ends (JR_CMD_MUTE_DAC_NOW), after a barge-in, in
 * sleep, and while the owner has muted the device (privacy hold, shade toggle,
 * face-down latch all end the session, which mutes the DAC). A tone has to
 * unmute the DAC to be heard at all, and the original earcon_commit() did so
 * for good: after any chime or sweep the device reported itself unmuted and
 * the feeder drained into an open amplifier until the next session end.
 *
 * Two rules:
 *   1. A tone never starts while the OWNER has muted the speaker (the owner's
 *      mute lives in main.c: privacy pause or flip latch; jr_audio asks
 *      through a probe).
 *   2. A tone never changes the mute state. If the DAC was muted when the
 *      tone claimed it, the DAC is muted again as soon as the tone has played
 *      out. A real unmute (session start, an audio push) during the tone
 *      cancels the restore, and a real mute cancels it too.
 *
 * jr_audio.c implements the same transitions on atomics; this model pins them
 * on the host, where the audio component cannot compile.
 */
#ifndef JR_CORE_DAC_MUTE_H
#define JR_CORE_DAC_MUTE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool muted;     /* the DAC is muted (jr_audio's s_muted) */
    bool restore;   /* a tone unmuted it and owes the mute back */
} jr_dac_mute_t;

/* Every kind of tone the firmware plays. The owner-mute policy is the table in
 * jr_dac_mute_sounds_under_owner_mute(): the one place to change whether a
 * given tone may sound while the owner has muted the speaker. */
typedef enum {
    JR_TONE_OWNER_ACK = 0,  /* jr_audio_play_sweep_owner_ack: mute / unmute ack */
    JR_TONE_DIAG_CHIRP,     /* jr_audio_diag_play_chirp: attention beat, self-test */
    JR_TONE_SWEEP,          /* jr_audio_play_sweep: the ask-miss reject */
    JR_TONE_CHIME,          /* jr_audio_play_chime: board-done chime */
    JR_TONE_UI_CUE,         /* jr_audio_play_cue: tick, confirm, dismiss, limit */
    JR_TONE_ALARM,          /* jr_audio_play_timer_chime: timer alarm (always audible) */
} jr_tone_kind_t;

/* May this kind of tone sound while the owner has muted the speaker? */
bool jr_dac_mute_sounds_under_owner_mute(jr_tone_kind_t kind);

/* A tone is about to play. Returns false, changing nothing, when the owner
 * has muted the speaker. Otherwise unmutes the DAC for the tone and, when it
 * was muted, records that the mute is owed back. */
bool jr_dac_mute_cue_begin(jr_dac_mute_t *m, bool owner_muted);

/* The same, by tone kind: refused only when the owner mute is set AND the
 * policy table says this kind stays silent under it. The DAC rule is the same
 * for every kind (a muted DAC is owed back, the owner mute is never touched). */
bool jr_dac_mute_tone_begin(jr_dac_mute_t *m, bool owner_muted,
                            jr_tone_kind_t kind);

/* A real unmute (session start, pushed audio): the mute is no longer owed. */
void jr_dac_mute_unmute(jr_dac_mute_t *m);

/* A real mute (fast-kill, sleep): any restore is moot. */
void jr_dac_mute_mute(jr_dac_mute_t *m);

/* The feeder found the ring empty. `playback_pending` is true while samples
 * are buffered, being written, or still sounding in the DAC tail. Mutes the
 * DAC again once a tone has fully played out; returns true when it did. */
bool jr_dac_mute_cue_settle(jr_dac_mute_t *m, bool playback_pending);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_DAC_MUTE_H */
