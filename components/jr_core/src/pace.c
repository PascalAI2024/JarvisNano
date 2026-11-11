/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/pace.c - see pace.h.
 */
#include "jr_core/pace.h"

bool jr_pace_skip_sleep(uint32_t wait_us, uint32_t work_us,
                        uint32_t skips_in_row)
{
    if (skips_in_row >= JR_PACE_MAX_SKIPS) {
        return false;
    }
    const bool read_did_not_wait =
        wait_us != JR_PACE_READ_UNKNOWN && wait_us < JR_PACE_NO_WAIT_US;
    return read_did_not_wait || work_us >= JR_PACE_FRAME_US;
}
