/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/echo_gate.h - what the uplink carries while the speaker is audible,
 * and how a tap cuts a reply the server is still sending.
 *
 * WHY. Server VAD owns barge-in (the local barge gate is off), and whatever
 * goes up the socket while the model speaks is what it judges. The uplink is
 * the AEC output times a fixed 6x make-up gain, and in the 2026-11-10 session
 * (volume 100) the AEC output measured 1300-3500 RMS at p90 while the model
 * spoke, against 75-143 for the owner's own voice: the server heard itself and
 * cut two of five replies with nobody speaking. See
 * docs/reference/voice-loop-2026-11-10.md (echo section, 2026-11-10 second pass).
 *
 * THE GATE. While the playback path is audible, and for JR_ECHO_TAIL_MS after
 * it stops, the caller sends a frame of silence in place of the microphone.
 * The cadence is untouched: every frame is still sent, with zeros, so the
 * server's stream never stalls and nothing is "nothing".
 *
 * THIS IS NOT THE PRIVACY GATE. A muted device sends no audio at all; that
 * decision is made earlier and elsewhere (jr_audio_uplink_batch_guard and the
 * paused/flip latches in main.c) and this module cannot reopen it: it only
 * ever turns a live frame into zeros, never zeros into a live frame.
 *
 * Pure: no clock, no I/O. The caller passes now_ms and whether the speaker is
 * audible (jr_audio_playback_pending() or a very recent chunk).
 */
#ifndef JR_CORE_ECHO_GATE_H
#define JR_CORE_ECHO_GATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Silence kept on the uplink after the playback path goes quiet. The DAC's own
 * 80 ms tail is already inside "audible" (jr_audio_playback_pending); this is
 * the room on top of it. From the 2026-11-10 VAD ring: after the last loud
 * frame at the end of a reply the AEC-clean level fell under 2x the room floor
 * in 150 ms (385 -> 88 -> 39 over 94/114/150 ms; 30013 -> 198 -> 90 -> 26 over
 * 43/99/122/160 ms). That is the number; it costs the owner at most this much
 * of the onset of a sentence begun the instant the reply ends. */
#define JR_ECHO_TAIL_MS  150u

typedef struct {
    bool     seen;             /* an audible frame has been seen */
    uint32_t last_audible_ms;
    uint32_t gated_frames;     /* frames replaced by silence (cockpit) */
    uint32_t passed_frames;    /* frames that went up as the microphone */
} jr_echo_gate_t;

void jr_echo_gate_init(jr_echo_gate_t *g);

/* One captured frame. `audible`: the playback path is emitting (or has just
 * been written). `enabled`: the run-time switch (/api/debug/gain?echogate=).
 * Returns true when this frame must go up as silence. */
bool jr_echo_gate_silence(jr_echo_gate_t *g, uint32_t now_ms, bool audible,
                          bool enabled);

/* ---- cutting a reply the server is still sending ------------------------ */

/* The transport has no message that ends the model's turn without producing
 * a new one (docs/reference/gemini-live-api-v5.md: audioStreamEnd is only a
 * stream marker; activityStart/End need manual VAD; a text turn starts a new
 * reply). So a tap that stops playback cannot silence the server; the device
 * discards the rest of that reply's audio until the server's own boundary
 * (interrupted, generationComplete, turnComplete, a new setup), or until no
 * chunk has arrived for JR_REPLY_CUT_GAP_MS (the boundary was lost: never
 * swallow a later reply because of it). */
#define JR_REPLY_CUT_GAP_MS  3000u

typedef struct {
    volatile bool     cutting;
    volatile uint32_t last_chunk_ms;
    volatile uint32_t dropped_chunks;   /* lifetime, cockpit */
    volatile uint32_t cuts;             /* lifetime, cockpit */
} jr_reply_cut_t;

void jr_reply_cut_init(jr_reply_cut_t *c);

/* A physical tap stopped playback during Speaking. */
void jr_reply_cut_begin(jr_reply_cut_t *c, uint32_t now_ms);

/* A server audio chunk arrived. True: discard it (do not stage it, do not
 * tell the orchestrator the model is speaking). */
bool jr_reply_cut_drop_audio(jr_reply_cut_t *c, uint32_t now_ms);

/* The server closed or abandoned the turn: the cut is over. */
void jr_reply_cut_boundary(jr_reply_cut_t *c);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_ECHO_GATE_H */
