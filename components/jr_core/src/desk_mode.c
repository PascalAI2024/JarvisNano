/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/desk_mode.c — pure unified Desk Presence, Focus & Breathe state
 * machine. No ESP-IDF, no heap, no libm.
 */
#include "jr_core/desk_mode.h"

#include <ctype.h>
#include <string.h>

static void sanitize_label(const char *src, char out[JR_DESK_LABEL_CAP],
                           const char *fallback)
{
    out[0] = '\0';
    if (src != NULL) {
        while (*src != '\0' && isspace((unsigned char)*src)) {
            ++src;
        }
        size_t len = 0u;
        bool prev_space = false;
        for (; *src != '\0' && len + 1u < JR_DESK_LABEL_CAP; ++src) {
            const unsigned char ch = (unsigned char)*src;
            if (isspace(ch)) {
                if (!prev_space && len > 0u) {
                    out[len++] = ' ';
                    prev_space = true;
                }
                continue;
            }
            if (ch >= 0x20u && ch <= 0x7Eu && ch != '"' && ch != '\\') {
                out[len++] = (char)ch;
                prev_space = false;
            }
        }
        while (len > 0u && out[len - 1u] == ' ') {
            --len;
        }
        out[len] = '\0';
    }
    if (out[0] == '\0' && fallback != NULL) {
        size_t i = 0u;
        for (; fallback[i] != '\0' && i + 1u < JR_DESK_LABEL_CAP; ++i) {
            out[i] = fallback[i];
        }
        out[i] = '\0';
    }
}

static void reset_active_fields(jr_desk_state_t *s)
{
    s->mode = JR_DESK_NORMAL;
    s->started_ms = 0u;
    s->duration_ms = 0u;
    s->silent = false;
    s->dnd_silent = false;
    memset(s->label, 0, sizeof(s->label));
    s->breathe_pattern = JR_BREATHE_BOX;
    s->breathe_cycles_total = 0u;
    s->breathe_cycle_cur = 0u;
    s->breathe_phase = JR_BREATHE_INHALE;
    s->breathe_phase_pct = 0u;
    s->breathe_sec_left = 0u;
    s->warn_5m_fired = false;
    s->on_air_completed_fired = false;
}

static void breathe_phase_durations(jr_breathe_pattern_t pat, uint8_t durs[4])
{
    switch (pat) {
    case JR_BREATHE_CALM:
        durs[0] = 4u;
        durs[1] = 7u;
        durs[2] = 8u;
        durs[3] = 0u;
        break;
    case JR_BREATHE_ENERGIZE:
        durs[0] = 4u;
        durs[1] = 2u;
        durs[2] = 4u;
        durs[3] = 0u;
        break;
    case JR_BREATHE_BOX:
    default:
        durs[0] = 4u;
        durs[1] = 4u;
        durs[2] = 4u;
        durs[3] = 4u;
        break;
    }
}

static uint32_t fnv1a_32(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0u; i < len; ++i) {
        h ^= (uint32_t)data[i];
        h *= 16777619u;
    }
    return h;
}

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

