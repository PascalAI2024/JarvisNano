/* SPDX-License-Identifier: Apache-2.0 */
#include "unity.h"
#include "jr_power/charge.h"

#include <string.h>

static jr_power_charge_t sample;
static jr_power_charge_decoded_t decoded;

static void decode_byte(size_t index, uint8_t raw)
{
    jr_power_charge_reset(&sample);
    jr_power_charge_begin(&sample, 100);
    jr_power_charge_record(&sample, index, raw, 0, 101);
    jr_power_charge_finish(&sample, 102);
    jr_power_charge_decode(&sample, &decoded);
}

static void assert_value(jr_power_charge_value_t v, bool available, uint32_t n)
{
    TEST_ASSERT_EQUAL(available, v.available);
    TEST_ASSERT_EQUAL_UINT32(available ? n : 0, v.value);
}

static void assert_unavailable(void)
{
    const jr_power_charge_decoded_t empty = {0};
    TEST_ASSERT_EQUAL_MEMORY(&empty, &decoded, sizeof(empty));
}

static void test_charge_fixed_register_bursts(void)
{
    const uint8_t expected[] = {0x00,0x01,0x15,0x16,0x18,0x50,0x58,0x59,
                               0x5A,0x5B,0x61,0x62,0x63,0x64,0x65,0x67};
    unsigned seen[JR_POWER_CHARGE_REG_COUNT] = {0};
    jr_power_charge_reset(&sample);
    for (size_t i = 0; i < JR_POWER_CHARGE_BURST_COUNT; ++i) {
        const jr_power_charge_burst_t *b = jr_power_charge_burst(i);
        TEST_ASSERT_NOT_NULL(b);
        TEST_ASSERT_TRUE(b->count >= 1 && b->count <= 5);
        TEST_ASSERT_TRUE(b->first_index + b->count <= sizeof(expected));
        for (size_t j = 0; j < b->count; ++j) {
            const size_t index = b->first_index + j;
            TEST_ASSERT_EQUAL_HEX8(expected[index], b->address + j);
            TEST_ASSERT_EQUAL_HEX8(expected[index], sample.regs[index].address);
            ++seen[index];
        }
    }
    for (size_t i = 0; i < JR_POWER_CHARGE_REG_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT(1, seen[i]);
    }
    TEST_ASSERT_NULL(jr_power_charge_burst(JR_POWER_CHARGE_BURST_COUNT));
    TEST_ASSERT_NULL(jr_power_charge_burst(SIZE_MAX));
}

static void test_charge_never_and_partial_unavailable(void)
{
    jr_power_charge_reset(&sample);
    jr_power_charge_decode(&sample, &decoded);
    assert_unavailable();
    TEST_ASSERT_FALSE(sample.complete);
    TEST_ASSERT_EQUAL_UINT32(0, sample.sample_seq);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, sample.age_ms);
    for (size_t i = 0; i < JR_POWER_CHARGE_REG_COUNT; ++i) {
        TEST_ASSERT_FALSE(sample.regs[i].attempted);
        TEST_ASSERT_FALSE(sample.regs[i].has_success);
        TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, sample.regs[i].last_success_age_ms);
    }
    jr_power_charge_begin(&sample, 100);
    jr_power_charge_record(&sample, JR_POWER_CHARGE_CC, 9, 0, 101);
    jr_power_charge_decode(&sample, &decoded);
    assert_unavailable();
}

static void test_charge_status_all_bytes(void)
{
    for (unsigned r = 0; r < 256; ++r) {
        decode_byte(JR_POWER_CHARGE_STATUS1, (uint8_t)r);
        assert_value(decoded.usb_present, true, (r & 0x20u) != 0);
        assert_value(decoded.battery_present, true, (r & 0x08u) != 0);
        assert_value(decoded.thermal_regulation, true, (r & 0x02u) != 0);
        assert_value(decoded.current_limited, true, (r & 0x01u) != 0);
        decode_byte(JR_POWER_CHARGE_STATUS2, (uint8_t)r);
        const unsigned direction = (r & 0x60u) / 32u, phase = r & 7u;
        assert_value(decoded.direction, direction != 3, direction);
        assert_value(decoded.charge_phase, phase < 6, phase);
        assert_value(decoded.vindpm_active, true, (r & 0x08u) != 0);
    }
}

static void test_charge_input_limit_all_codes(void)
{
    const uint16_t expected[] = {100,500,900,1000,1500,2000,0,0};
    for (unsigned r = 0; r < 256; ++r) {
        decode_byte(JR_POWER_CHARGE_INPUT_LIMIT, (uint8_t)r);
        assert_value(decoded.input_limit_milliamps, (r & 7u) < 6,
                     expected[r & 7u]);
        decode_byte(JR_POWER_CHARGE_VINDPM, (uint8_t)r);
        assert_value(decoded.vindpm_millivolts, true, 3880 + 80 * (r & 15u));
        decode_byte(JR_POWER_CHARGE_ENABLE, (uint8_t)r);
        assert_value(decoded.charge_enabled, true, (r & 2u) != 0);
    }
}

