/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/desk_mode.h — pure unified Desk Presence, Focus & Breathe state
 * machine (no ESP-IDF, no heap, no libm).
 *
 * Manages the Glass 3.5 Desk Copilot overlay modes without adding carousel
 * screens:
 *   - JR_DESK_NORMAL  : idle/default desk state
 *   - JR_DESK_ON_AIR  : silent crimson-coral meeting/call beacon (always
 *                       dnd_silent + silent; duration_s == 0 is open-ended;
 *                       timed emits a once-only 5-min warning if >=10 min and
 *                       stays ON_AIR after 0 until dismissed)
 *   - JR_DESK_FOCUS   : deep-work sprint (default 25 min) with NVS-serializable
 *                       daily session/minute tally and midnight rollover
 *   - JR_DESK_BREATHE : guided breathing cadence (BOX 4-4-4-4, CALM 4-7-8,
 *                       ENERGIZE 4-2-4) that auto-exits to NORMAL on finish
 *
 * All timing uses wrap-safe uint32_t monotonic millisecond arithmetic.
 */
#ifndef JR_CORE_DESK_MODE_H
#define JR_CORE_DESK_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_DESK_LABEL_CAP              24u      /* 23 ASCII chars + NUL */
#define JR_DESK_FOCUS_DEFAULT_S        1500u    /* 25 min default focus sprint */
#define JR_DESK_BREATHE_DEFAULT_CYCLES 4u       /* 4 default breath cycles */
#define JR_DESK_ON_AIR_WARN_S          300u     /* T-5:00 silent warning */
#define JR_DESK_ON_AIR_WARN_MIN_DUR_S  600u     /* warn only if >= 10 min */
#define JR_DESK_MAX_DURATION_S         43200u   /* 12 hours max duration */

#define JR_DESK_TALLY_MAGIC            0x4A524431u /* "JRD1" */
#define JR_DESK_TALLY_VERSION          1u
#define JR_DESK_TALLY_PACK_SIZE        16u

typedef enum {
    JR_DESK_NORMAL = 0,
    JR_DESK_ON_AIR,
    JR_DESK_FOCUS,
    JR_DESK_BREATHE,
} jr_desk_mode_t;

typedef enum {
    JR_BREATHE_BOX = 0,       /* 4s inhale / 4s hold / 4s exhale / 4s hold (16s) */
    JR_BREATHE_CALM,          /* 4s inhale / 7s hold / 8s exhale / 0s hold (19s) */
    JR_BREATHE_ENERGIZE,      /* 4s inhale / 2s hold / 4s exhale / 0s hold (10s) */
} jr_breathe_pattern_t;

typedef enum {
    JR_BREATHE_INHALE = 0,
    JR_BREATHE_HOLD_IN,
    JR_BREATHE_EXHALE,
    JR_BREATHE_HOLD_OUT,
} jr_breathe_phase_t;

typedef struct {
    jr_desk_mode_t       mode;
    uint32_t             started_ms;
    uint32_t             duration_ms;
    bool                 silent;
    bool                 dnd_silent;
    char                 label[JR_DESK_LABEL_CAP];

    jr_breathe_pattern_t breathe_pattern;
    uint8_t              breathe_cycles_total;
    uint8_t              breathe_cycle_cur;
    jr_breathe_phase_t   breathe_phase;
    uint8_t              breathe_phase_pct;     /* 0..255 progress within phase */
    uint8_t              breathe_sec_left;      /* whole seconds left in phase */

    /* Daily NVS-serializable focus tally. */
    uint16_t             tally_day_key;
    uint8_t              focus_sessions_today;
    uint16_t             focus_minutes_today;

    /* Internal edge-latch flags for once-only ON_AIR notifications. */
    bool                 warn_5m_fired;
    bool                 on_air_completed_fired;
} jr_desk_state_t;

typedef struct {
    bool           completed;
    bool           warn_5m;
    bool           breathe_phase_changed;
    jr_desk_mode_t completed_mode;
    uint32_t       rem_s;
    uint8_t        pct_left;                    /* 0..100 */
} jr_desk_tick_t;

