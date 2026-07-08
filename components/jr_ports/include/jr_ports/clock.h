/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/clock.h — Clock port.
 *
 * A monotonic millisecond source. Injected everywhere a deadline is computed
 * (ReconnectPolicy backoff, KeepaliveMonitor staleness, NoReply watchdog) so
 * every time-dependent policy is testable against a fake clock with NO
 * wall-clock waits (architecture.md "Testability" §5).
 */
#ifndef JR_PORTS_CLOCK_H
#define JR_PORTS_CLOCK_H

#include "jr_ports/jr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jr_clock {
    void *ctx;
    /* Monotonic milliseconds since an arbitrary epoch. Never goes backwards. */
    uint64_t (*now_ms)(void *ctx);
} jr_clock_t;

static inline uint64_t jr_clock_now_ms(const jr_clock_t *c)
{
    return c->now_ms(c->ctx);
}

#ifdef __cplusplus
}
#endif

#endif /* JR_PORTS_CLOCK_H */
