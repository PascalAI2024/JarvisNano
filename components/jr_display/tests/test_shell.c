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
#include "jr_display.c"

#include <assert.h>
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

static void reset_nav(void)
{
    __atomic_store_n(&s_nav_word, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_display.shell_word, 0U, __ATOMIC_RELEASE);
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
    s_detail_space = JR_DISPLAY_SPACE_STATUS;
    s_detail_ease = overlay == JR_DISPLAY_OVERLAY_DETAIL ? 256 : 0;
    s_shade_ease = overlay == JR_DISPLAY_OVERLAY_SHADE ? 256 : 0;
    __atomic_store_n(&s_nav_word,
                     (uint32_t)JR_DISPLAY_SPACE_STATUS |
                         ((uint32_t)overlay << NAV_OVL_SHIFT),
                     __ATOMIC_RELEASE);
    jr_display_set_status(90U);
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

static void test_status_face_follows_the_links(void)
{
    stage_space(JR_DISPLAY_SPACE_STATUS);
    jr_display_power_set(74U, 4020U, true, true);
    const int lx0 = SP_CX - 60, lx1 = SP_CX + 60;
    const int ly0 = SP_ST_LAMP_Y, ly1 = SP_ST_LAMP_Y + 14;
    const int bx0 = SP_CX - 68, bx1 = SP_CX + 68;   /* inside the r76 chord */
    const int by0 = SP_ST_WIFI_Y, by1 = SP_ST_WIFI_Y + 14;

    links(true, -34, "198.51.100.7", true, 2U, false, false);
    uint16_t *fb = render_frame();
    if (!fb) {
        printf("FAIL %s: allocation failed\n", __func__);
        g_failures++;
        return;
    }
    const size_t lamps_on = count_in(fb, SP_C_INK, lx0, lx1, ly0, ly1);
    const size_t bars4 = count_in(fb, SP_C_CYAN, bx0, bx1, by0, by1);
    const size_t big = count_in(fb, SP_C_INK, SP_CX - 40, SP_CX + 40,
                                SP_FOCAL_TEXT_Y, SP_FOCAL_TEXT_Y + 21);
    free(fb);
    CHECK(lamps_on > 40, "both lamps lit draw ink, got %zu", lamps_on);
    CHECK(bars4 >= 4 * 4 + 7 * 4 + 10 * 4 + 13 * 4,
          "four bars of accent, got %zu", bars4);
    CHECK(big > 100, "the percentage is the big ink, got %zu", big);

    links(true, -80, "198.51.100.7", false, 0U, false, false);
    fb = render_frame();
    if (!fb) {
        return;
    }
    const size_t lamps_off = count_in(fb, SP_C_INK, lx0, lx1, ly0, ly1);
    const size_t bars1 = count_in(fb, SP_C_CYAN, bx0, bx1, by0, by1);
    free(fb);
    CHECK(lamps_off == 0, "dim lamps draw no ink, got %zu", lamps_off);
    CHECK(bars1 > 0 && bars1 < bars4, "one bar is fewer than four: %zu vs %zu",
          bars1, bars4);

    links(false, -34, "", false, 2U, false, false);
    fb = render_frame();
    if (!fb) {
        return;
    }
    const size_t bars0 = count_in(fb, SP_C_CYAN, bx0, bx1, by0, by1);
    const size_t down = count_in(fb, SP_C_GREY, bx0, bx1, by0, by1);
    free(fb);
    CHECK(bars0 == 0, "no wifi lights no bar, got %zu", bars0);
    CHECK(down > 20, "DOWN is written in grey, got %zu", down);

    /* Nothing of the face reaches the update ring's band: the lamps sit
     * under r140 by design, so a flash in progress never overdraws them. */
    links(true, -34, "198.51.100.7", true, 2U, false, false);
    fb = render_frame();
    if (!fb) {
        return;
    }
    size_t leaked = 0;
    for (int y = ly0; y < ly1; ++y) {
        for (int x = lx0; x < lx1; ++x) {
            const int dx = x - SP_CX, dy = y - SP_CY;
            if (fb[(size_t)y * HUD_W + x] == SP_C_INK &&
                sp_isqrt(dx * dx + dy * dy) >= SP_OTA_IN) {
                leaked++;
            }
        }
    }
    free(fb);
    CHECK(leaked == 0, "%zu lamp pixels reach the update ring", leaked);
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
    CHECK(strcmp(s_shade_vol, "L VOL 100%") == 0,
          "shade volume complete, got '%s'", s_shade_vol);
    CHECK(strcmp(s_shade_light, "R LGT 100%") == 0,
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

/* --------------------------------------------------------------- hit test -- */

static void test_shade_hits_resolve_to_controls(void)
{
    reset_nav();
    jr_display_nav_down();

    /* Left half decrements, right half increments, exactly as the drawn -/+
     * end caps promise. */
    CHECK(jr_display_hit(SP_CX + 140, SP_CY) == JR_DISPLAY_ACT_VOLUME_UP,
          "volume up");
    CHECK(jr_display_hit(SP_CX - 140, SP_CY) == JR_DISPLAY_ACT_VOLUME_DOWN,
          "volume down");
    CHECK(jr_display_hit(SP_BTN_PRIV_CX, SP_BTN_CY) ==
              JR_DISPLAY_ACT_PRIVACY_TOGGLE,
          "privacy");
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
            sp_compose();

            for (size_t i = 0; i < px; ++i) {
                fb[i] = POISON;
            }
            for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
                const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
                uint16_t *strip = fb + (size_t)y * HUD_W;
                apply_space_overlay(&s_display, y, y2, strip);
                if (pass == 1) {
                    apply_clock_overlay(&s_display, y, y2, strip);
                }
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
    for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
        if (fb[i] == POISON || (int)(i / HUD_W) >= SP_LABEL_Y) {
            continue;                 /* the headline rows are text, by design */
        }
        const int r = radius_of((int)i);
        /* The rain track's inner edge floors one unit under SP_WX_RAIN_IN,
         * as every annulus does; the disc proper starts inside that. */
        if (r < SP_WX_RAIN_IN - 1) {
            ++interior;
        } else if (r <= SP_WX_MARK_OUT) {
            if (fb[i] == SP_C_TRACK) {
                ++track;
            } else {
                ++nontrack;
            }
        }
    }
    CHECK(interior == 0, "%zu pixels inside the disc with no weather", interior);
    CHECK(nontrack == 0, "%zu coloured pixels on the gauge with no weather",
          nontrack);
    CHECK(track > 1000, "the bare tracks drew (%zu track pixels)", track);
    free(fb);
}

/* The mark is the temperature. 70 F is the middle of a 40..100 gauge that
 * opens at 7:30 and sweeps 270 degrees, so it sits at 12 o'clock exactly;
 * the ends pin to the ends. Measure the CENTROID of the mark-coloured pixels
 * rather than trust the angle arithmetic. */
static void test_weather_mark_sits_at_the_temperature(void)
{
    static const struct { int temp; int angle; } cases[] = {
        { 70, SP_A_TOP }, { 85, 240 }, { 40, SP_WX_A0 },
        { 100, SP_WX_A0 + SP_WX_SWEEP }, { -20, SP_WX_A0 },
    };
    for (size_t c = 0; c < sizeof cases / sizeof *cases; ++c) {
        stage_space(JR_DISPLAY_SPACE_WEATHER);
        set_weather(true, cases[c].temp, 100, 40, JR_DISPLAY_SKY_CLOUDS,
                    "OVERCAST", 0U);
        sp_compose();
        CHECK(s_wx_mark_a == cases[c].angle, "%d F composed at %d, wanted %d",
              cases[c].temp, s_wx_mark_a, cases[c].angle);

        uint16_t *fb = render_frame();
        if (!fb) {
            printf("FAIL %s: allocation failed\n", __func__);
            g_failures++;
            return;
        }
        long sx = 0, sy = 0, n = 0;
        for (size_t i = 0; i < (size_t)HUD_W * HUD_H; ++i) {
            if (fb[i] != SP_C_CYAN || radius_of((int)i) > SP_WX_MARK_OUT) {
                continue;             /* the orbit's mark is cyan too, at r187+ */
            }
            sx += (long)(i % HUD_W);
            sy += (long)(i / HUD_W);
            ++n;
        }
        CHECK(n > 40, "%d F: the mark drew only %ld pixels", cases[c].temp, n);
        if (n > 0) {
            const int a = cases[c].angle;
            const int R = (SP_WX_MARK_IN + SP_WX_MARK_OUT) / 2;
            const int ex = SP_CX + ((sp_cos(a) * R) >> 15);
            const int ey = SP_CY + ((sp_sin(a) * R) >> 15);
            const int cx = (int)(sx / n), cy = (int)(sy / n);
            CHECK(cx >= ex - 4 && cx <= ex + 4 && cy >= ey - 4 && cy <= ey + 4,
                  "%d F: mark centroid (%d,%d), expected (%d,%d)",
                  cases[c].temp, cx, cy, ex, ey);
        }
        free(fb);
    }
    /* The band is lo..hi on the same scale, and lo/hi order is not trusted. */
    set_weather(true, 80, 76, 86, JR_DISPLAY_SKY_CLOUDS, "OVERCAST", 0U);
    sp_compose();
    CHECK(s_wx_band_a0 == sp_wx_angle(76) &&
              s_wx_band_sweep == sp_wx_angle(86) - sp_wx_angle(76),
          "band from lo to hi regardless of order: a0 %d sweep %d",
          s_wx_band_a0, s_wx_band_sweep);
    CHECK(strcmp(s_wx_hilo, "H76 L86") == 0, "hi/lo line, got '%s'", s_wx_hilo);
    CHECK(strcmp(s_wx_head, "OVERCAST 80") == 0, "headline, got '%s'", s_wx_head);
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
        CHECK(count_color_within(fb, SP_C_AMBER, SP_WX_MARK_IN, SP_WX_MARK_OUT,
                                 HUD_H) > 40,
              "a fresh CLEAR day draws its mark in amber");
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
        CHECK(count_color_within(fb, SP_C_GREY, SP_WX_MARK_IN, SP_WX_MARK_OUT,
                                 HUD_H) > 40,
              "the stale mark is grey");
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
        CHECK(count_color_within(fb, SP_C_GOLD, SP_WX_MARK_IN, SP_WX_MARK_OUT,
                                 HUD_H) > 40, "muted draws the mark in gold");
        free(fb);
    }
    __atomic_store_n(&s_hud_env_word, 74U, __ATOMIC_RELEASE);
    s_fake_us = 0;
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
        const size_t centre = count_color_within(fb, SP_C_GREY, 0, 168, SP_CY + 7)
                            - count_color_within(fb, SP_C_GREY, 0, 168, SP_CY - 7);
        const size_t top = count_color_within(fb, SP_C_GREY, 0, 168, SP_ACT_Y0 + 14)
                         + count_color_within(fb, SP_C_INK, 0, 168, SP_ACT_Y0 + 14);
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
    CHECK(strncmp(s_act_row[0], "SAID ", 5) == 0, "newest first, got '%s'",
          s_act_row[0]);
    CHECK(strncmp(s_act_row[1], "ASK ", 4) == 0, "then, got '%s'", s_act_row[1]);
    CHECK(strncmp(s_act_row[2], "TIME ", 5) == 0, "then, got '%s'", s_act_row[2]);
    CHECK(strcmp(s_act_row[0], "SAID VOLUME IS NOW FORTY") == 0,
          "kind and summary on one line, got '%s'", s_act_row[0]);
    for (int i = 0; i < 3; ++i) {
        CHECK(strstr(s_act_row[i], "WEB") == NULL, "the fourth-oldest survived");
    }
    fb = render_frame();
    if (fb) {
        for (int i = 0; i < 3; ++i) {
            const int y0 = SP_ACT_Y0 + i * SP_ACT_PITCH;
            const size_t ink = count_color_within(fb, SP_C_INK, 0, 168, y0 + 14)
                             - count_color_within(fb, SP_C_INK, 0, 168, y0);
            CHECK(ink > 100, "row %d drew no summary ink (%zu)", i, ink);
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
    s_fake_us = 0;

    /* A long summary is cut at the row and marked, never silently. */
    jr_display_activity_push("WEATHER", "83 OVERCAST FORT LAUDERDALE");
    sp_compose();
    CHECK(strlen(s_act_row[0]) == SP_ACT_ROW_GLYPHS,
          "row fills its width exactly, got %zu", strlen(s_act_row[0]));
    CHECK(s_act_row[0][SP_ACT_ROW_GLYPHS - 1] == '.',
          "the cut is marked, got '%s'", s_act_row[0]);
    CHECK(s_act_row_klen[0] == 7, "kind length recorded for the colour split");
    reset_activity();
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
        if (r >= 185 && r <= 194) {
            inside++;
        } else {
            outside++;
            if (outside == 1 || r < omin) omin = r;
            if (outside == 1 || r > omax) omax = r;
        }
    }
    CHECK(inside > 0, "the orbit drew nothing in r185-194");
    CHECK(outside == 0, "%zu orbit pixels outside r185-194 (r%d..%d)", outside, omin, omax);
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
    const int oy = 60;
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
                      255);
        sp_draw_space(&s_display, y, y2, slid + (size_t)y * HUD_W, space, oy,
                      255);
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
    check_focal_follows_the_slide(JR_DISPLAY_SPACE_STATUS, SP_FOCAL_OUT);
    /* WEATHER has three arcs, a mark and three lines of text, all placed
     * about SP_CY + oy; one of them measured about the panel centre would
     * shear exactly as the POWER wedge once did. */
    check_focal_follows_the_slide(JR_DISPLAY_SPACE_WEATHER, SP_WX_MARK_OUT);
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

int main(void)
{
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

    test_every_space_composes_its_own_sheet();
    test_clock_clear_spares_open_shell_surfaces();

    test_desk_is_on_the_ring_only_while_live();
    test_desk_going_dark_moves_the_owner_on();
    test_orbit_mark_eases_when_desk_appears();
    test_weather_without_data_prints_no_number();
    test_weather_mark_sits_at_the_temperature();
    test_stale_weather_loses_its_colour();
    test_activity_is_honest_when_empty_and_newest_first();
    test_desk_sheet_heads_with_the_marked_task();
    test_orbit_stays_in_free_band();
    test_low_battery_uses_the_rim_palette();
    test_focal_wedge_follows_the_slide();
    test_pinned_caption_survives_other_writers();
    test_render_cadence_reaches_the_engine_and_clamps();
    test_missing_clip_falls_back_to_the_face_it_grew_from();

    if (g_failures) {
        printf("%d failure(s) of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("all shell tests passed (%d checks)\n", g_checks);
    return 0;
}
