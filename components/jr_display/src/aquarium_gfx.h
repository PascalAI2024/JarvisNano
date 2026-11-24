/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * aquarium_gfx.h — 466x466 Procedural OLED "Nano Reef" Virtual Aquarium
 * Renderer & Fixed-Point Physics Engine (pure C11, zero heap, zero libm).
 *
 * 100% strip-invariant across arbitrary [y0, y0 + nrows) slices, strictly
 * clipped inside r <= JR_AQ_R_CLIP (213 < JR_DISPLAY_SHELL_R_MAX 214) so the
 * outer privacy ring and battery rim stay completely untouched.
 */
#ifndef JR_DISPLAY_AQUARIUM_GFX_H
#define JR_DISPLAY_AQUARIUM_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "jr_display/jr_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JR_AQ_API
#define JR_AQ_API
#endif

#define JR_AQ_W            466
#define JR_AQ_H            466
#define JR_AQ_CX           233
#define JR_AQ_CY           233
#define JR_AQ_R_CLIP       213  /* strictly inside JR_DISPLAY_SHELL_R_MAX (214) */
#define JR_AQ_MAX_FISH       8
#define JR_AQ_MAX_PELLETS   12
#define JR_AQ_MAX_RIPPLES    4
#define JR_AQ_MAX_BUBBLES   32

/* Advance simulation by one frame (call ONCE per frame at y1 == 0).
 * tilt_x, tilt_y are shaped IMU lean in [-256..+256] (JR_TILT_FULL). */
JR_AQ_API void jr_aquarium_step(uint32_t now_ms, int tilt_x, int tilt_y,
                                const jr_display_aq_state_t *state);

/* Drop 3 golden sinking food pellets and spawn an expanding water ripple ring
 * at (x, y). Clamped smoothly inside the water bowl. */
JR_AQ_API void jr_aquarium_tap(uint16_t x, uint16_t y);

/* Spawn +14 burst air bubbles and send fish into a 1.2s playful dart swirl. */
JR_AQ_API void jr_aquarium_shake(void);

/* Render rows [y0, y0 + nrows) onto dst (466 px wide RGB565 strip), honoring
 * swap_bytes. Strictly clipped to r <= JR_AQ_R_CLIP; 100% strip-invariant. */
JR_AQ_API void jr_aquarium_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes);

/* Active 2.2s biome change toast label ("TROPICAL REEF", "DEEP ABYSS",
 * "ZEN KOI POND") and alpha 0..255, or NULL when expired/hidden. */
JR_AQ_API const char *jr_aquarium_biome_toast(uint32_t now_ms, uint8_t *out_alpha);

#ifdef __cplusplus
}
#endif

#endif /* JR_DISPLAY_AQUARIUM_GFX_H */
