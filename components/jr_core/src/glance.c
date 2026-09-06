/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_glance — the rain warning and the morning briefing. Pure; see glance.h.
 */
#include "jr_core/glance.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- RAIN --------------------------------------------------------------- */

void jr_rain_warn_init(jr_rain_warn_t *st)
{
    st->armed = true;
}

int jr_rain_warning_step(jr_rain_warn_t *st, const uint8_t *pp, int n, int hour_now)
{
    if (pp == NULL || n <= 0 || hour_now < 0 || hour_now > 23) {
        return 0;
    }
    int first_wet = 0;      /* hours ahead of the first hour >= WARN */
    int wettest = -1;       /* highest known probability in the window */
    for (int k = 1; k <= JR_RAIN_LOOKAHEAD_H; ++k) {
        const int idx = hour_now + k;
        if (idx >= n) {
            break;
        }
        const int p = pp[idx];
        if (p == JR_RAIN_PP_UNKNOWN) {
            continue;
        }
        if (p > wettest) {
            wettest = p;
        }
        if (first_wet == 0 && p >= JR_RAIN_WARN_PCT) {
            first_wet = k;
        }
    }
    if (st->armed) {
        if (first_wet != 0) {
            st->armed = false;
            return first_wet;
        }
        return 0;
    }
    /* Warned already: only a window that has genuinely dried out re-arms,
     * so a probability wobbling 55 <-> 65 does not warn twice. A window
     * with no known hour at all keeps the latch as it is. */
    if (wettest >= 0 && wettest < JR_RAIN_CLEAR_PCT) {
        st->armed = true;
    }
    return 0;
}

/* ---- BRIEF -------------------------------------------------------------- */

void jr_briefing_init(jr_briefing_t *st)
{
    st->last_yday = -1;
}

bool jr_briefing_due(jr_briefing_t *st, int hour, int yday, bool lifted_after_rest)
{
    if (!lifted_after_rest || hour < JR_BRIEF_HOUR_FROM || hour >= JR_BRIEF_HOUR_TO ||
        yday < 0 || yday > 366) {
        return false;
    }
    if (st->last_yday == (int16_t)yday) {
        return false;
    }
    st->last_yday = (int16_t)yday;
    return true;
}

static const char *const k_wday[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};
static const char *const k_mon[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

/* snprintf that appends and never runs past cap; returns the new length. */
static int app(char *dst, int len, size_t cap, const char *fmt, ...)
{
    if (len < 0 || (size_t)len >= cap) {
        return len;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(dst + len, cap - (size_t)len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return len;
    }
    return (size_t)len + (size_t)n >= cap ? (int)cap - 1 : len + n;
}

int jr_briefing_compose(char *dst, size_t cap, const jr_briefing_facts_t *f)
{
    if (dst == NULL || cap == 0) {
        return 0;
    }
    dst[0] = '\0';
    int len = 0;
    len = app(dst, len, cap,
              "Give Sir his morning briefing in two or three short spoken "
              "sentences, no list, only these facts.");
    if (f->have_date && f->wday >= 0 && f->wday < 7 && f->mon >= 0 && f->mon < 12) {
        len = app(dst, len, cap, " It is %s %d %s.", k_wday[f->wday], f->mday, k_mon[f->mon]);
    }
    if (f->have_weather) {
        len = app(dst, len, cap, " Fort Lauderdale is %d degrees", f->temp_f);
        if (f->condition != NULL && f->condition[0] != '\0') {
            len = app(dst, len, cap, " and %s", f->condition);
        }
        len = app(dst, len, cap, ", high %d, low %d", f->hi_f, f->lo_f);
        if (f->rain_in_h > 0) {
            len = app(dst, len, cap, ", rain likely in %d hour%s", f->rain_in_h,
                      f->rain_in_h == 1 ? "" : "s");
        }
        len = app(dst, len, cap, ".");
    }
    if (f->tasks_done > 0) {
        len = app(dst, len, cap, " %d delegated task%s finished", f->tasks_done,
                  f->tasks_done == 1 ? "" : "s");
        if (f->task_title != NULL && f->task_title[0] != '\0') {
            len = app(dst, len, cap, ", the latest: %.80s", f->task_title);
        }
        len = app(dst, len, cap, ".");
    }
    if (f->battery_pct >= 0 && f->battery_pct < 30 && !f->on_usb) {
        len = app(dst, len, cap, " Battery %d percent, worth charging.", f->battery_pct);
    }
    return len;
}

int jr_briefing_caption(char *dst, size_t cap, const jr_briefing_facts_t *f)
{
    if (dst == NULL || cap == 0) {
        return 0;
    }
    char line[JR_BRIEF_CAPTION_GLYPHS + 1];
    int len = 0;
    line[0] = '\0';
    len = app(line, len, sizeof line, "GOOD MORNING");
    if (f->have_weather) {
        /* The last word of the condition is the one that names the sky:
         * "LIGHT DRIZZLE" -> "DRIZZLE", "MODERATE RAIN SHOWERS" -> "SHOWERS". */
        const char *w = f->condition != NULL ? f->condition : "";
        const char *sp = strrchr(w, ' ');
        w = sp != NULL ? sp + 1 : w;
        len = app(line, len, sizeof line, " %d*", f->temp_f);
        if (w[0] != '\0') {
            len = app(line, len, sizeof line, " %.8s", w);
        }
    }
    if (f->tasks_done > 0) {
        len = app(line, len, sizeof line, " %d DONE", f->tasks_done);
    }
    for (char *c = line; *c != '\0'; ++c) {
        if (*c >= 'a' && *c <= 'z') {
            *c = (char)(*c - 'a' + 'A');
        }
    }
    const int n = snprintf(dst, cap, "%s", line);
    return n < 0 ? 0 : (size_t)n >= cap ? (int)cap - 1 : n;
}
