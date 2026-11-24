/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_cue — quiet interface sounds. Pure; see the header.
 *
 * Every cue is synthesized from 1..3 phase-integrated multi-harmonic voices
 * under raised-cosine attack/release envelopes and exponential decay, so
 * edges never click, harmonics stay below Nyquist, and first/last samples
 * are always exactly zero. Zero heap allocation, O(1) stack footprint.
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
    } else if (kind == JR_CUE_SCRUB) {
        if (gate->scrub_seen &&
            (uint32_t)(ctx->now_ms - gate->last_scrub_ms) <
                JR_CUE_SCRUB_MIN_GAP_MS) {
            return false;
        }
        gate->scrub_seen = true;
        gate->last_scrub_ms = ctx->now_ms;
    }
    return true;
}

uint16_t jr_cue_amplitude_permille(jr_cue_kind_t kind, int owner_volume)
{
    static const uint16_t base[JR_CUE_KIND_COUNT] = {
        [JR_CUE_TICK]     = 24,
        [JR_CUE_CONFIRM]  = 42,
        [JR_CUE_DISMISS]  = 36,
        [JR_CUE_LIMIT]    = 42,
        [JR_CUE_SCRUB]    = 16,
        [JR_CUE_MODE]     = 46,
        [JR_CUE_MUTE_ON]  = 38,
        [JR_CUE_MUTE_OFF] = 40,
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
    case JR_CUE_TICK:     return ms_to_samples(22);
    case JR_CUE_CONFIRM:  return ms_to_samples(90);
    case JR_CUE_DISMISS:  return ms_to_samples(70);
    case JR_CUE_LIMIT:    return ms_to_samples(55);
    case JR_CUE_SCRUB:    return ms_to_samples(14);
    case JR_CUE_MODE:     return ms_to_samples(120);
    case JR_CUE_MUTE_ON:  return ms_to_samples(65);
    case JR_CUE_MUTE_OFF: return ms_to_samples(75);
    default:              return 0;
    }
}

/* One synthesis voice: phase-integrated pitch glide f0 -> f1 with up to 3
 * harmonic partials, raised-cosine attack/release, and optional exponential
 * decay. Evaluated per-sample across a small stack array (<= 3 voices) so
 * overlapping chimes and layered clicks need zero staging buffer. */
typedef struct {
    size_t start;
    size_t len;
    float f0;
    float f1;
    float h1;
    float h2;
    float h3;
    size_t attack;
    size_t release;
    float tau_ms;
    float gain;
    float phase;
} cue_voice_t;

static void render_voices(int16_t *out, size_t total,
                          cue_voice_t *voices, size_t n_voices, float amp)
{
    for (size_t v = 0; v < n_voices; ++v) {
        cue_voice_t *vc = &voices[v];
        if (vc->attack + vc->release > vc->len) {
            vc->attack = vc->release = vc->len / 2u;
        }
        if (vc->attack == 0u) vc->attack = 1u;
        if (vc->release == 0u) vc->release = 1u;
        vc->phase = 0.0f;
    }

    for (size_t i = 0; i < total; ++i) {
        float mix = 0.0f;
        for (size_t v = 0; v < n_voices; ++v) {
            cue_voice_t *vc = &voices[v];
            if (i < vc->start || i >= vc->start + vc->len) {
                continue;
            }
            const size_t k = i - vc->start;
            const size_t n = vc->len;
            const float f = vc->f0 + (vc->f1 - vc->f0) * ((float)k / (float)n);

            float env = 1.0f;
            if (k < vc->attack) {
                env = 0.5f * (1.0f - cosf((float)M_PI * (float)k / (float)vc->attack));
            } else if (n - 1u - k < vc->release) {
                env = 0.5f * (1.0f - cosf((float)M_PI * (float)(n - 1u - k) / (float)vc->release));
            }
            if (vc->tau_ms > 0.0f) {
                const float t_ms = (float)k * 1000.0f / RATE;
                env *= expf(-t_ms / vc->tau_ms);
            }

            const float w1 = vc->h1;
            const float w2 = (2.0f * f < RATE * 0.48f) ? vc->h2 : 0.0f;
            const float w3 = (3.0f * f < RATE * 0.48f) ? vc->h3 : 0.0f;
            const float wsum = fabsf(w1) + fabsf(w2) + fabsf(w3);
            const float inv = (wsum > 0.0f) ? (1.0f / wsum) : 1.0f;
            const float p = vc->phase;
            float wave = w1 * sinf(p);
            if (w2 != 0.0f) wave += w2 * sinf(2.0f * p);
            if (w3 != 0.0f) wave += w3 * sinf(3.0f * p);
            mix += vc->gain * env * wave * inv;

            vc->phase += 2.0f * (float)M_PI * f / RATE;
        }
        if (mix > 1.0f) mix = 1.0f;
        if (mix < -1.0f) mix = -1.0f;
        out[i] = (int16_t)(amp * mix);
    }
    out[0] = 0;
    out[total - 1u] = 0;
}

