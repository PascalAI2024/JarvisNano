/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass3_screens.h — JarvisNano Glass 3.5 procedural OLED data-screen engines.
 *
 * Pure C11, integer-only, zero-libm, zero-heap, <512 B stack, 100% strip-
 * invariant across arbitrary [y0, y0+nrows) slices on a 466x466 RGB565 round
 * AMOLED panel with optional panel-order byte swapping (`swap_bytes`).
 *
 * Static Backdrop + Live Overlay Split:
 *   Each screen exposes:
 *     - `*_static_key(env)`  — deterministic non-zero hash of all static-plate
 *                              inputs (returns 0 on NULL) so `jr_display.c`
 *                              rebakes a shared 434 KB evictable PSRAM plate
 *                              ONLY when minute/telemetry changes.
 *     - `*_static_rows(...)` — renders the static backdrop/perimeter/card at
 *                              full strength (255) into `dst`.
 *     - `*_live_rows(...)`   — composites ONLY the tiny animated overlay
 *                              (Atmosphaera weather emblem ~3.7% area; Cortex
 *                              sparkline ~2.5% area) over `dst`.
 *     - `*_rows(...)`        — full procedural fallback (100% byte-for-byte
 *                              identical to static_rows + live_rows at
 *                              strength=255, plus smooth partial-strength fade).
 *
 * Two museum-grade procedural screen backdrops & perimeter instruments with
 * strict, mathematically enforced text safe zones (ZERO overlap with Barlow
 * typography drawn by jr_display.c):
 *
 *   1. ATMOSPHAERA    — jr_g3_atmosphaera_{static_key,static_rows,live_rows,rows}()
 *      Weather Screen Instrument:
 *        - Deep midnight-indigo radial atmospheric bowl inside r < 190 fading
 *          smoothly to pure OLED true black (#000000) at the rim.
 *        - 360-deg continuous thermal/solar perimeter arc (r in [190, 204])
 *          with 256-entry RGB565 LUT + outer bezel micro-ticks (r in [208, 224])
 *          with major cardinal ticks at 6A / 12P / 6P / 12A.
 *        - Live glowing golden Sun Jewel positioned on the 24h arc by hour &
 *          minute (6A=9 o'clock, 12P=12 o'clock, 6P=3 o'clock, 12A=6 o'clock).
 *        - Illuminated curved planetary horizon arc (cy=620, R=350..352)
 *          sweeping strictly below y >= 266 with razor-sharp crest & bloom.
 *        - Sculpted translucent weather emblem strictly clipped inside
 *          x in [248, 344], y in [148, 232] (ZERO overlap with temp digits).
 *
 *   2. CORTEX COMMAND — jr_g3_cortex_{static_key,static_rows,live_rows,rows}()
 *      Status + Codex Desk Aerospace Telemetry Deck:
 *        - 4 chamfered segmented 18-cell LED-bar radial gauges (r in [194, 224])
 *          + inner micro-ticks (r in [182, 190]):
 *            Top-Left     (200..255 deg): Emerald MEM         (mem_pct)
 *            Top-Right    (285..340 deg): Electric Cyan CPU   (cpu_pct)
 *            Bottom-Right ( 20.. 75 deg): Warm Amber PWR      (batt_pct, emerald glow if charging)
 *            Bottom-Left  (105..160 deg): Electric Violet AGENT (agent_pct)
 *        - Inner hairline radar crosshair reticle (r=148, r=168 rings + 4
 *          cardinal hairlines r in [120, 178]), cleanly faded/excluded inside
 *          the center mission card box and sparkline box.
 *        - Centered translucent glass mission card (x in [104, 362],
 *          y in [136, 304]) with luminous border, header pill bar
 *          (y in [148, 184]), and 3 glowing status LED dots at x=128,
 *          y=208 / 242 / 276.
 *        - Bottom live 24-bar symmetric cyan-to-violet sparkline waveform
 *          strictly inside x in [140, 326], y in [324, 354].
 */
#ifndef JR_DISPLAY_GLASS3_SCREENS_H
#define JR_DISPLAY_GLASS3_SCREENS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Panel geometry ------------------------------------------------------ */

#define JR_G3_PANEL_W   466
#define JR_G3_PANEL_H   466

/* ---- ATMOSPHAERA strict layout & text safe zones ------------------------- *
 * jr_display.c places Barlow typography inside these exact regions with
 * guaranteed ZERO overlap against the emblem, horizon, and perimeter arc:
 *   - Top condition/city label: y in [96, 134], centered inside r < 176
 *   - Giant temperature hero:   x in [96, 242], y in [142, 244]
 *   - Sculpted weather emblem:  x in [248, 344], y in [148, 232] (owned here)
 *   - Planetary horizon arc:    y >= 266 (clears completely below y = 264)
 *   - Bottom micro-stats band:  y in [326, 366], strictly inside r < 176
 */
#define JR_G3_ATMOS_BOWL_R         190
#define JR_G3_ATMOS_ARC_R_IN       190
#define JR_G3_ATMOS_ARC_R_OUT      204
#define JR_G3_ATMOS_TICK_R_IN      208
#define JR_G3_ATMOS_TICK_R_OUT     224

#define JR_G3_ATMOS_TEMP_X0         96
#define JR_G3_ATMOS_TEMP_X1        242
#define JR_G3_ATMOS_TEMP_Y0        142
#define JR_G3_ATMOS_TEMP_Y1        244

#define JR_G3_ATMOS_EMBLEM_X0      248
#define JR_G3_ATMOS_EMBLEM_X1      344
#define JR_G3_ATMOS_EMBLEM_Y0      148
#define JR_G3_ATMOS_EMBLEM_Y1      232

#define JR_G3_ATMOS_HORIZON_CLEAR_Y 264
#define JR_G3_ATMOS_HORIZON_Y_MIN   266
#define JR_G3_ATMOS_STATS_Y0        326
#define JR_G3_ATMOS_STATS_Y1        366
#define JR_G3_ATMOS_STATS_R_MAX     176

/* Concept 3 cardinal clock micro-label safe-zone anchor coordinates
 * (strictly inside r < 190, zero overlap with thermal arc, ticks, temp hero,
 * weather emblem, and bottom micro-stats). */
#define JR_G3_ATMOS_LBL_12P_X      233
#define JR_G3_ATMOS_LBL_12P_Y       56
#define JR_G3_ATMOS_LBL_12A_X      233
#define JR_G3_ATMOS_LBL_12A_Y      398
#define JR_G3_ATMOS_LBL_6A_X        58
#define JR_G3_ATMOS_LBL_6A_Y       228
#define JR_G3_ATMOS_LBL_6P_X       392
#define JR_G3_ATMOS_LBL_6P_Y       228

/* Weather condition kind — values 0..7 match jr_display_sky_t verbatim.
 * Raw WMO weather codes (e.g. 45/48 fog, 51..67/80..82 rain, 71..77/85..86
 * snow, 95..99 thunderstorm) are also normalized automatically. */
typedef enum {
    JR_G3_WX_UNKNOWN = 0,
    JR_G3_WX_CLEAR   = 1,
    JR_G3_WX_PARTLY  = 2,
    JR_G3_WX_CLOUDS  = 3,
    JR_G3_WX_FOG     = 4,
    JR_G3_WX_RAIN    = 5,
    JR_G3_WX_STORM   = 6,
    JR_G3_WX_SNOW    = 7,
} jr_g3_wx_kind_t;

typedef struct {
    uint8_t  hour;          /* 0..23 local hour (6A=left, 12P=top, 6P=right, 12A=bottom) */
    uint8_t  minute;        /* 0..59 local minute                                         */
    uint8_t  weather_code;  /* jr_g3_wx_kind_t (0..7) or WMO code (0..99)                 */
    bool     is_day;        /* true = daytime sun/palette, false = nighttime moon/palette */
    int16_t  temp_f;        /* current temperature in deg F (tints horizon atmosphere)    */
    uint8_t  rain_pct;      /* 0..100 precipitation chance (modulates rain/mist intensity)*/
    uint32_t now_ms;        /* free-running animation clock in milliseconds               */
    uint8_t  strength;      /* 0..255 overall fade (0 = no-op, 255 = full intensity)      */
} jr_g3_atmosphaera_t;

/* Deterministic non-zero hash of ATMOSPHAERA static-plate inputs
 * (hour, minute, is_day, temp_f; excludes now_ms, weather_code, rain_pct,
 * strength). Returns 0 if env == NULL. */
uint32_t jr_g3_atmosphaera_static_key(const jr_g3_atmosphaera_t *env);

/* Render ONLY the static ATMOSPHAERA backdrop (atmospheric bowl, 360-deg
 * thermal/solar arc, 48 bezel micro-ticks, golden Sun Jewel, and curved
 * planetary horizon for y >= 266) at full strength (255) into dst (starting
 * from #000000; outer OLED corners r > 232 are never touched). */
void jr_g3_atmosphaera_static_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                                   const jr_g3_atmosphaera_t *env);

