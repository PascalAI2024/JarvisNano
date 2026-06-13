/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t emote_start(void);
esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid);

/* Set a short status detail (e.g. the active LLM model name) shown on the
 * idle screen as "Ready * <detail>" when the station is connected. */
esp_err_t emote_set_status_detail(const char *detail);

/* ---- Persistent alert overlay --------------------------------------------- *
 * A "something is wrong" screen the user can't miss: the dark-visor offline
 * face plus a short (<=24 char) strip line, latched so a later status write
 * can't quietly clobber it. Use it for boot-time hardware faults (e.g. touch
 * controller failed to init) that would otherwise look like the calm idle face.
 *
 * Pure state writes, safe to call from any caller task (same contract as
 * emote_set_network_status — it refreshes only when emote owns the display).
 * Idempotent. emote_clear_alert() restores the normal idle/status screen. */
typedef enum {
    EMOTE_ALERT_GENERIC = 0,
    EMOTE_ALERT_TOUCH,    /* touch controller failed to initialise */
} emote_alert_t;

esp_err_t emote_set_alert(emote_alert_t kind, const char *line);
esp_err_t emote_clear_alert(void);

/* Voice state helpers — called by cap_gemini_live when session state changes.
 * One per GL_STATE_* so every transition repaints the face. */
esp_err_t emote_set_connecting(void);
esp_err_t emote_set_listening(void);
esp_err_t emote_set_thinking(void);
esp_err_t emote_set_speaking(void);
esp_err_t emote_set_voice_idle(void);

/* ---- Reactive waveform face (the primary baked-EAF voice visual) ----------- *
 * A full-panel gfx_anim object replaces the static emote eye while active.
 * The firmware never draws pixels at runtime; it switches among pre-baked
 * rwave_*.eaf clips and maps mic/output RMS to stable animation segments.
 * Idle is warm amber, listening cyan, thinking violet, and speaking amber. */
typedef enum {
    EMOTE_FACE_OFF = 0,   /* hide the waveform, restore the emote eye/idle */
    EMOTE_FACE_IDLE,      /* calm breathing centre line */
    EMOTE_FACE_LISTENING, /* bars react to mic amplitude */
    EMOTE_FACE_THINKING,  /* traveling scan pulse (no audio) */
    EMOTE_FACE_SPEAKING,  /* bars react to output amplitude */
} emote_face_state_t;

/* Switch the waveform face state. Idempotent; safe before emote_start (the
 * state is applied once the engine is up). */
esp_err_t emote_face_set_state(emote_face_state_t state);

/* Feed a synthetic amplitude (0..1000) for LISTENING/SPEAKING when no live
 * audio RMS is driving the face — used by the `face` CLI demo so the visual can
 * be validated on the panel without a Gemini session. <0 restores the live
 * audio source (the registered amplitude callback). */
void emote_face_set_synthetic_amplitude(int amp_milli);

/* Register the live audio-amplitude source (voice-eng's mic/out RMS getters).
 * cb(state) returns 0..1000 for the given active state; NULL = synthetic only. */
typedef uint16_t (*emote_face_amp_cb_t)(emote_face_state_t state);
void emote_face_set_amplitude_source(emote_face_amp_cb_t cb);

typedef struct {
    int state;
    int applied_state;
    int synth_amp;
    float displayed_amp;
    int last_start;
    int last_end;
    uint32_t driver_ticks;
    uint32_t segment_sets;
    uint32_t keepalive_kicks;
    uint32_t state_changes;
    bool running;
    bool object_ready;
    bool display_owned;
    bool current_clip_loaded;
} emote_face_debug_t;

esp_err_t emote_face_get_debug(emote_face_debug_t *out);

/* Debug-only display snapshot mirror. The emote flush path records the exact
 * RGB565 pixels last sent to the panel so HTTP diagnostics can export a screen
 * capture without relying on panel readback.
 *
 * The mirror is consumer-gated (STABILITY_PLAN P2.6): per-strip copies only run
 * while a consumer has been seen within the last ~10 s. Both getters below mark
 * the consumer as active (re-arming the mirror), so the first request after an
 * idle period may return one stale frame — the next flush refreshes it. With no
 * consumer the flush path is a single flag check: no mutex take, no memcpy. */
typedef struct {
    uint16_t width;
    uint16_t height;
    size_t bytes;
    uint64_t frame_id;
    uint64_t last_flush_ms;
    bool valid;
} emote_display_snapshot_info_t;

esp_err_t emote_display_snapshot_get_info(emote_display_snapshot_info_t *out);
esp_err_t emote_display_snapshot_copy_rgb565(void *dst,
                                             size_t dst_size,
                                             emote_display_snapshot_info_t *out);

/* Mark a snapshot consumer as active, (re-)arming the mirror for ~10 s. Called
 * internally by both getters above; exposed for any consumer path that wants to
 * pre-arm the mirror without reading it (e.g. an HTTP layer outside this
 * component). Safe from any task; never blocks. */
void emote_display_snapshot_notify_consumer(void);

#ifdef __cplusplus
}
#endif
