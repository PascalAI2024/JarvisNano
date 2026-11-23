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
