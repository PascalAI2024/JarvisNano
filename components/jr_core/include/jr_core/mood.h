/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_mood — four-mood rest ladder. Pure: no drivers, no FreeRTOS.
 *
 * AWAKE   full face, voice wanted
 * AMBIENT still + idle: dim, clock, still listening
 * WHISPER longer rest: quieter glass, Gemini session closed; the local
 *         wake watch still listens
 * DREAM   asleep glow, session closed; lift / tap / motion / "Jarvis" wakes
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
 * DREAM close the Gemini session and stop uplinking audio, so conversation
 * costs a reconnect and they remain far out. The first tuning shipped 33 s to
 * voice-off, which made a desk assistant stop listening while its owner read
 * one email.
 *
 * THESE MOODS DO NOT TURN THE MICROPHONE OFF, and this header used to say
 * three times that they did. The codec keeps sampling and WakeNet keeps
 * running locally, which is exactly what lets "Jarvis" still wake the device
 * from DREAM. Only PRIVACY silences the microphone — the glass hold, the flip
 * latch, or an explicit disarm. Rest is a power and attention state; privacy
 * is a capability state. Writing rest down as if it were privacy is the kind
 * of comment someone later trusts instead of reading the code. */
#define JR_MOOD_AMBIENT_MS  20000u  /* 20 s — dim, STILL LISTENING */
#define JR_MOOD_WHISPER_MS 300000u  /* 5 min — session closed, wake armed */
#define JR_MOOD_DREAM_MS   900000u  /* 15 min — deep rest, wake armed */
/* DREAM this long, on battery, and the chip itself sleeps (main.c owns the
 * world: no USB, no update, no companion). Ten minutes past DREAM is 25 min
 * of stillness face-up, or 10 min face-down. */
#define JR_MOOD_SLEEP_MS   600000u
/* BATTERY SAVER: below the low-cell line on battery every wait above is
 * divided by this, so a device that is running out rests in seconds, closes
 * its session in a minute or two, and sleeps in six instead of twenty-five. */
#define JR_MOOD_SAVER_DIV  4u
/* QUIET: the microphone is off by the owner's hand (privacy), so there is
 * nothing to listen for and the glass becomes a watch: WHISPER after this
 * much stillness, AMBIENT skipped (it exists to keep listening). Motion, a
 * touch and a busy phase still light it, so a muted device that is asking
 * or answering stays readable — the mistake this replaces was mute forcing
 * DREAM at brightness 8 mid-question. DREAM and sleep keep their own clocks. */
#define JR_MOOD_QUIET_MS    5000u

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
    bool saver;             /* the saver ladder is in force (input, kept) */
} jr_mood_state_t;

typedef struct {
    uint32_t now_ms;
    bool face_down;
    bool moving;
    bool user_busy;
    bool saver;             /* low cell on battery: every wait / SAVER_DIV */
    bool quiet;             /* privacy mute: rest as a watch after QUIET_MS */
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

/* True once the ladder has sat in DREAM for JR_MOOD_SLEEP_MS. Pure: it says
 * nothing about power or links, which the composition root adds. */
bool jr_mood_sleep_due(const jr_mood_state_t *s, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_MOOD_H */
