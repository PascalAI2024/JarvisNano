/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reactive "Siri-style" waveform face — baked-EAF implementation.
 *
 * This header re-declares the SAME public API that emote.h already exposes
 * (emote_face_init / _set_state / _set_synthetic_amplitude / _set_amplitude_source),
 * so reactive_face.c is a drop-in replacement for the old waveform.c with zero
 * changes to emote.c or app_claw_face_bridge.c. It is provided as a separate
 * header only for documentation; emote.h remains the canonical declaration.
 *
 * Unlike waveform.c (which drew a runtime RGB565A8 buffer and spiraled on this
 * panel), this implementation uses ONLY the engine's baked gfx_anim widget:
 * four pre-baked looping EAF animations (one per state), with the live audio
 * amplitude selecting the looping segment END for the reactive states. There is
 * NO runtime pixel drawing. See docs/REACTIVE_FACE_PLAN.md.
 */
#pragma once

#include "emote.h"   /* emote_face_state_t + the public API are declared there */
