/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the live audio-amplitude source (cap_gemini_live mic/out levels) with
 * the emote reactive waveform face. Call once after emote_start(). No-op when
 * the emote face or gemini-live capability is disabled. */
void app_claw_face_bridge_register(void);

#ifdef __cplusplus
}
#endif
