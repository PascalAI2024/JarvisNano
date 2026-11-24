/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed, read-only AXP2101 diagnostics. No interrupt/gauge programming address.
 * The seven bursts are spread across sampler passes, not an atomic PMIC view.
 * Settings describe configured limits, never measured charge current. */
#define JR_POWER_CHARGE_REG_COUNT 16u
#define JR_POWER_CHARGE_BURST_COUNT 7u
#define JR_POWER_CHARGE_MAX_AGE_MS 10000u

typedef enum {
    JR_POWER_CHARGE_STATUS1, JR_POWER_CHARGE_STATUS2,
    JR_POWER_CHARGE_VINDPM, JR_POWER_CHARGE_INPUT_LIMIT,
    JR_POWER_CHARGE_ENABLE, JR_POWER_CHARGE_TS,
    JR_POWER_CHARGE_JEITA, JR_POWER_CHARGE_TEMP1,
    JR_POWER_CHARGE_TEMP2, JR_POWER_CHARGE_TEMP3,
    JR_POWER_CHARGE_PRECHARGE, JR_POWER_CHARGE_CC,
    JR_POWER_CHARGE_TERMINATION, JR_POWER_CHARGE_CV,
    JR_POWER_CHARGE_THERMAL, JR_POWER_CHARGE_TIMEOUT
} jr_power_charge_index_t;

typedef struct {
    uint8_t address, raw;
    bool attempted, read_ok, available, has_success;
    int32_t error;                       /* driver error; 0 on success */
    uint32_t attempt_age_ms, last_success_age_ms; /* UINT32_MAX if never */
    uint64_t attempted_ms, last_success_ms;      /* monotonic timestamps */
} jr_power_charge_reg_t;

typedef struct {
    uint32_t sample_seq, span_ms, age_ms;
    uint64_t started_ms, completed_ms;
    bool complete;
    jr_power_charge_reg_t regs[JR_POWER_CHARGE_REG_COUNT];
} jr_power_charge_t;

typedef struct {
    uint8_t address, count, first_index;
} jr_power_charge_burst_t;

typedef struct {
    bool available;
    uint32_t value;                       /* booleans use 0/1 */
} jr_power_charge_value_t;

typedef struct {
    jr_power_charge_value_t usb_present, battery_present;
    jr_power_charge_value_t thermal_regulation, current_limited;
    jr_power_charge_value_t direction, charge_phase, vindpm_active;
    jr_power_charge_value_t vindpm_millivolts, input_limit_milliamps;
    jr_power_charge_value_t charge_enabled, precharge_milliamps;
    jr_power_charge_value_t constant_current_milliamps;
    jr_power_charge_value_t termination_enabled, termination_milliamps;
    jr_power_charge_value_t cv_millivolts, thermal_threshold_c;
    jr_power_charge_value_t precharge_timeout_enabled, precharge_timeout_minutes;
    jr_power_charge_value_t charge_timeout_enabled, charge_timeout_hours;
    jr_power_charge_value_t timeout_slows_when_limited;
} jr_power_charge_decoded_t;

/* Pure helpers. Only the sampler mutates its staging snapshot. Each begin
 * clears latest-attempt data while retaining actual last-success timestamps.
 * Publish only after finish; all-failed completed attempts still have a seq.
 * Copy/age must use the current monotonic time before decode. */
void jr_power_charge_reset(jr_power_charge_t *sample);
void jr_power_charge_begin(jr_power_charge_t *sample, uint64_t now_ms);
void jr_power_charge_record(jr_power_charge_t *sample, size_t index,
                            uint8_t raw, int32_t error, uint64_t now_ms);
void jr_power_charge_finish(jr_power_charge_t *sample, uint64_t now_ms);
void jr_power_charge_age(jr_power_charge_t *sample, uint64_t now_ms);
const jr_power_charge_burst_t *jr_power_charge_burst(size_t index);

/* Reserved codes and failed/never/stale registers produce unavailable fields.
 * direction: 0 standby, 1 charge, 2 discharge; phase: 0 trickle, 1 precharge,
 * 2 constant current, 3 constant voltage, 4 done, 5 not charging. */
void jr_power_charge_decode(const jr_power_charge_t *sample,
                            jr_power_charge_decoded_t *out);

#ifdef __cplusplus
}
#endif
