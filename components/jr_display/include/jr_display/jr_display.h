/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_display — asynchronous CO5300 / esp_emote_gfx presenter.
 *
 * jr_display_start() returns the domain display port immediately. Hardware,
 * SPIFFS, and the graphics engine are brought up by a dedicated presenter
 * task. The port callbacks never call the graphics engine and never block;
 * latest intent wins in a 32-bit mailbox.
 */
#ifndef JR_DISPLAY_JR_DISPLAY_H
#define JR_DISPLAY_JR_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "jr_ports/display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JR_DISPLAY_INIT_STOPPED = 0,
    JR_DISPLAY_INIT_STARTING,
    JR_DISPLAY_INIT_READY,
    JR_DISPLAY_INIT_FAILED,
} jr_display_init_state_t;

typedef enum {
    JR_DISPLAY_TEST_OFF = 0,
    JR_DISPLAY_TEST_COLOR_BARS,
    JR_DISPLAY_TEST_GRID,
    JR_DISPLAY_TEST_WHITE,
    JR_DISPLAY_TEST_RED,
    JR_DISPLAY_TEST_GREEN,
    JR_DISPLAY_TEST_BLUE,
    JR_DISPLAY_TEST_TOUCH_CHALLENGE,
} jr_display_test_pattern_t;

typedef enum {
    JR_DISPLAY_AGENT_NONE = 0,
    JR_DISPLAY_AGENT_WORKING,
    JR_DISPLAY_AGENT_VERIFYING,
    JR_DISPLAY_AGENT_WAITING,
    JR_DISPLAY_AGENT_SUCCEEDED,
    JR_DISPLAY_AGENT_FAILED,
} jr_display_agent_state_t;

#define JR_DISPLAY_SURFACE_TITLE_CAP   25U
#define JR_DISPLAY_SURFACE_BODY_CAP    49U
#define JR_DISPLAY_SURFACE_ACTION_CAP  3U
#define JR_DISPLAY_SURFACE_LABEL_CAP   13U

typedef enum {
    JR_DISPLAY_SURFACE_NOTICE = 0,
    JR_DISPLAY_SURFACE_PROGRESS,
    JR_DISPLAY_SURFACE_RESULT,
    JR_DISPLAY_SURFACE_CHOICE,
    JR_DISPLAY_SURFACE_CONSENT,
} jr_display_surface_kind_t;

/* Bounded transient card supplied by a paired Android/Mac companion. Text is
 * deliberately short enough to remain readable on the 466 px round panel. */
typedef struct {
    jr_display_surface_kind_t kind;
    char title[JR_DISPLAY_SURFACE_TITLE_CAP];
    char body[JR_DISPLAY_SURFACE_BODY_CAP];
    char action_labels[JR_DISPLAY_SURFACE_ACTION_CAP]
                      [JR_DISPLAY_SURFACE_LABEL_CAP];
    uint8_t action_count;
} jr_display_surface_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    size_t bytes;
    uint64_t frame_id;
    uint64_t last_flush_ms;
    bool valid;
    jr_display_test_pattern_t test_pattern;
} jr_display_snapshot_info_t;

typedef struct {
    jr_display_init_state_t init_state;
    esp_err_t last_error;
    bool task_running;
    bool blanked;

    jr_face_t requested_face;
    jr_face_t applied_face;
    uint8_t requested_amplitude;
    uint8_t applied_bucket;

    uint32_t requests;
    uint32_t state_changes;
    uint32_t segment_sets;
    uint32_t asset_load_failures;
    uint32_t flush_submissions;
    uint32_t flush_completions;
    uint32_t flush_errors;
    uint32_t actual_fps;
    uint32_t current_asset_bytes;
    uint32_t free_psram_bytes;
    uint32_t task_stack_hwm;
} jr_display_diag_t;

/* Start the fail-soft presenter and return its nonblocking display port.
 * ESP_OK means the presenter task was created, not that physical init has
 * already completed. Use jr_display_is_ready()/jr_display_get_diag() for the
 * asynchronous result. Idempotent: repeated calls return the same port. */
esp_err_t jr_display_start(jr_display_t *out_port);

bool jr_display_is_ready(void);
esp_err_t jr_display_get_diag(jr_display_diag_t *out_diag);

/* Software mirror of the exact byte-swapped RGB565 strips submitted to the
 * CO5300. This is deliberately labelled a mirror, not panel readback. The
 * buffer is allocated lazily in PSRAM and records for a bounded interval after
 * a consumer asks, avoiding a permanent render-bandwidth tax. */
