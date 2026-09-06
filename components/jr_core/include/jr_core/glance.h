/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_glance — what the glass says without being asked. Pure: no drivers,
 * no clock, no FreeRTOS. The composition root feeds it the hour, the day
 * and the numbers it already holds; it answers with a decision and a
 * sentence. Two things live here:
 *
 *   RAIN   the hourly rain warning. Open-Meteo's precipitation probability
 *          for 36 hours from today's midnight arrives with the weather
 *          glance; a warning is due once when any of the NEXT THREE hours
 *          reaches JR_RAIN_WARN_PCT, and it re-arms only after the whole
 *          window has dropped under JR_RAIN_CLEAR_PCT, so a showery
 *          afternoon says "rain in 2 h" once, not every ten minutes.
 *
 *   BRIEF  the morning glance. The first lift after a rest, once per
 *          calendar day, inside the morning window, earns one spoken
 *          briefing composed from the date, the weather, the rain window,
 *          the delegated tasks that finished and the battery. Nothing is
 *          invented: a fact the device does not hold is left out of the
 *          sentence, never guessed.
 */
#ifndef JR_CORE_GLANCE_H
#define JR_CORE_GLANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- RAIN --------------------------------------------------------------- */
#define JR_RAIN_HOURS        36    /* today's 24 + tomorrow's first 12        */
#define JR_RAIN_PP_UNKNOWN   255   /* the hour was not in the answer          */
#define JR_RAIN_WARN_PCT     60
#define JR_RAIN_CLEAR_PCT    30
#define JR_RAIN_LOOKAHEAD_H  3

typedef struct {
    bool armed;             /* true: the next qualifying window warns       */
} jr_rain_warn_t;

void jr_rain_warn_init(jr_rain_warn_t *st);

/* pp[0..n) is the probability per hour from today's midnight; hour_now is
 * the local hour (0..23). Returns 1..JR_RAIN_LOOKAHEAD_H — hours until the
 * first hour at or above JR_RAIN_WARN_PCT — when a warning is due NOW, else
 * 0. Unknown hours never warn and never clear. */
int jr_rain_warning_step(jr_rain_warn_t *st, const uint8_t *pp, int n, int hour_now);

/* ---- BRIEF -------------------------------------------------------------- */
#define JR_BRIEF_HOUR_FROM   5     /* 05:00 local ...                         */
#define JR_BRIEF_HOUR_TO     11    /* ... up to but not including 11:00       */

typedef struct {
    int16_t last_yday;      /* day-of-year the last briefing was given; -1   */
} jr_briefing_t;

void jr_briefing_init(jr_briefing_t *st);

/* True exactly once per yday, when a lift-after-rest lands inside the
 * morning window. hour is local 0..23, yday 0..365. */
bool jr_briefing_due(jr_briefing_t *st, int hour, int yday, bool lifted_after_rest);

typedef struct {
    bool        have_date;
    int         wday;           /* 0 = Sunday                                */
    int         mday;
    int         mon;            /* 0 = January                               */
    bool        have_weather;
    int         temp_f;
    int         hi_f;
    int         lo_f;
    const char *condition;      /* "Light drizzle"; any case; may be ""      */
    int         rain_in_h;      /* 0: none in the window                     */
    int         tasks_done;     /* delegated items finished since last time  */
    const char *task_title;     /* the newest; NULL when none                */
    int         battery_pct;    /* 0..100; anything else = no gauge          */
    bool        on_usb;
} jr_briefing_facts_t;

/* The text turn handed to the model: an instruction plus the facts, so the
 * voice stays the persona's and the numbers stay the device's. Returns the
 * length written (snprintf semantics, always NUL-terminated). */
int jr_briefing_compose(char *dst, size_t cap, const jr_briefing_facts_t *f);

/* The muted form: one caption line in the shell's glyphs (no degree sign —
 * the shell prints '*'), at most JR_BRIEF_CAPTION_GLYPHS characters. */
#define JR_BRIEF_CAPTION_GLYPHS 38
int jr_briefing_caption(char *dst, size_t cap, const jr_briefing_facts_t *f);

#ifdef __cplusplus
}
#endif
#endif /* JR_CORE_GLANCE_H */
