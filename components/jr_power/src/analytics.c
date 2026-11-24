/*
 * SPDX-FileCopyrightText: 2026 Pascal Ledesma / Ingenious Digital
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure C11 rolling battery analytics & ETA engine (no ESP-IDF dependencies).
 */
#include "jr_power/analytics.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define JR_POWER_TEMP_INVALID ((int8_t)-128)
#define JR_POWER_MIN_SPAN_S   180u
#define JR_POWER_MIN_DPCT     2
#define JR_POWER_MIN_DMV      15
#define JR_POWER_ETA_MIN_CLAMP 1
#define JR_POWER_ETA_MAX_CLAMP 5999

static void add_u32_sat(uint32_t *acc, uint32_t delta)
{
    if (UINT32_MAX - *acc < delta) {
        *acc = UINT32_MAX;
    } else {
        *acc += delta;
    }
}

static int16_t div_round_clamp_i16(int64_t num, int64_t den)
{
    if (den <= 0) {
        return 0;
    }
    int64_t q;
    if (num >= 0) {
        q = (num + den / 2) / den;
    } else {
        q = -((-num + den / 2) / den);
    }
    if (q > INT16_MAX) {
        return INT16_MAX;
    }
    if (q < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)q;
}

static int32_t clamp_eta_min(int64_t mins)
{
    if (mins < JR_POWER_ETA_MIN_CLAMP) {
        return JR_POWER_ETA_MIN_CLAMP;
    }
    if (mins > JR_POWER_ETA_MAX_CLAMP) {
        return JR_POWER_ETA_MAX_CLAMP;
    }
    return (int32_t)mins;
}

void jr_power_analytics_init(jr_power_analytics_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->chip_c_max = JR_POWER_TEMP_INVALID;
    ctx->pmic_c_max = JR_POWER_TEMP_INVALID;
}

void jr_power_analytics_push(jr_power_analytics_ctx_t *ctx,
                             const jr_power_sample_t *s,
                             bool care_mode)
{
    if (ctx == NULL || s == NULL) {
        return;
    }

    if (ctx->have_prev) {
        const uint32_t dt = (s->uptime_s >= ctx->last_uptime_s)
                                ? (s->uptime_s - ctx->last_uptime_s)
                                : 0u;
        if (ctx->last_usb) {
            add_u32_sat(&ctx->time_on_usb_s, dt);
        } else {
            add_u32_sat(&ctx->time_on_bat_s, dt);
        }

        const bool usb_changed = (s->usb != ctx->last_usb);
        const bool mode_edge = usb_changed ||
                               (s->charging != ctx->last_charging) ||
                               (care_mode != ctx->last_care) ||
                               (s->uptime_s < ctx->last_uptime_s);
        if (mode_edge) {
            ctx->head = 0u;
            ctx->count = 0u;
        }
        if (usb_changed) {
            /* Keep battery voltage sag isolated from USB charging voltage. */
            ctx->session_min_mv = 0u;
            ctx->session_max_mv = 0u;
        }
    }

    /* Update cumulative session peak temperatures (ignore -128 invalid). */
    if (s->chip_c != JR_POWER_TEMP_INVALID &&
        (ctx->chip_c_max == JR_POWER_TEMP_INVALID || s->chip_c > ctx->chip_c_max)) {
        ctx->chip_c_max = s->chip_c;
    }
    if (s->pmic_c != JR_POWER_TEMP_INVALID &&
        (ctx->pmic_c_max == JR_POWER_TEMP_INVALID || s->pmic_c > ctx->pmic_c_max)) {
        ctx->pmic_c_max = s->pmic_c;
    }

    /* Update session voltage envelope for valid readings. */
    if (s->mv > 0u) {
        if (ctx->session_min_mv == 0u || s->mv < ctx->session_min_mv) {
            ctx->session_min_mv = s->mv;
        }
        if (s->mv > ctx->session_max_mv) {
            ctx->session_max_mv = s->mv;
        }
    }

    /* Append into rolling ring buffer. */
    ctx->ring[ctx->head] = *s;
    ctx->head = (uint16_t)((ctx->head + 1u) % JR_POWER_ANALYTICS_CAP);
    if (ctx->count < JR_POWER_ANALYTICS_CAP) {
        ctx->count++;
    }

    ctx->have_prev = true;
    ctx->last_usb = s->usb;
    ctx->last_charging = s->charging;
    ctx->last_care = care_mode;
    ctx->last_uptime_s = s->uptime_s;
}