esp_err_t jr_display_snapshot_get_info(jr_display_snapshot_info_t *out_info);
esp_err_t jr_display_snapshot_copy_rgb565(void *dst, size_t dst_size,
                                          jr_display_snapshot_info_t *out_info);

/* Replace outgoing renderer pixels with a deterministic hardware test pattern.
 * The normal gfx state continues to run underneath and resumes immediately
 * when JR_DISPLAY_TEST_OFF is selected. */
esp_err_t jr_display_set_test_pattern(jr_display_test_pattern_t pattern);
jr_display_test_pattern_t jr_display_get_test_pattern(void);

/* Physical panel + touch challenge. sector is 0..7 clockwise from 12 o'clock;
 * -1 renders the all-green success state. progress is 0..3. Calling this puts
 * the presenter into JR_DISPLAY_TEST_TOUCH_CHALLENGE until test_pattern=OFF. */
esp_err_t jr_display_set_touch_challenge(int sector, uint8_t progress);

/* Operator/agent identity, plus a second way in to the control shade.
 *
 * agent_active/progress/state own the OUTER rim segments (r224-230) and
 * nothing else, so Agent Link never steals Listening/Speaking foreground and
 * never collides with the spatial shell, which stops at
 * JR_DISPLAY_SHELL_R_MAX. The same state tints the Desk focal ring.
 *
 * shade_open is OR-ed with jr_display_nav_down()'s overlay state: either
 * source opens the one control shade described under SPATIAL SHELL below, so
 * an existing caller that tracks its own shade flag keeps working unchanged
 * while gesture routing moves to the nav API. */
void jr_display_set_shell_state(bool shade_open, bool agent_active,
                                uint8_t agent_progress,
                                jr_display_agent_state_t agent_state);

/* Desk/companion surface. Present/dismiss are thread-safe. hit_test returns a
 * zero-based action index or -1 and uses exactly the geometry drawn on glass. */
esp_err_t jr_display_surface_present(const jr_display_surface_t *surface);
void jr_display_surface_dismiss(void);
bool jr_display_surface_is_active(void);
int jr_display_surface_hit_test(uint16_t x, uint16_t y);


/* Feed the HUD layer the world-state it cannot see from inside the display:
 * battery charge, privacy state, and device tilt. Cheap and lock-free (one
 * packed word), so the caller may push it from any task at any cadence — the
 * flush path reads the most recent value. Keeping sensor/session components
 * out of jr_display's dependency list is deliberate: the composition root
 * owns that wiring.
 *
 * batt_pct: 0..100, or 0xFF when no battery/unknown.
 * privacy_muted: true renders the persistent outer gold privacy ring.
 * roll_deg/pitch_deg: straight from jr_imu; 0,0 disables tilt parallax. */
void jr_display_set_hud_env(uint8_t batt_pct, bool charging,
                            bool privacy_muted,
                            float roll_deg, float pitch_deg);

/* Enable/disable the procedural HUD overlay at runtime. Exists so the HUD's
 * true frame-rate cost can be A/B measured on the panel instead of estimated —
 * flip it and read actual_fps from /api/display. Enabled by default. */
void jr_display_set_hud_enabled(bool enabled);

/* STATE-05/06: present / dismiss the tap-to-answer choice arcs.
 *
 * `question` and labels[] only have to stay alive ACROSS THIS CALL: the
 * display copies everything it renders (labels capped at 24 chars, question
 * wrapped to 2x24) into its own storage before publishing. A caller may
 * therefore rewrite or drop its ask snapshot the moment this returns — a
 * re-ask that memsets the previous ask's storage while frames of the old
 * presentation are still flushing can no longer blank or tear the glass.
 * question may be NULL: the arcs and labels then render without a prompt.
 * n == 0 (or labels == NULL) dismisses. Idempotent: dismissing when nothing is
 * shown is a no-op, which is what JR_CMD_DISMISS_CHOICES requires.
 *
 * jr_display_choice_hit() maps a raw panel tap to a choice index, or -1. It
 * gates on the arc band, so taps on the face or the bezel ticks do NOT answer.
 *
 * The presentation eases in/out over ~250 ms render-side. A dismissal keeps
 * the tapped arc lit through the exit fade (the confirmation beat); the hit
 * test dies with the dismiss itself, so a fading ask never answers a tap. */
void jr_display_present_choices(const char *question,
                                const char *const *labels, int n);
