/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_wake — L1 wake-word adapter (esp-sr WakeNet9, standalone iface).
 *
 * Wraps the bare esp_wn_iface (NOT the AFE framework — same decision as the
 * AEC path, docs/reference/aec-barge-in.md: the AFE pipeline costs 60-80 KB
 * internal RAM, the bare engine ~16-20 KB, and v5 already produces the
 * AEC-clean 16 kHz mono stream the engine wants).
 *
 * Ownership: jr_wake never touches the codec. The composition root hands it
 * frames from the SAME single-owner capture seam the uplink uses. Detection
 * runs on its own worker task (`jr_wake`, core 0), never in the voice task:
 * the voice task owes the canceller a frame every 32 ms, and WakeNet9 plus
 * aec_process did not fit in that budget together. The voice task only copies
 * a frame into a bounded ring (jr_wake_push) and later asks whether a hit has
 * arrived (jr_wake_take_hit); both are non-blocking.
 *
 * Privacy contract: the caller pushes only frames that are live today, and
 * calls jr_wake_flush() on every privacy edge. A flush empties the ring and
 * discards any hit, including one whose chunk was already inside detect(), so
 * audio captured before a mute is never analysed after it and a hit cannot
 * arrive while muted. A hit never clears privacy; that stays the caller's
 * business.
 *
 * Degradation: no `model` partition, no selected model, or alloc failure all
 * leave jr_wake_ready() false and every call a no-op. Wake is a delight
 * feature — it must never block boot or the voice path.
 */
#ifndef JR_WAKE_JR_WAKE_H
#define JR_WAKE_JR_WAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Map the `model` partition, pick the first WakeNet model in it, create the
 * detector and start the worker. ESP_ERR_NOT_FOUND when the partition or model
 * is absent (WARN and carry on — see degradation note above). Call once,
 * after NVS/flash init; needs no codec, display, or network. */
esp_err_t jr_wake_init(void);

/* True when a detector is live and the worker runs. */
bool jr_wake_ready(void);

/* Loaded model name ("wn9_jarvis_tts"), or "" before successful init. */
const char *jr_wake_model(void);

/* Copy 16 kHz mono AEC-clean samples into the worker's ring. Never blocks.
 * Returns false (and counts a drop) when the ring has no room for all n
 * samples; nothing is stored then. Single caller only (the voice task). */
bool jr_wake_push(const int16_t *samples, size_t n);

/* Non-zero when a detection newer than max_age_ms has not been taken yet; the
 * detection is marked taken. A stale one is discarded, never delivered late.
 * now_ms is the same millisecond clock as esp_timer / 1000. Voice task only. */
uint32_t jr_wake_take_hit(uint32_t now_ms, uint32_t max_age_ms);

/* Empty the ring and discard any untaken (or in-flight) hit. Voice task only;
 * cheap enough to call on every loop while the microphone is gated. */
void jr_wake_flush(void);

/* Diagnostics for /api surfaces. */
uint32_t jr_wake_detections(void);
uint32_t jr_wake_last_detect_ms(void);
uint32_t jr_wake_dropped(void);            /* pushes refused: ring full */
uint32_t jr_wake_stale_hits(void);         /* hits discarded for age */
uint32_t jr_wake_chunks(void);             /* chunks run through detect() */
uint32_t jr_wake_detect_last_us(void);
uint32_t jr_wake_detect_max_us(void);
uint32_t jr_wake_stack_free(void);         /* worker stack high-water, bytes */

/* Bench: ask the worker to run 64 chunks of pseudo-random noise through
 * detect() and record the time per chunk. No microphone, no session; a hit
 * during the bench is discarded. Callable from any task. */
void jr_wake_bench_request(void);
bool jr_wake_bench_pending(void);
/* False until a bench has finished once. */
bool jr_wake_bench_result(uint32_t *mean_us, uint32_t *max_us);

#ifdef __cplusplus
}
#endif

#endif /* JR_WAKE_JR_WAKE_H */
