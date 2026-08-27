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
 * Ownership: jr_wake never touches the codec. The composition root feeds it
 * frames from the SAME single-owner capture seam the uplink uses — the voice
 * task pulls frames even while the session is disarmed and hands them here
 * instead of the transport. Detection is synchronous inside feed (~3-4 ms per
 * 32 ms frame on one core, measured envelope from the esp-sr benchmark table)
 * — acceptable because feeding only happens while the session is idle.
 *
 * Degradation: no `model` partition, no selected model, or alloc failure all
 * leave jr_wake_ready() false and feed a no-op. Wake is a delight feature —
 * it must never block boot or the voice path.
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
 * detector. ESP_ERR_NOT_FOUND when the partition or model is absent (WARN and
 * carry on — see degradation note above). Call once, after NVS/flash init;
 * needs no codec, display, or network. */
esp_err_t jr_wake_init(void);

/* True when a detector is live and jr_wake_feed() is meaningful. */
bool jr_wake_ready(void);

/* Loaded model name ("wn9_jarvis_tts"), or "" before successful init. */
const char *jr_wake_model(void);

/* Feed 16 kHz mono AEC-clean samples; returns true exactly once per wake-word
 * hit. Internally re-chunks to the model's native chunk size, so any frame
 * length is fine. Single caller only (the voice task) — not thread-safe. */
bool jr_wake_feed(const int16_t *samples, size_t n);

/* Diagnostics for /api surfaces. */
uint32_t jr_wake_detections(void);
uint32_t jr_wake_last_detect_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* JR_WAKE_JR_WAKE_H */