static void test_charge_cc_all_codes_and_ignored_bits(void)
{
    const uint16_t expected[32] = {
        0,25,50,75,100,125,150,175,200,300,400,500,600,700,800,900,
        1000,1100,1200,1300,1400,1500,0,0,0,0,0,0,0,0,0,0
    };
    for (unsigned r = 0; r < 256; ++r) {
        decode_byte(JR_POWER_CHARGE_CC, (uint8_t)r);
        assert_value(decoded.constant_current_milliamps, (r & 31u) < 22,
                     expected[r & 31u]);
    }
}

static void test_charge_precharge_and_termination_reserved(void)
{
    for (unsigned r = 0; r < 256; ++r) {
        const unsigned code = r & 15u;
        decode_byte(JR_POWER_CHARGE_PRECHARGE, (uint8_t)r);
        assert_value(decoded.precharge_milliamps, code < 9, 25 * code);
        decode_byte(JR_POWER_CHARGE_TERMINATION, (uint8_t)r);
        assert_value(decoded.termination_enabled, true, (r & 16u) != 0);
        assert_value(decoded.termination_milliamps, code < 9, 25 * code);
    }
}

static void test_charge_cv_and_thermal_all_codes(void)
{
    const uint16_t expected[] = {0,4000,4100,4200,4350,4400,0,0};
    const uint8_t thermal[] = {60,80,100,120};
    for (unsigned r = 0; r < 256; ++r) {
        const unsigned code = r & 7u;
        decode_byte(JR_POWER_CHARGE_CV, (uint8_t)r);
        assert_value(decoded.cv_millivolts, code > 0 && code < 6, expected[code]);
        decode_byte(JR_POWER_CHARGE_THERMAL, (uint8_t)r);
        assert_value(decoded.thermal_threshold_c, true, thermal[r & 3u]);
    }
}

static void test_charge_timeout_all_bytes(void)
{
    const uint8_t hours[] = {5,8,12,20}, minutes[] = {40,50,60,70};
    for (unsigned r = 0; r < 256; ++r) {
        decode_byte(JR_POWER_CHARGE_TIMEOUT, (uint8_t)r);
        assert_value(decoded.precharge_timeout_enabled, true, (r & 4u) != 0);
        assert_value(decoded.precharge_timeout_minutes, true, minutes[r & 3u]);
        assert_value(decoded.charge_timeout_enabled, true, (r & 0x40u) != 0);
        assert_value(decoded.charge_timeout_hours, true, hours[(r / 16) & 3u]);
        assert_value(decoded.timeout_slows_when_limited, true, (r & 0x80u) != 0);
    }
}

static void test_charge_failed_attempt_preserves_only_success_age(void)
{
    decode_byte(JR_POWER_CHARGE_CC, 9);
    jr_power_charge_begin(&sample, 6000);
    jr_power_charge_record(&sample, JR_POWER_CHARGE_CC, 21, 0x107, 6100);
    jr_power_charge_record(&sample, JR_POWER_CHARGE_INPUT_LIMIT, 5, 0, 6200);
    jr_power_charge_finish(&sample, 6600);
    jr_power_charge_age(&sample, 7000);
    const jr_power_charge_reg_t *reg = &sample.regs[JR_POWER_CHARGE_CC];
    TEST_ASSERT_TRUE(reg->attempted);
    TEST_ASSERT_FALSE(reg->read_ok);
    TEST_ASSERT_FALSE(reg->available);
    TEST_ASSERT_EQUAL_HEX32(0x107, reg->error);
    TEST_ASSERT_EQUAL_HEX8(0, reg->raw);
    TEST_ASSERT_TRUE(reg->has_success);
    TEST_ASSERT_EQUAL_UINT32(900, reg->attempt_age_ms);
    TEST_ASSERT_EQUAL_UINT32(6899, reg->last_success_age_ms);
    TEST_ASSERT_EQUAL_UINT32(2, sample.sample_seq);
    TEST_ASSERT_EQUAL_UINT32(600, sample.span_ms);
    TEST_ASSERT_EQUAL_UINT32(400, sample.age_ms);
    jr_power_charge_decode(&sample, &decoded);
    assert_value(decoded.constant_current_milliamps, false, 0);
    assert_value(decoded.input_limit_milliamps, true, 2000);
}

