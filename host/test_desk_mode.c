/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_desk_mode.c — unit tests for jr_core/desk_mode (pure C11,
 * no ESP-IDF, no heap, no libm).
 */
#include "unity.h"
#include "jr_core/desk_mode.h"
#include <string.h>

static void test_desk_init_names_and_day_key(void)
{
    jr_desk_state_t s;
    jr_desk_init(&s);
    TEST_ASSERT_EQUAL_INT(JR_DESK_NORMAL, s.mode);
    TEST_ASSERT_FALSE(s.silent);
    TEST_ASSERT_FALSE(s.dnd_silent);
    TEST_ASSERT_EQUAL_UINT16(0u, s.tally_day_key);
    TEST_ASSERT_EQUAL_UINT8(0u, s.focus_sessions_today);
    TEST_ASSERT_EQUAL_UINT16(0u, s.focus_minutes_today);

    TEST_ASSERT_EQUAL_STRING("NORMAL", jr_desk_mode_name(JR_DESK_NORMAL));
    TEST_ASSERT_EQUAL_STRING("ON AIR", jr_desk_mode_name(JR_DESK_ON_AIR));
    TEST_ASSERT_EQUAL_STRING("FOCUS", jr_desk_mode_name(JR_DESK_FOCUS));
    TEST_ASSERT_EQUAL_STRING("BREATHE", jr_desk_mode_name(JR_DESK_BREATHE));

    TEST_ASSERT_EQUAL_STRING("BOX", jr_breathe_pattern_name(JR_BREATHE_BOX));
    TEST_ASSERT_EQUAL_STRING("CALM", jr_breathe_pattern_name(JR_BREATHE_CALM));
    TEST_ASSERT_EQUAL_STRING("ENERGIZE", jr_breathe_pattern_name(JR_BREATHE_ENERGIZE));

    TEST_ASSERT_EQUAL_STRING("INHALE", jr_breathe_phase_name(JR_BREATHE_INHALE));
    TEST_ASSERT_EQUAL_STRING("HOLD", jr_breathe_phase_name(JR_BREATHE_HOLD_IN));
    TEST_ASSERT_EQUAL_STRING("EXHALE", jr_breathe_phase_name(JR_BREATHE_EXHALE));
    TEST_ASSERT_EQUAL_STRING("HOLD", jr_breathe_phase_name(JR_BREATHE_HOLD_OUT));

    const uint16_t d1 = jr_desk_make_day_key(2026u, 11u, 23u);
    const uint16_t d2 = jr_desk_make_day_key(2026u, 11u, 24u);
    TEST_ASSERT_NOT_EQUAL(0u, d1);
    TEST_ASSERT_NOT_EQUAL(d1, d2);
    TEST_ASSERT_EQUAL_UINT16(0u, jr_desk_make_day_key(1999u, 12u, 31u));
    TEST_ASSERT_EQUAL_UINT16(0u, jr_desk_make_day_key(2026u, 0u, 10u));
    TEST_ASSERT_EQUAL_UINT16(0u, jr_desk_make_day_key(2026u, 13u, 10u));
    TEST_ASSERT_EQUAL_UINT16(0u, jr_desk_make_day_key(2026u, 11u, 0u));

    /* NULL safety */
    jr_desk_init(NULL);
    jr_desk_set_normal(NULL);
    jr_desk_start_on_air(NULL, 0u, 60u, "call");
    jr_desk_start_focus(NULL, 0u, 60u, "work", false);
    jr_desk_start_breathe(NULL, 0u, JR_BREATHE_BOX, 4u, false);
    jr_desk_tick_t t;
    jr_desk_tick(NULL, 1000u, d1, &t);
    TEST_ASSERT_FALSE(t.completed);
}

