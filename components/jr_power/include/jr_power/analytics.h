/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_power/analytics.h — Pure C11 rolling battery analytics & ETA engine.
 *
 * Zero ESP-IDF includes: compiles identically on ESP32-S3 firmware and host
 * test suites. Tracks a 60-sample rolling window (e.g. 30 s cadence = 30 min)
 * for signed %/hr and mV/hr slopes, Time-to-Empty on battery, Time-to-Full
 * (80% Desk Care linear target or 100% target with CV taper), battery voltage
 * sag, session peak chip/PMIC temperatures, and cumulative battery/USB time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_POWER_ANALYTICS_CAP 60

typedef struct {
    uint32_t uptime_s;
    int8_t   pct;
    uint16_t mv;
    bool     usb;
    bool     charging;
    int8_t   chip_c;
    int8_t   pmic_c;
} jr_power_sample_t;

typedef struct {
    bool     rate_valid;       /* true once span >=180s and (|dpct|>=2 or |dmv|>=15) */
    int16_t  rate_pct_x10_hr;  /* signed %/hr * 10 (neg=drain, pos=charge) */
    int16_t  mv_per_hr;        /* signed mV/hr */
    int32_t  eta_empty_min;    /* -1 if unknown or on USB */
    int32_t  eta_full_min;     /* -1 if unknown or not charging; target=80 if care else 100 */
    uint16_t session_min_mv;
    uint16_t session_max_mv;
    uint16_t sag_mv;           /* session_max_mv - session_min_mv on battery (0 if <2 samples) */
    int8_t   chip_c_max;
    int8_t   pmic_c_max;
    uint32_t time_on_bat_s;
    uint32_t time_on_usb_s;
    uint16_t sample_count;
} jr_power_analytics_t;

typedef struct {
    jr_power_sample_t ring[JR_POWER_ANALYTICS_CAP];
    uint16_t head;
    uint16_t count;
    bool     have_prev;
    bool     last_usb;
    bool     last_charging;
    bool     last_care;
    uint32_t last_uptime_s;
    uint16_t session_min_mv;
    uint16_t session_max_mv;
    int8_t   chip_c_max;
    int8_t   pmic_c_max;
    uint32_t time_on_bat_s;
    uint32_t time_on_usb_s;
} jr_power_analytics_ctx_t;

void jr_power_analytics_init(jr_power_analytics_ctx_t *ctx);
void jr_power_analytics_push(jr_power_analytics_ctx_t *ctx, const jr_power_sample_t *s, bool care_mode);
void jr_power_analytics_snapshot(const jr_power_analytics_ctx_t *ctx, bool care_mode, jr_power_analytics_t *out);

#ifdef __cplusplus
}
#endif
