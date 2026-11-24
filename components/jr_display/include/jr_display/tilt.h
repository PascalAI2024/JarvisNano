/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * tilt.h — the SHAPE of the display's tilt input. Pure, header-only, no IDF:
 * the render task calls jr_tilt_step() once a frame with the low-passed
 * in-plane gravity (milli-g, screen axes) and gets back what the dial should
 * lean by. docs/reference/tilt-parallax.md has the numbers' reasons.
 *
 *   raw (mg) --> centre eases to raw (tau 3 s) --> offset = raw - centre
 *            --> dead zone (a still hand gives exactly 0) --> clamp
 *            --> out = -256..+256   (256 = the clamp, whatever the unit was)
 *
 * Neutral is wherever the device is held: any steady pose drifts to zero.
 * Integer maths only, so the host result is the device result.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define JR_TILT_FULL      256     /* the output at the clamp                       */
#define JR_TILT_TAU_MS    3000    /* the re-centre time constant                   */
#define JR_TILT_DEAD_MG   12      /* offset inside this is exactly 0 (~0.7 deg)    */
#define JR_TILT_CLAMP_MG  350     /* offset beyond the dead zone that reads FULL   */
#define JR_TILT_GAP_MS    1000    /* a longer gap between steps reseeds the centre */

typedef struct {
    bool    seeded;               /* false: the next step adopts its input         */
    int32_t cx_q8, cy_q8;         /* the neutral pose, mg * 256                    */
    int     x, y;                 /* the last output, -256..+256                   */
} jr_tilt_t;

static inline void jr_tilt_reset(jr_tilt_t *t)
{
    t->seeded = false;
    t->cx_q8 = t->cy_q8 = 0;
    t->x = t->y = 0;
}

/* One axis: ease the centre toward the reading, then shape what is left. */
static inline int jr_tilt_axis(int raw_mg, int32_t *c_q8, int dt_ms)
{
    const int32_t raw_q8 = raw_mg * 256;
    *c_q8 += (int32_t)((int64_t)(raw_q8 - *c_q8) * dt_ms / (JR_TILT_TAU_MS + dt_ms));
    int32_t off = raw_q8 - *c_q8;
    const int32_t dead = JR_TILT_DEAD_MG * 256;
    if (off > dead) {
        off -= dead;
    } else if (off < -dead) {
        off += dead;
    } else {
        return 0;
    }
    int32_t out = off / JR_TILT_CLAMP_MG;      /* mg*256 / mg = 256 at the clamp */
    if (out > JR_TILT_FULL) {
        out = JR_TILT_FULL;
    } else if (out < -JR_TILT_FULL) {
        out = -JR_TILT_FULL;
    }
    return (int)out;
}

/* Once a frame. dt_ms is the time since the previous step; a gap over
 * JR_TILT_GAP_MS (the dial was not on the glass) reseeds, so coming back to it
 * starts at rest rather than at the pose it was left in. */
static inline void jr_tilt_step(jr_tilt_t *t, int x_mg, int y_mg, int dt_ms)
{
    if (!t->seeded || dt_ms < 0 || dt_ms > JR_TILT_GAP_MS) {
        t->seeded = true;
        t->cx_q8 = x_mg * 256;
        t->cy_q8 = y_mg * 256;
        t->x = t->y = 0;
        return;
    }
    t->x = jr_tilt_axis(x_mg, &t->cx_q8, dt_ms);
    t->y = jr_tilt_axis(y_mg, &t->cy_q8, dt_ms);
}

/* The diag word: x in bits 0-15, y in bits 16-31, both signed 16-bit. */
static inline uint32_t jr_tilt_pack(int x, int y)
{
    return ((uint32_t)(uint16_t)(int16_t)x) | ((uint32_t)(uint16_t)(int16_t)y << 16);
}

static inline int jr_tilt_unpack_x(uint32_t w) { return (int)(int16_t)(uint16_t)(w & 0xFFFFu); }
static inline int jr_tilt_unpack_y(uint32_t w) { return (int)(int16_t)(uint16_t)(w >> 16); }