void jr_display_dismiss_choices(void);
bool jr_display_choices_active(void);
int  jr_display_choice_hit(int x, int y);
void jr_display_set_choice_selected(int index);
bool jr_display_hud_enabled(void);

/* STATE-04: live caption chip — a subtitle band at the bottom of the glass
 * mirroring what the agent is saying. The text is COPIED at set time (up to
 * 96 chars, wrapped to 2x38 display columns; the tail of a longer sentence is
 * clipped — it is a rolling caption). SINGLE-WRITER: the app task calls
 * set/clear, the render task only reads. Hidden automatically while a choice
 * ask is on screen — the ask owns the glass. NULL or "" clears. The on/off
 * edges ease over ~250 ms render-side (band and text together); text swaps
 * while visible stay immediate — a live transcript must not lag its voice. */
void jr_display_caption_set(const char *text);
void jr_display_caption_clear(void);

/* TRANS-05: transient tap ripple — an expanding, fading ring from panel
 * point (x, y) that self-clears after ~400 ms. Fire-and-forget from the app
 * task; a new tap replaces any ripple still in flight (single slot, no
 * queue). Drawn under the ask presentation and the caption: feedback, not
 * chrome. */
void jr_display_ripple(int x, int y);

/* The same single slot, drawn as a REFUSAL: the ring contracts in dim neutral
 * instead of expanding in cyan. A layer that rejects an event overwrites the
 * accept ripple already fired for that tap, so the user sees exactly one
 * transient — the rejecting one. docs/INTERACTION_MODEL.md §7. */
void jr_display_ripple_reject(int x, int y);

/* TRANS-01: the wake bloom — VISION.md's "point of light blooms into the
 * ring", for the moment WakeNet hears "Jarvis" from rest. A centre seed of
 * light collapses as a cyan wavefront expands to the face ring over ~600 ms,
 * then erases itself. Fire-and-forget from ANY task (one release-store); a
 * re-fire restarts it; no dismiss exists or is needed. Drawn topmost so it
 * reads over a watch mid fade-out — the exact state a wake from rest is in.
 * Renders only while frames flush: fired while the panel is blanked, the
 * visible part is whatever remains of the 600 ms once the face returns, so
 * call it AFTER (or simultaneously with) the mood poke that wakes the
 * display, never before a deliberate delay. */
void jr_display_bloom(void);

/* UI-01: ambient watch face for the privacy-muted state — the whole strip
 * dims (the baked bezel ticks become the dial) and two hands plus a hub draw
 * over it. SINGLE-WRITER: the app task calls this at ~1 Hz; the render task
 * only reads. on/hh/mm are packed into one word and published with a single
 * release-store, so the renderer can never see a torn time. hh 0..23,
 * mm 0..59 (out-of-range folds to 0). Never coexists with a choice ask (the
 * ask wins); renders UNDER the caption so status text stays readable.
 *
 * `on` is intent, not an instant switch: the presenter eases the dim and the
 * hands in/out over ~400 ms (ease-in-out, render-side), so callers toggle
 * freely — a reversal mid-fade walks back from wherever it is. During the
 * fade-out the hands hold the LAST shown time; publishing zeros with off is
 * therefore safe. */
void jr_display_clock_set(bool on, int hh, int mm, int ss);

/* Pushed canvas: a full-frame RGB565 (little-endian) image that temporarily
 * replaces the face — the glass as a remote drawing surface for the paired
 * companion / JarvisMCP. Exact panel dimensions only (466x466). The image is
 * copied (caller keeps ownership) and converted to panel byte order once.
 * ttl_ms is clamped to (0, 300000]; 0 picks the 30 s default. A test pattern,
 * if set, still wins (diagnostics outrank decoration). Any-task safe.
 * Arrival and departure (clear or TTL expiry) crossfade with the face over
 * ~400 ms render-side; a repeat show while already visible swaps content
 * without re-fading, so streamed updates stay immediate. */
esp_err_t jr_display_canvas_show(const uint16_t *rgb565, size_t width,
                                 size_t height, uint32_t ttl_ms);
void jr_display_canvas_clear(void);
bool jr_display_canvas_active(void);

/* CO5300 panel brightness 0..100. Safe to call from ANY task at ANY rate: this
 * only publishes a target, which the render task applies during its next flush.
 * The panel command must not be issued off the render task — it shares one QSPI
 * device with the frame flush and racing it asserts in spi_device_release_bus.
 * No-op until the presenter is ready. */
esp_err_t jr_display_set_brightness(uint8_t percent);

