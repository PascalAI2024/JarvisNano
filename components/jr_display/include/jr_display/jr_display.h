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

#ifdef __cplusplus
}
#endif

#endif /* JR_DISPLAY_JR_DISPLAY_H */
