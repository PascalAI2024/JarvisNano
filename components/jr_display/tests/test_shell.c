/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_shell.c — host tests for the spatial shell: navigation state and
 * round-safe geometry.
 *
 * jr_display.c is #included, not linked, so the tests can read the render
 * task's private state (s_nav_word, s_detail_value, the sp_* primitives)
 * without widening the public API for testing's sake. The IDF surface comes
 * from the same stub file the visual probe uses.
 *
 * TWO CLAIMS ARE LOAD-BEARING, and both are made in the header's design
 * comment, where a reader will believe them:
 *
 *   1. "THE SPATIAL SHELL NEVER WRITES A PIXEL BEYOND JR_DISPLAY_SHELL_R_MAX
 *      — not even its backdrop dim", which is what makes it structurally
 *      impossible for the control shade to collide with the gold privacy ring
 *      (r221-222), the battery rim (r215-220) or the choice arcs (r223-231).
 *      test_never_leaves_shell_radius proves it by rendering the whole state
 *      matrix over a poisoned frame and checking that every pixel outside the
 *      circle is untouched.
 *
 *   2. The firmware update ring at r140-154 is drawn on EVERY space, clears
 *      every space's headline beneath it and JR_DISPLAY_SAFE_R above it, and
 *      is the one thing that may appear on JARVIS at rest. Those are
 *      arithmetic claims about glyph corners and a gating claim about the
 *      shell, so test_ota_ring_band and its neighbours measure the actual
 *      drawn pixels instead of trusting the comment.
 *
 * Strip invariance gets the same treatment as hud_render: the presenter blits
 * 12-row strips (466 = 38*12 + 10, ragged tail), so any y-dependent state that
 * leaks between calls shows up on glass as a seam and nowhere else.
 */
#include <stdint.h>
/* Host-only scheduler seam: inject an opposite request exactly before a CAS.
 * The actual intrinsic still executes, with no firmware hook/state/API. */
static void (*s_before_cas)(volatile void *address, uintptr_t desired);
#define __atomic_compare_exchange_n(p,e,d,w,s,f) __extension__ ({ \
    if (s_before_cas) { s_before_cas((volatile void *)(p), (uintptr_t)(d)); } \
    __atomic_compare_exchange_n(p,e,d,w,s,f); })
#include "jr_display.c"
#undef __atomic_compare_exchange_n

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "idf_stubs.inc"

static int g_failures;
static int g_checks;   /* empty is not pass: the summary names the count */

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: ", __func__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define STRIP_ROWS 12   /* must match JR_DISPLAY_STRIP_ROWS in jr_display.c */
#define POISON     0xA55Au
#define TEXT_H     (7 * 2)   /* sp_text_row draws 7*scale rows; scale 2 here */

/* ------------------------------------------------------------------ state -- */

/* The frame step that publishes what the presenter is asked for (the wanted dial,
 * the hands' strength). Tests that change nav, style or the clock by hand and then
 * read watch_dial_face() or draw the watch run it, as the render task would have. */
static uint32_t s_wd_now = 1000000U;
static void wd_settle(void)
{
    for (int i = 0; i < 80; ++i) {
        s_wd_now += 42U;
        sp_fade_tick(s_wd_now, 42U, 8);
    }
}

static void reset_nav(void)
{
    __atomic_store_n(&s_nav_word, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_display.shell_word, 0U, __ATOMIC_RELEASE);
    s_space_black = 0;   /* the scaffolds below stand in for sp_fade_tick's verdicts */
    s_parked = false;
}

/* Publish one weather sample through the public setter, exactly as main.c
 * will. feels/rain/humidity/wind are fixed so the sheet rows are checkable. */
static void set_weather(bool valid, int temp, int hi, int lo,
                        jr_display_sky_t sky, const char *cond,
                        uint32_t fetched_ms)
{
    jr_display_weather_t w;
    memset(&w, 0, sizeof w);
    w.valid = valid;
    w.temp_f = (int16_t)temp;
    w.feels_f = (int16_t)(temp + 2);
    w.hi_f = (int16_t)hi;
    w.lo_f = (int16_t)lo;
    w.rain_pct = 40U;
    w.humidity_pct = 72U;
    w.wind_mph = 12U;
    w.sky = sky;
    strncpy(w.condition, cond, sizeof w.condition - 1U);
    w.fetched_ms = fetched_ms;
    jr_display_weather_set(&w);
}

static void reset_activity(void)
{
    __atomic_store_n(&s_act_count, 0U, __ATOMIC_RELEASE);
    memset(s_act_kind, 0, sizeof s_act_kind);
    memset(s_act_sum, 0, sizeof s_act_sum);
    memset(s_act_ms, 0, sizeof s_act_ms);
    memset(s_act_wall, 0, sizeof s_act_wall);
    s_act_live = 0U;
}

static bool space_is(int space, const char *what)
{
    const bool ok = (int)jr_display_nav_space() == space;
    if (!ok) {
        printf("  (%s: on space %d, wanted %d)\n", what,
               (int)jr_display_nav_space(), space);
    }
    return ok;
}

static void test_space_ring_wraps_both_ways(void)
{
    /* The ring WRAPS (changed 2026-08-29). This test previously asserted the
     * opposite — that PREV off the first screen was an honest no-op, because a
     * wrap made "am I at the end" unanswerable. An endless ring never raises
     * that question: there is no end, and a swipe never dies against a wall.
     * The old assertions are inverted here rather than deleted, so the change
     * of contract stays visible to whoever reads this next.
     *
     * DESK is made live first: this test walks the WHOLE ring, and DESK is on
     * it only while an agent, a claim or a lease is. The shorter dark ring
     * has its own test. */
    reset_nav();
    jr_display_set_shell_state(false, true, 40U, JR_DISPLAY_AGENT_WORKING);
    CHECK(jr_display_nav_space() == JR_DISPLAY_SPACE_JARVIS, "cold start");

    /* Backwards off the first screen lands on the LAST one. */
    jr_display_nav_prev();
    CHECK((int)jr_display_nav_space() == (int)JR_DISPLAY_SPACE_COUNT - 1,
          "prev from first wraps to last");

    /* Forward from the last screen returns to the first. */
    jr_display_nav_next();
    CHECK(jr_display_nav_space() == JR_DISPLAY_SPACE_JARVIS,
          "next from last wraps to first");

    /* A full lap returns exactly where it started — the property that makes
     * "endless" true rather than merely long. */
    for (int i = 0; i < (int)JR_DISPLAY_SPACE_COUNT; ++i) {
        jr_display_nav_next();
    }
    CHECK(jr_display_nav_space() == JR_DISPLAY_SPACE_JARVIS,
          "a full lap forward returns home");

    for (int i = 0; i < (int)JR_DISPLAY_SPACE_COUNT; ++i) {
        jr_display_nav_prev();
    }
    CHECK(jr_display_nav_space() == JR_DISPLAY_SPACE_JARVIS,
          "a full lap backward returns home");

    /* Every screen is reachable going forward — a ring with a gap is a bug
     * that only shows up when the count changes. */
    reset_nav();
    jr_display_set_shell_state(false, true, 40U, JR_DISPLAY_AGENT_WORKING);
    for (int i = 1; i < (int)JR_DISPLAY_SPACE_COUNT; ++i) {
        jr_display_nav_next();
        CHECK((int)jr_display_nav_space() == i,
              "every screen reachable forward");
    }
}

static void test_overlay_axis_is_unambiguous(void)
{
    reset_nav();
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE, "cold overlay");

    /* Up opens DETAIL; up again must NOT toggle it shut, or a hurried second
     * swipe would close the sheet the first one opened. Only DOWN closes. */
    jr_display_nav_up();
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_DETAIL, "up opens");
    jr_display_nav_up();
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_DETAIL, "up is idempotent");
    jr_display_nav_down();
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE, "down closes detail");

    jr_display_nav_down();
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_SHADE, "down opens shade");
    jr_display_nav_down();
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_SHADE, "down is idempotent");
    jr_display_nav_up();
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE, "up closes shade");
}

static void test_sideways_resets_and_home_escapes(void)
{
    /* Sideways navigation always lands on a main view: a space change that
     * kept an overlay up would show one space's sheet over another's centre. */
    reset_nav();
    jr_display_nav_up();
    jr_display_nav_next();
    CHECK(jr_display_nav_space() == JR_DISPLAY_SPACE_WATCH, "moved");
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE, "detail dropped");

    jr_display_nav_down();
    jr_display_nav_prev();
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE, "shade dropped");

    /* A move still closes overlays even when it wraps. Nothing is "clamped"
     * any more, so this asserts the RESET rather than the no-op: the gesture
     * was made, so dropping the overlay is what the user asked for, and where
     * they land is the previous screen on the ring. */
    reset_nav();
    jr_display_nav_up();
    jr_display_nav_prev();
    CHECK((int)jr_display_nav_space() == (int)JR_DISPLAY_SPACE_COUNT - 1,
          "prev from home wraps to the last screen");
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE,
          "clamped swipe still resets");

    jr_display_nav_set(JR_DISPLAY_SPACE_ACTIVITY);
    jr_display_nav_up();
    jr_display_nav_home();
    CHECK(jr_display_nav_space() == JR_DISPLAY_SPACE_JARVIS, "home space");
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE, "home overlay");
}

static void test_transition_publishes_a_new_serial(void)
{
    /* The render task detects a space change by serial, not by comparing
     * spaces, so a move must bump it even when from == to would look equal. */
    reset_nav();
    const uint32_t before = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    jr_display_nav_next();
    const uint32_t after = __atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE);
    CHECK((before >> NAV_SERIAL_SHIFT) != (after >> NAV_SERIAL_SHIFT),
          "serial advanced");
    CHECK(((after >> NAV_PREV_SHIFT) & NAV_SPACE_MASK) ==
              JR_DISPLAY_SPACE_JARVIS,
          "previous space recorded for the slide");
    CHECK((after & NAV_FORWARD_BIT) != 0U, "direction recorded");
}

static void test_legacy_shade_bit_still_opens_the_shade(void)
{
    /* An existing caller that tracks its own shade flag keeps working while
     * gesture routing migrates to the nav API: the two sources are OR-ed. */
    reset_nav();
    CHECK(!sp_shade_open(), "closed");
    jr_display_set_shell_state(true, false, 0, JR_DISPLAY_AGENT_NONE);
    CHECK(sp_shade_open(), "legacy bit opens");
    jr_display_set_shell_state(false, false, 0, JR_DISPLAY_AGENT_NONE);
    CHECK(!sp_shade_open(), "legacy bit closes");

    jr_display_nav_down();
    CHECK(sp_shade_open(), "nav opens");
    jr_display_set_shell_state(true, false, 0, JR_DISPLAY_AGENT_NONE);
    jr_display_nav_up();
    CHECK(sp_shade_open(), "legacy bit still holds it open");
    jr_display_set_shell_state(false, false, 0, JR_DISPLAY_AGENT_NONE);
    CHECK(!sp_shade_open(), "both sources clear");
}

/* -------------------------------------------------------------------- ota -- */

static uint32_t ota_word(void)
{
    return __atomic_load_n(&s_ota_word, __ATOMIC_ACQUIRE);
}

static void test_ota_word_clamps_everything(void)
{
    jr_display_ota_set(JR_DISPLAY_OTA_RECEIVING, 200U, 0U, 1U, true);
    CHECK(((ota_word() >> 8) & 0xFFu) == 100u, "percent clamped");
    CHECK((ota_word() & 0xFu) == JR_DISPLAY_OTA_RECEIVING, "state kept");
    CHECK(((ota_word() >> 16) & 0xFu) == 0u, "active slot");
    CHECK(((ota_word() >> 20) & 0xFu) == 1u, "target slot");
    CHECK((ota_word() & (1u << 24)) != 0u, "preflight set");

    /* Anything that is not a real partition index has to land on the unknown
     * nibble, so the renderer never has to distrust what it reads. */
    jr_display_ota_set(JR_DISPLAY_OTA_IDLE, 0U, 9U, 0xFFU, false);
    CHECK(((ota_word() >> 16) & 0xFu) == OTA_SLOT_NONE, "bad active -> none");
    CHECK(((ota_word() >> 20) & 0xFu) == OTA_SLOT_NONE, "no target -> none");
    CHECK((ota_word() & (1u << 24)) == 0u, "preflight clear");

    jr_display_ota_set((jr_display_ota_state_t)99, 0U, 0U, 0U, false);
    CHECK((ota_word() & 0xFu) == JR_DISPLAY_OTA_IDLE, "bad state -> idle");
}

static void test_ota_arc_semantics(void)
{
    /* A half-filled ring must never appear for a state that is not making
     * progress: only RECEIVING is a measurement. */
    CHECK(sp_ota_fill(JR_DISPLAY_OTA_RECEIVING, 42) == 42, "receiving tracks");
    CHECK(sp_ota_fill(JR_DISPLAY_OTA_RECEIVING, 250) == 100, "clamped high");
    CHECK(sp_ota_fill(JR_DISPLAY_OTA_RECEIVING, -3) == 0, "clamped low");
    CHECK(sp_ota_fill(JR_DISPLAY_OTA_PREFLIGHT, 70) == 0, "preflight is a track");
    CHECK(sp_ota_fill(JR_DISPLAY_OTA_PROBATION, 0) == 100, "probation is full");
    CHECK(sp_ota_fill(JR_DISPLAY_OTA_ROLLED_BACK, 0) == 100, "rollback is full");
    CHECK(sp_ota_fill(JR_DISPLAY_OTA_FAILED, 0) == 100, "failure is full");

    /* The two healthy resting states leave the dial quiet. */
    CHECK(!sp_ota_ringing(JR_DISPLAY_OTA_IDLE), "idle is quiet");
    CHECK(!sp_ota_ringing(JR_DISPLAY_OTA_VALID), "valid is quiet");
    CHECK(sp_ota_ringing(JR_DISPLAY_OTA_PREFLIGHT), "preflight rings");
    CHECK(sp_ota_ringing(JR_DISPLAY_OTA_BLOCKED), "blocked rings");
    CHECK(sp_ota_ringing(JR_DISPLAY_OTA_PROBATION), "probation rings");

    /* Gold is privacy's alone; an update must never borrow it. */
    for (int s = 0; s <= JR_DISPLAY_OTA_ROLLED_BACK; ++s) {
        CHECK(sp_ota_native((jr_display_ota_state_t)s) != SP_C_GOLD,
              "state %d is not gold", s);
    }
}

static void test_ota_names_fit_their_column(void)
{
    /* A state change must not be able to truncate the value column. */
    for (int s = 0; s <= JR_DISPLAY_OTA_ROLLED_BACK; ++s) {
        const char *n = sp_ota_name((jr_display_ota_state_t)s);
        CHECK(strlen(n) < (size_t)SP_COL_MAX, "%s fits the column", n);
        CHECK(strlen(n) < (size_t)SP_LABEL_CAP, "%s fits the headline", n);
    }
}

/* Stage POWER, fully presented, with the given OTA state. POWER is where the
 * update's UPDATE and SLOT rows live now that SETTINGS is gone; the ring
 * itself belongs to no space, and stage_ring_at_rest below stages each one. */
static void stage_power(jr_display_ota_state_t st, uint8_t pct, int overlay)
{
    reset_nav();
    jr_display_nav_set(JR_DISPLAY_SPACE_STATUS);
    s_display.board.width = HUD_W;
    s_display.board.height = HUD_H;
    s_display.board.swap_color_bytes = false;
    s_space_on = true;
    s_space_from = JR_DISPLAY_SPACE_STATUS;
    s_space_to = JR_DISPLAY_SPACE_STATUS;
    s_space_prog = 256;
    s_space_ease = 256;
    s_space_veil = 256;
    s_space_black = 0;
    s_detail_space = JR_DISPLAY_SPACE_STATUS;
    s_detail_ease = overlay == JR_DISPLAY_OVERLAY_DETAIL ? 256 : 0;
    s_shade_ease = overlay == JR_DISPLAY_OVERLAY_SHADE ? 256 : 0;
    __atomic_store_n(&s_nav_word,
                     (uint32_t)JR_DISPLAY_SPACE_STATUS |
                         ((uint32_t)overlay << NAV_OVL_SHIFT),
                     __ATOMIC_RELEASE);
    jr_display_set_status(90U, 0U);
    jr_display_power_set(74U, 4020U, true, true);
    jr_display_ota_set(st, pct, 0U, 1U, true);
    sp_compose();
}

/* Stage any ring screen at rest — no sheet, no shade, no slide — with the
 * given OTA state. JARVIS at rest means the shell is OFF (s_space_on false,
 * no veil), exactly as the presenter leaves it, so whatever this draws on
 * JARVIS is drawn without the shell's help. */
static void stage_ring_at_rest(int space, jr_display_ota_state_t st,
                               uint8_t pct)
{
    stage_power(st, pct, JR_DISPLAY_OVERLAY_NONE);
    s_space_from = (uint8_t)space;
    s_space_to = (uint8_t)space;
    s_detail_space = (uint8_t)space;
    s_space_veil = space == JR_DISPLAY_SPACE_JARVIS ? 0 : 256;
    s_space_on = space != JR_DISPLAY_SPACE_JARVIS;
    __atomic_store_n(&s_nav_word, (uint32_t)space, __ATOMIC_RELEASE);
    __atomic_store_n(&s_hud_env_word, 74U, __ATOMIC_RELEASE);
    sp_compose();
}

static int g_chip_c = 52;   /* the die reading the links helper publishes */

static void links(bool wifi, int rssi, const char *ip, bool link, uint8_t tools,
                  bool desk, bool saving)
{
    jr_display_links_t l = {
        .wifi_up = wifi, .rssi_dbm = (int8_t)rssi, .link_open = link,
        .tools = tools, .desk_live = desk, .radio_saving = saving,
        .chip_c = (int8_t)g_chip_c, .chip_c_valid = g_chip_c > -100,
        .cpu_mhz = (uint16_t)(saving ? 160 : 240),
    };
    strncpy(l.ip, ip, sizeof l.ip - 1);
    jr_display_links_set(&l);
}

/* THE DEVICE IN NINE FACTS. Every row is checked by value, in order, and
 * every value is driven both ways, so a row that stops reading its input
 * (say, RADIO pinned to REALTIME) fails here rather than lying on the glass.
 * stage_power publishes 74 %, 4020 mV, on USB, charging. */
static void test_status_sheet_is_the_device_in_nine_rows(void)
{
    stage_power(JR_DISPLAY_OTA_RECEIVING, 42U, JR_DISPLAY_OVERLAY_DETAIL);
    links(true, -34, "192.0.2.20", false, 0U, false, false);
    sp_compose();
    CHECK(strcmp(s_detail_head, "STATUS") == 0, "status sheet, got '%s'",
          s_detail_head);
    CHECK(s_detail_rows == 9, "nine rows, got %d", s_detail_rows);
    CHECK(s_detail_rows <= SP_ROWS_MAX, "within the row budget");
    static const char *const order[9] = {
        "BATTERY", "POWER", "WIFI", "IP", "LINK", "TOOLS", "CHIP", "CPU",
        "UPDATE",
    };
    for (int i = 0; i < 9; ++i) {
        CHECK(strcmp(s_detail_label[i], order[i]) == 0,
              "row %d is %s, got '%s'", i, order[i], s_detail_label[i]);
    }
    CHECK(strcmp(s_detail_value[0], "74% 4.02V") == 0,
          "battery folds the volts in, got '%s'", s_detail_value[0]);
    CHECK(strcmp(s_detail_value[1], "CHARGING") == 0, "power word, got '%s'",
          s_detail_value[1]);
    CHECK(strcmp(s_detail_value[2], "GOOD -34") == 0, "wifi, got '%s'",
          s_detail_value[2]);
    CHECK(strcmp(s_detail_value[3], "192.0.2.20") == 0, "ip, got '%s'",
          s_detail_value[3]);
    CHECK(strcmp(s_detail_value[4], "STANDBY") == 0,
          "a closed socket is standby, got '%s'", s_detail_value[4]);
    CHECK(strcmp(s_detail_value[5], "NO KEY") == 0, "tools, got '%s'",
          s_detail_value[5]);
    CHECK(strcmp(s_detail_value[6], "52C") == 0, "chip, got '%s'",
          s_detail_value[6]);
    CHECK(strcmp(s_detail_value[7], "240 LIVE") == 0, "cpu row, got '%s'",
          s_detail_value[7]);
    CHECK(strcmp(s_detail_value[8], "42%") == 0, "update percent, got '%s'",
          s_detail_value[8]);

    /* Every link the other way. */
    links(true, -70, "198.51.100.7", true, 2U, true, true);
    sp_compose();
    CHECK(strcmp(s_detail_value[2], "FAIR -70") == 0, "fair wifi, got '%s'",
          s_detail_value[2]);
    CHECK(strcmp(s_detail_value[3], "198.51.100.7") == 0, "ip follows, got '%s'",
          s_detail_value[3]);
    CHECK(strcmp(s_detail_value[4], "OPEN") == 0, "open socket, got '%s'",
          s_detail_value[4]);
    CHECK(strcmp(s_detail_value[5], "READY") == 0, "tools ready, got '%s'",
          s_detail_value[5]);
    g_chip_c = -100;
    links(true, -70, "198.51.100.7", true, 2U, true, true);
    sp_compose();
    CHECK(strcmp(s_detail_value[6], "NONE") == 0, "no thermometer, got '%s'",
          s_detail_value[6]);
    g_chip_c = 52;
    CHECK(strcmp(s_detail_value[7], "160 SAVE") == 0, "cpu saving, got '%s'",
          s_detail_value[7]);
    links(false, -34, "", false, 1U, false, false);
    sp_compose();
    CHECK(strcmp(s_detail_value[2], "DOWN") == 0, "wifi down, got '%s'",
          s_detail_value[2]);
    CHECK(strcmp(s_detail_value[3], "NONE") == 0, "no ip, got '%s'",
          s_detail_value[3]);
    CHECK(strcmp(s_detail_value[5], "STARTING") == 0, "tools starting, got '%s'",
          s_detail_value[5]);

    /* Power the other way: on the cell, and full on the cable. */
    jr_display_power_set(61U, 3870U, false, false);
    sp_compose();
    CHECK(strcmp(s_detail_value[0], "61% 3.87V") == 0, "on cell, got '%s'",
          s_detail_value[0]);
    CHECK(strcmp(s_detail_value[1], "ON CELL") == 0, "cell word, got '%s'",
          s_detail_value[1]);
    jr_display_power_set(100U, 4180U, true, false);
    sp_compose();
    CHECK(strcmp(s_detail_value[1], "FULL") == 0, "full word, got '%s'",
          s_detail_value[1]);
    jr_display_power_set(0xFFU, 0U, true, false);
    sp_compose();
    CHECK(strcmp(s_detail_value[0], "NONE") == 0, "no gauge, got '%s'",
          s_detail_value[0]);
    CHECK(strcmp(s_detail_value[1], "ON USB") == 0, "no gauge on usb, got '%s'",
          s_detail_value[1]);

    /* At rest the UPDATE row answers readiness instead of naming a non-event. */
    jr_display_ota_set(JR_DISPLAY_OTA_IDLE, 0U, 0U, 0xFFU, true);
    sp_compose();
    CHECK(strcmp(s_detail_value[8], "READY") == 0, "idle+ok, got '%s'",
          s_detail_value[8]);
    jr_display_ota_set(JR_DISPLAY_OTA_IDLE, 0U, 0U, 0xFFU, false);
    sp_compose();
    CHECK(strcmp(s_detail_value[8], "HOLD") == 0, "idle+blocked, got '%s'",
          s_detail_value[8]);
    jr_display_ota_set(JR_DISPLAY_OTA_PROBATION, 0U, 1U, 0xFFU, true);
    sp_compose();
    CHECK(strcmp(s_detail_value[8], "PROBATION") == 0, "probation, got '%s'",
          s_detail_value[8]);

    /* The one wide row: 15 glyphs right-aligned still clear the label. */
    links(true, -34, "255.255.255.255", false, 2U, false, false);
    sp_compose();
    CHECK(strcmp(s_detail_value[3], "255.255.255.255") == 0,
          "the widest address is whole, got '%s'", s_detail_value[3]);
    const int vx = SP_SHEET_RIGHT - 12 * (int)strlen(s_detail_value[3]);
    const int kend = SP_SHEET_LEFT + 12 * (int)strlen(s_detail_label[3]);
    CHECK(vx >= kend + 4, "ip value at x=%d collides with its label ending %d",
          vx, kend);
}

/* THE HEADLINE SAYS THE WORST THING, or the quiet. Each priority is entered
 * from a healthy device and left again, so the order is pinned as well as
 * the words. A closed session socket is NOT on the list any more: it closes
 * at rest by design, and the old "NO LINK" fired on every idle device. */
static void test_status_headline_says_the_worst_thing_first(void)
{
    stage_power(JR_DISPLAY_OTA_IDLE, 0U, JR_DISPLAY_OVERLAY_NONE);
    jr_display_ota_set(JR_DISPLAY_OTA_VALID, 100U, 0U, 0xFFU, true);
    links(true, -40, "198.51.100.7", false, 2U, false, true);
    jr_display_jarvis_set_session(false, 0U, 3725U);
    const char *h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "UP 1H 2M") == 0, "healthy says uptime, got '%s'", h);
    CHECK(strlen(h) < (size_t)SP_LABEL_CAP, "headline fits");

    jr_display_jarvis_set_session(false, 0U, 600U);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "UP 10M") == 0, "minutes alone, got '%s'", h);
    jr_display_jarvis_set_session(false, 0U, 2U * 86400U + 14U * 3600U + 59U);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "UP 2D 14H") == 0, "days and hours, got '%s'", h);
    jr_display_jarvis_set_session(false, 0U, 99U * 86400U + 23U * 3600U);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "UP 99D 23H") == 0 && strlen(h) < (size_t)SP_LABEL_CAP,
          "the longest uptime fits, got '%s'", h);

    g_chip_c = 70;
    links(true, -40, "198.51.100.7", false, 2U, false, true);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "RUNNING HOT") == 0, "70C is hot, got '%s'", h);
    g_chip_c = 69;
    links(true, -40, "198.51.100.7", false, 2U, false, true);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strncmp(h, "UP ", 3) == 0, "69C is not, got '%s'", h);
    g_chip_c = 52;
    links(true, -40, "198.51.100.7", false, 0U, false, true);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "NO TOOLS") == 0, "no key, got '%s'", h);
    links(true, -40, "198.51.100.7", false, 1U, false, true);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strncmp(h, "UP ", 3) == 0, "a starting worker is quiet, got '%s'", h);
    links(false, -40, "", false, 0U, false, true);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "NO WIFI") == 0, "no wifi outranks tools, got '%s'", h);
    jr_display_power_set(8U, 3600U, false, false);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "LOW BATTERY") == 0, "low cell outranks wifi, got '%s'", h);
    jr_display_power_set(8U, 3600U, true, true);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "NO WIFI") == 0, "a low cell on the charger is no alarm");
    jr_display_power_set(0xFFU, 0U, true, false);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "NO BATTERY") == 0, "no gauge outranks all, got '%s'", h);
    jr_display_ota_set(JR_DISPLAY_OTA_RECEIVING, 42U, 0U, 1U, true);
    h = sp_headline(JR_DISPLAY_SPACE_STATUS);
    CHECK(strcmp(h, "UPDATE 42%") == 0, "an update outranks everything");
}

/* THE CLOSED FACE FOLLOWS THE LINKS. Lamps: "LINK TOOLS" above the ring,
 * ink when up, dim when not — counted as ink pixels in the lamp band. Bars:
 * accent pixels inside the ring's bottom line rise with RSSI and vanish when
 * Wi-Fi is down (the word DOWN is grey). The big percentage is ink inside
 * the ring. Each count is asserted positive where it should be, so an
 * empty frame cannot pass. */
static void stage_space(int space);
static uint16_t *render_frame(void);

static size_t count_in(const uint16_t *fb, uint16_t px, int x0, int x1,
                       int y0, int y1)
{
    size_t n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (fb[(size_t)y * HUD_W + x] == px) {
                n++;
            }
        }
    }
    return n;
}

static void test_missing_clip_falls_back_to_the_face_it_grew_from(void)
{
    CHECK(face_fallback(JR_FACE_RESTING) == JR_FACE_IDLE, "rest -> idle");
    CHECK(face_fallback(JR_FACE_MUTED) == JR_FACE_IDLE, "muted -> idle");
    CHECK(face_fallback(JR_FACE_LINKING) == JR_FACE_THINKING, "linking -> thinking");
    CHECK(face_fallback(JR_FACE_DIAL_DIVER) == JR_FACE_IDLE, "dial diver -> idle");
    CHECK(face_fallback(JR_FACE_DIAL_DRESS) == JR_FACE_IDLE, "dial dress -> idle");
    CHECK(face_fallback(JR_FACE_DIAL_PILOT) == JR_FACE_IDLE, "dial pilot -> idle");
    CHECK(face_fallback(JR_FACE_DIAL_FUTURE) == JR_FACE_IDLE, "dial future -> idle");
    CHECK(face_fallback(JR_FACE_DIAL_SKELETON) == JR_FACE_IDLE, "dial skeleton -> idle");
    CHECK(face_fallback(JR_FACE_DIAL_CELESTIAL) == JR_FACE_IDLE, "dial celestial -> idle");
    for (int f = 0; f < (int)JR_FACE_COUNT; ++f) {
        const jr_face_t p = face_fallback((jr_face_t)f);
        CHECK(p == (jr_face_t)f || face_fallback(p) == p,
              "face %d falls back at most once (parent %d has none)", f, (int)p);
    }
    for (int f = 0; f <= (int)JR_FACE_ERROR; ++f) {
        CHECK(face_fallback((jr_face_t)f) == (jr_face_t)f,
              "the original five faces are their own fallback (%d)", f);
    }
}

/* Back to an empty dial cache and the stubs' defaults: no frame held, the
 * counters at zero, 16 MB of PSRAM, nothing hooked. */
static void dial_cache_reset(void)
{
    s_dial_blit = false;
    dial_cache_release();
    s_dial_filling = -1;
    s_dial_clock = 0;
    s_dial_glass_ms = 0;
    s_dial_warm_skip = 0;
    s_dial_hits = s_dial_misses = s_dial_evictions = s_dial_warms = s_dial_decode_ms = 0;
    s_dial_frame = NULL;
    s_dial_frame_face = JR_FACE_COUNT;
    s_stub_psram_budget = (size_t)16 << 20;
    s_stub_malloc_fail = 0;
    s_stub_eaf_salt = 0;
    s_stub_eaf_decode_us = 0;
    s_stub_eaf_decode_hook = NULL;
    s_stub_eaf_decode_error = ESP_OK;
    s_stub_lock_failures = 0;
}

/* N14.2: a baked dial is decoded once and copied into every strip; the
 * anim path stays the answer for anything the blit cannot take. */
static void test_dial_decodes_once_and_blits_every_strip(void)
{
    jr_display_ctx_t *ctx = &s_display;
    ctx->board.width = HUD_W;
    ctx->board.height = HUD_H;
    ctx->board.swap_color_bytes = false;
    s_stub_eaf_head = (eaf_header_t){ 8, HUD_W, HUD_H };
    dial_cache_reset();
    s_stub_eaf_decodes = 0;
    ctx->missing_faces = 0;
    ctx->clips[JR_FACE_DIAL_DIVER] = (jr_display_clip_t){
        heap_caps_malloc(32U, MALLOC_CAP_SPIRAM), 32U, 8U, 7U, false };
    ctx->clips[JR_FACE_DIAL_DRESS] = (jr_display_clip_t){
        heap_caps_malloc(32U, MALLOC_CAP_SPIRAM), 32U, 8U, 7U, false };

    CHECK(!dial_show(ctx, JR_FACE_IDLE), "a face that is not a dial stays on the anim");
    CHECK(!s_dial_blit && s_stub_eaf_decodes == 0, "and decodes nothing");

    CHECK(dial_show(ctx, JR_FACE_DIAL_DIVER), "a resident dial goes to the blit");
    CHECK(s_dial_blit && ctx->shown_face == JR_FACE_DIAL_DIVER &&
          ctx->active == NULL, "blit on, dial shown, no visible anim");
    CHECK(ctx->clips[JR_FACE_DIAL_DIVER].data == NULL,
          "successfully decoded unbound dial releases its compressed source");
    CHECK(dial_show(ctx, JR_FACE_DIAL_DIVER), "again");
    CHECK(s_stub_eaf_decodes == 1, "the same dial is decoded once, got %d",
          s_stub_eaf_decodes);

    static uint16_t strip[HUD_W * STRIP_ROWS];
    const int y1 = 227 - STRIP_ROWS / 2;
    const int x1 = 10, x2 = HUD_W - 10;
    apply_dial(ctx, x1, y1, x2, y1 + STRIP_ROWS, strip);
    int bad = 0;
    for (int r = 0; r < STRIP_ROWS; ++r) {
        for (int c = 0; c < x2 - x1; ++c) {
            const size_t i = (size_t)(y1 + r) * HUD_W + (size_t)(x1 + c);
            if (strip[(size_t)r * (size_t)(x2 - x1) + (size_t)c] !=
                (uint16_t)(i ^ 0x5a5au)) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "every strip pixel is the decoded frame's, %d wrong", bad);

    CHECK(dial_show(ctx, JR_FACE_DIAL_DRESS) && s_stub_eaf_decodes == 2,
          "a dial shown for the first time decodes into its own frame");

    dial_cache_reset();
    s_stub_eaf_head.width = 400;
    ctx->clips[JR_FACE_DIAL_DIVER] = (jr_display_clip_t){
        heap_caps_malloc(32U, MALLOC_CAP_SPIRAM), 32U, 8U, 7U, false };
    CHECK(!dial_show(ctx, JR_FACE_DIAL_DIVER) && !s_dial_blit,
          "a dial that is not panel-sized stays on the anim");
    s_stub_eaf_head.width = HUD_W;
    ctx->missing_faces = 1U << JR_FACE_DIAL_DIVER;
    CHECK(!dial_show(ctx, JR_FACE_DIAL_DIVER), "a missing clip keeps its fallback");

    ctx->missing_faces = 0;
    heap_caps_free(ctx->clips[JR_FACE_DIAL_DIVER].data);
    memset(&ctx->clips[JR_FACE_DIAL_DIVER], 0, sizeof ctx->clips[0]);
    dial_cache_reset();
}

static void test_render_cadence_reaches_the_engine_and_clamps(void)
{
    gfx_handle_t saved = s_display.gfx;
    s_display.gfx = (gfx_handle_t)&saved;          /* "up": the stub records */
    __atomic_store_n(&s_render_fps, JR_DISPLAY_RENDER_FPS, __ATOMIC_RELEASE);

    g_stub_render_fps = 0;
    CHECK(jr_display_set_render_fps(4) == ESP_OK, "set 4 ok");
    CHECK(g_stub_render_fps == 4, "engine got 4, saw %u", (unsigned)g_stub_render_fps);
    CHECK(jr_display_render_fps() == 4, "getter follows");

    g_stub_render_fps = 0;
    jr_display_set_render_fps(4);
    CHECK(g_stub_render_fps == 0, "an unchanged value never reaches the engine");

    jr_display_set_render_fps(0);
    CHECK(g_stub_render_fps == JR_DISPLAY_RENDER_FPS_MIN,
          "0 clamps to the floor, saw %u", (unsigned)g_stub_render_fps);
    jr_display_set_render_fps(90);
    CHECK(g_stub_render_fps == JR_DISPLAY_RENDER_FPS,
          "90 clamps to the panel ceiling, saw %u", (unsigned)g_stub_render_fps);

    s_display.gfx = NULL;                            /* before the presenter */
    g_stub_render_fps = 0;
    CHECK(jr_display_set_render_fps(6) == ESP_OK, "pre-init set is remembered");
    CHECK(g_stub_render_fps == 0 && jr_display_render_fps() == 6,
          "init will read 6 from the getter");

    __atomic_store_n(&s_render_fps, JR_DISPLAY_RENDER_FPS, __ATOMIC_RELEASE);
    s_display.gfx = saved;
}

/* The render's own clock (E1): strip costs add up, frames count at frame
 * starts only, and a whole second of whole frames latches its average into
 * render_frame_us — the number /api/display serves. Four strips of 100 us
 * per frame, a frame every 400 ms: the first second closes on three frames
 * at exactly 400 us each. */
static void test_render_clock_counts_frames_and_latches_a_second(void)
{
    diag_store(&s_display.render_us, 0U);
    diag_store(&s_display.render_frames, 0U);
    diag_store(&s_display.render_frame_us, 0U);
    s_render_win_ms = 0U;
    s_render_win_us = 0U;
    s_render_win_frames = 0U;
    for (int f = 0; f < 4; ++f) {
        const uint32_t now_ms = 1000U + (uint32_t)f * 400U;
        for (int strip = 0; strip < 4; ++strip) {
            render_account(&s_display, strip == 0, 100U, now_ms);
        }
        if (f == 2) {
            CHECK(diag_load(&s_display.render_frame_us) == 0U,
                  "the window latched before a second had passed (%u)",
                  (unsigned)diag_load(&s_display.render_frame_us));
        }
    }
    jr_display_diag_t d;
    CHECK(jr_display_get_diag(&d) == ESP_OK, "diag reads");
    CHECK(d.render_us == 1600U, "render_us %u, wanted 1600", (unsigned)d.render_us);
    CHECK(d.render_frames == 4U, "render_frames %u, wanted 4", (unsigned)d.render_frames);
    CHECK(d.render_frame_us == 400U, "render_frame_us %u, wanted 400 (three whole frames)",
          (unsigned)d.render_frame_us);
    /* a strip that is not a frame start moves the cost, not the count */
    render_account(&s_display, false, 50U, 2300U);
    CHECK(diag_load(&s_display.render_us) == 1650U && diag_load(&s_display.render_frames) == 4U,
          "a mid-frame strip counted as a frame");
    s_render_win_ms = 0U;
    s_render_win_us = 0U;
    s_render_win_frames = 0U;
}

/* The three stats under the ring: label over value, a value in ink when it is
 * up and dim (grey at about a third) when it is not. Ink is sampled at full
 * coverage, so "on" is exact and "dim" is the absence of exact ink. While an
 * update rings the stats are not drawn at all: they cross its band. */
static int stat_cx(int i)
{
    return SP_CX + (i - 1) * SG_STAT_DX;
}

static size_t stat_value_ink(const uint16_t *fb, int i, uint16_t color)
{
    return count_in(fb, color, stat_cx(i) - 32, stat_cx(i) + 32, SG_STAT_VALUE_Y,
                    SG_STAT_VALUE_Y + 16);
}

static size_t stat_value_drawn(const uint16_t *fb, int i)
{
    size_t n = 0;
    for (int y = SG_STAT_VALUE_Y; y < SG_STAT_VALUE_Y + 16; ++y) {
        for (int x = stat_cx(i) - 32; x < stat_cx(i) + 32; ++x) {
            n += fb[(size_t)y * HUD_W + x] != POISON;
        }
    }
    return n;
}

static void test_status_face_follows_the_links(void)
{
    stage_space(JR_DISPLAY_SPACE_STATUS);
    jr_display_power_set(74U, 4020U, true, true);
    const int cyr = SP_CY + SG_RING_DY;
    const int hero_y0 = cyr - 31;

    links(true, -34, "198.51.100.7", true, 2U, false, false);
    uint16_t *fb = render_frame();
    if (!fb) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        return;
    }
    const size_t wifi_on = stat_value_ink(fb, 0, SP_C_INK);
    const size_t link_on = stat_value_ink(fb, 1, SP_C_INK);
    const size_t tools_on = stat_value_ink(fb, 2, SP_C_INK);
    const size_t big = count_in(fb, SP_C_INK, SP_CX - 60, SP_CX + 60, hero_y0, hero_y0 + 59);
    free(fb);
    CHECK(wifi_on > 15, "WIFI up is ink, got %zu", wifi_on);
    CHECK(link_on > 15, "LINK on is ink, got %zu", link_on);
    CHECK(tools_on > 15, "TOOLS on is ink, got %zu", tools_on);
    CHECK(big > 200, "the percentage is the big ink, got %zu", big);

    links(true, -80, "198.51.100.7", false, 0U, false, false);
    fb = render_frame();
    if (!fb) {
        return;
    }
    const size_t wifi_weak = stat_value_ink(fb, 0, SP_C_INK);
    const size_t link_off = stat_value_ink(fb, 1, SP_C_INK);
    const size_t tools_off = stat_value_ink(fb, 2, SP_C_INK);
    const size_t link_off_drawn = stat_value_drawn(fb, 1);
    free(fb);
    CHECK(wifi_weak > 15, "a weak link is still up, in ink: %zu", wifi_weak);
    CHECK(link_off == 0 && tools_off == 0, "off stats draw no ink: %zu %zu", link_off, tools_off);
    CHECK(link_off_drawn > 15, "an off stat is dim, not blank: %zu", link_off_drawn);

    links(false, -34, "", false, 2U, false, false);
    fb = render_frame();
    if (!fb) {
        return;
    }
    const size_t down_ink = stat_value_ink(fb, 0, SP_C_INK);
    const size_t down = stat_value_drawn(fb, 0);
    free(fb);
    CHECK(down_ink == 0, "no wifi is dim: %zu ink pixels", down_ink);
    CHECK(down > 30, "DOWN is written, dim: %zu pixels", down);

    /* An update rings across the stats' rows, so they are not drawn: every
     * pixel in the stat boxes is either the update ring's or untouched. */
    links(true, -34, "198.51.100.7", true, 2U, false, false);
    stage_ring_at_rest(JR_DISPLAY_SPACE_STATUS, JR_DISPLAY_OTA_RECEIVING, 42U);
    s_space_veil = 0;         /* the veil would dim the poison too */
    fb = render_frame();
    if (!fb) {
        return;
    }
    size_t leaked = 0;
    for (int y = SG_STAT_LABEL_Y - 2; y < SG_STAT_VALUE_Y + 18; ++y) {
        for (int x = SP_CX - 100; x < SP_CX + 100; ++x) {
            const int dx = x - SP_CX, dy = y - SP_CY;
            const int r = sp_isqrt(dx * dx + dy * dy);
            if (r < SP_OTA_IN - 1 || r > SP_OTA_OUT + 1) {
                leaked += fb[(size_t)y * HUD_W + x] != POISON && r < SP_ORB_TRACK_IN - 1;
            }
        }
    }
    free(fb);
    CHECK(leaked == 0, "%zu stat pixels drawn while the update ring is up", leaked);
}

/* The shade's two readouts are where volume and light are read now that the
 * SETTINGS headline is gone. Staged at the WORST CASE — 100 and 100 — because
 * that is the only place this breaks: the column stores 10 glyphs, and
 * "R LIGHT 100%" is 12, which used to render "R LIGHT 10" — one of the two
 * levels the shade exists to show, silently wrong rather than absent. */
static void test_shade_readouts_survive_at_100(void)
{
    stage_power(JR_DISPLAY_OTA_IDLE, 0U, JR_DISPLAY_OVERLAY_SHADE);
    s_status_word = 100U;                     /* volume 100 */
    __atomic_store_n(&s_brightness_want, 100U, __ATOMIC_RELEASE);
    sp_compose();
    CHECK(strcmp(s_shade_vol, "100%") == 0,
          "shade volume complete, got '%s'", s_shade_vol);
    CHECK(strcmp(s_shade_light, "100%") == 0,
          "shade light complete, got '%s'", s_shade_light);
    CHECK(strlen(s_shade_light) < (size_t)SP_COL_MAX,
          "shade light fits its store, got %zu of %d",
          strlen(s_shade_light), SP_COL_MAX);
}

/* --------------------------------------------------------------- geometry -- */

static uint16_t *render_frame(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) {
        return NULL;
    }
    for (size_t i = 0; i < px; ++i) {
        fb[i] = POISON;
    }
    /* The presenter's order, minus the clock (which has its own test): the
     * shell first, then the update ring above it. */
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        apply_space_overlay(&s_display, y, y2, fb + (size_t)y * HUD_W);
        apply_ota_ring(&s_display, y, y2, fb + (size_t)y * HUD_W);
    }
    return fb;
}

static int radius_of(int i)
{
    const int dx = (i % HUD_W) - SP_CX;
    const int dy = (i / HUD_W) - SP_CY;
    return sp_isqrt(dx * dx + dy * dy);
}

/* The structural claim: the shell cannot reach the battery rim (r215-220),
 * the gold privacy ring (r221-222), or the choice arcs (r223-231), because it
 * cannot draw past JR_DISPLAY_SHELL_R_MAX at all. Checked across the whole
 * state matrix rather than argued one renderer at a time. */
static void test_never_leaves_shell_radius(void)
{
    static const jr_display_ota_state_t ota[] = {
        JR_DISPLAY_OTA_IDLE,      JR_DISPLAY_OTA_PREFLIGHT,
        JR_DISPLAY_OTA_BLOCKED,   JR_DISPLAY_OTA_RECEIVING,
        JR_DISPLAY_OTA_PROBATION, JR_DISPLAY_OTA_VALID,
        JR_DISPLAY_OTA_FAILED,    JR_DISPLAY_OTA_ROLLED_BACK,
    };
    static const int overlays[] = {
        JR_DISPLAY_OVERLAY_NONE, JR_DISPLAY_OVERLAY_DETAIL,
        JR_DISPLAY_OVERLAY_SHADE,
    };
    int worst = 0;

    jr_display_desk_set_task("REINDEX", 64U, JR_DISPLAY_AGENT_WORKING);
    jr_display_jarvis_set_session(true, 17U, 900U);
    /* Feed the two live-data screens so they draw their full content — an
     * empty WEATHER is two bare tracks, which proves nothing about the mark
     * or the text; an empty ACTIVITY is one line. */
    set_weather(true, 83, 86, 76, JR_DISPLAY_SKY_CLOUDS, "OVERCAST", 0U);
    reset_activity();
    jr_display_activity_push("WEATHER", "83 OVERCAST FORT LAUDERDALE");
    jr_display_activity_push("WEB", "FOUND THREE RESULTS");
    jr_display_activity_push("SAID", "VOLUME IS NOW FORTY");

    for (int space = 0; space < JR_DISPLAY_SPACE_COUNT; ++space) {
        for (size_t o = 0; o < sizeof overlays / sizeof *overlays; ++o) {
            for (size_t s = 0; s < sizeof ota / sizeof *ota; ++s) {
                for (int muted = 0; muted <= 1; ++muted) {
                    stage_power(ota[s], 63U, overlays[o]);
                    s_space_from = (uint8_t)space;
                    s_space_to = (uint8_t)space;
                    s_detail_space = (uint8_t)space;
                    __atomic_store_n(&s_nav_word,
                                     (uint32_t)space |
                                         ((uint32_t)overlays[o]
                                          << NAV_OVL_SHIFT),
                                     __ATOMIC_RELEASE);
                    __atomic_store_n(&s_hud_env_word,
                                     74U | (muted ? (1U << 9) : 0U),
                                     __ATOMIC_RELEASE);
                    sp_compose();

                    uint16_t *fb = render_frame();
                    if (!fb) {
                        printf("FAIL %s: allocation failed\n", __func__);
                        g_failures++;
                        return;
                    }
                    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
                        if (fb[i] == POISON) {
                            continue;
                        }
                        const int r = radius_of((int)i);
                        if (r > worst) {
                            worst = r;
                        }
                        if (r > JR_DISPLAY_SHELL_R_MAX) {
                            CHECK(false,
                                  "space %d overlay %d ota %d muted %d wrote "
                                  "r=%d (max %d)",
                                  space, overlays[o], (int)ota[s], muted, r,
                                  JR_DISPLAY_SHELL_R_MAX);
                            free(fb);
                            return;
                        }
                    }
                    free(fb);
                }
            }
        }
    }
    /* Guard against this passing because nothing was drawn at all. */
    CHECK(worst > JR_DISPLAY_SAFE_R, "the shell actually drew (worst r=%d)",
          worst);
}

/* The update ring's two neighbours are the headline inside it and the safe
 * area outside it. Measure the pixels actually drawn rather than trust the
 * arithmetic in the comment — on EVERY space, because the ring belongs to
 * none of them: a firmware update must be visible wherever the glass is,
 * including the face at rest, where the shell itself draws nothing. */
static void test_ota_ring_band(void)
{
    for (int space = 0; space < (int)JR_DISPLAY_SPACE_COUNT; ++space) {
        /* Pin the headline so the OTA state change cannot move it: a caller
         * label outranks the composed one, which leaves the ring as the only
         * difference between the two frames. JARVIS has no headline. */
        stage_ring_at_rest(space, JR_DISPLAY_OTA_IDLE, 0U);
        if (space != JR_DISPLAY_SPACE_JARVIS) {
            jr_display_space_set_label((jr_display_space_t)space, "PINNED",
                                       NULL);
            sp_compose();
        }
        uint16_t *quiet = render_frame();
        stage_ring_at_rest(space, JR_DISPLAY_OTA_RECEIVING, 63U);
        uint16_t *ringing = render_frame();
        if (!quiet || !ringing) {
            printf("FAIL %s: allocation failed\n", __func__);
            g_failures++;
            free(quiet);
            free(ringing);
            return;
        }

        int lo = 9999, hi = -1, changed = 0;
        for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
            if (quiet[i] == ringing[i]) {
                continue;
            }
            const int r = radius_of((int)i);
            /* STATUS's stats and WEATHER's age line cross the ring's band, so
             * they give way while it rings: that is a change, by design,
             * everywhere in their rows but the band itself. */
            const int y = (int)(i / HUD_W);
            if ((space == JR_DISPLAY_SPACE_STATUS || space == JR_DISPLAY_SPACE_WEATHER) &&
                y >= SG_STAT_LABEL_Y - 2 && y < SG_STAT_VALUE_Y + 18 &&
                (r < SP_OTA_IN - 1 || r > SP_OTA_OUT + 1)) {
                continue;
            }
            if (r < lo) {
                lo = r;
            }
            if (r > hi) {
                hi = r;
            }
            ++changed;
        }

        CHECK(changed > 0, "space %d: the ring drew something", space);
        CHECK(lo >= SP_OTA_IN - 1, "space %d: inner edge at r=%d, expected >= %d",
              space, lo, SP_OTA_IN - 1);
        CHECK(hi <= SP_OTA_OUT + 1, "space %d: outer edge at r=%d, expected <= %d",
              space, hi, SP_OTA_OUT + 1);
        CHECK(hi <= JR_DISPLAY_SAFE_R,
              "space %d: readable content stays in the safe area", space);
        CHECK(hi < 215,
              "space %d: clears the battery rim, the privacy ring and the arcs",
              space);

        /* IDLE and VALID are both quiet, so moving between them must change
         * nothing at all on the glass. */
        free(ringing);
        stage_ring_at_rest(space, JR_DISPLAY_OTA_VALID, 100U);
        ringing = render_frame();
        if (ringing) {
            CHECK(memcmp(quiet, ringing,
                         (size_t)HUD_W * HUD_H * sizeof *quiet) == 0,
                  "space %d: VALID draws no ring", space);
        }
        if (space != JR_DISPLAY_SPACE_JARVIS) {
            jr_display_space_set_label((jr_display_space_t)space, NULL, NULL);
        }
        free(quiet);
        free(ringing);
    }
}

/* The ring sits outside the headline's worst-case glyph corner, on every
 * space that has a headline. Find that corner by diffing against a headline
 * of maximum width: an update in flight must never be crossed by the words
 * under the focal object. */
static void test_ota_ring_clears_the_headline(void)
{
    for (int space = JR_DISPLAY_SPACE_JARVIS + 1;
         space < (int)JR_DISPLAY_SPACE_COUNT; ++space) {
        stage_ring_at_rest(space, JR_DISPLAY_OTA_IDLE, 0U);
        uint16_t *a = render_frame();
        jr_display_space_set_label((jr_display_space_t)space, "WWWWWWWWWWWW",
                                   NULL);
        sp_compose();
        uint16_t *b = render_frame();
        if (!a || !b) {
            printf("FAIL %s: allocation failed\n", __func__);
            g_failures++;
            free(a);
            free(b);
            return;
        }

        int hi = -1;
        for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
            if (a[i] != b[i]) {
                const int r = radius_of((int)i);
                if (r > hi) {
                    hi = r;
                }
            }
        }
        CHECK(hi > 0, "space %d: the headline drew something", space);
        CHECK(hi < SP_OTA_IN, "space %d: headline reaches r=%d, ring starts at r=%d",
              space, hi, SP_OTA_IN);
        CHECK(hi <= JR_DISPLAY_SAFE_R, "space %d: headline stays readable",
              space);

        jr_display_space_set_label((jr_display_space_t)space, NULL, NULL);
        free(a);
        free(b);
    }
}

/* The detail sheet grew a row for the update; prove the last one still lands
 * inside the sheet and inside the glass. */
static void test_detail_sheet_rows_stay_inside(void)
{
    const int last = SP_SHEET_ROW_Y + (SP_ROWS_MAX - 1) * SP_SHEET_ROW_DY;
    CHECK(last + TEXT_H <= SP_SHEET_Y1,
          "row %d ends at y=%d, sheet ends at %d", SP_ROWS_MAX - 1,
          last + TEXT_H, SP_SHEET_Y1);

    /* Both text columns on the last row's bottom edge have to be inside the
     * safe circle, not merely inside the panel rectangle. */
    for (int x = SP_SHEET_LEFT; x <= SP_SHEET_RIGHT; ++x) {
        const int dx = x - SP_CX;
        const int dy = (last + TEXT_H) - SP_CY;
        CHECK(sp_isqrt(dx * dx + dy * dy) <= JR_DISPLAY_SHELL_R_MAX,
              "sheet column x=%d escapes the shell circle", x);
    }
}

/* 466 = 38*12 + 10: the last strip is ragged, and any y-dependent state that
 * leaks between calls becomes a seam on glass and nowhere else. */
static void test_strip_invariance(void)
{
    stage_power(JR_DISPLAY_OTA_RECEIVING, 63U, JR_DISPLAY_OVERLAY_DETAIL);

    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *whole = malloc(px * sizeof *whole);
    uint16_t *strips = malloc(px * sizeof *strips);
    if (!whole || !strips) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        free(whole);
        free(strips);
        return;
    }
    for (size_t i = 0; i < px; ++i) {
        whole[i] = strips[i] = POISON;
    }

    apply_space_overlay(&s_display, 0, HUD_H, whole);
    apply_ota_ring(&s_display, 0, HUD_H, whole);
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        apply_space_overlay(&s_display, y, y2, strips + (size_t)y * HUD_W);
        apply_ota_ring(&s_display, y, y2, strips + (size_t)y * HUD_W);
    }

    size_t diff = 0;
    for (size_t i = 0; i < px; ++i) {
        if (whole[i] != strips[i]) {
            ++diff;
        }
    }
    CHECK(diff == 0, "%zu pixels differ between whole-frame and strip render",
          diff);
    free(whole);
    free(strips);
}

/* JARVIS at rest is the compatibility contract: every pre-shell scene stays
 * bit-identical, which requires the shell to write nothing whatsoever while
 * there is nothing to report. */
static void test_jarvis_at_rest_draws_nothing(void)
{
    stage_ring_at_rest(JR_DISPLAY_SPACE_JARVIS, JR_DISPLAY_OTA_IDLE, 0U);

    uint16_t *fb = render_frame();
    if (!fb) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        return;
    }
    size_t touched = 0;
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        if (fb[i] != POISON) {
            ++touched;
        }
    }
    CHECK(touched == 0, "%zu pixels written in JARVIS at rest", touched);
    free(fb);
}

/* The ONE exception to that contract. A firmware update used to be visible
 * only on SETTINGS, so the updater navigated the glass there to show it;
 * with that screen gone, the ring must reach the face at rest on its own —
 * the shell is OFF here (no veil, no orbit, no headline), and the update
 * word alone lights the band. Mutation: gating the ring on s_space_on fails
 * this test and nothing else. */
static void test_jarvis_at_rest_still_shows_the_update(void)
{
    stage_ring_at_rest(JR_DISPLAY_SPACE_JARVIS, JR_DISPLAY_OTA_RECEIVING, 63U);

    uint16_t *fb = render_frame();
    if (!fb) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        return;
    }
    size_t touched = 0, outside = 0;
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        if (fb[i] == POISON) {
            continue;
        }
        ++touched;
        const int r = radius_of((int)i);
        if (r < SP_OTA_IN - 1 || r > SP_OTA_OUT + 1) {
            ++outside;
        }
    }
    /* A full track at r140-154 is ~13k pixels; demand most of it so a single
     * stray write cannot pass as "the ring". */
    CHECK(touched > 10000, "only %zu pixels: the update ring did not reach "
          "JARVIS at rest", touched);
    CHECK(outside == 0, "%zu pixels outside the ring band: the shell woke up "
          "for an update", outside);
    free(fb);
}

/* On WATCH the clock keeps its dial clean by clearing the disc AFTER the
 * shell has drawn, so a ring drawn as shell furniture was wiped on precisely
 * the screen an owner glances at. The ring is therefore drawn above the
 * watch. Compose the presenter's real order and demand the band survives.
 * Mutation: moving apply_ota_ring before the clock fails this. */
static void test_update_ring_outlives_the_watch_clear(void)
{
    stage_ring_at_rest(JR_DISPLAY_SPACE_WATCH, JR_DISPLAY_OTA_RECEIVING, 63U);
    jr_display_clock_set(true, 10, 8, 30);
    s_clock_ease = 256;
    s_watch_s = 256;
    sp_compose();

    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        return;
    }
    for (size_t i = 0; i < px; ++i) {
        fb[i] = POISON;
    }
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        uint16_t *strip = fb + (size_t)y * HUD_W;
        apply_space_overlay(&s_display, y, y2, strip);
        apply_clock_overlay(&s_display, y, y2, strip);
        apply_ota_ring(&s_display, y, y2, strip);
    }

    size_t band = 0, lit = 0;
    for (size_t i = 0; i < px; ++i) {
        const int r = radius_of((int)i);
        if (r < SP_OTA_IN || r > SP_OTA_OUT) {
            continue;
        }
        ++band;
        if (fb[i] != 0U && fb[i] != POISON) {
            ++lit;
        }
    }
    CHECK(band > 10000, "band is %zu pixels — test is vacuous", band);
    CHECK(lit * 10 >= band * 8,
          "%zu of %zu band pixels lit on WATCH: the clock clear ate the update",
          lit, band);
    s_clock_ease = 0;
    free(fb);
}

/* The screen-name label is glass type, and its dark band belongs to the face:
 * over a face it still dims y360-430, on a black data screen it touches none of
 * the screen's own rows (STATUS keeps its stats at y352-382, which the old band
 * dimmed from y360). */
static void test_caption_band_gives_way_to_a_black_backdrop(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    CHECK(fb != NULL, "frame allocated");
    if (!fb) {
        return;
    }
    jr_display_caption_label("STATUS", JR_DISPLAY_LABEL_SCREEN);
    s_caption_prog = s_caption_ease = 0;     /* nothing is up: the new name copies at once */
    s_cap_rev_seen = 0xFFFFFFFFu;
    caption_tick(1U);                  /* the render task takes the staged words */
    s_caption_prog = s_caption_ease = 256;
    s_cap_p = 256;
    s_cap_alpha = 256;
    for (int black = 0; black <= 256; black += 256) {
        s_space_black = black;
        s_shade_ease = 0;
        for (int y = 0; y < HUD_H; ++y) {
            for (int x = 0; x < HUD_W; ++x) {
                /* stats rows lit; the label's own rows start black */
                fb[(size_t)y * HUD_W + x] = (y >= 352 && y <= 382) ? 0x07FFu : 0u;
            }
        }
        for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
            const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
            apply_caption_overlay(&s_display, y, y2, fb + (size_t)y * HUD_W);
        }
        int dimmed = 0, stats = 0, ink = 0;
        for (int y = 352; y <= 382; ++y) {
            for (int x = 150; x <= 316; ++x) {
                ++stats;
                dimmed += fb[(size_t)y * HUD_W + x] != 0x07FFu;
            }
        }
        for (int y = 390; y <= 415; ++y) {
            for (int x = 150; x <= 316; ++x) {
                ink += fb[(size_t)y * HUD_W + x] != 0u;
            }
        }
        CHECK(stats == 31 * 167, "scanned %d stats pixels", stats);
        CHECK(ink > 100, "the label draws its glyphs (black=%d, %d pixels)", black, ink);
        /* a label wears a halo, not a plate: it dims no row, over a face or on black */
        CHECK(dimmed == 0, "a label dims no stats pixel (black=%d, %d were)", black, dimmed);
        CHECK(fb[(size_t)200 * HUD_W + 233] == 0u, "nothing above the band is touched");
    }
    s_space_black = 0;
    s_caption_prog = s_caption_ease = 0;
    jr_display_caption_clear();
    free(fb);
}

/* THE SHADE ON THE GLASS. Volume and brightness are rim arcs that fill with
 * their value, the mute button is cyan live and gold muted, and the breathing
 * ring exists only while the shade is open AND the microphone is muted. */
static int exact_in_annulus(const uint16_t *fb, uint16_t color, int cx, int cy, int r0, int r1,
                            int side /* -1 left of cx, 1 right, 0 both */)
{
    int n = 0;
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const int dx = x - cx, dy = y - cy;
            const int r = sp_isqrt(dx * dx + dy * dy);
            if (r < r0 || r > r1 || (side < 0 && dx >= 0) || (side > 0 && dx <= 0)) {
                continue;
            }
            n += fb[(size_t)y * HUD_W + x] == color;
        }
    }
    return n;
}

static int drawn_in_annulus(const uint16_t *fb, int cx, int cy, int r0, int r1)
{
    int n = 0;
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const int dx = x - cx, dy = y - cy;
            const int r = sp_isqrt(dx * dx + dy * dy);
            n += r >= r0 && r <= r1 && fb[(size_t)y * HUD_W + x] != POISON;
        }
    }
    return n;
}

static void stage_shade(bool muted, int vol, int light, uint32_t ent, uint32_t breath)
{
    stage_power(JR_DISPLAY_OTA_IDLE, 0U, JR_DISPLAY_OVERLAY_SHADE);
    s_space_veil = 0;
    s_space_black = 0;
    s_status_word = (uint32_t)vol;
    __atomic_store_n(&s_brightness_want, (uint8_t)light, __ATOMIC_RELEASE);
    __atomic_store_n(&s_hud_env_word, 74U | (muted ? (1U << 9) : 0U), __ATOMIC_RELEASE);
    s_shade_ms = ent;
    s_breath_ms = breath;
    sp_compose();
}

/* The light arc and its number are the owner's setting, not the level the mood
 * ladder has dimmed the panel to. The cap rides in bits 8..15 of the status
 * word; 0 there means unknown and the panel's own target stands in. */
static void test_shade_light_is_the_owner_setting(void)
{
    /* dimmed to 30 by the ladder, owner set 80 */
    stage_shade(false, 50, 30, SP_SHADE_ENTRY_MS, 0U);
    s_status_word = 50U | (80U << 8);
    sp_compose();
    CHECK(strcmp(s_shade_light, "80%") == 0, "the number is the owner's 80, not the dimmed 30: \"%s\"", s_shade_light);
    CHECK(strcmp(s_shade_vol, "50%") == 0, "the volume number is untouched by the cap: \"%s\"", s_shade_vol);
    uint16_t *fb = render_frame();
    CHECK(fb != NULL, "frame allocated");
    if (!fb) {
        return;
    }
    const int dimmed = exact_in_annulus(fb, SP_C_AMBER, SP_CX, SP_CY, 180, 188, 1);
    free(fb);

    stage_shade(false, 50, 80, SP_SHADE_ENTRY_MS, 0U);      /* the same 80, no cap word */
    fb = render_frame();
    CHECK(fb != NULL, "frame allocated");
    if (!fb) {
        return;
    }
    const int at80 = exact_in_annulus(fb, SP_C_AMBER, SP_CX, SP_CY, 180, 188, 1);
    free(fb);
    CHECK(dimmed > 100 && dimmed == at80, "the arc sweeps to 80 whatever the panel is at: %d vs %d", dimmed, at80);

    /* no cap yet: the panel's own target is all there is */
    stage_shade(false, 50, 30, SP_SHADE_ENTRY_MS, 0U);
    CHECK(strcmp(s_shade_light, "30%") == 0, "without a cap the number falls back to the panel: \"%s\"", s_shade_light);
}

static void test_shade_button_arcs_and_breath(void)
{
    /* the arcs fill with their value */
    stage_shade(false, 25, 25, SP_SHADE_ENTRY_MS, 0U);
    uint16_t *fb = render_frame();
    CHECK(fb != NULL, "frame allocated");
    if (!fb) {
        return;
    }
    const int v25 = exact_in_annulus(fb, SP_C_CYAN, SP_CX, SP_CY, 180, 188, -1);
    const int l25 = exact_in_annulus(fb, SP_C_AMBER, SP_CX, SP_CY, 180, 188, 1);
    free(fb);
    stage_shade(false, 75, 75, SP_SHADE_ENTRY_MS, 0U);
    fb = render_frame();
    CHECK(fb != NULL, "frame allocated");
    if (!fb) {
        return;
    }
    const int v75 = exact_in_annulus(fb, SP_C_CYAN, SP_CX, SP_CY, 180, 188, -1);
    const int l75 = exact_in_annulus(fb, SP_C_AMBER, SP_CX, SP_CY, 180, 188, 1);
    CHECK(v25 > 100 && l25 > 100, "a quarter fill is drawn: %d %d", v25, l25);
    CHECK(v75 > 2 * v25 && l75 > 2 * l25, "three quarters is about three times a quarter: vol %d/%d light %d/%d",
          v75, v25, l75, l25);
    /* the volume arc is on the left rim and brightness on the right, never the other way */
    CHECK(exact_in_annulus(fb, SP_C_CYAN, SP_CX, SP_CY, 180, 188, 1) == 0, "no cyan arc on the right rim");
    CHECK(exact_in_annulus(fb, SP_C_AMBER, SP_CX, SP_CY, 180, 188, -1) == 0, "no amber arc on the left rim");
    /* and the fill is the low end: lower left for volume, lower right for light */
    int upper = 0, lower = 0;
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < SP_CX; ++x) {
            const int dx = x - SP_CX, dy = y - SP_CY;
            const int r = sp_isqrt(dx * dx + dy * dy);
            if (r >= 180 && r <= 188 && fb[(size_t)y * HUD_W + x] == SP_C_CYAN) {
                if (dy < 0) { ++upper; } else { ++lower; }
            }
        }
    }
    CHECK(upper > 0 && lower > upper && upper * 100 > lower * 35 && upper * 100 < lower * 65,
          "75%% fills past the middle, from the bottom: %d above, %d below", upper, lower);
    free(fb);

    /* the entry: nothing at 0, growing, then settled */
    int prev = -1;
    static const uint32_t at[] = {0, 150, 300, 450, 700};
    for (size_t k = 0; k < sizeof at / sizeof *at; ++k) {
        stage_shade(false, 80, 80, at[k], 0U);
        fb = render_frame();
        if (!fb) {
            CHECK(false, "frame allocated");
            return;
        }
        const int n = exact_in_annulus(fb, SP_C_CYAN, SP_CX, SP_CY, 180, 188, -1);
        if (k == 0) {
            CHECK(n == 0, "no volume fill at entry 0: %d", n);
        }
        CHECK(n >= prev, "the volume arc only grows: %d then %d at %u ms", prev, n, (unsigned)at[k]);
        prev = n;
        free(fb);
    }

    /* the button: gold muted, cyan live, no cross-over */
    for (int muted = 0; muted <= 1; ++muted) {
        stage_shade(muted != 0, 50, 50, SP_SHADE_ENTRY_MS, 0U);
        fb = render_frame();
        if (!fb) {
            CHECK(false, "frame allocated");
            return;
        }
        const int gold = exact_in_annulus(fb, SP_C_GOLD, SP_BTN_PRIV_CX, SP_BTN_CY, 0, 64, 0);
        const int cyan = exact_in_annulus(fb, SP_C_CYAN, SP_BTN_PRIV_CX, SP_BTN_CY, 0, 64, 0);
        const int halo = drawn_in_annulus(fb, SP_BTN_PRIV_CX, SP_BTN_CY, 66, 74);
        if (muted) {
            CHECK(gold > 200 && cyan == 0, "muted button is gold only: gold %d cyan %d", gold, cyan);
            CHECK(halo > 100, "muted draws the breathing ring: %d px", halo);
        } else {
            CHECK(cyan > 200 && gold == 0, "live button is cyan only: cyan %d gold %d", cyan, gold);
            CHECK(halo == 0, "live draws no breathing ring: %d px", halo);
        }
        free(fb);
    }

    /* the breath is a bounded, periodic raised cosine that rests at the edge */
    int32_t g0 = 0, g_mid = 0, g_end = 0, g_lo = 1 << 20, g_hi = -1;
    int a0 = 0, a_mid = 0, a_end = 0, a_lo = 1 << 20, a_hi = -1;
    sg_breath(0, &g0, &a0);
    sg_breath(SP_BREATH_MS / 2, &g_mid, &a_mid);
    sg_breath(SP_BREATH_MS, &g_end, &a_end);
    CHECK(g0 == 0 && g_end == 0 && a0 == a_end, "the breath is periodic and starts at rest");
    CHECK(g_mid >= 62 && a_mid < a0, "it swells 4 px and fades at the top: %d px/16, alpha %d vs %d", g_mid,
          a_mid, a0);
    for (uint32_t ms = 0; ms < SP_BREATH_MS; ms += 50) {
        int32_t g;
        int a;
        sg_breath(ms, &g, &a);
        g_lo = g < g_lo ? g : g_lo;
        g_hi = g > g_hi ? g : g_hi;
        a_lo = a < a_lo ? a : a_lo;
        a_hi = a > a_hi ? a : a_hi;
    }
    CHECK(g_lo >= 0 && g_hi <= 64 && a_lo >= 25 && a_hi <= 130, "the breath stays in bounds: grow %d..%d alpha %d..%d",
          g_lo, g_hi, a_lo, a_hi);

    reset_nav();
    s_shade_ms = SP_SHADE_ENTRY_MS;
    s_breath_ms = 0U;
    __atomic_store_n(&s_hud_env_word, 74U, __ATOMIC_RELEASE);
}

/* The shade's clocks: entry starts when it opens and parks when it closes, and
 * the breath advances only with the shade open and the microphone muted, so a
 * closed shade runs nothing and the device can rest. */
static void test_shade_clocks_run_only_while_they_must(void)
{
    uint32_t now = 1000;
    reset_nav();
    __atomic_store_n(&s_hud_env_word, 74U | (1U << 9), __ATOMIC_RELEASE);   /* muted */
    s_shade_ms = SP_SHADE_ENTRY_MS;
    s_breath_ms = 0U;
    s_shade_was_open = false;
    for (int i = 0; i < 20; ++i) {
        now += 70;
        sp_fade_tick(now, 70, 8);
    }
    CHECK(s_breath_ms == 0U, "a closed shade does not breathe, even muted (%u)", (unsigned)s_breath_ms);
    CHECK(s_shade_ms == SP_SHADE_ENTRY_MS && !glass_entry_animating(s_shade_ms, SP_SHADE_ENTRY_MS),
          "a closed shade has no entry running");

    jr_display_nav_down();
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_SHADE, "the shade opens");
    now += 70;
    sp_fade_tick(now, 70, 8);
    CHECK(s_shade_ms == 0U && glass_entry_animating(s_shade_ms, SP_SHADE_ENTRY_MS),
          "opening starts the entry at 0, got %u", (unsigned)s_shade_ms);
    uint32_t last = 0;
    int frames = 0;
    while (glass_entry_animating(s_shade_ms, SP_SHADE_ENTRY_MS) && frames < 100) {
        now += 70;
        sp_fade_tick(now, 70, 8);
        CHECK(s_shade_ms >= last, "the entry never runs backwards");
        last = s_shade_ms;
        ++frames;
    }
    CHECK(s_shade_ms == SP_SHADE_ENTRY_MS && frames >= 5 && frames <= 7, "it parks at the total in about 6 frames at 70 ms (11 at 24 fps), got %d", frames);
    CHECK(s_breath_ms > 0U && s_breath_ms < SP_BREATH_MS, "an open, muted shade breathes (%u)", (unsigned)s_breath_ms);
    for (int i = 0; i < 200; ++i) {
        now += 70;
        sp_fade_tick(now, 70, 8);
        CHECK(s_breath_ms < SP_BREATH_MS, "the phase wraps");
    }

    /* live: the phase holds */
    __atomic_store_n(&s_hud_env_word, 74U, __ATOMIC_RELEASE);
    const uint32_t held = s_breath_ms;
    for (int i = 0; i < 10; ++i) {
        now += 70;
        sp_fade_tick(now, 70, 8);
    }
    CHECK(s_breath_ms == held, "an open, live shade does not breathe");

    /* closing parks the entry; reopening replays it from 0 */
    __atomic_store_n(&s_hud_env_word, 74U | (1U << 9), __ATOMIC_RELEASE);
    jr_display_nav_up();
    now += 70;
    sp_fade_tick(now, 70, 8);
    CHECK(s_shade_ms == SP_SHADE_ENTRY_MS, "closing parks the entry");
    const uint32_t closed = s_breath_ms;
    for (int i = 0; i < 10; ++i) {
        now += 70;
        sp_fade_tick(now, 70, 8);
    }
    CHECK(s_breath_ms == closed, "and the breath stops with it");
    jr_display_nav_down();
    now += 70;
    sp_fade_tick(now, 70, 8);
    CHECK(s_shade_ms == 0U, "reopening replays the entry from 0");

    reset_nav();
    s_shade_ms = SP_SHADE_ENTRY_MS;
    s_breath_ms = 0U;
    s_shade_was_open = false;
    __atomic_store_n(&s_hud_env_word, 74U, __ATOMIC_RELEASE);
}

/* --------------------------------------------------------------- hit test -- */

static void test_shade_hits_resolve_to_controls(void)
{
    reset_nav();
    jr_display_nav_down();

    /* The volume arc is the left rim and fills upward: above the middle is
     * louder, below it quieter. The right rim is the brightness arc, which has
     * no touch action, so a tap there closes the shade like any other. */
    CHECK(jr_display_hit(SP_CX - 190, SP_CY - 60) == JR_DISPLAY_ACT_VOLUME_UP,
          "volume up: upper left rim");
    CHECK(jr_display_hit(SP_CX - 190, SP_CY + 60) == JR_DISPLAY_ACT_VOLUME_DOWN,
          "volume down: lower left rim");
    CHECK(jr_display_hit(SP_CX - 170, SP_CY - 100) == JR_DISPLAY_ACT_VOLUME_UP,
          "volume up near the arc's upper end");
    CHECK(jr_display_hit(SP_CX + 190, SP_CY - 60) == JR_DISPLAY_ACT_DISMISS,
          "the right rim is brightness: not volume");
    CHECK(jr_display_hit(SP_CX + 190, SP_CY + 60) == JR_DISPLAY_ACT_DISMISS,
          "the right rim is brightness: not volume (lower)");
    CHECK(jr_display_hit(SP_CX - 150, SP_CY - SP_VOL_HIT_DY - 20) == JR_DISPLAY_ACT_DISMISS,
          "above the arc is not volume");
    CHECK(jr_display_hit(SP_CX - 100, SP_CY - 60) == JR_DISPLAY_ACT_DISMISS,
          "the icon and number cluster inside the arc is not volume");
    CHECK(jr_display_hit(SP_BTN_PRIV_CX, SP_BTN_CY) ==
              JR_DISPLAY_ACT_PRIVACY_TOGGLE,
          "privacy at its disc's centre");
    CHECK(jr_display_hit(SP_BTN_PRIV_CX + SP_BTN_R - 2, SP_BTN_CY) ==
              JR_DISPLAY_ACT_PRIVACY_TOGGLE,
          "privacy up to its drawn edge");
    CHECK(jr_display_hit(SP_BTN_PRIV_CX + SP_BTN_R + 6, SP_BTN_CY) ==
              JR_DISPLAY_ACT_DISMISS,
          "privacy no further than its drawn edge");
    /* Anywhere else inside an open shade closes it rather than doing nothing. */
    CHECK(jr_display_hit(SP_CX, SP_CY + 180) == JR_DISPLAY_ACT_DISMISS,
          "outside the controls dismisses");

    /* A control that is not presented must not act: the hit test is the
     * geometry the shell actually drew, not a static table. */
    reset_nav();
    CHECK(jr_display_hit(SP_BTN_PRIV_CX, SP_BTN_CY) !=
              JR_DISPLAY_ACT_PRIVACY_TOGGLE,
          "closed shade does not answer");
    /* The outer band belongs to the battery rim, the privacy ring and the
     * choice arcs; the shell must not claim taps there either. */
    CHECK(jr_display_hit(SP_CX, SP_CY - 225) == JR_DISPLAY_ACT_NONE,
          "outer band is not the shell's");
}

/* Every ring screen's detail sheet must be ABOUT that screen.
 *
 * WATCH and POWER used to fall through to the composer's `default:` case,
 * which built the (since deleted) SETTINGS sheet — so tapping the clock or
 * the battery opened a sheet headed SETTINGS listing privacy, link, update
 * and slot rows. The screen you touched was not the screen you got.
 *
 * This asserts the heading of each space individually AND that no space
 * composes an EMPTY sheet, which is what the default case now yields: a new
 * space added without its own case would show nothing, and this test is what
 * refuses it. DESK heads with its task, so it is covered by its own test. */
static void test_every_space_composes_its_own_sheet(void)
{
    static const struct { int space; const char *head; } expect[] = {
        { JR_DISPLAY_SPACE_JARVIS,   "SESSION"  },
        { JR_DISPLAY_SPACE_WATCH,    "TIME"     },
        { JR_DISPLAY_SPACE_WEATHER,  "WEATHER"  },
        { JR_DISPLAY_SPACE_STATUS,   "STATUS"   },
        { JR_DISPLAY_SPACE_ACTIVITY, "ACTIVITY" },
    };
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; ++i) {
        sp_compose_detail(expect[i].space);
        CHECK(strcmp(s_detail_head, expect[i].head) == 0,
              "space %d should head '%s', got '%s'",
              expect[i].space, expect[i].head, s_detail_head);
        CHECK(s_detail_rows > 0, "space %d composed no rows", expect[i].space);
    }

    /* No space may fall into the empty default. */
    for (int space = 0; space < (int)JR_DISPLAY_SPACE_COUNT; ++space) {
        sp_compose_detail(space);
        CHECK(s_detail_head[0] != '\0' && s_detail_rows > 0,
              "space %d has no sheet of its own", space);
    }

    /* WATCH must not present a duration where a wall time belongs: an
     * unsynced clock reads --:--, never 0:00, which looks like midnight. */
    jr_display_clock_set(false, 0, 0, 0);
    sp_compose_detail(JR_DISPLAY_SPACE_WATCH);
    CHECK(strcmp(s_detail_value[1], "--:--") == 0,
          "unsynced clock should read --:--, got '%s'", s_detail_value[1]);
    jr_display_clock_set(true, 0, 7, 0);
    sp_compose_detail(JR_DISPLAY_SPACE_WATCH);
    CHECK(strcmp(s_detail_value[1], "00:07") == 0,
          "midnight hour should zero-pad, got '%s'", s_detail_value[1]);
}

/* The clock's disc clear must not eat a shell surface the owner opened.
 *
 * apply_clock_overlay clears every pixel inside JR_DISPLAY_SHELL_R_MAX and
 * runs AFTER apply_space_overlay, so on WATCH it wiped whatever the shell had
 * just drawn. The first fix rescued the detail sheet only, leaving the CONTROL
 * SHADE invisible-but-live: jr_display_hit still routed taps to a volume arc
 * and a privacy button nobody could see.
 *
 * Two traps this test had to avoid, both of which made an earlier version of
 * it pass while the bug was present:
 *   - render_frame() calls only apply_space_overlay, so the clock never runs
 *     and nothing can fail. The real flush order is composed by hand instead.
 *   - "some pixels survive inside the disc" is satisfied by the clock's own
 *     HANDS, which are drawn after the clear. So this compares POSITIONS: the
 *     pixels the shell drew must still hold the shell's values afterwards.
 * Verified by mutation — reverting the guard to detail-only fails this. */
static void test_clock_clear_spares_open_shell_surfaces(void)
{
    static const int overlays[] = {
        JR_DISPLAY_OVERLAY_SHADE, JR_DISPLAY_OVERLAY_DETAIL,
    };
    static const char *const names[] = { "shade", "detail" };
    const size_t px = (size_t)HUD_W * HUD_H;

    for (size_t o = 0; o < sizeof overlays / sizeof *overlays; ++o) {
        uint16_t *base = malloc(px * sizeof *base);
        uint16_t *withclock = malloc(px * sizeof *withclock);
        if (!base || !withclock) {
            printf("FAIL %s: allocation failed\n", __func__);
            g_failures++;
            free(base); free(withclock);
            return;
        }
        for (int pass = 0; pass < 2; ++pass) {
            uint16_t *fb = pass == 0 ? base : withclock;
            stage_power(JR_DISPLAY_OTA_IDLE, 0U, overlays[o]);
            s_space_from = (uint8_t)JR_DISPLAY_SPACE_WATCH;
            s_space_to = (uint8_t)JR_DISPLAY_SPACE_WATCH;
            s_detail_space = (uint8_t)JR_DISPLAY_SPACE_WATCH;
            __atomic_store_n(&s_nav_word,
                             (uint32_t)JR_DISPLAY_SPACE_WATCH |
                                 ((uint32_t)overlays[o] << NAV_OVL_SHIFT),
                             __ATOMIC_RELEASE);
            /* Drive the eases directly: sp_fade_tick is time-based and this
             * test must not depend on a clock it cannot advance. */
            s_shade_ease = overlays[o] == JR_DISPLAY_OVERLAY_SHADE ? 256 : 0;
            s_detail_ease = overlays[o] == JR_DISPLAY_OVERLAY_DETAIL ? 256 : 0;
            s_space_veil = 256;
            jr_display_clock_set(true, 10, 8, 30);
            s_clock_ease = 256;
    s_watch_s = 256;
            sp_compose();

            for (size_t i = 0; i < px; ++i) {
                fb[i] = 0U;         /* black under the sheet: the veil blends real pixels, not poison */
            }
            for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
                const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
                uint16_t *strip = fb + (size_t)y * HUD_W;
                if (pass == 1) {
                    /* under a sheet apply_hud_overlay draws the watch BEFORE the
                     * shell, so the shell's veil dims it and the sheet lands on top */
                    apply_clock_overlay(&s_display, y, y2, strip);
                }
                apply_space_overlay(&s_display, y, y2, strip);
            }
        }

        int drawn = 0, survived = 0;
        for (size_t i = 0; i < px; ++i) {
            if (base[i] == POISON || base[i] == 0U) {
                continue;
            }
            const int r = radius_of((int)i);
            if (r > 40 && r <= JR_DISPLAY_SHELL_R_MAX) {
                drawn++;
                if (withclock[i] == base[i]) {
                    survived++;
                }
            }
        }
        /* Empty is not pass: if the surface drew nothing, the comparison
         * proves nothing and must fail rather than read as success. */
        CHECK(drawn > 500, "WATCH + %s drew only %d pixels — test is vacuous",
              names[o], drawn);
        CHECK(drawn > 0 && survived * 10 >= drawn * 8,
              "WATCH + %s: %d of %d shell pixels survived the clock clear — "
              "the surface is invisible but still hit-tested",
              names[o], survived, drawn);
        free(base); free(withclock);
    }
}

/* ------------------------------------------------------------ batch N8 -- */

/* Stage an arbitrary ring screen at rest: no slide, no sheet, no shade, and
 * no veil, so the only writer outside the focal object is whatever the test
 * is measuring. */
static void stage_space(int space)
{
    stage_power(JR_DISPLAY_OTA_IDLE, 0U, JR_DISPLAY_OVERLAY_NONE);
    s_space_from = (uint8_t)space;
    s_space_to = (uint8_t)space;
    s_detail_space = (uint8_t)space;
    s_space_veil = 0;
    __atomic_store_n(&s_nav_word, (uint32_t)space, __ATOMIC_RELEASE);
    __atomic_store_n(&s_hud_env_word, 74U, __ATOMIC_RELEASE);
    sp_compose();
}

/* ------------------------------------------------------- DESK, while live -- */

/* DESK is on the ring only while JR_DISPLAY_SHELL_AGENT is set. Dark, the
 * ring steps over it in both directions and a lap is one screen shorter;
 * live, the full ring is back. Both laps are walked and every landing named,
 * so a wrap that quietly re-admits DESK cannot pass. Mutation: dropping
 * either sp_desk_live() test in nav_step fails the matching half. */
/* THE WATCH STYLE LIVES IN THE NAV WORD (2026-09-02): bits 17-18, wrapping
 * both ways, untouched by every nav step, and it reaches the render path —
 * DIVER's cream triangle at 12 o'clock is painted on WATCH and nothing else
 * paints cream. Mutation-checked: a step that drops the style bits fails the
 * "survives nav_next" check; a renderer that ignores the style fails the
 * triangle count. */
static void test_watch_style_is_three_nav_bits_that_wrap(void)
{
    reset_nav();
    CHECK(jr_display_watch_style() == JR_WATCH_JARVIS, "cold start is JARVIS");
    jr_display_watch_style_set(JR_WATCH_DIVER);
    CHECK(((__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) >> 17) & 7u) == 1u,
          "DIVER is not bit pattern 001 at bit 17 (word 0x%08x)",
          (unsigned)__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE));
    jr_display_watch_style_set(JR_WATCH_FUTURE);
    CHECK(((__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) >> 17) & 7u) == 5u,
          "FUTURE is not bit pattern 101 at bit 17");
    CHECK((__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) >> 20) == 0u,
          "the style spilled above bit 19");
    jr_display_watch_style_set(JR_WATCH_CELESTIAL);
    CHECK(((__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) >> 17) & 7u) == 7u,
          "CELESTIAL is not bit pattern 111 at bit 17");
    CHECK((__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) >> 20) == 0u,
          "the last style spilled above bit 19");
    jr_display_watch_style_set(JR_WATCH_DIVER);
    static const jr_watch_style_t walk[] = {
        JR_WATCH_DRESS, JR_WATCH_PILOT, JR_WATCH_MINIMAL, JR_WATCH_FUTURE,
        JR_WATCH_SKELETON, JR_WATCH_CELESTIAL, JR_WATCH_JARVIS, JR_WATCH_DIVER };
    for (size_t i = 0; i < sizeof walk / sizeof *walk; ++i) {
        jr_display_watch_style_step(1);
        CHECK(jr_display_watch_style() == walk[i], "step %zu right landed on %d, wanted %d",
              i, (int)jr_display_watch_style(), (int)walk[i]);
    }
    jr_display_watch_style_step(-1);
    CHECK(jr_display_watch_style() == JR_WATCH_JARVIS, "left of DIVER is JARVIS");
    jr_display_watch_style_step(-1);
    CHECK(jr_display_watch_style() == JR_WATCH_CELESTIAL, "left of JARVIS wraps to CELESTIAL");
    jr_display_nav_next();
    jr_display_nav_up();
    jr_display_nav_home();
    CHECK(jr_display_watch_style() == JR_WATCH_CELESTIAL,
          "the style did not survive next/up/home (now %d)", (int)jr_display_watch_style());
    CHECK(jr_display_nav_space() == JR_DISPLAY_SPACE_JARVIS, "home still goes home");
    jr_display_watch_style_set((jr_watch_style_t)JR_WATCH_STYLE_COUNT);
    CHECK(jr_display_watch_style() == JR_WATCH_CELESTIAL, "an out-of-range style is refused");
    static const char *const names[] = { "JARVIS", "DIVER", "DRESS", "PILOT", "MINIMAL", "FUTURE",
                                         "SKELETON", "CELESTIAL" };
    for (int i = 0; i < (int)JR_WATCH_STYLE_COUNT; ++i) {
        CHECK(strcmp(jr_display_watch_style_name((jr_watch_style_t)i), names[i]) == 0,
              "style %d is named %s", i, jr_display_watch_style_name((jr_watch_style_t)i));
    }
    reset_nav();
}

static void render_watch_frame_at(uint16_t *fb, int style, jr_face_t shown,
                                  int hh, int mm, int ss)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    stage_power(JR_DISPLAY_OTA_IDLE, 0U, JR_DISPLAY_OVERLAY_NONE);
    s_space_from = (uint8_t)JR_DISPLAY_SPACE_WATCH;
    s_space_to = (uint8_t)JR_DISPLAY_SPACE_WATCH;
    __atomic_store_n(&s_nav_word, (uint32_t)JR_DISPLAY_SPACE_WATCH, __ATOMIC_RELEASE);
    jr_display_watch_style_set((jr_watch_style_t)style);
    s_space_prog = s_space_ease = 256;                  /* settled: no slide, no style dip */
    s_style_prog = 256;
    s_style_shown = s_style_seen = s_style_from = (uint8_t)style;
    s_style_black = 0;
    s_space_black = 0;
    s_display.shown_face = shown;
    s_shade_ease = 0;
    s_detail_ease = 0;
    s_space_veil = 0;                 /* a settled watch: no sheet, no veil */
    jr_display_clock_set(true, hh, mm, ss);
    s_clock_shown_word = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
    s_clock_ease = 256;
    s_watch_s = 256;
    sp_compose();
    watch_compose();                 /* the frame-start latch, as the flush does */
    for (size_t k = 0; k < px; ++k) fb[k] = 0x4208;     /* "the art" */
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        uint16_t *strip = fb + (size_t)y * HUD_W;
        apply_space_overlay(&s_display, y, y2, strip);
        apply_clock_overlay(&s_display, y, y2, strip);
    }
}

/* WATCH on a style with no baked dial (JARVIS, MINIMAL) asks the presenter for
 * a frame every pass, as a blitted dial does, instead of riding the face clip's
 * own ~8 fps tick; a baked dial does not need to (its blit refreshes), and
 * neither does a glass that is not showing the watch. */
static void test_watch_without_a_dial_asks_for_frames(void)
{
    static const struct { int style; int clock; bool expect; const char *what; } k[] = {
        { JR_WATCH_JARVIS,  1, true,  "JARVIS settled" },
        { JR_WATCH_MINIMAL, 1, true,  "MINIMAL settled" },
        { JR_WATCH_DRESS,   1, false, "DRESS (its blit refreshes)" },
        { JR_WATCH_SKELETON, 1, false, "SKELETON (its blit refreshes)" },
        { JR_WATCH_MINIMAL, 0, false, "MINIMAL with the clock gated off" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; ++i) {
        s_space_from = s_space_to = (uint8_t)JR_DISPLAY_SPACE_WATCH;
        __atomic_store_n(&s_nav_word, (uint32_t)JR_DISPLAY_SPACE_WATCH, __ATOMIC_RELEASE);
        jr_display_watch_style_set((jr_watch_style_t)k[i].style);
        s_space_prog = s_space_ease = 256;
        s_style_prog = 256;
        s_style_shown = s_style_seen = s_style_from = (uint8_t)k[i].style;
        jr_display_clock_set(k[i].clock != 0, 6, 30, 15);
        s_clock_ease = k[i].clock ? 256 : 0;
        watch_publish();
        CHECK((__atomic_load_n(&s_watch_tick, __ATOMIC_RELAXED) != 0u) == k[i].expect,
              "%s: tick flag %u, wanted %d", k[i].what,
              (unsigned)s_watch_tick, (int)k[i].expect);
        s_dial_blit = false;
        s_parked = false;
        CHECK(presenter_ticks_the_glass() == k[i].expect,
              "%s: the presenter would %s the glass every pass", k[i].what,
              presenter_ticks_the_glass() ? "invalidate" : "leave");
    }
    /* away from WATCH, the clock off: nothing to ask for */
    s_space_from = s_space_to = (uint8_t)JR_DISPLAY_SPACE_JARVIS;
    __atomic_store_n(&s_nav_word, (uint32_t)JR_DISPLAY_SPACE_JARVIS, __ATOMIC_RELEASE);
    jr_display_clock_set(false, 0, 0, 0);
    s_clock_ease = 0;
    s_clock_prog = 0;
    watch_publish();
    CHECK(__atomic_load_n(&s_watch_tick, __ATOMIC_RELAXED) == 0u, "away from WATCH the flag is %u",
          (unsigned)s_watch_tick);
    CHECK(!presenter_ticks_the_glass(), "away from WATCH the presenter ticks the glass");
    s_dial_blit = true;
    CHECK(presenter_ticks_the_glass(), "a blitted dial no longer ticks the glass");
    s_dial_blit = false;
    s_parked = true;
    CHECK(presenter_ticks_the_glass(), "a parked backdrop no longer ticks the glass");
    s_parked = false;
    reset_nav();
    jr_display_clock_set(false, 0, 0, 0);
    watch_publish();
}

static void render_watch_frame(uint16_t *fb, int style, jr_face_t shown)
{
    render_watch_frame_at(fb, style, shown, 6, 30, 15);
}

/* Read FUTURE's weather line back off the glass: the top line of the cell is
 * `want` exactly when every cyan pixel in the line's rows sits where the
 * shell's own glyph sampler puts `want` at the cell's centre, and nowhere
 * else. A different string lands on different pixels (the run is centred on
 * its own length), so this is "the cell reads X", not "something is lit". */
static bool future_wx_line_reads(const uint16_t *fb, const char *want,
                                 size_t *ink_out)
{
    const int n = (int)strlen(want);
    const int x0 = HUD_WATCH_CELL_WX_CX - (6 * 2 * n) / 2;
    const int y0 = HUD_WATCH_CELL_WX_CY - 16;
    const uint16_t cyan = sp_tint(&s_display, 0x073F, 255);
    uint16_t ref[HUD_W];
    size_t ink = 0, wrong = 0;
    for (int y = y0; y < y0 + TEXT_H; ++y) {
        memset(ref, 0, sizeof ref);
        sp_text_row(ref, y, want, n, x0, y0, 2, 0xFFFF);
        for (int x = HUD_WATCH_CELL_WX_X0 + 1; x < HUD_WATCH_CELL_WX_X1 - 1; ++x) {
            const bool lit = fb[(size_t)y * HUD_W + x] == cyan;
            const bool exp = ref[x] != 0;
            if (exp) ink++;
            if (lit != exp) wrong++;
        }
    }
    *ink_out = ink;
    return wrong == 0 && ink > 0;
}

/* F3: the FUTURE weather cell says one whole short word. The panel showed
 * "75* LIGHT DRIZZ" (2026-09-05), then, after the cut was declared with a mark,
 * "82* MAINLY." on the watch (2026-11-10): a condition cut mid-phrase with a
 * full stop, on a dial. The cell now folds the condition the display holds to
 * a small fixed vocabulary (CLEAR, CLOUDY, RAIN, SNOW, FOG, STORM), so it never
 * holds a cut word and never ends in a stop; a condition with no word here
 * leaves the temperature alone. The WEATHER screen's headline still fits by the
 * old rule (a word, or a marked cut). 10:10:00 keeps every hand out of the cell
 * below centre; the cyan the hands are drawn in is the cell's own cyan. */
static void test_future_weather_cell_shortens_instead_of_cutting(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; return; }
    static const struct { const char *cond; int temp; const char *want; } cases[] = {
        { "LIGHT DRIZZLE",          75, "75* RAIN" },
        { "MODERATE RAIN SHOWERS",  75, "75* RAIN" },
        { "THUNDERSTORM",           75, "75* STORM" },
        { "FREEZING FOG",           75, "75* FOG" },
        { "HEAVY RAIN",             75, "75* RAIN" },
        { "HEAVY SLEET",           100, "100* SNOW" },
        { "SNOW SHOWERS",           75, "75* SNOW" },         /* snow before showers */
        { "OVERCAST",              100, "100* CLOUDY" },
        { "OVERCAST",              -12, "-12* CLOUDY" },
        { "PARTLY CLOUDY",          75, "75* CLOUDY" },
        { "MAINLY CLEAR",           82, "82* CLEAR" },        /* the capture's cut word */
        { "CLEAR",                  75, "75* CLEAR" },
        { "UNKNOWN",                75, "75*" },              /* no word: the temperature alone */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        char line[24];
        const int n = watch_wx_line(line, sizeof line, cases[i].temp, cases[i].cond);
        CHECK(strcmp(line, cases[i].want) == 0 && n == (int)strlen(cases[i].want),
              "%s at %d composes '%s' (%d), wanted '%s'",
              cases[i].cond, cases[i].temp, line, n, cases[i].want);
        CHECK(n <= WATCH_WX_GLYPHS, "'%s' is %d glyphs in a %d-glyph cell",
              line, n, WATCH_WX_GLYPHS);
        /* never a stop, never a cut word: the part after the temperature is a whole
         * word of the vocabulary or nothing */
        const char *word = strchr(line, ' ');
        CHECK(line[n - 1] != '.', "'%s' ends in a stop", line);
        CHECK(word == NULL || strcmp(word + 1, "CLEAR") == 0 || strcmp(word + 1, "CLOUDY") == 0 ||
              strcmp(word + 1, "RAIN") == 0 || strcmp(word + 1, "SNOW") == 0 ||
              strcmp(word + 1, "FOG") == 0 || strcmp(word + 1, "STORM") == 0,
              "'%s' holds a word outside the vocabulary", line);
    }
    /* every condition jr_weather_condition can answer, through the real function's
     * vocabulary: a whole word or the temperature alone, and no stop */
    static const char *const all_conditions[] = {
        "CLEAR", "MAINLY CLEAR", "PARTLY CLOUDY", "OVERCAST", "FOG", "RIME FOG", "LIGHT DRIZZLE",
        "DRIZZLE", "HEAVY DRIZZLE", "FREEZING DRIZZLE", "LIGHT RAIN", "RAIN", "HEAVY RAIN",
        "FREEZING RAIN", "LIGHT SNOW", "SNOW", "HEAVY SNOW", "SNOW GRAINS", "LIGHT SHOWERS",
        "SHOWERS", "HEAVY SHOWERS", "SNOW SHOWERS", "THUNDERSTORM", "UNKNOWN", "",
    };
    for (size_t i = 0; i < sizeof all_conditions / sizeof *all_conditions; ++i) {
        char line[24];
        const int n = watch_wx_line(line, sizeof line, 75, all_conditions[i]);
        CHECK(n > 0 && line[n - 1] != '.' && strncmp(line, "75*", 3) == 0,
              "%s composes '%s'", all_conditions[i], line);
        if (strcmp(all_conditions[i], "UNKNOWN") != 0 && all_conditions[i][0] != '\0') {
            CHECK(strchr(line, ' ') != NULL, "%s lost its word: '%s'", all_conditions[i], line);
        }
    }
    /* The setter keeps the whole phrase: a 21-glyph condition survives it. */
    set_weather(true, 75, 82, 68, JR_DISPLAY_SKY_RAIN, "MODERATE RAIN SHOWERS",
                (uint32_t)(s_fake_us / 1000));
    CHECK(strcmp(s_weather[__atomic_load_n(&s_weather_slot, __ATOMIC_ACQUIRE) & 1u].condition,
                 "MODERATE RAIN SHOWERS") == 0,
          "the condition was cut on the way in: '%s'",
          s_weather[__atomic_load_n(&s_weather_slot, __ATOMIC_ACQUIRE) & 1u].condition);
    /* The WEATHER headline: the word fitted at a word, and no number — the
     * disc already prints it large (the panel read "76" over "OVERCAST 76"). */
    static const struct { const char *cond; const char *want; } head[] = {
        { "LIGHT DRIZZLE",         "DRIZZLE" },
        { "MODERATE RAIN SHOWERS", "RAIN SHOWERS" },
        { "THUNDERSTORMS",         "THUNDERSTOR." },
        { "OVERCAST",              "OVERCAST" },
    };
    for (size_t i = 0; i < sizeof head / sizeof *head; ++i) {
        set_weather(true, 75, 82, 68, JR_DISPLAY_SKY_RAIN, head[i].cond,
                    (uint32_t)(s_fake_us / 1000));
        sp_compose_weather();
        CHECK(strcmp(s_wx_head, head[i].want) == 0, "%s heads '%s', wanted '%s'",
              head[i].cond, s_wx_head, head[i].want);
    }
    /* On the glass, not just in the buffer: stage the weather and read the
     * cell back. Both the dropped qualifier and the marked cut must render. */
    static const struct { const char *cond; const char *want; } glass[] = {
        { "LIGHT DRIZZLE", "75* RAIN" },
        { "THUNDERSTORM",  "75* STORM" },
    };
    for (size_t i = 0; i < sizeof glass / sizeof *glass; ++i) {
        set_weather(true, 75, 82, 68, JR_DISPLAY_SKY_RAIN, glass[i].cond,
                    (uint32_t)(s_fake_us / 1000));
        render_watch_frame_at(fb, JR_WATCH_FUTURE, JR_FACE_DIAL_FUTURE, 10, 10, 0);
        size_t ink = 0;
        const bool reads = future_wx_line_reads(fb, glass[i].want, &ink);
        CHECK(reads, "with %s the cell does not read '%s'", glass[i].cond, glass[i].want);
        CHECK(ink > 200, "'%s' put only %zu ink px in the cell", glass[i].want, ink);
    }
    set_weather(false, 0, 0, 0, JR_DISPLAY_SKY_UNKNOWN, "", 0U);
    s_display.shown_face = JR_FACE_IDLE;
    free(fb);
    reset_nav();
}

/* The words are composed once a frame (watch_compose) and placed per strip:
 * the strip render of FUTURE, words and all, equals one whole-frame call,
 * and recomposing with new weather moves the cell. */
static void test_watch_words_are_composed_once_and_strip_invariant(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    uint16_t *whole = malloc(px * sizeof *whole);
    if (!fb || !whole) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; free(fb); free(whole); return; }
    set_weather(true, 75, 82, 68, JR_DISPLAY_SKY_RAIN, "OVERCAST", (uint32_t)(s_fake_us / 1000));
    render_watch_frame_at(fb, JR_WATCH_FUTURE, JR_FACE_DIAL_FUTURE, 10, 10, 0);
    for (size_t k = 0; k < px; ++k) whole[k] = 0x4208;
    apply_space_overlay(&s_display, 0, HUD_H, whole);
    apply_clock_overlay(&s_display, 0, HUD_H, whole);
    CHECK(memcmp(whole, fb, px * sizeof *fb) == 0, "FUTURE strips differ from the whole frame");
    size_t ink = 0;
    CHECK(future_wx_line_reads(fb, "75* CLOUDY", &ink) && ink > 200,
          "the cell does not read the composed line (%zu ink px)", ink);
    /* new weather, no recompose: the strips still say OVERCAST */
    set_weather(true, 75, 82, 68, JR_DISPLAY_SKY_CLEAR, "CLEAR", (uint32_t)(s_fake_us / 1000));
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        for (size_t k = (size_t)y * HUD_W; k < (size_t)y2 * HUD_W; ++k) fb[k] = 0x4208;
        apply_space_overlay(&s_display, y, y2, fb + (size_t)y * HUD_W);
        apply_clock_overlay(&s_display, y, y2, fb + (size_t)y * HUD_W);
    }
    CHECK(memcmp(whole, fb, px * sizeof *fb) == 0, "a strip recomposed the words by itself");
    /* the frame-start latch moves it */
    watch_compose();
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        for (size_t k = (size_t)y * HUD_W; k < (size_t)y2 * HUD_W; ++k) fb[k] = 0x4208;
        apply_space_overlay(&s_display, y, y2, fb + (size_t)y * HUD_W);
        apply_clock_overlay(&s_display, y, y2, fb + (size_t)y * HUD_W);
    }
    CHECK(future_wx_line_reads(fb, "75* CLEAR", &ink) && ink > 150,
          "after the latch the cell does not read CLEAR (%zu ink px)", ink);
    set_weather(false, 0, 0, 0, JR_DISPLAY_SKY_UNKNOWN, "", 0U);
    s_display.shown_face = JR_FACE_IDLE;
    free(fb); free(whole);
    reset_nav();
}

/* Shadows follow the cadence: at the 24 fps awake rung the hands cast them
 * — neutral pixels darker than the mid-grey dial beside the gold, which no
 * gold flank can be (its blue channel collapses first) and the hub's own
 * dark ring is inside r20 — and at the ambient rung (main's 12) they do
 * not, while the hands are the same hands. */
static void test_watch_shadows_only_at_the_awake_cadence(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; return; }
    size_t dark[2] = { 0, 0 }, lit[2] = { 0, 0 };
    static const uint8_t fps[2] = { JR_DISPLAY_RENDER_FPS, 12U };
    for (int k = 0; k < 2; ++k) {
        __atomic_store_n(&s_render_fps, fps[k], __ATOMIC_RELEASE);
        render_watch_frame_at(fb, JR_WATCH_DRESS, JR_FACE_DIAL_DRESS, 10, 10, 0);
        for (int y = 0; y < HUD_H; ++y) {
            for (int x = 0; x < HUD_W; ++x) {
                const int dx = x - 232, dy = y - 232;
                /* the hands reach r166 (+3 for the shadow); the orbit track
                 * at r186-194 and the hub ring inside r13 are not the hands */
                if (dx * dx + dy * dy > 178 * 178 || dx * dx + dy * dy < 20 * 20) continue;
                const uint16_t p = fb[(size_t)y * HUD_W + x];
                if (p == 0x4208) continue;
                lit[k]++;
                const int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                const int r8 = (r << 3) | (r >> 2), g8 = (g << 2) | (g >> 4), b8 = (b << 3) | (b >> 2);
                const bool neutral = r8 - b8 <= 16 && b8 - r8 <= 16 && g8 - r8 <= 20 && r8 - g8 <= 20;
                if (neutral && r8 + g8 + b8 < 190) dark[k]++;
            }
        }
    }
    __atomic_store_n(&s_render_fps, JR_DISPLAY_RENDER_FPS, __ATOMIC_RELEASE);
    CHECK(dark[0] > dark[1] + 300, "awake cast %zu dark px, ambient %zu: no shadow at 24 fps",
          dark[0], dark[1]);
    /* what is left at ambient is the dark flank's own anti-aliased edge over
     * the grey, under a hundred pixels on two hands; a shadow is hundreds */
    CHECK(dark[1] < 150, "ambient still casts %zu dark px", dark[1]);
    CHECK(lit[1] > 1000 && lit[0] > lit[1] + 300,
          "hands at ambient lit %zu px (awake %zu): the shadow is not the difference", lit[1], lit[0]);
    s_display.shown_face = JR_FACE_IDLE;
    free(fb);
    reset_nav();
}

/* THE DAY ARC. The JARVIS dial draws the daylight on its outer band, from the
 * published sunrise round the top to sunset, and a dot for now. No other dial
 * does: on a baked dial the band ran across PILOT's tick track and FUTURE's
 * rings (2026-11-10), and MINIMAL's own minute ticks sit on it. No dial draws
 * anything without a sun. Sampled by angle on the band's centre line (r206),
 * away from where the hands point (13:15:45 - hour near 1, minute at 3, seconds
 * at 9 - so noon and midnight are clear). JARVIS paints its own black disc; the
 * baked styles are drawn over the flat grey stand-in art, which must be left
 * exactly as it was. */
static uint16_t sun_px(const uint16_t *fb, int angle_256)
{
    /* 1/256 turn, clockwise from 3 o'clock - the shell's own sine, so the
     * sample lands where the renderer put the pixel */
    const int x = SP_CX + ((sp_cos(angle_256) * 206) >> 15);
    const int y = SP_CY + ((sp_sin(angle_256) * 206) >> 15);
    return fb[(size_t)y * HUD_W + x];
}

static size_t sun_band_ink(const uint16_t *fb, uint16_t base)
{
    size_t n = 0;
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const int dx = x - 232, dy = y - 232, r2 = dx * dx + dy * dy;
            if (r2 < 203 * 203 || r2 > 209 * 209) continue;
            if (fb[(size_t)y * HUD_W + x] != base) n++;
        }
    }
    return n;
}

static void test_watch_day_arc_is_the_sun_on_the_jarvis_dial_only(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; return; }

    /* 07:02 -> 19:35, the real Fort Lauderdale day of 2026-09-05 */
    jr_display_sun_set(422, 1175);
    render_watch_frame_at(fb, JR_WATCH_JARVIS, JR_FACE_IDLE, 13, 15, 45);
    const size_t with = sun_band_ink(fb, 0x0000);
    CHECK(with > 1500, "JARVIS with a sun inks %zu px on the band (want > 1500)", with);
    CHECK(sun_px(fb, 192) != 0x0000, "noon (12 o'clock) is inside the daylight");
    CHECK(sun_px(fb, 64) == 0x0000, "midnight (6 o'clock) is night: nothing on the band");
    /* 07:02 is angle 139, 19:35 is 17: just inside each end is day, just
     * outside is night */
    CHECK(sun_px(fb, 143) != 0x0000, "just after sunrise is day");
    CHECK(sun_px(fb, 133) == 0x0000, "before sunrise is night");
    CHECK(sun_px(fb, 13) != 0x0000, "just before sunset is day");
    CHECK(sun_px(fb, 23) == 0x0000, "after sunset is night");
    /* the dot for 13:15 (75 min past noon) sits on the band at angle
     * 192 + 75*256/1440 = 205, brighter than the band */
    const uint16_t band = sun_px(fb, 215), dot = sun_px(fb, 205);
    CHECK(dot != band && dot != 0x0000, "now is a dot on the band (band %04x, dot %04x)", band, dot);
    /* the dial's own ticks end inside the band: the day arc paints over none of them */
    size_t tick_px_in_band = 0;
    jr_display_sun_set(-1, -1);
    render_watch_frame_at(fb, JR_WATCH_JARVIS, JR_FACE_IDLE, 13, 15, 45);
    tick_px_in_band = sun_band_ink(fb, 0x0000);
    CHECK(tick_px_in_band < 200, "JARVIS without a sun inks %zu px on the band (want < 200)", tick_px_in_band);
    jr_display_sun_set(422, 1175);

    /* every other dial, with the sun published: the band is not touched */
    static const struct { jr_watch_style_t style; jr_face_t face; const char *name; } others[] = {
        { JR_WATCH_DIVER, JR_FACE_DIAL_DIVER, "DIVER" },
        { JR_WATCH_DRESS, JR_FACE_DIAL_DRESS, "DRESS" },
        { JR_WATCH_PILOT, JR_FACE_DIAL_PILOT, "PILOT" },
        { JR_WATCH_FUTURE, JR_FACE_DIAL_FUTURE, "FUTURE" },
        { JR_WATCH_SKELETON, JR_FACE_DIAL_SKELETON, "SKELETON" },
        { JR_WATCH_CELESTIAL, JR_FACE_DIAL_CELESTIAL, "CELESTIAL" },
    };
    for (size_t i = 0; i < sizeof others / sizeof *others; ++i) {
        render_watch_frame_at(fb, others[i].style, others[i].face, 13, 15, 45);
        CHECK(sun_px(fb, 192) == 0x4208 && sun_band_ink(fb, 0x4208) < 200,
              "%s draws no day arc over its art (%zu px on the band)", others[i].name,
              sun_band_ink(fb, 0x4208));
    }
    render_watch_frame_at(fb, JR_WATCH_MINIMAL, JR_FACE_IDLE, 13, 15, 45);
    {
        /* MINIMAL paints its own dial, ticks at r196-208 included: with the sun and
         * without, the band is the same frame */
        uint16_t *ref = malloc(px * sizeof *ref);
        if (ref) {
            memcpy(ref, fb, px * sizeof *ref);
            jr_display_sun_set(-1, -1);
            render_watch_frame_at(fb, JR_WATCH_MINIMAL, JR_FACE_IDLE, 13, 15, 45);
            CHECK(memcmp(ref, fb, px * sizeof *ref) == 0, "MINIMAL draws a day arc over its minute ticks");
            free(ref);
        }
    }

    /* no sun published: nothing on the band beyond the hands' own reach */
    jr_display_sun_set(-1, -1);
    render_watch_frame_at(fb, JR_WATCH_JARVIS, JR_FACE_IDLE, 13, 15, 45);
    CHECK(sun_px(fb, 192) == 0x0000, "JARVIS without a sun has no arc");
    /* a sunset before sunrise is refused, not drawn backwards */
    jr_display_sun_set(1175, 422);
    render_watch_frame_at(fb, JR_WATCH_JARVIS, JR_FACE_IDLE, 13, 15, 45);
    CHECK(sun_px(fb, 192) == 0x0000, "an inverted sun is not a night arc");

    s_display.shown_face = JR_FACE_IDLE;
    free(fb);
    reset_nav();
}

static void test_watch_style_reaches_the_glass(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; return; }
    const uint16_t cream = (uint16_t)(((232 & 0xF8) << 8) | ((226 & 0xFC) << 3) | (200 >> 3));
    static const int styles[] = { JR_WATCH_JARVIS, JR_WATCH_DIVER };
    int tri[2] = { 0, 0 };
    for (int k = 0; k < 2; ++k) {
        render_watch_frame(fb, styles[k], JR_FACE_IDLE);
        for (int y = 52; y <= 78; ++y) {
            for (int x = 205; x <= 259; ++x) {
                if (fb[(size_t)y * HUD_W + x] == cream) tri[k]++;
            }
        }
    }
    CHECK(tri[0] == 0, "JARVIS painted %d cream px at 12 o'clock", tri[0]);
    CHECK(tri[1] > 150, "DIVER painted only %d cream px at 12 o'clock", tri[1]);
    free(fb);
    reset_nav();
}

/* The dial is a face: on WATCH with the clock published, a style with baked
 * art asks the face pipeline for its dial; JARVIS and MINIMAL ask for
 * nothing; off WATCH, or with the clock off, nothing either. */
static void test_watch_dial_face_follows_the_style_and_the_screen(void)
{
    reset_nav();
    jr_display_clock_set(true, 6, 30, 15);
    __atomic_store_n(&s_nav_word, (uint32_t)JR_DISPLAY_SPACE_WATCH, __ATOMIC_RELEASE);
    static const struct { jr_watch_style_t st; jr_face_t face; } want[] = {
        { JR_WATCH_JARVIS,  JR_FACE_COUNT },
        { JR_WATCH_DIVER,   JR_FACE_DIAL_DIVER },
        { JR_WATCH_DRESS,   JR_FACE_DIAL_DRESS },
        { JR_WATCH_PILOT,   JR_FACE_DIAL_PILOT },
        { JR_WATCH_MINIMAL, JR_FACE_COUNT },
        { JR_WATCH_FUTURE,  JR_FACE_DIAL_FUTURE },
        { JR_WATCH_SKELETON,  JR_FACE_DIAL_SKELETON },
        { JR_WATCH_CELESTIAL, JR_FACE_DIAL_CELESTIAL },
    };
    for (size_t i = 0; i < sizeof want / sizeof *want; ++i) {
        jr_display_watch_style_set(want[i].st);
        wd_settle();
        CHECK(watch_dial_face() == want[i].face, "style %d asks for face %d, wanted %d",
              (int)want[i].st, (int)watch_dial_face(), (int)want[i].face);
    }
    jr_display_watch_style_set(JR_WATCH_PILOT);
    jr_display_nav_home();
    wd_settle();
    CHECK(watch_dial_face() == JR_FACE_COUNT, "off WATCH the dial is still requested");
    __atomic_store_n(&s_nav_word,
                     (uint32_t)JR_DISPLAY_SPACE_WATCH |
                         ((uint32_t)JR_WATCH_PILOT << 17), __ATOMIC_RELEASE);
    jr_display_clock_set(false, 0, 0, 0);
    wd_settle();
    CHECK(watch_dial_face() == JR_FACE_COUNT, "with the clock off the dial is still requested");
    jr_display_clock_set(true, 6, 30, 15);
    wd_settle();
    CHECK(watch_dial_face() == JR_FACE_DIAL_PILOT, "clock back on, PILOT's dial is not requested");
    reset_nav();
}

/* Baked or fallen back: with the dial clip shown, the watch keeps the art
 * under the hands (the grey survives everywhere the hands are not); with the
 * pipeline fallen back to RESTING (clip missing), the watch clears its disc
 * and draws the stand-in dial on black, so no grey survives inside r214. */
static void test_watch_keeps_a_baked_dial_and_clears_a_missing_one(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; return; }
    /* the shell veils the space, so "the art survived" is "not black" */
    size_t kept[2] = { 0, 0 }, outside[2] = { 0, 0 };
    const jr_face_t shown[2] = { JR_FACE_DIAL_DRESS, JR_FACE_IDLE };
    for (int k = 0; k < 2; ++k) {
        render_watch_frame(fb, JR_WATCH_DRESS, shown[k]);
        for (int y = 0; y < HUD_H; ++y) {
            for (int x = 0; x < HUD_W; ++x) {
                const int dx = x - 232, dy = y - 232;
                const bool in = dx * dx + dy * dy <= 200 * 200;
                if (fb[(size_t)y * HUD_W + x] != 0) {
                    if (in) kept[k]++; else outside[k]++;
                }
            }
        }
    }
    CHECK(kept[0] > 100000, "baked DRESS kept only %zu art px under the hands", kept[0]);
    /* Kept means UNVEILED: the art is the screen, so its pixels come through
     * at their own value, not halved by the space veil (2026-09-02: every
     * dial on the panel was dim). Under a sheet the veil returns. */
    size_t exact = 0, exact_sheet = 0;
    render_watch_frame(fb, JR_WATCH_DRESS, JR_FACE_DIAL_DRESS);
    for (int y = 32; y < HUD_H - 32; ++y) {
        for (int x = 32; x < HUD_W - 32; ++x) {
            const int dx = x - 232, dy = y - 232;
            if (dx * dx + dy * dy <= 200 * 200) exact += fb[(size_t)y * HUD_W + x] == 0x4208;
        }
    }
    s_detail_ease = 256;
    s_space_veil = 256;               /* what sp_fade_tick asks for under a sheet */
    for (size_t k = 0; k < px; ++k) fb[k] = 0x4208;
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        apply_space_overlay(&s_display, y, y2, fb + (size_t)y * HUD_W);
    }
    for (int y = 32; y < HUD_H - 32; ++y) {
        for (int x = 32; x < HUD_W - 32; ++x) {
            const int dx = x - 232, dy = y - 232;
            if (dx * dx + dy * dy <= 200 * 200) exact_sheet += fb[(size_t)y * HUD_W + x] == 0x4208;
        }
    }
    s_detail_ease = 0;
    CHECK(exact > 100000, "baked DRESS art was veiled: only %zu px at their own value", exact);
    CHECK(exact_sheet < exact / 4, "under a sheet the dial is not veiled (%zu px untouched)", exact_sheet);
    CHECK(kept[1] < 20000, "fallen-back DRESS left %zu lit px inside the dial (not cleared)", kept[1]);
    CHECK(outside[1] > 0, "the fallback clear reached outside the shell disc");
    s_display.shown_face = JR_FACE_IDLE;
    free(fb);
    reset_nav();
}

static void test_desk_is_on_the_ring_only_while_live(void)
{
    const int count = (int)JR_DISPLAY_SPACE_COUNT;

    reset_nav();                            /* shell word 0: DESK is dark */
    jr_display_nav_set(JR_DISPLAY_SPACE_STATUS);
    jr_display_nav_next();
    CHECK(space_is(JR_DISPLAY_SPACE_ACTIVITY, "next"),
          "next from POWER steps over a dark DESK");
    jr_display_nav_prev();
    CHECK(space_is(JR_DISPLAY_SPACE_STATUS, "prev"),
          "prev from ACTIVITY steps over a dark DESK");

    /* Forward lap, dark: count-1 steps, DESK never seen, home again. */
    reset_nav();
    bool saw_desk = false;
    for (int i = 0; i < count - 1; ++i) {
        jr_display_nav_next();
        saw_desk |= jr_display_nav_space() == JR_DISPLAY_SPACE_DESK;
    }
    CHECK(!saw_desk, "a dark DESK was landed on going forward");
    CHECK(space_is(JR_DISPLAY_SPACE_JARVIS, "dark lap fwd"),
          "a dark-DESK lap forward is %d steps", count - 1);

    /* Backward lap, dark, through the wrap first. */
    reset_nav();
    saw_desk = false;
    for (int i = 0; i < count - 1; ++i) {
        jr_display_nav_prev();
        saw_desk |= jr_display_nav_space() == JR_DISPLAY_SPACE_DESK;
    }
    CHECK(!saw_desk, "a dark DESK was landed on going backward");
    CHECK(space_is(JR_DISPLAY_SPACE_JARVIS, "dark lap back"),
          "a dark-DESK lap backward is %d steps", count - 1);

    /* Live: the ring is whole again, in both directions. */
    jr_display_set_shell_state(false, true, 40U, JR_DISPLAY_AGENT_WORKING);
    jr_display_nav_set(JR_DISPLAY_SPACE_STATUS);
    jr_display_nav_next();
    CHECK(space_is(JR_DISPLAY_SPACE_DESK, "live next"),
          "next from POWER reaches a live DESK");
    jr_display_nav_next();
    CHECK(space_is(JR_DISPLAY_SPACE_ACTIVITY, "live next 2"), "and on");
    jr_display_nav_prev();
    CHECK(space_is(JR_DISPLAY_SPACE_DESK, "live prev"),
          "prev from ACTIVITY reaches a live DESK");
    jr_display_nav_home();
    for (int i = 0; i < count; ++i) {
        jr_display_nav_next();
    }
    CHECK(space_is(JR_DISPLAY_SPACE_JARVIS, "live lap"),
          "a live lap is the full %d steps", count);

    /* The wrap itself, both states: the last screen's neighbour forward is
     * home, and home's neighbour backward is the last screen. */
    for (int live = 0; live <= 1; ++live) {
        reset_nav();
        jr_display_set_shell_state(false, live != 0, 40U,
                                   live ? JR_DISPLAY_AGENT_WORKING
                                        : JR_DISPLAY_AGENT_NONE);
        jr_display_nav_set(JR_DISPLAY_SPACE_ACTIVITY);
        jr_display_nav_next();
        CHECK(space_is(JR_DISPLAY_SPACE_JARVIS, "wrap fwd"),
              "live=%d: next from the last screen wraps home", live);
        jr_display_nav_prev();
        CHECK(space_is(JR_DISPLAY_SPACE_ACTIVITY, "wrap back"),
              "live=%d: prev from home wraps to the last screen", live);
    }
    reset_nav();
}

/* A job that ends while DESK is the current screen must not leave the owner
 * on a screen the ring no longer admits: they move one step forward, to
 * ACTIVITY, and any sheet they had open closes with the move. Going dark on
 * any OTHER screen moves nobody, and an explicit nav_set(DESK) while dark is
 * a caller's decision and stays. Mutation: deleting the edge in
 * jr_display_set_shell_state fails the first check only. */
static void test_desk_going_dark_moves_the_owner_on(void)
{
    reset_nav();
    jr_display_set_shell_state(false, true, 50U, JR_DISPLAY_AGENT_WORKING);
    jr_display_nav_set(JR_DISPLAY_SPACE_DESK);
    jr_display_nav_up();
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_DETAIL, "sheet open");
    jr_display_set_shell_state(false, false, 0U, JR_DISPLAY_AGENT_NONE);
    CHECK(space_is(JR_DISPLAY_SPACE_ACTIVITY, "strand"),
          "DESK going dark moves the owner on to ACTIVITY");
    CHECK(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE,
          "the move closed the sheet");

    jr_display_set_shell_state(false, true, 50U, JR_DISPLAY_AGENT_WORKING);
    jr_display_nav_set(JR_DISPLAY_SPACE_STATUS);
    jr_display_set_shell_state(false, false, 0U, JR_DISPLAY_AGENT_NONE);
    CHECK(space_is(JR_DISPLAY_SPACE_STATUS, "no strand"),
          "going dark elsewhere moves nobody");

    jr_display_nav_set(JR_DISPLAY_SPACE_DESK);
    jr_display_set_shell_state(false, false, 0U, JR_DISPLAY_AGENT_NONE);
    CHECK(space_is(JR_DISPLAY_SPACE_DESK, "explicit"),
          "an explicit nav_set(DESK) while dark is honoured");
    reset_nav();
}

/* The orbit has one mark per VISIBLE screen, so when DESK appears every slot
 * moves. The mark for the screen you are standing on must not jump with
 * them: it eases to its new slot over a slide. Drive sp_fade_tick by hand —
 * one frame after DESK appears the mark must have moved only part of the
 * way, and after a slide's worth of frames it must have arrived. Mutation:
 * removing the s_orbit_off16 bank makes the first frame land on the target
 * and fails the "did not jump" check. */
static void test_orbit_mark_eases_when_desk_appears(void)
{
    stage_space(JR_DISPLAY_SPACE_ACTIVITY);        /* reset_nav: DESK dark */
    s_space_serial_seen = 0;
    s_orbit_n_seen = 0;
    s_orbit_off16 = 0;
    sp_fade_tick(1000U, 60U, 60);
    const int a_dark = s_orbit_a16;
    CHECK(a_dark == sp_orbit_angle(JR_DISPLAY_SPACE_ACTIVITY, false) * 16,
          "settled on the five-screen slot, got %d", a_dark);

    jr_display_set_shell_state(false, true, 50U, JR_DISPLAY_AGENT_WORKING);
    const int a_target = sp_orbit_angle(JR_DISPLAY_SPACE_ACTIVITY, true) * 16;
    CHECK(a_target != a_dark, "the slot actually moved");
    sp_fade_tick(1060U, 60U, 60);
    const int first = s_orbit_a16 - a_dark;
    const int full = a_target - a_dark;
    CHECK((first < 0 ? -first : first) < (full < 0 ? -full : full),
          "the mark jumped: first frame moved %d of %d", first, full);
    CHECK(first != 0, "the mark did not start moving");
    for (int i = 0; i < 60; ++i) {
        sp_fade_tick(1120U + (uint32_t)i * 60U, 60U, 60);
    }
    CHECK(s_orbit_a16 == a_target, "the mark settles on its new slot: %d vs %d",
          s_orbit_a16, a_target);

    /* The dots re-space at once: six of them now, evenly spaced. */
    uint16_t *fb = render_frame();
    if (fb) {
        int dots = 0;
        bool prev_on = false;
        for (int a = 0; a < 2048; ++a) {
            const int x = SP_CX + ((sp_cos((a * 256) / 2048) * SP_ORB_R) >> 15);
            const int y = SP_CY + ((sp_sin((a * 256) / 2048) * SP_ORB_R) >> 15);
            const uint16_t px = fb[(size_t)y * HUD_W + x];
            const bool on = px == SP_C_CYAN_DIM || px == SP_C_CYAN;
            if (a > 0 && on && !prev_on) {
                dots++;
            }
            prev_on = on;
        }
        CHECK(dots == (int)JR_DISPLAY_SPACE_COUNT,
              "expected %d orbit marks with DESK live, counted %d",
              (int)JR_DISPLAY_SPACE_COUNT, dots);
        free(fb);
    }
    reset_nav();
}

/* ---------------------------------------------------------------- WEATHER -- */

static size_t count_color_within(const uint16_t *fb, uint16_t c, int rmin,
                                 int rmax, int ymax)
{
    size_t n = 0;
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        if ((int)(i / HUD_W) >= ymax || fb[i] != c) {
            continue;
        }
        const int r = radius_of((int)i);
        if (r >= rmin && r <= rmax) {
            ++n;
        }
    }
    return n;
}

/* The weather ring's centre (the ring sits a little above the panel centre)
 * and a pixel on its centre line at slot k's middle. */
#define WX_CY (SP_CY + SG_RING_DY)

static uint16_t hour_px(const uint16_t *fb, int k)
{
    const uint32_t a = (uint32_t)sg_slot_centre_q16(k);
    const int x = SP_CX + (int)((glass_cos_q15(a) * 118) >> 15);
    const int y = WX_CY + (int)((glass_sin_q15(a) * 118) >> 15);
    return fb[(size_t)y * HUD_W + x];
}

/* Radius from the weather ring's centre. */
static int wx_radius(int i)
{
    const int dx = (i % HUD_W) - SP_CX;
    const int dy = (i / HUD_W) - WX_CY;
    return sp_isqrt(dx * dx + dy * dy);
}

/* Pixels on the stroke's core (r116..120) that are neither untouched nor the
 * bare track: the rain hours, and nothing else. */
static size_t hour_ring_ink(const uint16_t *fb)
{
    size_t n = 0;
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        const int r = wx_radius((int)i);
        if (r >= 116 && r <= 120 && fb[i] != POISON && fb[i] != SP_C_TRACK) {
            ++n;
        }
    }
    return n;
}

/* Exact-colour pixels in the label's box (the condition word and, beside the
 * temperature, the degree sign: both wear the accent). */
static size_t wx_accent_ink(const uint16_t *fb, uint16_t c)
{
    const int hero_top = WX_CY - 31;
    return count_in(fb, c, SP_CX - 60, SP_CX + 60, hero_top - 28, hero_top + 24);
}

/* No weather means NO NUMBER: the headline says why, the disc is empty, and
 * the focal object is two bare tracks — not a zero, not "--", nothing shaped
 * like a reading. The sheet is one honest row. */
static void test_weather_without_data_prints_no_number(void)
{
    stage_space(JR_DISPLAY_SPACE_WEATHER);
    jr_display_weather_set(NULL);
    sp_compose();
    CHECK(strcmp(s_wx_head, "NO WEATHER") == 0, "headline, got '%s'", s_wx_head);
    CHECK(s_wx_temp[0] == '\0' && s_wx_hilo[0] == '\0' && s_wx_age[0] == '\0',
          "no number composed: '%s' '%s' '%s'", s_wx_temp, s_wx_hilo, s_wx_age);
    for (const char *p = s_wx_head; *p; ++p) {
        CHECK(*p < '0' || *p > '9', "a digit in the no-data headline");
    }
    sp_compose_detail(JR_DISPLAY_SPACE_WEATHER);
    CHECK(s_detail_rows == 1 && strcmp(s_detail_value[0], "NONE") == 0,
          "the sheet admits it has nothing: %d rows, '%s'", s_detail_rows,
          s_detail_value[0]);

    uint16_t *fb = render_frame();
    if (!fb) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        return;
    }
    size_t interior = 0, nontrack = 0, track = 0;
    const int hero_top = WX_CY - 31;
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        const int y = (int)(i / HUD_W);
        const int r = wx_radius((int)i);
        if (fb[i] == POISON) {
            continue;
        }
        if (r < 100 && y >= hero_top - 4) {
            ++interior;               /* below the label: where the numbers go */
        } else if (r >= 116 && r <= 120) {
            if (fb[i] == SP_C_TRACK) {
                ++track;
            } else {
                ++nontrack;
            }
        }
    }
    CHECK(interior == 0, "%zu pixels inside the ring with no weather", interior);
    CHECK(nontrack == 0, "%zu coloured pixels on the ring with no weather", nontrack);
    CHECK(track > 1000, "the bare track drew (%zu track pixels)", track);
    free(fb);
}

/* Age is honest in three steps: under two minutes it is not worth a line;
 * up to thirty it is a line; beyond that the data is STALE and the accent
 * loses its colour — a CLEAR day stops being amber. Muted outranks all of
 * it with gold, as on every screen. Mutation: deleting the s_wx_stale branch
 * in sp_focal_weather leaves amber on the stale frame. */
static void test_stale_weather_loses_its_colour(void)
{
    stage_space(JR_DISPLAY_SPACE_WEATHER);
    set_weather(true, 83, 86, 76, JR_DISPLAY_SKY_CLEAR, "CLEAR", 0U);
    s_fake_us = 0;
    sp_compose();
    CHECK(s_wx_age[0] == '\0', "fresh: no age line, got '%s'", s_wx_age);
    uint16_t *fb = render_frame();
    if (fb) {
        CHECK(wx_accent_ink(fb, SP_C_AMBER) > 40,
              "a fresh CLEAR day wears amber");
        free(fb);
    }

    s_fake_us = 12LL * 60 * 1000 * 1000;
    sp_compose();
    CHECK(strcmp(s_wx_age, "12M AGO") == 0, "12 min: got '%s'", s_wx_age);
    CHECK(!s_wx_stale, "12 min is not stale");

    s_fake_us = 45LL * 60 * 1000 * 1000;
    sp_compose();
    CHECK(s_wx_stale, "45 min is stale");
    CHECK(strcmp(s_wx_age, "STALE 45M") == 0, "45 min: got '%s'", s_wx_age);
    CHECK(strcmp(s_wx_temp, "83") == 0, "the number survives, got '%s'", s_wx_temp);
    fb = render_frame();
    if (fb) {
        CHECK(count_color_within(fb, SP_C_AMBER, 0, JR_DISPLAY_SHELL_R_MAX,
                                 HUD_H) == 0,
              "stale weather still drew amber");
        CHECK(wx_accent_ink(fb, SP_C_GREY) > 40, "the stale accent is grey");
        free(fb);
    }
    sp_compose_detail(JR_DISPLAY_SPACE_WEATHER);
    CHECK(strcmp(s_detail_label[4], "AGE") == 0 &&
              strcmp(s_detail_value[4], "45M STALE") == 0,
          "the sheet says so too: '%s' '%s'", s_detail_label[4],
          s_detail_value[4]);

    /* Days. fetched_ms is a uint32 of esp_timer ms, so the largest age the
     * contract can express is 49 days — "STALE 49D" is the nine-glyph worst
     * case the disc was measured for. */
    s_fake_us = 3LL * 24 * 60 * 60 * 1000 * 1000;
    sp_compose();
    CHECK(strcmp(s_wx_age, "STALE 3D") == 0, "3 days: got '%s'", s_wx_age);
    s_fake_us = 49LL * 24 * 60 * 60 * 1000 * 1000;
    sp_compose();
    CHECK(strcmp(s_wx_age, "STALE 49D") == 0, "49 days: got '%s'", s_wx_age);

    /* Muted: gold, whatever the sky or the age. */
    s_fake_us = 0;
    __atomic_store_n(&s_hud_env_word, 74U | (1U << 9), __ATOMIC_RELEASE);
    sp_compose();
    fb = render_frame();
    if (fb) {
        CHECK(count_color_within(fb, SP_C_AMBER, 0, JR_DISPLAY_SHELL_R_MAX,
                                 HUD_H) == 0, "muted still drew amber");
        CHECK(wx_accent_ink(fb, SP_C_GOLD) > 40, "muted wears gold");
        free(fb);
    }
    __atomic_store_n(&s_hud_env_word, 74U, __ATOMIC_RELEASE);
    s_fake_us = 0;
}

/* THE NEXT TWELVE HOURS. Hour k from now takes slot k of twelve on the
 * gauge's own 270 degrees, lit by its chance of rain; an hour the answer
 * lacks draws nothing; stale, hourless or clockless weather draws no ring.
 * Sampled on the ring's centre line at each slot's middle. Mutations:
 * starting the selection at midnight (now_idx 0) turns slot 0 into a 0 %
 * track; dropping the +24 for yesterday's anchor reads the wrong hours;
 * dropping the stale gate leaves a ring on a 45-minute-old frame. */
static void set_weather_hours(const uint8_t *pp, int mday, uint32_t fetched_ms)
{
    jr_display_weather_t w;
    memset(&w, 0, sizeof w);
    w.valid = true;
    w.temp_f = 76;
    w.feels_f = 78;
    w.hi_f = 82;
    w.lo_f = 76;
    w.sky = JR_DISPLAY_SKY_CLOUDS;
    strncpy(w.condition, "OVERCAST", sizeof w.condition - 1U);
    w.fetched_ms = fetched_ms;
    memcpy(w.rain_hours, pp, sizeof w.rain_hours);
    w.rain_hours_mday = (uint8_t)mday;
    jr_display_weather_set(&w);
}

static void test_weather_hours_ring_starts_now(void)
{
    /* the pure pieces: which hour is now, and what the twelve slots hold */
    jr_display_clock_set(true, 14, 5, 0);
    jr_display_clock_set_date(3, 23, 8);
    const uint32_t cw = __atomic_load_n(&s_clock_word, __ATOMIC_ACQUIRE);
    const uint32_t dw = __atomic_load_n(&s_clock_date_word, __ATOMIC_ACQUIRE);
    CHECK(sp_rain_now_idx(cw, dw, 23) == 14, "same day: hour 14, got %d",
          sp_rain_now_idx(cw, dw, 23));
    CHECK(sp_rain_now_idx(cw, dw, 22) == 38, "yesterday's midnight: 14 + 24, got %d",
          sp_rain_now_idx(cw, dw, 22));
    CHECK(sp_rain_now_idx(cw, dw, 0) == -1, "no hours in the answer");
    CHECK(sp_rain_now_idx(cw & ~(1u << 16), dw, 23) == -1, "clock off");
    CHECK(sp_rain_now_idx(cw, 0u, 23) == -1, "no date");

    uint8_t pp[JR_DISPLAY_RAIN_HOURS];
    for (int i = 0; i < JR_DISPLAY_RAIN_HOURS; ++i) {
        pp[i] = (uint8_t)i;
    }
    pp[16] = SP_WX_HOUR_NONE;
    pp[17] = 101;                              /* not a percentage */
    uint8_t out[SP_WX_HOURS];
    CHECK(sp_rain_hours(out, pp, JR_DISPLAY_RAIN_HOURS, 14) == 10,
          "twelve slots, two unknown");
    CHECK(out[0] == 14 && out[1] == 15 && out[11] == 25,
          "slot k is hour now+k: %u %u %u", out[0], out[1], out[11]);
    CHECK(out[2] == SP_WX_HOUR_NONE && out[3] == SP_WX_HOUR_NONE,
          "unknown and out-of-range hours stay unknown: %u %u", out[2], out[3]);
    CHECK(sp_rain_hours(out, pp, JR_DISPLAY_RAIN_HOURS, 30) == 6 &&
              out[5] == 35 && out[6] == SP_WX_HOUR_NONE,
          "past the answer's 36th hour nothing is invented");
    CHECK(sp_rain_hours(out, pp, JR_DISPLAY_RAIN_HOURS, -1) == 0 &&
              out[0] == SP_WX_HOUR_NONE, "no now, no hours");
    /* the slots: twelve 30 degree slots clockwise from 12 o'clock, a mark
     * that grows with the chance and starts from nothing */
    CHECK(sg_slot_centre_q16(0) == GLASS_Q16_12_OCLOCK + GLASS_TURN_Q16 / 24,
          "slot 0 is the first half-slot after 12: %d", (int)sg_slot_centre_q16(0));
    for (int k = 1; k < SP_WX_HOURS; ++k) {
        const int step = (int)((sg_slot_centre_q16(k) - sg_slot_centre_q16(k - 1) + GLASS_TURN_Q16) &
                               0xFFFF);
        CHECK(step >= GLASS_TURN_Q16 / 12 - 2 && step <= GLASS_TURN_Q16 / 12 + 2,
              "slot %d is a twelfth round from the one before: %d", k, step);
    }
    int32_t w_dry, w_half, w_full, w_none, sw_dry, sw_wet;
    int a_dry, a_half, a_full, a_none;
    sg_rain_mark(0, GLASS_ONE_Q16, &w_dry, &sw_dry, &a_dry);
    sg_rain_mark(50, GLASS_ONE_Q16, &w_half, &sw_wet, &a_half);
    sg_rain_mark(100, GLASS_ONE_Q16, &w_full, &sw_wet, &a_full);
    sg_rain_mark(100, 0, &w_none, &sw_wet, &a_none);
    CHECK(w_dry < w_half && w_half < w_full, "stroke grows with the chance: %d %d %d",
          (int)w_dry, (int)w_half, (int)w_full);
    CHECK(a_dry < a_half && a_half < a_full && a_full == 255, "alpha grows with the chance: %d %d %d",
          a_dry, a_half, a_full);
    CHECK(sw_dry < sw_wet && sw_wet < GLASS_TURN_Q16 / 12, "a tick is short, a stroke fits its slot");
    CHECK(w_none == 0 && a_none == 0, "an hour not yet built draws nothing");

    /* on the glass: 14:05, now is 100 %, the next hour dry, the one after
     * unknown, six hours out 50 %, and midnight..13:00 all dry */
    memset(pp, 0, sizeof pp);
    pp[14] = 100;
    pp[16] = SP_WX_HOUR_NONE;
    pp[20] = 50;
    s_fake_us = 0;
    stage_space(JR_DISPLAY_SPACE_WEATHER);
    set_weather_hours(pp, 23, 0U);
    sp_compose();
    CHECK(strcmp(s_wx_head, "OVERCAST") == 0, "the headline is the word: '%s'", s_wx_head);
    uint16_t *fb = render_frame();
    if (!fb) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; return; }
    CHECK(hour_px(fb, 0) == SP_C_RAIN, "now at 100 %% is full rain, got %04x", hour_px(fb, 0));
    CHECK(hour_px(fb, 1) != POISON && hour_px(fb, 1) != SP_C_RAIN,
          "a dry hour is a faint tick, got %04x", hour_px(fb, 1));
    CHECK(hour_px(fb, 2) == POISON, "an unknown hour draws nothing, got %04x", hour_px(fb, 2));
    const uint16_t half = hour_px(fb, 6);
    CHECK(half != SP_C_RAIN && half != SP_C_TRACK && half != POISON &&
              (half & 0x1F) < (SP_C_RAIN & 0x1F),
          "50 %% is rain, dimmer than 100 %%: %04x", half);
    CHECK(hour_ring_ink(fb) > 400, "the ring drew (%zu px)", hour_ring_ink(fb));
    free(fb);

    /* just after midnight the 23:40 fetch is still fresh: 00:10 is its hour 24 */
    memset(pp, 0, sizeof pp);
    pp[24] = 100;
    jr_display_clock_set(true, 0, 10, 0);
    set_weather_hours(pp, 22, 0U);
    sp_compose();
    fb = render_frame();
    if (fb) {
        CHECK(hour_px(fb, 0) == SP_C_RAIN, "yesterday's fetch, hour 24 is now: %04x",
              hour_px(fb, 0));
        free(fb);
    }

    /* muted: gold, like every accent */
    __atomic_store_n(&s_hud_env_word, 74U | (1U << 9), __ATOMIC_RELEASE);
    fb = render_frame();
    if (fb) {
        CHECK(hour_px(fb, 0) == SP_C_GOLD, "muted hour is gold: %04x", hour_px(fb, 0));
        free(fb);
    }
    __atomic_store_n(&s_hud_env_word, 74U, __ATOMIC_RELEASE);

    /* stale, hourless, clockless: no ring at all */
    s_fake_us = 45LL * 60 * 1000 * 1000;
    sp_compose();
    fb = render_frame();
    if (fb) {
        CHECK(hour_ring_ink(fb) == 0, "stale drew %zu ring px", hour_ring_ink(fb));
        free(fb);
    }
    s_fake_us = 0;
    set_weather_hours(pp, 0, 0U);
    sp_compose();
    fb = render_frame();
    if (fb) {
        CHECK(hour_ring_ink(fb) == 0, "hourless drew %zu ring px", hour_ring_ink(fb));
        free(fb);
    }
    set_weather_hours(pp, 22, 0U);
    jr_display_clock_set(false, 0, 0, 0);
    sp_compose();
    fb = render_frame();
    if (fb) {
        CHECK(hour_ring_ink(fb) == 0, "clockless drew %zu ring px", hour_ring_ink(fb));
        free(fb);
    }

    /* the sun, compact, under the disc; only with weather and a sun */
    jr_display_sun_set(422, 1175);
    sp_compose();
    CHECK(strcmp(s_wx_sun, "7:02 19:35") == 0, "sun line '%s'", s_wx_sun);
    fb = render_frame();
    if (fb) {
        size_t grey = 0;
        const int sun_y = WX_CY - 31 + 59 + 40;
        for (int y = sun_y; y < sun_y + 16; ++y) {
            for (int x = 0; x < HUD_W; ++x) {
                grey += fb[(size_t)y * HUD_W + x] == SP_C_GREY;
            }
        }
        CHECK(grey > 60, "the sun line drew %zu grey px", grey);
        free(fb);
    }
    jr_display_sun_set(-1, -1);
    sp_compose();
    CHECK(s_wx_sun[0] == '\0', "no sun, no line: '%s'", s_wx_sun);
    jr_display_sun_set(422, 1175);
    jr_display_weather_set(NULL);
    sp_compose();
    CHECK(s_wx_sun[0] == '\0', "no weather, no digits: '%s'", s_wx_sun);

    jr_display_sun_set(-1, -1);
    jr_display_clock_set_date(0, 0, 0);
    reset_nav();
}

/* THE ICONS. WEATHER shows the sky as an icon (day or night from the sun times
 * and the clock), STATUS a bolt beside the power word while charging. Every sky
 * the tools side names is mapped; the one it cannot name draws no icon. */
static void test_icons_follow_the_sky_and_the_charger(void)
{
    static const struct { jr_display_sky_t sky; int day, night; } map[] = {
        { JR_DISPLAY_SKY_CLEAR,  GLASS_ICON_SUN,     GLASS_ICON_MOON  },
        { JR_DISPLAY_SKY_PARTLY, GLASS_ICON_PARTLY,  GLASS_ICON_CLOUD },
        { JR_DISPLAY_SKY_CLOUDS, GLASS_ICON_CLOUD,   GLASS_ICON_CLOUD },
        { JR_DISPLAY_SKY_FOG,    GLASS_ICON_FOG,     GLASS_ICON_FOG   },
        { JR_DISPLAY_SKY_RAIN,   GLASS_ICON_RAIN,    GLASS_ICON_RAIN  },
        { JR_DISPLAY_SKY_STORM,  GLASS_ICON_THUNDER, GLASS_ICON_THUNDER },
        { JR_DISPLAY_SKY_SNOW,   GLASS_ICON_SNOW,    GLASS_ICON_SNOW  },
        { JR_DISPLAY_SKY_UNKNOWN, -1,                -1               },
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; ++i) {
        CHECK(sp_wx_icon_for(map[i].sky, false) == map[i].day, "sky %d by day", (int)map[i].sky);
        CHECK(sp_wx_icon_for(map[i].sky, true) == map[i].night, "sky %d by night", (int)map[i].sky);
    }

    jr_display_sun_set(422, 1175);                       /* 7:02 .. 19:35 */
    jr_display_clock_set(true, 14, 5, 0);
    CHECK(!sp_wx_is_night(), "14:05 is day");
    jr_display_clock_set(true, 23, 5, 0);
    CHECK(sp_wx_is_night(), "23:05 is night");
    jr_display_clock_set(true, 3, 0, 0);
    CHECK(sp_wx_is_night(), "03:00 is night");
    jr_display_clock_set(true, 7, 2, 0);
    CHECK(!sp_wx_is_night(), "sunrise minute is day");
    jr_display_clock_set(true, 19, 35, 0);
    CHECK(sp_wx_is_night(), "sunset minute is night");
    jr_display_clock_set(false, 0, 0, 0);
    CHECK(!sp_wx_is_night(), "no clock: day");
    jr_display_clock_set(true, 23, 5, 0);
    jr_display_sun_set(-1, -1);
    CHECK(!sp_wx_is_night(), "no sun times: day");

    jr_display_sun_set(422, 1175);
    set_weather(true, 83, 86, 76, JR_DISPLAY_SKY_CLEAR, "CLEAR", 0U);
    sp_compose();
    CHECK(s_wx_icon == GLASS_ICON_MOON, "clear at 23:05 composes the moon, got %d", s_wx_icon);
    set_weather(false, 0, 0, 0, JR_DISPLAY_SKY_RAIN, "RAIN", 0U);
    sp_compose();
    CHECK(s_wx_icon == -1, "no weather, no icon");

    /* the bolt: ink left of the power word while charging, none otherwise */
    jr_display_clock_set(true, 14, 5, 0);
    for (int charging = 0; charging <= 1; ++charging) {
        stage_space(JR_DISPLAY_SPACE_STATUS);
        s_space_ease = 256;
        s_entry_ms = SP_ENTRY_TOTAL_MS;
        jr_display_power_set(74U, 4020U, charging != 0, charging != 0);
        sp_compose();
        uint16_t *fb = render_frame();
        CHECK(fb != NULL, "frame allocated");
        if (!fb) {
            return;
        }
        int bolt = 0, fx = -1, fy = -1;
        for (int y = 255; y <= 290; ++y) {
            for (int x = 170; x <= 190; ++x) {
                if (fb[(size_t)y * HUD_W + x] != POISON && bolt++ == 0) {
                    fx = x;
                    fy = y;
                }
            }
        }
        if (charging) {
            CHECK(bolt > 40, "the charging bolt draws (%d px)", bolt);
        } else {
            CHECK(bolt == 0, "no bolt off the charger (%d px, first at %d,%d)", bolt, fx, fy);
        }
        free(fb);
    }
    jr_display_sun_set(-1, -1);
    jr_display_weather_set(NULL);
    jr_display_clock_set(false, 0, 0, 0);
    reset_nav();
}

/* THE ENTRY. A screen becoming current starts a clock; STATUS and WEATHER
 * draw from it; everything else parks it. It runs for SP_ENTRY_TOTAL_MS and
 * then the screen is still, at the same frame it would have drawn without any
 * clock, and the outgoing screen of a slide never replays it. */
static size_t drawn_px(const uint16_t *fb)
{
    size_t n = 0;
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        n += fb[i] != POISON;
    }
    return n;
}

/* Pixels lit (non-black) on the rows [y0, y1). The data screens sit on true
 * black, so this counts exactly what the glass drew whatever its colour or
 * alpha. */
static size_t lit_between(const uint16_t *fb, int y0, int y1)
{
    size_t n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = SP_CX - 140; x <= SP_CX + 140; ++x) {
            n += fb[(size_t)y * HUD_W + x] != POISON;
        }
    }
    return n;
}

/* DESK on the glass: the ring is the STATUS ring (r113-123 plus its soft
 * edge), the progress number is in its middle, and nothing else of DESK
 * reaches the band between the ring and the orbit rail. */
static void test_desk_is_a_glass_ring_with_its_number(void)
{
    stage_space(JR_DISPLAY_SPACE_DESK);
    jr_display_desk_set_task("FIX BUG", 50U, JR_DISPLAY_AGENT_WORKING);
    sp_compose();
    s_entry_ms = SP_ENTRY_TOTAL_MS;
    uint16_t *fb = render_frame();
    if (!fb) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        return;
    }
    size_t ring = 0, between = 0, middle = 0;
    int rmin = 999, rmax = 0, bmin = 999, bmax = 0;
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        if (fb[i] == POISON) {
            continue;
        }
        const int r = radius_of((int)i);
        if (r >= 110 && r <= 126) {
            ring++;
            if (r < rmin) rmin = r;
            if (r > rmax) rmax = r;
        } else if (r > 136 && r < 183) {   /* past the ring glow */
            between++;
            if (r < bmin) bmin = r;
            if (r > bmax) bmax = r;
        } else if (r < 100) {
            middle++;
        }
    }
    CHECK(ring > 2500, "the ring is drawn (%zu pixels)", ring);
    CHECK(rmin >= 110 && rmax <= 126, "ring r%d..r%d", rmin, rmax);
    CHECK(middle > 800, "the task, the number and the state are drawn (%zu)", middle);
    CHECK(between == 0, "%zu DESK pixels between the ring and the orbit rail (r%d..%d)", between, bmin, bmax);
    free(fb);
    s_entry_ms = SP_ENTRY_TOTAL_MS;
}

static void test_entry_clock_runs_then_stops(void)
{
    reset_nav();
    jr_display_nav_set(JR_DISPLAY_SPACE_WATCH);
    uint32_t now = 1000;
    sp_fade_tick(now, 16, 8);
    CHECK(!glass_entry_animating(s_entry_ms, SP_ENTRY_TOTAL_MS), "WATCH has no entry");
    jr_display_nav_set(JR_DISPLAY_SPACE_ACTIVITY);
    sp_fade_tick(now, 16, 8);
    CHECK(s_entry_ms == 0 && glass_entry_animating(s_entry_ms, SP_ENTRY_TOTAL_MS),
          "ACTIVITY arriving starts the clock");
    jr_display_nav_set(JR_DISPLAY_SPACE_DESK);
    sp_fade_tick(now, 16, 8);
    CHECK(s_entry_ms == 0 && glass_entry_animating(s_entry_ms, SP_ENTRY_TOTAL_MS),
          "DESK arriving starts the clock");

    jr_display_nav_set(JR_DISPLAY_SPACE_STATUS);
    sp_fade_tick(now, 16, 8);
    CHECK(s_entry_ms == 0 && glass_entry_animating(s_entry_ms, SP_ENTRY_TOTAL_MS),
          "STATUS arriving starts the clock at 0, got %u", (unsigned)s_entry_ms);
    uint32_t last = 0;
    int frames = 0;
    while (glass_entry_animating(s_entry_ms, SP_ENTRY_TOTAL_MS) && frames < 200) {
        now += 70;                      /* the live cadence is about 14 fps */
        sp_fade_tick(now, 70, 8);
        CHECK(s_entry_ms >= last, "the clock never runs backwards");
        last = s_entry_ms;
        ++frames;
    }
    CHECK(s_entry_ms == SP_ENTRY_TOTAL_MS, "it parks at the total, got %u", (unsigned)s_entry_ms);
    CHECK(frames >= 14 && frames <= 16, "about 13 frames at 14 fps after the 3-frame dark cut, got %d", frames);
    for (int i = 0; i < 5; ++i) {
        now += 70;
        sp_fade_tick(now, 70, 8);
    }
    CHECK(s_entry_ms == SP_ENTRY_TOTAL_MS, "and stays parked");

    /* WEATHER restarts it, and so does leaving and coming back */
    jr_display_nav_set(JR_DISPLAY_SPACE_WEATHER);
    sp_fade_tick(now, 16, 8);
    CHECK(s_entry_ms == 0, "WEATHER arriving restarts it");
    jr_display_nav_set(JR_DISPLAY_SPACE_JARVIS);
    sp_fade_tick(now, 16, 8);
    CHECK(!glass_entry_animating(s_entry_ms, SP_ENTRY_TOTAL_MS), "going home parks it");
    s_entry_ms = SP_ENTRY_TOTAL_MS;
    reset_nav();
}

static void test_entry_builds_the_screens(void)
{
    /* STATUS: nothing at 0, more and more of the ring, then the settled frame,
     * which is bit-for-bit what a screen with no clock draws. */
    stage_space(JR_DISPLAY_SPACE_STATUS);
    jr_display_power_set(74U, 4020U, false, false);
    links(true, -34, "198.51.100.7", true, 2U, false, false);
    static const uint32_t at[] = {0, 100, 250, 450, 700, SP_ENTRY_TOTAL_MS};
    size_t prev_ring = 0;
    for (size_t k = 0; k < sizeof at / sizeof *at; ++k) {
        s_entry_ms = at[k];
        uint16_t *fb = render_frame();
        if (!fb) {
            printf("FAIL %s: allocation failed\n", __func__);
            g_failures++;
            return;
        }
        size_t fill = 0;
        for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
            const int dx = (int)(i % HUD_W) - SP_CX, dy = (int)(i / HUD_W) - (SP_CY + SG_RING_DY);
            const int r = sp_isqrt(dx * dx + dy * dy);
            fill += fb[i] == SP_C_CYAN && r >= 116 && r <= 120;
        }
        const size_t all = drawn_px(fb);
        if (k == 0) {
            CHECK(fill == 0, "no fill at entry 0: %zu", fill);
        }
        CHECK(fill >= prev_ring, "the ring only grows: %zu then %zu at %u ms", prev_ring, fill,
              (unsigned)at[k]);
        CHECK(all > 5000, "something is drawn at every step: %zu", all);
        prev_ring = fill;
        free(fb);
    }
    CHECK(prev_ring > 1500, "the settled ring is 74 %% of a circle: %zu px", prev_ring);

    s_entry_ms = SP_ENTRY_TOTAL_MS;
    uint16_t *a = render_frame();
    s_entry_ms = SP_ENTRY_TOTAL_MS + 5000U;
    uint16_t *b = render_frame();
    if (a && b) {
        CHECK(memcmp(a, b, (size_t)HUD_W * HUD_H * sizeof *a) == 0, "settled is one frame");
    }
    free(a);
    free(b);

    /* WEATHER: the hours build clockwise, 45 ms apart */
    uint8_t pp[JR_DISPLAY_RAIN_HOURS];
    memset(pp, 80, sizeof pp);
    jr_display_clock_set(true, 14, 5, 0);
    jr_display_clock_set_date(3, 23, 8);
    s_fake_us = 0;
    stage_space(JR_DISPLAY_SPACE_WEATHER);
    set_weather_hours(pp, 23, 0U);
    sp_compose();
    static const uint32_t wat[] = {0, 100, 300, 500, 800};
    size_t prev = 0;
    for (size_t k = 0; k < sizeof wat / sizeof *wat; ++k) {
        s_entry_ms = wat[k];
        uint16_t *fb = render_frame();
        if (!fb) {
            return;
        }
        const size_t ink = hour_ring_ink(fb);
        if (k == 0) {
            CHECK(ink == 0, "no hour is drawn at entry 0: %zu", ink);
        }
        CHECK(ink >= prev, "hours only build: %zu then %zu", prev, ink);
        if (k == 1) {
            CHECK(hour_px(fb, 0) != POISON && hour_px(fb, 11) == POISON,
                  "hour 0 has begun and hour 11 has not at 100 ms");
        }
        prev = ink;
        free(fb);
    }
    CHECK(prev > 400, "all twelve are in by 800 ms: %zu", prev);
    s_entry_ms = SP_ENTRY_TOTAL_MS;
    jr_display_clock_set_date(0, 0, 0);
}

static void test_outgoing_screen_does_not_replay_the_entry(void)
{
    stage_space(JR_DISPLAY_SPACE_WEATHER);
    set_weather(true, 83, 86, 76, JR_DISPLAY_SKY_CLEAR, "CLEAR", 0U);
    sp_compose();
    s_entry_ms = 0;
    uint16_t *only = render_frame();
    /* mid-slide from STATUS to WEATHER, the new screen still at entry 0 */
    s_space_from = JR_DISPLAY_SPACE_STATUS;
    s_space_ease = 128;
    s_space_prog = 60;               /* before the cut: the outgoing screen is still on the glass */
    s_space_dir = 1;
    uint16_t *slide = render_frame();
    if (only && slide) {
        CHECK(drawn_px(slide) > drawn_px(only) + 3000,
              "the outgoing STATUS is drawn whole: %zu vs %zu", drawn_px(slide), drawn_px(only));
    }
    free(only);
    free(slide);
    s_space_ease = 256;
    s_entry_ms = SP_ENTRY_TOTAL_MS;
}

/* --------------------------------------------------------------- ACTIVITY -- */

/* Empty is one honest line and nothing else; three pushes are three rows,
 * newest first; a fourth push drops the oldest. A summary too long for the
 * row is cut and marked. The sheet says WHEN, not what, so it never re-cuts
 * the summary to a ten-glyph column. */
static void test_activity_is_honest_when_empty_and_newest_first(void)
{
    reset_activity();
    stage_space(JR_DISPLAY_SPACE_ACTIVITY);
    sp_compose();
    CHECK(s_act_rows == 0, "empty composes no rows, got %d", s_act_rows);
    uint16_t *fb = render_frame();
    if (fb) {
        const size_t centre = lit_between(fb, SP_CY - 14, SP_CY + 14);
        const size_t top = lit_between(fb, SP_ACT_Y0 - 4, SP_ACT_Y0 + SP_ACT_SUM_DY + 20);
        CHECK(centre > 100, "NOTHING YET is drawn on the centre row (%zu)", centre);
        CHECK(top == 0, "an empty feed drew a top row (%zu pixels)", top);
        free(fb);
    }
    sp_compose_detail(JR_DISPLAY_SPACE_ACTIVITY);
    CHECK(s_detail_rows == 1 && strcmp(s_detail_value[0], "EMPTY") == 0,
          "empty sheet: %d rows, '%s'", s_detail_rows, s_detail_value[0]);

    s_fake_us = 0;
    jr_display_activity_push("WEB", "FOUND THREE RESULTS");
    jr_display_activity_push("TIME", "SAID 4:20 PM");
    jr_display_activity_push("ASK", "TAP ONE OF THREE");
    jr_display_activity_push("SAID", "VOLUME IS NOW FORTY");
    sp_compose();
    CHECK(s_act_rows == 3, "three rows kept, got %d", s_act_rows);
    CHECK(strcmp(s_act_kind[0], "SAID") == 0, "newest first, got '%s'", s_act_kind[0]);
    CHECK(strcmp(s_act_kind[1], "ASK") == 0, "then, got '%s'", s_act_kind[1]);
    CHECK(strcmp(s_act_kind[2], "TIME") == 0, "then, got '%s'", s_act_kind[2]);
    CHECK(strcmp(s_act_row[0], "VOLUME IS NOW FORTY") == 0,
          "the summary alone is the row, got '%s'", s_act_row[0]);
    CHECK(strcmp(s_act_age[0], "JUST NOW") == 0, "age beside the kind, got '%s'", s_act_age[0]);
    for (int i = 0; i < 3; ++i) {
        CHECK(strstr(s_act_row[i], "FOUND") == NULL, "the fourth-oldest survived");
    }
    fb = render_frame();
    if (fb) {
        for (int i = 0; i < 3; ++i) {
            const int y0 = SP_ACT_Y0 + i * SP_ACT_PITCH;
            const size_t lit = lit_between(fb, y0 + SP_ACT_SUM_DY - 2, y0 + SP_ACT_SUM_DY + 22);
            CHECK(lit > 100, "row %d drew no summary (%zu)", i, lit);
            CHECK(lit_between(fb, y0 - 3, y0 + 12) > 40, "row %d drew no kind and age", i);
        }
        /* Rows are readable content: inside the safe radius, every pixel. */
        CHECK(count_color_within(fb, SP_C_INK, JR_DISPLAY_SAFE_R + 1, 999, HUD_H)
                  == 0, "row ink outside the safe radius");
        free(fb);
    }

    sp_compose_detail(JR_DISPLAY_SPACE_ACTIVITY);
    CHECK(s_detail_rows == 3, "sheet rows %d", s_detail_rows);
    CHECK(strcmp(s_detail_label[0], "SAID") == 0 &&
              strcmp(s_detail_value[0], "JUST NOW") == 0,
          "sheet says when: '%s' '%s'", s_detail_label[0], s_detail_value[0]);
    s_fake_us = 7LL * 60 * 1000 * 1000;
    sp_compose_detail(JR_DISPLAY_SPACE_ACTIVITY);
    CHECK(strcmp(s_detail_value[2], "7M AGO") == 0, "aged: '%s'",
          s_detail_value[2]);
    sp_compose();
    CHECK(strcmp(s_act_age[2], "7M AGO") == 0, "the screen ages the same way: '%s'", s_act_age[2]);
    s_fake_us = 0;

    /* A long summary is cut at the row and marked, never silently. */
    jr_display_activity_push("WEATHER", "83 OVERCAST FORT LAUDERDALE");
    sp_compose();
    CHECK(strcmp(s_act_row[0], "83 OVERCAST FORT...") == 0,
          "the cut is a whole word and three dots, got '%s'", s_act_row[0]);
    CHECK(strcmp(s_act_kind[0], "WEATHER") == 0, "the kind is kept whole, got '%s'", s_act_kind[0]);
    reset_activity();
}

/* A deep sleep or a reset restarts esp_timer and wipes RAM: what the RTC
 * copy holds is all that comes back. Simulated here by clearing the RAM rows
 * and the timer, then restoring as jr_display_start does at boot. */
static void act_simulate_boot(void)
{
    reset_activity();
    s_act_restored = false;
    s_fake_us = 0;
}

static void act_rtc_seal(void)
{
    s_act_rtc.check = act_rtc_sum(&s_act_rtc);
}

/* THE RECEIPT OUTLIVES THE SLEEP. Rows pushed before a sleep come back in
 * order after it, aged by the wall clock (their esp_timer stamps died with
 * the boot), EARLIER when no clock stamped them; and whatever a cold
 * power-on leaves in RTC memory is refused whole. Mutations: dropping the
 * act_rtc_save call empties the restored feed; dropping the checksum test
 * lets the sealed-then-flipped block through. */
static void test_activity_survives_the_sleep(void)
{
    act_simulate_boot();
    memset(&s_act_rtc, 0, sizeof s_act_rtc);
    s_fake_us = 5LL * 60 * 1000 * 1000;
    jr_display_activity_push("WEB", "FOUND THREE RESULTS");
    jr_display_activity_push("RAIN", "IN 2H 70% LIKELY");
    CHECK(s_act_rtc.magic == ACT_RTC_MAGIC && s_act_rtc.count == 2U,
          "a push writes the RTC copy (magic %08x, count %u)",
          (unsigned)s_act_rtc.magic, (unsigned)s_act_rtc.count);

    /* the sleep: RAM gone, timer back at zero, one row stamped 10 min ago
     * by the wall clock and one never stamped at all */
    const uint32_t now = act_wall_now();
    CHECK(now != 0U, "the host clock is a valid wall clock");
    s_act_rtc.wall[0] = now - 600U;
    s_act_rtc.wall[1] = 0U;
    act_rtc_seal();
    act_simulate_boot();
    CHECK(act_rtc_restore() == 2U, "two rows come back");
    CHECK(act_rtc_restore() == 0U, "restore runs once per boot");
    stage_space(JR_DISPLAY_SPACE_ACTIVITY);
    sp_compose();
    CHECK(s_act_rows == 2, "rows after the sleep: %d", s_act_rows);
    CHECK(strcmp(s_act_row[0], "IN 2H 70% LIKELY") == 0 &&
              strcmp(s_act_row[1], "FOUND THREE RESULTS") == 0 &&
              strcmp(s_act_age[0], "10M AGO") == 0 && strcmp(s_act_age[1], "EARLIER") == 0,
          "same rows, same order: '%s' / '%s'", s_act_row[0], s_act_row[1]);
    sp_compose_detail(JR_DISPLAY_SPACE_ACTIVITY);
    CHECK(strcmp(s_detail_value[0], "10M AGO") == 0,
          "aged by the wall clock, not the restarted timer: '%s'", s_detail_value[0]);
    CHECK(strcmp(s_detail_value[1], "EARLIER") == 0,
          "no stamp, no invented age: '%s'", s_detail_value[1]);

    /* a push after the wake is newest, live, and the older rows keep theirs */
    s_fake_us = 3LL * 60 * 1000 * 1000;
    jr_display_activity_push("SAID", "GOOD MORNING");
    s_fake_us = 4LL * 60 * 1000 * 1000;
    sp_compose();
    sp_compose_detail(JR_DISPLAY_SPACE_ACTIVITY);
    CHECK(s_act_rows == 3 && strcmp(s_act_kind[0], "SAID") == 0 &&
              strcmp(s_act_kind[1], "RAIN") == 0,
          "new row on top: '%s' / '%s'", s_act_row[0], s_act_row[1]);
    CHECK(strcmp(s_detail_value[0], "1M AGO") == 0 &&
              strcmp(s_detail_value[1], "10M AGO") == 0 &&
              strcmp(s_detail_value[2], "EARLIER") == 0,
          "ages: '%s' '%s' '%s'", s_detail_value[0], s_detail_value[1],
          s_detail_value[2]);
    CHECK(s_act_rtc.count == 3U, "the RTC copy follows, count %u",
          (unsigned)s_act_rtc.count);

    /* a cold power-on: RTC noise, even noise that happens to carry the magic */
    act_rtc_t good = s_act_rtc;
    memset(&s_act_rtc, 0xA5, sizeof s_act_rtc);
    act_simulate_boot();
    CHECK(act_rtc_restore() == 0U && s_act_count == 0U, "noise is refused");
    s_act_rtc = good;
    s_act_rtc.sum[1][0] ^= 0x01;                       /* flipped, not resealed */
    act_simulate_boot();
    CHECK(act_rtc_restore() == 0U, "a bad checksum is refused");
    s_act_rtc = good;
    s_act_rtc.magic = ACT_RTC_MAGIC + 1U;              /* another layout version */
    act_rtc_seal();
    act_simulate_boot();
    CHECK(act_rtc_restore() == 0U, "another version is refused");
    s_act_rtc = good;
    s_act_rtc.count = ACT_MAX + 1U;
    act_rtc_seal();
    act_simulate_boot();
    CHECK(act_rtc_restore() == 0U, "a count past the cap is refused");
    s_act_rtc = good;
    memset(s_act_rtc.kind[2], 'X', ACT_KIND_CAP);      /* no terminator */
    act_rtc_seal();
    act_simulate_boot();
    CHECK(act_rtc_restore() == 0U, "an unterminated kind is refused");
    s_act_rtc = good;
    s_act_rtc.sum[0][2] = '\x07';
    act_rtc_seal();
    act_simulate_boot();
    CHECK(act_rtc_restore() == 0U, "a control byte is refused");
    sp_compose();
    CHECK(s_act_rows == 0, "a refused block shows NOTHING YET, got %d rows",
          s_act_rows);
    s_act_rtc = good;
    act_simulate_boot();
    CHECK(act_rtc_restore() == 3U, "the intact block still restores");

    memset(&s_act_rtc, 0, sizeof s_act_rtc);
    act_simulate_boot();
    s_act_restored = true;
    reset_nav();
}

/* The DESK sheet re-cut main.c's 12-glyph title to the 10-glyph value column,
 * deleting the "." mark title_shorten() spends its last glyph on. The marked
 * title is now the sheet's head, intact. */
static void test_desk_sheet_heads_with_the_marked_task(void)
{
    jr_display_desk_set_task("DEPLOY STAG.", 64U, JR_DISPLAY_AGENT_WORKING);
    sp_compose_detail(JR_DISPLAY_SPACE_DESK);
    CHECK(strcmp(s_detail_head, "DEPLOY STAG.") == 0,
          "the whole marked title should head the sheet, got '%s'",
          s_detail_head);
    for (int i = 0; i < s_detail_rows; ++i) {
        CHECK(strcmp(s_detail_label[i], "JOB") != 0,
              "the JOB row is back, and it truncates");
    }
    jr_display_desk_set_task("", 0U, JR_DISPLAY_AGENT_NONE);
    sp_compose_detail(JR_DISPLAY_SPACE_DESK);
    CHECK(strcmp(s_detail_head, "TASK") == 0,
          "no task should head TASK, got '%s'", s_detail_head);
}

/* Two ACTIVITY rows read "FORGIVE ME, SIR. I." and "I'M NOT QUITE.": the cut
 * took a word boundary and spent the last glyph on a full stop, so a
 * fragment looked like an ending. A sentence that fits ends the row; a cut
 * mid-sentence is "..." on a whole word, never ".". The strings are the
 * cut points of those two lines (19 glyphs, what a SAID row has left). */
static void test_activity_fit_ends_on_a_sentence_or_an_ellipsis(void)
{
    char out[32];
    jr_display_activity_fit(out, 20, "FORGIVE ME, SIR. I'M AFRAID I DID NOT CATCH THAT");
    CHECK(strcmp(out, "FORGIVE ME, SIR.") == 0, "a whole sentence ends the row, no orphan: '%s'", out);
    jr_display_activity_fit(out, 20, "I'M NOT QUITE SURE WHAT YOU MEAN");
    CHECK(strcmp(out, "I'M NOT QUITE...") == 0, "a mid-sentence cut is a word and three dots: '%s'", out);
    jr_display_activity_fit(out, 20, "  ONE SENTENCE. TWO SENTENCES HERE");
    CHECK(strcmp(out, "ONE SENTENCE.") == 0, "leading spaces go and the first sentence stands: '%s'", out);
    jr_display_activity_fit(out, 20, "VOLUME IS NOW FORTY");
    CHECK(strcmp(out, "VOLUME IS NOW FORTY") == 0, "a line that fits is left alone: '%s'", out);
    jr_display_activity_fit(out, 20, "ANTIDISESTABLISHMENTARIANISM IS LONG");
    CHECK(strlen(out) == 19 && strcmp(out + 16, "...") == 0,
          "one long word is cut hard and marked: '%s'", out);
    for (int cap = 5; cap < 30; cap++) {
        jr_display_activity_fit(out, (size_t)cap, "I'M NOT QUITE SURE WHAT YOU MEAN, SIR");
        CHECK(strlen(out) < (size_t)cap, "fits in cap %d: '%s'", cap, out);
        CHECK(strlen(out) < 3 || strcmp(out + strlen(out) - 1, ".") != 0 ||
                  strcmp(out + strlen(out) - 3, "...") == 0,
              "never a lone full stop at cap %d: '%s'", cap, out);
    }
    /* and the sheet's own cut, for a store longer than the row */
    __atomic_store_n(&s_act_count, 0U, __ATOMIC_RELEASE);
    jr_display_activity_push("SAID", "FORGIVE ME, SIR. I'M AFRAID");
    sp_compose_activity();
    CHECK(strcmp(s_act_row[0], "FORGIVE ME, SIR.") == 0, "the row ends the sentence: '%s'", s_act_row[0]);
    jr_display_activity_push("SAID", "I'M NOT QUITE SURE WHAT YOU MEAN");
    sp_compose_activity();
    CHECK(strcmp(s_act_row[0], "I'M NOT QUITE SURE WHAT...") != 0 &&
              strcmp(s_act_row[0] + strlen(s_act_row[0]) - 3, "...") == 0,
          "the row ends on three dots: '%s'", s_act_row[0]);
}

/* The orbit rail spanned r184-196 against a measured free band of r185-194,
 * so it overwrote baked art at both edges every frame. Everything the shell
 * draws outside the safe radius must now sit inside the band. */
static void test_orbit_stays_in_free_band(void)
{
    stage_space(JR_DISPLAY_SPACE_JARVIS);
    uint16_t *fb = render_frame();
    if (!fb) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        return;
    }
    size_t inside = 0, outside = 0;
    int omin = 0, omax = 0;
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        if (fb[i] == POISON) {
            continue;
        }
        const int r = radius_of((int)i);
        if (r <= JR_DISPLAY_SAFE_R) {
            continue;
        }
        if (r >= 184 && r <= 195) {   /* r186-194 plus one anti-aliased pixel each side */
            inside++;
        } else {
            outside++;
            if (outside == 1 || r < omin) omin = r;
            if (outside == 1 || r > omax) omax = r;
        }
    }
    CHECK(inside > 0, "the orbit drew nothing in r184-195");
    CHECK(outside == 0, "%zu orbit pixels outside r184-195 (r%d..%d)", outside, omin, omax);
    free(fb);
}

/* One battery alarm: the POWER arc must use the rim's red, on the rim's rule
 * (low AND not charging). It used amber, so a low cell wore two alarm hues
 * at once on the one screen about the battery. */
static void test_low_battery_uses_the_rim_palette(void)
{
    stage_space(JR_DISPLAY_SPACE_STATUS);
    jr_display_power_set(8U, 3600U, false, false);
    uint16_t *fb = render_frame();
    if (!fb) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        return;
    }
    size_t red = 0, amber = 0;
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        if (fb[i] == SP_C_RED) {
            red++;
        } else if (fb[i] == SP_C_AMBER) {
            amber++;
        }
    }
    CHECK(red > 0, "8%% battery drew no red");
    CHECK(amber == 0, "8%% battery drew %zu amber pixels", amber);
    free(fb);

    /* A low cell on the charger is not an alarm — same rule as the rim. */
    jr_display_power_set(8U, 3600U, true, true);
    fb = render_frame();
    if (!fb) {
        return;
    }
    red = 0;
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        if (fb[i] == SP_C_RED) {
            red++;
        }
    }
    CHECK(red == 0, "charging at 8%% still drew %zu red pixels", red);
    free(fb);
}

/* Mid-slide, a focal arc is drawn about the OFFSET centre but its wedge was
 * measured about the panel centre, so the arc's endpoints walked round the
 * ring as the screen slid. Draw POWER at rest and 60 px down the slide: the
 * slid frame must be the resting frame translated by 60 rows, pixel for
 * pixel, inside the focal band. Reverting sp_annulus_row's `dy` to
 * `y - SP_CY` fails this. */
static void check_focal_follows_the_slide(int space, int rmax)
{
    const int oy = 64;      /* a multiple of the glow dither's period */
    stage_space(space);
    jr_display_power_set(37U, 3800U, false, false);
    set_weather(true, 83, 86, 76, JR_DISPLAY_SKY_RAIN, "RAIN", 0U);
    sp_compose();
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *rest = malloc(px * sizeof *rest);
    uint16_t *slid = malloc(px * sizeof *slid);
    if (!rest || !slid) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        free(rest);
        free(slid);
        return;
    }
    for (size_t i = 0; i < px; ++i) {
        rest[i] = POISON;
        slid[i] = POISON;
    }
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        sp_draw_space(&s_display, y, y2, rest + (size_t)y * HUD_W, space, 0,
                      255, SP_ENTRY_TOTAL_MS);
        sp_draw_space(&s_display, y, y2, slid + (size_t)y * HUD_W, space, oy,
                      255, SP_ENTRY_TOTAL_MS);
    }
    size_t compared = 0, mismatched = 0, drawn = 0;
    for (int y = 0; y < HUD_H - oy; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const int dx = x - SP_CX, dy = y - SP_CY;
            if (dx * dx + dy * dy > rmax * rmax) {
                continue;            /* the focal object only: text also slides */
            }
            const uint16_t a = rest[(size_t)y * HUD_W + x];
            const uint16_t b = slid[(size_t)(y + oy) * HUD_W + x];
            compared++;
            drawn += a != POISON;
            if (a != b) {
                mismatched++;
            }
        }
    }
    CHECK(compared > 0, "space %d: nothing compared", space);
    CHECK(drawn > 1000, "space %d: the focal object drew only %zu pixels",
          space, drawn);
    CHECK(mismatched == 0,
          "space %d: %zu of %zu focal pixels differ after the slide", space,
          mismatched, compared);
    free(rest);
    free(slid);
}

static void test_focal_wedge_follows_the_slide(void)
{
    check_focal_follows_the_slide(JR_DISPLAY_SPACE_STATUS, 140);
    /* WEATHER has three arcs, a mark and three lines of text, all placed
     * about SP_CY + oy; one of them measured about the panel centre would
     * shear exactly as the POWER wedge once did. */
    check_focal_follows_the_slide(JR_DISPLAY_SPACE_WEATHER, 140);
}

/* The OTA warning is pinned: nothing else may replace it until the upload
 * ends. The voice task's captions used to win by being last. */
static void test_pinned_caption_survives_other_writers(void)
{
    jr_display_caption_unpin();
    jr_display_caption_pin("UPDATING - DO NOT UNPLUG");
    jr_display_caption_set("LISTENING");
    CHECK(strcmp(s_caption_text, "UPDATING - DO NOT UNPLUG") == 0,
          "a pinned caption was replaced, got '%s'", s_caption_text);
    jr_display_caption_clear();
    CHECK(s_caption_text[0] != '\0', "a pinned caption was cleared");
    jr_display_caption_unpin();
    jr_display_caption_set("LISTENING");
    CHECK(strcmp(s_caption_text, "LISTENING") == 0,
          "after unpin the next writer should win, got '%s'", s_caption_text);
    jr_display_caption_clear();
}

/* Every face the port can name has a clip on the partition, a frame rate the
 * panel can hold, and a procedural stand-in. A face added to jr_face_t without
 * all three renders blank on glass and nothing on the device says why (the
 * asset pipeline marks a missing clip "lack": true and carries on), so the
 * table is pinned here, by basename, against the CMake staging list. */
static void test_every_face_has_a_clip_and_a_hud_face(void)
{
    static const struct { jr_face_t face; const char *base; } expect[] = {
        { JR_FACE_IDLE,      "rwave_idle.eaf" },
        { JR_FACE_LISTENING, "rwave_listen.eaf" },
        { JR_FACE_THINKING,  "rwave_think.eaf" },
        { JR_FACE_SPEAKING,  "rwave_speak.eaf" },
        { JR_FACE_ERROR,     "error.eaf" },
        { JR_FACE_RESTING,   "rwave_rest.eaf" },
        { JR_FACE_MUTED,     "rwave_muted.eaf" },
        { JR_FACE_LINKING,   "rwave_link.eaf" },
        { JR_FACE_DIAL_DIVER,  "dial_diver.eaf" },
        { JR_FACE_DIAL_DRESS,  "dial_dress.eaf" },
        { JR_FACE_DIAL_PILOT,  "dial_pilot.eaf" },
        { JR_FACE_DIAL_FUTURE, "dial_future.eaf" },
        { JR_FACE_DIAL_SKELETON,  "dial_skeleton.eaf" },
        { JR_FACE_DIAL_CELESTIAL, "dial_celestial.eaf" },
    };
    CHECK(sizeof expect / sizeof expect[0] == (size_t)JR_FACE_COUNT,
          "face table has %zu rows for %d faces",
          sizeof expect / sizeof expect[0], (int)JR_FACE_COUNT);
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        const char *path = face_asset(expect[i].face);
        CHECK(path != NULL, "face %d has no clip path", (int)expect[i].face);
        if (path) {
            const char *slash = strrchr(path, '/');
            CHECK(slash && strcmp(slash + 1, expect[i].base) == 0,
                  "face %d maps to %s, expected %s",
                  (int)expect[i].face, path, expect[i].base);
        }
        uint32_t fps = face_fps(expect[i].face);
        CHECK(fps >= 8 && fps <= 24, "face %d fps %u outside 8..24",
              (int)expect[i].face, (unsigned)fps);
        CHECK(hud_face_of(expect[i].face) < HUD_FACE_COUNT,
              "face %d has no procedural stand-in", (int)expect[i].face);
    }
    CHECK(face_asset((jr_face_t)JR_FACE_COUNT) == NULL,
          "the bound is not a face");
    /* The quiet faces are slow by design; a 24 fps rest clip would spend the
     * DREAM budget decoding a breath nobody can see. */
    CHECK(face_fps(JR_FACE_RESTING) <= 8, "rest clip runs at %u fps",
          (unsigned)face_fps(JR_FACE_RESTING));
    CHECK(face_fps(JR_FACE_MUTED) <= 8, "muted clip runs at %u fps",
          (unsigned)face_fps(JR_FACE_MUTED));
}

/* N8.12 exercises the production intent, presenter and strip seams. No test
 * reimplementation stands in for the stopped-emote or refresh decisions. */
static void dream_fixture(void)
{
    static uint8_t clip[32];
    reset_nav();
    s_display.task = (TaskHandle_t)1;
    s_display.gfx = (gfx_handle_t)1;
    s_display.disp = (gfx_disp_t *)1;
    s_display.anim = (gfx_obj_t *)1;
    s_display.board.width = HUD_W;
    s_display.board.height = HUD_H;
    s_display.board.panel = (esp_lcd_panel_handle_t)1;
    s_display.board.swap_color_bytes = false;
    s_display.clips[JR_FACE_RESTING] = (jr_display_clip_t){ clip, sizeof clip, 8U, 7U, false };
    s_display.active = &s_display.clips[JR_FACE_RESTING];
    s_display.loaded_face = s_display.shown_face = JR_FACE_RESTING;
    s_display.last_end = 7;
    s_display.blanked = false;
    s_display.dream_active = false;
    s_display.quiet_active = false;
    s_display.panel_touched = s_display.panel_prepared = false;
    s_display.panel_sleep_attempted = false;
    memset(s_display.transfer_tags, 0, sizeof s_display.transfer_tags);
    s_display.transfer_head = s_display.transfer_tail = 0U;
    s_display.first_glance_time_transfer_state = 0U;
    s_display.first_glance_time_transfer_done_boot_ms = 0U;
    s_display.requested_word = (uint32_t)JR_FACE_RESTING << JR_DISPLAY_CMD_FACE_SHIFT;
    s_display.applied_face = JR_FACE_RESTING;
    s_display.requested_amplitude = 0U;
    s_display.test_pattern = JR_DISPLAY_TEST_OFF;
    s_display.snapshot_refresh_requested = 0U;
    s_display.apply_retry_gate_ms = 0U;
    s_hud_env_word = 0xffu;
    s_hud_enabled = 1U;
    s_choice_n = 0;
    s_commit_pct = 0U;
    s_ripple_start_ms = s_bloom_start_ms = 0U;
    s_canvas_until_ms = 0U;
    s_surface_active = false;
    s_caption_on = 0;
    atomic_store(&s_caption_pinned, false);
    s_panel_off = 0U;
    s_panel_on_ack_valid = 0U;
    s_panel_on_ack_word = 0U;
    s_detail_ease = s_shade_ease = s_ask_ease = s_canvas_ease = 0;
    s_stub_brightness_error = s_stub_on_error = s_stub_off_error = ESP_OK;
    s_stub_sleep_in_error = s_stub_sleep_out_error = s_stub_draw_error = ESP_OK;
    s_stub_brightness_calls = s_stub_on_calls = s_stub_off_calls = 0U;
    s_stub_sleep_in_calls = s_stub_sleep_out_calls = s_stub_draw_calls = 0U;
    s_stub_draw_hook = s_stub_delay_hook = NULL;
    s_power_word = 74U;
    s_ota_word = JR_DISPLAY_OTA_IDLE;
    s_clock_word = 0U;
    s_brightness_want = 8U;
    atomic_store(&s_brightness_have, 8U);
    atomic_store(&s_brightness_shown, 8);
    s_render_fps = 2U;
    s_fake_us = 1000000;
    jr_display_set_hud_env(74U, false, true, 0, 0);
    jr_display_dream_set(true);
    jr_display_caption_set("ASLEEP. TAP TO WAKE");
    s_stub_notifies = s_stub_refreshes = 0U;
    s_stub_anim_starts = s_stub_anim_stops = 0U;
    s_stub_anim_visible = true;
    s_dial_blit = false;
}

static void test_dream_intent_preserves_env_and_ignores_unchanged_publish(void)
{
    dream_fixture();
    jr_display_set_hud_env(61U, false, true, 12, -8);
    CHECK(s_hud_env_word & JR_DISPLAY_ENV_DREAM, "env publication preserves DREAM intent");
    const uint32_t env = s_hud_env_word & ~JR_DISPLAY_ENV_DREAM;
    jr_display_dream_set(false);
    CHECK(s_hud_env_word == env, "DREAM clear preserves battery/privacy/tilt");
    jr_display_dream_set(true);
    CHECK((s_hud_env_word & ~JR_DISPLAY_ENV_DREAM) == env, "DREAM set preserves env");
    jr_display_clock_set(true, 17, 42, 1);
    s_stub_notifies = 0U;
    jr_display_dream_set(true);
    jr_display_set_hud_env(61U, false, true, 12, -8);
    display_publish(s_display.requested_word);
    jr_display_caption_set("ASLEEP. TAP TO WAKE");
    jr_display_clock_set(true, 17, 42, 2);
    CHECK(s_stub_notifies == 0U, "unchanged publishers/seconds do not wake DREAM (%u)", s_stub_notifies);
    CHECK(((s_clock_word >> 17) & 0x3fu) == 2U, "seconds still reach WATCH intent");
    jr_display_set_hud_env(61U, false, true, -12, 8);
    CHECK(s_stub_notifies == 0U, "tilt cannot animate the DREAM rim");
    jr_display_clock_set(true, 17, 43, 0);
    CHECK(s_stub_notifies == 1U, "minute change wakes presenter");
    jr_display_set_hud_env(60U, false, true, -12, 8);
    CHECK(s_stub_notifies == 2U, "battery change wakes presenter");
    jr_display_dream_set(false);
    s_stub_notifies = 0U;
    jr_display_clock_set(true, 17, 43, 1);
    CHECK(s_stub_notifies == 1U, "awake WATCH seconds wake presenter");
    display_publish((uint32_t)JR_FACE_ERROR << JR_DISPLAY_CMD_FACE_SHIFT);
    CHECK(s_stub_notifies == 2U, "changed face wakes immediately");
}

static void test_dream_never_hides_foreground_or_ota(void)
{
    dream_fixture();
    const char *quiet[] = { "JARVIS", "MUTED - HOLD TO LISTEN", "MUTED - HOLD TO RESUME",
        "MUTED - TAP TO WAKE", "ASLEEP - TAP TO WAKE", "ASLEEP. TAP TO WAKE" };
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; ++i) {
        jr_display_caption_set(quiet[i]);
        CHECK(dream_eligible(&s_display), "quiet hint permits DREAM: %s", quiet[i]);
    }
    const char *foreground[] = { "DONE", "ERROR - RETRY", "CONSENT REQUIRED",
        "MUTED - NETWORK ERROR", "ASLEEP. OTA FAILED" };
    for (size_t i = 0; i < sizeof foreground / sizeof foreground[0]; ++i) {
        jr_display_caption_set(foreground[i]);
        CHECK(!dream_eligible(&s_display), "foreground remains lit: %s", foreground[i]);
    }
    jr_display_caption_pin("JARVIS");
    CHECK(!dream_eligible(&s_display), "even an ambient pinned instruction stays lit");
    jr_display_caption_unpin();
    const char *answers[] = { "YES", "NO" };
    s_stub_notifies = 0U;
    jr_display_present_choices("Allow?", answers, 2);
    CHECK(s_stub_notifies > 0U && !dream_eligible(&s_display), "ask wakes and defeats stale DREAM");
    jr_display_dismiss_choices();
    CHECK(dream_eligible(&s_display), "dismissed ask releases guard");
    jr_display_surface_t card = { .kind = JR_DISPLAY_SURFACE_CONSENT };
    strcpy(card.title, "Allow camera?");
    CHECK(jr_display_surface_present(&card) == ESP_OK && !dream_eligible(&s_display),
          "consent card defeats stale DREAM");
    jr_display_surface_dismiss();
    s_canvas_until_ms = 2000U;
    CHECK(!dream_eligible(&s_display), "canvas remains visible");
    jr_display_canvas_clear();
    for (int state = JR_DISPLAY_OTA_PREFLIGHT; state <= JR_DISPLAY_OTA_ROLLED_BACK; ++state) {
        jr_display_ota_set((jr_display_ota_state_t)state, 25U, 0U, 1U, true);
        CHECK(dream_eligible(&s_display) == (state == JR_DISPLAY_OTA_VALID),
              "OTA state %d keeps its correct foreground", state);
    }
    jr_display_ota_set(JR_DISPLAY_OTA_IDLE, 0U, 0U, 1U, true);
    s_nav_word = (uint32_t)JR_DISPLAY_OVERLAY_SHADE << NAV_OVL_SHIFT;
    CHECK(!dream_eligible(&s_display), "shade stays lit");
    s_nav_word = JR_DISPLAY_SPACE_WEATHER;
    CHECK(!dream_eligible(&s_display), "weather remains foreground");
    s_nav_word = JR_DISPLAY_SPACE_WATCH;
    CHECK(dream_eligible(&s_display), "quiet WATCH permits DREAM");
    s_display.requested_word = (uint32_t)JR_FACE_SPEAKING << JR_DISPLAY_CMD_FACE_SHIFT;
    CHECK(!dream_eligible(&s_display), "busy face remains foreground");
    s_display.requested_word = (uint32_t)JR_FACE_RESTING << JR_DISPLAY_CMD_FACE_SHIFT;
    jr_display_set_hud_env(74U, true, true, 0, 0);
    CHECK(!dream_eligible(&s_display), "charging never enters local DREAM");
    jr_display_set_hud_env(74U, false, true, 0, 0);
    jr_display_power_set(74U, 3996U, true, false);
    CHECK(!dream_eligible(&s_display), "USB stays lit even when not charging");
    jr_display_power_set(74U, 3996U, false, false);
    s_display.shell_word = JR_DISPLAY_SHELL_AGENT;
    CHECK(!dream_eligible(&s_display), "operator remains foreground");
    s_display.shell_word = 0U;
    s_display.test_pattern = JR_DISPLAY_TEST_WHITE;
    CHECK(!dream_eligible(&s_display), "panel diagnostic remains foreground");
}

static void test_dream_glass_is_black_clock_and_intact_rims(void)
{
    dream_fixture();
    const size_t n = (size_t)HUD_W * HUD_H;
    uint16_t *whole = malloc(n * sizeof *whole);
    uint16_t *strips = malloc(n * sizeof *strips);
    uint16_t *rim = calloc(n, sizeof *rim);
    CHECK(whole && strips && rim, "DREAM pixel fixtures allocate");
    if (!whole || !strips || !rim) { free(whole); free(strips); free(rim); return; }
    jr_display_clock_set(true, 17, 42, 59);
    for (size_t i = 0; i < n; ++i) { whole[i] = strips[i] = POISON; }
    apply_hud_overlay(&s_display, 0, 0, HUD_W, HUD_H, whole);
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int end = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        apply_hud_overlay(&s_display, 0, y, HUD_W, end, strips + (size_t)y * HUD_W);
    }
    CHECK(memcmp(whole, strips, n * sizeof *whole) == 0, "minute clock is strip invariant");
    const hud_env_t env = { .face = HUD_FACE_IDLE, .batt_pct = 74U, .privacy_muted = true };
    hud_overlay_frame(rim, 0, HUD_H, 0U, false, &env);
    size_t unexpected = 0U, rim_pixels = 0U, black = 0U, clock_pixels = 0U;
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const size_t i = (size_t)y * HUD_W + x;
            const bool clock_box = x >= 120 && x < 346 && y >= 195 && y < 270;     /* the HERO time, centred */
            if (!clock_box && whole[i] != rim[i]) { unexpected++; }
            if (rim[i]) { rim_pixels++; }
            if (!whole[i]) { black++; }
            if (clock_box && whole[i] != 0U) { clock_pixels++; }       /* warm amber, a quarter strength */
        }
    }
    CHECK(unexpected == 0U, "only clock pixels differ from the original privacy/battery rims (%zu)", unexpected);
    CHECK(rim_pixels > 100U && clock_pixels > 100U, "both rims and dim clock are present (%zu/%zu)", rim_pixels, clock_pixels);
    CHECK(black * 100U > n * 90U, "DREAM is more than 90 percent black (%zu/%zu)", black, n);
    char text[6];
    dream_clock_text(text, s_clock_word);
    CHECK(strcmp(text, "17:42") == 0, "known time is honest HH:MM");
    jr_display_clock_set(false, 0, 0, 0);
    dream_clock_text(text, s_clock_word);
    CHECK(strcmp(text, "--:--") == 0, "unknown time never retains an invented clock");
    apply_hud_overlay(&s_display, 0, 0, HUD_W, HUD_H, strips);
    CHECK(memcmp(whole, strips, n * sizeof *whole) != 0, "unknown mark replaces prior time pixels");
    free(whole); free(strips); free(rim);
}

static void test_dream_stops_emote_and_refreshes_only_for_real_changes(void)
{
    dream_fixture();
    uint32_t last_word = s_display.requested_word;
    jr_display_clock_set(true, 17, 42, 1);
    CHECK(presenter_dream_tick(&s_display, &last_word), "DREAM enters production presenter");
    CHECK(s_stub_anim_stops == 1U && !s_stub_anim_visible && s_display.blanked,
          "emote stops and hides");
    const unsigned initial = s_stub_refreshes;
    CHECK(presenter_wait_ms(&s_display) == 1000U, "settled DREAM sleeps between diagnostics");
    jr_display_clock_set(true, 17, 42, 2);
    presenter_dream_tick(&s_display, &last_word);
    CHECK(s_stub_refreshes == initial && s_stub_anim_stops == 1U, "seconds neither refresh nor restop emote");
    jr_display_set_hud_env(74U, false, true, 15, -10);
    presenter_dream_tick(&s_display, &last_word);
    CHECK(s_stub_refreshes == initial, "tilt leaves DREAM static");
    jr_display_clock_set(true, 17, 43, 0);
    presenter_dream_tick(&s_display, &last_word);
    CHECK(s_stub_refreshes == initial + 1U, "minute creates one fresh frame");
    jr_display_set_hud_env(73U, false, true, 15, -10);
    presenter_dream_tick(&s_display, &last_word);
    CHECK(s_stub_refreshes == initial + 2U, "battery creates one fresh frame");
    jr_display_set_brightness(50U);
    CHECK(presenter_wait_ms(&s_display) == 500U, "brightness settles at requested FPS");
    s_fade_prev_ms = 1000U;
    for (int i = 0; i < 8; ++i) {
        s_fake_us += 500000;
        const unsigned frames = s_stub_refreshes;
        presenter_dream_tick(&s_display, &last_word);
        if (s_stub_refreshes != frames) {
            overlay_fade_tick();
            brightness_pump();
        }
    }
    CHECK(!brightness_pending() && s_stub_panel_brightness == 50U, "static DREAM still applies target brightness");
    const unsigned settled = s_stub_refreshes;
    presenter_dream_tick(&s_display, &last_word);
    CHECK(s_stub_refreshes == settled && presenter_wait_ms(&s_display) == 1000U,
          "brightness burst ends when panel settles");
    jr_display_caption_set("ERROR - RETRY");
    CHECK(!presenter_dream_tick(&s_display, &last_word) && last_word == UINT32_MAX,
          "foreground invalidates same-face latch immediately");
    bool stale = false;
    CHECK(apply_face(&s_display, JR_FACE_RESTING, JR_FACE_RESTING, 0U, &stale) == ESP_OK &&
          !stale && s_stub_anim_starts > 0U && s_stub_anim_visible && !s_display.blanked,
          "same face animation is restored on exit");
    jr_display_set_render_fps(5U);
    CHECK(presenter_wait_ms(&s_display) == 200U, "awake presenter respects 5 FPS rather than 33 ms polling");
}

static void test_dream_capture_panel_off_and_input_wake(void)
{
    dream_fixture();
    uint32_t last_word = s_display.requested_word;
    presenter_dream_tick(&s_display, &last_word);
    const unsigned before = s_stub_refreshes;
    s_display.snapshot_refresh_requested = 1U;
    presenter_refresh_pending(&s_display, false);
    CHECK(s_stub_refreshes == before + 1U && s_display.snapshot_refresh_requested == 0U,
          "snapshot gets a full frame with emote stopped");
    presenter_refresh_pending(&s_display, false);
    CHECK(s_stub_refreshes == before + 1U, "snapshot request consumed once");
    jr_display_panel_off_request();
    presenter_refresh_pending(&s_display, false);
    CHECK(s_stub_refreshes == before + 2U, "panel-off request creates a flush despite static clock");
    s_stub_disp_user_data = &s_display;
    uint16_t strip[HUD_W] = {0};
    panel_flush(s_display.disp, 0, 0, HUD_W, 1, strip);
    CHECK(jr_display_panel_is_off() && s_stub_panel_brightness == 0U,
          "flush owner applies panel sleep handoff");
    s_stub_disp_user_data = NULL;
    s_panel_off = 0U;
    s_stub_notifies = 0U;
    jr_display_nav_next();
    CHECK(s_stub_notifies > 0U && !(s_hud_env_word & JR_DISPLAY_ENV_DREAM), "navigation clears DREAM and wakes");
    CHECK(!presenter_dream_tick(&s_display, &last_word), "input immediately releases stopped presenter");
    jr_display_dream_set(true);
    jr_display_ripple(233, 233);
    CHECK(!(s_hud_env_word & JR_DISPLAY_ENV_DREAM), "tap feedback clears DREAM");
    jr_display_dream_set(true);
    CHECK(!dream_eligible(&s_display), "stale mood cannot erase an active tap ripple");
    s_fake_us += (int64_t)HUD_RIPPLE_MS * 1000;
    CHECK(dream_eligible(&s_display), "expired ripple releases local guard");
    jr_display_dream_set(true);
    jr_display_bloom();
    CHECK(!(s_hud_env_word & JR_DISPLAY_ENV_DREAM), "wake bloom clears DREAM");
    jr_display_dream_set(true);
    CHECK(!dream_eligible(&s_display), "stale mood cannot erase an active wake bloom");
    s_display.gfx = NULL;
    s_display.task = NULL;
}

static void test_panel_order_color_covers_both_wire_orders(void)
{
    for (int swap = 0; swap < 2; ++swap) {
        s_display.board.swap_color_bytes = swap != 0;
        unsigned bad = 0U;
        for (unsigned c = 0U; c <= UINT16_MAX; ++c) {
            const uint16_t expected = swap ? (uint16_t)((c >> 8) | (c << 8)) : (uint16_t)c;
            if (panel_order_color(&s_display, (uint16_t)c) != expected) { bad++; }
        }
        CHECK(bad == 0U, "retained byte helper covers all65536 colors, swap%d (%u)", swap, bad);
    }
    s_display.board.swap_color_bytes = false;
}

static void snapshot_arrives_while_locking(void)
{
    s_display.snapshot_refresh_requested = 1U;
}

static void test_refresh_failure_retries_keys_snapshot_and_pattern(void)
{
    dream_fixture();
    uint32_t last_word = s_display.requested_word;
    jr_display_clock_set(true, 17, 42, 1);
    CHECK(presenter_dream_tick(&s_display, &last_word), "enter DREAM before fault injection");
    const unsigned before = s_stub_refreshes;
    const uint32_t old_clock = s_display.dream_clock_key;
    const uint32_t old_rim = s_display.dream_rim_key;
    jr_display_clock_set(true, 17, 43, 0);
    jr_display_set_hud_env(73U, false, true, 0, 0);
    s_stub_lock_failures = 1U;
    CHECK(presenter_dream_tick(&s_display, &last_word) && s_stub_refreshes == before,
          "failed DREAM lock submits nothing");
    CHECK(s_display.dream_clock_key == old_clock && s_display.dream_rim_key == old_rim,
          "failed lock preserves both retry keys");
    presenter_dream_tick(&s_display, &last_word);
    CHECK(s_stub_refreshes == before + 1U && s_display.dream_clock_key != old_clock &&
          s_display.dream_rim_key != old_rim, "successful retry commits both changed keys");
    presenter_dream_tick(&s_display, &last_word);
    CHECK(s_stub_refreshes == before + 1U, "unchanged successful keys stay sparse");

    s_display.snapshot_refresh_requested = 1U;
    s_stub_lock_failures = 1U;
    CHECK(!presenter_refresh_pending(&s_display, false) &&
          s_display.snapshot_refresh_requested == 1U, "failed snapshot remains pending");
    CHECK(presenter_refresh_pending(&s_display, false) &&
          s_display.snapshot_refresh_requested == 0U && s_stub_refreshes == before + 2U,
          "snapshot retry submits once");
    s_display.snapshot_refresh_requested = 1U;
    s_stub_lock_hook = snapshot_arrives_while_locking;
    s_stub_lock_failures = 1U;
    CHECK(!presenter_refresh_pending(&s_display, false) &&
          s_display.snapshot_refresh_requested == 1U, "concurrent snapshot survives failed lock");
    s_stub_lock_hook = NULL;
    presenter_refresh_pending(&s_display, false);
    CHECK(s_display.snapshot_refresh_requested == 0U && s_stub_refreshes == before + 3U,
          "coalesced concurrent snapshot retries once");

    uint32_t last_pattern = JR_DISPLAY_TEST_OFF;
    s_display.test_pattern = JR_DISPLAY_TEST_WHITE;
    s_stub_lock_failures = 1U;
    presenter_pattern_tick(&s_display, &last_pattern);
    CHECK(last_pattern == JR_DISPLAY_TEST_OFF && s_stub_refreshes == before + 3U,
          "failed pattern refresh is not acknowledged");
    presenter_pattern_tick(&s_display, &last_pattern);
    CHECK(last_pattern == JR_DISPLAY_TEST_WHITE && s_stub_refreshes == before + 4U,
          "pattern retries then acknowledges");
    presenter_pattern_tick(&s_display, &last_pattern);
    CHECK(s_stub_refreshes == before + 4U, "pattern acknowledgement consumes exactly once");
    s_display.test_pattern = JR_DISPLAY_TEST_OFF;
    s_display.gfx = NULL;
    s_display.task = NULL;
}

static void test_finite_fades_use_elapsed_time_at_rest(void)
{
    const uint32_t periods[] = { 50U, 200U, 500U };
    for (size_t p = 0; p < sizeof periods / sizeof periods[0]; ++p) {
        dream_fixture();
        jr_display_dream_set(false);
        s_clock_word = (1u << 16) | (12u << 8) | 34U;
        s_clock_prog = s_canvas_prog = s_caption_prog = s_ask_prog = 0;
        s_detail_prog = s_shade_prog = 0;
        s_clock_ease = s_canvas_ease = s_caption_ease = s_ask_ease = 0;
        s_detail_ease = s_shade_ease = 0;
        s_canvas_until_ms = 10000U;
        s_caption_on = 1;
        s_choice_n = 1;
        s_nav_word = JR_DISPLAY_SPACE_WEATHER | (1u << NAV_SERIAL_SHIFT) |
                     ((uint32_t)JR_DISPLAY_OVERLAY_DETAIL << NAV_OVL_SHIFT);
        s_display.shell_word = JR_DISPLAY_SHELL_SHADE;
        s_space_serial_seen = 0U;
        s_fade_prev_ms = 1000U;
        const uint32_t period = periods[p];
        uint32_t elapsed = 0U;
        while (elapsed < JR_DISPLAY_FADE_MS + period) {
            elapsed += period;
            s_fake_us = (int64_t)(1000U + elapsed) * 1000;
            overlay_fade_tick();
            if (elapsed >= JR_DISPLAY_CAPTION_FADE_MS + period) {
                CHECK(s_caption_prog == 256 && s_ask_prog == 256 &&
                      s_detail_prog == 256,
                      "250ms lanes settle by nominal+one frame at%ums", period);
            }
            if (elapsed >= SP_SHADE_OPEN_MS + period) {
                CHECK(s_shade_prog == 256,
                      "the shade (300 ms) settles by nominal+one frame at%ums", period);
            }
            if (elapsed >= JR_DISPLAY_SPACE_MS + period) {
                CHECK(s_space_prog == 256, "slide settles by nominal+one frame at%ums", period);
            }
        }
        CHECK(s_clock_prog == 256 && s_canvas_prog == 256,
              "400ms lanes settle by nominal+one frame at%ums", period);
        s_clock_word = 0U;
        s_canvas_until_ms = 0U;
        s_caption_on = 0;
        s_choice_n = 0;
        s_nav_word = JR_DISPLAY_SPACE_JARVIS | (2u << NAV_SERIAL_SHIFT);
        s_display.shell_word = 0U;
        const uint32_t start = elapsed;
        while (elapsed - start < JR_DISPLAY_FADE_MS + period) {
            elapsed += period;
            s_fake_us = (int64_t)(1000U + elapsed) * 1000;
            overlay_fade_tick();
        }
        CHECK(s_clock_prog == 0 && s_canvas_prog == 0 && s_caption_prog == 0 &&
              s_ask_prog == 0 && s_detail_prog == 0 && s_shade_prog == 0,
              "every finite lane exits at%ums", period);
    }
    /* Unsigned elapsed time across a very long stall cannot overflow products
     * or accelerate the separately capped physical brightness slew. */
    s_clock_prog = 0;
    s_clock_word = 1u << 16;
    s_fade_prev_ms = (uint32_t)(s_fake_us / 1000) - UINT32_MAX + 10U;
    atomic_store(&s_brightness_shown, 0);
    s_brightness_want = 100U;
    overlay_fade_tick();
    CHECK(s_clock_prog == 256, "long stall completes finite fade without overflow");
    CHECK(atomic_load(&s_brightness_shown) == 100U * 100U / JR_DISPLAY_BRIGHT_SLEW_MS,
          "brightness retains100ms slew cap");
    s_display.gfx = NULL;
    s_display.task = NULL;
    s_clock_word = 0U;
}

static jr_display_clip_t *allocate_test_clip(jr_face_t face, size_t bytes)
{
    jr_display_clip_t *clip = &s_display.clips[face];
    *clip = (jr_display_clip_t){ heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM), bytes, 8U, 7U, false };
    CHECK(clip->data != NULL, "tracked clip allocation for face%d", face);
    return clip;
}

/* SKELETON and CELESTIAL live in the app image, not on the partition. The
 * host stub hands back the REAL baked EAF; the device hands back flash
 * rodata, which faults on a write. So: the clip is flagged embedded, points
 * at the very bytes (no copy), is never freed or rewritten by any release
 * path, and the bytes are unchanged after decodes. The vendor decoder is not
 * linked on the host (idf_stubs.inc stubs eaf_init / eaf_frame_decode); the
 * real EAF container itself is checked with the shell's own frame counter. */
static void test_embedded_dials_are_read_only_and_never_freed(void)
{
    dream_fixture();
    jr_display_ctx_t *ctx = &s_display;
    ctx->active = ctx->bound = NULL;
    s_stub_anim_source = NULL;
    dial_cache_reset();
    s_stub_eaf_head = (eaf_header_t){ 8, HUD_W, HUD_H };
    const jr_face_t dials[] = { JR_FACE_DIAL_SKELETON, JR_FACE_DIAL_CELESTIAL };
    for (size_t i = 0; i < 2; ++i) {
        const jr_face_t face = dials[i];
        memset(&ctx->clips[face], 0, sizeof ctx->clips[face]);
        size_t n = 0;
        const uint8_t *real = jr_host_embedded_dial(face, &n);
        CHECK(real != NULL && n > 150000U && n < 260000U,
              "dial %d: the real EAF did not load (%zu bytes)", (int)face, n);
        if (real == NULL) { continue; }
        CHECK(eaf_frame_count(real, n) == 2U, "dial %d is one frame plus the sentinel", (int)face);
        uint8_t *copy = malloc(n);
        memcpy(copy, real, n);
        s_stub_free_watch = real;
        s_stub_free_watch_hits = 0U;
        const size_t heap_before = s_stub_heap_bytes;
        const int decodes = s_stub_eaf_decodes;
        CHECK(dial_show(ctx, face), "dial %d shows from the embedded bytes", (int)face);
        jr_display_clip_t *clip = &ctx->clips[face];
        CHECK(clip->embedded && clip->data == real && clip->size == n,
              "dial %d clip is not the embedded bytes in place", (int)face);
        CHECK(s_stub_eaf_init_src == real, "the decoder was handed a copy, not the embedded bytes");
        CHECK(s_stub_eaf_decodes == decodes + 1, "one decode, got %d", s_stub_eaf_decodes - decodes);
        CHECK(s_stub_heap_bytes <= heap_before + (size_t)HUD_W * HUD_H * sizeof(uint16_t),
              "dial %d grew the heap by more than the decoded frame", (int)face);
        clip_release_unbound_dial(ctx, clip);
        CHECK(clip->data == real && clip->embedded && s_stub_free_watch_hits == 0U,
              "the release path let go of an embedded clip");
        /* away and back while the cache still holds it: no decode, no reload */
        CHECK(dial_show(ctx, face) && s_stub_eaf_decodes == decodes + 1 &&
              ctx->clips[face].data == real, "a cached embedded dial is not decoded again");
        /* the cache let go (a sleep, low memory): decoded again from the same bytes */
        s_dial_blit = false;
        dial_cache_release();
        CHECK(dial_show(ctx, face) && s_stub_eaf_decodes == decodes + 2 &&
              ctx->clips[face].data == real, "after a release the embedded bytes decode again");
        CHECK(memcmp(real, copy, n) == 0, "dial %d: the source bytes changed", (int)face);
        CHECK(s_stub_free_watch_hits == 0U, "dial %d: an embedded clip was freed", (int)face);
        free(copy);
        s_stub_free_watch = NULL;
    }
    /* a heap clip is still released after its decode: the flag is the only difference */
    jr_display_clip_t *heap = allocate_test_clip(JR_FACE_DIAL_PILOT, 64U);
    CHECK(dial_show(ctx, JR_FACE_DIAL_PILOT) && heap->data == NULL,
          "a heap dial clip is no longer released");
}

/* ---- THE DIAL CACHE (N17) ------------------------------------------------ */

/* Pixels of the shown dial that differ from what a decode with `salt` writes,
 * over the 12 rows the first test already uses. 0 = the glass shows that dial. */
static int dial_strip_mismatch(const jr_display_ctx_t *ctx, uint16_t salt)
{
    static uint16_t strip[HUD_W * STRIP_ROWS];
    const int y1 = 227 - STRIP_ROWS / 2;
    const int x1 = 10, x2 = HUD_W - 10;
    memset(strip, 0, sizeof strip);
    apply_dial(ctx, x1, y1, x2, y1 + STRIP_ROWS, strip);
    int bad = 0;
    for (int r = 0; r < STRIP_ROWS; ++r) {
        for (int c = 0; c < x2 - x1; ++c) {
            const size_t i = (size_t)(y1 + r) * HUD_W + (size_t)(x1 + c);
            if (strip[(size_t)r * (size_t)(x2 - x1) + (size_t)c] !=
                (uint16_t)(i ^ 0x5a5au ^ salt)) {
                bad++;
            }
        }
    }
    return bad;
}

static const jr_face_t s_dial_of_slot[DIAL_SLOTS] = {
    JR_FACE_DIAL_DIVER, JR_FACE_DIAL_DRESS, JR_FACE_DIAL_PILOT,
    JR_FACE_DIAL_FUTURE, JR_FACE_DIAL_SKELETON, JR_FACE_DIAL_CELESTIAL };

static void dial_clips_drop(void)
{
    for (int i = 0; i < DIAL_SLOTS; ++i) {
        jr_display_clip_t *c = &s_display.clips[s_dial_of_slot[i]];
        if (c->data != NULL && !c->embedded) {
            heap_caps_free(c->data);
        }
        memset(c, 0, sizeof *c);
    }
}

static void dial_clip_seed(jr_face_t face)
{
    jr_display_clip_t *c = &s_display.clips[face];
    if (c->data != NULL && !c->embedded) {
        heap_caps_free(c->data);
    }
    *c = (jr_display_clip_t){ heap_caps_malloc(32U, MALLOC_CAP_SPIRAM), 32U, 8U, 7U, false };
}

static void dial_test_board(void)
{
    s_display.board.width = HUD_W;
    s_display.board.height = HUD_H;
    s_display.board.swap_color_bytes = false;
    s_stub_eaf_head = (eaf_header_t){ 8, HUD_W, HUD_H };
    s_display.missing_faces = 0;
    s_display.bound = s_display.active = NULL;
}

/* A > B > A: each dial is decoded once, the second look at A is the first
 * decode's frame (a re-decode would carry the new salt), and the stats say so. */
static void test_dial_cache_a_b_a_decodes_each_once(void)
{
    jr_display_ctx_t *ctx = &s_display;
    dream_fixture();
    dial_test_board();
    dial_cache_reset();
    dial_clips_drop();
    s_stub_eaf_decodes = 0;
    const size_t frame_bytes = (size_t)HUD_W * HUD_H * sizeof(uint16_t);

    s_stub_eaf_salt = 0x11;
    dial_clip_seed(JR_FACE_DIAL_DIVER);
    CHECK(dial_show(ctx, JR_FACE_DIAL_DIVER), "A shows");
    CHECK(dial_strip_mismatch(ctx, 0x11) == 0, "A is on the glass");
    CHECK(watch_style_bound((uint8_t)JR_WATCH_DIVER),
          "the style is bound the moment its frame is switched in");

    s_stub_eaf_salt = 0x22;
    s_stub_eaf_decode_us = 300000;
    dial_clip_seed(JR_FACE_DIAL_DRESS);
    CHECK(dial_show(ctx, JR_FACE_DIAL_DRESS), "B shows");
    CHECK(dial_strip_mismatch(ctx, 0x22) == 0, "B is on the glass");
    CHECK(!watch_style_bound((uint8_t)JR_WATCH_DIVER) && watch_style_bound((uint8_t)JR_WATCH_DRESS),
          "the bound style follows the dial on the glass");

    s_stub_eaf_salt = 0x33;                     /* a re-decode would show this */
    s_stub_eaf_decode_us = 0;
    CHECK(dial_show(ctx, JR_FACE_DIAL_DIVER), "A again");
    CHECK(s_stub_eaf_decodes == 2, "A and B were each decoded once, got %d", s_stub_eaf_decodes);
    CHECK(dial_strip_mismatch(ctx, 0x11) == 0, "A's second look is A's first frame");
    CHECK(dial_show(ctx, JR_FACE_DIAL_DRESS) && s_stub_eaf_decodes == 2 &&
          dial_strip_mismatch(ctx, 0x22) == 0, "and B's");
    CHECK(s_dial_hits == 2 && s_dial_misses == 2 && s_dial_evictions == 0,
          "hits %u misses %u evictions %u", (unsigned)s_dial_hits, (unsigned)s_dial_misses,
          (unsigned)s_dial_evictions);

    jr_display_diag_t d;
    CHECK(jr_display_get_diag(&d) == ESP_OK, "diag reads");
    CHECK(d.dial_cached == 2 && d.dial_bytes == 2 * frame_bytes && d.dial_hits == 2 &&
          d.dial_misses == 2 && d.dial_evictions == 0 && d.dial_decode_ms == 300,
          "diag: n %u bytes %u hits %u misses %u evict %u ms %u", (unsigned)d.dial_cached,
          (unsigned)d.dial_bytes, (unsigned)d.dial_hits, (unsigned)d.dial_misses,
          (unsigned)d.dial_evictions, (unsigned)d.dial_decode_ms);
    dial_clips_drop();
    dial_cache_reset();
}

static unsigned s_dc_lock_calls, s_dc_lock_seen, s_dc_frames, s_dc_bad;
static void dc_lock_hook(void) { s_dc_lock_calls++; }
static void dc_decode_hook(uint16_t *px, size_t n)
{
    (void)px; (void)n;
    s_dc_lock_seen = s_dc_lock_calls;
    for (int f = 0; f < 5; ++f) {               /* five render frames mid-decode */
        s_dc_frames++;
        if (!s_dial_blit || s_dial_frame_face != JR_FACE_DIAL_DIVER ||
            dial_strip_mismatch(&s_display, 0x11) != 0) {
            s_dc_bad++;
        }
    }
}

/* The render path keeps drawing the old dial while the new one decodes, and the
 * render lock is taken once, to switch, after the decode. */
static void test_dial_cache_old_dial_draws_while_the_new_one_decodes(void)
{
    jr_display_ctx_t *ctx = &s_display;
    dream_fixture();
    dial_test_board();
    dial_cache_reset();
    dial_clips_drop();
    s_stub_eaf_salt = 0x11;
    dial_clip_seed(JR_FACE_DIAL_DIVER);
    CHECK(dial_show(ctx, JR_FACE_DIAL_DIVER), "the old dial is up");

    s_stub_eaf_salt = 0x22;
    dial_clip_seed(JR_FACE_DIAL_PILOT);
    s_dc_lock_calls = s_dc_lock_seen = s_dc_frames = s_dc_bad = 0;
    s_stub_lock_hook = dc_lock_hook;
    s_stub_eaf_decode_hook = dc_decode_hook;
    CHECK(dial_show(ctx, JR_FACE_DIAL_PILOT), "the new dial shows");
    s_stub_lock_hook = NULL;
    s_stub_eaf_decode_hook = NULL;
    CHECK(s_dc_frames == 5 && s_dc_bad == 0,
          "%u of %u frames drawn mid-decode were not the old dial", s_dc_bad, s_dc_frames);
    CHECK(s_dc_lock_seen == 0, "the render lock was taken %u times before the decode ended",
          s_dc_lock_seen);
    CHECK(s_dc_lock_calls == 1, "one lock to switch the frame, got %u", s_dc_lock_calls);
    CHECK(dial_strip_mismatch(ctx, 0x22) == 0, "then the new dial is on the glass");
    dial_clips_drop();
    dial_cache_reset();
}

static unsigned s_dw_calls, s_dw_mid_valid, s_dw_mid_filling, s_dw_mid_evicted;
static int s_dw_idx;
static void dw_decode_hook(uint16_t *px, size_t n)
{
    (void)px; (void)n;
    s_dw_calls++;
    s_dw_mid_valid = s_dial_slot[s_dw_idx].valid ? 1U : 0U;
    s_dw_mid_filling = s_dial_filling == s_dw_idx ? 1U : 0U;
    /* something under PSRAM pressure asks for a frame mid-decode: the glass
     * frame and the one being decoded into must both survive it */
    const size_t saved = s_stub_psram_budget;
    s_stub_psram_budget = 0;
    const unsigned before = s_dial_evictions;
    const uint16_t *p = dial_slot_alloc((size_t)HUD_W * HUD_H * sizeof(uint16_t), 4, true);
    s_dw_mid_evicted = (p != NULL || s_dial_evictions != before) ? 1U : 0U;
    s_stub_psram_budget = saved;
}

static void warm_fixture(void)
{
    jr_display_ctx_t *ctx = &s_display;
    dream_fixture();
    dial_test_board();
    dial_cache_reset();
    dial_clips_drop();
    s_stub_eaf_decodes = 0;
    s_stub_eaf_salt = 0x11;
    dial_clip_seed(JR_FACE_DIAL_DIVER);
    CHECK(dial_show(ctx, JR_FACE_DIAL_DIVER), "DIVER is on the glass");
    s_dial_glass_ms = 0;
    s_style_prog = 256;
    s_space_prog = 256;
    s_dip_mode = DIP_MODE_NONE;
    s_parked = false;
    ctx->dream_active = ctx->quiet_active = false;
    ctx->blanked = false;
    ctx->requested_word = (uint32_t)JR_FACE_IDLE << JR_DISPLAY_CMD_FACE_SHIFT;
    s_ota_word = JR_DISPLAY_OTA_IDLE;
    s_stub_eaf_salt = 0x44;                     /* what a warm decode of DRESS carries */
    dial_clip_seed(JR_FACE_DIAL_DRESS);
}

/* The neighbours are warmed only when nothing else wants the CPU, and a warm-up
 * that fails or is cut short never leaves a frame marked valid. */
static void test_dial_cache_warms_the_neighbour_only_when_idle(void)
{
    jr_display_ctx_t *ctx = &s_display;
    const int dress = dial_slot_of(JR_FACE_DIAL_DRESS);
    warm_fixture();

    s_dial_glass_ms = 1000U;                    /* on the glass 1000 ms at now=2000 */
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    CHECK(s_dial_warms == 0 && !s_dial_slot[dress].valid, "warm-up ran before the settle delay");
    s_dial_glass_ms = 0;
    dial_warm_tick(ctx, 1499U, JR_FACE_IDLE, true);
    CHECK(s_dial_warms == 0 && !s_dial_slot[dress].valid, "warm-up ran at 1499 ms");
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, false);
    CHECK(s_dial_warms == 0 && !s_dial_slot[dress].valid, "warm-up ran with a change pending");
    dial_warm_tick(ctx, 2000U, JR_FACE_LISTENING, true);
    dial_warm_tick(ctx, 2000U, JR_FACE_THINKING, true);
    dial_warm_tick(ctx, 2000U, JR_FACE_SPEAKING, true);
    dial_warm_tick(ctx, 2000U, JR_FACE_LINKING, true);
    CHECK(s_dial_warms == 0 && !s_dial_slot[dress].valid, "warm-up ran in a voice session");
    s_ota_word = JR_DISPLAY_OTA_RECEIVING;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    s_ota_word = JR_DISPLAY_OTA_PROBATION;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    s_ota_word = JR_DISPLAY_OTA_PREFLIGHT;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    s_ota_word = JR_DISPLAY_OTA_IDLE;
    CHECK(s_dial_warms == 0 && !s_dial_slot[dress].valid, "warm-up ran during an OTA");
    s_style_prog = 100;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    s_style_prog = 256;
    s_space_prog = 100;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    s_space_prog = 256;
    s_dip_mode = DIP_MODE_STYLE;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    s_dip_mode = DIP_MODE_NONE;
    CHECK(s_dial_warms == 0 && !s_dial_slot[dress].valid, "warm-up ran during a transition");
    ctx->dream_active = true;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    ctx->dream_active = false;
    ctx->quiet_active = true;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    ctx->quiet_active = false;
    ctx->blanked = true;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    ctx->blanked = false;
    ctx->requested_word |= JR_DISPLAY_CMD_BLANK;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    ctx->requested_word &= ~JR_DISPLAY_CMD_BLANK;
    s_parked = true;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    s_parked = false;
    CHECK(s_dial_warms == 0 && !s_dial_slot[dress].valid && s_stub_eaf_decodes == 1,
          "warm-up ran during a sleep or blank");

    /* cut short: the decode fails half way. Nothing is marked valid, nothing is
     * held, the glass is untouched, and the pressure probe cannot take the
     * glass frame or the one being filled. */
    s_dw_idx = dress;
    s_dw_calls = 0;
    s_stub_eaf_decode_hook = dw_decode_hook;
    s_stub_eaf_decode_error = ESP_FAIL;
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    CHECK(s_dw_calls == 1, "the warm-up decoded once, got %u", s_dw_calls);
    CHECK(s_dw_mid_filling == 1 && s_dw_mid_valid == 0, "mid-decode the frame was %s",
          s_dw_mid_valid ? "already valid" : "not marked as being filled");
    CHECK(s_dw_mid_evicted == 0, "a frame request mid-decode evicted the glass or the filling frame");
    CHECK(!s_dial_slot[dress].valid && s_dial_slot[dress].px == NULL && s_dial_filling == -1 &&
          s_dial_warms == 0, "an interrupted warm-up left a frame behind");
    CHECK((s_dial_warm_skip & (1U << dress)) != 0U, "a failed warm-up is remembered");
    CHECK(s_dial_blit && s_dial_frame_face == JR_FACE_DIAL_DIVER &&
          dial_strip_mismatch(ctx, 0x11) == 0, "the dial on the glass was not disturbed");
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    CHECK(s_dw_calls == 1, "a failed warm-up was retried at once");

    /* and a clean one: DRESS decoded off the glass, valid, clip released */
    s_stub_eaf_decode_hook = NULL;
    s_stub_eaf_decode_error = ESP_OK;
    s_dial_warm_skip = 0;
    dial_clip_seed(JR_FACE_DIAL_DRESS);
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    CHECK(s_dial_slot[dress].valid && s_dial_warms == 1 && s_stub_eaf_decodes == 2,
          "the neighbour was not warmed");
    CHECK(ctx->clips[JR_FACE_DIAL_DRESS].data == NULL, "the warmed dial's compressed clip is released");
    CHECK(s_dial_frame_face == JR_FACE_DIAL_DIVER && dial_strip_mismatch(ctx, 0x11) == 0,
          "the glass still shows DIVER");
    CHECK(s_dial_hits == 0 && s_dial_misses == 1, "a warm-up counts as neither hit nor miss");
    dial_warm_tick(ctx, 2000U, JR_FACE_IDLE, true);
    CHECK(s_stub_eaf_decodes == 2, "nothing is left to warm next to DIVER (JARVIS has no dial)");

    /* the swipe finds it: no decode, no clip, the new frame is DRESS's */
    s_stub_eaf_salt = 0x77;
    CHECK(dial_show(ctx, JR_FACE_DIAL_DRESS) && s_stub_eaf_decodes == 2 &&
          dial_strip_mismatch(ctx, 0x44) == 0 && s_dial_hits == 1 && s_dial_misses == 1,
          "the warmed dial is served from the cache");
    dial_clips_drop();
    dial_cache_reset();
}

/* Under the PSRAM floor: the dial used longest ago goes, never the one on the
 * glass; with nothing to evict the shared frame still shows the dial; with not
 * even that, the anim path keeps the face. */
static void test_dial_cache_floor_eviction_and_the_shared_fallback(void)
{
    jr_display_ctx_t *ctx = &s_display;
    const size_t f = (size_t)HUD_W * HUD_H * sizeof(uint16_t);
    dream_fixture();
    dial_test_board();
    dial_cache_reset();
    dial_clips_drop();
    s_stub_eaf_decodes = 0;
    const jr_face_t d[] = { JR_FACE_DIAL_DIVER, JR_FACE_DIAL_DRESS, JR_FACE_DIAL_PILOT,
                            JR_FACE_DIAL_FUTURE };
    uint16_t salt[] = { 0x01, 0x02, 0x03, 0x04 };

    /* room for exactly three frames above the 2 MB floor */
    s_stub_psram_budget = DIAL_PSRAM_FLOOR + 3 * f + 4096;
    for (int i = 0; i < 3; ++i) {
        s_stub_eaf_salt = salt[i];
        dial_clip_seed(d[i]);
        CHECK(dial_show(ctx, d[i]), "dial %d shows", i);
    }
    CHECK(s_dial_evictions == 0 && s_dial_held_n == 3, "three frames fit, evictions %u held %u",
          (unsigned)s_dial_evictions, (unsigned)s_dial_held_n);
    CHECK(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < f + DIAL_PSRAM_FLOOR,
          "a fourth frame would break the floor");
    CHECK(dial_show(ctx, d[1]), "DRESS again");                  /* DIVER is now the oldest */
    s_stub_eaf_salt = salt[3];
    dial_clip_seed(d[3]);
    CHECK(dial_show(ctx, d[3]), "a fourth dial shows under the floor");
    CHECK(s_dial_evictions == 1, "evictions %u", (unsigned)s_dial_evictions);
    CHECK(s_dial_slot[dial_slot_of(d[0])].px == NULL, "the dial used longest ago was evicted");
    CHECK(s_dial_slot[dial_slot_of(d[1])].valid && s_dial_slot[dial_slot_of(d[2])].valid &&
          s_dial_slot[dial_slot_of(d[3])].valid, "the others were kept");
    CHECK(dial_strip_mismatch(ctx, salt[3]) == 0 && s_dial_held_bytes == 3 * f,
          "the new dial is on the glass and the cache is still three frames");

    /* the dial on the glass is the least recently used by the clock: it still stays */
    s_dial_slot[dial_slot_of(d[3])].used = 0;
    dial_clip_seed(d[0]);
    s_stub_eaf_salt = 0x09;
    CHECK(dial_show(ctx, d[0]), "DIVER is wanted again");
    CHECK(s_dial_evictions == 2 && s_dial_slot[dial_slot_of(d[0])].valid, "DIVER decoded after an eviction");
    CHECK(s_dial_slot[dial_slot_of(d[3])].valid, "the old glass dial was not the victim");
    CHECK(dial_strip_mismatch(ctx, 0x09) == 0, "and the new dial is on the glass");

    /* a dial that is on the glass is never the victim, even when it is the oldest */
    {
        const int onglass = dial_slot_of(d[0]);
        s_dial_slot[onglass].used = 0;
        for (int i = 0; i < DIAL_SLOTS; ++i) {
            if (i != onglass && s_dial_slot[i].px != NULL) { s_dial_slot[i].used = 100U + (uint32_t)i; }
        }
        CHECK(dial_slot_alloc(f, dial_slot_of(JR_FACE_DIAL_SKELETON), true) == NULL ||
              s_dial_slot[onglass].px != NULL, "the shown dial's frame was evicted");
        CHECK(s_dial_slot[onglass].px == s_dial_frame, "the glass still reads its frame");
    }

    /* a warm-up under the floor does nothing: it never evicts an idle dial for itself */
    {
        const int dress = dial_slot_of(JR_FACE_DIAL_DRESS);
        dial_slot_free(&s_dial_slot[dress]);
        CHECK(s_dial_blit && s_dial_frame_face == JR_FACE_DIAL_DIVER, "DIVER is on the glass for the warm-up");
        int idle = 0;
        for (int i = 0; i < DIAL_SLOTS; ++i) {
            idle += (s_dial_slot[i].px != NULL && s_dial_slot[i].px != s_dial_frame) ? 1 : 0;
        }
        CHECK(idle > 0, "an idle frame is there to be tempted by");
        s_stub_psram_budget = s_stub_heap_bytes + DIAL_PSRAM_FLOOR + f / 2;
        s_style_prog = 256; s_space_prog = 256; s_dip_mode = DIP_MODE_NONE;
        ctx->requested_word = (uint32_t)JR_FACE_IDLE << JR_DISPLAY_CMD_FACE_SHIFT;
        const unsigned ev = s_dial_evictions;
        dial_warm_tick(ctx, 5000U, JR_FACE_IDLE, true);
        CHECK(s_dial_evictions == ev && s_dial_warms == 0 && s_dial_slot[dress].px == NULL,
              "a warm-up under the PSRAM floor evicted or allocated");
    }

    /* nothing to evict and under the floor: the shared frame shows the dial */
    dial_clips_drop();
    dial_cache_reset();
    s_stub_eaf_decodes = 0;
    s_stub_psram_budget = DIAL_PSRAM_FLOOR + f + 4096;
    s_stub_eaf_salt = 0x0a;
    dial_clip_seed(d[0]);
    CHECK(dial_show(ctx, d[0]) && s_dial_slot[dial_slot_of(d[0])].valid, "the first dial gets a frame");
    s_stub_eaf_salt = 0x0b;
    dial_clip_seed(d[1]);
    CHECK(dial_show(ctx, d[1]), "the second dial shows with no frame to be had");
    CHECK(s_dial_slot[dial_slot_of(d[1])].px == NULL && s_dial_shared != NULL &&
          s_dial_frame == s_dial_shared && s_dial_shared_face == d[1],
          "it was decoded into the shared frame");
    CHECK(dial_strip_mismatch(ctx, 0x0b) == 0, "and that is what the glass shows");
    CHECK(s_dial_evictions == 0, "the glass dial was not evicted for it");
    CHECK(dial_show(ctx, d[0]) && s_dial_frame == s_dial_slot[dial_slot_of(d[0])].px &&
          dial_strip_mismatch(ctx, 0x0a) == 0, "the first dial comes back from its own frame");
    CHECK(dial_show(ctx, d[1]) && s_stub_eaf_decodes == 2 && dial_strip_mismatch(ctx, 0x0b) == 0,
          "the shared frame is a cache hit while it still holds the dial");

    /* and with not even the shared frame: the face stays on the anim path */
    dial_clips_drop();
    dial_cache_reset();
    s_stub_eaf_decodes = 0;
    s_stub_psram_budget = DIAL_PSRAM_FLOOR;
    dial_clip_seed(d[0]);
    s_stub_malloc_fail = 1;
    CHECK(!dial_show(ctx, d[0]) && !s_dial_blit && s_stub_eaf_decodes == 0 &&
          ctx->clips[d[0]].data != NULL, "no frame at all leaves the dial on the anim path");
    s_stub_malloc_fail = 0;
    CHECK(dial_show(ctx, d[0]) && s_dial_frame == s_dial_shared && s_stub_eaf_decodes == 1,
          "and the next try uses the shared frame");
    dial_clips_drop();
    dial_cache_reset();
}

/* The frames go back when the glass has no dial any more (the dream), and the
 * release never takes the frame the blit is reading. */
static void test_dial_cache_is_released_for_sleep_and_kept_on_the_glass(void)
{
    jr_display_ctx_t *ctx = &s_display;
    dream_fixture();
    dial_test_board();
    dial_cache_reset();
    dial_clips_drop();
    const size_t baseline = s_stub_heap_bytes;
    const jr_face_t d[] = { JR_FACE_DIAL_DIVER, JR_FACE_DIAL_DRESS, JR_FACE_DIAL_PILOT };
    for (int i = 0; i < 3; ++i) {
        dial_clip_seed(d[i]);
        CHECK(dial_show(ctx, d[i]), "dial %d shows", i);
    }
    CHECK(s_dial_held_n == 3, "three frames held");
    dial_cache_release();
    CHECK(s_dial_held_n == 1 && s_dial_blit && s_dial_frame_face == JR_FACE_DIAL_PILOT &&
          s_dial_frame == s_dial_slot[dial_slot_of(d[2])].px && dial_strip_mismatch(ctx, 0) == 0,
          "a release with the blit on keeps the shown frame only (held %u)", (unsigned)s_dial_held_n);

    uint32_t last_word = ctx->requested_word;
    jr_display_clock_set(true, 17, 42, 1);
    CHECK(presenter_dream_tick(ctx, &last_word) && ctx->dream_active, "the DREAM enters");
    CHECK(s_dial_held_n == 0 && s_dial_held_bytes == 0 && s_dial_frame == NULL &&
          s_stub_heap_bytes <= baseline + 3U * 64U, "the dream gives every frame back (%u held)",
          (unsigned)s_dial_held_n);
    dial_clips_drop();
    dial_cache_reset();
}

static void test_dial_sources_release_without_invalidating_vendor_ownership(void)
{
    dream_fixture();
    jr_display_ctx_t *ctx = &s_display;
    ctx->active = ctx->bound = NULL;
    s_stub_anim_source = NULL;
    s_stub_bound_frees = 0U;
    dial_cache_reset();
    s_stub_eaf_head = (eaf_header_t){ 8, HUD_W, HUD_H };
    const size_t baseline = s_stub_heap_bytes;
    const size_t frame_bytes = (size_t)HUD_W * HUD_H * sizeof(uint16_t);
    const jr_face_t dials[] = { JR_FACE_DIAL_FUTURE, JR_FACE_DIAL_DIVER,
                               JR_FACE_DIAL_DRESS, JR_FACE_DIAL_PILOT };
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (size_t i = 0; i < sizeof dials / sizeof dials[0]; ++i) {
            jr_display_clip_t *clip = allocate_test_clip(dials[i], 64U + i);
            CHECK(dial_show(ctx, dials[i]), "style%d decodes", dials[i]);
            const size_t frames = cycle == 0 ? i + 1 : sizeof dials / sizeof dials[0];
            CHECK(!clip->data && s_stub_heap_bytes == baseline + frames * frame_bytes,
                  "style cycling keeps one decoded frame per dial without compressed growth");
            const int decodes = s_stub_eaf_decodes;
            CHECK(dial_show(ctx, dials[i]) && s_stub_eaf_decodes == decodes,
                  "same dial reuses decoded frame after source release, no load/decode");
        }
    }

    jr_display_clip_t *dial = allocate_test_clip(JR_FACE_DIAL_DIVER, 96U);
    dial_slot_t *diver = &s_dial_slot[dial_slot_of(JR_FACE_DIAL_DIVER)];
    dial_slot_free(diver);                     /* DIVER is a miss again */
    s_stub_lock_failures = 1;
    CHECK(!dial_show(ctx, JR_FACE_DIAL_DIVER) && dial->data && diver->valid &&
          s_dial_frame_face == JR_FACE_DIAL_PILOT,
          "failed switch lock keeps the old frame and the retry source, and the decode");
    dial_slot_free(diver);
    s_stub_eaf_decode_error = ESP_FAIL;
    CHECK(!dial_show(ctx, JR_FACE_DIAL_DIVER) && dial->data && !diver->valid &&
          diver->px == NULL && s_dial_frame_face == JR_FACE_DIAL_PILOT,
          "decode failure retains compressed fallback source and holds no frame");
    s_stub_eaf_decode_error = ESP_OK;
    ctx->requested_word = (uint32_t)JR_FACE_DIAL_DIVER << JR_DISPLAY_CMD_FACE_SHIFT;
    bool stale = false;
    CHECK(apply_face(ctx, JR_FACE_DIAL_DIVER, JR_FACE_DIAL_DIVER, 0U, &stale) == ESP_OK,
          "failed baked decode can fall back to vendor anim");
    CHECK(ctx->bound == dial && s_stub_anim_source == dial->data,
          "vendor source is tracked separately from visible anim");
    const void *bound_data = dial->data;
    CHECK(dial_decode(ctx, JR_FACE_DIAL_DIVER, dial) == ESP_OK,
          "fallback dial later decodes successfully");
    s_stub_lock_failures = 1;
    CHECK(!dial_show(ctx, JR_FACE_DIAL_DIVER) && ctx->active == dial &&
          ctx->bound == dial && dial->data == bound_data,
          "failed hide lock preserves bound source for retry");
    CHECK(dial_show(ctx, JR_FACE_DIAL_DIVER) && ctx->active == NULL &&
          ctx->bound == dial && dial->data == bound_data && s_stub_bound_frees == 0U,
          "successful later decode/hide keeps stopped vendor parser's source alive");

    jr_display_clip_t *voice = allocate_test_clip(JR_FACE_LISTENING, 128U);
    ctx->requested_word = (uint32_t)JR_FACE_LISTENING << JR_DISPLAY_CMD_FACE_SHIFT;
    s_stub_lock_failures = 1;
    CHECK(apply_face(ctx, JR_FACE_LISTENING, JR_FACE_LISTENING, 0U, &stale) != ESP_OK &&
          ctx->bound == dial && dial->data == bound_data && s_stub_anim_source == bound_data,
          "failed rebind lock retains hidden old parser and new cached source");
    s_stub_set_src_error = ESP_FAIL;
    CHECK(apply_face(ctx, JR_FACE_LISTENING, JR_FACE_LISTENING, 0U, &stale) != ESP_OK &&
          ctx->bound == dial && dial->data == bound_data && s_stub_anim_source == bound_data,
          "failed rebind preserves hidden old parser and retry source");
    s_stub_set_src_error = ESP_OK;
    CHECK(apply_face(ctx, JR_FACE_LISTENING, JR_FACE_LISTENING, 0U, &stale) == ESP_OK &&
          ctx->bound == voice && ctx->active == voice && !dial->data &&
          s_stub_anim_source == voice->data && s_stub_bound_frees == 0U,
          "successful rebind releases only old unreferenced baked source");
    CHECK(dial_show(ctx, JR_FACE_DIAL_DIVER) && voice->data && ctx->bound == voice,
          "showing decoded dial keeps fast voice source resident and vendor-bound");
    CHECK(s_stub_heap_bytes == baseline + 4U * frame_bytes + 128U,
          "retained heap consists of the four decoded dials and the voice source only");
    ctx->bound = ctx->active = NULL;
    s_stub_anim_source = NULL;
    heap_caps_free(voice->data);
    memset(voice, 0, sizeof *voice);
    dial_cache_reset();
    CHECK(s_stub_heap_bytes == baseline && s_stub_bound_frees == 0U,
          "tracked test allocations balance, never freeing retained vendor source");
    s_display.gfx = NULL;
    s_display.task = NULL;
}

/* A strip must keep a rim stamp whose first row lies just above it. Exercise
 * every battery value, both panel orders and centre shifts through the real
 * overlay, including the 100% repeated endpoint and absent-battery sentinel. */
/* distance of a pixel from another, summed over its channels (both in panel order) */
static unsigned rim_dist(uint16_t a, uint16_t b, bool swap)
{
    if (swap) { a = (uint16_t)((a >> 8) | (a << 8)); b = (uint16_t)((b >> 8) | (b << 8)); }
    const int dr = (int)((a >> 11) & 31) - (int)((b >> 11) & 31);
    const int dg = (int)((a >> 5) & 63) - (int)((b >> 5) & 63);
    const int db = (int)(a & 31) - (int)(b & 31);
    return (unsigned)((dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db));
}

static void test_battery_rim_is_strip_invariant_at_every_percent(void)
{
    const size_t n = (size_t)HUD_W * HUD_H;
    uint16_t *whole = malloc(n * sizeof *whole);
    uint16_t *strips = malloc(n * sizeof *strips);
    CHECK(whole && strips, "battery strip fixtures allocate");
    if (!whole || !strips) { free(whole); free(strips); return; }
    static const int offsets[] = {-128, 0, 127};
    for (int pct = 0; pct <= 102; ++pct) {
        const uint8_t batt = pct == 102 ? 255U : (uint8_t)pct;
        for (int swap = 0; swap < 2; ++swap) {
            for (int charge = 0; charge < 2; ++charge) {
                for (int shift = 0; shift < 3; ++shift) {
                    const hud_env_t env = { .face = HUD_FACE_IDLE,
                        .batt_pct = batt, .charging = charge != 0,
                        .ox = (int8_t)offsets[shift], .oy = (int8_t)offsets[2 - shift] };
                    const uint32_t ms = (uint32_t)pct * 37U * 12U;
                    for (size_t i = 0; i < n; ++i) whole[i] = strips[i] = POISON;
                    hud_overlay_frame(whole, 0, HUD_H, ms, swap != 0, &env);
                    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
                        const int rows = y + STRIP_ROWS > HUD_H ? HUD_H - y : STRIP_ROWS;
                        hud_overlay_frame(strips + (size_t)y * HUD_W, y, rows,
                                          ms, swap != 0, &env);
                    }
                    CHECK(memcmp(whole, strips, n * sizeof *whole) == 0,
                          "battery %u charge%d order%d offset%d remains strip invariant",
                          (unsigned)batt, charge, swap, offsets[shift]);
                    if (shift == 1 && (batt == 0U || batt > 100U)) {
                        size_t drawn = 0;
                        for (size_t i = 0; i < n; ++i) drawn += whole[i] != POISON;
                        /* one ring: an empty gauge is the dim ring all round with a lit stamp at
                         * 12 o'clock (brighter than 6 o'clock); an absent battery draws nothing */
                        if (batt == 0U) {
                            const uint16_t top = whole[(size_t)(232 - 218) * HUD_W + 233];
                            const uint16_t bot = whole[(size_t)(232 + 218) * HUD_W + 233];
                            const unsigned gt = rim_dist(top, POISON, swap != 0), gb = rim_dist(bot, POISON, swap != 0);
                            CHECK(drawn > 3000U && gt > gb,
                                  "zero battery: the dim ring all round, a lit stamp at 12 o'clock (%zu px, %u vs %u)", drawn, gt, gb);
                        } else {
                            CHECK(drawn == 0U, "absent battery draws nothing (%zu)", drawn);
                        }
                    }
                }
            }
        }
    }
    free(whole); free(strips);
}

static jmp_buf s_boot_init_done;
static bool s_boot_fail_after_early_transfer;

static void boot_init_reached_presenter_loop(void)
{
    longjmp(s_boot_init_done, 1);
}

/* Run the real display_task initialization, leaving its infinite presenter
 * loop at the first scheduler wait. This also proves the timed wrapper is
 * actually wired into production, rather than testing an unused helper. */
static void run_boot_init(void)
{
    s_stub_notify_take_hook = boot_init_reached_presenter_loop;
    if (setjmp(s_boot_init_done) == 0) {
        display_task(&s_display);
    }
    s_stub_notify_take_hook = NULL;
}

static void boot_timing_fixture(void)
{
    memset(&s_display, 0, sizeof s_display);
    s_display.test_pattern = JR_DISPLAY_TEST_WHITE;
    s_stub_board_display = (jarvis_board_display_t){
        .panel = (esp_lcd_panel_handle_t)1,
        .io = (esp_lcd_panel_io_handle_t)1,
        .width = HUD_W, .height = HUD_H,
    };
    s_stub_board_get_error = s_stub_spiffs_error = s_stub_draw_error = ESP_OK;
    s_stub_brightness_error = ESP_OK;
    s_stub_brightness_calls = 0U;
    s_stub_draw_hook = NULL;
    s_stub_board_init_us = 17000;
    s_stub_engine_ready = true;
    s_stub_disp_add_hook = NULL;
    s_boot_fail_after_early_transfer = false;
    s_stub_disp_user_data = NULL;
    s_stub_panel_callbacks = (esp_lcd_panel_io_callbacks_t){0};
    s_stub_panel_callback_ctx = NULL;
    s_stub_flush_ready_calls = 0;
    s_stub_lock_failures = 0;
    s_stub_lock_hook = NULL;
    s_panel_off = 0U;
    s_dial_blit = false;
    s_fake_us = 100123;
}

static jr_display_boot_timing_t boot_timing_read(void)
{
    jr_display_diag_t diag;
    CHECK(jr_display_get_diag(&diag) == ESP_OK, "boot timing uses public diagnostic readback");
    return diag.boot_timing;
}

static void test_boot_timing_waits_for_a_real_completion(void)
{
    const jr_display_ctx_t saved = s_display;
    boot_timing_fixture();
    jr_display_boot_timing_t timing = boot_timing_read();
    CHECK(!timing.init_started && !timing.init_finished &&
          !timing.first_transfer_valid && timing.first_transfer_done_boot_ms == 0U,
          "no boot timing exists before initialization or a callback");
    run_boot_init();
    timing = boot_timing_read();
    CHECK(s_display.init_state == JR_DISPLAY_INIT_READY && timing.init_started &&
          timing.init_finished && timing.init_start_boot_ms == 100U &&
          timing.init_end_boot_ms == 117U,
          "actual init task records timer-relative start/end, including elapsed board work");
    CHECK(!timing.first_transfer_valid, "successful init alone is not a completed transfer");

    uint16_t strip[HUD_W] = {0};
    s_stub_disp_user_data = &s_display;
    s_stub_draw_error = ESP_FAIL;
    panel_flush(s_display.disp, 0, 0, HUD_W, 1, strip);
    timing = boot_timing_read();
    CHECK(!timing.first_transfer_valid && s_display.flush_errors == 1U &&
          s_stub_flush_ready_calls == 1U && s_display.snapshot_frame_dropped == 1U,
          "failed submission does not invent DMA completion and still releases renderer");
    s_stub_draw_error = ESP_OK;
    panel_flush(s_display.disp, 0, 0, HUD_W, 1, strip);
    CHECK(!boot_timing_read().first_transfer_valid,
          "successful submission and mirror handling are not transfer completion");
    s_fake_us = 238765;
    CHECK(s_stub_panel_callbacks.on_color_trans_done && s_stub_panel_callback_ctx == &s_display,
          "real init registered the production callback with its context");
    (void)s_stub_panel_callbacks.on_color_trans_done(NULL, NULL, s_stub_panel_callback_ctx);
    timing = boot_timing_read();
    CHECK(timing.first_transfer_valid && timing.first_transfer_done_boot_ms == 238U &&
          s_display.flush_completions == 1U && s_stub_flush_ready_calls == 2U,
          "callback latches milliseconds and preserves completion/flush-ready semantics");
    s_fake_us = 999999;
    (void)s_stub_panel_callbacks.on_color_trans_done(NULL, NULL, s_stub_panel_callback_ctx);
    CHECK(boot_timing_read().first_transfer_done_boot_ms == 238U &&
          s_display.flush_completions == 2U && s_stub_flush_ready_calls == 3U,
          "later callbacks release strips but never replace the first timestamp");
    s_display = saved;
    s_stub_disp_user_data = NULL;
    s_stub_engine_ready = false;
}

static void boot_init_immediate_transfer(gfx_disp_t *disp, const gfx_disp_config_t *cfg)
{
    s_stub_disp_user_data = cfg->user_data;
    uint16_t strip[HUD_W] = {0};
    cfg->flush_cb(disp, 0, 0, HUD_W, 1, strip);
    CHECK(s_display.disp == disp && !boot_timing_read().first_transfer_valid,
          "first-flush race publishes display handle without inventing completion");
    s_fake_us += 5333;
    (void)s_stub_panel_callbacks.on_color_trans_done(NULL, NULL, s_stub_panel_callback_ctx);
    jr_display_boot_timing_t timing = boot_timing_read();
    CHECK(timing.init_started && !timing.init_finished && timing.first_transfer_valid &&
          timing.first_transfer_done_boot_ms == 122U,
          "actual early callback is valid before init finishes");
    s_fake_us += 9000;
    if (s_boot_fail_after_early_transfer) {
        s_stub_engine_ready = false;
    }
}

static void test_boot_timing_records_init_failure_and_early_callback(void)
{
    const jr_display_ctx_t saved = s_display;
    for (int fail_at = 0; fail_at < 3; ++fail_at) {
        boot_timing_fixture();
        if (fail_at == 0) s_stub_board_get_error = ESP_FAIL;
        if (fail_at == 1) s_stub_spiffs_error = ESP_FAIL;
        if (fail_at == 2) s_stub_engine_ready = false;
        run_boot_init();
        jr_display_boot_timing_t timing = boot_timing_read();
        CHECK(s_display.init_state == JR_DISPLAY_INIT_FAILED && s_display.last_error == ESP_FAIL &&
              timing.init_started && timing.init_finished && timing.init_start_boot_ms == 100U &&
              timing.init_end_boot_ms == 117U && !timing.first_transfer_valid,
              "init failure %d still ends timing without fabricating a transfer", fail_at);
    }
    boot_timing_fixture();
    s_stub_disp_add_hook = boot_init_immediate_transfer;
    run_boot_init();
    jr_display_boot_timing_t timing = boot_timing_read();
    CHECK(s_display.init_state == JR_DISPLAY_INIT_READY && timing.init_finished &&
          timing.init_end_boot_ms == 131U && timing.first_transfer_valid &&
          timing.first_transfer_done_boot_ms < timing.init_end_boot_ms,
          "callback-before-init-end remains coherent after successful init");
    boot_timing_fixture();
    s_stub_disp_add_hook = boot_init_immediate_transfer;
    s_boot_fail_after_early_transfer = true;
    run_boot_init();
    timing = boot_timing_read();
    CHECK(s_display.init_state == JR_DISPLAY_INIT_FAILED &&
          s_display.last_error == ESP_ERR_NO_MEM && timing.init_finished &&
          timing.init_end_boot_ms == 131U && timing.first_transfer_valid &&
          timing.first_transfer_done_boot_ms == 122U,
          "later init failure cannot erase a genuinely completed early transfer");
    s_stub_disp_add_hook = NULL;
    s_stub_disp_user_data = NULL;
    s_stub_engine_ready = false;
    s_display = saved;
}

static void test_boot_timing_ignores_unusable_callbacks_and_unpublished_stamp(void)
{
    const jr_display_ctx_t saved = s_display;
    boot_timing_fixture();
    (void)panel_color_done(NULL, NULL, NULL);
    (void)panel_color_done(NULL, NULL, &s_display);
    CHECK(!boot_timing_read().first_transfer_valid && s_display.flush_completions == 0U &&
          s_stub_flush_ready_calls == 0U, "missing context/display cannot publish a timing marker");
    s_display.first_transfer_done_boot_ms = 42U;
    s_display.first_transfer_state = 2U;
    CHECK(!boot_timing_read().first_transfer_valid &&
          boot_timing_read().first_transfer_done_boot_ms == 0U,
          "in-progress publication is unavailable through the public API");
    s_display.first_transfer_state = 0U;
    s_display.disp = (gfx_disp_t *)2;
    s_fake_us = 999;
    (void)panel_color_done(NULL, NULL, &s_display);
    jr_display_boot_timing_t timing = boot_timing_read();
    CHECK(timing.first_transfer_valid && timing.first_transfer_done_boot_ms == 0U,
          "a genuine sub-millisecond callback remains valid; zero is not a validity sentinel");
    s_fake_us = 1000000;
    (void)panel_color_done(NULL, NULL, &s_display);
    CHECK(boot_timing_read().first_transfer_valid &&
          boot_timing_read().first_transfer_done_boot_ms == 0U,
          "zero first timestamp is still latched exactly once");
    s_display = saved;
    s_stub_engine_ready = false;
}

static void complete_test_transfer(void)
{
    (void)panel_color_done(NULL, NULL, &s_display);
}

static void flush_test_strip(int y);

static void cancel_at_off_ack_cas(volatile void *address, uintptr_t desired)
{
    if (address == (volatile void *)&s_panel_off && (desired & 3U) == 2U) {
        s_before_cas = NULL;
        jr_display_panel_on_request();
    }
}

static void test_panel_opposite_request_at_ack_boundary(void)
{
    dream_fixture();
    jr_display_panel_off_request();
    s_before_cas = cancel_at_off_ack_cas;
    flush_test_strip(0);
    s_before_cas = NULL;
    CHECK(!jr_display_panel_is_off() && (s_panel_off & 3U) == 3U,
          "opposite request between final settling check and CAS cannot be overwritten by old off ack");
    flush_test_strip(0); complete_test_transfer();
    CHECK(jr_display_panel_is_on(), "latest recovery still completes after off-ack boundary race");
}

static unsigned s_boot_cap_at_first_draw;
static void boot_cap_draw_hook(void)
{
    s_boot_cap_at_first_draw = s_stub_panel_brightness;
}

static void test_boot_brightness_seed_precedes_first_transfer(void)
{
    const jr_display_ctx_t saved = s_display;
    const unsigned caps[] = {0U, 8U, 65U, 100U, 255U};
    for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; ++i) {
        boot_timing_fixture(); s_stub_panel_brightness = 0U;
        s_boot_cap_at_first_draw = 999U;
        jr_display_set_brightness((uint8_t)caps[i]);
        CHECK(s_stub_brightness_calls == 0U, "pre-start brightness publisher performs no panel IO cap%d", caps[i]);
        s_stub_draw_hook = boot_cap_draw_hook;
        s_stub_disp_add_hook = boot_init_immediate_transfer;
        run_boot_init();
        const unsigned expected = caps[i] > 100U ? 100U : caps[i];
        CHECK(s_boot_cap_at_first_draw == expected && s_stub_panel_brightness == expected,
              "first actual early gfx draw sees seed cap%d", caps[i]);
        CHECK(atomic_load(&s_brightness_shown) == (int)expected &&
              atomic_load(&s_brightness_have) == expected, "shown/have begin at cap%d", caps[i]);
        CHECK(jr_display_is_ready(), "seed cap%d permits normal display init", caps[i]);
    }
    boot_timing_fixture(); jr_display_set_brightness(65U);
    s_stub_brightness_error = ESP_FAIL; s_stub_draw_calls = 0U;
    s_stub_disp_add_hook = boot_init_immediate_transfer;
    run_boot_init();
    CHECK(!jr_display_is_ready() && s_display.init_state == JR_DISPLAY_INIT_FAILED &&
          s_display.last_error == ESP_FAIL, "failed initial seed truthfully refuses init");
    CHECK(s_stub_draw_calls == 0U && !boot_timing_read().first_transfer_valid,
          "failed seed cannot submit content or invent completion");
    s_stub_draw_hook = NULL; s_stub_disp_add_hook = NULL;
    s_stub_brightness_error = ESP_OK; s_stub_engine_ready = false;
    s_display = saved;
}

static void flush_test_strip(int y)
{
    uint16_t strip[HUD_W] = {0};
    s_stub_disp_user_data = &s_display;
    panel_flush(s_display.disp, 0, y, HUD_W, y + 1, strip);
}

static void test_rest_mailboxes_survive_start_and_env_updates(void)
{
    dream_fixture();
    s_display.started = 0U;
    jr_display_rest_set(JR_DISPLAY_REST_NIGHTSTAND, true, true);
    jr_display_power_set(61, 3890, true, false);
    jr_display_clock_set(true, 23, 54, 19);
    const uint32_t rest = s_hud_env_word & JR_DISPLAY_ENV_REST_MASK;
    jr_display_set_hud_env(61, false, true, 8, 9);
    CHECK((s_hud_env_word & JR_DISPLAY_ENV_REST_MASK) == rest, "HUD writer preserves every Rest request bit");
    const uint32_t env = s_hud_env_word, clock = s_clock_word, power = s_power_word;
    jr_display_t port;
    CHECK(jr_display_start(&port) == ESP_OK, "seeded mailbox start succeeds");
    CHECK(s_hud_env_word == env && s_clock_word == clock && s_power_word == power,
          "start keeps time/power/privacy/rest seeded before Wi-Fi");
    CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NIGHTSTAND, "seeded Nightstand survives start");
    s_stub_notifies = 0U;
    jr_display_clock_set(true, 23, 54, 20);
    jr_display_set_hud_env(61, false, true, -7, -3);
    CHECK(s_stub_notifies == 0U, "Nightstand seconds and tilt do not wake static presenter");
    jr_display_clock_set(true, 23, 55, 0);
    CHECK(s_stub_notifies == 1U, "minute wakes static presenter");
    jr_display_dream_set(false);
    CHECK(!(s_hud_env_word & JR_DISPLAY_ENV_REST_MASK), "explicit foreground exit clears all companion flags");
    jr_display_rest_set((jr_display_rest_mode_t)255, true, true);
    CHECK(!(s_hud_env_word & JR_DISPLAY_ENV_REST_MASK), "invalid Rest mode cleared");
    jr_display_rest_set(JR_DISPLAY_REST_GLANCE, false, true);
    CHECK(!(s_hud_env_word & JR_DISPLAY_ENV_NIGHT), "Glance cannot inherit Nightstand night flag");
    jr_display_rest_set(JR_DISPLAY_REST_GLANCE, true, true);
    CHECK(!(s_hud_env_word & JR_DISPLAY_ENV_NIGHT), "USB Glance remains distinct from Nightstand night flag");
    s_display.started = 0U;
}

static void test_rest_independently_refuses_exiting_foreground(void)
{
    for (int mode = JR_DISPLAY_REST_DREAM; mode <= JR_DISPLAY_REST_GLANCE; ++mode) {
        dream_fixture();
        s_power_word = 74U | (mode == JR_DISPLAY_REST_NIGHTSTAND ? 1u << 24 : 0U);
        jr_display_rest_set((jr_display_rest_mode_t)mode, mode == JR_DISPLAY_REST_NIGHTSTAND, true);
        CHECK(rest_eligible(&s_display) == (jr_display_rest_mode_t)mode, "mode%d eligible at rest", mode);
        int *eases[] = {&s_detail_ease, &s_shade_ease, &s_ask_ease, &s_canvas_ease};
        for (unsigned i = 0; i < sizeof eases / sizeof eases[0]; ++i) {
            *eases[i] = 1;
            CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "mode%d refuses exiting foreground%d", mode, i);
            *eases[i] = 0;
        }
        s_space_from = JR_DISPLAY_SPACE_JARVIS; s_space_to = JR_DISPLAY_SPACE_WATCH; s_space_ease = 255;
        CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "mode%d refuses unfinished navigation", mode);
        s_space_ease = 256;
        CHECK(rest_eligible(&s_display) == (jr_display_rest_mode_t)mode, "mode%d admits completed navigation", mode);
        s_display.requested_word = (uint32_t)JR_FACE_SPEAKING << JR_DISPLAY_CMD_FACE_SHIFT;
        CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "mode%d refuses voice foreground", mode);
    }
    dream_fixture();
    jr_display_rest_set(JR_DISPLAY_REST_NIGHTSTAND, true, true);
    CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "current power USB required, stale main request insufficient");
    s_power_word |= 1u << 24;
    jr_display_rest_set(JR_DISPLAY_REST_NIGHTSTAND, false, true);
    CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "request must also observe USB");
}

static void test_rest_idle_listening_yields_immediately_to_voice_busy(void)
{
    for (int mode = JR_DISPLAY_REST_NIGHTSTAND; mode <= JR_DISPLAY_REST_GLANCE; ++mode) {
        dream_fixture();
        s_power_word |= 1u << 24;
        s_display.requested_word = ((uint32_t)JR_FACE_LISTENING << JR_DISPLAY_CMD_FACE_SHIFT) | 123U;
        jr_display_caption_set("LISTENING");
        jr_display_rest_set((jr_display_rest_mode_t)mode, true, false);
        CHECK(rest_eligible(&s_display) == (jr_display_rest_mode_t)mode,
              "idle listening mode%d ignores ambient mic amplitude", mode);
        jr_display_rest_voice_busy_set(true);
        jr_display_set_hud_env(62, false, false, 12, -4);
        CHECK((s_hud_env_word & JR_DISPLAY_ENV_VOICE_BUSY) &&
              rest_eligible(&s_display) == JR_DISPLAY_REST_NONE,
              "VAD edge busy preserved across HUD publication and refuses stale Rest mode%d", mode);
        jr_display_rest_set((jr_display_rest_mode_t)mode, true, true);
        CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "Rest request cannot clear independently published busy");
        jr_display_rest_set(JR_DISPLAY_REST_NONE, false, false);
        CHECK(s_hud_env_word & JR_DISPLAY_ENV_VOICE_BUSY, "foreground request cancellation keeps busy edge");
        jr_display_rest_voice_busy_set(false);
        jr_display_rest_set((jr_display_rest_mode_t)mode, true, false);
        jr_display_caption_set("RESTING - TAP TO WAKE");
        CHECK(rest_eligible(&s_display) == (jr_display_rest_mode_t)mode, "fixed resting hint yields only with idle voice");
        jr_display_caption_set("LIVE TRANSCRIPT");
        CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "live transcript independently refuses idle-listening Rest");
        jr_display_caption_set("LISTENING ERROR");
        CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "partial idle-hint match cannot suppress error");
        jr_display_caption_set("LISTENING");
        const jr_face_t busy_faces[] = {JR_FACE_THINKING, JR_FACE_SPEAKING, JR_FACE_ERROR};
        for (unsigned i = 0; i < sizeof busy_faces / sizeof busy_faces[0]; ++i) {
            s_display.requested_word = (uint32_t)busy_faces[i] << JR_DISPLAY_CMD_FACE_SHIFT;
            CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "busy phase face%d independently refuses Rest", busy_faces[i]);
        }
        s_display.requested_word = (uint32_t)JR_FACE_LISTENING << JR_DISPLAY_CMD_FACE_SHIFT;
        s_choice_n = 2;
        CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "asking phase independently refuses via choices");
    }
    dream_fixture();
    s_display.requested_word = (uint32_t)JR_FACE_LISTENING << JR_DISPLAY_CMD_FACE_SHIFT;
    CHECK(!dream_eligible(&s_display), "DREAM never admits idle listening");
    s_display.requested_word = (uint32_t)JR_FACE_MUTED << JR_DISPLAY_CMD_FACE_SHIFT;
    jr_display_rest_voice_busy_set(true);
    CHECK(!dream_eligible(&s_display), "busy independently refuses DREAM too");
    s_display.started = 0;
    const uint32_t env = s_hud_env_word;
    jr_display_t port; (void)jr_display_start(&port);
    CHECK(s_hud_env_word == env && (s_hud_env_word & JR_DISPLAY_ENV_VOICE_BUSY), "start preserves pre-seeded voice guard");
    s_display.started = 0;
}

static void test_power_hints_yield_only_after_plug_feedback(void)
{
    for (unsigned percent = 0; percent <= 100; ++percent) {
        char hint[32]; snprintf(hint, sizeof hint, "CHARGING %u%%", percent);
        CHECK(caption_is_ambient(hint), "canonical charging%d yields", percent);
        jr_display_caption_unpin(); jr_display_caption_set(hint);
        CHECK(!jr_display_caption_foreground(), "canonical charge status is not foreground%d", percent);
    }
    const char *bad[] = {"CHARGING", "CHARGING %", "CHARGING -1%", "CHARGING 101%", "CHARGING 999%",
        "CHARGING 00%", "CHARGING 01%", "CHARGING 1000%", "CHARGING 1.0%", "CHARGING 50% ERROR",
        "CHARGING 50% ", "CHARGING +1%", "CHARGING 50", "POWER CONNECTED ERROR", "ON BATTERY ERROR"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK(!caption_is_ambient(bad[i]), "noncanonical/live/error hint remains foreground: %s", bad[i]);
        jr_display_caption_set(bad[i]);
        CHECK(jr_display_caption_foreground(), "noncanonical charge caption reports foreground%d", i);
    }
    const char *hints[] = {"POWER CONNECTED", "CHARGE COMPLETE", "ON BATTERY", "CHARGING 62%"};
    for (unsigned i = 0; i < sizeof hints / sizeof hints[0]; ++i) {
        dream_fixture(); s_power_word |= 1u << 24;
        jr_display_caption_set(hints[i]);
        CHECK(!jr_display_caption_foreground(), "fixed power hint%d is ambient", i);
        jr_display_bloom(); /* real power feedback precedes the idle publisher */
        jr_display_rest_set(JR_DISPLAY_REST_NIGHTSTAND, true, false);
        CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "power hint preserves plug bloom%d", i);
        s_fake_us += (int64_t)HUD_BLOOM_MS * 1000;
        CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NIGHTSTAND, "settled power status permits Nightstand%d", i);
        jr_display_caption_set("POWER ERROR");
        CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_NONE, "power error remains foreground%d", i);
        CHECK(jr_display_caption_foreground(), "power error%d reports foreground for main", i);
    }
    jr_display_caption_clear();
    CHECK(!jr_display_caption_foreground(), "clear caption releases foreground immediately despite retained fade text");
    jr_display_caption_pin("LISTENING");
    CHECK(jr_display_caption_foreground(), "pinned ambient text still owns foreground");
    jr_display_caption_unpin();
    CHECK(!jr_display_caption_foreground(), "unpin fixed idle hint releases foreground");
}

static void test_companion_sparse_refresh_and_retry(void)
{
    dream_fixture();
    s_power_word |= 1u << 24;
    jr_display_rest_set(JR_DISPLAY_REST_NIGHTSTAND, true, false);
    jr_display_clock_set(true, 23, 59, 59);
    uint32_t last = s_display.requested_word;
    CHECK(presenter_dream_tick(&s_display, &last), "Nightstand stops emote");
    CHECK(s_display.rest_view.mode == JR_COMPANION_NIGHTSTAND && !s_display.rest_view.night, "stores visible Nightstand view");
    unsigned before = s_stub_refreshes;
    jr_display_clock_set(true, 23, 59, 0);
    jr_display_set_hud_env(74, false, true, 20, 20);
    presenter_dream_tick(&s_display, &last);
    CHECK(s_stub_refreshes == before, "same minute/view does not redraw");
    jr_display_rest_set(JR_DISPLAY_REST_NIGHTSTAND, true, true);
    s_stub_lock_failures = 1;
    presenter_dream_tick(&s_display, &last);
    CHECK(!s_display.rest_view.night && s_stub_refreshes == before, "failed refresh keeps previous night key");
    presenter_dream_tick(&s_display, &last);
    CHECK(s_display.rest_view.night && s_stub_refreshes == before + 1, "night change retries after lock refusal");
    jr_display_set_hud_env(73, true, false, 0, 0);
    presenter_dream_tick(&s_display, &last);
    CHECK(s_stub_refreshes == before + 2 && s_display.rest_view.battery_percent == 73 &&
          s_display.rest_view.charging && !s_display.rest_view.privacy_muted, "battery/privacy/charge changes invalidate");
    jr_display_rest_set(JR_DISPLAY_REST_GLANCE, false, false);
    presenter_dream_tick(&s_display, &last);
    CHECK(s_stub_refreshes == before + 3 && strcmp(s_display.rest_view.lines[1].text, "NEXT UNAVAILABLE") == 0,
          "mode change redraws honest Glance item");
    s_power_word &= ~(1u << 24);
    jr_display_set_hud_env(73, false, false, 0, 0);
    presenter_dream_tick(&s_display, &last);
    CHECK(s_stub_refreshes == before + 4, "Glance USB text change invalidates");
    jr_display_clock_set(false, 0, 0, 0);
    presenter_dream_tick(&s_display, &last);
    CHECK(strcmp(s_display.rest_view.lines[0].text, "--:--") == 0, "invalid local time stays unknown");
    jr_display_rest_set(JR_DISPLAY_REST_NONE, false, false);
    CHECK(!presenter_dream_tick(&s_display, &last) && !s_display.dream_active && last == UINT32_MAX,
          "request exit forces same-face normal rebind");
}

/* ---- A DIAL OWNS THE GLASS (2026-11-10) ------------------------------------------------ */

/* The rim's ink: every pixel at r >= 213 that the HUD layer paints differently from the
 * same screen with no battery gauge, no charging and no mute (the env word 255). A dial
 * that paints its own pixels out there (MINIMAL's disc) is the same in both. */
static size_t rim_ink_away_count;      /* rim ink more than 8 px from the 12 o'clock dot (set by rim_ink_count) */

static size_t rim_ink_count(uint16_t *fb, uint16_t *levels, size_t nlevels, size_t *distinct)
{
    static uint16_t ref[HUD_W * HUD_H];
    const size_t px = (size_t)HUD_W * HUD_H;
    const uint32_t word = __atomic_load_n(&s_hud_env_word, __ATOMIC_ACQUIRE);
    memset(fb, 0, px * sizeof *fb);
    apply_hud_overlay(&s_display, 0, 0, HUD_W, HUD_H, fb);
    /* the reference is "nothing to show": no gauge, and on WATCH muted so that no dot is drawn */
    const bool on_watch = (__atomic_load_n(&s_nav_word, __ATOMIC_ACQUIRE) & NAV_SPACE_MASK) ==
                          (uint32_t)JR_DISPLAY_SPACE_WATCH;
    __atomic_store_n(&s_hud_env_word, 255U, __ATOMIC_RELEASE);
    memset(ref, 0, sizeof ref);
    apply_hud_overlay(&s_display, 0, 0, HUD_W, HUD_H, ref);
    bool ref_ink = false;
    for (int y = 0; y < HUD_H && on_watch && !ref_ink; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const int dx = x - 232, dy = y - 232;
            if (dx * dx + dy * dy >= 213 * 213 && x >= 233 - 8 && x <= 233 + 8 && y <= 15 + 8 &&
                ref[(size_t)y * HUD_W + x] != 0) { ref_ink = true; break; }
        }
    }
    if (ref_ink) {                       /* a quiet WATCH with the microphone live shows its dot: mute it */
        __atomic_store_n(&s_hud_env_word, 255U | (1U << 9), __ATOMIC_RELEASE);
        memset(ref, 0, sizeof ref);
        apply_hud_overlay(&s_display, 0, 0, HUD_W, HUD_H, ref);
    }
    __atomic_store_n(&s_hud_env_word, word, __ATOMIC_RELEASE);
    size_t n = 0, d = 0;
    rim_ink_away_count = 0;
    for (int y = 0; y < HUD_H; ++y) {
        for (int x = 0; x < HUD_W; ++x) {
            const int dx = x - 232, dy = y - 232;
            const uint16_t v = fb[(size_t)y * HUD_W + x];
            if (dx * dx + dy * dy < 213 * 213 || v == ref[(size_t)y * HUD_W + x]) continue;
            n++;
            if (x < 233 - 8 || x > 233 + 8 || y > 15 + 8) rim_ink_away_count++;
            if (levels != NULL) {
                size_t k = 0;
                while (k < d && levels[k] != v) k++;
                if (k == d && d < nlevels) levels[d++] = v;
            }
        }
    }
    if (distinct != NULL) *distinct = d;
    return n;
}

/* The staged screen: `space` at rest, microphone muted or live, charging, 74 %. */
static void stage_rim(int space, bool muted)
{
    stage_ring_at_rest(space, JR_DISPLAY_OTA_IDLE, 0);
    __atomic_store_n(&s_hud_env_word, 74U | (1U << 8) | (muted ? (1U << 9) : 0U), __ATOMIC_RELEASE);
    s_hud_enabled = 1;
    s_display.applied_face = JR_FACE_IDLE;       /* a dial face maps to IDLE in hud_face_of */
    s_display.requested_word = (uint32_t)JR_FACE_IDLE << JR_DISPLAY_CMD_FACE_SHIFT;
    s_style_shown = (uint8_t)JR_WATCH_JARVIS;
}

static void test_watch_dial_owns_the_glass_no_idle_rim(void)
{
    uint16_t *fb = malloc((size_t)HUD_W * HUD_H * sizeof *fb);
    if (!fb) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; return; }

    /* muted at rest on WATCH: nothing at all on any of the eight styles */
    for (int st = 0; st < (int)JR_WATCH_STYLE_COUNT; ++st) {
        stage_rim(JR_DISPLAY_SPACE_WATCH, true);
        s_style_shown = (uint8_t)st;
        const size_t n = rim_ink_count(fb, NULL, 0, NULL);
        CHECK(n == 0, "WATCH style %s muted at rest draws no rim (%zu px)",
              jr_display_watch_style_name((jr_watch_style_t)st), n);
    }
    /* microphone live and idle on a dial: ONE dot at 12 o'clock, about 3 px radius, and nothing else */
    for (int st = 0; st < (int)JR_WATCH_STYLE_COUNT; ++st) {
        stage_rim(JR_DISPLAY_SPACE_WATCH, false);
        s_style_shown = (uint8_t)st;
        const size_t n = rim_ink_count(fb, NULL, 0, NULL);
        CHECK(n >= 20 && n <= 130, "WATCH style %s live and idle: a privacy dot (%zu px)",
              jr_display_watch_style_name((jr_watch_style_t)st), n);
        CHECK(rim_ink_away_count == 0, "WATCH style %s live and idle: nothing but the dot (%zu px away)",
              jr_display_watch_style_name((jr_watch_style_t)st), rim_ink_away_count);
    }
    /* every other screen keeps its ring, muted (gold) and live (cyan, the gauge and the comet) */
    static const int others[] = { JR_DISPLAY_SPACE_JARVIS, JR_DISPLAY_SPACE_STATUS, JR_DISPLAY_SPACE_WEATHER };
    for (size_t i = 0; i < sizeof others / sizeof *others; ++i) {
        for (int muted = 0; muted < 2; ++muted) {
            stage_rim(others[i], muted != 0);
            const size_t n = rim_ink_count(fb, NULL, 0, NULL);
            CHECK(n > 3000, "space %d keeps its ring (muted %d): %zu px", others[i], muted, n);
        }
    }

    /* a session, an error, or anything else in the foreground brings the whole ring back at once,
     * muted or live */
    static const char *const why[] = {
        "an agent is working", "a question is up", "a commit ring is up", "a firmware update is running",
        "an operator lease caption is up", "the voice is capturing", "the voice is thinking",
        "the voice is speaking", "the voice failed", "the voice is busy",
    };
    for (int muted = 0; muted < 2; ++muted) {
        for (size_t k = 0; k < sizeof why / sizeof why[0]; ++k) {
            stage_rim(JR_DISPLAY_SPACE_WATCH, muted != 0);
            switch (k) {
            case 0: s_display.shell_word = JR_DISPLAY_SHELL_AGENT; break;
            case 1: s_choice_n = 1; break;
            case 2: s_commit_pct = 50U; break;
            case 3: jr_display_ota_set(JR_DISPLAY_OTA_RECEIVING, 42U, 0U, 1U, true); break;
            case 4: s_caption_on = 1; s_caption_lease = true; break;
            case 5: s_display.requested_word = (uint32_t)JR_FACE_LISTENING << JR_DISPLAY_CMD_FACE_SHIFT; break;
            case 6: s_display.requested_word = (uint32_t)JR_FACE_THINKING << JR_DISPLAY_CMD_FACE_SHIFT; break;
            case 7: s_display.requested_word = (uint32_t)JR_FACE_SPEAKING << JR_DISPLAY_CMD_FACE_SHIFT; break;
            case 8: s_display.requested_word = (uint32_t)JR_FACE_ERROR << JR_DISPLAY_CMD_FACE_SHIFT; break;
            default: __atomic_fetch_or(&s_hud_env_word, JR_DISPLAY_ENV_VOICE_BUSY, __ATOMIC_RELEASE); break;
            }
            const size_t n = rim_ink_count(fb, NULL, 0, NULL);
            /* (the commit and error rings draw over the same radius, so the difference can be smaller than a whole ring; the dot is at most 130) */
            CHECK(n > 500, "WATCH (muted %d), but %s: the whole ring is back (%zu px)", muted, why[k], n);
            s_display.shell_word = 0U;
            s_choice_n = 0;
            s_commit_pct = 0U;
            s_caption_on = 0;
            s_caption_lease = false;
            jr_display_ota_set(JR_DISPLAY_OTA_IDLE, 0U, 0U, 1U, true);
        }
    }

    /* the ring is anti-aliased: along the battery arc's edge the pixels run through many levels */
    stage_rim(JR_DISPLAY_SPACE_JARVIS, false);
    uint16_t levels[64];
    size_t distinct = 0;
    (void)rim_ink_count(fb, levels, 64, &distinct);
    CHECK(distinct > 8, "the rim runs through %zu distinct levels (want more than 8)", distinct);

    /* ONE RING, ONE RADIUS: nothing the shell draws comes within 6 px of it. The ring sits at r218;
     * render the HUD layer alone and the HUD layer plus the shell on the home screen and require the
     * shell to leave every pixel r215..221 as it was. */
    {
        stage_rim(JR_DISPLAY_SPACE_JARVIS, false);
        static uint16_t a[HUD_W * HUD_H], b[HUD_W * HUD_H];
        memset(a, 0, sizeof a);
        apply_hud_overlay(&s_display, 0, 0, HUD_W, HUD_H, a);
        memcpy(b, a, sizeof b);
        apply_shell_overlay(&s_display, 0, 0, HUD_W, HUD_H, b);
        size_t touched = 0;
        for (int y = 0; y < HUD_H; ++y) for (int x = 0; x < HUD_W; ++x) {
            const int dx2 = 2 * x - 465, dy2 = 2 * y - 465, r2 = dx2 * dx2 + dy2 * dy2;
            if (r2 >= 430 * 430 && r2 <= 442 * 442 && a[(size_t)y * HUD_W + x] != b[(size_t)y * HUD_W + x]) touched++;
        }
        CHECK(touched == 0, "the shell drew %zu px within 6 px of the rim radius", touched);
    }

    stage_rim(JR_DISPLAY_SPACE_JARVIS, false);
    reset_nav();
    free(fb);
}

/* The watch clears a disc to black under its dial, then draws: ink is anything that is neither
 * that black nor the stand-in art. */
static bool dial_ink(uint16_t v) { return v != 0x0000 && v != 0x4208; }

/* JARVIS gets a face: ticks, beads and rings are drawn on the settled watch, awake (the
 * microphone live, the rim up) or muted, with or without a sun. */
static void test_watch_jarvis_style_has_a_dial(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; return; }
    for (int sun = 0; sun < 2; ++sun) {
        for (int muted = 0; muted < 2; ++muted) {
            if (sun) jr_display_sun_set(422, 1175); else jr_display_sun_set(-1, -1);
            __atomic_store_n(&s_hud_env_word, 74U | (muted ? (1U << 9) : 0U), __ATOMIC_RELEASE);
            render_watch_frame_at(fb, JR_WATCH_JARVIS, JR_FACE_IDLE, 10, 8, 0);
            /* the 6 o'clock hour tick is r186..201 on the vertical through the centre (the
             * hands, at 10:08:00, are all in the upper half) */
            size_t tick = 0, ring = 0;
            for (int y = 232 + 186; y <= 232 + 201; ++y) {
                if (dial_ink(fb[(size_t)y * HUD_W + 232]) || dial_ink(fb[(size_t)y * HUD_W + 233]) || dial_ink(fb[(size_t)y * HUD_W + 231])) tick++;
            }
            /* the ring at r184: pixels on a quarter of its circle are not the art */
            for (int ox = 0; ox < 130; ++ox) {
                int oy = 184;
                while (oy * oy + ox * ox > 184 * 184) oy--;
                if (dial_ink(fb[(size_t)(232 + oy) * HUD_W + 232 + ox])) ring++;
            }
            CHECK(tick >= 8, "JARVIS (sun %d, muted %d): the 6 o'clock tick is drawn (%zu px)", sun, muted, tick);
            CHECK(ring >= 30, "JARVIS (sun %d, muted %d): the ring is drawn (%zu of 130)", sun, muted, ring);
        }
    }
    jr_display_sun_set(422, 1175);
    s_display.shown_face = JR_FACE_IDLE;
    reset_nav();
    free(fb);
}

/* The shade's mute button is an opaque disc: what the face is does not show through it. */
static void test_shade_button_disc_hides_the_face(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *a = malloc(px * sizeof *a), *b = malloc(px * sizeof *b);
    if (!a || !b) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; free(a); free(b); return; }
    for (int muted = 0; muted < 2; ++muted) {
        stage_shade(muted != 0, 40, 60, SP_SHADE_ENTRY_MS, 0U);
        for (size_t i = 0; i < px; ++i) { a[i] = 0x0000; b[i] = 0xFFFF; }
        for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
            const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
            apply_space_overlay(&s_display, y, y2, a + (size_t)y * HUD_W);
            apply_space_overlay(&s_display, y, y2, b + (size_t)y * HUD_W);
        }
        size_t seen = 0, inside = 0;
        for (int y = 0; y < HUD_H; ++y) {
            for (int x = 0; x < HUD_W; ++x) {
                const int dx = x - SP_CX, dy = y - SP_BTN_CY;
                if (dx * dx + dy * dy > 36 * 36) continue;
                inside++;
                if (a[(size_t)y * HUD_W + x] != b[(size_t)y * HUD_W + x]) seen++;
            }
        }
        CHECK(inside > 4000 && seen == 0, "muted %d: %zu of %zu px under the button disc show the face", muted, seen, inside);
    }
    reset_nav();
    free(a);
    free(b);
}

/* The rest face is one face: the same pixels whichever screen and style the device was
 * on when it went to rest. */
static void test_rest_face_is_one_face(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *ref = malloc(px * sizeof *ref), *got = malloc(px * sizeof *got);
    if (!ref || !got) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; free(ref); free(got); return; }
    for (int mode = JR_DISPLAY_REST_NIGHTSTAND; mode <= JR_DISPLAY_REST_GLANCE; ++mode) {
        static const struct { int space; int style; } via[] = {
            { JR_DISPLAY_SPACE_WATCH, JR_WATCH_JARVIS }, { JR_DISPLAY_SPACE_WATCH, JR_WATCH_DIVER },
            { JR_DISPLAY_SPACE_WATCH, JR_WATCH_MINIMAL }, { JR_DISPLAY_SPACE_JARVIS, JR_WATCH_PILOT },
        };
        for (size_t v = 0; v < sizeof via / sizeof via[0]; ++v) {
            dream_fixture();
            s_power_word |= 1u << 24;
            stage_ring_at_rest(via[v].space, JR_DISPLAY_OTA_IDLE, 0);
            s_style_shown = (uint8_t)via[v].style;
            jr_display_rest_set((jr_display_rest_mode_t)mode, true, true);
            jr_display_clock_set(true, 6, 42, 0);
            uint16_t *dst = v == 0 ? ref : got;
            memset(dst, 0, px * sizeof *dst);
            for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
                const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
                (void)apply_companion_overlay(&s_display, y, y2, dst + (size_t)y * HUD_W, (jr_display_rest_mode_t)mode);
            }
            if (v > 0) {
                /* the face itself (inside the rim band) is one face; the rim differs by the rule of the screen */
                size_t differ = 0;
                for (int y = 0; y < HUD_H; ++y) for (int x = 0; x < HUD_W; ++x) {
                    const int dx = x - 232, dy = y - 232;
                    if (dx * dx + dy * dy < 205 * 205 && ref[(size_t)y * HUD_W + x] != got[(size_t)y * HUD_W + x]) differ++;
                }
                CHECK(differ == 0,
                      "rest mode %d: the face reached via space %d style %d is not the one reached via WATCH JARVIS (%zu px)",
                      mode, via[v].space, via[v].style, differ);
            }
        }
    }
    reset_nav();
    free(ref);
    free(got);
}

/* ---- A WATCH AT REST BY DAY KEEPS ITS DIAL (2026-11-10, round 2) --------------------------- */

static void stage_rest_on_watch(int space, jr_display_rest_mode_t mode, bool night, bool time_valid)
{
    warm_fixture();                       /* DIVER decoded and on the glass */
    stage_ring_at_rest(space, JR_DISPLAY_OTA_IDLE, 0);
    s_power_word |= 1u << 24;
    jr_display_rest_set(mode, true, night);
    jr_display_clock_set(time_valid, 6, 42, 0);
    jr_display_set_hud_env(53, false, true, 0, 0);
    s_display.dream_active = s_display.quiet_active = false;
}

/* Which rest is a dial and which is the glass-font face: by day (a glance, or the nightstand
 * while it is not night, with a time to show) the chosen dial stays; at night, in DREAM, without a
 * time, and on every screen but WATCH the rest face is unchanged. */
static void test_rest_by_day_on_watch_keeps_the_dial(void)
{
    static const struct {
        int space; jr_display_rest_mode_t mode; bool night, time; bool keeps; const char *what;
    } k[] = {
        { JR_DISPLAY_SPACE_WATCH,  JR_DISPLAY_REST_NIGHTSTAND, false, true,  true,  "WATCH nightstand by day" },
        { JR_DISPLAY_SPACE_WATCH,  JR_DISPLAY_REST_GLANCE,     false, true,  true,  "WATCH glance by day" },
        { JR_DISPLAY_SPACE_WATCH,  JR_DISPLAY_REST_GLANCE,     true,  true,  true,  "WATCH glance (the night flag is the nightstand's)" },
        { JR_DISPLAY_SPACE_WATCH,  JR_DISPLAY_REST_NIGHTSTAND, true,  true,  false, "WATCH nightstand at night" },
        { JR_DISPLAY_SPACE_WATCH,  JR_DISPLAY_REST_NIGHTSTAND, false, false, false, "WATCH nightstand with no time" },
        { JR_DISPLAY_SPACE_JARVIS, JR_DISPLAY_REST_NIGHTSTAND, false, true,  false, "home nightstand by day" },
        { JR_DISPLAY_SPACE_JARVIS, JR_DISPLAY_REST_GLANCE,     false, true,  false, "home glance" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; ++i) {
        stage_rest_on_watch(k[i].space, k[i].mode, k[i].night, k[i].time);
        const jr_display_rest_mode_t raw = rest_eligible_raw(&s_display);
        CHECK(raw == k[i].mode, "%s: the raw rest mode is %d (got %d)", k[i].what, (int)k[i].mode, (int)raw);
        CHECK(watch_rest_day(&s_display) == k[i].keeps, "%s: keeps the dial is %d", k[i].what, (int)k[i].keeps);
        CHECK(rest_eligible(&s_display) == (k[i].keeps ? JR_DISPLAY_REST_NONE : k[i].mode),
              "%s: the rest face is %s", k[i].what, k[i].keeps ? "not drawn" : "drawn");
    }
    /* DREAM (off USB, not charging) on WATCH is the dark time face, never the dial */
    stage_rest_on_watch(JR_DISPLAY_SPACE_WATCH, JR_DISPLAY_REST_DREAM, false, true);
    s_power_word &= ~(1u << 24);
    jr_display_set_hud_env(53, false, true, 0, 0);
    CHECK(rest_eligible(&s_display) == JR_DISPLAY_REST_DREAM && !watch_rest_day(&s_display),
          "WATCH in DREAM keeps the glass-font rest face");
    reset_nav();
}

/* A resting dial is redrawn only when the minute (or what the rim shows) changes: frames asked
 * of the glass across 120 s of rest by day, against the awake cadence, and what else stands down. */
static void test_resting_dial_is_redrawn_only_when_the_minute_changes(void)
{
    jr_display_ctx_t *ctx = &s_display;
    stage_rest_on_watch(JR_DISPLAY_SPACE_WATCH, JR_DISPLAY_REST_NIGHTSTAND, false, true);
    CHECK(s_dial_blit, "a dial is on the glass");

    /* awake: every pass of the presenter asks for a frame */
    s_rest_day = false;
    s_still_key = UINT32_MAX;
    unsigned awake = 0;
    for (int t = 0; t < 120; ++t) awake += presenter_ticks_the_glass() ? 1U : 0U;
    CHECK(awake == 120U, "awake, every pass asks for a frame (%u of 120)", awake);

    /* at rest: 120 s in 100 ms passes, the seconds running and the minute turning twice */
    unsigned frames = 0, first_frame_at = 9999;
    s_still_frames = 0;
    for (int t = 0; t < 1200; ++t) {
        jr_display_clock_set(true, 6, 42 + (t / 10 + 30) / 60, (t / 10 + 30) % 60);
        s_rest_day = watch_rest_day(ctx);
        if (presenter_ticks_the_glass()) {
            if (frames == 0) first_frame_at = (unsigned)t;
            frames++;
        }
    }
    CHECK(frames == 3U && first_frame_at == 0U,
          "120 s of rest by day asked for %u frames (want 3: entering rest, then two minute turns)", frames);
    printf("note: 120 s of rest by day asks the glass for %u frames (awake: one per presenter pass, 120 per 12 s)\n", frames);

    /* a change of what the rim shows redraws once (the microphone goes live) */
    jr_display_set_hud_env(53, false, false, 0, 0);
    CHECK(presenter_ticks_the_glass() && !presenter_ticks_the_glass(), "the microphone going live redraws once");
    /* and so does a brightness change still to settle */
    s_still_key = UINT32_MAX;
    CHECK(presenter_ticks_the_glass(), "leaving rest and coming back draws once");

    /* nothing else runs while it rests: no neighbour decode, the on-glass frame is kept */
    s_dial_glass_ms = 0;
    s_stub_eaf_decodes = 0;
    s_rest_day = true;
    dial_warm_tick(ctx, 5000U, JR_FACE_IDLE, true);
    CHECK(s_stub_eaf_decodes == 0, "no warm-up decode while the dial rests (%d)", (int)s_stub_eaf_decodes);
    s_rest_day = false;
    dial_warm_tick(ctx, 5000U, JR_FACE_IDLE, true);
    CHECK(s_stub_eaf_decodes == 1, "the warm-up runs again once awake (%d)", (int)s_stub_eaf_decodes);
    s_rest_day = true;
    dial_cache_release();
    CHECK(s_dial_blit && s_dial_frame != NULL && s_dial_frame_face == JR_FACE_DIAL_DIVER &&
          dial_strip_mismatch(ctx, 0x11) == 0, "releasing the cache at rest keeps the dial on the glass");
    s_rest_day = false;
    reset_nav();
}

/* The pixels of a resting dial do not move with the seconds, on a dial drawn by us (JARVIS) and
 * on baked art with a seconds hand of ours (DIVER); an awake one does. */
static void test_resting_dial_has_no_seconds_hand(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *a = malloc(px * sizeof *a), *b = malloc(px * sizeof *b);
    if (!a || !b) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; free(a); free(b); return; }
    static const struct { int style; jr_face_t face; } dials[] = {
        { JR_WATCH_JARVIS, JR_FACE_IDLE }, { JR_WATCH_DIVER, JR_FACE_DIAL_DIVER },
        { JR_WATCH_DRESS, JR_FACE_DIAL_DRESS }, { JR_WATCH_PILOT, JR_FACE_DIAL_PILOT },
        { JR_WATCH_MINIMAL, JR_FACE_IDLE }, { JR_WATCH_FUTURE, JR_FACE_DIAL_FUTURE },
    };
    for (size_t i = 0; i < sizeof dials / sizeof dials[0]; ++i) {
        s_watch_still = true;
        render_watch_frame_at(a, dials[i].style, dials[i].face, 10, 8, 3);
        render_watch_frame_at(b, dials[i].style, dials[i].face, 10, 8, 41);
        size_t moved = 0;
        for (size_t k = 0; k < px; ++k) moved += a[k] != b[k];
        CHECK(moved == 0, "%s at rest: the pixels move with the seconds (%zu px)",
              jr_display_watch_style_name((jr_watch_style_t)dials[i].style), moved);
        s_watch_still = false;
        render_watch_frame_at(a, dials[i].style, dials[i].face, 10, 8, 3);
        render_watch_frame_at(b, dials[i].style, dials[i].face, 10, 8, 41);
        moved = 0;
        for (size_t k = 0; k < px; ++k) moved += a[k] != b[k];
        CHECK(moved > 30, "%s awake: the seconds hand moves (%zu px)",
              jr_display_watch_style_name((jr_watch_style_t)dials[i].style), moved);
    }
    s_watch_still = false;
    s_display.shown_face = JR_FACE_IDLE;
    reset_nav();
    free(a);
    free(b);
}

/* ---- THE FINISHED JARVIS DIAL ---------------------------------------------------------------- */

static unsigned nat565(const uint16_t v) { return s_display.board.swap_color_bytes ? (unsigned)((v >> 8) | (v << 8)) & 0xFFFFu : v; }

/* the sun bead is bright gold: the amber ticks have less green and no blue, the white hand has all of it */
static bool is_bead(unsigned v) { return ((v >> 11) & 31) >= 29 && ((v >> 5) & 63) >= 52 && (v & 31) >= 12 && (v & 31) <= 26; }

static void test_jarvis_dial_hands_arc_bead_and_date(void)
{
    const size_t px = (size_t)HUD_W * HUD_H;
    uint16_t *fb = malloc(px * sizeof *fb);
    if (!fb) { printf("FAIL %s: allocation failed\n", __func__); g_failures++; return; }
    s_display.board.swap_color_bytes = false;
    jr_display_clock_set_date(2, 10, 10);                       /* TUE 10 NOV */
    jr_display_sun_set(422, 1175);

    /* the minute hand at 3:00 is the long one straight up: tapered (wider at the hub than at the
     * tip) and anti-aliased (more than the two levels of a hard edge across its width) */
    render_watch_frame_at(fb, JR_WATCH_JARVIS, JR_FACE_IDLE, 3, 0, 0);
    int w_near = 0, w_far = 0;
    unsigned levels_seen = 0;
    uint16_t seen[16] = { 0 };
    for (int x = 215; x <= 250; ++x) {
        const uint16_t near_px = fb[(size_t)(232 - 40) * HUD_W + x], far_px = fb[(size_t)(232 - 150) * HUD_W + x];
        if (nat565(near_px) != 0) w_near++;
        if (nat565(far_px) != 0) w_far++;
        const uint16_t v = near_px;
        unsigned k = 0;
        while (k < levels_seen && seen[k] != v) k++;
        if (k == levels_seen && levels_seen < 16) seen[levels_seen++] = v;
    }
    CHECK(w_near > w_far && w_far >= 1, "the minute hand tapers (%d px at r40, %d px at r150)", w_near, w_far);
    CHECK(levels_seen >= 4, "the minute hand is anti-aliased (%u levels across it)", levels_seen);

    /* the day's arc: a thin 2 px line of dim amber at r205 (between ticks, which end at r201) */
    /* radial profile along the ray 20 deg clockwise of 12 o'clock, between ticks */
    {
        int ink = 0, bright = 0;
        for (int i = 0; i < 40; ++i) {
            const double r = 199.0 + i * 0.25;
            const int x = 233 + (int)(r * 0.3420201 + 0.5), y = 233 - (int)(r * 0.9396926 + 0.5);
            const unsigned v = nat565(fb[(size_t)y * HUD_W + x]);
            if (v != 0) ink++;
            if (((v >> 11) & 31) > 20) bright++;
        }
        CHECK(ink >= 6 && ink <= 22, "the day arc is a thin line (%d of 40 samples inked)", ink);
        CHECK(bright == 0, "the day arc is dim amber, not the bead (%d bright samples)", bright);
    }

    /* the bead at the sun: bright gold at 13:15 (75 min past noon), nothing bright at night */
    render_watch_frame_at(fb, JR_WATCH_JARVIS, JR_FACE_IDLE, 13, 15, 0);
    unsigned bead_day = 0, bead_px_at = 0;
    for (int y = 0; y < HUD_H; ++y) for (int x = 0; x < HUD_W; ++x) {
        const int dx = x - 232, dy = y - 232, r2 = dx * dx + dy * dy;
        if (r2 < 199 * 199 || r2 > 212 * 212) continue;
        const unsigned v = nat565(fb[(size_t)y * HUD_W + x]);
        if (is_bead(v)) bead_day++;
    }
    {
        /* its place: 75 min past noon = 75/1440 of a turn clockwise from 12, at r205 */
        /* sin and cos of 18.75 degrees (the test links without libm) */
        const int bx = 233 + (int)(205 * 0.321439 + 0.5), by = 233 - (int)(205 * 0.946930 + 0.5);
        const unsigned v = nat565(fb[(size_t)by * HUD_W + bx]);
        bead_px_at = is_bead(v);
    }
    CHECK(bead_day >= 20 && bead_px_at, "the sun is a bright bead on the arc (%u bright px, at the sun %u)", bead_day, bead_px_at);
    render_watch_frame_at(fb, JR_WATCH_JARVIS, JR_FACE_IDLE, 22, 15, 0);
    unsigned bead_night = 0;
    for (int y = 0; y < HUD_H; ++y) for (int x = 0; x < HUD_W; ++x) {
        const int dx = x - 232, dy = y - 232, r2 = dx * dx + dy * dy;
        if (r2 < 199 * 199 || r2 > 212 * 212) continue;
        const unsigned v = nat565(fb[(size_t)y * HUD_W + x]);
        if (is_bead(v)) bead_night++;
    }
    CHECK(bead_night == 0, "no bead at night (%u bright px)", bead_night);

    /* the date under the centre, in the glass LABEL type, dim amber; none without a date */
    render_watch_frame_at(fb, JR_WATCH_JARVIS, JR_FACE_IDLE, 3, 0, 0);
    unsigned date_ink = 0;
    const glass_metrics_t ml = glass_font_metrics(GLASS_FONT_LABEL);
    for (int y = JARVIS_DATE_Y + ml.ink_top; y <= JARVIS_DATE_Y + ml.ink_bottom; ++y)
        for (int x = 233 - 60; x <= 233 + 60; ++x) date_ink += nat565(fb[(size_t)y * HUD_W + x]) != 0;
    CHECK(date_ink >= 40, "the date is drawn under the centre (%u px)", date_ink);
    CHECK(strcmp(s_watch_words.jdate, "TUE 10") == 0, "the date reads TUE 10 (got '%s')", s_watch_words.jdate);
    jr_display_clock_set_date(0, 0, 0);
    render_watch_frame_at(fb, JR_WATCH_JARVIS, JR_FACE_IDLE, 3, 0, 0);
    unsigned none = 0;
    for (int y = JARVIS_DATE_Y + ml.ink_top; y <= JARVIS_DATE_Y + ml.ink_bottom; ++y)
        for (int x = 233 - 60; x <= 233 + 60; ++x) none += nat565(fb[(size_t)y * HUD_W + x]) != 0;
    CHECK(date_ink >= none + 40, "no date, no date text (%u px with, %u without)", date_ink, none);
    jr_display_clock_set_date(3, 23, 8);
    s_display.shown_face = JR_FACE_IDLE;
    reset_nav();
    free(fb);
}

static void test_companion_actual_flush_preserves_rims_and_strip_order(void)
{
  for (int muted = 0; muted < 2; ++muted) {
    for (int mode = JR_DISPLAY_REST_NIGHTSTAND; mode <= JR_DISPLAY_REST_GLANCE; ++mode) {
        for (int swap = 0; swap < 2; ++swap) {
            dream_fixture();
            s_power_word |= 1u << 24;
            stage_ring_at_rest(JR_DISPLAY_SPACE_WATCH, JR_DISPLAY_OTA_IDLE, 0);
            s_display.board.swap_color_bytes = swap != 0;
            jr_display_rest_set((jr_display_rest_mode_t)mode, true, true);
            jr_display_clock_set(true, 6, 42, 0);
            jr_display_set_hud_env(53, true, muted != 0, 24, -24);
            uint16_t *whole = calloc(HUD_W * HUD_H, sizeof *whole);
            uint16_t *strips = calloc(HUD_W * HUD_H, sizeof *strips);
            uint16_t *rims = calloc(HUD_W * HUD_H, sizeof *rims);
            uint16_t *later = calloc(HUD_W * HUD_H, sizeof *later);
            CHECK(whole && strips && rims && later, "allocate frame comparison");
            s_stub_disp_user_data = &s_display;
            panel_flush(s_display.disp, 0, 0, HUD_W, HUD_H, whole); complete_test_transfer();
            for (int y = 0; y < HUD_H; y += 13) {
                const int end = y + 13 < HUD_H ? y + 13 : HUD_H;
                panel_flush(s_display.disp, 0, y, HUD_W, end, strips + y * HUD_W); complete_test_transfer();
            }
            apply_rest_rims(&s_display, 0, HUD_H, rims);
            CHECK(memcmp(whole, strips, HUD_W * HUD_H * sizeof *whole) == 0,
                  "companion mode%d swap%d preserves full/strip composition", mode, swap);
            unsigned bad_rim = 0, near_black = 0, clock_ink = 0, item_ink = 0, rim_ink = 0, dial_ink_px = 0;
            for (int y = 0; y < HUD_H; ++y) for (int x = 0; x < HUD_W; ++x) {
                const int dx = x - 232, dy = y - 232;
                const uint16_t px = whole[y * HUD_W + x];
                if (dx * dx + dy * dy >= 180 * 180 && px != rims[y * HUD_W + x]) { bad_rim++; }
                if (dx * dx + dy * dy >= 205 * 205 && px != 0) { rim_ink++; }
                if (dx * dx + dy * dy < 205 * 205 && px != 0) { dial_ink_px++; }
                /* the glass type is anti-aliased warm amber: inside the face every pixel is black or
                 * a blend of black and amber (no blue to speak of) */
                const uint16_t nat = s_display.board.swap_color_bytes ? (uint16_t)((px >> 8) | (px << 8)) : px;
                if (dx * dx + dy * dy < 180 * 180 && (nat & 31) > 4) { near_black++; }
                if (y >= 187 && y < 247 && ((nat >> 11) & 31) > 3) { clock_ink++; }
                if (y >= 288 && y < 304 && px != 0) { item_ink++; }     /* the one quiet line */
            }
            if (mode == JR_DISPLAY_REST_NIGHTSTAND) {
                CHECK(bad_rim == 0 && near_black == 0 && clock_ink > 0 && item_ink > 0,
                      "actual WATCH flush at night keeps the amber glass clock, its one line and black content mode%d swap%d muted%d (rim %u, black %u, clock %u, line %u)",
                      mode, swap, muted, bad_rim, near_black, clock_ink, item_ink);
            } else {
                /* BY DAY the chosen dial stays, drawn as a still: it has ink, and the seconds do not move it */
                CHECK(s_watch_still && dial_ink_px > 500,
                      "a glance on WATCH keeps the dial (still %d, %u px) swap%d muted%d", (int)s_watch_still, dial_ink_px, swap, muted);
                jr_display_clock_set(true, 6, 42, 37);
                panel_flush(s_display.disp, 0, 0, HUD_W, HUD_H, later); complete_test_transfer();
                CHECK(memcmp(whole, later, HUD_W * HUD_H * sizeof *whole) == 0,
                      "a resting dial draws the same pixels at second 0 and second 37 swap%d muted%d", swap, muted);
            }
            /* ONE RULE ON WATCH, dial or rest face: muted and idle shows nothing at the rim; live and idle a single
             * privacy dot */
            CHECK(muted ? rim_ink == 0 : (rim_ink >= 20 && rim_ink <= 130),
                  "WATCH at rest: the rim is %s mode%d swap%d (%u px)", muted ? "absent when muted" : "one dot when live",
                  mode, swap, rim_ink);
            free(whole); free(strips); free(rims); free(later);
        }
    }
  }
}

static void test_panel_ack_requires_transport_settle_and_completed_recovery(void)
{
    for (unsigned failure = 0; failure < 4; ++failure) {
        dream_fixture();
        if (failure == 1) { s_stub_brightness_error = ESP_FAIL; }
        if (failure == 2) { s_stub_off_error = ESP_FAIL; }
        if (failure == 3) { s_stub_sleep_in_error = ESP_FAIL; }
        CHECK(!jr_display_panel_is_on() && !jr_display_panel_is_off(), "initial panel ack unavailable");
        jr_display_panel_off_request();
        const uint32_t request = s_panel_off;
        jr_display_panel_off_request();
        CHECK(s_panel_off == request, "repeated off request retains generation");
        const unsigned ready = s_stub_flush_ready_calls;
        flush_test_strip(0);
        CHECK(jr_display_panel_is_off() == (failure == 0), "off acknowledges only all successful commands failure%d", failure);
        CHECK(s_stub_draw_calls == 0 && s_stub_flush_ready_calls == ready + 1, "no DMA; releases failed/off strip once");
        CHECK(s_stub_sleep_in_calls == (failure < 3 && failure != 0 ? 0U : 1U), "off failure stops later hardware mutation%d", failure);
        if (failure == 0) { CHECK(s_fake_us - s_stub_sleep_in_us >= 120000, "off includes real settling interval"); }
        s_stub_brightness_error = s_stub_off_error = s_stub_sleep_in_error = ESP_OK;
        jr_display_panel_on_request();
        const uint32_t recover = s_panel_off;
        jr_display_panel_on_request();
        CHECK(s_panel_off == recover && recover != request, "opposite intent supersedes generation, repeated on idempotent");
        flush_test_strip(0);
        CHECK(!jr_display_panel_is_on() && !jr_display_panel_is_off(), "successful recovery submit alone cannot ack");
        CHECK(s_stub_sleep_out_calls == 1 && s_stub_on_calls == 1 && s_stub_panel_brightness == 8,
              "recover conservatively wakes, displays and restores prior effective brightness");
        CHECK(s_fake_us - s_stub_sleep_out_us >= 120000, "on settles before render");
        if (failure == 3) { CHECK(s_stub_sleep_out_us - s_stub_sleep_in_us >= 120000, "uncertain failed sleep obeys opposite exclusion"); }
        complete_test_transfer();
        CHECK(jr_display_panel_is_on() && !jr_display_panel_is_off(), "only matching completed post-recovery strip acknowledges on");
        atomic_store(&s_brightness_have, 17); atomic_store(&s_brightness_shown, 17); s_brightness_want = 17;
        jr_display_panel_off_request(); flush_test_strip(0);
        jr_display_panel_on_request(); flush_test_strip(0); complete_test_transfer();
        CHECK(s_stub_panel_brightness == 17 && jr_display_panel_is_on(), "next sleep captures fresh brightness, not first recovery value");
    }
}

/* The panel dip never outlives the panel: a panel-off request in the middle of a dip ends the
 * dip, no brightness is written while the panel is off, and the recovery restores the shown
 * brightness rather than the dimmed value the dip had last written. */
static void test_panel_dip_dies_with_the_panel(void)
{
    dream_fixture();
    jr_display_set_perf_knob(JR_KNOB_PANEL_DIP);
    knob_apply();
    dip_abort();
    s_dip_tail = false;
    s_brightness_want = 100U;
    atomic_store(&s_brightness_have, 100U);
    atomic_store(&s_brightness_shown, 100);
    s_stub_panel_brightness = 100U;
    s_stub_brightness_calls = 0U;
    const uint32_t now_ms = (uint32_t)(s_fake_us / 1000);
    dip_begin(DIP_MODE_SPACE, TR_CUT_SLIDE, (uint32_t)JR_DISPLAY_SPACE_MS, now_ms);
    s_fake_us += 60000;
    brightness_pump();
    CHECK(s_dip < 256 && s_stub_panel_brightness < 100U && s_stub_brightness_calls > 0U,
          "the dip dims the panel (dip %d, panel %u)", s_dip, (unsigned)s_stub_panel_brightness);
    jr_display_panel_off_request();
    flush_test_strip(0);
    CHECK(jr_display_panel_is_off() && s_stub_panel_brightness == 0U, "the panel went off in the middle of the dip");
    CHECK(s_dip_phase == DIP_IDLE && s_dip == 256 && s_dip_mode == DIP_MODE_NONE && !s_dip_tail,
          "a panel-off request ends the dip (phase %d dip %d)", (int)s_dip_phase, s_dip);
    const unsigned calls = s_stub_brightness_calls;
    s_fake_us += 100000;
    flush_test_strip(0);
    CHECK(s_stub_brightness_calls == calls && s_stub_panel_brightness == 0U,
          "no brightness write while the panel is off (%u more)", s_stub_brightness_calls - calls);
    jr_display_panel_on_request();
    flush_test_strip(0);
    CHECK(s_stub_panel_brightness == 100U, "recovery restores the shown brightness, not the dimmed one (%u)",
          (unsigned)s_stub_panel_brightness);
    complete_test_transfer();
    CHECK(jr_display_panel_is_on() && s_dip == 256 && s_dip_phase == DIP_IDLE, "recovered, undimmed");
    jr_display_set_perf_knob(0U);
    knob_apply();
}

/* The dip multiplies the brightness the mood slew has reached (s_brightness_shown), not the one it
 * is heading for: a dip in the middle of a slew never sends more than the slew has given. */
static void test_panel_dip_composes_with_the_mood_slew(void)
{
    dream_fixture();
    jr_display_set_perf_knob(JR_KNOB_PANEL_DIP);
    knob_apply();
    dip_abort();
    s_dip_tail = false;
    s_dip_wrote = false;
    s_brightness_want = 100U;
    atomic_store(&s_brightness_have, 30U);
    atomic_store(&s_brightness_shown, 30);
    s_stub_panel_brightness = 30U;
    s_stub_brightness_calls = 0U;
    dip_begin(DIP_MODE_SPACE, TR_CUT_SLIDE, (uint32_t)JR_DISPLAY_SPACE_MS, (uint32_t)(s_fake_us / 1000));
    s_fake_us += 80000;
    brightness_pump();
    const int law = (30 * dip_light(s_dip)) >> 8;
    CHECK(s_dip < 256 && s_stub_brightness_calls == 1U && (int)s_stub_panel_brightness == law,
          "the dip composes with the shown brightness (dip %d: sent %u, wanted %d)", s_dip,
          (unsigned)s_stub_panel_brightness, law);
    dip_abort();
    s_dip_tail = false;
    jr_display_set_perf_knob(0U);
    knob_apply();
}

/* The restoring write after the rise is a dip write like the others: rate-limited, counted, and
 * not made by the plain path a moment after the dip's last step. */
static void test_panel_dip_tail_is_rate_limited(void)
{
    dream_fixture();
    jr_display_set_perf_knob(JR_KNOB_PANEL_DIP);
    knob_apply();
    dip_abort();
    s_brightness_want = 100U;
    atomic_store(&s_brightness_shown, 100);
    atomic_store(&s_brightness_have, 99U);        /* the last step of the rise sent 99 */
    s_stub_panel_brightness = 99U;
    s_stub_brightness_calls = 0U;
    s_dip_writes = 0U;
    const uint32_t now = (uint32_t)(s_fake_us / 1000) + 1000U;
    s_fake_us = (int64_t)now * 1000;
    s_dip_mode = DIP_MODE_SPACE;
    s_dip_cut = TR_CUT_SLIDE;
    s_dip_in_ms = 100U;
    s_dip_t0 = now - 100U;                         /* the rise ends on this pump */
    s_dip_phase = DIP_IN;
    s_dip_wrote = true;
    s_dip_write_ms = now - 5U;                     /* ... 5 ms after the write that sent 99 */
    brightness_pump();
    CHECK(s_stub_brightness_calls == 0U && s_dip_phase == DIP_IDLE && s_dip_tail,
          "the rise ends inside the write interval: the restoring write waits (calls %u)", s_stub_brightness_calls);
    s_fake_us += 1000;
    brightness_pump();
    CHECK(s_stub_brightness_calls == 0U, "6 ms after the last write it still waits (the tail keeps the limit)");
    s_fake_us += 9000;
    brightness_pump();
    CHECK(s_stub_brightness_calls == 0U, "15 ms after the last write it still waits");
    s_fake_us += 1000;
    brightness_pump();
    CHECK(s_stub_brightness_calls == 1U && s_stub_panel_brightness == 100U && !s_dip_tail && s_dip_writes == 1U,
          "after the interval the restoring write is made and counted (calls %u, panel %u)", s_stub_brightness_calls,
          (unsigned)s_stub_panel_brightness);
    dip_abort();
    s_dip_tail = false;
    jr_display_set_perf_knob(0U);
    knob_apply();
}

static void cancel_off_during_settle(void)
{
    s_stub_delay_hook = NULL;
    CHECK(!jr_display_panel_is_off(), "off remains unacknowledged during settling");
    jr_display_panel_on_request();
}

static void test_panel_cancellation_failures_and_old_completions(void)
{
    dream_fixture();
    flush_test_strip(0); /* old neutral transfer still in flight */
    jr_display_panel_off_request(); s_stub_delay_hook = cancel_off_during_settle;
    flush_test_strip(0);
    CHECK(!jr_display_panel_is_off() && !jr_display_panel_is_on(), "cancelled old off cannot publish acknowledgement");
    complete_test_transfer();
    CHECK(!jr_display_panel_is_on(), "old neutral DMA cannot acknowledge new recovery");
    flush_test_strip(0);
    CHECK(s_stub_sleep_out_us - s_stub_sleep_in_us >= 120000, "cancelled sleep still observes opposite exclusion");
    jr_display_panel_off_request(); /* cancel recovery before its callback */
    complete_test_transfer();
    CHECK(!jr_display_panel_is_on() && !jr_display_panel_is_off(), "late old recovery cannot overwrite newer off");
    flush_test_strip(0);
    CHECK(jr_display_panel_is_off(), "latest off still completes");
    for (unsigned failure = 0; failure < 4; ++failure) {
        jr_display_panel_on_request();
        s_stub_sleep_out_error = failure == 0 ? ESP_FAIL : ESP_OK;
        s_stub_on_error = failure == 1 ? ESP_FAIL : ESP_OK;
        s_stub_brightness_error = failure == 2 ? ESP_FAIL : ESP_OK;
        s_stub_draw_error = failure == 3 ? ESP_FAIL : ESP_OK;
        const unsigned draw = s_stub_draw_calls;
        flush_test_strip(0);
        CHECK(!jr_display_panel_is_on(), "recovery stage%d error never acknowledges", failure);
        CHECK(s_stub_draw_calls == draw + (failure == 3), "recovery stage%d stops before unsafe DMA", failure);
        s_stub_sleep_out_error = s_stub_on_error = s_stub_brightness_error = s_stub_draw_error = ESP_OK;
        flush_test_strip(0); complete_test_transfer();
        CHECK(jr_display_panel_is_on(), "recovery stage%d retries without stranding renderer", failure);
        jr_display_panel_off_request(); flush_test_strip(0);
    }
}

static void early_transfer_hook(void)
{
    complete_test_transfer();
    CHECK(s_display.first_glance_time_transfer_state == 0U,
          "early callback cannot mark valid-time before submission result");
    s_fake_us += 17000; /* submission returns after the actual completion */
}

/* a row inside the rest face's time (cap top 187 with a quiet line under it, 59 px tall) */
#define REST_TIME_ROW 200
static void test_glance_proxy_tracks_actual_successful_inflight_content(void)
{
    dream_fixture();
    s_display.first_transfer_state = 0;
    flush_test_strip(REST_TIME_ROW); /* neutral DREAM before valid-time Glance */
    jr_display_rest_set(JR_DISPLAY_REST_GLANCE, false, false);
    jr_display_clock_set(true, 5, 10, 0);
    complete_test_transfer();
    CHECK(s_display.first_transfer_state == 1U && s_display.first_glance_time_transfer_state == 0U,
          "neutral first-strip B proxy distinct from later Glance proxy");
    flush_test_strip(0); complete_test_transfer();
    CHECK(s_display.first_glance_time_transfer_state == 0U, "non-clock Glance strip is not useful-time proof");
    jr_display_clock_set(false, 0, 0, 0);
    flush_test_strip(REST_TIME_ROW); jr_display_clock_set(true, 5, 10, 0); complete_test_transfer();
    CHECK(s_display.first_glance_time_transfer_state == 0U, "unknown clock tag remains unknown despite later valid mailbox");
    s_stub_draw_hook = early_transfer_hook; s_stub_draw_error = ESP_FAIL;
    const unsigned early_ready = s_stub_flush_ready_calls;
    flush_test_strip(REST_TIME_ROW);
    CHECK(s_display.first_glance_time_transfer_state == 0U, "even early completion plus failed draw cannot publish Glance proof");
    CHECK(s_stub_flush_ready_calls == early_ready + 1U, "early failed draw releases gfx exactly once");
    s_stub_draw_error = ESP_OK; s_fake_us = 2345678;
    flush_test_strip(REST_TIME_ROW);
    CHECK(s_display.first_glance_time_transfer_state == 1U &&
          s_display.first_glance_time_transfer_done_boot_ms == 2345, "actual early completion records its timestamp after successful draw");
    s_stub_draw_hook = NULL;
    s_fake_us = 3456000; flush_test_strip(REST_TIME_ROW); complete_test_transfer();
    CHECK(s_display.first_glance_time_transfer_done_boot_ms == 2345, "Glance first useful transfer timestamp immutable");
    const jr_display_boot_timing_t timing = boot_timing_read();
    CHECK(timing.first_glance_time_transfer_valid && timing.first_glance_time_transfer_done_boot_ms == 2345,
          "public diagnostic exposes separate non-optical completed-time proxy");
    dream_fixture();
    s_display.first_glance_time_transfer_state = 0;
    jr_display_rest_set(JR_DISPLAY_REST_GLANCE, false, false); jr_display_clock_set(true, 0, 0, 0);
    s_fake_us = 0; flush_test_strip(REST_TIME_ROW); complete_test_transfer();
    CHECK(s_display.first_glance_time_transfer_state == 1 && s_display.first_glance_time_transfer_done_boot_ms == 0,
          "zero-time completed Glance is valid independently of timestamp sentinel");
    const jr_display_boot_timing_t zero_timing = boot_timing_read();
    CHECK(zero_timing.first_glance_time_transfer_valid &&
          zero_timing.first_glance_time_transfer_done_boot_ms == 0,
          "public proxy reports zero-time completion as valid");
    dream_fixture();
    for (unsigned i = 0; i < DISPLAY_TRANSFER_TAGS; ++i) { flush_test_strip(0); }
    CHECK(s_stub_draw_calls == DISPLAY_TRANSFER_TAGS, "every bounded FIFO slot admits one in-flight strip");
    const unsigned draws = s_stub_draw_calls, ready = s_stub_flush_ready_calls;
    flush_test_strip(0);
    CHECK(s_stub_draw_calls == draws && s_stub_flush_ready_calls == ready + 1,
          "bounded tag overflow refuses DMA and releases renderer");
    for (unsigned i = 0; i < DISPLAY_TRANSFER_TAGS; ++i) { complete_test_transfer(); }
    flush_test_strip(0); complete_test_transfer();
    CHECK(s_stub_draw_calls == draws + 1, "tag FIFO wraps after completed transfers");
}

/* Quiet HOME keeps its visible baked frame. These checks run the production
 * presenter decisions; pinned-vendor one-shot playback is tested separately. */
static void quiet_fixture(void)
{
    dream_fixture();
    jr_display_dream_set(false);
    jr_display_caption_clear();
    s_caption_prog = s_caption_ease = 0;
    s_clock_prog = s_clock_ease = 0;
    s_space_from = s_space_to = JR_DISPLAY_SPACE_JARVIS;
    s_space_serial_seen = 0U;
    s_space_prog = s_space_ease = 256;
    s_space_on = false;
    s_detail_prog = s_shade_prog = s_ask_prog = s_canvas_prog = 0;
    s_render_fps = 6U;
    s_stub_segments = 0U;
    s_stub_segment_error = ESP_OK;
    s_stub_refreshes = s_stub_anim_starts = s_stub_anim_stops = 0U;
    s_stub_lock_failures = 0U;
    s_stub_lock_hook = NULL;
}

static void test_quiet_home_one_frame_and_sparse_facts(void)
{
    quiet_fixture();
    uint32_t word = s_display.requested_word;
    CHECK(quiet_eligible(&s_display) && presenter_quiet_tick(&s_display, &word),
          "muted settled HOME selects visible quiet frame");
    CHECK(s_display.quiet_active && s_stub_segment_start == 0U &&
          s_stub_segment_end == 1U && !s_stub_segment_repeat &&
          s_stub_segment_fps == face_fps(s_display.shown_face) &&
          s_stub_anim_starts == 1U && s_stub_anim_stops == 1U && s_stub_anim_visible,
          "single frame is decoded once, stays visible, and never loops");
    CHECK(presenter_wait_ms(&s_display) == 1000U, "settled quiet presenter only checks TTL/diagnostics");
    for (unsigned i = 0; i < 720U; ++i) {
        s_fake_us += 1000000;
        jr_display_set_hud_env(74U, false, true, (float)i, -(float)i);
        CHECK(presenter_quiet_tick(&s_display, &word), "quiet stays eligible at second%u", i);
    }
    CHECK(s_stub_segments == 1U && s_stub_refreshes == 0U,
          "720 unchanged seconds never replay or dirty the quiet face");
    jr_display_set_hud_env(73U, false, true, 0, 0);
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 1U,
          "changed battery redraws its truthful rim");
    jr_display_set_hud_env(73U, false, true, 1, -1);
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 1U,
          "disabled parallax and repeated same facts do not dirty quiet HOME");
    s_hud_env_word ^= 1U << 16;
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 2U,
          "a genuine packed rim position change remains visible");
    jr_display_caption_set("MUTED - TAP TO WAKE");
    s_caption_prog = s_caption_ease = 256;
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 3U,
          "new ambient caption redraws even with unchanged flag bits");
    jr_display_caption_set("ON BATTERY");
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 4U,
          "second ambient caption bytes reach the glass");
    const uint32_t same_caption_revision = s_caption_revision;
    jr_display_caption_set("ON BATTERY");
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 4U,
          "idempotent caption stays still");
    CHECK(s_caption_revision == same_caption_revision,
          "identical caption bytes do not advance revision");
    jr_display_clock_set(true, 12, 34, 0);
    s_clock_prog = s_clock_ease = 256;
    s_watch_s = 256;
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 5U,
          "valid clock admission redraws");
    jr_display_clock_set(true, 12, 34, 1);
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 6U,
          "clock seconds tick honestly without repeating the face");
    jr_display_clock_set(true, 12, 35, 0);
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 7U,
          "minute boundary redraws honestly");
    CHECK(s_stub_segments == 1U, "fact updates do not restart animation");
    jr_display_set_brightness(22U);
    CHECK(presenter_wait_ms(&s_display) == 167U, "brightness slew keeps existing cadence");
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 8U,
          "brightness pending forces render-owned pump");
    s_display.snapshot_refresh_requested = 1U;
    CHECK(presenter_refresh_pending(&s_display, false) && s_stub_refreshes == 9U,
          "stopped quiet face still services a fresh snapshot");
    jr_display_panel_off_request();
    CHECK(presenter_refresh_pending(&s_display, false) && s_stub_refreshes == 10U,
          "stopped quiet face still services panel off");
    jr_display_panel_on_request();
    CHECK(presenter_refresh_pending(&s_display, false) && s_stub_refreshes == 11U,
          "stopped quiet face still services recovery");
}

static void test_quiet_refuses_dynamic_owners_and_unfinished_fades(void)
{
    for (unsigned guard = 0; guard < 30U; ++guard) {
        quiet_fixture();
        uint32_t word = s_display.requested_word;
        CHECK(presenter_quiet_tick(&s_display, &word), "quiet guard%u starts active", guard);
        switch (guard) {
        case 0: s_hud_env_word &= ~(1u << 9); break;
        case 1: s_hud_env_word |= 1u << 8; break;
        case 2: jr_display_rest_voice_busy_set(true); break;
        case 3: s_render_fps = 12U; break;
        case 4: s_display.requested_word |= JR_DISPLAY_CMD_BLANK; break;
        case 5: s_display.requested_word = (uint32_t)JR_FACE_LISTENING << 8; break;
        case 6: s_display.requested_word = (uint32_t)JR_FACE_SPEAKING << 8; break;
        case 7: s_display.requested_word = (uint32_t)JR_FACE_THINKING << 8; break;
        case 8: s_display.requested_word = (uint32_t)JR_FACE_ERROR << 8; break;
        case 9: s_nav_word = JR_DISPLAY_SPACE_WATCH; break;
        case 10: s_nav_word = JR_DISPLAY_SPACE_WEATHER; break;
        case 11: s_nav_word = (uint32_t)JR_DISPLAY_OVERLAY_SHADE << NAV_OVL_SHIFT; break;
        case 12: s_display.shell_word = JR_DISPLAY_SHELL_AGENT; break;
        case 13: s_choice_n = 1; break;
        case 14: s_commit_pct = 20U; break;
        case 15: s_ripple_start_ms = 999U; break;
        case 16: s_bloom_start_ms = 999U; break;
        case 17: s_canvas_until_ms = 2000U; break;
        case 18: s_surface_active = true; break;
        case 19:
            jr_display_caption_set("NETWORK ERROR");
            s_caption_prog = 256; /* settled foreground, independent of fade guard */
            break;
        case 20: atomic_store(&s_caption_pinned, true); break;
        case 21: s_space_prog = 128; break;
        case 22: s_nav_word = 1U << NAV_SERIAL_SHIFT; break;
        case 23: s_detail_ease = 1; break;
        case 24: s_shade_ease = 1; break;
        case 25: s_ask_ease = 1; break;
        case 26: s_canvas_ease = 1; break;
        case 27: s_caption_prog = 1; break;
        case 28: s_clock_prog = 1; break;
        case 29: s_ota_word = JR_DISPLAY_OTA_RECEIVING; break;
        }
        CHECK(!quiet_eligible(&s_display) && !presenter_quiet_tick(&s_display, &word),
              "dynamic owner/fade%u exits quiet", guard);
        CHECK(!s_display.quiet_active && s_display.last_end == -1 && word == UINT32_MAX,
              "exit%u forces same-face reapplication", guard);
        if (guard == 1U) {
            bool stale;
            CHECK(apply_face(&s_display, JR_FACE_RESTING, JR_FACE_RESTING, 0U, &stale) == ESP_OK &&
                  s_stub_segment_repeat && s_stub_segment_end == 7U && s_stub_anim_visible,
                  "charging restores repeating same face and comet");
        }
    }
    quiet_fixture(); s_display.test_pattern = JR_DISPLAY_TEST_WHITE;
    CHECK(!quiet_eligible(&s_display), "panel diagnostic remains dynamic");
    quiet_fixture(); s_hud_enabled = 0U;
    CHECK(!quiet_eligible(&s_display), "HUD-disabled art stays on normal path");
    quiet_fixture(); jr_display_rest_set(JR_DISPLAY_REST_GLANCE, false, false);
    CHECK(!quiet_eligible(&s_display), "explicit Rest owns its own presentation");
}

static void quiet_late_speech(void)
{
    s_stub_lock_hook = NULL;
    jr_display_rest_voice_busy_set(true);
}

static void test_quiet_failures_retry_and_never_freeze_old_assets(void)
{
    quiet_fixture();
    uint32_t word = s_display.requested_word;
    s_stub_lock_failures = 1U;
    CHECK(!presenter_quiet_tick(&s_display, &word) && !s_display.quiet_active,
          "failed entry lock cannot latch quiet");
    CHECK(presenter_quiet_tick(&s_display, &word), "entry retries after lock failure");
    jr_display_set_hud_env(72U, false, true, 0, 0);
    const uint32_t old_env = s_display.quiet_env_key;
    s_stub_lock_failures = 1U;
    CHECK(presenter_quiet_tick(&s_display, &word) && s_display.quiet_env_key == old_env,
          "failed fact refresh cannot consume the new rim");
    CHECK(presenter_quiet_tick(&s_display, &word) && s_display.quiet_env_key == s_hud_env_word,
          "failed fact refresh retries the new rim");
    quiet_fixture(); CHECK(presenter_quiet_tick(&s_display, &word), "quiet before DMA fault");
    jr_display_set_hud_env(72U, false, true, 0, 0);
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 1U,
          "new rim schedules sparse transfer");
    flush_test_strip(0); /* the real panel callback receives a failed transport */
    const uint32_t good_errors = s_display.quiet_flush_error_key;
    s_stub_draw_error = ESP_FAIL;
    flush_test_strip(12);
    s_stub_draw_error = ESP_OK;
    s_stub_lock_failures = 1U;
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 1U &&
          s_display.quiet_flush_error_key == good_errors,
          "failed retry lock preserves unacknowledged DMA error");
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 2U &&
          s_display.quiet_flush_error_key == s_display.flush_errors,
          "actual failed strip retries the whole stopped frame despite unchanged fact keys");
    CHECK(presenter_quiet_tick(&s_display, &word) && s_stub_refreshes == 2U,
          "successful sparse retry returns to zero periodic redraw");
    quiet_fixture(); s_stub_segment_error = ESP_FAIL;
    CHECK(!presenter_quiet_tick(&s_display, &word) && !s_display.quiet_active,
          "failed segment command cannot latch quiet");
    s_stub_segment_error = ESP_OK;
    CHECK(presenter_quiet_tick(&s_display, &word), "segment entry retries");
    quiet_fixture(); s_stub_lock_hook = quiet_late_speech;
    CHECK(!presenter_quiet_tick(&s_display, &word) && s_stub_segments == 0U,
          "late speech under render lock refuses one-shot programming");
    quiet_fixture(); s_display.loaded_face = JR_FACE_ERROR;
    CHECK(!presenter_quiet_tick(&s_display, &word), "old error asset cannot freeze as a quiet face");
    quiet_fixture(); s_display.blanked = true;
    CHECK(!presenter_quiet_tick(&s_display, &word), "hidden blank must rebind before quiet");
    quiet_fixture(); s_display.active = NULL;
    CHECK(!presenter_quiet_tick(&s_display, &word), "unloaded asset retains loader/retry path");
    quiet_fixture();
    jr_display_clip_t one_frame_clip = *s_display.active;
    one_frame_clip.last_frame = 0U;
    s_display.active = &one_frame_clip;
    CHECK(!presenter_quiet_tick(&s_display, &word),
          "vendor would clamp a one-frame asset to0 before preparing; keep normal fallback");
    quiet_fixture(); CHECK(presenter_quiet_tick(&s_display, &word), "quiet before asset change");
    s_display.requested_word = (uint32_t)JR_FACE_MUTED << 8;
    CHECK(!presenter_quiet_tick(&s_display, &word) && !s_display.quiet_active,
          "different quiet asset releases old one-shot latch");
    s_display.loaded_face = JR_FACE_MUTED;
    CHECK(presenter_quiet_tick(&s_display, &word), "newly bound quiet asset gets its own one-shot");
    CHECK(apply_blank(&s_display) == ESP_OK && !s_display.quiet_active,
          "DREAM/blank entry clears quiet ownership");
}

static jmp_buf s_quiet_task_done;
static unsigned s_quiet_task_passes;
static void quiet_task_step(void)
{
    if (++s_quiet_task_passes > 721U) { longjmp(s_quiet_task_done, 1); }
    s_fake_us += 1000000;
}
static void test_actual_presenter_runs_quiet_not_repeat(void)
{
    quiet_fixture();
    s_stub_engine_ready = true;
    s_stub_board_get_error = s_stub_spiffs_error = ESP_OK;
    s_stub_board_display = s_display.board;
    s_stub_board_init_us = 0;
    s_stub_disp_add_hook = NULL;
    s_quiet_task_passes = 0U;
    s_stub_notify_take_hook = quiet_task_step;
    if (setjmp(s_quiet_task_done) == 0) { display_task(&s_display); }
    s_stub_notify_take_hook = NULL;
    CHECK(s_display.quiet_active && s_stub_segments == 1U && !s_stub_segment_repeat &&
          s_stub_anim_visible && s_stub_refreshes == 1U,
          "actual task721 passes: one startup pattern refresh, one visible one-shot, zero periodic refresh");
}

/* ------------------------------------------------------------------ light -- */

static bool light_eq_rest(const hud_light_t *a, const hud_light_t *b)
{
    return a->shadow_dx == b->shadow_dx && a->shadow_dy == b->shadow_dy &&
           a->bevel_lx == b->bevel_lx && a->bevel_ly == b->bevel_ly &&
           a->glint_x == b->glint_x && a->glint_y == b->glint_y;
}

/* The in-plane light travels as ONE packed word: gx bits 0-11, gy bits 12-23
 * (signed 12-bit), valid bit 24. Pin the layout and the round trip. */
static void test_light_word_round_trips(void)
{
    static const int vals[] = { 0, 1, -1, 80, -80, 1000, -1000, 1200, -1200, 2047, -2048, 5000, -5000 };
    for (size_t i = 0; i < sizeof vals / sizeof *vals; ++i) {
        for (size_t j = 0; j < sizeof vals / sizeof *vals; ++j) {
            for (int valid = 0; valid < 2; ++valid) {
                jr_display_set_light(vals[i], vals[j], valid != 0);
                const uint32_t w = __atomic_load_n(&s_light_word, __ATOMIC_ACQUIRE);
                const int ex = vals[i] > 1200 ? 1200 : vals[i] < -1200 ? -1200 : vals[i];
                const int ey = vals[j] > 1200 ? 1200 : vals[j] < -1200 ? -1200 : vals[j];
                CHECK(light_field(w, 0) == ex, "gx %d came back %d", vals[i], light_field(w, 0));
                CHECK(light_field(w, 12) == ey, "gy %d came back %d", vals[j], light_field(w, 12));
                CHECK(((w & JR_LIGHT_VALID) != 0u) == (valid != 0), "valid bit lost");
                CHECK((w >> 25) == 0u, "bits above the valid bit are set (0x%08x)", w);
            }
        }
    }
    jr_display_set_light(-1, 1, true);
    CHECK(__atomic_load_n(&s_light_word, __ATOMIC_ACQUIRE) == (0xFFFu | (1u << 12) | JR_LIGHT_VALID),
          "layout drifted: 0x%08x", __atomic_load_n(&s_light_word, __ATOMIC_ACQUIRE));
}

/* The frame hook eases toward the word, solves once, and falls back to the
 * rest light when the data goes invalid. */
static void test_light_tick_eases_and_falls_back(void)
{
    hud_light_t rest;
    hud_light_solve(0, 0, &rest);
    s_light_fx_q8 = s_light_fy_q8 = 0;
    s_light_prev_ms = 1000;
    jr_display_set_light(0, -1000, true);
    uint32_t t = 1000;
    int last_dy = s_light.shadow_dy;
    for (int i = 0; i < 80; ++i) {
        t += 42;
        light_tick(t);
        CHECK(s_light.shadow_dy >= last_dy - 1, "shadow reversed while easing in (%d -> %d)",
              last_dy, s_light.shadow_dy);
        last_dy = s_light.shadow_dy;
        CHECK(s_light.shadow_dy <= 96, "shadow beyond 6 px: %d", s_light.shadow_dy);
    }
    CHECK(s_light_gy_mg == -1000 && s_light.shadow_dy == 96 && s_light.shadow_dx == 0,
          "settled at gy %d shadow %d,%d", s_light_gy_mg, s_light.shadow_dx, s_light.shadow_dy);

    jr_display_set_light(0, -1000, false);        /* IMU gone */
    for (int i = 0; i < 80; ++i) {
        t += 42;
        light_tick(t);
    }
    CHECK(light_eq_rest(&s_light, &rest), "invalid data did not return the rest light");

    jr_display_set_light(0, 0, true);
    t += 42;
    light_tick(t);
    CHECK(light_eq_rest(&s_light, &rest), "flat is not the rest light");
}

/* ---- TILT PARALLAX in the render task -------------------------------------
 * The diag word's layout, and the one rule that matters for power and for the
 * sensor: the source is asked only while CELESTIAL is the watch on a lit panel
 * at the awake cadence. */
static int s_tilt_calls;
static int s_tilt_raw_x, s_tilt_raw_y;
static bool s_tilt_have = true;

static bool tilt_test_source(int *x, int *y)
{
    s_tilt_calls++;
    *x = s_tilt_raw_x;
    *y = s_tilt_raw_y;
    return s_tilt_have;
}

static void test_tilt_word_layout_is_pinned(void)
{
    __atomic_store_n(&s_tilt_word, jr_tilt_pack(-1, 1), __ATOMIC_RELAXED);
    CHECK(__atomic_load_n(&s_tilt_word, __ATOMIC_RELAXED) == 0x0001FFFFu,
          "tilt word layout drifted: 0x%08x", __atomic_load_n(&s_tilt_word, __ATOMIC_RELAXED));
    jr_tilt_t t;
    jr_tilt_reset(&t);
    t.x = -256; t.y = 255;
    __atomic_store_n(&s_tilt_word, jr_tilt_pack(t.x, t.y), __ATOMIC_RELAXED);
    jr_display_diag_t d;
    memset(&d, 0, sizeof d);
    jr_display_get_diag(&d);
    CHECK(d.tilt_x == -256 && d.tilt_y == 255, "diag carries %d,%d", (int)d.tilt_x, (int)d.tilt_y);
    __atomic_store_n(&s_tilt_word, 0u, __ATOMIC_RELAXED);
}

static void test_tilt_is_read_only_on_a_lit_awake_celestial(void)
{
    const uint8_t style_was = s_style_shown;
    const uint32_t panel_was = __atomic_load_n(&s_panel_off, __ATOMIC_ACQUIRE);
    const uint8_t fps_was = jr_display_render_fps();
    jr_display_set_tilt_source(tilt_test_source);
    __atomic_store_n(&s_panel_off, 0u, __ATOMIC_RELEASE);
    jr_display_set_render_fps(JR_DISPLAY_RENDER_FPS);
    s_tilt_calls = 0;
    s_tilt_have = true;
    s_tilt_raw_x = 0; s_tilt_raw_y = 0;
    s_tilt_prev_ms = 5000;

    /* another dial: never asked, never leans */
    s_style_shown = (uint8_t)JR_WATCH_JARVIS;
    uint32_t t = 5000;
    for (int i = 0; i < 5; ++i) { t += 42; tilt_tick(t); }
    CHECK(s_tilt_calls == 0, "the sensor was read %d times off CELESTIAL", s_tilt_calls);

    /* CELESTIAL, lit, awake: asked once a frame, leans, then comes back */
    s_style_shown = (uint8_t)JR_WATCH_CELESTIAL;
    t += 42; tilt_tick(t);
    CHECK(s_tilt_calls == 1 && s_tilt.x == 0, "first frame: %d calls, lean %d", s_tilt_calls, s_tilt.x);
    s_tilt_raw_x = 200;
    t += 42; tilt_tick(t);
    CHECK(s_tilt_calls == 2 && s_tilt.x > 40, "a lean did not register: %d calls, lean %d", s_tilt_calls, s_tilt.x);
    CHECK(jr_tilt_unpack_x(__atomic_load_n(&s_tilt_word, __ATOMIC_RELAXED)) == s_tilt.x, "diag word is not the lean");
    for (int i = 0; i < 400; ++i) { t += 42; tilt_tick(t); }
    CHECK(s_tilt.x == 0, "a held pose did not re-centre: %d", s_tilt.x);

    /* a lean in flight, then each gate closes in turn: no read, lean gone */
    s_tilt_raw_x = -300;
    t += 42; tilt_tick(t);
    CHECK(s_tilt.x < 0, "no lean to drop");
    int calls = s_tilt_calls;
    jr_display_set_render_fps(6);                          /* resting cadence */
    t += 42; tilt_tick(t);
    CHECK(s_tilt_calls == calls && s_tilt.x == 0 && s_tilt.y == 0, "a resting watch read the sensor or kept a lean");
    jr_display_set_render_fps(JR_DISPLAY_RENDER_FPS);
    t += 42; tilt_tick(t);
    t += 42; tilt_tick(t);
    CHECK(s_tilt_calls == calls + 2 && s_tilt.x == 0, "reopening the gate did not reseed at rest: %d calls, lean %d",
          s_tilt_calls - calls, s_tilt.x);
    calls = s_tilt_calls;
    __atomic_store_n(&s_panel_off, 2u, __ATOMIC_RELEASE);  /* panel off */
    t += 42; tilt_tick(t);
    CHECK(s_tilt_calls == calls && s_tilt.x == 0, "a dark panel read the sensor or kept a lean");
    __atomic_store_n(&s_panel_off, 0u, __ATOMIC_RELEASE);
    s_tilt_have = false;                                   /* sensor has no answer */
    t += 42; tilt_tick(t);
    CHECK(s_tilt.x == 0 && s_tilt.y == 0, "no data still leaned");
    jr_display_set_tilt_source(NULL);
    s_tilt_have = true;
    calls = s_tilt_calls;
    t += 42; tilt_tick(t);
    CHECK(s_tilt_calls == calls && s_tilt.x == 0, "a removed source was called");

    s_style_shown = style_was;
    __atomic_store_n(&s_panel_off, panel_was, __ATOMIC_RELEASE);
    jr_display_set_render_fps(fps_was);
    jr_tilt_reset(&s_tilt);
    __atomic_store_n(&s_tilt_word, 0u, __ATOMIC_RELAXED);
}

/* ------------------------------------------- data screens on true black -- */

static uint32_t s_pk_now = 100000U;

static void pk_tick(void)
{
    s_pk_now += 70U;
    sp_fade_tick(s_pk_now, 70U, 8);
    presenter_backdrop_tick(&s_display);
}

static void pk_settle(void)
{
    for (int i = 0; i == 0 || (i < 80 && s_space_prog < 256); ++i) {
        pk_tick();
    }
    pk_tick();
}

static void pk_reset(void)
{
    jr_display_ctx_t *ctx = &s_display;
    reset_nav();
    s_parked = false;
    __atomic_store_n(&s_park_want, 0, __ATOMIC_RELEASE);
    s_space_black = 0;
    s_ask_ease = 0;
    s_canvas_ease = 0;
    s_dial_blit = false;
    ctx->dream_active = false;
    ctx->quiet_active = false;
    __atomic_store_n(&ctx->blanked, false, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->requested_word, 0U, __ATOMIC_RELEASE);
    s_stub_lock_failures = 0;
    s_stub_anim_visible = false;
    s_space_from = s_space_to = (uint8_t)JR_DISPLAY_SPACE_JARVIS;
    s_space_serial_seen = 0;
    s_space_prog = s_space_ease = 256;
    s_detail_prog = s_detail_ease = 0;
    s_shade_prog = s_shade_ease = 0;
    pk_tick();
}

static void pk_request(jr_face_t face)
{
    __atomic_store_n(&s_display.requested_word,
                     (uint32_t)face << JR_DISPLAY_CMD_FACE_SHIFT, __ATOMIC_RELEASE);
    bool stale = false;
    CHECK(apply_face(&s_display, face, face, 0U, &stale) == ESP_OK && !stale,
          "face %d applies", (int)face);
}

static void test_veil_reaches_black_on_the_way_in_and_back_on_the_way_out(void)
{
    pk_reset();
    CHECK(s_space_black == 0, "JARVIS at rest asks for no black");
    jr_display_nav_set(JR_DISPLAY_SPACE_STATUS);
    int prev = -1, mid = 0;
    for (int i = 0; i == 0 || (i < 80 && s_space_prog < 256); ++i) {
        pk_tick();
        CHECK(s_space_black >= prev, "black only deepens on the way in");
        prev = s_space_black;
        if (s_space_black > 0 && s_space_black < 256) { mid++; }
    }
    CHECK(mid > 0, "the way in is a fade, not a cut");
    CHECK(s_space_black == 256, "the slide ends at full black, got %d", s_space_black);

    jr_display_nav_set(JR_DISPLAY_SPACE_JARVIS);
    pk_tick();
    /* leaving a data screen the glass is dark until the cut, then lifts: the
     * face is not the first thing back, and not a frame later than the cut */
    CHECK(s_space_black <= 256, "black is a share of the glass");
    prev = s_space_black;
    for (int i = 0; i == 0 || (i < 80 && s_space_prog < 256); ++i) {
        pk_tick();
        CHECK(s_space_black <= prev, "black only lifts on the way home");
        prev = s_space_black;
    }
    CHECK(s_space_black == 0, "the face is back in full, got %d", s_space_black);
    pk_reset();
}

static void test_black_is_pixels_and_a_parked_backdrop_skips_the_fold(void)
{
    pk_reset();
    s_display.board.swap_color_bytes = false;
    static uint16_t full[HUD_W * STRIP_ROWS], half[HUD_W * STRIP_ROWS];
    const int y1 = 227 - STRIP_ROWS / 2;
    for (size_t i = 0; i < sizeof full / sizeof *full; ++i) {
        full[i] = half[i] = 0xFFFFu;
    }
    sp_veil_black(&s_display, y1, y1 + STRIP_ROWS, full, 32);
    sp_veil_black(&s_display, y1, y1 + STRIP_ROWS, half, 16);
    CHECK(full[(STRIP_ROWS / 2) * HUD_W + 233] == 0U, "full black clears the disc");
    CHECK(full[(STRIP_ROWS / 2) * HUD_W + 2] == 0xFFFFu, "and spares the rim outside it");
    const uint16_t h = half[(STRIP_ROWS / 2) * HUD_W + 233];
    CHECK(h != 0U && h != 0xFFFFu, "half way is dimmer, not gone (0x%04x)", h);
    CHECK(sp_fade_to_black565(0xFFFFu, false, 0) == 0xFFFFu, "k=0 is the identity");
    CHECK(sp_fade_to_black565(0xFFFFu, true, 32) == 0U, "k=32 is black");

    /* apply_space_overlay: black not yet parked folds the face to zero; parked
     * leaves the (already black) strip alone, so the same screen draws the same
     * with or without the black term. */
    stage_space(JR_DISPLAY_SPACE_STATUS);
    s_space_veil = 0;
    static uint16_t a[HUD_W * STRIP_ROWS], b[HUD_W * STRIP_ROWS], c[HUD_W * STRIP_ROWS];
    for (size_t i = 0; i < sizeof a / sizeof *a; ++i) { a[i] = b[i] = c[i] = 0xFFFFu; }
    s_space_black = 0;
    s_parked = false;
    apply_space_overlay(&s_display, y1, y1 + STRIP_ROWS, a);
    s_space_black = 256;
    apply_space_overlay(&s_display, y1, y1 + STRIP_ROWS, b);
    s_parked = true;
    apply_space_overlay(&s_display, y1, y1 + STRIP_ROWS, c);
    CHECK(memcmp(a, c, sizeof a) == 0, "parked: no fold touches the strip");
    CHECK(memcmp(a, b, sizeof a) != 0, "unparked at full black the face is cleared");
    pk_reset();
}

/* The part-strength veil and black fold remember the last pixel they folded. Pinned
 * against a per-pixel fold written here: runs of one colour, zeros between them, and
 * neighbours that differ by one bit, for both byte orders and several strengths. */
static void test_the_veil_memo_folds_like_a_per_pixel_fold(void)
{
    pk_reset();
    static uint16_t src[HUD_W * STRIP_ROWS], got[HUD_W * STRIP_ROWS];
    const int y1 = 227 - STRIP_ROWS / 2;
    for (size_t i = 0; i < sizeof src / sizeof *src; ++i) {
        const uint32_t x = (uint32_t)(i % HUD_W), r = (uint32_t)(i / HUD_W);
        const uint32_t seg = (x / 7u + r) % 6u;
        static const uint16_t pal[6] = {0xFFFFu, 0x0000u, 0xF81Fu, 0xF81Eu, 0x07E0u, 0x1234u};
        src[i] = pal[seg];
    }
    int checked = 0;
    for (int sw = 0; sw < 2; ++sw) {
        s_display.board.swap_color_bytes = sw != 0;
        for (int k = 1; k < 32; k += 5) {
            for (int which = 0; which < 2; ++which) {
                memcpy(got, src, sizeof got);
                if (which == 0) {
                    sp_veil(&s_display, y1, y1 + STRIP_ROWS, got, k);
                } else {
                    sp_veil_black(&s_display, y1, y1 + STRIP_ROWS, got, k);
                }
                for (int y = y1; y < y1 + STRIP_ROWS; ++y) {
                    int lo = 0, hi = HUD_W - 1;
                    const bool in = sp_clip(y, &lo, &hi);
                    for (int x = 0; x < HUD_W; ++x) {
                        const size_t i = (size_t)(y - y1) * HUD_W + (size_t)x;
                        uint16_t want = src[i];
                        if (in && x >= lo && x <= hi && want != 0U) {
                            want = which == 0 ? hud_fade565(want, sw != 0, k)
                                              : sp_fade_to_black565(want, sw != 0, k);
                        }
                        if (got[i] != want) {
                            CHECK(false, "veil %d swap %d k %d (%d,%d): 0x%04x, want 0x%04x",
                                  which, sw, k, x, y, got[i], want);
                            pk_reset();
                            return;
                        }
                        ++checked;
                    }
                }
            }
        }
    }
    CHECK(checked > 100000, "checked %d pixels", checked);
    s_display.board.swap_color_bytes = false;
    pk_reset();
}

static void test_the_face_parks_only_when_settled_on_a_data_screen(void)
{
    pk_reset();
    CHECK(!s_parked && s_park_want == 0, "JARVIS does not park");

    jr_display_nav_set(JR_DISPLAY_SPACE_STATUS);
    bool parked_mid_slide = false;
    for (int i = 0; i == 0 || (i < 80 && s_space_prog < 256); ++i) {
        pk_tick();
        parked_mid_slide |= s_parked && s_space_prog < 256;
    }
    CHECK(!parked_mid_slide, "a slide in progress does not park");
    pk_tick();
    CHECK(s_parked && !s_stub_anim_visible, "settled at full black: parked, anim hidden");

    /* each data screen, reached from a data screen, stays parked */
    const unsigned stops = s_stub_anim_stops;
    static const jr_display_space_t data[] = {
        JR_DISPLAY_SPACE_WEATHER, JR_DISPLAY_SPACE_ACTIVITY, JR_DISPLAY_SPACE_STATUS };
    for (size_t k = 0; k < sizeof data / sizeof *data; ++k) {
        jr_display_nav_set(data[k]);
        for (int i = 0; i == 0 || (i < 80 && s_space_prog < 256); ++i) {
            pk_tick();
            CHECK(s_parked && s_space_black == 256 && !s_stub_anim_visible,
                  "between data screens the backdrop stays black and parked");
        }
    }
    CHECK(s_stub_anim_stops == stops, "and the engine is not stopped again");

    jr_display_nav_set(JR_DISPLAY_SPACE_JARVIS);
    pk_settle();
    CHECK(!s_parked && s_park_want == 0, "home unparks");

    /* a sheet or the shade over JARVIS keeps today's veil and the face */
    jr_display_nav_up();
    pk_settle();
    CHECK(jr_display_nav_space() == JR_DISPLAY_SPACE_JARVIS && s_detail_ease > 0 &&
          !s_parked && s_space_black == 0, "a sheet over JARVIS does not park");
    jr_display_nav_home();
    pk_settle();
    jr_display_nav_down();
    pk_settle();
    CHECK(jr_display_nav_space() == JR_DISPLAY_SPACE_JARVIS && !s_parked &&
          s_space_black == 0, "the shade over JARVIS does not park");
    pk_reset();
}

/* ---- motion: the shade's two tempos, the caption's life, the label swap ---- */

/* One 24 fps frame: the render task's own step, with the presenter after it. */
#define MT_FRAME_MS 42U
static void mt_frame(void)
{
    s_fake_us += (int64_t)MT_FRAME_MS * 1000;
    overlay_fade_tick();                 /* sp_fade_tick and caption_tick, as panel_flush runs them */
    presenter_backdrop_tick(&s_display);
}

static void mt_settle(void)
{
    for (int i = 0; i < 80; ++i) { mt_frame(); }
}

static void test_the_shade_opens_in_about_eleven_frames_and_closes_in_about_seven(void)
{
    pk_reset();
    s_fade_prev_ms = (uint32_t)(s_fake_us / 1000);
    mt_settle();
    jr_display_nav_down();                               /* the shade over JARVIS */
    int open_frames = 0;
    do { mt_frame(); ++open_frames; } while (open_frames < 60 && s_shade_ms < SP_SHADE_ENTRY_MS);
    CHECK(open_frames >= 10 && open_frames <= 12,
          "the shade's authored timeline plays out in 10 to 12 frames, got %d", open_frames);
    mt_settle();
    CHECK(s_shade_prog == 256, "the shade is fully up");
    jr_display_nav_up();                                 /* either vertical leaves it */
    int close_frames = 0;
    while (close_frames < 60 && s_shade_prog > 0) { mt_frame(); ++close_frames; }
    CHECK(close_frames >= 7 && close_frames <= 8,
          "the shade closes in 7 or 8 frames, got %d", close_frames);
    CHECK(close_frames < open_frames, "it leaves faster than it came");
    pk_reset();
}

static int mt_frames_until_caption_clears(jr_display_label_kind_t kind, const char *text)
{
    jr_display_caption_label(text, kind);
    mt_frame();                          /* the render task takes the staged words */
    for (int i = 0; i < 200 && s_cap_p < 256; ++i) { mt_frame(); }
    CHECK(s_cap_hold_ms != 0U, "a label carries a hold");
    int n = 0;
    while (n < 400 && s_cap_hold_ms != 0U) { mt_frame(); ++n; }
    return n;
}

static void test_a_label_is_up_for_its_hold_after_settle_and_then_fades(void)
{
    pk_reset();
    mt_settle();
    jr_display_caption_clear();
    mt_settle();
    int n = mt_frames_until_caption_clears(JR_DISPLAY_LABEL_SCREEN, "WEATHER");
    CHECK(n * (int)MT_FRAME_MS >= 600 - (int)MT_FRAME_MS &&
          n * (int)MT_FRAME_MS < 600 + 2 * (int)MT_FRAME_MS,
          "a screen name stays 600 ms after it settles, got %d ms", n * (int)MT_FRAME_MS);
    int f = 0;
    while (f < 60 && s_cap_alpha > 0) { mt_frame(); ++f; }
    CHECK(f >= 4 && f <= 8, "and fades in about six frames, got %d", f);
    mt_settle();
    n = mt_frames_until_caption_clears(JR_DISPLAY_LABEL_WATCH_STYLE, "WATCH - DRESS");
    CHECK(n * (int)MT_FRAME_MS >= 1200 - (int)MT_FRAME_MS &&
          n * (int)MT_FRAME_MS < 1200 + 2 * (int)MT_FRAME_MS,
          "a style name stays 1200 ms, got %d ms", n * (int)MT_FRAME_MS);
    pk_reset();
}

static void test_no_caption_is_drawn_while_a_sheet_is_open(void)
{
    pk_reset();
    s_fade_prev_ms = (uint32_t)(s_fake_us / 1000);
    mt_settle();
    jr_display_caption_label("WEATHER", JR_DISPLAY_LABEL_SCREEN);
    mt_frame();
    CHECK(!caption_hidden_by_sheet(), "a caption on a bare screen is drawn");
    jr_display_nav_down();
    for (int i = 0; i < 3; ++i) { mt_frame(); }
    CHECK(caption_hidden_by_sheet(), "no caption once the shade has begun to open");
    mt_settle();
    jr_display_nav_up();
    mt_settle();
    CHECK(!caption_hidden_by_sheet(), "and none is hidden after it has closed");
    jr_display_nav_up();                                 /* the detail sheet */
    for (int i = 0; i < 3; ++i) { mt_frame(); }
    CHECK(caption_hidden_by_sheet(), "no caption over the detail sheet either");
    pk_reset();
}

static void test_a_new_label_swaps_in_at_the_cut_and_not_before(void)
{
    pk_reset();
    s_fade_prev_ms = (uint32_t)(s_fake_us / 1000);
    mt_settle();
    jr_display_caption_label("WEATHER", JR_DISPLAY_LABEL_SCREEN);
    /* up and still held (600 ms after it settles): not the 80 frames the test
     * once waited, after which the name had timed out and been gone for ages */
    for (int i = 0; i < 12; ++i) { mt_frame(); }
    CHECK(strcmp(s_capd_line[0], "WEATHER") == 0 && s_cap_alpha > 200 && s_cap_hold_ms != 0U,
          "the old name is on the glass and held (alpha %d)", s_cap_alpha);
    jr_display_caption_label("STATUS", JR_DISPLAY_LABEL_SCREEN);
    int swap_at = -1, cut_at = -1;
    for (int i = 0; i < 40 && swap_at < 0; ++i) {
        mt_frame();
        if (cut_at < 0 && s_cap_p >= TR_CUT_SLIDE) { cut_at = i; }
        if (strcmp(s_capd_line[0], "STATUS") == 0) { swap_at = i; }
    }
    CHECK(swap_at >= 2, "the name does not swap in the first frames, got frame %d", swap_at);
    CHECK(swap_at == cut_at, "the swap happens on the frame that crosses the cut (%d vs %d)",
          swap_at, cut_at);
    pk_reset();
}

/* ---- a style change on a settled WATCH ---- */

/* The presenter as the preview models it: the dial it binds is the one the
 * compositor asked for on its previous frame. */
static jr_face_t s_sd_prev_want = JR_FACE_COUNT;
static void sd_frame(void)
{
    mt_frame();
    const jr_face_t was = s_sd_prev_want;
    if (was != JR_FACE_COUNT) {
        s_dial_blit = true;
        s_dial_frame_face = was;
    } else {
        s_dial_blit = false;
    }
    s_sd_prev_want = watch_dial_face();
}

static void sd_settled_watch(int style, bool with_caption)
{
    pk_reset();
    s_fade_prev_ms = (uint32_t)(s_fake_us / 1000);
    jr_display_caption_clear();
    for (int i = 0; i < 40; ++i) { mt_frame(); }          /* no earlier test's name is up */
    jr_display_clock_set(true, 6, 30, 15);
    jr_display_watch_style_set((jr_watch_style_t)style);
    jr_display_nav_set(JR_DISPLAY_SPACE_WATCH);
    s_sd_prev_want = JR_FACE_COUNT;
    for (int i = 0; i < 100; ++i) { sd_frame(); }
    if (with_caption) {
        jr_display_caption_label("WATCH - DRESS", JR_DISPLAY_LABEL_WATCH_STYLE);
        sd_frame();                                   /* the render task takes the staged words */
        for (int i = 0; i < 200 && s_cap_hold_ms != 0U; ++i) { sd_frame(); }
        for (int i = 0; i < 30; ++i) { sd_frame(); }
    } else {
        jr_display_caption_clear();
        for (int i = 0; i < 30; ++i) { sd_frame(); }
    }
}

/* A style change on a settled WATCH, with the presenter modelled as the preview
 * does. The new dial eases in over the full five frames (the cut waits for the
 * layer to bind instead of the bind eating the ease), and a name that has
 * already timed out does not come back for the dip. */
static void test_a_style_change_eases_the_new_dial_in_over_five_frames(void)
{
    static const int pairs[][2] = { { JR_WATCH_DIVER, JR_WATCH_DRESS }, { JR_WATCH_DRESS, JR_WATCH_PILOT },
                                    { JR_WATCH_MINIMAL, JR_WATCH_FUTURE } };
    for (int k = 0; k < 3; ++k) {
        sd_settled_watch(pairs[k][0], false);
        jr_display_watch_style_step(1);
        CHECK(jr_display_watch_style() == (jr_watch_style_t)pairs[k][1], "step lands on style %d", pairs[k][1]);
        int layer_steps = 0, first = -1, prev = 0, hands_steps = 0, frames = 0;
        bool monotone = true;
        sd_frame();                                    /* the render task sees the new style */
        for (int i = 0; i < 40 && s_style_prog < 256; ++i, ++frames) {
            sd_frame();
            const int layer = 256 - s_style_black;
            if (s_style_prog > TR_CUT_STYLE && layer > 0 && layer < 250) {
                if (first < 0) first = layer;
                if (layer < prev) monotone = false;
                prev = layer;
                layer_steps++;
            }
            if (s_style_prog > TR_CUT_STYLE && s_watch_s > 0 && s_watch_s < 250) hands_steps++;
        }
        CHECK(s_style_prog == 256, "pair %d: the change finished", k);
        CHECK(hands_steps >= 3, "pair %d: the hands ease in over %d partial frames, wanted 3 or more", k, hands_steps);
        if (watch_dial_of_style((uint8_t)pairs[k][1]) != JR_FACE_COUNT) {
            CHECK(layer_steps >= 3, "pair %d: the new dial shows %d partial frames, wanted 3 or more", k, layer_steps);
            CHECK(first >= 0 && first < 160, "pair %d: the dial's first visible frame is %d of 256", k, first);
            CHECK(monotone, "pair %d: the dial's ease went backwards", k);
        }
        CHECK(frames <= 16, "pair %d: the change took %d frames", k, frames);
    }
    pk_reset();
    jr_display_caption_clear();
    for (int i = 0; i < 40; ++i) { mt_frame(); }
}

static void test_a_name_that_has_timed_out_does_not_return_for_the_dip(void)
{
    sd_settled_watch(JR_WATCH_PILOT, true);            /* DRESS up, expired, faded away */
    CHECK(s_cap_alpha == 0 && s_caption_prog == 0, "the old name is gone before the change (alpha %d)", s_cap_alpha);
    jr_display_watch_style_step(1);
    jr_display_caption_label("WATCH - MINIMAL", JR_DISPLAY_LABEL_WATCH_STYLE);
    int seen_old = 0, seen_new = 0;
    for (int i = 0; i < 20; ++i) {
        sd_frame();
        if (s_cap_alpha > 0 && strstr(s_capd_line[0], "DRESS") != NULL) seen_old++;
        if (s_cap_alpha > 0 && strstr(s_capd_line[0], "MINIMAL") != NULL) seen_new++;
    }
    CHECK(seen_old == 0, "the expired name was drawn again for %d frames", seen_old);
    CHECK(seen_new >= 6, "the new name shows for %d frames", seen_new);
    /* a name that is still held does ease out where it stands */
    sd_settled_watch(JR_WATCH_PILOT, false);
    jr_display_caption_label("WATCH - PILOT", JR_DISPLAY_LABEL_WATCH_STYLE);
    for (int i = 0; i < 12; ++i) sd_frame();
    jr_display_watch_style_step(1);
    jr_display_caption_label("WATCH - MINIMAL", JR_DISPLAY_LABEL_WATCH_STYLE);
    sd_frame();
    CHECK(strstr(s_capd_line[0], "PILOT") != NULL && s_cap_alpha > 0, "a held name eases out first (alpha %d)", s_cap_alpha);
    pk_reset();
    jr_display_caption_clear();
    for (int i = 0; i < 40; ++i) { mt_frame(); }
}

static void test_a_baked_dial_watch_never_parks(void)
{
    pk_reset();
    jr_display_clock_set(true, 6, 30, 15);
    jr_display_watch_style_set(JR_WATCH_DIVER);
    jr_display_nav_set(JR_DISPLAY_SPACE_WATCH);
    pk_settle();
    CHECK(watch_dial_face() == JR_FACE_DIAL_DIVER && !s_parked && s_park_want == 0 &&
          s_space_black == 0, "a dial on WATCH is the screen, not a black backdrop");
    jr_display_watch_style_set(JR_WATCH_MINIMAL);
    pk_settle();
    CHECK(!s_parked && s_space_black == 0, "WATCH keeps today's veil in every style");
    jr_display_clock_set(false, 0, 0, 0);
    pk_reset();
}

static void test_other_owners_of_the_backdrop_withdraw_the_park(void)
{
    pk_reset();
    jr_display_nav_set(JR_DISPLAY_SPACE_WEATHER);
    pk_settle();
    CHECK(s_parked, "parked on WEATHER");
    s_ask_ease = 40;
    pk_tick();
    CHECK(!s_parked, "an ask over the backdrop brings the face back");
    s_ask_ease = 0;
    pk_tick();
    CHECK(s_parked, "and it parks again once the ask is gone");
    s_canvas_ease = 12;
    pk_tick();
    CHECK(!s_parked, "a canvas fade does too");
    s_canvas_ease = 0;
    pk_reset();
}

static void test_a_face_requested_while_parked_is_the_face_that_comes_back(void)
{
    jr_display_ctx_t *ctx = &s_display;
    pk_reset();
    static uint8_t blob[JR_FACE_COUNT][16];
    for (int f = 0; f < (int)JR_FACE_COUNT; ++f) {
        ctx->clips[f] = (jr_display_clip_t){ blob[f], 16U, 8U, 7U, false };
    }
    ctx->missing_faces = 0;
    ctx->loaded_face = JR_FACE_COUNT;
    ctx->active = NULL;
    pk_request(JR_FACE_IDLE);
    CHECK(s_stub_anim_visible, "unparked: applying a face shows the anim");

    jr_display_nav_set(JR_DISPLAY_SPACE_STATUS);
    pk_settle();
    CHECK(s_parked && !s_stub_anim_visible, "parked and hidden on STATUS");

    const unsigned starts = s_stub_anim_starts;
    pk_request(JR_FACE_LISTENING);
    CHECK(ctx->shown_face == JR_FACE_LISTENING && s_stub_anim_source == blob[JR_FACE_LISTENING],
          "a face requested while parked is bound at once");
    pk_request(JR_FACE_SPEAKING);
    CHECK(ctx->shown_face == JR_FACE_SPEAKING && s_stub_anim_source == blob[JR_FACE_SPEAKING],
          "and the latest request wins");
    CHECK(!s_stub_anim_visible && s_stub_anim_starts == starts,
          "but binding it does not start or show the anim");

    jr_display_nav_set(JR_DISPLAY_SPACE_JARVIS);
    s_pk_now += 70U;
    sp_fade_tick(s_pk_now, 70U, 8);
    CHECK(s_park_want == 0, "the first frame home withdraws the park");
    presenter_backdrop_tick(ctx);
    CHECK(!s_parked && s_stub_anim_visible && s_stub_anim_starts == starts + 1U,
          "the anim runs again before the slide has moved");
    CHECK(ctx->shown_face == JR_FACE_SPEAKING &&
          s_stub_anim_source == blob[JR_FACE_SPEAKING],
          "and the face is the one requested now, not the one at entry");
    pk_reset();
}

static void test_park_yields_to_blank_to_a_failed_lock_and_to_a_dial(void)
{
    jr_display_ctx_t *ctx = &s_display;
    pk_reset();
    s_stub_lock_failures = 1;               /* the first lock the park takes fails */
    jr_display_nav_set(JR_DISPLAY_SPACE_STATUS);
    for (int i = 0; i == 0 || (i < 80 && s_space_prog < 256); ++i) { pk_tick(); }
    CHECK(!s_parked && s_stub_lock_failures == 0 && s_park_want == 1,
          "the park is wanted and the lock was refused");
    pk_tick();
    CHECK(s_parked, "a failed lock is retried on the next pass");

    __atomic_store_n(&ctx->blanked, true, __ATOMIC_RELAXED);
    s_stub_anim_visible = false;
    presenter_backdrop_tick(ctx);
    CHECK(!s_parked && !s_stub_anim_visible, "a blanked panel unparks without showing the face");
    __atomic_store_n(&ctx->blanked, false, __ATOMIC_RELAXED);
    presenter_backdrop_tick(ctx);
    CHECK(s_parked, "and the screen parks again after the resume");

    /* parked STATUS -> WATCH with a dial: no flash of the anim on the way */
    jr_display_clock_set(true, 6, 30, 15);
    jr_display_watch_style_set(JR_WATCH_DIVER);
    jr_display_nav_set(JR_DISPLAY_SPACE_WATCH);
    pk_settle();
    /* the anim may be up under the cover while the entry is dark; the wanted dial
     * is what the presenter binds, and the layer stays covered until it has */
    CHECK(!s_parked && watch_dial_face() == JR_FACE_DIAL_DIVER,
          "unparking toward a baked dial asks the presenter for that dial");
    jr_display_clock_set(false, 0, 0, 0);
    pk_reset();
}

static void test_snapshot_mirror_blacks_out_the_corners(void)
{
    enum { W = 466, H = 466, STRIP = 12 };
    const jr_display_ctx_t saved = s_display;
    uint16_t *mirror = malloc((size_t)W * H * sizeof(uint16_t));
    uint16_t *other = malloc((size_t)W * H * sizeof(uint16_t));
    uint16_t *strip = malloc((size_t)W * STRIP * sizeof(uint16_t));
    CHECK(mirror && other && strip, "mirror test buffers");
    for (size_t i = 0; i < (size_t)W * H; ++i) { mirror[i] = 0x1234; other[i] = 0x1234; }
    for (size_t i = 0; i < (size_t)W * STRIP; ++i) { strip[i] = 0xAAAA; }
    s_display.board.width = W;
    s_display.board.height = H;
    s_display.snapshot_capture_rgb565 = mirror;
    s_display.snapshot_rgb565 = other;
    s_display.snapshot_initialized = 1U;
    s_display.snapshot_consumer_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_display.snapshot_frame_dropped = 0U;
    s_display.snapshot_next_y = 0U;
    for (int y = 0; y < H; y += STRIP) {
        const int y2 = y + STRIP < H ? y + STRIP : H;
        snapshot_record_flush(&s_display, 0, y, W, y2, strip);
    }
    CHECK(s_display.snapshot_valid && s_display.snapshot_rgb565 == mirror,
          "the full frame was published");
    int wrong = 0, lit = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const long dx = 2L * x + 1 - W, dy = 2L * y + 1 - H;
            const bool inside = dx * dx + dy * dy <= (long)W * W;
            const uint16_t want = inside ? 0xAAAA : 0;
            lit += inside;
            wrong += mirror[(size_t)y * W + x] != want;
        }
    }
    CHECK(wrong == 0 && lit > 0, "outside the glass radius the mirror is black, inside it is the strip");
    CHECK(mirror[0] == 0 && mirror[W - 1] == 0 && mirror[(size_t)(H - 1) * W] == 0 &&
          mirror[(size_t)H * W - 1] == 0, "the four corner pixels are black");
    free(mirror); free(other); free(strip);
    s_display = saved;
}

int main(void)
{
    test_light_word_round_trips();
    test_tilt_word_layout_is_pinned();
    test_tilt_is_read_only_on_a_lit_awake_celestial();
    test_light_tick_eases_and_falls_back();
    test_every_face_has_a_clip_and_a_hud_face();
    test_space_ring_wraps_both_ways();
    test_overlay_axis_is_unambiguous();
    test_sideways_resets_and_home_escapes();
    test_transition_publishes_a_new_serial();
    test_legacy_shade_bit_still_opens_the_shade();

    test_ota_word_clamps_everything();
    test_ota_arc_semantics();
    test_ota_names_fit_their_column();
    test_status_sheet_is_the_device_in_nine_rows();
    test_status_headline_says_the_worst_thing_first();
    test_status_face_follows_the_links();
    test_shade_readouts_survive_at_100();

    test_never_leaves_shell_radius();
    test_ota_ring_band();
    test_ota_ring_clears_the_headline();
    test_detail_sheet_rows_stay_inside();
    test_strip_invariance();
    test_jarvis_at_rest_draws_nothing();
    test_jarvis_at_rest_still_shows_the_update();
    test_update_ring_outlives_the_watch_clear();

    test_shade_hits_resolve_to_controls();
    test_shade_button_arcs_and_breath();
    test_shade_light_is_the_owner_setting();
    test_activity_fit_ends_on_a_sentence_or_an_ellipsis();
    test_shade_clocks_run_only_while_they_must();
    test_caption_band_gives_way_to_a_black_backdrop();

    test_every_space_composes_its_own_sheet();
    test_clock_clear_spares_open_shell_surfaces();
    test_watch_style_is_three_nav_bits_that_wrap();
    test_watch_dial_face_follows_the_style_and_the_screen();
    test_watch_keeps_a_baked_dial_and_clears_a_missing_one();
    test_watch_style_reaches_the_glass();
    test_future_weather_cell_shortens_instead_of_cutting();
    test_watch_words_are_composed_once_and_strip_invariant();
    test_watch_shadows_only_at_the_awake_cadence();
    test_watch_day_arc_is_the_sun_on_the_jarvis_dial_only();
    test_watch_dial_owns_the_glass_no_idle_rim();
    test_watch_jarvis_style_has_a_dial();
    test_shade_button_disc_hides_the_face();
    test_rest_face_is_one_face();
    test_rest_by_day_on_watch_keeps_the_dial();
    test_resting_dial_is_redrawn_only_when_the_minute_changes();
    test_resting_dial_has_no_seconds_hand();
    test_jarvis_dial_hands_arc_bead_and_date();

    test_desk_is_on_the_ring_only_while_live();
    test_desk_going_dark_moves_the_owner_on();
    test_orbit_mark_eases_when_desk_appears();
    test_weather_without_data_prints_no_number();
    test_stale_weather_loses_its_colour();
    test_weather_hours_ring_starts_now();
    test_icons_follow_the_sky_and_the_charger();
    test_entry_clock_runs_then_stops();
    test_entry_builds_the_screens();
    test_outgoing_screen_does_not_replay_the_entry();
    test_desk_is_a_glass_ring_with_its_number();
    test_activity_is_honest_when_empty_and_newest_first();
    test_activity_survives_the_sleep();
    test_desk_sheet_heads_with_the_marked_task();
    test_orbit_stays_in_free_band();
    test_low_battery_uses_the_rim_palette();
    test_focal_wedge_follows_the_slide();
    test_pinned_caption_survives_other_writers();
    test_render_cadence_reaches_the_engine_and_clamps();
    test_render_clock_counts_frames_and_latches_a_second();
    test_missing_clip_falls_back_to_the_face_it_grew_from();
    test_dial_decodes_once_and_blits_every_strip();
    test_dial_cache_a_b_a_decodes_each_once();
    test_dial_cache_old_dial_draws_while_the_new_one_decodes();
    test_dial_cache_warms_the_neighbour_only_when_idle();
    test_dial_cache_floor_eviction_and_the_shared_fallback();
    test_dial_cache_is_released_for_sleep_and_kept_on_the_glass();
    test_dream_intent_preserves_env_and_ignores_unchanged_publish();
    test_dream_never_hides_foreground_or_ota();
    test_dream_glass_is_black_clock_and_intact_rims();
    test_dream_stops_emote_and_refreshes_only_for_real_changes();
    test_dream_capture_panel_off_and_input_wake();
    test_panel_order_color_covers_both_wire_orders();
    test_refresh_failure_retries_keys_snapshot_and_pattern();
    test_finite_fades_use_elapsed_time_at_rest();
    test_dial_sources_release_without_invalidating_vendor_ownership();
    test_embedded_dials_are_read_only_and_never_freed();
    test_battery_rim_is_strip_invariant_at_every_percent();
    test_boot_timing_waits_for_a_real_completion();
    test_boot_timing_records_init_failure_and_early_callback();
    test_boot_timing_ignores_unusable_callbacks_and_unpublished_stamp();
    test_rest_mailboxes_survive_start_and_env_updates();
    test_boot_brightness_seed_precedes_first_transfer();
    test_panel_dip_dies_with_the_panel();
    test_panel_dip_composes_with_the_mood_slew();
    test_panel_dip_tail_is_rate_limited();
    test_rest_independently_refuses_exiting_foreground();
    test_rest_idle_listening_yields_immediately_to_voice_busy();
    test_power_hints_yield_only_after_plug_feedback();
    test_companion_sparse_refresh_and_retry();
    test_companion_actual_flush_preserves_rims_and_strip_order();
    test_panel_ack_requires_transport_settle_and_completed_recovery();
    test_panel_opposite_request_at_ack_boundary();
    test_panel_cancellation_failures_and_old_completions();
    test_glance_proxy_tracks_actual_successful_inflight_content();
    test_quiet_home_one_frame_and_sparse_facts();
    test_quiet_refuses_dynamic_owners_and_unfinished_fades();
    test_quiet_failures_retry_and_never_freeze_old_assets();
    test_actual_presenter_runs_quiet_not_repeat();

    test_veil_reaches_black_on_the_way_in_and_back_on_the_way_out();
    test_black_is_pixels_and_a_parked_backdrop_skips_the_fold();
    test_the_veil_memo_folds_like_a_per_pixel_fold();
    test_the_face_parks_only_when_settled_on_a_data_screen();
    test_a_baked_dial_watch_never_parks();
    test_the_shade_opens_in_about_eleven_frames_and_closes_in_about_seven();
    test_a_label_is_up_for_its_hold_after_settle_and_then_fades();
    test_watch_without_a_dial_asks_for_frames();
    test_a_style_change_eases_the_new_dial_in_over_five_frames();
    test_a_name_that_has_timed_out_does_not_return_for_the_dip();
    test_no_caption_is_drawn_while_a_sheet_is_open();
    test_a_new_label_swaps_in_at_the_cut_and_not_before();
    test_other_owners_of_the_backdrop_withdraw_the_park();
    test_a_face_requested_while_parked_is_the_face_that_comes_back();
    test_park_yields_to_blank_to_a_failed_lock_and_to_a_dial();
    test_snapshot_mirror_blacks_out_the_corners();

    if (g_failures) {
        printf("%d failure(s) of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("all shell tests passed (%d checks)\n", g_checks);
    return 0;
}
