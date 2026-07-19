/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_dsp/dsp.c — pure DSP math. Links only the C standard library.
 */
#include "jr_dsp/dsp.h"
#include <math.h>

float jr_dsp_rms(const int16_t *samples, size_t n)
{
    if (samples == NULL || n == 0) {
        return 0.0f;
    }
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double s = (double)samples[i];
        acc += s * s;
    }
    return (float)sqrt(acc / (double)n);
}