static void test_desk_on_air_open_ended_and_timed_warnings(void)
{
    jr_desk_state_t s;
    jr_desk_init(&s);
    jr_desk_tick_t t;

    /* Open-ended ON_AIR (duration_s == 0): always silent + dnd_silent, never warns/completes */
    jr_desk_start_on_air(&s, 1000u, 0u, "  \"Zoom\"   Call  ");
    TEST_ASSERT_EQUAL_INT(JR_DESK_ON_AIR, s.mode);
    TEST_ASSERT_TRUE(s.silent);
    TEST_ASSERT_TRUE(s.dnd_silent);
    TEST_ASSERT_EQUAL_UINT32(0u, s.duration_ms);
    TEST_ASSERT_EQUAL_STRING("Zoom Call", s.label);

    jr_desk_tick(&s, 1000u + 3600000u, 0u, &t);
    TEST_ASSERT_EQUAL_INT(JR_DESK_ON_AIR, s.mode);
    TEST_ASSERT_FALSE(t.warn_5m);
    TEST_ASSERT_FALSE(t.completed);
    TEST_ASSERT_EQUAL_UINT32(0u, t.rem_s);

    /* Short timed ON_AIR (< 600 s): NO 5-minute warning, completes once and stays ON_AIR */
    jr_desk_start_on_air(&s, 10000u, 300u, NULL);
    TEST_ASSERT_EQUAL_STRING("ON AIR", s.label);
    jr_desk_tick(&s, 10000u + 150000u, 0u, &t);
    TEST_ASSERT_FALSE(t.warn_5m);
    TEST_ASSERT_FALSE(t.completed);
    TEST_ASSERT_EQUAL_UINT32(150u, t.rem_s);
    TEST_ASSERT_EQUAL_UINT8(50u, t.pct_left);

    jr_desk_tick(&s, 10000u + 300000u, 0u, &t);
    TEST_ASSERT_FALSE(t.warn_5m);
    TEST_ASSERT_TRUE(t.completed);
    TEST_ASSERT_EQUAL_INT(JR_DESK_ON_AIR, t.completed_mode);
    TEST_ASSERT_EQUAL_INT(JR_DESK_ON_AIR, s.mode);
    TEST_ASSERT_TRUE(s.dnd_silent);

    /* Subsequent ticks stay ON_AIR without re-firing completed */
    jr_desk_tick(&s, 10000u + 305000u, 0u, &t);
    TEST_ASSERT_FALSE(t.completed);
    TEST_ASSERT_EQUAL_INT(JR_DESK_ON_AIR, s.mode);

    /* Long timed ON_AIR (1800 s = 30 min): fires warn_5m ONCE at 300 s remaining */
    jr_desk_start_on_air(&s, 5000u, 1800u, "Design Review");
    jr_desk_tick(&s, 5000u + 1499999u, 0u, &t);
    TEST_ASSERT_FALSE(t.warn_5m);
    TEST_ASSERT_EQUAL_UINT32(301u, t.rem_s);

    jr_desk_tick(&s, 5000u + 1500000u, 0u, &t);
    TEST_ASSERT_TRUE(t.warn_5m);
    TEST_ASSERT_FALSE(t.completed);
    TEST_ASSERT_EQUAL_UINT32(300u, t.rem_s);

    jr_desk_tick(&s, 5000u + 1501000u, 0u, &t);
    TEST_ASSERT_FALSE(t.warn_5m);
    TEST_ASSERT_EQUAL_UINT32(299u, t.rem_s);

    jr_desk_tick(&s, 5000u + 1800000u, 0u, &t);
    TEST_ASSERT_TRUE(t.completed);
    TEST_ASSERT_EQUAL_INT(JR_DESK_ON_AIR, t.completed_mode);
    TEST_ASSERT_EQUAL_INT(JR_DESK_ON_AIR, s.mode);

    /* Explicit tap/dismiss returns to NORMAL */
    jr_desk_set_normal(&s);
    TEST_ASSERT_EQUAL_INT(JR_DESK_NORMAL, s.mode);
    TEST_ASSERT_FALSE(s.dnd_silent);
    TEST_ASSERT_FALSE(s.silent);
}

