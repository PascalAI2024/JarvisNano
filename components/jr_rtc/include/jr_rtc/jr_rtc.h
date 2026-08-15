/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_rtc — PCF85063 on the shared Waveshare I2C bus (7-bit 0x51).
 *
 * The chip is on the board and was never claimed by v5. The watch face used
 * to require SNTP; this lets a desk puck keep time after a power cycle
 * without Wi-Fi. No IRQ, no alarm — read/write civil time only.
 */
#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t jr_rtc_start(void);
bool jr_rtc_present(void);
esp_err_t jr_rtc_get(struct tm *out);
esp_err_t jr_rtc_set(const struct tm *in);

#ifdef __cplusplus
}
#endif
