/* SPDX-License-Identifier: Apache-2.0
 *
 * jr_probe/board_probe.h - result types and the JSON writer for GET
 * /api/diag/board.
 *
 * Plain C99 with no ESP-IDF include, so the writer compiles on the host
 * (components/jr_probe/tests/test_board_json.py). The probes themselves live
 * with the hardware they touch: jr_imu_probe(), jr_power_probe(),
 * jr_hal_touch_probe() fill these structs, and gpio_probe.h covers the two
 * pins. Every `why` is a string literal with nothing to escape.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_PROBE_TOUCH_RAW_LEN 15u   /* 2 fingers x 5 bytes + 5 header bytes */

typedef struct {
    bool ok; const char *why;
    uint32_t window_ms;
    uint32_t edges;                  /* rising edges seen in the window      */
    uint32_t hz;                     /* edges * 1000 / window_ms             */
    uint32_t period_us_min, period_us_max;
    int level_now;
} jr_probe_te_t;

typedef struct {
    bool ok; const char *why;
    int level;                       /* GPIO3 now                            */
    bool watched; uint32_t watch_ms;
    uint32_t transitions;            /* level changes seen while watching    */
    uint32_t high_ms;                /* time spent high while watching       */
    int level_min, level_max;
} jr_probe_pwr_t;

typedef struct {
    bool ok; const char *why;
    uint8_t who, rev, ctrl1, ctrl2, ctrl3, ctrl7;
    bool wom;                        /* chip is in (or sampler left it in) WoM */
    bool temp_ok; int temp_tenths;   /* tenths of a degree C                 */
    bool gyro_asked, gyro_ran; const char *gyro_why;
    uint8_t gyro_n;
    float gmin[3], gmax[3], gmean[3];/* dps, x y z                           */
    bool restored;                   /* CTRL3/CTRL7 read back as found       */
    uint8_t ctrl3_after, ctrl7_after;
} jr_probe_imu_t;

typedef struct {
    bool ok; const char *why;
    bool watched; uint32_t watch_ms;
    uint32_t reads;                  /* raw reports read while watching      */
    uint8_t max_fingers;             /* largest count byte (& 0x7F) seen     */
    bool seen; uint8_t raw[JR_PROBE_TOUCH_RAW_LEN]; /* report with that count */
    uint8_t ack;                     /* byte 6 of that report (0xAB = valid) */
} jr_probe_touch_t;

typedef struct {
    bool ok; const char *why;
    uint8_t adc_before, adc_during, adc_after;
    bool adc_restored;
    uint8_t chg_cc_raw; uint32_t chg_cc_ma; bool chg_cc_known;
    bool die_ok; int die_tenths;     /* tenths of a degree C                 */
    bool vbus_ok; uint32_t vbus_mv;
    bool vsys_ok; uint32_t vsys_mv;
    bool vbat_ok; uint32_t vbat_mv;
    bool ts_ok; uint32_t ts_raw;     /* 0.5 mV per count                     */
} jr_probe_pmic_t;

typedef struct {
    jr_probe_te_t te;
    jr_probe_pwr_t pwr_key;
    jr_probe_imu_t imu;
    jr_probe_touch_t touch;
    jr_probe_pmic_t pmic;
    uint32_t spi_hz;                 /* panel QSPI clock the firmware sets   */
    const char *spi_source;
} jr_probe_report_t;

/* Write the report as one JSON object. Returns the byte count (excluding the
 * NUL), or -1 if `cap` was too small; the buffer is then still terminated. */
int jr_probe_report_json(const jr_probe_report_t *r, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
