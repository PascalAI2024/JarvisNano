/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Glue between cap_gemini_live's audio level getters and the emote reactive
 * waveform face. Lives here (app_claw layer) — the ONLY place that knows about
 * both APIs — so cap_gemini_live stays display-agnostic and the emote component
 * stays voice-agnostic. Registered from app_claw_ui_start() after emote_start().
 *
 * Patch 0032. Depends only on:
 *   - emote.h:           emote_face_set_amplitude_source / emote_face_state_t
 *   - cap_gemini_live.h: cap_gemini_live_get_mic_level / _get_output_level (float 0..1)
 */

#include "app_claw_face_bridge.h"

#if CONFIG_APP_CLAW_ENABLE_EMOTE && CONFIG_APP_CLAW_CAP_GEMINI_LIVE

#include <stdint.h>
#include "emote.h"
#include "cap_gemini_live.h"

/* The getters return RMS/32768 (0.0..1.0). With ES7210 in-gain=30, typical
 * speech mic RMS sits ~2000-6000 raw → ~0.06..0.18 float, so we scale mic up so
 * normal talking fills a useful range. Gemini TTS output reads much hotter, so
 * it needs a gentler scale. Per-stream gain + clamp; tune after hardware test.
 * (The render task's lerp provides the fast-attack/slow-decay smoothing.) */
#define FACE_MIC_GAIN  6.0f   /* 0.17 speech → ~1.0 */
#define FACE_OUT_GAIN  1.6f   /* TTS already hot → modest lift */

static uint16_t gl_face_amp(emote_face_state_t state)
{
    float lvl, gain;
    if (state == EMOTE_FACE_LISTENING) {
        lvl = cap_gemini_live_get_mic_level();
        gain = FACE_MIC_GAIN;
    } else if (state == EMOTE_FACE_SPEAKING) {
        lvl = cap_gemini_live_get_output_level();
        gain = FACE_OUT_GAIN;
    } else {
        return 0;   /* IDLE/CONNECTING/THINKING → flat (synthetic motion only) */
    }
    float milli = lvl * gain * 1000.0f;
    if (milli < 0.0f) milli = 0.0f;
    if (milli > 1000.0f) milli = 1000.0f;
    return (uint16_t)milli;
}

void app_claw_face_bridge_register(void)
{
    emote_face_set_amplitude_source(gl_face_amp);
}

#else  /* emote or gemini-live disabled — no-op so callers need no guards */

void app_claw_face_bridge_register(void) {}

#endif
