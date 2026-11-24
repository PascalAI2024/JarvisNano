/* SPDX-License-Identifier: Apache-2.0 */
#include "jr_probe/board_probe.h"

#include <stdarg.h>
#include <stdio.h>

typedef struct { char *buf; size_t cap, off; bool over; } sink_t;

static void put(sink_t *s, const char *fmt, ...)
{
    if (s->over) { return; }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(s->buf + s->off, s->cap - s->off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= s->cap - s->off) {
        s->over = true;
        return;
    }
    s->off += (size_t)n;
}

static const char *tf(bool v) { return v ? "true" : "false"; }

/* Every block opens with its ok flag and, when it failed, the reason. */
static void head(sink_t *s, const char *key, bool ok, const char *why)
{
    put(s, "\"%s\":{\"ok\":%s", key, tf(ok));
    if (!ok) { put(s, ",\"why\":\"%s\"", why ? why : "unknown"); }
}

/* Tenths of a degree as "-12.3", sign-safe around zero. */
static void tenths(sink_t *s, const char *key, int t)
{
    const int a = t < 0 ? -t : t;
    put(s, ",\"%s\":%s%d.%d", key, t < 0 ? "-" : "", a / 10, a % 10);
}

static void axis3(sink_t *s, const char *key, const float v[3])
{
    put(s, ",\"%s\":[%.2f,%.2f,%.2f]", key, (double)v[0], (double)v[1],
        (double)v[2]);
}

int jr_probe_report_json(const jr_probe_report_t *r, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) { return -1; }
    buf[0] = '\0';
    if (r == NULL) { return -1; }
    sink_t s = { buf, cap, 0, false };

    put(&s, "{");

    const jr_probe_te_t *te = &r->te;
    head(&s, "te", te->ok, te->why);
    if (te->ok) {
        put(&s, ",\"window_ms\":%u,\"edges\":%u,\"hz\":%u,"
                "\"period_us_min\":%u,\"period_us_max\":%u,\"level_now\":%d",
            (unsigned)te->window_ms, (unsigned)te->edges, (unsigned)te->hz,
            (unsigned)te->period_us_min, (unsigned)te->period_us_max,
            te->level_now);
        if (te->edges == 0) {
            put(&s, ",\"note\":\"no edges: panel off, TE not routed, or pin dead\"");
        }
    }
    put(&s, "},");

    const jr_probe_pwr_t *p = &r->pwr_key;
    head(&s, "pwr_key", p->ok, p->why);
    if (p->ok) {
        put(&s, ",\"level\":%d,\"watched\":%s", p->level, tf(p->watched));
        if (p->watched) {
            put(&s, ",\"watch_ms\":%u,\"changed\":%s,\"transitions\":%u,"
                    "\"high_ms\":%u,\"level_min\":%d,\"level_max\":%d",
                (unsigned)p->watch_ms, tf(p->transitions > 0),
                (unsigned)p->transitions, (unsigned)p->high_ms,
                p->level_min, p->level_max);
        }
    }
    put(&s, "},");

    const jr_probe_imu_t *m = &r->imu;
    head(&s, "imu", m->ok, m->why);
    if (m->ok) {
        put(&s, ",\"who_am_i\":%u,\"revision\":%u,\"ctrl1\":%u,\"ctrl2\":%u,"
                "\"ctrl3\":%u,\"ctrl7\":%u,\"wom\":%s,\"temp_ok\":%s",
            m->who, m->rev, m->ctrl1, m->ctrl2, m->ctrl3, m->ctrl7,
            tf(m->wom), tf(m->temp_ok));
        if (m->temp_ok) { tenths(&s, "temp_c", m->temp_tenths); }
        if (m->gyro_asked) {
            put(&s, ",\"gyro\":{\"ran\":%s", tf(m->gyro_ran));
            if (m->gyro_ran) {
                put(&s, ",\"samples\":%u", m->gyro_n);
                axis3(&s, "min_dps", m->gmin);
                axis3(&s, "max_dps", m->gmax);
                axis3(&s, "mean_dps", m->gmean);
                put(&s, ",\"restored\":%s,\"ctrl3_after\":%u,\"ctrl7_after\":%u",
                    tf(m->restored), m->ctrl3_after, m->ctrl7_after);
            } else {
                put(&s, ",\"why\":\"%s\"", m->gyro_why ? m->gyro_why : "unknown");
            }
            put(&s, "}");
        }
    }
    put(&s, "},");

    const jr_probe_touch_t *t = &r->touch;
    head(&s, "touch", t->ok, t->why);
    if (t->ok) {
        put(&s, ",\"watched\":%s,\"driver_max_fingers\":1,"
                "\"sensorlib_max_fingers\":2", tf(t->watched));
        if (t->watched) {
            put(&s, ",\"watch_ms\":%u,\"reads\":%u,\"max_fingers\":%u,"
                    "\"seen\":%s", (unsigned)t->watch_ms, (unsigned)t->reads,
                t->max_fingers, tf(t->seen));
            if (t->seen) {
                put(&s, ",\"ack\":%u,\"raw\":\"", t->ack);
                for (unsigned i = 0; i < JR_PROBE_TOUCH_RAW_LEN; ++i) {
                    put(&s, "%02x", t->raw[i]);
                }
                put(&s, "\"");
            }
        }
    }
    put(&s, "},");

    const jr_probe_pmic_t *x = &r->pmic;
    head(&s, "pmic", x->ok, x->why);
    if (x->ok) {
        put(&s, ",\"adc_before\":%u,\"adc_during\":%u,\"adc_after\":%u,"
                "\"adc_restored\":%s", x->adc_before, x->adc_during,
            x->adc_after, tf(x->adc_restored));
        put(&s, ",\"charge_cc_raw\":%u", x->chg_cc_raw);
        if (x->chg_cc_known) { put(&s, ",\"charge_cc_ma\":%u", (unsigned)x->chg_cc_ma); }
        if (x->die_ok) { tenths(&s, "die_c", x->die_tenths); }
        if (x->vbus_ok) { put(&s, ",\"vbus_mv\":%u", (unsigned)x->vbus_mv); }
        if (x->vsys_ok) { put(&s, ",\"vsys_mv\":%u", (unsigned)x->vsys_mv); }
        if (x->vbat_ok) { put(&s, ",\"vbat_mv\":%u", (unsigned)x->vbat_mv); }
        if (x->ts_ok) {
            put(&s, ",\"ts_raw\":%u,\"ts_mv\":%u.%u", (unsigned)x->ts_raw,
                (unsigned)(x->ts_raw / 2u), (unsigned)((x->ts_raw % 2u) * 5u));
        }
    }
    put(&s, "},");

    put(&s, "\"spi\":{\"ok\":true,\"hz\":%u,\"source\":\"%s\"}}",
        (unsigned)r->spi_hz, r->spi_source ? r->spi_source : "unknown");

    if (s.over) {
        buf[cap - 1] = '\0';
        return -1;
    }
    return (int)s.off;
}