static void test_desk_focus_completion_tally_and_midnight_rollover(void)
{
    jr_desk_state_t s;
    jr_desk_init(&s);
    jr_desk_tick_t t;
    const uint16_t day1 = jr_desk_make_day_key(2026u, 11u, 23u);
    const uint16_t day2 = jr_desk_make_day_key(2026u, 11u, 24u);

    /* Default focus duration == 25 min (1500 s), audible (silent=false) */
    jr_desk_start_focus(&s, 1000u, 0u, NULL, false);
    TEST_ASSERT_EQUAL_INT(JR_DESK_FOCUS, s.mode);
    TEST_ASSERT_EQUAL_UINT32(1500000u, s.duration_ms);
    TEST_ASSERT_EQUAL_STRING("FOCUS", s.label);
    TEST_ASSERT_FALSE(s.silent);
    TEST_ASSERT_FALSE(s.dnd_silent);

    /* Mid-sprint check */
    jr_desk_tick(&s, 1000u + 750000u, day1, &t);
    TEST_ASSERT_FALSE(t.completed);
    TEST_ASSERT_EQUAL_UINT32(750u, t.rem_s);
    TEST_ASSERT_EQUAL_UINT8(50u, t.pct_left);
    TEST_ASSERT_EQUAL_UINT16(day1, s.tally_day_key);

    /* Complete sprint 1 -> increments tally and exits to NORMAL */
    jr_desk_tick(&s, 1000u + 1500000u, day1, &t);
    TEST_ASSERT_TRUE(t.completed);
    TEST_ASSERT_EQUAL_INT(JR_DESK_FOCUS, t.completed_mode);
    TEST_ASSERT_EQUAL_INT(JR_DESK_NORMAL, s.mode);
    TEST_ASSERT_EQUAL_UINT8(1u, s.focus_sessions_today);
    TEST_ASSERT_EQUAL_UINT16(25u, s.focus_minutes_today);

    /* Second silent sprint (45 min) accumulates cleanly */
    jr_desk_start_focus(&s, 2000000u, 2700u, "Deep Code", true);
    TEST_ASSERT_TRUE(s.silent);
    TEST_ASSERT_TRUE(s.dnd_silent);
    TEST_ASSERT_EQUAL_STRING("Deep Code", s.label);
    jr_desk_tick(&s, 2000000u + 2700000u, day1, &t);
    TEST_ASSERT_TRUE(t.completed);
    TEST_ASSERT_EQUAL_INT(JR_DESK_NORMAL, s.mode);
    TEST_ASSERT_EQUAL_UINT8(2u, s.focus_sessions_today);
    TEST_ASSERT_EQUAL_UINT16(70u, s.focus_minutes_today);

    /* day_key == 0 (clock unavailable) does NOT wipe tally */
    jr_desk_tick(&s, 5000000u, 0u, &t);
    TEST_ASSERT_EQUAL_UINT8(2u, s.focus_sessions_today);
    TEST_ASSERT_EQUAL_UINT16(70u, s.focus_minutes_today);
    TEST_ASSERT_EQUAL_UINT16(day1, s.tally_day_key);

    /* Midnight rollover to day2 resets tally */
    jr_desk_tick(&s, 5001000u, day2, &t);
    TEST_ASSERT_EQUAL_UINT16(day2, s.tally_day_key);
    TEST_ASSERT_EQUAL_UINT8(0u, s.focus_sessions_today);
    TEST_ASSERT_EQUAL_UINT16(0u, s.focus_minutes_today);

    /* set_normal / switching modes preserves tally */
    jr_desk_start_focus(&s, 6000000u, 600u, "Sprint", false);
    jr_desk_tick(&s, 6000000u + 600000u, day2, &t);
    TEST_ASSERT_EQUAL_UINT8(1u, s.focus_sessions_today);
    TEST_ASSERT_EQUAL_UINT16(10u, s.focus_minutes_today);
    jr_desk_start_on_air(&s, 7000000u, 600u, "Standup");
    jr_desk_set_normal(&s);
    TEST_ASSERT_EQUAL_UINT8(1u, s.focus_sessions_today);
    TEST_ASSERT_EQUAL_UINT16(10u, s.focus_minutes_today);
}

