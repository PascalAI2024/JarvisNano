/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gemini Live voice capability — toggle-on/off voice conversation via
 * Gemini Live API (BidiGenerateContent WebSocket).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_gemini_live_set_api_key(const char *api_key);
esp_err_t cap_gemini_live_set_mcp_key(const char *mcp_key);
esp_err_t cap_gemini_live_set_mcp_url(const char *mcp_url);
esp_err_t cap_gemini_live_register_group(void);
esp_err_t cap_gemini_live_start(void);
esp_err_t cap_gemini_live_stop(void);
esp_err_t cap_gemini_live_send_text(const char *text);
esp_err_t cap_gemini_live_end_input(void);
esp_err_t cap_gemini_live_test(void);
bool      cap_gemini_live_is_active(void);
/* Toggle session on/off — call from emote tap callback (Phase 5) */
void      cap_gemini_live_toggle(void);

/* ---- Audio level hooks for the reactive waveform (display layer) ----------
 * Normalised 0.0..1.0. mic = ES7210 capture level (drives the LISTENING
 * waveform), output = decoded Gemini playback level (drives the SPEAKING
 * waveform). Both read 0 when their stream is idle. Lock-free (plain atomics) —
 * safe to poll from the display task. The display amplitude-source adapter
 * (patch 0032) scales these to its 0..1000 range. The synthetic setter drives
 * the waveform with no live session (own audio-path testing). */
float     cap_gemini_live_get_mic_level(void);
float     cap_gemini_live_get_output_level(void);
void      cap_gemini_live_set_synthetic_levels(float mic, float out);

/* Hands-free barge-in calibration (P3.4): RMS threshold for the local barge
 * detector on the AEC-cleaned, post-gain mic during SPEAKING. 0 disables the
 * detector (tap interrupt still works). Default GL_BARGE_RMS (2500). Safe to
 * call from any task, mid-session. */
void      cap_gemini_live_set_barge_rms(uint16_t rms);

/* Runtime toggle for server-side barge-in (manual mode): 1 =
 * START_OF_ACTIVITY_INTERRUPTS (barge stops her reply), 0 = NO_INTERRUPTION.
 * Takes effect on the next session/WS (re)connect. */
void      cap_gemini_live_set_activity_interrupts(int enable);

/* Runtime AEC gain staging for hardware calibration. Any arg < 0 is left
 * unchanged. mic_db/ref_db = ES7210 per-channel PGA; out_vol = ES8311 0-100. */
void      cap_gemini_live_set_in_gains(int mic_db, int ref_db, int out_vol);

/* During-SPEAKING mic gain (barge-in calibration). Higher = a barge stands out
 * over the residual echo; too high re-clips the echo into the AEC. <0 = leave. */
void      cap_gemini_live_set_speak_gain(int db);
void      cap_gemini_live_print_diagnostics(void);
esp_err_t cap_gemini_live_get_diagnostics_json(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
