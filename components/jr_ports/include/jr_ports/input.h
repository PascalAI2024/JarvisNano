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

typedef struct {
    jr_input_kind_t kind;
    uint16_t x;
    uint16_t y;
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
