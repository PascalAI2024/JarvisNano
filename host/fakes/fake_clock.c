/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/fakes/fake_clock.c — Clock port test double.
 */
#include "fake_clock.h"

static uint64_t s_now_ms = 0;

static uint64_t fake_now_ms(void *ctx)
{
    (void)ctx;
    return s_now_ms;
}

jr_clock_t fake_clock_make(void)
{
    jr_clock_t c;
    c.ctx = NULL;
    c.now_ms = fake_now_ms;
    return c;
}

void fake_clock_advance(uint64_t ms) { s_now_ms += ms; }
void fake_clock_reset(void)          { s_now_ms = 0; }