void jr_power_analytics_snapshot(const jr_power_analytics_ctx_t *ctx,
                                 bool care_mode,
                                 jr_power_analytics_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->eta_empty_min = -1;
    out->eta_full_min = -1;
    out->chip_c_max = JR_POWER_TEMP_INVALID;
    out->pmic_c_max = JR_POWER_TEMP_INVALID;

    if (ctx == NULL) {
        return;
    }

    out->session_min_mv = ctx->session_min_mv;
    out->session_max_mv = ctx->session_max_mv;
    out->chip_c_max = ctx->chip_c_max;
    out->pmic_c_max = ctx->pmic_c_max;
    out->time_on_bat_s = ctx->time_on_bat_s;
    out->time_on_usb_s = ctx->time_on_usb_s;
    out->sample_count = ctx->count;

    if (ctx->count == 0u) {
        return;
    }

    const uint16_t count = (ctx->count <= JR_POWER_ANALYTICS_CAP)
                               ? ctx->count
                               : JR_POWER_ANALYTICS_CAP;
    const uint16_t head = (uint16_t)(ctx->head % JR_POWER_ANALYTICS_CAP);
    const uint16_t newest_idx = (uint16_t)((head + JR_POWER_ANALYTICS_CAP - 1u) %
                                           JR_POWER_ANALYTICS_CAP);
    const uint16_t oldest_idx = (uint16_t)((head + JR_POWER_ANALYTICS_CAP - count) %
                                           JR_POWER_ANALYTICS_CAP);

    const jr_power_sample_t *oldest = &ctx->ring[oldest_idx];
    const jr_power_sample_t *newest = &ctx->ring[newest_idx];

    /* Battery voltage sag across the current on-battery session. */
    if (!newest->usb && count >= 2u &&
        ctx->session_min_mv > 0u &&
        ctx->session_max_mv >= ctx->session_min_mv) {
        out->sag_mv = (uint16_t)(ctx->session_max_mv - ctx->session_min_mv);
    }

    if (count < 2u || newest->uptime_s <= oldest->uptime_s) {
        return;
    }

    const uint32_t span_s = newest->uptime_s - oldest->uptime_s;
    const bool pct_ok = (oldest->pct >= 0 && oldest->pct <= 100 &&
                         newest->pct >= 0 && newest->pct <= 100);
    const bool mv_ok = (oldest->mv > 0u && newest->mv > 0u);

    const int32_t dpct = pct_ok ? ((int32_t)newest->pct - (int32_t)oldest->pct) : 0;
    const int32_t dmv = mv_ok ? ((int32_t)newest->mv - (int32_t)oldest->mv) : 0;
    const int32_t abs_dpct = dpct < 0 ? -dpct : dpct;
    const int32_t abs_dmv = dmv < 0 ? -dmv : dmv;

    /* Gate noisy startup intervals until enough time and measurable movement. */
    if (span_s < JR_POWER_MIN_SPAN_S ||
        ((!pct_ok || abs_dpct < JR_POWER_MIN_DPCT) && abs_dmv < JR_POWER_MIN_DMV)) {
        return;
    }

    out->rate_valid = true;
    if (pct_ok) {
        out->rate_pct_x10_hr = div_round_clamp_i16((int64_t)dpct * 36000,
                                                   (int64_t)span_s);
    }
    if (mv_ok) {
        out->mv_per_hr = div_round_clamp_i16((int64_t)dmv * 3600,
                                             (int64_t)span_s);
    }

    /* Time-to-empty on battery. */
    if (!newest->usb && pct_ok && out->rate_pct_x10_hr < 0) {
        if (newest->pct <= 0) {
            out->eta_empty_min = 0;
        } else {
            const int64_t abs_rate = -(int64_t)out->rate_pct_x10_hr;
            const int64_t mins = ((int64_t)newest->pct * 600 + abs_rate / 2) / abs_rate;
            out->eta_empty_min = clamp_eta_min(mins);
        }
    }

    /* Time-to-full while actively charging on USB. */
    if (newest->usb && newest->charging && pct_ok && out->rate_pct_x10_hr > 0) {
        const int32_t target = care_mode ? 80 : 100;
        const int32_t cur_pct = newest->pct;
        if (cur_pct >= target) {
            out->eta_full_min = 0;
        } else {
            int64_t eff_rem_x100;
            if (!care_mode && target == 100 && cur_pct < 95) {
                /* Apply modest 1.15x CV taper factor to the >80% segment. */
                const int32_t cc_pct = (cur_pct < 80) ? (80 - cur_pct) : 0;
                const int32_t cv_pct = (target - cur_pct) - cc_pct;
                eff_rem_x100 = (int64_t)cc_pct * 100 + (int64_t)cv_pct * 115;
            } else {
                eff_rem_x100 = (int64_t)(target - cur_pct) * 100;
            }
            const int64_t rate = (int64_t)out->rate_pct_x10_hr;
            /* mins = (eff_rem_x100 / 100) * 600 / rate = eff_rem_x100 * 6 / rate */
            const int64_t mins = (eff_rem_x100 * 6 + rate / 2) / rate;
            out->eta_full_min = clamp_eta_min(mins);
        }
    }
}
