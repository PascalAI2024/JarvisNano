/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_cue — quiet interface sounds. Pure; see the header.
 *
 * Every cue is a sine (or two) under a raised-cosine attack and release, so
 * the first and last samples are exactly zero and no edge clicks. Phase is
 * integrated per sample, which keeps a sweep clean.
 */
#include "jr_core/cue.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RATE ((float)JR_CUE_SAMPLE_RATE_HZ)

static size_t ms_to_samples(uint32_t ms)
{
    return (size_t)JR_CUE_SAMPLE_RATE_HZ * ms / 1000u;
}

bool jr_cue_phase_blocks(jr_state_t phase)
{
    return phase == JR_ST_LISTENING || phase == JR_ST_THINKING ||
           phase == JR_ST_SPEAKING || phase == JR_ST_ASKING;
}

bool jr_cue_allow(jr_cue_gate_t *gate, const jr_cue_ctx_t *ctx,
                  jr_cue_kind_t kind, jr_cue_source_t source)
{
    if ((unsigned)kind >= (unsigned)JR_CUE_KIND_COUNT) return false;
    if (source != JR_CUE_SOURCE_PHYSICAL) return false;
    if (!ctx->ui_sounds) return false;
    if (ctx->owner_muted) return false;
    if (jr_cue_phase_blocks(ctx->phase)) return false;
    if (ctx->mood == JR_MOOD_WHISPER || ctx->mood == JR_MOOD_DREAM) return false;
    if (ctx->ota_active) return false;
    if (kind == JR_CUE_TICK) {
        if (gate->tick_seen &&
            (uint32_t)(ctx->now_ms - gate->last_tick_ms) <
                JR_CUE_TICK_MIN_GAP_MS) {
            return false;
        }
        gate->tick_seen = true;
        gate->last_tick_ms = ctx->now_ms;
    }
    return true;
}

uint16_t jr_cue_amplitude_permille(jr_cue_kind_t kind, int owner_volume)
{
    static const uint16_t base[JR_CUE_KIND_COUNT] = {
        [JR_CUE_TICK] = 20, [JR_CUE_CONFIRM] = 40,
        [JR_CUE_DISMISS] = 35, [JR_CUE_LIMIT] = 40,
    };
    if ((unsigned)kind >= (unsigned)JR_CUE_KIND_COUNT) return 0;
    if (owner_volume < 10) owner_volume = 10;
    if (owner_volume > 100) owner_volume = 100;
    /* 55 % of the base at volume 10 up to the full base at 100. The codec's
     * own volume applies on top, so the owner's setting is felt twice: a cue
     * is always the quietest thing the device does. */
    uint32_t scaled = (uint32_t)base[kind] * (uint32_t)(50 + owner_volume / 2) / 100u;
    if (scaled > JR_CUE_MAX_PERMILLE) scaled = JR_CUE_MAX_PERMILLE;
    return (uint16_t)scaled;
}

size_t jr_cue_samples(jr_cue_kind_t kind)
{
    switch (kind) {
    case JR_CUE_TICK:    return ms_to_samples(20);
    case JR_CUE_CONFIRM: return ms_to_samples(90);
    case JR_CUE_DISMISS: return ms_to_samples(70);
    case JR_CUE_LIMIT:   return ms_to_samples(50);
    default:             return 0;
    }
}

/* One note: a sine gliding f0 -> f1 (Hz), optionally with a second harmonic,
 * under a raised-cosine attack and release (in samples) and an exponential
 * decay with time constant `tau_ms` (0 = none). */
static void render_note(int16_t *out, size_t n, float f0, float f1,
                        float harmonic, size_t attack, size_t release,
                        float tau_ms, float amp)
{
    if (attack + release > n) attack = release = n / 2u;
    if (attack == 0u) attack = 1u;
    if (release == 0u) release = 1u;
    const float norm = 1.0f / (1.0f + harmonic);
    float phase = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float t = (float)i / RATE;
        const float f = f0 + (f1 - f0) * ((float)i / (float)n);
        float env = 1.0f;
        if (i < attack) {
            env = 0.5f * (1.0f - cosf((float)M_PI * (float)i / (float)attack));
        } else if (n - i <= release) {
            env = 0.5f * (1.0f - cosf((float)M_PI * (float)(n - i) / (float)release));
        }
        if (tau_ms > 0.0f) env *= expf(-t * 1000.0f / tau_ms);
        const float s = (sinf(phase) + harmonic * sinf(2.0f * phase)) * norm;
        out[i] = (int16_t)(amp * env * s);
        phase += 2.0f * (float)M_PI * f / RATE;
    }
    out[0] = 0;
    out[n - 1u] = 0;
}

size_t jr_cue_render(jr_cue_kind_t kind, uint16_t amp_permille,
                     int16_t *out, size_t cap)
{
    const size_t total = jr_cue_samples(kind);
    if (total == 0u || out == NULL || cap < total) return 0u;
    if (amp_permille > JR_CUE_MAX_PERMILLE) amp_permille = JR_CUE_MAX_PERMILLE;
    const float amp = 32767.0f * (float)amp_permille / 1000.0f;

    switch (kind) {
    case JR_CUE_TICK:
        /* 2 kHz, 1 ms attack, 4 ms release, decay time constant 4 ms. */
        render_note(out, total, 2000.0f, 2000.0f, 0.0f,
                    ms_to_samples(1), ms_to_samples(4), 4.0f, amp);
        break;
    case JR_CUE_CONFIRM: {
        /* G5 then D6: a rising fifth, 45 ms each, 6 ms ramps. Each note
         * starts at phase zero under a zero-slope ramp: no click between. */
        const size_t half = total / 2u;
        render_note(out, half, 784.0f, 784.0f, 0.0f,
                    ms_to_samples(6), ms_to_samples(6), 0.0f, amp);
        render_note(out + half, total - half, 1175.0f, 1175.0f, 0.0f,
                    ms_to_samples(6), ms_to_samples(6), 0.0f, amp);
        break;
    }
    case JR_CUE_DISMISS:
        /* One falling note, A5 gliding to D5, 8 ms ramps, gentle decay. */
        render_note(out, total, 880.0f, 587.0f, 0.0f,
                    ms_to_samples(8), ms_to_samples(8), 60.0f, amp);
        break;
    case JR_CUE_LIMIT:
        /* One dull low note, C4 with a soft second harmonic so a small
         * speaker can carry it, quick decay. */
        render_note(out, total, 262.0f, 262.0f, 0.35f,
                    ms_to_samples(6), ms_to_samples(6), 25.0f, amp);
        break;
    default:
        return 0u;
    }
    return total;
}