void jr_desk_init(jr_desk_state_t *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

void jr_desk_set_normal(jr_desk_state_t *s)
{
    if (s == NULL) {
        return;
    }
    reset_active_fields(s);
}

void jr_desk_start_on_air(jr_desk_state_t *s, uint32_t now_ms,
                          uint32_t duration_s, const char *label)
{
    if (s == NULL) {
        return;
    }
    if (duration_s > JR_DESK_MAX_DURATION_S) {
        duration_s = JR_DESK_MAX_DURATION_S;
    }
    reset_active_fields(s);
    s->mode = JR_DESK_ON_AIR;
    s->started_ms = now_ms;
    s->duration_ms = duration_s * 1000u;
    s->silent = true;
    s->dnd_silent = true;
    sanitize_label(label, s->label, "ON AIR");
}

void jr_desk_start_focus(jr_desk_state_t *s, uint32_t now_ms,
                         uint32_t duration_s, const char *label, bool silent)
{
    if (s == NULL) {
        return;
    }
    if (duration_s == 0u) {
        duration_s = JR_DESK_FOCUS_DEFAULT_S;
    } else if (duration_s > JR_DESK_MAX_DURATION_S) {
        duration_s = JR_DESK_MAX_DURATION_S;
    }
    reset_active_fields(s);
    s->mode = JR_DESK_FOCUS;
    s->started_ms = now_ms;
    s->duration_ms = duration_s * 1000u;
    s->silent = silent;
    s->dnd_silent = silent;
    sanitize_label(label, s->label, "FOCUS");
}

void jr_desk_start_breathe(jr_desk_state_t *s, uint32_t now_ms,
                           jr_breathe_pattern_t pat, uint8_t cycles,
                           bool silent)
{
    if (s == NULL) {
        return;
    }
    if (pat != JR_BREATHE_BOX && pat != JR_BREATHE_CALM &&
        pat != JR_BREATHE_ENERGIZE) {
        pat = JR_BREATHE_BOX;
    }
    if (cycles == 0u) {
        cycles = JR_DESK_BREATHE_DEFAULT_CYCLES;
    }
    uint8_t durs[4];
    breathe_phase_durations(pat, durs);
    const uint32_t cycle_s =
        (uint32_t)durs[0] + (uint32_t)durs[1] +
        (uint32_t)durs[2] + (uint32_t)durs[3];

    reset_active_fields(s);
    s->mode = JR_DESK_BREATHE;
    s->started_ms = now_ms;
    s->duration_ms = (uint32_t)cycles * cycle_s * 1000u;
    s->silent = silent;
    s->dnd_silent = silent;
    s->breathe_pattern = pat;
    s->breathe_cycles_total = cycles;
    s->breathe_cycle_cur = 0u;
    s->breathe_phase = JR_BREATHE_INHALE;
    s->breathe_phase_pct = 0u;
    s->breathe_sec_left = durs[0];
    sanitize_label("BREATHE", s->label, "BREATHE");
}

void jr_desk_tick(jr_desk_state_t *s, uint32_t now_ms, uint16_t day_key,
                  jr_desk_tick_t *out)
{
    jr_desk_tick_t tick;
    memset(&tick, 0, sizeof(tick));

    if (s == NULL) {
        if (out != NULL) {
            *out = tick;
        }
        return;
    }

    /* Midnight rollover check before crediting any completing focus sprint. */
    if (day_key != 0u && s->tally_day_key != day_key) {
        if (s->tally_day_key != 0u) {
            s->focus_sessions_today = 0u;
            s->focus_minutes_today = 0u;
        }
        s->tally_day_key = day_key;
    }

    const int32_t diff = (int32_t)(now_ms - s->started_ms);
    const uint32_t elapsed_ms = diff > 0 ? (uint32_t)diff : 0u;

    switch (s->mode) {
    case JR_DESK_NORMAL:
        break;

    case JR_DESK_FOCUS: {
        if (s->duration_ms == 0u || elapsed_ms >= s->duration_ms) {
            if (s->focus_sessions_today < 255u) {
                s->focus_sessions_today++;
            }
            const uint32_t add_min = s->duration_ms / 60000u;
            const uint32_t tot_min = (uint32_t)s->focus_minutes_today + add_min;
            s->focus_minutes_today =
                tot_min > 65535u ? 65535u : (uint16_t)tot_min;
            tick.completed = true;
            tick.completed_mode = JR_DESK_FOCUS;
            tick.rem_s = 0u;
            tick.pct_left = 0u;
            reset_active_fields(s);
        } else {
            const uint32_t rem_ms = s->duration_ms - elapsed_ms;
            tick.rem_s = (rem_ms + 999u) / 1000u;
            uint32_t pct = (uint32_t)(((uint64_t)rem_ms * 100u) /
                                      (uint64_t)s->duration_ms);
            if (pct > 100u) {
                pct = 100u;
            }
            tick.pct_left = (uint8_t)pct;
        }
        break;
    }

    case JR_DESK_ON_AIR: {
        if (s->duration_ms == 0u) {
            /* Open-ended ON_AIR beacon: remains active with no countdown. */
            break;
        }
        if (elapsed_ms >= s->duration_ms) {
            tick.rem_s = 0u;
            tick.pct_left = 0u;
            if (!s->on_air_completed_fired) {
                s->on_air_completed_fired = true;
                s->warn_5m_fired = true;
                tick.completed = true;
                tick.completed_mode = JR_DESK_ON_AIR;
            }
            /* Stays in JR_DESK_ON_AIR until user taps/dismisses. */
        } else {
            const uint32_t rem_ms = s->duration_ms - elapsed_ms;
            tick.rem_s = (rem_ms + 999u) / 1000u;
            uint32_t pct = (uint32_t)(((uint64_t)rem_ms * 100u) /
                                      (uint64_t)s->duration_ms);
            if (pct > 100u) {
                pct = 100u;
            }
            tick.pct_left = (uint8_t)pct;

            if (!s->warn_5m_fired &&
                s->duration_ms >= JR_DESK_ON_AIR_WARN_MIN_DUR_S * 1000u &&
                rem_ms <= JR_DESK_ON_AIR_WARN_S * 1000u) {
                s->warn_5m_fired = true;
                tick.warn_5m = true;
            }
        }
        break;
    }

    case JR_DESK_BREATHE: {
        uint8_t durs[4];
        breathe_phase_durations(s->breathe_pattern, durs);
        const uint32_t cycle_ms =
            ((uint32_t)durs[0] + (uint32_t)durs[1] +
             (uint32_t)durs[2] + (uint32_t)durs[3]) * 1000u;

        if (s->duration_ms == 0u || cycle_ms == 0u ||
            elapsed_ms >= s->duration_ms) {
            tick.completed = true;
            tick.completed_mode = JR_DESK_BREATHE;
            tick.rem_s = 0u;
            tick.pct_left = 0u;
            reset_active_fields(s);
        } else {
            const uint32_t rem_ms = s->duration_ms - elapsed_ms;
            tick.rem_s = (rem_ms + 999u) / 1000u;
            uint32_t pct = (uint32_t)(((uint64_t)rem_ms * 100u) /
                                      (uint64_t)s->duration_ms);
            if (pct > 100u) {
                pct = 100u;
            }
            tick.pct_left = (uint8_t)pct;

            const uint8_t cur_cycle = (uint8_t)(elapsed_ms / cycle_ms);
            uint32_t in_cycle_ms = elapsed_ms % cycle_ms;

            jr_breathe_phase_t new_phase = JR_BREATHE_INHALE;
            uint32_t phase_elapsed_ms = 0u;
            uint32_t phase_dur_ms = (uint32_t)durs[0] * 1000u;

            for (unsigned p = 0u; p < 4u; ++p) {
                if (durs[p] == 0u) {
                    continue;
                }
                const uint32_t p_ms = (uint32_t)durs[p] * 1000u;
                if (in_cycle_ms < p_ms) {
                    new_phase = (jr_breathe_phase_t)p;
                    phase_elapsed_ms = in_cycle_ms;
                    phase_dur_ms = p_ms;
                    break;
                }
                in_cycle_ms -= p_ms;
            }

            uint32_t phase_pct = phase_dur_ms > 0u
                ? (phase_elapsed_ms * 256u) / phase_dur_ms
                : 0u;
            if (phase_pct > 255u) {
                phase_pct = 255u;
            }
            const uint32_t phase_rem_ms = phase_dur_ms > phase_elapsed_ms
                ? (phase_dur_ms - phase_elapsed_ms)
                : 0u;
            const uint8_t sec_left = (uint8_t)((phase_rem_ms + 999u) / 1000u);

            if (new_phase != s->breathe_phase ||
                cur_cycle != s->breathe_cycle_cur) {
                tick.breathe_phase_changed = true;
            }
            s->breathe_cycle_cur = cur_cycle;
            s->breathe_phase = new_phase;
            s->breathe_phase_pct = (uint8_t)phase_pct;
            s->breathe_sec_left = sec_left;
        }
        break;
    }

    default:
        reset_active_fields(s);
        break;
    }

    if (out != NULL) {
        *out = tick;
    }
}

size_t jr_desk_tally_pack(const jr_desk_state_t *s, uint8_t *buf, size_t cap)
{
    if (s == NULL || buf == NULL || cap < JR_DESK_TALLY_PACK_SIZE) {
        return 0u;
    }
    put_u32_le(buf + 0u, JR_DESK_TALLY_MAGIC);
    buf[4] = (uint8_t)JR_DESK_TALLY_VERSION;
    buf[5] = s->focus_sessions_today;
    put_u16_le(buf + 6u, s->tally_day_key);
    put_u16_le(buf + 8u, s->focus_minutes_today);
    buf[10] = 0u;
    buf[11] = 0u;
    const uint32_t csum = fnv1a_32(buf, 12u);
    put_u32_le(buf + 12u, csum);
    return JR_DESK_TALLY_PACK_SIZE;
}

bool jr_desk_tally_unpack(jr_desk_state_t *s, const uint8_t *buf, size_t len)
{
    if (s == NULL || buf == NULL || len < JR_DESK_TALLY_PACK_SIZE) {
        return false;
    }
    if (get_u32_le(buf + 0u) != JR_DESK_TALLY_MAGIC ||
        buf[4] != (uint8_t)JR_DESK_TALLY_VERSION) {
        return false;
    }
    const uint32_t expect = fnv1a_32(buf, 12u);
    if (get_u32_le(buf + 12u) != expect) {
        return false;
    }
    s->focus_sessions_today = buf[5];
    s->tally_day_key = get_u16_le(buf + 6u);
    s->focus_minutes_today = get_u16_le(buf + 8u);
    return true;
}

uint16_t jr_desk_make_day_key(uint16_t year, uint8_t month, uint8_t day)
{
    if (year < 2000u || year > 2127u ||
        month < 1u || month > 12u ||
        day < 1u || day > 31u) {
        return 0u;
    }
    return (uint16_t)(((uint32_t)(year - 2000u) << 9u) |
                      ((uint32_t)month << 5u) |
                      (uint32_t)day);
}

const char *jr_desk_mode_name(jr_desk_mode_t m)
{
    switch (m) {
    case JR_DESK_ON_AIR:  return "ON AIR";
    case JR_DESK_FOCUS:   return "FOCUS";
    case JR_DESK_BREATHE: return "BREATHE";
    case JR_DESK_NORMAL:
    default:              return "NORMAL";
    }
}

const char *jr_breathe_pattern_name(jr_breathe_pattern_t p)
{
    switch (p) {
    case JR_BREATHE_CALM:     return "CALM";
    case JR_BREATHE_ENERGIZE: return "ENERGIZE";
    case JR_BREATHE_BOX:
    default:                  return "BOX";
    }
}

const char *jr_breathe_phase_name(jr_breathe_phase_t p)
{
    switch (p) {
    case JR_BREATHE_HOLD_IN:  return "HOLD";
    case JR_BREATHE_EXHALE:   return "EXHALE";
    case JR_BREATHE_HOLD_OUT: return "HOLD";
    case JR_BREATHE_INHALE:
    default:                  return "INHALE";
    }
}