static void test_charge_all_failed_then_retry_recovery(void)
{
    jr_power_charge_reset(&sample);
    jr_power_charge_begin(&sample, 0);
    for (size_t i = 0; i < JR_POWER_CHARGE_REG_COUNT; ++i) {
        jr_power_charge_record(&sample, i, 0xFF, -7, 20 + i);
    }
    jr_power_charge_finish(&sample, 40);
    TEST_ASSERT_TRUE(sample.complete);
    TEST_ASSERT_EQUAL_UINT32(1, sample.sample_seq);
    jr_power_charge_decode(&sample, &decoded);
    assert_unavailable();
    for (size_t i = 0; i < JR_POWER_CHARGE_REG_COUNT; ++i) {
        TEST_ASSERT_TRUE(sample.regs[i].attempted);
        TEST_ASSERT_FALSE(sample.regs[i].read_ok);
        TEST_ASSERT_EQUAL_INT32(-7, sample.regs[i].error);
        TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, sample.regs[i].last_success_age_ms);
    }
    jr_power_charge_begin(&sample, 5000);
    jr_power_charge_record(&sample, JR_POWER_CHARGE_CC, 21, 0, 5020);
    jr_power_charge_finish(&sample, 5040);
    jr_power_charge_age(&sample, 5060);
    TEST_ASSERT_EQUAL_UINT32(2, sample.sample_seq);
    TEST_ASSERT_EQUAL_UINT32(40, sample.regs[JR_POWER_CHARGE_CC].last_success_age_ms);
    TEST_ASSERT_EQUAL_INT32(0, sample.regs[JR_POWER_CHARGE_CC].error);
    jr_power_charge_decode(&sample, &decoded);
    assert_value(decoded.constant_current_milliamps, true, 1500);
}

static void test_charge_each_register_failure_is_isolated(void)
{
    const struct { size_t index, offset; } fields[] = {
#define FIELD(i, name) {i, offsetof(jr_power_charge_decoded_t, name)}
        FIELD(JR_POWER_CHARGE_STATUS1, usb_present),
        FIELD(JR_POWER_CHARGE_STATUS1, battery_present),
        FIELD(JR_POWER_CHARGE_STATUS1, thermal_regulation),
        FIELD(JR_POWER_CHARGE_STATUS1, current_limited),
        FIELD(JR_POWER_CHARGE_STATUS2, direction),
        FIELD(JR_POWER_CHARGE_STATUS2, charge_phase),
        FIELD(JR_POWER_CHARGE_STATUS2, vindpm_active),
        FIELD(JR_POWER_CHARGE_VINDPM, vindpm_millivolts),
        FIELD(JR_POWER_CHARGE_INPUT_LIMIT, input_limit_milliamps),
        FIELD(JR_POWER_CHARGE_ENABLE, charge_enabled),
        FIELD(JR_POWER_CHARGE_PRECHARGE, precharge_milliamps),
        FIELD(JR_POWER_CHARGE_CC, constant_current_milliamps),
        FIELD(JR_POWER_CHARGE_TERMINATION, termination_enabled),
        FIELD(JR_POWER_CHARGE_TERMINATION, termination_milliamps),
        FIELD(JR_POWER_CHARGE_CV, cv_millivolts),
        FIELD(JR_POWER_CHARGE_THERMAL, thermal_threshold_c),
        FIELD(JR_POWER_CHARGE_TIMEOUT, precharge_timeout_enabled),
        FIELD(JR_POWER_CHARGE_TIMEOUT, precharge_timeout_minutes),
        FIELD(JR_POWER_CHARGE_TIMEOUT, charge_timeout_enabled),
        FIELD(JR_POWER_CHARGE_TIMEOUT, charge_timeout_hours),
        FIELD(JR_POWER_CHARGE_TIMEOUT, timeout_slows_when_limited)
#undef FIELD
    };
    for (size_t bad = 0; bad < JR_POWER_CHARGE_REG_COUNT; ++bad) {
        jr_power_charge_reset(&sample);
        jr_power_charge_begin(&sample, 0);
        for (size_t i = 0; i < JR_POWER_CHARGE_REG_COUNT; ++i) {
            jr_power_charge_record(&sample, i, i == JR_POWER_CHARGE_CV ? 3 : 0,
                                   i == bad ? -7 : 0, 20 + i);
        }
        jr_power_charge_finish(&sample, 40);
        jr_power_charge_decode(&sample, &decoded);
        for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f) {
            const jr_power_charge_value_t *v = (const jr_power_charge_value_t *)
                ((const char *)&decoded + fields[f].offset);
            TEST_ASSERT_EQUAL(fields[f].index != bad, v->available);
        }
        for (size_t i = 0; i < JR_POWER_CHARGE_REG_COUNT; ++i) {
            TEST_ASSERT_EQUAL(i != bad, sample.regs[i].available);
            TEST_ASSERT_EQUAL_INT32(i == bad ? -7 : 0, sample.regs[i].error);
        }
    }
}

