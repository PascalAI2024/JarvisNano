/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * showpiece_gfx.h — Wave 4 showpiece procedural OLED renderers (pure C11).
 *
 * Three strip-invariant, fixed-point, zero-allocation, zero-libm renderers:
 *   1. Real-Time 3D Rotating Geodesic Hologram Engine (shared by REACTOR + JARVIS)
 *   2. WATCH:ORRERY (real J2000 astronomical planetarium + live synodic Moon phase)
 *   3. WATCH:REACTOR (32-bar radial audio spectrum + 3D holo core + HH:MM + pip)
 */
#ifndef JR_DISPLAY_SHOWPIECE_GFX_H
#define JR_DISPLAY_SHOWPIECE_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "jr_display/hud_render.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JR_ORRERY_PLANETS 6

typedef enum {
    JR_HOLO_IDLE = 0,
    JR_HOLO_LISTEN,
    JR_HOLO_THINK,
    JR_HOLO_SPEAK,
    JR_HOLO_MUTED,
    JR_HOLO_REACTOR,
} jr_holo_mode_t;

typedef struct {
    uint32_t planet_q16[JR_ORRERY_PLANETS]; /* heliocentric screen angle, Q16 turns */
    uint32_t moon_phase_q16;                /* synodic moon phase 0..65535 (0=new, 32768=full) */
    uint32_t earth_moon_q16;                /* Moon orbital angle around Earth, Q16 turns */
} jr_orrery_eph_t;

/* Pure integer J2000 heliocentric planetary longitudes & synodic moon phase.
 * Falls back to 2026-11-23 when year < 1970 or month/day out of range. */
void jr_orrery_ephemeris(uint16_t year, uint8_t month, uint8_t day,
                         int hh, int mm, int ss, jr_orrery_eph_t *out);

/* Real-time 3D rotating geodesic icosahedron + counter-rotating inner octahedron.
 * Strictly clipped to circle r <= max_r about (232.5, 232.5).
 * Pure fixed-point, strip-invariant across arbitrary [y0, y0+nrows). */
void jr_showpiece_holo_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                            uint32_t now_ms, int max_r, jr_holo_mode_t mode,
                            int tilt_x, int tilt_y,
                            const uint8_t bands[16], uint8_t amp, int strength);

/* WATCH:ORRERY live planetarium layer (sun corona, 6 planets on measured golden
 * tracks with 28-deg trails & IMU parallax, Earth's Moon, Saturn's 3D ring, and
 * 6 o'clock synodic moon phase disc). Drawn over dial_orrery.eaf, under hands. */
void jr_showpiece_orrery_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                              uint32_t now_ms, uint16_t year, uint8_t month, uint8_t day,
                              int hh, int mm, int ss,
                              int tilt_x, int tilt_y,
                              const hud_light_t *light, bool alive, int strength);

/* WATCH:REACTOR live core inside dial_reactor.eaf's recessed chamber (r <= 122):
 * 32-bar radial electric-cyan spectrum (or harmonic breathing wave when idle),
 * central 3D geodesic hologram (r <= 72), crisp HH:MM readout, and r=120 seconds pip. */
void jr_showpiece_reactor_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                               uint32_t now_ms, int hh, int mm, int ss, int ms,
                               bool still, bool alive, int tilt_x, int tilt_y,
                               const uint8_t bands[16], uint8_t amp, int strength);

#ifdef __cplusplus
}
#endif

#endif /* JR_DISPLAY_SHOWPIECE_GFX_H */
