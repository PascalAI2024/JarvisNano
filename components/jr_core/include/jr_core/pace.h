/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/pace.h - should the voice loop sleep its tick after this frame?
 *
 * WHY. The loop ends with a one-tick vTaskDelay so IDLE1 runs and the task
 * watchdog stays fed. CONFIG_FREERTOS_HZ=100, so a tick is up to 10 ms. When
 * the loop is on time that sleep hides inside the codec read, which blocks
 * for the rest of the 32 ms frame anyway. When the loop is behind, the next
 * frame is already waiting and the sleep just adds 10 ms to a loop that
 * cannot afford it (measured: 18 fps instead of 31 while speaking).
 *
 * THE RULE. Skip the sleep only when behind, and never more than
 * JR_PACE_MAX_SKIPS times in a row: a few skipped ticks cost IDLE1 nothing
 * (it still gets a tick every fourth frame, far inside the watchdog window),
 * an unbounded skip would starve it.
 *
 * Pure: no clock, no I/O.
 */
#ifndef JR_CORE_PACE_H
#define JR_CORE_PACE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_PACE_FRAME_US      32000u  /* 512 samples at 16 kHz */
/* A codec read that returns in under this did not wait: the frame was already
 * there. A paced read that is on time blocks for most of a frame. */
#define JR_PACE_NO_WAIT_US    2000u
#define JR_PACE_MAX_SKIPS     3u

#define JR_PACE_READ_UNKNOWN  UINT32_MAX

/* wait_us:  how long the codec read was BLOCKED, or JR_PACE_READ_UNKNOWN.
 *           Not the read's duration: the canceller runs inside the read and
 *           takes 12 to 15 ms while speaking, which is work, not waiting.
 * work_us:  the rest of this iteration (iteration time minus the wait).
 * skips_in_row: consecutive skips so far, as the caller counts them.
 * Returns true to skip the sleep. The caller sets
 *   skips_in_row = skip ? skips_in_row + 1 : 0. */
bool jr_pace_skip_sleep(uint32_t wait_us, uint32_t work_us,
                        uint32_t skips_in_row);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_PACE_H */