/* Render ONLY the live animated translucent weather emblem strictly inside
 * x in [248, 344], y in [148, 232] (~3.7% of screen) composited over dst
 * using env->strength. */
void jr_g3_atmosphaera_live_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                                 const jr_g3_atmosphaera_t *env);

/* Render full procedural rows [y0, y0 + nrows) of ATMOSPHAERA into dst
 * (nrows * 466 RGB565). Pure integer, zero heap, 100% strip-invariant, and
 * byte-for-byte identical to static_rows + live_rows at strength=255. */
void jr_g3_atmosphaera_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                            const jr_g3_atmosphaera_t *env);

/* ---- CORTEX COMMAND strict layout & text safe zones ---------------------- *
 * jr_display.c places Barlow header + 3 tabular telemetry rows inside the
 * glass mission card with guaranteed ZERO overlap:
 *   - Mission card frame:     x in [104, 362], y in [136, 304]
 *   - Header pill bar:        x in [116, 350], y in [148, 184]
 *                             (text safe x in [134, 344], y in [152, 180])
 *   - 3 glowing status LEDs:  x = 128, y = 208 / 242 / 276
 *   - Tabular row text area:  x in [144, 350], row centers y = 208, 242, 276
 *   - Bottom live sparkline:  x in [140, 326], y in [324, 354] (owned here)
 */