size_t jr_cue_render(jr_cue_kind_t kind, uint16_t amp_permille,
                     int16_t *out, size_t cap)
{
    const size_t total = jr_cue_samples(kind);
    if (total == 0u || out == NULL || cap < total) return 0u;
    if (amp_permille > JR_CUE_MAX_PERMILLE) amp_permille = JR_CUE_MAX_PERMILLE;
    const float amp = 32767.0f * (float)amp_permille / 1000.0f;

    switch (kind) {
    case JR_CUE_TICK: {
        /* Crisp mechanical rotary/bezel detent click: 1.8 kHz + 3.6 kHz
         * transient impulse blended with a warm 620 Hz body tap and fast
         * 4.5 ms decay. */
        cue_voice_t v[2] = {
            {
                .start = 0u, .len = total,
                .f0 = 1800.0f, .f1 = 1720.0f,
                .h1 = 0.65f, .h2 = 0.35f, .h3 = 0.0f,
                .attack = 18u, .release = ms_to_samples(4),
                .tau_ms = 4.2f, .gain = 0.62f,
            },
            {
                .start = 0u, .len = total,
                .f0 = 620.0f, .f1 = 540.0f,
                .h1 = 0.85f, .h2 = 0.15f, .h3 = 0.0f,
                .attack = 24u, .release = ms_to_samples(5),
                .tau_ms = 5.0f, .gain = 0.38f,
            },
        };
        render_voices(out, total, v, 2u, amp);
        break;
    }
    case JR_CUE_CONFIRM: {
        /* Crystalline dual-harmonic glass chime: G5 (784 Hz) -> D6 (1175 Hz)
         * rising fifth enriched with 2nd + 3rd bell partials (0.18 + 0.06)
         * and a silky overlapping tail into the upper note. */
        const size_t half = total / 2u;
        cue_voice_t v[2] = {
            {
                .start = 0u, .len = ms_to_samples(52),
                .f0 = 784.0f, .f1 = 784.0f,
                .h1 = 0.76f, .h2 = 0.18f, .h3 = 0.06f,
                .attack = ms_to_samples(5), .release = ms_to_samples(12),
                .tau_ms = 34.0f, .gain = 0.85f,
            },
            {
                .start = half, .len = total - half,
                .f0 = 1175.0f, .f1 = 1175.0f,
                .h1 = 0.76f, .h2 = 0.18f, .h3 = 0.06f,
                .attack = ms_to_samples(4), .release = ms_to_samples(14),
                .tau_ms = 38.0f, .gain = 0.96f,
            },
        };
        render_voices(out, total, v, 2u, amp);
        break;
    }
    case JR_CUE_DISMISS: {
        /* Soft descending optical glass drop: smooth A5 -> D5 glide with
         * gentle 2nd harmonic warmth (0.14) and velvet 45 ms decay. */
        cue_voice_t v[1] = {
            {
                .start = 0u, .len = total,
                .f0 = 880.0f, .f1 = 587.0f,
                .h1 = 0.86f, .h2 = 0.14f, .h3 = 0.0f,
                .attack = ms_to_samples(6), .release = ms_to_samples(14),
                .tau_ms = 45.0f, .gain = 0.96f,
            },
        };
        render_voices(out, total, v, 1u, amp);
        break;
    }
    case JR_CUE_LIMIT: {
        /* Tactile damped double-thump: soft low C4 (262 -> 210 Hz) + 2nd
         * harmonic (0.38) with a gentle physical micro-bounce rebound. */
        const size_t bounce_start = ms_to_samples(25);
        cue_voice_t v[2] = {
            {
                .start = 0u, .len = ms_to_samples(28),
                .f0 = 262.0f, .f1 = 210.0f,
                .h1 = 0.62f, .h2 = 0.38f, .h3 = 0.0f,
                .attack = ms_to_samples(3), .release = ms_to_samples(7),
                .tau_ms = 14.0f, .gain = 0.96f,
            },
            {
                .start = bounce_start, .len = total - bounce_start,
                .f0 = 232.0f, .f1 = 196.0f,
                .h1 = 0.62f, .h2 = 0.38f, .h3 = 0.0f,
                .attack = ms_to_samples(3), .release = ms_to_samples(10),
                .tau_ms = 12.0f, .gain = 0.56f,
            },
        };
        render_voices(out, total, v, 2u, amp);
        break;
    }
    case JR_CUE_SCRUB: {
        /* Feather-light precision micro-detent tick: 2.4 kHz + 4.8 kHz
         * shimmer with fast 2.8 ms decay for circular rotary bezel scrubbing. */
        cue_voice_t v[1] = {
            {
                .start = 0u, .len = total,
                .f0 = 2400.0f, .f1 = 2320.0f,
                .h1 = 0.72f, .h2 = 0.28f, .h3 = 0.0f,
                .attack = 16u, .release = ms_to_samples(3),
                .tau_ms = 2.8f, .gain = 0.95f,
            },
        };
        render_voices(out, total, v, 1u, amp);
        break;
    }
    case JR_CUE_MODE: {
        /* Luxurious 3-note futuristic harmonic triad bloom: C5 -> G5 -> D6
         * overlapping fifths with subtle 2nd + 3rd harmonic shimmer. */
        const size_t n2_start = ms_to_samples(32);
        const size_t n3_start = ms_to_samples(64);
        cue_voice_t v[3] = {
            {
                .start = 0u, .len = ms_to_samples(55),
                .f0 = 523.25f, .f1 = 523.25f,
                .h1 = 0.80f, .h2 = 0.16f, .h3 = 0.04f,
                .attack = ms_to_samples(5), .release = ms_to_samples(15),
                .tau_ms = 34.0f, .gain = 0.60f,
            },
            {
                .start = n2_start, .len = ms_to_samples(58),
                .f0 = 784.0f, .f1 = 784.0f,
                .h1 = 0.78f, .h2 = 0.17f, .h3 = 0.05f,
                .attack = ms_to_samples(5), .release = ms_to_samples(16),
                .tau_ms = 36.0f, .gain = 0.65f,
            },
            {
                .start = n3_start, .len = total - n3_start,
                .f0 = 1174.66f, .f1 = 1174.66f,
                .h1 = 0.76f, .h2 = 0.18f, .h3 = 0.06f,
                .attack = ms_to_samples(5), .release = ms_to_samples(18),
                .tau_ms = 42.0f, .gain = 0.76f,
            },
        };
        render_voices(out, total, v, 3u, amp);
        break;
    }
    case JR_CUE_MUTE_ON: {
        /* Distinct warm descending mechanical "thock down" (520 -> 290 Hz
         * + warm body harmonics and subtle latch onset). */
        cue_voice_t v[2] = {
            {
                .start = 0u, .len = total,
                .f0 = 520.0f, .f1 = 290.0f,
                .h1 = 0.70f, .h2 = 0.24f, .h3 = 0.06f,
                .attack = ms_to_samples(3), .release = ms_to_samples(15),
                .tau_ms = 32.0f, .gain = 0.84f,
            },
            {
                .start = 0u, .len = ms_to_samples(12),
                .f0 = 1120.0f, .f1 = 760.0f,
                .h1 = 0.75f, .h2 = 0.25f, .h3 = 0.0f,
                .attack = ms_to_samples(1), .release = ms_to_samples(4),
                .tau_ms = 3.5f, .gain = 0.16f,
            },
        };
        render_voices(out, total, v, 2u, amp);
        break;
    }
    case JR_CUE_MUTE_OFF: {
        /* Distinct bright airy ascending "bloom up" (440 -> 880 Hz +
         * crystalline 2nd harmonic and upper air shimmer). */
        const size_t air_start = ms_to_samples(25);
        cue_voice_t v[2] = {
            {
                .start = 0u, .len = total,
                .f0 = 440.0f, .f1 = 880.0f,
                .h1 = 0.76f, .h2 = 0.20f, .h3 = 0.04f,
                .attack = ms_to_samples(5), .release = ms_to_samples(18),
                .tau_ms = 52.0f, .gain = 0.78f,
            },
            {
                .start = air_start, .len = total - air_start,
                .f0 = 880.0f, .f1 = 1319.0f,
                .h1 = 0.76f, .h2 = 0.24f, .h3 = 0.0f,
                .attack = ms_to_samples(8), .release = ms_to_samples(20),
                .tau_ms = 40.0f, .gain = 0.28f,
            },
        };
        render_voices(out, total, v, 2u, amp);
        break;
    }
    default:
        return 0u;
    }
    return total;
}
