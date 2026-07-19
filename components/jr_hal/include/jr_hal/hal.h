/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_hal/hal.h — L0 HAL / Board Support public surface.
 *
 * This is the ONLY component allowed to speak BOTH vocabularies: ESP-IDF
 * (esp_err_t, esp_board_manager, esp_timer, the drivers) on the inside and the
 * pure jr_ports interfaces on the outside. It hands the composition root
 * ready-to-inject port structs; the core never sees an IDF type.
 *
 * The CST9217 adapter resolves the board-manager-owned touch handle, consumes
 * the board factory's IRQ semaphore from one worker, and exposes debounced
 * gestures through the provider-neutral Input port. The CO5300 presenter is
 * kept behind the same boundary.
 */
#ifndef JR_HAL_HAL_H
#define JR_HAL_HAL_H

#include "esp_err.h"
#include "jr_ports/clock.h"
#include "jr_ports/display.h"
#include "jr_ports/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the board up (esp_board_manager: TCA9554 -> AXP2101 -> display ->
 * touch). Returns ESP_OK on success. */
esp_err_t jr_hal_init(void);

/* Concrete port factories — each returns a struct the composition root injects
 * into the core. Valid only after jr_hal_init() (except the clock, which is
 * always valid). */
jr_clock_t   jr_hal_clock(void);
jr_display_t jr_hal_display(void);
jr_input_t   jr_hal_input(void);

#ifdef __cplusplus
}
#endif

#endif /* JR_HAL_HAL_H */
