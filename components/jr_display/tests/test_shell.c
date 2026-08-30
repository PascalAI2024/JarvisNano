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
 *   2. The firmware update ring at r140-154 clears BOTH the Settings headline
 *      beneath it and JR_DISPLAY_SAFE_R above it. That is an arithmetic claim
 *      about glyph corners, so test_ota_ring_band measures the actual drawn
 *      pixels instead of trusting the comment.
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

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
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

static void test_space_ring_wraps_both_ways(void)
{
    /* The ring WRAPS (changed 2026-08-29). This test previously asserted the
     * opposite — that PREV off the first screen was an honest no-op, because a
     * wrap made "am I at the end" unanswerable. An endless ring never raises
     * that question: there is no end, and a swipe never dies against a wall.
     * The old assertions are inverted here rather than deleted, so the change
     * of contract stays visible to whoever reads this next. */
    reset_nav();
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

    jr_display_nav_set(JR_DISPLAY_SPACE_SETTINGS);
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

static void test_ota_slot_pair_reads_as_a_journey(void)
{
    char buf[SP_COL_MAX];

    sp_ota_slots(buf, SP_COL_MAX, 0u, 1u);
    CHECK(strcmp(buf, "0/1") == 0, "staged reads as a journey, got '%s'", buf);

    /* Nothing staged: the destination is not invented. */
    sp_ota_slots(buf, SP_COL_MAX, 1u, OTA_SLOT_NONE);
    CHECK(strcmp(buf, "1") == 0, "unstaged is the active slot, got '%s'", buf);
    sp_ota_slots(buf, SP_COL_MAX, 1u, 1u);
    CHECK(strcmp(buf, "1") == 0, "self-target is not a journey, got '%s'", buf);

    sp_ota_slots(buf, SP_COL_MAX, OTA_SLOT_NONE, OTA_SLOT_NONE);
    CHECK(strcmp(buf, "?") == 0, "unknown is admitted, got '%s'", buf);
}

/* Stage SETTINGS, fully presented, with the given OTA state. */
static void stage_settings(jr_display_ota_state_t st, uint8_t pct, int overlay)
{
    reset_nav();
    jr_display_nav_set(JR_DISPLAY_SPACE_SETTINGS);
    s_display.board.width = HUD_W;
    s_display.board.height = HUD_H;
    s_display.board.swap_color_bytes = false;
    s_space_on = true;
    s_space_from = JR_DISPLAY_SPACE_SETTINGS;
    s_space_to = JR_DISPLAY_SPACE_SETTINGS;
    s_space_prog = 256;
    s_space_ease = 256;
    s_space_veil = 256;
    s_detail_space = JR_DISPLAY_SPACE_SETTINGS;
    s_detail_ease = overlay == JR_DISPLAY_OVERLAY_DETAIL ? 256 : 0;
    s_shade_ease = overlay == JR_DISPLAY_OVERLAY_SHADE ? 256 : 0;
    __atomic_store_n(&s_nav_word,
                     (uint32_t)JR_DISPLAY_SPACE_SETTINGS |
                         ((uint32_t)overlay << NAV_OVL_SHIFT),
                     __ATOMIC_RELEASE);
    jr_display_set_status(90U, true, -61, 1893U);
    jr_display_power_set(74U, 4020U, true, true);
    jr_display_ota_set(st, pct, 0U, 1U, true);
    sp_compose();
}

static void test_settings_detail_reports_update_and_slot(void)
{
    stage_settings(JR_DISPLAY_OTA_RECEIVING, 42U, JR_DISPLAY_OVERLAY_DETAIL);
    CHECK(s_detail_rows == 5, "five rows, got %d", s_detail_rows);
    CHECK(s_detail_rows <= SP_ROWS_MAX, "within the row budget");
    CHECK(strcmp(s_detail_label[2], "POWER") == 0, "power row present");
    CHECK(strcmp(s_detail_value[2], "74%+4.0V") == 0,
          "power shown, got '%s'", s_detail_value[2]);
    CHECK(strcmp(s_detail_label[3], "UPDATE") == 0, "update row present");
    CHECK(strcmp(s_detail_value[3], "42%") == 0, "percent shown, got '%s'",
          s_detail_value[3]);
    CHECK(strcmp(s_detail_label[4], "SLOT") == 0, "slot row present");
    CHECK(strcmp(s_detail_value[4], "0/1") == 0, "slots shown, got '%s'",
          s_detail_value[4]);

    /* At rest the row answers readiness instead of naming a non-event. */
    jr_display_ota_set(JR_DISPLAY_OTA_IDLE, 0U, 0U, 0xFFU, true);
    sp_compose();
    CHECK(strcmp(s_detail_value[3], "READY") == 0, "idle+ok, got '%s'",
          s_detail_value[3]);
    jr_display_ota_set(JR_DISPLAY_OTA_IDLE, 0U, 0U, 0xFFU, false);
    sp_compose();
    CHECK(strcmp(s_detail_value[3], "HOLD") == 0, "idle+blocked, got '%s'",
          s_detail_value[3]);

    jr_display_ota_set(JR_DISPLAY_OTA_PROBATION, 0U, 1U, 0xFFU, true);
    sp_compose();
    CHECK(strcmp(s_detail_value[3], "PROBATION") == 0, "probation, got '%s'",
          s_detail_value[3]);
}

static void test_update_outranks_the_settings_headline(void)
{
    stage_settings(JR_DISPLAY_OTA_RECEIVING, 42U, JR_DISPLAY_OVERLAY_NONE);
    CHECK(strcmp(s_settings_label, "UPDATE 42%") == 0, "writing, got '%s'",
          s_settings_label);

    jr_display_ota_set(JR_DISPLAY_OTA_ROLLED_BACK, 0U, 0U, 0xFFU, true);
    sp_compose();
    CHECK(strcmp(s_settings_label, "ROLLBACK") == 0, "rollback, got '%s'",
          s_settings_label);

    /* Both healthy resting states hand the headline back to the numbers.
     *
     * Staged at the WORST CASE — 100 and 100 — because that is the only place
     * this breaks. The previous assertion here searched for the substring
     * "VOL", which stayed green while the label was rendering "VOL 100%  10"
     * with the brightness digits cut off: it pinned a word that happened to be
     * present rather than the numbers the screen exists to show. Assert both
     * values survive, and that the string still fits its store. */
    s_status_word = 100U;                     /* volume 100 */
    __atomic_store_n(&s_brightness_want, 100U, __ATOMIC_RELEASE);

    jr_display_ota_set(JR_DISPLAY_OTA_VALID, 100U, 1U, 0xFFU, true);
    sp_compose();
    CHECK(strcmp(s_settings_label, "V100% L100%") == 0,
          "valid yields both levels, got '%s'", s_settings_label);
    CHECK(strlen(s_settings_label) < (size_t)SP_LABEL_CAP,
          "headline fits its store, got %zu of %d",
          strlen(s_settings_label), SP_LABEL_CAP);

    jr_display_ota_set(JR_DISPLAY_OTA_IDLE, 0U, 1U, 0xFFU, true);
    sp_compose();
    CHECK(strcmp(s_settings_label, "V100% L100%") == 0,
          "idle yields both levels, got '%s'", s_settings_label);

    /* The shade columns have a tighter budget (10 glyphs) and the same trap:
     * "R LIGHT 100%" is 12 and used to render "R LIGHT 10". */
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
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        apply_space_overlay(&s_display, y, y2, fb + (size_t)y * HUD_W);
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
    static const char *const tools[] = { "AUDIO", "VISION", "MEMORY", "WEB" };
    int worst = 0;

    jr_display_desk_set_task("REINDEX", 64U, JR_DISPLAY_AGENT_WORKING);
    jr_display_tools_set(tools, 4, 2);
    jr_display_jarvis_set_session(true, 17U, 900U);

    for (int space = 0; space < JR_DISPLAY_SPACE_COUNT; ++space) {
        for (size_t o = 0; o < sizeof overlays / sizeof *overlays; ++o) {
            for (size_t s = 0; s < sizeof ota / sizeof *ota; ++s) {
                for (int muted = 0; muted <= 1; ++muted) {
                    stage_settings(ota[s], 63U, overlays[o]);
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
 * arithmetic in the comment. */
static void test_ota_ring_band(void)
{
    /* Pin the headline so the OTA state change cannot move it: a caller
     * label outranks the composed one, which leaves the ring as the only
     * difference between the two frames. */
    stage_settings(JR_DISPLAY_OTA_IDLE, 0U, JR_DISPLAY_OVERLAY_NONE);
    jr_display_space_set_label(JR_DISPLAY_SPACE_SETTINGS, "PINNED", NULL);
    sp_compose();
    uint16_t *quiet = render_frame();
    stage_settings(JR_DISPLAY_OTA_RECEIVING, 63U, JR_DISPLAY_OVERLAY_NONE);
    sp_compose();
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

    CHECK(changed > 0, "the ring drew something");
    CHECK(lo >= SP_OTA_IN - 1, "inner edge at r=%d, expected >= %d", lo,
          SP_OTA_IN - 1);
    CHECK(hi <= SP_OTA_OUT + 1, "outer edge at r=%d, expected <= %d", hi,
          SP_OTA_OUT + 1);
    CHECK(hi <= JR_DISPLAY_SAFE_R, "readable content stays in the safe area");
    CHECK(hi < 215, "clears the battery rim, the privacy ring and the arcs");

    /* IDLE and VALID are both quiet, so moving between them must change
     * nothing at all on the dial. */
    free(ringing);
    stage_settings(JR_DISPLAY_OTA_VALID, 100U, JR_DISPLAY_OVERLAY_NONE);
    sp_compose();
    ringing = render_frame();
    if (ringing) {
        CHECK(memcmp(quiet, ringing,
                     (size_t)HUD_W * HUD_H * sizeof *quiet) == 0,
              "VALID draws no ring");
    }
    jr_display_space_set_label(JR_DISPLAY_SPACE_SETTINGS, NULL, NULL);
    free(quiet);
    free(ringing);
}

/* The ring sits outside the headline's worst-case glyph corner. Find that
 * corner by diffing against a headline of maximum width. */
static void test_ota_ring_clears_the_headline(void)
{
    stage_settings(JR_DISPLAY_OTA_IDLE, 0U, JR_DISPLAY_OVERLAY_NONE);
    uint16_t *a = render_frame();
    jr_display_space_set_label(JR_DISPLAY_SPACE_SETTINGS, "WWWWWWWWWWWW",
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
    CHECK(hi > 0, "the headline drew something");
    CHECK(hi < SP_OTA_IN, "headline reaches r=%d, ring starts at r=%d", hi,
          SP_OTA_IN);
    CHECK(hi <= JR_DISPLAY_SAFE_R, "headline stays readable");

    jr_display_space_set_label(JR_DISPLAY_SPACE_SETTINGS, NULL, NULL);
    free(a);
    free(b);
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
    stage_settings(JR_DISPLAY_OTA_RECEIVING, 63U, JR_DISPLAY_OVERLAY_DETAIL);

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
    for (int y = 0; y < HUD_H; y += STRIP_ROWS) {
        const int y2 = y + STRIP_ROWS > HUD_H ? HUD_H : y + STRIP_ROWS;
        apply_space_overlay(&s_display, y, y2, strips + (size_t)y * HUD_W);
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
 * bit-identical, which requires the shell to write nothing whatsoever. */
static void test_jarvis_at_rest_draws_nothing(void)
{
    stage_settings(JR_DISPLAY_OTA_RECEIVING, 63U, JR_DISPLAY_OVERLAY_NONE);
    reset_nav();
    s_space_from = JR_DISPLAY_SPACE_JARVIS;
    s_space_to = JR_DISPLAY_SPACE_JARVIS;
    s_space_veil = 0;
    s_detail_ease = 0;
    s_shade_ease = 0;
    s_space_on = false;

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
 * which builds the SETTINGS sheet — so tapping the clock or the battery
 * opened a sheet headed SETTINGS listing privacy, link, update and slot rows.
 * The screen you touched was not the screen you got.
 *
 * This asserts the heading of each space individually AND that no space other
 * than SETTINGS is headed "SETTINGS", which is the shape of the original bug:
 * a new space added without its own case would silently inherit that sheet
 * again, and this test is what refuses it. */
static void test_every_space_composes_its_own_sheet(void)
{
    static const struct { int space; const char *head; } expect[] = {
        { JR_DISPLAY_SPACE_JARVIS,   "SESSION"  },
        { JR_DISPLAY_SPACE_WATCH,    "TIME"     },
        { JR_DISPLAY_SPACE_POWER,    "POWER"    },
        { JR_DISPLAY_SPACE_TOOLS,    "TOOLS"    },
        { JR_DISPLAY_SPACE_SETTINGS, "SETTINGS" },
    };
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; ++i) {
        sp_compose_detail(expect[i].space);
        CHECK(strcmp(s_detail_head, expect[i].head) == 0,
              "space %d should head '%s', got '%s'",
              expect[i].space, expect[i].head, s_detail_head);
        CHECK(s_detail_rows > 0, "space %d composed no rows", expect[i].space);
    }

    /* No space may borrow the SETTINGS sheet. */
    for (int space = 0; space < (int)JR_DISPLAY_SPACE_COUNT; ++space) {
        if (space == (int)JR_DISPLAY_SPACE_SETTINGS) {
            continue;
        }
        sp_compose_detail(space);
        CHECK(strcmp(s_detail_head, "SETTINGS") != 0,
              "space %d borrowed the SETTINGS sheet", space);
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

int main(void)
{
    test_space_ring_wraps_both_ways();
    test_overlay_axis_is_unambiguous();
    test_sideways_resets_and_home_escapes();
    test_transition_publishes_a_new_serial();
    test_legacy_shade_bit_still_opens_the_shade();

    test_ota_word_clamps_everything();
    test_ota_arc_semantics();
    test_ota_names_fit_their_column();
    test_ota_slot_pair_reads_as_a_journey();
    test_settings_detail_reports_update_and_slot();
    test_update_outranks_the_settings_headline();

    test_never_leaves_shell_radius();
    test_ota_ring_band();
    test_ota_ring_clears_the_headline();
    test_detail_sheet_rows_stay_inside();
    test_strip_invariance();
    test_jarvis_at_rest_draws_nothing();

    test_shade_hits_resolve_to_controls();

    test_every_space_composes_its_own_sheet();

    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("all shell tests passed\n");
    return 0;
}
