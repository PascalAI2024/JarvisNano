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

/* A tone is about to play. Returns false, changing nothing, when the owner
 * has muted the speaker. Otherwise unmutes the DAC for the tone and, when it
 * was muted, records that the mute is owed back. */
bool jr_dac_mute_cue_begin(jr_dac_mute_t *m, bool owner_muted);

/* The same, for a tone that may be the acknowledgement of the owner's own mute
 * gesture (`owner_ack`): it plays even though the owner mute is set, and the
 * DAC rule is unchanged (a muted DAC is owed back, the owner's state is never
 * touched). Every other tone passes owner_ack = false. */
bool jr_dac_mute_tone_begin(jr_dac_mute_t *m, bool owner_muted, bool owner_ack);

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
