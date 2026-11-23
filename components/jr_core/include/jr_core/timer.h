/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/timer.h — pure countdown & Pomodoro timer bank (no ESP-IDF).
 *
 * Up to JR_TIMER_MAX (4) concurrent timers, 5 s .. 43200 s (12 h), with
 * 16-char uppercase labels. Normalizes "focus" -> 25 min and "break" -> 5 min
 * when seconds is omitted/0. Wrap-safe uint32_t monotonic ms arithmetic.
 * Ringing timers emit a chime immediately on expiry, repeat every 4 s, and
 * auto-stop after 60 s if not dismissed.
 */
#ifndef JR_CORE_TIMER_H
#define JR_CORE_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_TIMER_MAX                4u
#define JR_TIMER_MIN_S              5u
#define JR_TIMER_MAX_S              43200u   /* 12 hours */
#define JR_TIMER_LABEL_CAP          17u      /* 16 glyphs + NUL */
#define JR_TIMER_RING_AUTO_STOP_MS  60000u   /* 60 s auto-stop once ringing */
#define JR_TIMER_CHIME_INTERVAL_MS  4000u    /* 4 s periodic chime while ringing */
#define JR_TIMER_FOCUS_DEFAULT_S    1500u    /* 25 min Pomodoro focus */
#define JR_TIMER_BREAK_DEFAULT_S    300u     /* 5 min Pomodoro break */

typedef enum {
    JR_TIMER_IDLE = 0,
    JR_TIMER_RUNNING,
    JR_TIMER_RINGING,
} jr_timer_state_t;

typedef struct {
    jr_timer_state_t state;
    uint32_t started_ms;
    uint32_t duration_ms;
    uint32_t end_ms;
    uint32_t ring_start_ms;
    uint32_t next_chime_ms;
    char     label[JR_TIMER_LABEL_CAP];
} jr_timer_slot_t;

typedef struct {
    jr_timer_slot_t slots[JR_TIMER_MAX];
} jr_timer_bank_t;

typedef struct {
    bool     active;
    bool     ringing;
    uint16_t rem_s;                       /* remaining seconds (0..43200) */
    uint8_t  pct_left;                    /* 0..100 */
    char     label[JR_TIMER_LABEL_CAP];
} jr_timer_view_t;

typedef struct {
    bool expired;                         /* >=1 timer expired this step */
    bool chime;                           /* sound alarm chime this step */
    bool auto_stopped;                    /* >=1 ringing timer hit 60 s limit */
    char expired_label[JR_TIMER_LABEL_CAP];
} jr_timer_events_t;

/* Reset all slots to IDLE. */
void jr_timer_init(jr_timer_bank_t *bank);

/* Start a timer. Normalizes `label` to <=16 uppercase ASCII chars:
 *   - "focus" with seconds==0 -> 1500 s (25 min)
 *   - "break" with seconds==0 -> 300 s (5 min)
 *   - empty/NULL label -> "TIMER"
 * Rejects durations outside [JR_TIMER_MIN_S, JR_TIMER_MAX_S] or a full bank.
 * If out_label != NULL, writes the normalized label. If out_seconds != NULL,
 * writes the effective duration in seconds. */
bool jr_timer_set(jr_timer_bank_t *bank, uint32_t now_ms, uint32_t seconds,
                  const char *label, char *out_label, size_t out_label_cap,
                  uint32_t *out_seconds);

/* Cancel timer(s):
 *   - label == NULL or "" -> cancel the nearest active/ringing timer
 *   - normalized label == "ALL" -> cancel every active/ringing timer
 *   - otherwise -> cancel every active/ringing timer matching normalized label
 * Returns the number of timers cancelled. */
unsigned jr_timer_cancel(jr_timer_bank_t *bank, uint32_t now_ms,
                         const char *label);

/* Dismiss all RINGING timers (leaves RUNNING timers untouched).
 * Returns the number of ringing timers dismissed. */
unsigned jr_timer_dismiss_ringing(jr_timer_bank_t *bank);

/* Advance all slots to `now_ms`. Reports newly expired timer, periodic 4 s
 * chime trigger while ringing, and 60 s auto-stop. */
void jr_timer_step(jr_timer_bank_t *bank, uint32_t now_ms,
                   jr_timer_events_t *out_events);

/* Snapshot of the highest-priority timer (ringing first, otherwise smallest
 * remaining time). Returns true if any timer is active or ringing. */
bool jr_timer_nearest(const jr_timer_bank_t *bank, uint32_t now_ms,
                      jr_timer_view_t *out_view);

/* Remaining whole seconds for a single slot at `now_ms` (0 if IDLE/RINGING). */
uint16_t jr_timer_slot_rem_s(const jr_timer_slot_t *slot, uint32_t now_ms);

bool jr_timer_any_active(const jr_timer_bank_t *bank);
bool jr_timer_any_ringing(const jr_timer_bank_t *bank);
unsigned jr_timer_active_count(const jr_timer_bank_t *bank);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_TIMER_H */