static void test_desk_breathe_box_calm_and_energize(void)
{
    jr_desk_state_t s;
    jr_desk_init(&s);
    jr_desk_tick_t t;

    /* BOX (4-4-4-4, 16 s/cycle), cycles=0 -> defaults to 4 cycles (64 s) */
    jr_desk_start_breathe(&s, 1000u, JR_BREATHE_BOX, 0u, false);
    TEST_ASSERT_EQUAL_INT(JR_DESK_BREATHE, s.mode);
    TEST_ASSERT_EQUAL_UINT8(4u, s.breathe_cycles_total);
    TEST_ASSERT_EQUAL_UINT32(64000u, s.duration_ms);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_INHALE, s.breathe_phase);
    TEST_ASSERT_EQUAL_UINT8(4u, s.breathe_sec_left);

    /* t = +0 ms: INHALE, pct = 0, sec_left = 4, not changed yet */
    jr_desk_tick(&s, 1000u, 0u, &t);
    TEST_ASSERT_FALSE(t.breathe_phase_changed);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_INHALE, s.breathe_phase);
    TEST_ASSERT_EQUAL_UINT8(0u, s.breathe_phase_pct);
    TEST_ASSERT_EQUAL_UINT8(4u, s.breathe_sec_left);

    /* t = +2000 ms: midpoint of 4s INHALE -> pct = 128, sec_left = 2 */
    jr_desk_tick(&s, 3000u, 0u, &t);
    TEST_ASSERT_FALSE(t.breathe_phase_changed);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_INHALE, s.breathe_phase);
    TEST_ASSERT_EQUAL_UINT8(128u, s.breathe_phase_pct);
    TEST_ASSERT_EQUAL_UINT8(2u, s.breathe_sec_left);

    /* t = +4000 ms: transitions to HOLD_IN */
    jr_desk_tick(&s, 5000u, 0u, &t);
    TEST_ASSERT_TRUE(t.breathe_phase_changed);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_HOLD_IN, s.breathe_phase);
    TEST_ASSERT_EQUAL_UINT8(0u, s.breathe_phase_pct);
    TEST_ASSERT_EQUAL_UINT8(4u, s.breathe_sec_left);

    /* t = +8000 ms: transitions to EXHALE */
    jr_desk_tick(&s, 9000u, 0u, &t);
    TEST_ASSERT_TRUE(t.breathe_phase_changed);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_EXHALE, s.breathe_phase);

    /* t = +12000 ms: transitions to HOLD_OUT */
    jr_desk_tick(&s, 13000u, 0u, &t);
    TEST_ASSERT_TRUE(t.breathe_phase_changed);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_HOLD_OUT, s.breathe_phase);
    TEST_ASSERT_EQUAL_UINT8(0u, s.breathe_cycle_cur);

    /* t = +16000 ms: cycle 1 INHALE */
    jr_desk_tick(&s, 17000u, 0u, &t);
    TEST_ASSERT_TRUE(t.breathe_phase_changed);
    TEST_ASSERT_EQUAL_UINT8(1u, s.breathe_cycle_cur);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_INHALE, s.breathe_phase);

    /* t = +64000 ms: auto-exits to NORMAL with completed=true */
    jr_desk_tick(&s, 65000u, 0u, &t);
    TEST_ASSERT_TRUE(t.completed);
    TEST_ASSERT_EQUAL_INT(JR_DESK_BREATHE, t.completed_mode);
    TEST_ASSERT_EQUAL_INT(JR_DESK_NORMAL, s.mode);

    /* CALM (4-7-8-0, 19 s/cycle, 2 cycles = 38 s): skips HOLD_OUT cleanly */
    jr_desk_start_breathe(&s, 0u, JR_BREATHE_CALM, 2u, true);
    TEST_ASSERT_TRUE(s.silent);
    TEST_ASSERT_EQUAL_UINT32(38000u, s.duration_ms);

    jr_desk_tick(&s, 4000u, 0u, &t);
    TEST_ASSERT_TRUE(t.breathe_phase_changed);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_HOLD_IN, s.breathe_phase);
    TEST_ASSERT_EQUAL_UINT8(7u, s.breathe_sec_left);

    jr_desk_tick(&s, 11000u, 0u, &t);
    TEST_ASSERT_TRUE(t.breathe_phase_changed);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_EXHALE, s.breathe_phase);
    TEST_ASSERT_EQUAL_UINT8(8u, s.breathe_sec_left);

    /* At 19000 ms wraps directly to cycle 1 INHALE (no HOLD_OUT) */
    jr_desk_tick(&s, 19000u, 0u, &t);
    TEST_ASSERT_TRUE(t.breathe_phase_changed);
    TEST_ASSERT_EQUAL_UINT8(1u, s.breathe_cycle_cur);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_INHALE, s.breathe_phase);

    jr_desk_tick(&s, 38000u, 0u, &t);
    TEST_ASSERT_TRUE(t.completed);
    TEST_ASSERT_EQUAL_INT(JR_DESK_NORMAL, s.mode);

    /* ENERGIZE (4-2-4-0, 10 s/cycle, 3 cycles = 30 s) */
    jr_desk_start_breathe(&s, 100u, JR_BREATHE_ENERGIZE, 3u, false);
    TEST_ASSERT_EQUAL_UINT32(30000u, s.duration_ms);
    jr_desk_tick(&s, 100u + 4000u, 0u, &t);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_HOLD_IN, s.breathe_phase);
    TEST_ASSERT_EQUAL_UINT8(2u, s.breathe_sec_left);
    jr_desk_tick(&s, 100u + 6000u, 0u, &t);
    TEST_ASSERT_EQUAL_INT(JR_BREATHE_EXHALE, s.breathe_phase);
    TEST_ASSERT_EQUAL_UINT8(4u, s.breathe_sec_left);
    jr_desk_tick(&s, 100u + 30000u, 0u, &t);
    TEST_ASSERT_TRUE(t.completed);
    TEST_ASSERT_EQUAL_INT(JR_DESK_NORMAL, s.mode);
}

