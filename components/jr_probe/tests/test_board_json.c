/* SPDX-License-Identifier: Apache-2.0 */
/* Prints the report JSON for one scenario chosen by argv[1]. Compiled by
 * test_board_json.py against the production board_json.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jr_probe/board_probe.h"

static void fill_full(jr_probe_report_t *r)
{
    memset(r, 0, sizeof *r);
    r->te = (jr_probe_te_t){ .ok = true, .window_ms = 500, .edges = 30, .hz = 60,
                             .period_us_min = 16600, .period_us_max = 16700,
                             .level_now = 1 };
    r->pwr_key = (jr_probe_pwr_t){ .ok = true, .level = 0, .watched = true,
                                   .watch_ms = 3000, .transitions = 2,
                                   .high_ms = 640, .level_min = 0, .level_max = 1 };
    r->imu = (jr_probe_imu_t){ .ok = true, .who = 5, .rev = 0x7C, .ctrl1 = 0x40,
                               .ctrl2 = 0x26, .ctrl3 = 0, .ctrl7 = 1,
                               .temp_ok = true, .temp_tenths = -53,
                               .gyro_asked = true, .gyro_ran = true, .gyro_n = 20,
                               .gmin = { -0.5f, 0.25f, -1.0f },
                               .gmax = { 0.75f, 1.5f, 2.0f },
                               .gmean = { 0.125f, 0.5f, 0.0f },
                               .restored = true, .ctrl3_after = 0, .ctrl7_after = 1 };
    r->touch = (jr_probe_touch_t){ .ok = true, .watched = true, .watch_ms = 4000,
                                   .reads = 17, .max_fingers = 2, .seen = true,
                                   .ack = 0xAB };
    for (unsigned i = 0; i < JR_PROBE_TOUCH_RAW_LEN; ++i) {
        r->touch.raw[i] = (uint8_t)(0xA0 + i);
    }
    r->pmic = (jr_probe_pmic_t){ .ok = true, .adc_before = 1, .adc_during = 31,
                                 .adc_after = 1, .adc_restored = true,
                                 .chg_cc_raw = 0x0A, .chg_cc_ma = 400,
                                 .chg_cc_known = true,
                                 .die_ok = true, .die_tenths = 412,
                                 .vbus_ok = true, .vbus_mv = 5012,
                                 .vsys_ok = true, .vsys_mv = 4180,
                                 .vbat_ok = true, .vbat_mv = 4090,
                                 .ts_ok = true, .ts_raw = 1001 };
    r->spi_hz = 40000000u;
    r->spi_source = "macro";
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "full";
    jr_probe_report_t r;
    fill_full(&r);
    char buf[2048];
    size_t cap = sizeof buf;

    if (strcmp(mode, "failed") == 0) {
        memset(&r, 0, sizeof r);
        r.te.why = "isr add failed";
        r.pwr_key.why = "gpio_set_direction failed";
        r.imu.why = "imu not present";
        r.touch.why = "touch worker not running";
        r.pmic.why = "pmic not present";
        r.spi_hz = 40000000u;
    } else if (strcmp(mode, "quiet") == 0) {
        r.te.edges = 0; r.te.hz = 0;
        r.pwr_key.watched = false;
        r.imu.gyro_asked = false;
        r.imu.temp_tenths = -5;
        r.touch.watched = false;
        r.pmic.die_tenths = -7;
    } else if (strcmp(mode, "refused") == 0) {
        r.imu.gyro_ran = false;
        r.imu.gyro_why = "refused: wake-on-motion armed";
        r.imu.wom = true;
    } else if (strcmp(mode, "dirty") == 0) {
        r.imu.restored = false;
        r.imu.ctrl3_after = 0x56;
    } else if (strcmp(mode, "small") == 0) {
        cap = 200;
    }
    memset(buf, 'X', sizeof buf);
    const int n = jr_probe_report_json(&r, buf, cap);
    if (n < 0 && memchr(buf, 0, cap) == NULL) { return 3; }   /* must stay terminated */
    printf("%d\n%s", n, n < 0 ? "" : buf);
    return 0;
}
