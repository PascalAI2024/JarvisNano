/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_rtc — PCF85063 on the shared Waveshare I2C bus (7-bit 0x51).
 *
 * The chip exists only on the original 1.75 compatibility board; the 1.75C
 * composition deliberately does not start this component and uses SNTP. On
 * the original board this provides read/write civil time across power cycles.
 * No IRQ or alarm support.
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
