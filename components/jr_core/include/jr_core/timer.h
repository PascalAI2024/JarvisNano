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
#define JR_STOPWATCH_MAX_LAPS       4u
#define JR_TIMER_SNAP_MAGIC         0x4A525431u /* "JRT1" */
#define JR_TIMER_SNAP_VERSION       1u
#define JR_TIMER_SLEEP_MIN_REM_S    30u      /* only sleep if timer has >30 s left */
#define JR_TIMER_SLEEP_WAKE_LEAD_S  5u       /* wake 5 s before expiry */
#define JR_TIMER_SLEEP_WAKE_MIN_S   5u       /* minimum clamped sleep wake */
#define JR_TIMER_RECENT_EXPIRE_S    60u      /* still ring if expired <=60 s ago */

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
    uint16_t total_s;                     /* total duration seconds */
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

/* ---- Deep-sleep RTC timer snapshot & restore (N16.4) ------------------- */

typedef struct {
    uint8_t  state;                       /* 0=idle, 1=running, 2=ringing */
    uint8_t  reserved[3];
    uint32_t duration_s;
    uint32_t remaining_s;                 /* relative fallback if no wall clock */
    int64_t  end_epoch_s;                 /* wall-clock expiry epoch (>0 if valid) */
    char     label[JR_TIMER_LABEL_CAP];
} jr_timer_snap_slot_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    int64_t  saved_epoch_s;               /* wall-clock epoch at save (0 if unset) */
    uint32_t wake_delay_s;                /* planned sleep wake delay in seconds */
    uint8_t  count;
    uint8_t  reserved[3];
    jr_timer_snap_slot_t slots[JR_TIMER_MAX];
    uint32_t checksum;                    /* FNV-1a 32-bit over preceding bytes */
} jr_timer_snapshot_t;

/* FNV-1a 32-bit checksum over all fields before `checksum`. */
uint32_t jr_timer_snapshot_checksum(const jr_timer_snapshot_t *snap);

/* True when magic, version, count, and FNV-1a checksum all match. */
bool jr_timer_snapshot_valid(const jr_timer_snapshot_t *snap);

/* Serialize active/ringing timers into `out_snap` with FNV-1a checksum.
 * Returns true if at least one timer was saved; still writes a valid empty
 * snapshot (count=0) when no timers are active so stale RTC memory is cleared. */
bool jr_timer_snapshot_save(const jr_timer_bank_t *bank, uint32_t now_ms,
                            int64_t now_epoch_s, uint32_t wake_delay_s,
                            jr_timer_snapshot_t *out_snap);

/* Restore live or just-expired (<=60 s ago) timers from a validated snapshot.
 * Uses wall-clock delta when both `now_epoch_s` and `snap->saved_epoch_s` are
 * valid (>0 and non-regressing), else falls back to `elapsed_fallback_s`.
 * Just-expired timers restore with end_ms == now_ms so the next step rings.
 * Returns the number of timers restored (0 on invalid/corrupt snapshot). */
unsigned jr_timer_snapshot_restore(jr_timer_bank_t *bank,
                                   const jr_timer_snapshot_t *snap,
                                   uint32_t now_ms, int64_t now_epoch_s,
                                   uint32_t elapsed_fallback_s);

/* Compute clamped deep-sleep wake duration when timers are active:
 *   - If no active timers or any timer is ringing -> returns `requested_s`.
 *   - If nearest timer has >30 s remaining -> clamps to min(requested_s,
 *     max(5, remaining_s - 5)) so the chip wakes right before expiry. */
uint32_t jr_timer_sleep_wake_s(const jr_timer_bank_t *bank, uint32_t now_ms,
                               uint32_t requested_s);

/* True if any timer is ringing or has <= JR_TIMER_SLEEP_MIN_REM_S left
 * (device should stay awake rather than deep-sleeping right before expiry). */
bool jr_timer_blocks_sleep(const jr_timer_bank_t *bank, uint32_t now_ms);

/* ---- Chronograph stopwatch with up to 4 laps --------------------------- */

typedef struct {
    bool     active;
    bool     running;
    uint32_t start_ms;
    uint32_t accum_ms;
    uint32_t laps_ms[JR_STOPWATCH_MAX_LAPS];
    uint8_t  lap_count;
} jr_stopwatch_t;

void jr_stopwatch_init(jr_stopwatch_t *sw);
void jr_stopwatch_start(jr_stopwatch_t *sw, uint32_t now_ms);
void jr_stopwatch_stop(jr_stopwatch_t *sw, uint32_t now_ms);
bool jr_stopwatch_lap(jr_stopwatch_t *sw, uint32_t now_ms,
                      uint32_t *out_lap_ms);
void jr_stopwatch_reset(jr_stopwatch_t *sw);
uint32_t jr_stopwatch_elapsed_ms(const jr_stopwatch_t *sw, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_TIMER_H */