#define JR_G3_CORTEX_GAUGE_R_IN    194
#define JR_G3_CORTEX_GAUGE_R_OUT   224
#define JR_G3_CORTEX_TICK_R_IN     182
#define JR_G3_CORTEX_TICK_R_OUT    190
#define JR_G3_CORTEX_GAUGE_CELLS    18

#define JR_G3_CORTEX_CARD_X0       104
#define JR_G3_CORTEX_CARD_X1       362
#define JR_G3_CORTEX_CARD_Y0       136
#define JR_G3_CORTEX_CARD_Y1       304

#define JR_G3_CORTEX_HDR_X0        116
#define JR_G3_CORTEX_HDR_X1        350
#define JR_G3_CORTEX_HDR_Y0        148
#define JR_G3_CORTEX_HDR_Y1        184

#define JR_G3_CORTEX_LED_X         128
#define JR_G3_CORTEX_ROW0_Y        208
#define JR_G3_CORTEX_ROW1_Y        242
#define JR_G3_CORTEX_ROW2_Y        276
#define JR_G3_CORTEX_TEXT_X0       144
#define JR_G3_CORTEX_TEXT_X1       350

#define JR_G3_CORTEX_SPARK_X0      140
#define JR_G3_CORTEX_SPARK_X1      326
#define JR_G3_CORTEX_SPARK_Y0      324
#define JR_G3_CORTEX_SPARK_Y1      354
#define JR_G3_CORTEX_SPARK_BARS     24

