/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "jr_core/mood.h"

#include <stddef.h>

static uint8_t mood_brightness(jr_mood_t mood)
{
    switch (mood) {
    case JR_MOOD_AMBIENT: return 48;
    case JR_MOOD_WHISPER: return 22;
    case JR_MOOD_DREAM:   return 8;
    case JR_MOOD_AWAKE:
    default:              return 100;
    }
}

static jr_mood_t mood_from_still(uint32_t still_ms)
{
    if (still_ms >= JR_MOOD_DREAM_MS) {
        return JR_MOOD_DREAM;
    }
    if (still_ms >= JR_MOOD_WHISPER_MS) {
        return JR_MOOD_WHISPER;
    }
    if (still_ms >= JR_MOOD_AMBIENT_MS) {
        return JR_MOOD_AMBIENT;
    }
    return JR_MOOD_AWAKE;
}

static jr_mood_out_t pack(const jr_mood_state_t *s, bool changed)
{
    jr_mood_out_t o;
    o.mood = s->mood;
    o.brightness = mood_brightness(s->mood);
    o.clock_on = (s->mood != JR_MOOD_AWAKE);
    o.voice_armed = (s->mood == JR_MOOD_AWAKE || s->mood == JR_MOOD_AMBIENT);
    o.changed = changed;
    return o;
}

void jr_mood_reset(jr_mood_state_t *s, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }
    s->mood = JR_MOOD_AWAKE;
    s->still_since_ms = now_ms;
    s->last_change_ms = now_ms;
}

void jr_mood_poke_awake(jr_mood_state_t *s, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }
    s->mood = JR_MOOD_AWAKE;
    s->still_since_ms = now_ms;
    s->last_change_ms = now_ms;
}

jr_mood_out_t jr_mood_step(jr_mood_state_t *s, const jr_mood_in_t *in)
{
    jr_mood_state_t local = {0};
    if (s == NULL) {
        s = &local;
        jr_mood_reset(s, in ? in->now_ms : 0);
    }
    if (in == NULL) {
        return pack(s, false);
    }

    const uint32_t now = in->now_ms;
    jr_mood_t next = s->mood;

    if (in->face_down) {
        next = JR_MOOD_DREAM;
        s->still_since_ms = now;
    } else if (in->moving || in->user_busy) {
        next = JR_MOOD_AWAKE;
        s->still_since_ms = now;
    } else {
        uint32_t still = now - s->still_since_ms;
        next = mood_from_still(still);
    }

    const bool changed = (next != s->mood);
    s->mood = next;
    if (changed) {
        s->last_change_ms = now;
    }
    return pack(s, changed);
}

bool jr_mood_sleep_due(const jr_mood_state_t *s, uint32_t now_ms)
{
    return s != NULL && s->mood == JR_MOOD_DREAM &&
           (uint32_t)(now_ms - s->last_change_ms) >= JR_MOOD_SLEEP_MS;
}

const char *jr_mood_name(jr_mood_t mood)
{
    switch (mood) {
    case JR_MOOD_AMBIENT: return "AMBIENT";
    case JR_MOOD_WHISPER: return "WHISPER";
    case JR_MOOD_DREAM:   return "DREAM";
    case JR_MOOD_AWAKE:
    default:              return "AWAKE";
    }
}
