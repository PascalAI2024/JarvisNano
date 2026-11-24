/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass3_ring.h — JarvisNano Glass 3.0 Procedural OLED Ring & Voice-Wave Engine.
 *
 * Pure C11, integer-only, zero-libm, zero-heap, 100% strip-invariant across
 * arbitrary [y0, y0+nrows) slices on the 466x466 round RGB565 AMOLED panel.
 *
 * Three cohesive procedural OLED instruments:
 *   1. CHRONO-ARC  (jr_g3_chrono_arc_rows)  — Home perimeter micrometer bezel,
 *      4 sculpted luminous telemetry capsules, and whisper-soft sonar ripples.
 *   2. AETHERIUM   (jr_g3_aetherium_rows)   — Live voice harmonic ring (120 FFT
 *      filaments, 3 iridescent additive wave ribbons, outer millimeter ticks +
 *      4 cardinal jewels) & lower-center Crystalline Vocal Waveform Ribbon.
 *   3. BEZEL SCRUB (jr_g3_bezel_scrub_rows) — Luminous rotary dial arc, jewel
 *      indicator, and precision tick scale for rim-drag adjustments.
 *
 * Strict Radial & Text Safe-Zone Contract (about center (232.5, 232.5)):
 *   - CHRONO-ARC:
 *       * Sonar ripples:   r in [106, 168], alpha strictly <= 24/255
 *       * Bezel & arcs:    r in [186, 231]
 *       * Unobstructed:    r < 106, r in (168, 186), r > 231 (0 px touched)
 *   - AETHERIUM:
 *       * Outer ring:      r in [160, 231] (filaments 164..212, waves 178..208,
 *                          outer ticks 218..228, cardinal dots r=223)
 *       * Vocal ribbon:    x in [96, 370], y in [286, 334]
 *       * Center clean:    y in [165, 275] inside r < 160 (0 px touched)
 *   - BEZEL SCRUB:
 *       * Dial & jewel:    r in [204, 231] (ticks 206..211, arc 208..228)
 *       * Unobstructed:    r < 204, r > 231 (0 px touched)
 */
#ifndef JR_DISPLAY_GLASS3_RING_H
#define JR_DISPLAY_GLASS3_RING_H

#include <stdbool.h>
#include <stdint.h>

#include "jr_display/hud_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Safe-zone geometry constants (px about (232.5, 232.5)) -------------- */

#define JR_G3_PANEL_PX            HUD_W /* 466 */

#define JR_G3_CHRONO_RIPPLE_R_MIN 106
#define JR_G3_CHRONO_RIPPLE_R_MAX 168
#define JR_G3_CHRONO_BEZEL_R_MIN  186
#define JR_G3_CHRONO_BEZEL_R_MAX  231

#define JR_G3_AETH_RING_R_MIN     160
#define JR_G3_AETH_RING_R_MAX     231
#define JR_G3_AETH_RIBBON_X0       96
#define JR_G3_AETH_RIBBON_X1      370
#define JR_G3_AETH_RIBBON_Y0      286
#define JR_G3_AETH_RIBBON_Y1      334

#define JR_G3_SCRUB_R_MIN         204
#define JR_G3_SCRUB_R_MAX         231

/* ---- Telemetry & mode descriptors ---------------------------------------- */

/* Plain-data telemetry snapshot passed from jr_display.c into CHRONO-ARC. */
typedef struct {
    uint8_t batt_pct;     /* 0..100, or 0xFF when battery level is unknown    */
    bool    charging;     /* true when external power is charging the cell    */
    uint8_t wifi_bars;    /* 0..4 Wi-Fi link strength bars (clamped > 4)      */
    uint8_t amp;          /* 0..255 live mic RMS / acoustic level             */
    bool    muted;        /* true when privacy-muted                          */
    bool    codex_active; /* true when Codex Desk / operator mode is active   */
    bool    voice_busy;   /* true when a live voice interaction is active     */
} jr_g3_chrono_env_t;

/* Voice state palette & dynamics selector for AETHERIUM. */
typedef enum {
    JR_G3_AETH_LISTEN = 0, /* Electric turquoise-cyan breathing harmonics     */
    JR_G3_AETH_THINK  = 1, /* Iridescent cobalt-violet orbital harmonics      */
    JR_G3_AETH_SPEAK  = 2, /* High-energy cyan + magenta-violet + gold peaks  */
} jr_g3_aeth_mode_t;

/* Semantic accent palette for the Rotary Bezel Scrub overlay. */
typedef enum {
    JR_G3_SCRUB_VOLUME     = 0, /* Warm electric amber-gold                   */
    JR_G3_SCRUB_BRIGHTNESS = 1, /* Crisp ice-cyan / solar-white               */
    JR_G3_SCRUB_TIMER      = 2, /* Electric violet-cyan                       */
} jr_g3_scrub_kind_t;

/* ---- Strip-invariant renderers ------------------------------------------- */

/* Option 1: CHRONO-ARC — Home perimeter micrometer tick bezel (180 ticks,
 * 12 sapphire-cyan hour ticks, 4 bright vernier ticks at 12/3/6/9), 4 sculpted
 * luminous curved status capsules (Top AI/Mode, Right Battery, Bottom-Left
 * Acoustic/Mic, Top-Left Wi-Fi), and whisper-soft concentric sonar ripples.
 *
 * `dst` holds rows [y0, y0+nrows) of a 466-px wide RGB565 frame.
 * `env` may be NULL (treated as zero/unknown defaults).
 * `strength` in 0..255 (<= 0 paints nothing). */
void jr_g3_chrono_arc_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                           uint32_t now_ms, const jr_g3_chrono_env_t *env,
                           int strength);

/* Option 2: AETHERIUM — Live voice harmonic ring (outer millimeter tick ring
 * with 4 glowing cyan cardinal jewels, 120 radial FFT spectrum filaments,
 * 3 phase-shifted iridescent additive harmonic wave ribbons) and lower-center
 * Crystalline Vocal Waveform Ribbon strictly in x=[96,370], y=[286,334].
 *
 * `bands` may be NULL (treated as all zeros; organic breathing floor runs).
 * `amp` is 0..255 overall audio amplitude.
 * `strength` in 0..255 (<= 0 paints nothing). */
void jr_g3_aetherium_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                          uint32_t now_ms, jr_g3_aeth_mode_t mode,
                          const uint8_t bands[16], uint8_t amp,
                          int strength);

/* Rotary Bezel Scrub Overlay — Precision 270-deg luminous rotary dial arc
 * (7:30 -> 12:00 -> 4:30), 41-tick inner scale, and glowing faceted jewel
 * indicator at `value_pct` (0..100, clamped).
 *
 * `strength` in 0..255 (<= 0 paints nothing). */
void jr_g3_bezel_scrub_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                            uint32_t now_ms, uint8_t value_pct,
                            jr_g3_scrub_kind_t kind, int strength);

#ifdef __cplusplus
}
#endif

#endif /* JR_DISPLAY_GLASS3_RING_H */
