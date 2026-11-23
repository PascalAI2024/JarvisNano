/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/timer.c — pure countdown & Pomodoro timer bank. No ESP-IDF.
 */
#include "jr_core/timer.h"

#include <ctype.h>
#include <string.h>

static bool time_reached(uint32_t now_ms, uint32_t target_ms)
{
    return (int32_t)(now_ms - target_ms) >= 0;
}

static uint32_t slot_rem_ms(const jr_timer_slot_t *slot, uint32_t now_ms)
{
    if (slot == NULL || slot->state != JR_TIMER_RUNNING) {
        return 0u;
    }
    const int32_t diff = (int32_t)(slot->end_ms - now_ms);
    return diff > 0 ? (uint32_t)diff : 0u;
}

static void normalize_label(const char *src, char out[JR_TIMER_LABEL_CAP])
{
    out[0] = '\0';
    if (src == NULL) {
        return;
    }
    while (*src != '\0' && isspace((unsigned char)*src)) {
        ++src;
    }
    size_t len = 0u;
    bool prev_space = false;
    for (; *src != '\0' && len + 1u < JR_TIMER_LABEL_CAP; ++src) {
        const unsigned char ch = (unsigned char)*src;
        if (isspace(ch)) {
            if (!prev_space && len > 0u) {
                out[len++] = ' ';
                prev_space = true;
            }
            continue;
        }
        if (ch >= 0x20u && ch <= 0x7Eu && ch != '"' && ch != '\\') {
            out[len++] = (char)toupper(ch);
            prev_space = false;
        }
    }
    while (len > 0u && out[len - 1u] == ' ') {
        --len;
    }
    out[len] = '\0';
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0u;
    for (; src[i] != '\0' && i + 1u < cap; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

void jr_timer_init(jr_timer_bank_t *bank)
{
    if (bank == NULL) {
        return;
    }
    memset(bank, 0, sizeof(*bank));
}

bool jr_timer_set(jr_timer_bank_t *bank, uint32_t now_ms, uint32_t seconds,
                  const char *label, char *out_label, size_t out_label_cap,
                  uint32_t *out_seconds)
{
    if (bank == NULL) {
        return false;
    }
    char norm[JR_TIMER_LABEL_CAP];
    normalize_label(label, norm);

    if (seconds == 0u) {
        if (strcmp(norm, "FOCUS") == 0) {
            seconds = JR_TIMER_FOCUS_DEFAULT_S;
        } else if (strcmp(norm, "BREAK") == 0) {
            seconds = JR_TIMER_BREAK_DEFAULT_S;
        }
    }
    if (seconds < JR_TIMER_MIN_S || seconds > JR_TIMER_MAX_S) {
        return false;
    }
    if (norm[0] == '\0') {
        copy_str(norm, sizeof(norm), "TIMER");
    }

    jr_timer_slot_t *free_slot = NULL;
    for (unsigned i = 0u; i < JR_TIMER_MAX; ++i) {
        if (bank->slots[i].state == JR_TIMER_IDLE) {
            free_slot = &bank->slots[i];
            break;
        }
    }
    if (free_slot == NULL) {
        return false;
    }

    const uint32_t dur_ms = seconds * 1000u;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->state = JR_TIMER_RUNNING;
    free_slot->started_ms = now_ms;
    free_slot->duration_ms = dur_ms;
    free_slot->end_ms = now_ms + dur_ms;
    copy_str(free_slot->label, sizeof(free_slot->label), norm);

    if (out_label != NULL && out_label_cap > 0u) {
        copy_str(out_label, out_label_cap, free_slot->label);
    }
    if (out_seconds != NULL) {
        *out_seconds = seconds;
    }
    return true;
}

static int find_nearest_index(const jr_timer_bank_t *bank, uint32_t now_ms)
{
    if (bank == NULL) {
        return -1;
    }
    for (unsigned i = 0u; i < JR_TIMER_MAX; ++i) {
        if (bank->slots[i].state == JR_TIMER_RINGING) {
            return (int)i;
        }
    }
    int best = -1;
    uint32_t best_rem = 0u;
    for (unsigned i = 0u; i < JR_TIMER_MAX; ++i) {
        if (bank->slots[i].state == JR_TIMER_RUNNING) {
            const uint32_t rem = slot_rem_ms(&bank->slots[i], now_ms);
            if (best < 0 || rem < best_rem) {
                best = (int)i;
                best_rem = rem;
            }
        }
    }
    return best;
}

unsigned jr_timer_cancel(jr_timer_bank_t *bank, uint32_t now_ms,
                         const char *label)
{
    if (bank == NULL) {
        return 0u;
    }
    char norm[JR_TIMER_LABEL_CAP];
    normalize_label(label, norm);

    if (norm[0] == '\0') {
        const int idx = find_nearest_index(bank, now_ms);
        if (idx < 0) {
            return 0u;
        }
        memset(&bank->slots[idx], 0, sizeof(bank->slots[idx]));
        return 1u;
    }

    const bool all = strcmp(norm, "ALL") == 0;
    unsigned cancelled = 0u;
    for (unsigned i = 0u; i < JR_TIMER_MAX; ++i) {
        if (bank->slots[i].state == JR_TIMER_IDLE) {
            continue;
        }
        if (all || strcmp(bank->slots[i].label, norm) == 0) {
            memset(&bank->slots[i], 0, sizeof(bank->slots[i]));
            cancelled++;
        }
    }
    return cancelled;
}

unsigned jr_timer_dismiss_ringing(jr_timer_bank_t *bank)
{
    if (bank == NULL) {
        return 0u;
    }
    unsigned dismissed = 0u;
    for (unsigned i = 0u; i < JR_TIMER_MAX; ++i) {
        if (bank->slots[i].state == JR_TIMER_RINGING) {
            memset(&bank->slots[i], 0, sizeof(bank->slots[i]));
            dismissed++;
        }
    }
    return dismissed;
}

void jr_timer_step(jr_timer_bank_t *bank, uint32_t now_ms,
                   jr_timer_events_t *out_events)
{
    jr_timer_events_t ev;
    memset(&ev, 0, sizeof(ev));

    if (bank != NULL) {
        for (unsigned i = 0u; i < JR_TIMER_MAX; ++i) {
            jr_timer_slot_t *s = &bank->slots[i];
            if (s->state == JR_TIMER_RUNNING) {
                if (time_reached(now_ms, s->end_ms)) {
                    s->state = JR_TIMER_RINGING;
                    s->ring_start_ms = now_ms;
                    s->next_chime_ms = now_ms + JR_TIMER_CHIME_INTERVAL_MS;
                    if (!ev.expired) {
                        copy_str(ev.expired_label, sizeof(ev.expired_label),
                                 s->label);
                    }
                    ev.expired = true;
                    ev.chime = true;
                }
            } else if (s->state == JR_TIMER_RINGING) {
                if (time_reached(now_ms,
                                 s->ring_start_ms + JR_TIMER_RING_AUTO_STOP_MS)) {
                    memset(s, 0, sizeof(*s));
                    ev.auto_stopped = true;
                } else if (time_reached(now_ms, s->next_chime_ms)) {
                    ev.chime = true;
                    s->next_chime_ms = now_ms + JR_TIMER_CHIME_INTERVAL_MS;
                }
            }
        }
    }

    if (out_events != NULL) {
        *out_events = ev;
    }
}

uint16_t jr_timer_slot_rem_s(const jr_timer_slot_t *slot, uint32_t now_ms)
{
    if (slot == NULL || slot->state != JR_TIMER_RUNNING) {
        return 0u;
    }
    const uint32_t rem_ms = slot_rem_ms(slot, now_ms);
    uint32_t sec = (rem_ms + 999u) / 1000u;
    if (sec > JR_TIMER_MAX_S) {
        sec = JR_TIMER_MAX_S;
    }
    return (uint16_t)sec;
}

bool jr_timer_nearest(const jr_timer_bank_t *bank, uint32_t now_ms,
                      jr_timer_view_t *out_view)
{
    jr_timer_view_t view;
    memset(&view, 0, sizeof(view));

    const int idx = find_nearest_index(bank, now_ms);
    if (idx < 0) {
        if (out_view != NULL) {
            *out_view = view;
        }
        return false;
    }

    const jr_timer_slot_t *s = &bank->slots[idx];
    view.active = true;
    view.ringing = (s->state == JR_TIMER_RINGING);
    const uint32_t tot_s = (s->duration_ms + 999u) / 1000u;
    view.total_s = (uint16_t)(tot_s > JR_TIMER_MAX_S ? JR_TIMER_MAX_S : tot_s);
    copy_str(view.label, sizeof(view.label), s->label);
    if (view.ringing) {
        view.rem_s = 0u;
        view.pct_left = 0u;
    } else {
        const uint32_t rem_ms = slot_rem_ms(s, now_ms);
        view.rem_s = jr_timer_slot_rem_s(s, now_ms);
        uint32_t pct = s->duration_ms > 0u
            ? (uint32_t)(((uint64_t)rem_ms * 100u) / (uint64_t)s->duration_ms)
            : 0u;
        if (pct > 100u) {
            pct = 100u;
        }
        view.pct_left = (uint8_t)pct;
    }

    if (out_view != NULL) {
        *out_view = view;
    }
    return true;
}

bool jr_timer_any_active(const jr_timer_bank_t *bank)
{
    return jr_timer_active_count(bank) > 0u;
}

bool jr_timer_any_ringing(const jr_timer_bank_t *bank)
{
    if (bank == NULL) {
        return false;
    }
    for (unsigned i = 0u; i < JR_TIMER_MAX; ++i) {
        if (bank->slots[i].state == JR_TIMER_RINGING) {
            return true;
        }
    }
    return false;
}

unsigned jr_timer_active_count(const jr_timer_bank_t *bank)
{
    if (bank == NULL) {
        return 0u;
    }
    unsigned n = 0u;
    for (unsigned i = 0u; i < JR_TIMER_MAX; ++i) {
        if (bank->slots[i].state != JR_TIMER_IDLE) {
            n++;
        }
    }
    return n;
}

/* ---- Deep-sleep RTC timer snapshot & restore (N16.4) ------------------- */

uint32_t jr_timer_snapshot_checksum(const jr_timer_snapshot_t *snap)
{
    if (snap == NULL) {
        return 0u;
    }
    const uint8_t *p = (const uint8_t *)snap;
    const size_t n = offsetof(jr_timer_snapshot_t, checksum);
    uint32_t h = 2166136261u; /* FNV-1a 32-bit offset basis */
    for (size_t i = 0u; i < n; ++i) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

bool jr_timer_snapshot_valid(const jr_timer_snapshot_t *snap)
{
    if (snap == NULL) {
        return false;
    }
    if (snap->magic != JR_TIMER_SNAP_MAGIC ||
        snap->version != JR_TIMER_SNAP_VERSION ||
        snap->count > JR_TIMER_MAX) {
        return false;
    }
    return snap->checksum == jr_timer_snapshot_checksum(snap);
}

bool jr_timer_snapshot_save(const jr_timer_bank_t *bank, uint32_t now_ms,
                            int64_t now_epoch_s, uint32_t wake_delay_s,
                            jr_timer_snapshot_t *out_snap)
{
    if (out_snap == NULL) {
        return false;
    }
    memset(out_snap, 0, sizeof(*out_snap));
    out_snap->magic = JR_TIMER_SNAP_MAGIC;
    out_snap->version = JR_TIMER_SNAP_VERSION;
    out_snap->saved_epoch_s = now_epoch_s > 0 ? now_epoch_s : 0;
    out_snap->wake_delay_s = wake_delay_s;

    uint8_t out_idx = 0u;
    if (bank != NULL) {
        for (unsigned i = 0u; i < JR_TIMER_MAX && out_idx < JR_TIMER_MAX; ++i) {
            const jr_timer_slot_t *s = &bank->slots[i];
            if (s->state == JR_TIMER_IDLE) {
                continue;
            }
            jr_timer_snap_slot_t *dst = &out_snap->slots[out_idx++];
            dst->state = (s->state == JR_TIMER_RINGING) ? 2u : 1u;
            uint32_t dur_s = (s->duration_ms + 999u) / 1000u;
            if (dur_s < JR_TIMER_MIN_S) {
                dur_s = JR_TIMER_MIN_S;
            } else if (dur_s > JR_TIMER_MAX_S) {
                dur_s = JR_TIMER_MAX_S;
            }
            dst->duration_s = dur_s;
            if (s->state == JR_TIMER_RINGING) {
                dst->remaining_s = 0u;
                dst->end_epoch_s = (now_epoch_s > 0) ? now_epoch_s : 0;
            } else {
                const uint32_t rem_s = jr_timer_slot_rem_s(s, now_ms);
                dst->remaining_s = rem_s;
                dst->end_epoch_s = (now_epoch_s > 0)
                    ? (now_epoch_s + (int64_t)rem_s) : 0;
            }
            copy_str(dst->label, sizeof(dst->label), s->label);
        }
    }
    out_snap->count = out_idx;
    out_snap->checksum = jr_timer_snapshot_checksum(out_snap);
    return out_idx > 0u;
}

unsigned jr_timer_snapshot_restore(jr_timer_bank_t *bank,
                                   const jr_timer_snapshot_t *snap,
                                   uint32_t now_ms, int64_t now_epoch_s,
                                   uint32_t elapsed_fallback_s)
{
    if (bank == NULL || !jr_timer_snapshot_valid(snap)) {
        return 0u;
    }
    jr_timer_init(bank);
    const bool use_epoch = (now_epoch_s > 0 && snap->saved_epoch_s > 0 &&
                            now_epoch_s >= snap->saved_epoch_s);
    unsigned restored = 0u;

    for (unsigned i = 0u; i < snap->count && restored < JR_TIMER_MAX; ++i) {
        const jr_timer_snap_slot_t *src = &snap->slots[i];
        if (src->state != 1u && src->state != 2u) {
            continue;
        }
        bool live = false;
        uint32_t rem_s = 0u;
        if (src->state == 2u) {
            /* Was ringing before sleep: expire immediately on first step. */
            live = true;
            rem_s = 0u;
        } else if (use_epoch && src->end_epoch_s > 0) {
            const int64_t diff = src->end_epoch_s - now_epoch_s;
            if (diff > 0) {
                live = true;
                rem_s = (diff > (int64_t)JR_TIMER_MAX_S)
                    ? JR_TIMER_MAX_S : (uint32_t)diff;
            } else if (diff >= -(int64_t)JR_TIMER_RECENT_EXPIRE_S) {
                live = true;
                rem_s = 0u;
            }
        } else {
            if (src->remaining_s > elapsed_fallback_s) {
                live = true;
                rem_s = src->remaining_s - elapsed_fallback_s;
                if (rem_s > JR_TIMER_MAX_S) {
                    rem_s = JR_TIMER_MAX_S;
                }
            } else if (elapsed_fallback_s - src->remaining_s <=
                       JR_TIMER_RECENT_EXPIRE_S) {
                live = true;
                rem_s = 0u;
            }
        }
        if (!live) {
            continue;
        }

        uint32_t dur_s = src->duration_s;
        if (dur_s < rem_s) {
            dur_s = rem_s;
        }
        if (dur_s < JR_TIMER_MIN_S) {
            dur_s = JR_TIMER_MIN_S;
        } else if (dur_s > JR_TIMER_MAX_S) {
            dur_s = JR_TIMER_MAX_S;
        }

        char norm[JR_TIMER_LABEL_CAP];
        normalize_label(src->label, norm);
        if (norm[0] == '\0') {
            copy_str(norm, sizeof(norm), "TIMER");
        }

        jr_timer_slot_t *dst = &bank->slots[restored++];
        memset(dst, 0, sizeof(*dst));
        dst->state = JR_TIMER_RUNNING;
        dst->duration_ms = dur_s * 1000u;
        dst->end_ms = now_ms + rem_s * 1000u;
        dst->started_ms = dst->end_ms - dst->duration_ms;
        copy_str(dst->label, sizeof(dst->label), norm);
    }
    return restored;
}

uint32_t jr_timer_sleep_wake_s(const jr_timer_bank_t *bank, uint32_t now_ms,
                               uint32_t requested_s)
{
    if (bank == NULL || jr_timer_any_ringing(bank)) {
        return requested_s;
    }
    jr_timer_view_t view;
    if (!jr_timer_nearest(bank, now_ms, &view) || !view.active || view.ringing) {
        return requested_s;
    }
    if (view.rem_s <= JR_TIMER_SLEEP_MIN_REM_S) {
        return requested_s;
    }
    uint32_t wake_s = view.rem_s > JR_TIMER_SLEEP_WAKE_LEAD_S
        ? (uint32_t)view.rem_s - JR_TIMER_SLEEP_WAKE_LEAD_S
        : JR_TIMER_SLEEP_WAKE_MIN_S;
    if (wake_s < JR_TIMER_SLEEP_WAKE_MIN_S) {
        wake_s = JR_TIMER_SLEEP_WAKE_MIN_S;
    }
    if (requested_s == 0u || wake_s < requested_s) {
        return wake_s;
    }
    return requested_s;
}

bool jr_timer_blocks_sleep(const jr_timer_bank_t *bank, uint32_t now_ms)
{
    if (bank == NULL) {
        return false;
    }
    if (jr_timer_any_ringing(bank)) {
        return true;
    }
    jr_timer_view_t view;
    if (!jr_timer_nearest(bank, now_ms, &view) || !view.active) {
        return false;
    }
    return view.rem_s <= JR_TIMER_SLEEP_MIN_REM_S;
}

/* ---- Chronograph stopwatch with up to 4 laps --------------------------- */

void jr_stopwatch_init(jr_stopwatch_t *sw)
{
    if (sw == NULL) {
        return;
    }
    memset(sw, 0, sizeof(*sw));
}

void jr_stopwatch_start(jr_stopwatch_t *sw, uint32_t now_ms)
{
    if (sw == NULL) {
        return;
    }
    sw->active = true;
    if (!sw->running) {
        sw->start_ms = now_ms;
        sw->running = true;
    }
}

void jr_stopwatch_stop(jr_stopwatch_t *sw, uint32_t now_ms)
{
    if (sw == NULL || !sw->active || !sw->running) {
        return;
    }
    sw->accum_ms += (now_ms - sw->start_ms);
    sw->running = false;
}

uint32_t jr_stopwatch_elapsed_ms(const jr_stopwatch_t *sw, uint32_t now_ms)
{
    if (sw == NULL || !sw->active) {
        return 0u;
    }
    if (sw->running) {
        return sw->accum_ms + (now_ms - sw->start_ms);
    }
    return sw->accum_ms;
}

bool jr_stopwatch_lap(jr_stopwatch_t *sw, uint32_t now_ms,
                      uint32_t *out_lap_ms)
{
    if (sw == NULL || !sw->active || sw->lap_count >= JR_STOPWATCH_MAX_LAPS) {
        return false;
    }
    const uint32_t el = jr_stopwatch_elapsed_ms(sw, now_ms);
    sw->laps_ms[sw->lap_count++] = el;
    if (out_lap_ms != NULL) {
        *out_lap_ms = el;
    }
    return true;
}

void jr_stopwatch_reset(jr_stopwatch_t *sw)
{
    jr_stopwatch_init(sw);
}
