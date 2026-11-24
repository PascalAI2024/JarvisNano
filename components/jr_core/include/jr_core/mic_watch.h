/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/mic_watch.h - notices a microphone that is open but electrically
 * dead, and says when to reopen the ADC.
 *
 * WHY. On 2026-11-10 a failed I2C read during a gain change left the ES7210
 * channel with its PGA disabled: the raw microphone lane read rms 0.7 (a live
 * room reads 8 to 28) with the ADC open, and stayed that way until the codec
 * was closed and reopened (docs/reference/voice-loop-2026-11-10.md, N16.10).
 * The driver is patched so that cannot happen again; this is the net under it
 * for any other way the lane can die.
 *
 * WHAT IT MAY DO. It only ever asks for a reopen of something that is already
 * meant to be open. It is not judged, and never fires, while the microphone
 * is muted, the device sleeps, or the ADC is closed: a muted device sends no
 * audio, and nothing here can clear privacy or open a closed microphone.
 *
 * Pure: no clock, no I/O. The caller passes now_ms and the raw-lane RMS (the
 * microphone lane straight off the ADC, before AEC and before any make-up
 * gain).
 */
#ifndef JR_CORE_MIC_WATCH_H
#define JR_CORE_MIC_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Raw-lane RMS under this is electrical silence: the dead lane read 0.7, the
 * idle reference lane reads 0.7, the quietest live room read 8. */
#define JR_MIC_DEAD_RMS          3.0f
/* How long the lane must stay under it, with the microphone meant to be open. */
#define JR_MIC_DEAD_MS           4000u
/* Minimum gap between reopens; doubles on each reopen that did not bring a
 * live frame back, capped, and resets on the first live frame. */
#define JR_MIC_BACKOFF_MIN_MS    10000u
#define JR_MIC_BACKOFF_MAX_MS    60000u

typedef struct {
    bool     quiet;           /* the lane is under the threshold ... */
    uint32_t quiet_since_ms;  /* ... since this time */
    bool     dead;            /* published: dead until a live frame is seen */
    bool     have_next;
    uint32_t next_ok_ms;      /* no reopen before this time */
    uint32_t backoff_ms;
    uint32_t recoveries;      /* reopens requested, lifetime */
} jr_mic_watch_t;

void jr_mic_watch_init(jr_mic_watch_t *w);

/* Is the microphone "meant to be open" for the watch? Enabled, awake, and NOT
 * while the speaker is the source: with the PGA turned down to 6 dB for the
 * canceller (docs/reference/echo-canceller-2026-11-11.md) a quiet moment in a
 * reply reads under JR_MIC_DEAD_RMS on a healthy lane, so a lane is judged
 * only while the device is listening. */
bool jr_mic_watch_meant_open(bool mic_enabled, bool sleeping, bool speaking);

/* One captured frame. `adc_open`: the ADC is open. `meant_open`: the
 * microphone is enabled and the device is not sleeping. Returns true when the
 * caller should close and reopen the ADC now. */
bool jr_mic_watch_frame(jr_mic_watch_t *w, uint32_t now_ms, float raw_rms,
                        bool adc_open, bool meant_open);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_MIC_WATCH_H */
