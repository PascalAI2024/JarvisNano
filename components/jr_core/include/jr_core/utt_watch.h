/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/utt_watch.h - "the owner spoke and nothing answered", as policy.
 *
 * Gemini owns turn detection, so when the server stays quiet the device has
 * nothing to wake it: the local VAD sees a whole utterance, the uplink runs, no
 * frame comes back. This watch times that silence and decides what to do:
 *
 *   1st miss   PARDON     one short text turn; the model says "Pardon, Sir?"
 *   2nd miss   RECONNECT  the session is deaf: a fresh one
 *
 * IN A ROW MEANS IN A ROW. Until 2026-11-10 the count was only cleared when
 * the deadline was still armed as the phase left Listening, and a server reply
 * clears the deadline first. So a miss followed by seven answered utterances
 * still stood, and the next miss, minutes later, threw the session (and the
 * conversation in it) away. Any reply-bearing server frame now clears both.
 *
 * Pure: no clock of its own, no I/O. The caller passes now_ms and acts on the
 * returned action.
 */
#ifndef JR_CORE_UTT_WATCH_H
#define JR_CORE_UTT_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_UTT_MIN_MS         800u    /* shorter than this is a cough, not a question */
#define JR_UTT_REPLY_WAIT_MS  7000u   /* silence after speech end that counts as a miss */
#define JR_UTT_DEAF_MISSES    2u      /* consecutive misses that end the session */

typedef enum {
    JR_UTT_NONE = 0,
    JR_UTT_PARDON,      /* ask for it again, briefly */
    JR_UTT_RECONNECT,   /* the server is deaf: fresh session */
} jr_utt_action_t;

typedef struct {
    uint32_t start_ms;      /* local VAD speech start */
    uint32_t deadline_ms;   /* when silence becomes a miss (valid while armed) */
    bool     armed;
    bool     pardon_turn;   /* a PARDON text turn is in flight: its reply is not an answer */
    uint8_t  misses;        /* consecutive, since the server last answered */
    uint32_t misses_total;  /* lifetime, for the cockpit */
    uint32_t answered_total;
} jr_utt_watch_t;

void jr_utt_init(jr_utt_watch_t *w);

/* The local VAD saw speech begin. */
void jr_utt_speech_start(jr_utt_watch_t *w, uint32_t now_ms);

/* The local VAD saw speech end. `listening` is the orchestrator's phase and
 * `text_turn_pending` whether the device itself has a text turn queued (that
 * turn is its own answer to wait for, not this one). Arms the clock when the
 * utterance was long enough. */
void jr_utt_speech_end(jr_utt_watch_t *w, uint32_t now_ms, bool listening,
                       bool text_turn_pending);

/* Any server frame at all: the server is there, so this utterance is not a
 * miss yet. Does NOT clear the run of misses (a frame is not an answer). */
void jr_utt_server_alive(jr_utt_watch_t *w);

/* A reply-bearing server frame (audio, text, tool call, interruption, turn
 * boundary): the owner was answered. Clears the clock and the run of misses,
 * unless the reply is to the PARDON text turn the watch itself sent: that
 * proves the server hears text, not that it hears the owner, so only the
 * clock is cleared and the next utterance is judged on its own. */
void jr_utt_answered(jr_utt_watch_t *w);

/* Call every loop. Returns the action once per miss. */
jr_utt_action_t jr_utt_poll(jr_utt_watch_t *w, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_UTT_WATCH_H */
