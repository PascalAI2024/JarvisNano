/*
 * SPDX-FileCopyrightText: 2026 JarvisRobot
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* ui_layer — interactive, tappable UI for the round AMOLED.
 *
 * A SECOND display-arbiter owner that draws with the EXISTING display_hal
 * (no LVGL, no baked emote-gfx). The animated emote face yields the panel the
 * moment ui_layer acquires the arbiter, and repaints itself via the arbiter's
 * owner-changed callback the moment ui_layer releases.
 *
 * Threading contract (matches the documented render-lock deadlock rule):
 *   - display_hal is driven ONLY from ui_layer's own dedicated task.
 *   - Every public ui_layer_show / dismiss call merely POSTS a command to that
 *     task queue and returns immediately, so httpd / Gemini-tool / touch
 *     contexts never touch display_hal directly.
 *   - ui_layer_on_tap() runs the hit-test inline (cheap, no HAL draw beyond the
 *     highlight which is posted as a command) and is safe from the touch task.
 *
 * PSRAM budget: display_hal allocates ~1.3 MB of framebuffers on create.
 * ui_layer uses create-on-show / destroy-on-dismiss so those buffers are not
 * permanently resident alongside the emote strip buffers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of choice/menu options a single scene can carry. */
#define UI_LAYER_MAX_OPTIONS 6
/* Maximum number of label/value rows a data panel can carry. */
#define UI_LAYER_MAX_ROWS    5

typedef enum {
    UI_SCENE_NONE = 0,    /* no UI scene; emote face owns the panel */
    UI_SCENE_COCKPIT,     /* short boot/status frame; auto-dismisses */
    UI_SCENE_CHOICE_ARCS, /* tappable annular choice wedges */
    UI_SCENE_DATA_PANEL,  /* title + label/value rows */
    UI_SCENE_RADIAL_MENU, /* radial dial of selectable items */
    UI_SCENE_IMAGE,       /* a decoded JPEG presented once */
} ui_scene_t;

/* One-time init: grabs the board panel/io handles and starts the dedicated UI
 * task + command queue. Safe to call once at boot (idempotent — a second call
 * is a no-op that returns ESP_OK). Does NOT acquire the display; the panel
 * stays with the emote face until the first ui_layer_show_*. */
esp_err_t ui_layer_init(void);

/* ---- Scene requests (post a command, return immediately) ------------------ *
 * All of these are safe to call from any task. The actual arbiter acquire +
 * display_hal_create + render runs on the dedicated UI task. */

/* Show a tappable choice arc: the question at 12 o'clock and `n` annular wedges
 * (2..UI_LAYER_MAX_OPTIONS). Strings are copied internally. Clears any pending
 * result so a fresh tap can resolve it. */
esp_err_t ui_layer_show_choice(const char *question, const char *const *opts, int n);

/* Show a short boot cockpit/status frame, then automatically release the panel
 * back to the animated face. Used at boot to prove the display path is alive
 * without leaving a static UI scene that captures taps forever. */
esp_err_t ui_layer_show_cockpit(void);

/* Show a data panel: a title plus up to UI_LAYER_MAX_ROWS label/value rows.
 * `labels` and `values` are parallel arrays of length `n`. Either array entry
 * may be NULL (rendered blank). Strings are copied internally. */
esp_err_t ui_layer_show_data(const char *title, const char *const *labels,
                             const char *const *values, int n);

/* Show an image scene from an in-memory JPEG. The buffer is copied internally
 * (so the caller may free it after this returns). Pass `path` instead (with
 * jpeg=NULL,len=0) to load from a filesystem path; one of the two must be set. */
esp_err_t ui_layer_show_image(const uint8_t *jpeg, size_t len, const char *path);

/* Show a radial menu of selectable items (1..UI_LAYER_MAX_OPTIONS). Strings are
 * copied internally. A tap on an item resolves the result like a choice arc. */
esp_err_t ui_layer_show_menu(const char *const *items, int n);

/* Dismiss the current scene: the UI task releases the arbiter and destroys the
 * display_hal; the emote face repaints via the owner-changed callback. Safe to
 * call when no scene is active (no-op). */
esp_err_t ui_layer_dismiss(void);

/* ---- Touch + result --------------------------------------------------------*/

/* Hit-test a tap in panel coordinates (0..465, origin top-left). When a scene
 * with selectable regions is active and the tap lands on an option, the chosen
 * wedge is highlighted, the result is stored, the scene is dismissed, and any
 * result waiter is signalled. Returns the chosen 0-based index, or -1 if no
 * option was hit / no selectable scene is active. Safe from the touch task. */
int ui_layer_on_tap(int x, int y);

/* Poll the most recent selection result (for /api polling and the Gemini tool
 * worker). `out_index` receives the chosen 0-based index (valid only when
 * *out_done is true); `out_done` receives whether a selection has completed
 * since the scene was shown. Either out pointer may be NULL. Returns ESP_OK. */
esp_err_t ui_layer_get_result(int *out_index, bool *out_done);

/* True when a scene that should capture taps is active (the touch dispatcher
 * routes taps to ui_layer_on_tap instead of the Gemini toggle while true). */
bool ui_layer_is_active(void);

#ifdef __cplusplus
}
#endif
