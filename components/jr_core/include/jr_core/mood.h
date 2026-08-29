/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_mood — four-mood rest ladder. Pure: no drivers, no FreeRTOS.
 *
 * AWAKE   full face, voice wanted
 * AMBIENT still + idle: dim, clock, still listening
 * WHISPER longer rest: quieter glass, voice off
 * DREAM   asleep glow, voice off; lift / tap / motion wakes
 *
 * Face-down is immediate DREAM (privacy). Busy voice (listen/think/speak/ask)
 * holds AWAKE. Deep-sleep and rail gating stay out — this only names the
 * product state so the composition root can dim, clock, and disarm.
 */
#ifndef JR_CORE_MOOD_H
#define JR_CORE_MOOD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rest thresholds, in ms of continuous stillness.
 *
 * The split that matters is AMBIENT vs the rest: AMBIENT still ARMS VOICE, so
 * it is purely cosmetic (dim only) and is safe to reach quickly. WHISPER and
 * DREAM switch the microphone OFF, so they are a real loss of function and
 * remain far out. The first tuning shipped 33 s to voice-off, which made a
 * desk assistant stop listening while its owner read one email. */
#define JR_MOOD_AMBIENT_MS  20000u  /* 20 s — dim, STILL LISTENING */
#define JR_MOOD_WHISPER_MS 300000u  /* 5 min — mic off */
#define JR_MOOD_DREAM_MS   900000u  /* 15 min — deep rest */

typedef enum {
    JR_MOOD_AWAKE = 0,
    JR_MOOD_AMBIENT,
    JR_MOOD_WHISPER,
    JR_MOOD_DREAM,
} jr_mood_t;

typedef struct {
    jr_mood_t mood;
    uint32_t still_since_ms;
    uint32_t last_change_ms;
} jr_mood_state_t;

typedef struct {
    uint32_t now_ms;
    bool face_down;
    bool moving;
    bool user_busy;
} jr_mood_in_t;

typedef struct {
    jr_mood_t mood;
    uint8_t brightness; /* 0..100 panel target */
    bool clock_on;
    bool voice_armed;
    bool changed;
} jr_mood_out_t;

void jr_mood_reset(jr_mood_state_t *s, uint32_t now_ms);
void jr_mood_poke_awake(jr_mood_state_t *s, uint32_t now_ms);
jr_mood_out_t jr_mood_step(jr_mood_state_t *s, const jr_mood_in_t *in);
const char *jr_mood_name(jr_mood_t mood);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_MOOD_H */