static void test_charge_freshness_uses_read_time_not_publish_time(void)
{
    jr_power_charge_reset(&sample);
    jr_power_charge_begin(&sample, 0);
    jr_power_charge_record(&sample, JR_POWER_CHARGE_CC, 9, 0, 0);
    jr_power_charge_record(&sample, JR_POWER_CHARGE_INPUT_LIMIT, 5, 0, 3000);
    jr_power_charge_finish(&sample, 3000);
    jr_power_charge_age(&sample, JR_POWER_CHARGE_MAX_AGE_MS);
    jr_power_charge_decode(&sample, &decoded);
    assert_value(decoded.constant_current_milliamps, true, 300);
    jr_power_charge_age(&sample, JR_POWER_CHARGE_MAX_AGE_MS + 1);
    jr_power_charge_decode(&sample, &decoded);
    assert_value(decoded.constant_current_milliamps, false, 0);
    assert_value(decoded.input_limit_milliamps, true, 2000);
    TEST_ASSERT_EQUAL_UINT32(7001, sample.age_ms);
    TEST_ASSERT_EQUAL_UINT32(10001, sample.regs[JR_POWER_CHARGE_CC].last_success_age_ms);
    TEST_ASSERT_TRUE(sample.regs[JR_POWER_CHARGE_CC].read_ok);
}

static void test_charge_age_saturates_and_sequence_skips_zero(void)
{
    decode_byte(JR_POWER_CHARGE_CC, 9);
    jr_power_charge_age(&sample, (uint64_t)UINT32_MAX + 10000u);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, sample.age_ms);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, sample.regs[JR_POWER_CHARGE_CC].attempt_age_ms);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, sample.regs[JR_POWER_CHARGE_CC].last_success_age_ms);
    TEST_ASSERT_FALSE(sample.regs[JR_POWER_CHARGE_CC].available);
    sample.sample_seq = UINT32_MAX;
    jr_power_charge_begin(&sample, (uint64_t)UINT32_MAX + 10001u);
    TEST_ASSERT_EQUAL_UINT32(1, sample.sample_seq);
    jr_power_charge_finish(&sample, (uint64_t)UINT32_MAX * 3u);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, sample.span_ms);
}

static void test_charge_invalid_and_inconsistent_metadata_is_unavailable(void)
{
    decode_byte(JR_POWER_CHARGE_CC, 9);
    jr_power_charge_t before = sample;
    jr_power_charge_record(&sample, SIZE_MAX, 21, 0, 110);
    jr_power_charge_record(&sample, JR_POWER_CHARGE_CC, 21, 0, 110);
    TEST_ASSERT_EQUAL_MEMORY(&before, &sample, sizeof(sample));
    sample.regs[JR_POWER_CHARGE_CC].error = -1;
    jr_power_charge_decode(&sample, &decoded);
    assert_unavailable();
    sample.regs[JR_POWER_CHARGE_CC].error = 0;
    sample.regs[JR_POWER_CHARGE_CC].read_ok = false;
    jr_power_charge_decode(&sample, &decoded);
    assert_unavailable();
    jr_power_charge_decode(NULL, &decoded);
    assert_unavailable();
    jr_power_charge_reset(NULL);
    jr_power_charge_begin(NULL, 0);
    jr_power_charge_record(NULL, 0, 0, 0, 0);
    jr_power_charge_finish(NULL, 0);
    jr_power_charge_age(NULL, 0);
    jr_power_charge_decode(&sample, NULL);
}

void power_charge_tests_run(void)
{
    RUN_TEST(test_charge_fixed_register_bursts);
    RUN_TEST(test_charge_never_and_partial_unavailable);
    RUN_TEST(test_charge_status_all_bytes);
    RUN_TEST(test_charge_input_limit_all_codes);
    RUN_TEST(test_charge_cc_all_codes_and_ignored_bits);
    RUN_TEST(test_charge_precharge_and_termination_reserved);
    RUN_TEST(test_charge_cv_and_thermal_all_codes);
    RUN_TEST(test_charge_timeout_all_bytes);
    RUN_TEST(test_charge_failed_attempt_preserves_only_success_age);
    RUN_TEST(test_charge_all_failed_then_retry_recovery);
    RUN_TEST(test_charge_each_register_failure_is_isolated);
    RUN_TEST(test_charge_freshness_uses_read_time_not_publish_time);
    RUN_TEST(test_charge_age_saturates_and_sequence_skips_zero);
    RUN_TEST(test_charge_invalid_and_inconsistent_metadata_is_unavailable);
}
