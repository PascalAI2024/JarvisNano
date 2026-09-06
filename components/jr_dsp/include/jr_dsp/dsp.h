/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_dsp/dsp.h — pure DSP math library (part of the inner core).
 *
 * Zero hardware, zero IDF. Pure functions of numbers so the §8 on-hardware
 * tuning ordeal (11+ builds) becomes a host regression suite fed by recorded
 * WAV fixtures (architecture.md §Testability §4). Phase 0 = signatures +
 * trivial-but-correct bodies (rms) and honest stubs (adaptive VAD, resampler);
 * the real adaptive noise-floor tracker and linear resampler land in Phase 1.
 */
#ifndef JR_DSP_DSP_H
#define JR_DSP_DSP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Root-mean-square of a signed-16-bit PCM buffer. Returns 0 for an empty
 * buffer. Range 0..32768. The primitive every VAD/barge feature derives from. */
float jr_dsp_rms(const int16_t *samples, size_t n);

/* NOTE: the Phase-0 jr_vad_* stub and jr_dsp_resample_linear stub were removed
 * on 2026-07-18. Both were superseded and had ZERO production callers — their
 * only callers were host tests asserting stub behaviour, which is a false
 * signal, not coverage. The real implementations live where the data does:
 *   - VAD: adaptive floor tracking in main.c (see /api/diag/vadlog); the L4
 *     strategy is selected via jr_vad_mode_t in jr_core/session.h.
 *   - 24k->16k rate conversion: downsample_24to16_4lane() in jr_audio.c, which
 *     must preserve the 4-lane TDM interleave — a generic resampler cannot.
 * Re-add a pure primitive here only when something pure actually needs it. */

/* ---- playback jitter stepper: the feeder's reply / hole / pre-roll walk ----
 *
 * The playback feeder in jr_audio.c infers reply boundaries from ring
 * occupancy alone — nothing tells it where one Gemini reply ends and the next
 * begins. This is that inference, lifted out so it can be walked on the host:
 *
 *   data in the ring         a reply is open;
 *   dry under an audible     a GAP opens (the speaker just went quiet with
 *   DAC tail                 more expected);
 *   data back within         a HOLE — the network starved the DAC; the reply
 *   JR_JITTER_REPLY_GAP_MS   was heard to stutter;
 *   silence outlasting it    the reply ENDED; the pre-roll walks.
 *
 * ADAPTIVE PRE-ROLL. 600 ms is a second of latency won back; 1500 ms is what
 * the worst measured server stall (2.2 s) needs to stay seamless. Any reply
 * with a hole steps the lead up 300; three clean replies in a row step it
 * back 200. A pin (jr_jitter_pin with preroll > 0) stops the walk; 0
 * restarts it at the floor.
 *
 * EARCONS are the reason this is a separate stepper. A UI tone (the mute
 * sweep, a chime) rides the same ring, and the feeder used to read it as a
 * reply: measured 2026-09-05, one privacy long-press booked four 9 ms
 * "holes mid-reply" against a 100 ms sweep and stepped the pre-roll 600 ->
 * 900 — 300 ms added to the first word of every real reply that followed,
 * with no Gemini reply ever played. While earcon samples are outstanding, and
 * for JR_JITTER_TAIL_MS after the last one drained (the DAC tail of the tone
 * itself), the stepper sees nothing: no reply opens, no hole books, no reply
 * ends, the walk does not move. */
#define JR_JITTER_PREROLL_FLOOR_MS   600u
#define JR_JITTER_PREROLL_CEIL_MS   1500u
#define JR_JITTER_STEP_UP_MS         300u
#define JR_JITTER_STEP_DOWN_MS       200u
#define JR_JITTER_CLEAN_TO_STEP        3u
#define JR_JITTER_REPLY_GAP_MS      2500u   /* silence that ends a reply */
#define JR_JITTER_TAIL_MS             80u   /* audible DAC tail after a write */

typedef struct {
    uint32_t preroll_ms;
    bool     adaptive;          /* false while pinned by hand */
    bool     reply_open;
    bool     in_gap;
    uint32_t gap_start_ms;
    bool     empty;             /* ring has been dry since empty_since_ms */
    uint32_t empty_since_ms;
    uint32_t reply_holes;       /* holes in the reply now open */
    uint32_t clean_replies;     /* consecutive replies without one */
    bool     earcon_seen;       /* outstanding on the previous feed */
    uint32_t earcon_tail_until_ms;
} jr_jitter_t;

typedef struct {
    bool     quiet;           /* an earcon owns the ring: nothing was judged */
    bool     gap_opened;      /* dry under the tail: rebuild the REFILL lead */
    bool     hole_booked;     /* data back inside the reply window           */
    uint32_t hole_ms;         /* its length, to arrival                      */
    bool     gap_ended;       /* silence outlasted the window (a reply ended)*/
    bool     reply_ended;     /* the walk ran — once per reply               */
    bool     idle;            /* dry long enough: re-arm the PREROLL lead    */
    bool     preroll_changed;
    uint32_t preroll_prev_ms;
    uint32_t preroll_ms;      /* current lead, every call                    */
    uint32_t reply_holes;     /* holes in the reply that just ended          */
} jr_jitter_out_t;

/* Start the walk at preroll_ms (adaptive). */
void jr_jitter_init(jr_jitter_t *j, uint32_t preroll_ms);

/* The live knob: preroll_ms > 0 pins the lead there and stops the walk;
 * 0 resets to the floor and restarts it. Values above 3000 clamp. */
void jr_jitter_pin(jr_jitter_t *j, uint32_t preroll_ms);

/* One feeder iteration. avail_samples is the ring level seen this pass,
 * tail_active whether the last DAC write is still audible, earcon_outstanding
 * whether tone samples are still in the ring (or in the chunk being written).
 * Call it every pass, dry or not. */
void jr_jitter_feed(jr_jitter_t *j, uint32_t now_ms, uint32_t avail_samples,
                    bool tail_active, bool earcon_outstanding,
                    jr_jitter_out_t *out);

#ifdef __cplusplus
}
#endif

#endif /* JR_DSP_DSP_H */
