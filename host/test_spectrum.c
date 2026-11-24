/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_spectrum.c — pure host unit tests for the 16-band fixed-point
 * audio spectrum analyzer (jr_dsp/spectrum.h).
 */
#include "unity.h"
#include "jr_dsp/spectrum.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void fill_sine(int16_t *buf, size_t n, double freq_hz, double fs_hz,
                      double amp)
{
    for (size_t i = 0; i < n; ++i) {
        double v = amp * sin(2.0 * M_PI * freq_hz * (double)i / fs_hz);
        if (v > 32767.0) { v = 32767.0; }
        if (v < -32768.0) { v = -32768.0; }
        buf[i] = (int16_t)lrint(v);
    }
}

static uint32_t band_max(const uint8_t bands[JR_SPECTRUM_BANDS],
                         size_t lo, size_t hi)
{
    uint32_t m = 0U;
    for (size_t i = lo; i <= hi && i < JR_SPECTRUM_BANDS; ++i) {
        if (bands[i] > m) {
            m = bands[i];
        }
    }
    return m;
}

static uint32_t band_sum(const uint8_t bands[JR_SPECTRUM_BANDS],
                         size_t lo, size_t hi)
{
    uint32_t sum = 0U;
    for (size_t i = lo; i <= hi && i < JR_SPECTRUM_BANDS; ++i) {
        sum += bands[i];
    }
    return sum;
}

static void test_spectrum_silence_is_zero(void)
{
    jr_spectrum_t s;
    jr_spectrum_init(&s);

    int16_t zeros[128];
    memset(zeros, 0, sizeof zeros);
    for (int f = 0; f < 8; ++f) {
        jr_spectrum_feed(&s, zeros, 128U);
    }
    for (size_t i = 0; i < JR_SPECTRUM_BANDS; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0U, s.bands[i]);
    }
}

static void test_spectrum_low_freq_peaks_in_lower_bands(void)
{
    jr_spectrum_t s;
    jr_spectrum_init(&s);

    /* ~375 Hz sine wave at 16 kHz (64-pt window -> bin ~1.5 -> bands 0..4). */
    int16_t pcm[64];
    fill_sine(pcm, 64U, 375.0, 16000.0, 18000.0);
    for (int f = 0; f < 6; ++f) {
        jr_spectrum_feed(&s, pcm, 64U);
    }

    const uint32_t low_peak = band_max(s.bands, 0U, 4U);
    const uint32_t high_peak = band_max(s.bands, 10U, 15U);
    TEST_ASSERT_GREATER_THAN_UINT32(40U, low_peak);
    TEST_ASSERT_GREATER_THAN_UINT32(high_peak, low_peak);
}

static void test_spectrum_high_freq_peaks_in_upper_bands(void)
{
    jr_spectrum_t s;
    jr_spectrum_init(&s);

    /* ~4500 Hz sine wave at 16 kHz (64-pt window -> bin 18 -> bands 10..15). */
    int16_t pcm[64];
    fill_sine(pcm, 64U, 4500.0, 16000.0, 18000.0);
    for (int f = 0; f < 6; ++f) {
        jr_spectrum_feed(&s, pcm, 64U);
    }

    const uint32_t low_peak = band_max(s.bands, 0U, 4U);
    const uint32_t high_peak = band_max(s.bands, 10U, 15U);
    TEST_ASSERT_GREATER_THAN_UINT32(40U, high_peak);
    TEST_ASSERT_GREATER_THAN_UINT32(low_peak, high_peak);
}

static void test_spectrum_smooth_decay_on_null_feed(void)
{
    jr_spectrum_t s;
    jr_spectrum_init(&s);

    int16_t pcm[64];
    fill_sine(pcm, 64U, 1000.0, 16000.0, 20000.0);
    for (int f = 0; f < 6; ++f) {
        jr_spectrum_feed(&s, pcm, 64U);
    }
    const uint32_t initial_energy = band_sum(s.bands, 0U, JR_SPECTRUM_BANDS - 1U);
    TEST_ASSERT_GREATER_THAN_UINT32(50U, initial_energy);

    /* One NULL feed should decay smoothly (neither stuck nor instantly wiped). */
    jr_spectrum_feed(&s, NULL, 0U);
    const uint32_t after_one = band_sum(s.bands, 0U, JR_SPECTRUM_BANDS - 1U);
    TEST_ASSERT_LESS_THAN_UINT32(initial_energy, after_one);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, after_one);

    /* Repeated NULL feeds decay monotonically all the way down to zero. */
    uint32_t prev = after_one;
    for (int f = 0; f < 32; ++f) {
        jr_spectrum_feed(&s, NULL, 0U);
        const uint32_t cur = band_sum(s.bands, 0U, JR_SPECTRUM_BANDS - 1U);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(prev, cur);
        prev = cur;
    }
    TEST_ASSERT_EQUAL_UINT32(0U, prev);
}

static void test_spectrum_reset_zeroes_cleanly(void)
{
    jr_spectrum_t s;
    jr_spectrum_init(&s);

    int16_t pcm[64];
    fill_sine(pcm, 64U, 2000.0, 16000.0, 22000.0);
    for (int f = 0; f < 5; ++f) {
        jr_spectrum_feed(&s, pcm, 64U);
    }
    TEST_ASSERT_GREATER_THAN_UINT32(40U, band_sum(s.bands, 0U, JR_SPECTRUM_BANDS - 1U));

    jr_spectrum_reset(&s);
    for (size_t i = 0; i < JR_SPECTRUM_BANDS; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0U, s.bands[i]);
    }
}

void spectrum_tests_run(void)
{
    RUN_TEST(test_spectrum_silence_is_zero);
    RUN_TEST(test_spectrum_low_freq_peaks_in_lower_bands);
    RUN_TEST(test_spectrum_high_freq_peaks_in_upper_bands);
    RUN_TEST(test_spectrum_smooth_decay_on_null_feed);
    RUN_TEST(test_spectrum_reset_zeroes_cleanly);
}
