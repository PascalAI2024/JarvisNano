/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/input.h — Input port (L0 touch/gesture side).
 *
 * CST9217 touch is interrupt-driven (hardware.md non-negotiable #9): the
 * adapter is event/ISR-driven off the INT line, never a polling loop, and
 * surfaces discrete input events to the core. QMI8658 IMU gestures (Phase 4)
 * enter through the same port.
 */
#ifndef JR_PORTS_INPUT_H
#define JR_PORTS_INPUT_H

#include "jr_ports/jr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JR_INPUT_NONE = 0,
    JR_INPUT_TAP,
    JR_INPUT_LONG_PRESS,
    JR_INPUT_SWIPE,
} jr_input_kind_t;

typedef enum {
    JR_INPUT_DIRECTION_NONE = 0,
    JR_INPUT_DIRECTION_LEFT,
    JR_INPUT_DIRECTION_RIGHT,
    JR_INPUT_DIRECTION_UP,
    JR_INPUT_DIRECTION_DOWN,
} jr_input_direction_t;

enum {
    /* The press began in the display's top-edge gesture affordance. */
    JR_INPUT_FLAG_TOP_EDGE = 1U << 0,
};

typedef struct {
    jr_input_kind_t kind;
    /* Compatibility coordinates: always equal to end_x/end_y. */
    uint16_t x;
    uint16_t y;
    uint16_t start_x;
    uint16_t start_y;
    uint16_t end_x;
    uint16_t end_y;
    int16_t delta_x;
    int16_t delta_y;
    uint32_t duration_ms;
    jr_input_direction_t direction;
    uint8_t flags;
} jr_input_event_t;

typedef struct jr_input {
    void *ctx;
    /* Non-blocking poll of the event queue the ISR feeds.
     * Returns true and fills out when an event is available. */
    bool (*poll)(void *ctx, jr_input_event_t *out);
} jr_input_t;

static inline bool jr_input_poll(const jr_input_t *i, jr_input_event_t *out)
{
    return i->poll(i->ctx, out);
}

#ifdef __cplusplus
}
#endif

#endif /* JR_PORTS_INPUT_H */
