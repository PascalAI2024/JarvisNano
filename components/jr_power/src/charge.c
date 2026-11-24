/* SPDX-License-Identifier: Apache-2.0 */
#include "jr_power/charge.h"

#include <string.h>

static const uint8_t addresses[JR_POWER_CHARGE_REG_COUNT] = {
    0x00, 0x01, 0x15, 0x16, 0x18, 0x50, 0x58, 0x59,
    0x5A, 0x5B, 0x61, 0x62, 0x63, 0x64, 0x65, 0x67
};
static const jr_power_charge_burst_t bursts[JR_POWER_CHARGE_BURST_COUNT] = {
    {0x00, 2, 0}, {0x15, 2, 2}, {0x18, 1, 4}, {0x50, 1, 5},
    {0x58, 4, 6}, {0x61, 5, 10}, {0x67, 1, 15}
};

static uint32_t elapsed_ms(uint64_t now, uint64_t then)
{
    const uint64_t delta = now >= then ? now - then : 0;
    return delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
}

void jr_power_charge_reset(jr_power_charge_t *sample)
{
    if (!sample) { return; }
    memset(sample, 0, sizeof(*sample));
    sample->age_ms = UINT32_MAX;
    for (size_t i = 0; i < JR_POWER_CHARGE_REG_COUNT; ++i) {
        sample->regs[i].address = addresses[i];
        sample->regs[i].attempt_age_ms = UINT32_MAX;
        sample->regs[i].last_success_age_ms = UINT32_MAX;
    }
}

void jr_power_charge_begin(jr_power_charge_t *sample, uint64_t now_ms)
{
    if (!sample) { return; }
    if (++sample->sample_seq == 0) { sample->sample_seq = 1; }
    sample->started_ms = now_ms;
    sample->completed_ms = 0;
    sample->span_ms = 0;
    sample->age_ms = UINT32_MAX;
    sample->complete = false;
    for (size_t i = 0; i < JR_POWER_CHARGE_REG_COUNT; ++i) {
        jr_power_charge_reg_t *reg = &sample->regs[i];
        reg->address = addresses[i];
        reg->raw = 0;
        reg->attempted = reg->read_ok = reg->available = false;
        reg->error = 0;
        reg->attempted_ms = 0;
        reg->attempt_age_ms = UINT32_MAX;
    }
    jr_power_charge_age(sample, now_ms);
}

void jr_power_charge_record(jr_power_charge_t *sample, size_t index,
                            uint8_t raw, int32_t error, uint64_t now_ms)
{
    if (!sample || index >= JR_POWER_CHARGE_REG_COUNT || sample->complete) {
        return;
    }
    jr_power_charge_reg_t *reg = &sample->regs[index];
    reg->attempted = true;
    reg->read_ok = error == 0;
    reg->available = reg->read_ok;
    reg->error = error;
    reg->raw = reg->read_ok ? raw : 0;
    reg->attempted_ms = now_ms;
    reg->attempt_age_ms = 0;
    if (reg->read_ok) {
        reg->has_success = true;
        reg->last_success_ms = now_ms;
        reg->last_success_age_ms = 0;
    } else {
        reg->last_success_age_ms = reg->has_success
            ? elapsed_ms(now_ms, reg->last_success_ms) : UINT32_MAX;
    }
}

void jr_power_charge_finish(jr_power_charge_t *sample, uint64_t now_ms)
{
    if (!sample) { return; }
    sample->completed_ms = now_ms;
    sample->span_ms = elapsed_ms(now_ms, sample->started_ms);
    sample->complete = true;
    jr_power_charge_age(sample, now_ms);
}

void jr_power_charge_age(jr_power_charge_t *sample, uint64_t now_ms)
{
    if (!sample) { return; }
    sample->age_ms = sample->complete
        ? elapsed_ms(now_ms, sample->completed_ms) : UINT32_MAX;
    for (size_t i = 0; i < JR_POWER_CHARGE_REG_COUNT; ++i) {
        jr_power_charge_reg_t *reg = &sample->regs[i];
        reg->attempt_age_ms = reg->attempted
            ? elapsed_ms(now_ms, reg->attempted_ms) : UINT32_MAX;
        reg->last_success_age_ms = reg->has_success
            ? elapsed_ms(now_ms, reg->last_success_ms) : UINT32_MAX;
        reg->available = reg->attempted && reg->read_ok && reg->error == 0 &&
                         reg->attempt_age_ms <= JR_POWER_CHARGE_MAX_AGE_MS;
    }
}