/* ===== SPATIAL SHELL =====================================================
 *
 * THE VISUAL CONTRACT (this comment is the design document; there is no
 * DESIGN.md). The glass is a 466 px circle, not a small rectangle, so the
 * shell is built from rings, arcs and a centre — never from a drawer, a card,
 * or a list whose corners the bezel would eat.
 *
 * FOUR SPACES ON ONE HORIZONTAL RING, CLAMPED AT BOTH ENDS
 *
 *      JARVIS  <->  DESK  <->  TOOLS  <->  SETTINGS
 *
 * The ring deliberately does NOT wrap. A wrap would make the page indicator
 * jump the full width of the dial on one swipe, and it would make "am I at
 * the end" unanswerable. Clamped, the indicator can interpolate straight from
 * mark to mark, and a swipe past the end is an honest no-op.
 *
 * Edge controls are global; the vertical centre axis owns overlays:
 *
 *      left edge UP/DOWN  -> volume +/− from any screen.
 *      right edge DOWN/UP -> brightness +/− from any screen.
 *      top-edge down      -> SHADE with values and physical MUTE/LISTEN.
 *      centre swipe up    -> DETAIL, or closes an open shade.
 *      swipe L/R          -> move one temporary side space and close overlays.
 *                             Active voice immediately returns to JARVIS.
 *      double tap -> JARVIS, no overlay. The global escape, from anywhere.
 *      tap        -> jr_display_hit() resolves what was under the finger.
 *      hold       -> privacy. Owned entirely by the caller; the display only
 *                    reflects it through jr_display_set_hud_env().
 *
 * RADIAL BUDGET. The outer band already has three tenants that must stay
 * legible across a room, so THE SPATIAL SHELL NEVER WRITES A PIXEL BEYOND
 * JR_DISPLAY_SHELL_R_MAX — not even its backdrop dim. The battery rim
 * (r215-220), the gold privacy ring (r221-222) and the choice arcs (r223-231)
 * are therefore structurally safe from it: THE CONTROL SHADE CANNOT CONFLICT
 * WITH THE PRIVACY RING BECAUSE IT CANNOT REACH IT. This is enforced in one
 * place — every shell primitive clips its span to this circle — rather than
 * trusted to each renderer.
 *
 *      r <= 104   semantic focal object (the centre means the space)
 *      r <= 168   JR_DISPLAY_SAFE_R — all KEY content: focal object, labels,
 *                 detail rows, shade controls. Nothing readable sits outside,
 *                 so nothing readable is ever cut by the bezel.
 *      r 184-196  the orbital page indicator (peripheral chrome only)
 *      r <= 214   JR_DISPLAY_SHELL_R_MAX — the backdrop veil, and the hard
 *                 clip every shell primitive is bounded by.
 *
 * WHAT EACH SPACE MEANS, AND WHAT ITS CENTRE SHOWS
 *
 *      JARVIS    the animated face, untouched. The shell draws NOTHING here
 *                at rest — no veil, no label — so every existing scene (face,
 *                caption, ask, watch, canvas, bloom, ripple) is bit-identical
 *                to the pre-shell presenter. Detail: session facts.
 *      DESK      a progress ring around a big percentage: the active task.
 *                Ring colour is the agent state from set_shell_state.
 *      TOOLS     one petal per available capability around a live core; the
 *                most recently used petal is lit and thickened.
 *      SETTINGS  four cardinal gauges — volume, brightness, link, memory —
 *                around a privacy heart: a filled cyan core when live, a
 *                slashed gold ring when muted.
 *
 * IDENTITY. Privacy is the loudest thing on the glass: when muted, the
 * indicator, the Tools core and the shade's privacy control all turn gold and
 * the Settings heart is struck through. Nothing else in the shell uses gold.
 *
 * COST. All of this is procedural and strip-local. The shell adds NO frame
 * buffer, allocates nothing per frame, and holds no unbounded string: every
 * drawable string is a fixed-capacity static array, truncated once at the
 * setter. In JARVIS at rest the whole layer costs one boolean test per strip.
 */

#define JR_DISPLAY_SAFE_R        168   /* readable content stays inside     */
#define JR_DISPLAY_SHELL_R_MAX   214   /* hard clip; privacy ring is beyond */
#define JR_DISPLAY_SPACE_MS      260   /* space-to-space slide, ease-in-out */
#define JR_DISPLAY_SPACE_HOLD_MS 900   /* page indicator lingers this long  */
#define JR_DISPLAY_TOOLS_MAX     4     /* petals that stay distinguishable  */

