/* SPDX-License-Identifier: Apache-2.0
 *
 * jr_probe/gpio_probe.h - the two pin probes: LCD_TE (GPIO13) and the PWR-key
 * mirror (GPIO3). Input only, no pulls set, no panel command. Callable from
 * any task, one at a time (the httpd task is single threaded).
 */
#pragma once

#include "jr_probe/board_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Count rising edges on GPIO13 for `window_ms` (<= 1000). Leaves the pin an
 * input with its interrupt off and the handler removed. */
void jr_probe_te(uint32_t window_ms, jr_probe_te_t *out);

/* Read GPIO3 now; with watch_ms > 0 poll it for that long (<= 5000) and count
 * level changes. Only gpio_set_direction(INPUT) is applied: no pull, no drive. */
void jr_probe_pwr_key(uint32_t watch_ms, jr_probe_pwr_t *out);

#ifdef __cplusplus
}
#endif