/* Concept 5 quadrant gauge micro-label safe-zone anchor coordinates
 * (strictly inside r < 160, zero overlap with gauges, inner ticks, mission
 * card [104..362]x[136..304], and sparkline [140..326]x[324..354]). */
#define JR_G3_CORTEX_LBL_MEM_X     152
#define JR_G3_CORTEX_LBL_MEM_Y     114
#define JR_G3_CORTEX_LBL_CPU_X     314
#define JR_G3_CORTEX_LBL_CPU_Y     114
#define JR_G3_CORTEX_LBL_AGENT_X   132
#define JR_G3_CORTEX_LBL_AGENT_Y   310
#define JR_G3_CORTEX_LBL_PWR_X     334
#define JR_G3_CORTEX_LBL_PWR_Y     310

typedef enum {
    JR_G3_LED_OFF    = 0,
    JR_G3_LED_GREEN  = 1,   /* Emerald nominal / online        */
    JR_G3_LED_CYAN   = 2,   /* Electric cyan active / metric   */
    JR_G3_LED_AMBER  = 3,   /* Warm amber caution / power      */
    JR_G3_LED_VIOLET = 4,   /* Electric violet Codex / agent   */
    JR_G3_LED_RED    = 5,   /* Crimson fault / alert           */
} jr_g3_led_color_t;

typedef struct {
    uint8_t  mem_pct;       /* 0..100 Top-Left Emerald MEM gauge (PSRAM/heap)     */
    uint8_t  cpu_pct;       /* 0..100 Top-Right Electric Cyan CPU gauge (FPS)     */
    uint8_t  batt_pct;      /* 0..100 Bottom-Right Warm Amber PWR gauge           */
    uint8_t  agent_pct;     /* 0..100 Bottom-Left Electric Violet AGENT / Wi-Fi   */
    bool     charging;      /* true -> PWR gauge glows emerald                    */
    bool     codex_active;  /* true -> card frame illuminates electric violet     */
    uint8_t  row_led[3];    /* jr_g3_led_color_t for rows at y=208, 242, 276      */
    uint8_t  bands[16];     /* 16-band spectrum for 24-bar sparkline (0 = pulse)  */
    uint32_t now_ms;        /* free-running animation clock in milliseconds       */
    uint8_t  strength;      /* 0..255 overall fade (0 = no-op, 255 = full)        */
} jr_g3_cortex_t;

/* Deterministic non-zero hash of CORTEX COMMAND static-plate inputs
 * (mem_pct, cpu_pct, batt_pct, agent_pct, charging, codex_active,
 * row_led[0..2]; excludes bands[], now_ms, strength). Returns 0 if env == NULL. */
uint32_t jr_g3_cortex_static_key(const jr_g3_cortex_t *env);

/* Render ONLY the static CORTEX COMMAND backdrop (4 chamfered 18-cell LED
 * gauges, inner micro-ticks, hairline radar crosshair reticle, frosted
 * mission card frame, header pill, dividers, and 3 glowing status LEDs) at
 * full strength (255) into dst (outer OLED corners r > 232 are never touched). */
void jr_g3_cortex_static_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                              const jr_g3_cortex_t *env);

/* Render ONLY the live animated bottom 24-bar symmetric cyan-to-violet
 * sparkline strictly inside x in [140, 326], y in [324, 354] (~2.5% of screen)
 * composited over dst using env->strength. */
void jr_g3_cortex_live_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                            const jr_g3_cortex_t *env);

/* Render full procedural rows [y0, y0 + nrows) of CORTEX COMMAND into dst
 * (nrows * 466 RGB565). Pure integer, zero heap, 100% strip-invariant, and
 * byte-for-byte identical to static_rows + live_rows at strength=255. */
void jr_g3_cortex_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                       const jr_g3_cortex_t *env);

#ifdef __cplusplus
}
#endif

#endif /* JR_DISPLAY_GLASS3_SCREENS_H */
