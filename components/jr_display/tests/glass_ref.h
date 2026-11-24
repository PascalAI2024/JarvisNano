/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * TEST ONLY. The pre-optimisation shape primitives (glass_ref.c), same
 * signatures as glass.h under a glassref_ prefix.
 */
#ifndef GLASS_REF_H
#define GLASS_REF_H

#include "jr_display/glass.h"

void glassref_disc_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                       int32_t cx16, int32_t cy16, int32_t r16,
                       uint16_t color565, uint8_t alpha, bool swap_bytes);
void glassref_ring_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                       int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                       int32_t start_q16, int32_t sweep_q16,
                       uint16_t color565, uint8_t alpha, bool swap_bytes);
void glassref_arc_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                      int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                      int32_t start_q16, int32_t sweep_q16,
                      uint16_t color565, uint8_t alpha, bool swap_bytes);
void glassref_glow_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                       int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                       int32_t start_q16, int32_t sweep_q16, bool round_caps,
                       int32_t width16, uint8_t peak,
                       uint16_t color565, uint8_t alpha, bool swap_bytes);

#endif
