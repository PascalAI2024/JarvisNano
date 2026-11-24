/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_COMPANION_ITEM_CHARS 24
#define JR_COMPANION_LINE_COUNT 4

typedef enum {
    JR_COMPANION_OFF,
    JR_COMPANION_NIGHTSTAND,
    JR_COMPANION_GLANCE,
} jr_companion_mode_t;

typedef enum {
    JR_COMPANION_NEXT_UNKNOWN,
    JR_COMPANION_NEXT_NONE,
    JR_COMPANION_NEXT_VALID,
    JR_COMPANION_NEXT_STALE,
} jr_companion_next_state_t;

/* Main owns the request, local-time/night policy and cached future-item feed.
 * idle means no active conversation/work; this helper chooses no timeout.
 * foreground includes asks, captions, controls, canvas and diagnostic work.
 * A next_item need not be terminated: at most ITEM_CHARS bytes are read. */
typedef struct {
    jr_companion_mode_t requested;
    bool usb_present, idle, foreground, operator_active;
    bool ota_active, probation, privacy_muted, charging;
    bool time_valid;
    uint8_t hour, minute;
    uint8_t night_start_hour, night_end_hour;
    uint8_t battery_percent; /* >100 means unknown */
    jr_companion_next_state_t next_state;
    char next_item[JR_COMPANION_ITEM_CHARS + 1];
} jr_companion_input_t;

/* Matches the existing surface font: five ink columns in a 6x7 cell.
 * Empty lines have zero geometry. All visible rectangles are inside r168;
 * the renderer must keep its battery/privacy rims outside these rectangles. */
typedef struct {
    char text[JR_COMPANION_ITEM_CHARS + 1];
    int16_t x, y;
    uint8_t scale;
} jr_companion_line_t;

typedef struct {
    jr_companion_mode_t mode;
    bool night, privacy_muted, charging;
    uint8_t battery_percent;
    uint16_t background_rgb565, ink_rgb565;
    jr_companion_line_t lines[JR_COMPANION_LINE_COUNT];
} jr_companion_view_t;

/* Pure, allocation-free normal-colour layout. No IDMON, brightness, timers,
 * transport, navigation or privacy mutation. False clears out_view to OFF.
 * night is a half-open local-hour interval; equal endpoints disable it.
 * The caller applies its brightness policy and draws authoritative rims. */
bool jr_companion_build(const jr_companion_input_t *input,
                        jr_companion_view_t *out_view);

/* Sparse invalidation key for these initialized views, independent of padding.
 * Caller brightness/diagnostic refresh requests remain additional guards. */
bool jr_companion_view_equal(const jr_companion_view_t *a,
                             const jr_companion_view_t *b);

#ifdef __cplusplus
}
#endif