static void test_desk_clock_wrap_and_tally_pack_unpack(void)
{
    jr_desk_state_t s;
    jr_desk_init(&s);
    jr_desk_tick_t t;
    const uint16_t day = jr_desk_make_day_key(2026u, 11u, 24u);

    /* Wrap across 0xFFFFFFFF -> 0x00000000 in FOCUS */
    const uint32_t start = 0xFFFF0000u;
    jr_desk_start_focus(&s, start, 120u, "Wrap Sprint", false);
    jr_desk_tick(&s, start + 60000u, day, &t);
    TEST_ASSERT_FALSE(t.completed);
    TEST_ASSERT_EQUAL_UINT32(60u, t.rem_s);
    TEST_ASSERT_EQUAL_UINT8(50u, t.pct_left);

    jr_desk_tick(&s, start + 120000u, day, &t);
    TEST_ASSERT_TRUE(t.completed);
    TEST_ASSERT_EQUAL_INT(JR_DESK_NORMAL, s.mode);
    TEST_ASSERT_EQUAL_UINT8(1u, s.focus_sessions_today);
    TEST_ASSERT_EQUAL_UINT16(2u, s.focus_minutes_today);

    /* Pack daily tally */
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(0u, jr_desk_tally_pack(&s, buf, JR_DESK_TALLY_PACK_SIZE - 1u));
    const size_t n = jr_desk_tally_pack(&s, buf, sizeof buf);
    TEST_ASSERT_EQUAL_UINT(JR_DESK_TALLY_PACK_SIZE, n);

    /* Unpack into an active ON_AIR state does NOT clobber the active mode */
    jr_desk_state_t s2;
    jr_desk_init(&s2);
    jr_desk_start_on_air(&s2, 500u, 600u, "Live");
    TEST_ASSERT_TRUE(jr_desk_tally_unpack(&s2, buf, n));
    TEST_ASSERT_EQUAL_INT(JR_DESK_ON_AIR, s2.mode);
    TEST_ASSERT_EQUAL_STRING("Live", s2.label);
    TEST_ASSERT_EQUAL_UINT16(day, s2.tally_day_key);
    TEST_ASSERT_EQUAL_UINT8(1u, s2.focus_sessions_today);
    TEST_ASSERT_EQUAL_UINT16(2u, s2.focus_minutes_today);

    /* Corruption / short buffer / bad version rejected cleanly */
    jr_desk_state_t clean;
    jr_desk_init(&clean);
    TEST_ASSERT_FALSE(jr_desk_tally_unpack(&clean, buf, n - 1u));

    uint8_t bad[32];
    memcpy(bad, buf, n);
    bad[5] ^= 0x01u; /* flip sessions byte -> checksum mismatch */
    TEST_ASSERT_FALSE(jr_desk_tally_unpack(&clean, bad, n));
    TEST_ASSERT_EQUAL_UINT8(0u, clean.focus_sessions_today);

    memcpy(bad, buf, n);
    bad[4] = 99u; /* bad version */
    TEST_ASSERT_FALSE(jr_desk_tally_unpack(&clean, bad, n));

    memcpy(bad, buf, n);
    bad[0] ^= 0xFFu; /* bad magic */
    TEST_ASSERT_FALSE(jr_desk_tally_unpack(&clean, bad, n));
}

void desk_mode_tests_run(void)
{
    RUN_TEST(test_desk_init_names_and_day_key);
    RUN_TEST(test_desk_on_air_open_ended_and_timed_warnings);
    RUN_TEST(test_desk_focus_completion_tally_and_midnight_rollover);
    RUN_TEST(test_desk_breathe_box_calm_and_energize);
    RUN_TEST(test_desk_clock_wrap_and_tally_pack_unpack);
}
