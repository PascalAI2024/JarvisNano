/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/fakes/fake_clock.h — a test double for the Clock port.
 *
 * Demonstrates the port-fake seam: on device the Clock is an esp_timer
 * adapter; on host it is this settable counter, so every backoff/keepalive/
 * watchdog test runs with NO wall-clock waits (architecture.md §Testability §5).
 */
#ifndef HOST_FAKE_CLOCK_H
#define HOST_FAKE_CLOCK_H

#include "jr_ports/clock.h"

/* Returns a jr_clock_t whose now_ms() reads a mutable counter starting at 0. */
jr_clock_t fake_clock_make(void);

/* Advance the fake clock (all clocks made by fake_clock_make share the counter
 * for Phase-0 simplicity; Phase 1 can give each its own state). */
void fake_clock_advance(uint64_t ms);
void fake_clock_reset(void);

#endif /* HOST_FAKE_CLOCK_H */
