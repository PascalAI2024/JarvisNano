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

/* Lightweight shell compositor over the animated face. A top-edge shade is
 * visible on-device; Agent Link occupies only the outer violet/status rim and
 * therefore never steals Listening/Speaking foreground. */
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
 * battery charge and device tilt. Cheap and lock-free (one packed word), so the
 * caller may push it from any task at any cadence — the flush path reads the
 * most recent value. Keeping the sensor components out of jr_display's
 * dependency list is deliberate: the composition root owns that wiring.
 *
 * batt_pct: 0..100, or 0xFF when no cell is present (the gauge then hides).
 * roll_deg/pitch_deg: straight from jr_imu; 0,0 disables tilt parallax. */
void jr_display_set_hud_env(uint8_t batt_pct, bool charging,
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
 * gates on the arc band, so taps on the face or the bezel ticks do NOT answer. */
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

#ifdef __cplusplus
}
#endif

#endif /* JR_DISPLAY_JR_DISPLAY_H */