/* Initialize state to JR_DESK_NORMAL with zeroed daily tally. */
void jr_desk_init(jr_desk_state_t *s);

/* Return to JR_DESK_NORMAL while preserving the daily focus tally. */
void jr_desk_set_normal(jr_desk_state_t *s);

/* Enter ON_AIR presence mode. Always forces dnd_silent=true and silent=true.
 * duration_s == 0 starts an open-ended call beacon (never warns or completes).
 * Durations above JR_DESK_MAX_DURATION_S are clamped. NULL/empty label defaults
 * to "ON AIR". Preserves the daily focus tally. */
void jr_desk_start_on_air(jr_desk_state_t *s, uint32_t now_ms,
                          uint32_t duration_s, const char *label);

/* Enter FOCUS deep-work mode. duration_s == 0 defaults to 1500 s (25 min).
 * Durations above JR_DESK_MAX_DURATION_S are clamped. NULL/empty label defaults
 * to "FOCUS". Preserves the daily focus tally. */
void jr_desk_start_focus(jr_desk_state_t *s, uint32_t now_ms,
                         uint32_t duration_s, const char *label, bool silent);

/* Enter guided BREATHE mode. cycles == 0 defaults to 4 cycles. Invalid pattern
 * values fall back to JR_BREATHE_BOX. Preserves the daily focus tally. */
void jr_desk_start_breathe(jr_desk_state_t *s, uint32_t now_ms,
                           jr_breathe_pattern_t pat, uint8_t cycles,
                           bool silent);

/* Advance the desk state machine to `now_ms`.
 * - Midnight rollover: when day_key != 0 and s->tally_day_key != day_key:
 *     if s->tally_day_key != 0, resets focus_sessions_today and
 *     focus_minutes_today to 0; updates s->tally_day_key = day_key.
 * - FOCUS expiry: increments focus_sessions_today, adds duration_ms / 60000 to
 *     focus_minutes_today, sets completed=true / completed_mode=JR_DESK_FOCUS,
 *     and transitions back to JR_DESK_NORMAL.
 * - ON_AIR timed: sets warn_5m once when crossing <=300 s remaining (only if
 *     duration >= 600 s); sets completed=true / completed_mode=JR_DESK_ON_AIR
 *     once at 0 while staying in JR_DESK_ON_AIR until dismissed.
 * - BREATHE: updates breathe_phase, breathe_phase_pct (0..255),
 *     breathe_sec_left, breathe_cycle_cur, and breathe_phase_changed; auto-exits
 *     to JR_DESK_NORMAL with completed=true after breathe_cycles_total. */
void jr_desk_tick(jr_desk_state_t *s, uint32_t now_ms, uint16_t day_key,
                  jr_desk_tick_t *out);

/* Serialize the daily focus tally (magic + version + day_key + sessions +
 * minutes + FNV-1a checksum) into `buf`. Returns JR_DESK_TALLY_PACK_SIZE (16)
 * on success, or 0 if s/buf is NULL or cap < JR_DESK_TALLY_PACK_SIZE. */
size_t jr_desk_tally_pack(const jr_desk_state_t *s, uint8_t *buf, size_t cap);

/* Deserialize and validate a packed daily focus tally into `s` without
 * disturbing the current active mode. Returns true on valid magic, version,
 * and checksum; leaves `s` untouched on failure. */
bool jr_desk_tally_unpack(jr_desk_state_t *s, const uint8_t *buf, size_t len);

/* Encode a calendar date (year >= 2000, month 1..12, day 1..31) into a compact
 * non-zero uint16_t day key suitable for jr_desk_tick(). Returns 0 if invalid. */
uint16_t jr_desk_make_day_key(uint16_t year, uint8_t month, uint8_t day);

/* Human-readable constant names for display/JSON/logging. */
const char *jr_desk_mode_name(jr_desk_mode_t m);
const char *jr_breathe_pattern_name(jr_breathe_pattern_t p);
const char *jr_breathe_phase_name(jr_breathe_phase_t p);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_DESK_MODE_H */