typedef enum {
    JR_DISPLAY_SPACE_JARVIS = 0,
    JR_DISPLAY_SPACE_DESK,
    JR_DISPLAY_SPACE_TOOLS,
    JR_DISPLAY_SPACE_SETTINGS,
    JR_DISPLAY_SPACE_COUNT,
} jr_display_space_t;

typedef enum {
    JR_DISPLAY_OVERLAY_NONE = 0,
    JR_DISPLAY_OVERLAY_DETAIL,   /* swipe up:   this space's context sheet */
    JR_DISPLAY_OVERLAY_SHADE,    /* swipe down: the global control shade   */
} jr_display_overlay_t;

/* What jr_display_hit() found under a tap. The display resolves geometry — it
 * is the only thing that knows where it drew — and the caller decides policy.
 * Nothing here mutates state except through the caller: a PRIVACY_TOGGLE hit
 * does NOT mute the mic, it reports that the user pressed the mute control. */
typedef enum {
    JR_DISPLAY_ACT_NONE = 0,      /* not the shell's — run your own tap path */
    JR_DISPLAY_ACT_FOCUS,         /* the focal object: open/act on the space */
    JR_DISPLAY_ACT_DISMISS,       /* outside a live overlay: close it        */
    JR_DISPLAY_ACT_VOLUME_UP,
    JR_DISPLAY_ACT_VOLUME_DOWN,
    JR_DISPLAY_ACT_PRIVACY_TOGGLE,
} jr_display_action_t;

/* NAVIGATION. Call these from gesture routing; they are the whole state API.
 *
 * Lock-free and safe from ANY task: each is one compare-exchange on a single
 * packed word plus a task notify. They never block, never allocate, and never
 * touch the panel — the render task picks the change up on its next frame and
 * eases into it, so a caller may fire them as fast as a finger moves.
 *
 * next/prev clamp at the ends. up/down toggle: up opens DETAIL (or closes the
 * shade), down opens the SHADE (or closes the detail), which is what makes a
 * two-overlay vertical axis feel like one axis. Every call is idempotent —
 * re-issuing the current state does not restart an animation. */
void jr_display_nav_next(void);
void jr_display_nav_prev(void);
void jr_display_nav_up(void);
void jr_display_nav_down(void);
void jr_display_nav_home(void);   /* JARVIS, no overlay: the global escape */
void jr_display_nav_set(jr_display_space_t space);

jr_display_space_t   jr_display_nav_space(void);
jr_display_overlay_t jr_display_nav_overlay(void);

/* Resolve a raw panel tap against what is actually on the glass right now.
 *
 * Returns JR_DISPLAY_ACT_NONE whenever the shell does not own the point —
 * including while a test pattern, a choice ask, or a companion surface is up,
 * since each of those owns the glass and has its own hit test. A caller can
 * therefore run its existing tap path unchanged and consult this first:
 *
 *      int idx = jr_display_choice_hit(x, y);       // existing paths first
 *      if (idx >= 0) { answer(idx); }
 *      else switch (jr_display_hit(x, y)) { ... }   // then the shell
 *
 * Pure geometry, no side effects, any task. */
jr_display_action_t jr_display_hit(int x, int y);

/* CONTENT. Each setter is the single writer for its space and copies every
 * string it is given (capped at 12 display columns — a longer label is
 * truncated, never wrapped and never allocated). Callers keep ownership and
 * may pass NULL to clear. All are any-task safe and lock-free; text lands
 * before the packed word that gates it, so a racing frame shows stale text at
 * worst, never a partial length. Setting content does NOT navigate. */
void jr_display_space_set_label(jr_display_space_t space, const char *headline,
                                const char *note);

/* JARVIS detail: the live conversation. turns is the exchange count, elapsed_s
 * the session age, linked whether the transport is up. */
void jr_display_jarvis_set_session(bool linked, uint16_t turns,
                                   uint32_t elapsed_s);

/* DESK focal object and detail: the active task. progress 0..100 drives the
 * ring; state tints it with the same palette as the agent rim. */
void jr_display_desk_set_task(const char *task, uint8_t progress,
                              jr_display_agent_state_t state);

/* TOOLS focal object and detail: the capabilities this device can actually
 * reach. names[0..n) are copied; n is clamped to JR_DISPLAY_TOOLS_MAX. recent
 * is the index of the last-used tool, or <0 for none. */
void jr_display_tools_set(const char *const *names, int n, int recent);