const jr_power_charge_burst_t *jr_power_charge_burst(size_t index)
{
    return index < JR_POWER_CHARGE_BURST_COUNT ? &bursts[index] : NULL;
}

static bool raw_at(const jr_power_charge_t *sample, size_t index, uint8_t *raw)
{
    const jr_power_charge_reg_t *reg = &sample->regs[index];
    if (!sample->complete || !reg->attempted || !reg->read_ok ||
        !reg->available || reg->error != 0) { return false; }
    *raw = reg->raw;
    return true;
}

static jr_power_charge_value_t value(uint32_t v)
{
    return (jr_power_charge_value_t){ .available = true, .value = v };
}

void jr_power_charge_decode(const jr_power_charge_t *sample,
                            jr_power_charge_decoded_t *out)
{
    if (!out) { return; }
    memset(out, 0, sizeof(*out));
    if (!sample) { return; }
    uint8_t raw;
    if (raw_at(sample, JR_POWER_CHARGE_STATUS1, &raw)) {
        out->usb_present = value((raw >> 5) & 1u);
        out->battery_present = value((raw >> 3) & 1u);
        out->thermal_regulation = value((raw >> 1) & 1u);
        out->current_limited = value(raw & 1u);
    }
    if (raw_at(sample, JR_POWER_CHARGE_STATUS2, &raw)) {
        const uint8_t dir = (raw >> 5) & 3u, phase = raw & 7u;
        if (dir <= 2) { out->direction = value(dir); }
        if (phase <= 5) { out->charge_phase = value(phase); }
        out->vindpm_active = value((raw >> 3) & 1u);
    }
    if (raw_at(sample, JR_POWER_CHARGE_VINDPM, &raw)) {
        out->vindpm_millivolts = value(3880u + 80u * (raw & 15u));
    }
    if (raw_at(sample, JR_POWER_CHARGE_INPUT_LIMIT, &raw)) {
        static const uint16_t limits[] = {100, 500, 900, 1000, 1500, 2000};
        const uint8_t code = raw & 7u;
        if (code < sizeof(limits) / sizeof(limits[0])) {
            out->input_limit_milliamps = value(limits[code]);
        }
    }
    if (raw_at(sample, JR_POWER_CHARGE_ENABLE, &raw)) {
        out->charge_enabled = value((raw >> 1) & 1u);
    }
    if (raw_at(sample, JR_POWER_CHARGE_PRECHARGE, &raw)) {
        const uint8_t code = raw & 15u;
        if (code <= 8) { out->precharge_milliamps = value(25u * code); }
    }
    if (raw_at(sample, JR_POWER_CHARGE_CC, &raw)) {
        const uint8_t code = raw & 31u;
        if (code <= 21) {
            out->constant_current_milliamps = value(code <= 8 ? 25u * code
                                                   : 200u + 100u * (code - 8u));
        }
    }
    if (raw_at(sample, JR_POWER_CHARGE_TERMINATION, &raw)) {
        const uint8_t code = raw & 15u;
        out->termination_enabled = value((raw >> 4) & 1u);
        if (code <= 8) { out->termination_milliamps = value(25u * code); }
    }
    if (raw_at(sample, JR_POWER_CHARGE_CV, &raw)) {
        static const uint16_t volts[] = {0, 4000, 4100, 4200, 4350, 4400};
        const uint8_t code = raw & 7u;
        if (code >= 1 && code <= 5) { out->cv_millivolts = value(volts[code]); }
    }
    if (raw_at(sample, JR_POWER_CHARGE_THERMAL, &raw)) {
        out->thermal_threshold_c = value(60u + 20u * (raw & 3u));
    }
    if (raw_at(sample, JR_POWER_CHARGE_TIMEOUT, &raw)) {
        static const uint8_t hours[] = {5, 8, 12, 20};
        out->precharge_timeout_enabled = value((raw >> 2) & 1u);
        out->precharge_timeout_minutes = value(40u + 10u * (raw & 3u));
        out->charge_timeout_enabled = value((raw >> 6) & 1u);
        out->charge_timeout_hours = value(hours[(raw >> 4) & 3u]);
        out->timeout_slows_when_limited = value((raw >> 7) & 1u);
    }
}