/* ---- firmware update, as a SETTINGS citizen ----------------------------
 *
 * An OTA is the one background job that can brick the device, so it does not
 * get a toast that scrolls away. It lives in Settings — where a worried owner
 * actually goes to look — and it stays there through probation and rollback.
 *
 * This is deliberately STATUS, NOT CONTROL. The display renders what the
 * updater reports and never starts, confirms, or aborts anything: there is no
 * OTA hit target and no OTA member in jr_display_action_t, so no tap on a
 * progress ring can ever influence a flash write.
 *
 * Where it shows up, all inside the existing Settings geometry:
 *
 *   - a progress ring at r140-154, concentric with the Settings gauges and
 *     just outside the headline's worst-case glyph corner (r131.5). It is
 *     round-native, inside JR_DISPLAY_SAFE_R, and — like every shell
 *     primitive — clipped to JR_DISPLAY_SHELL_R_MAX, so it cannot reach the
 *     battery rim, the gold privacy ring, or the choice arcs.
 *   - the Settings headline, which an update in flight outranks.
 *   - two rows of the Settings detail sheet: UPDATE and SLOT.
 *
 * IDLE and VALID are the two HEALTHY RESTING states and draw NO ring at all,
 * so a device with nothing to report keeps a quiet Settings dial; the facts
 * stay one swipe up, in the detail sheet. Every other state rings.
 *
 * ARC SEMANTICS. percent drives the arc for RECEIVING only. PREFLIGHT draws
 * an empty track (armed, nothing written yet) and every other ringing state
 * draws a FULL ring in its own colour, because a half-filled ring would imply
 * progress that is not happening.
 *
 *      PREFLIGHT    dim cyan, empty track   checks running
 *      BLOCKED      amber, full             refused; nothing was written
 *      RECEIVING    cyan, percent           writing the target slot
 *      PROBATION    violet, full            new image up, not yet confirmed
 *      VALID        (no ring)               confirmed good
 *      ROLLED_BACK  amber, full             reverted to the previous slot
 *      FAILED       red, full
 *
 * SLOTS are partition indices, 0 or 1; anything else means unknown and
 * renders as "?". active_slot is what is running now, target_slot is what an
 * update would be (or is being) written into — the sheet shows them as
 * "0/1", which answers "where am I, where am I going" in three visible glyphs.
 * Passing 0xFF for target_slot, or the same value as active_slot, reads as
 * nothing staged and renders the active slot alone.
 *
 * preflight_ok is the updater's own readiness verdict (power, link, free
 * space). It is what separates PREFLIGHT from BLOCKED, and at rest it is what
 * the UPDATE row reports — so Settings answers "could this device take an
 * update right now" without an update having to be in flight.
 *
 * Any task, lock-free, no allocation: one packed word, one release-store. */
typedef enum {
    JR_DISPLAY_OTA_IDLE = 0,     /* nothing staged; slot + readiness only   */
    JR_DISPLAY_OTA_PREFLIGHT,    /* readiness checks running                */
    JR_DISPLAY_OTA_BLOCKED,      /* preflight refused (power/link/space)    */
    JR_DISPLAY_OTA_RECEIVING,    /* writing the target slot; percent live   */
    JR_DISPLAY_OTA_PROBATION,    /* booted the new image, not yet confirmed */
    JR_DISPLAY_OTA_VALID,        /* confirmed good; the healthy resting end */
    JR_DISPLAY_OTA_FAILED,
    JR_DISPLAY_OTA_ROLLED_BACK,  /* reverted; keep LAST (range clamp)       */
} jr_display_ota_state_t;

void jr_display_ota_set(jr_display_ota_state_t state, uint8_t percent,
                        uint8_t active_slot, uint8_t target_slot,
                        bool preflight_ok);

/* AXP2101 truth for Settings and the charging rim. percent is 0..100 or
 * 0xff when no battery sample exists. Edge animations are owned by the caller;
 * this setter only publishes bounded state. */
void jr_display_power_set(uint8_t percent, uint16_t millivolts,
                          bool usb_present, bool charging);

/* SETTINGS focal object, detail, and the control shade readouts: the real
 * numbers, not a mood. Brightness is not passed — the display already owns it
 * and reads back its own target, so the shade can never disagree with the
 * panel. rssi_dbm is ignored when net_up is false. */
void jr_display_set_status(uint8_t volume, bool net_up, int8_t rssi_dbm,
                           uint32_t free_psram_kib);

#ifdef __cplusplus
}
#endif

#endif /* JR_DISPLAY_JR_DISPLAY_H */
