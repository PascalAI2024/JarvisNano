/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass3_ring.c — JarvisNano Glass 3.5 Procedural OLED Ring & Voice-Wave Engine.
 *
 * Pure C11, integer-only, zero-libm, zero-heap, stack < 512 B, 100% strip-invariant.
 *   1. CHRONO-ARC  (jr_g3_chrono_arc_rows)  — Home perimeter bezel + 4 sculpted
 *      telemetry capsules + whisper-soft concentric sonar ripples.
 *   2. AETHERIUM   (jr_g3_aetherium_rows)   — 120 radial FFT spectrum filaments,
 *      3 iridescent additive harmonic wave ribbons, millimeter bezel + 4 cardinal
 *      jewels, and lower-center Crystalline Vocal Waveform Ribbon.
 *   3. BEZEL SCRUB (jr_g3_bezel_scrub_rows) — 270-deg luminous rotary dial arc,
 *      41-tick precision inner scale, and faceted glowing jewel indicator.
 *   4. BOOT SPLASH (jr_g3_boot_rows)        — Silky procedural cold-boot ignition
 *      ring, comet orb, 60 progressive ticks, and optical pulse rings.
 *   5. DESK MODE   (jr_g3_desk_mode_rows)   — Unified ON AIR studio beacon,
 *      Deep-Work FOCUS halo + sprint pips, and liquid Aetherium BREATHE orb.
 *
 * Performance Architecture (Glass 3.5 — 10x+ Speedup):
 *   - Portable __builtin_clz O(1) fast isqrt (97-entry Q14 LUT + linear interp)
 *     and 2-term Taylor radial approximations (zero per-pixel isqrt in ring loops).
 *   - Branch-free, division-free octant atan2_512 & atan2_q16 (433-entry Q21
 *     reciprocal LUT + 33-entry atan LUT; zero integer division in inner loops).
 *   - Incremental half-plane ray stepping in Chrono-Arc capsules, Boot ring, and
 *     ON AIR / FOCUS arcs (ZERO per-pixel atan2 across ALL of Chrono-Arc, Boot,
 *     and ON AIR!).
 *   - 512-entry per-frame angular wave & color LUT in static .bss computed ONCE
 *     per now_ms in g3_aeth_prepare(), paired with branchless combined aura+core
 *     .rodata LUTs (eliminates ~402,000 sin calls & nested branch mispredicts).
 *   - Exact strip-invariant incremental dot/cross radial segment renderer for
 *     180 Chrono-Arc bezel ticks, 120 Aetherium FFT filaments, 116 outer ticks,
 *     and 60 Boot Ignition ticks.
 *   - Pre-composited radial cross-section LUTs for capsules & ON AIR beacon.
 */
#include "glass3_ring.h"

#include <string.h>

#include "jr_display/glass.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#ifndef CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#include "esp_heap_caps.h"
#endif
#endif

/* Keep per-frame caches (~13.7 KB) in PSRAM on ESP32-S3 so internal SRAM
 * retains full headroom, whether or not CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
 * is enabled. Host builds keep plain static .bss with zero heap usage. */
#if defined(ESP_PLATFORM) && !defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
#define G3_DEFINE_CACHE(type, name)                                                   \
    static type *name##_ptr;                                                          \
    static inline type *name##_get(void)                                              \
    {                                                                                 \
        if (name##_ptr == NULL) {                                                     \
            name##_ptr = (type *)heap_caps_calloc(1u, sizeof(type),                   \
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); \
        }                                                                             \
        return name##_ptr;                                                            \
    }
#elif defined(ESP_PLATFORM)
#define G3_DEFINE_CACHE(type, name)           \
    static EXT_RAM_BSS_ATTR type name;        \
    static inline type *name##_get(void)      \
    {                                         \
        return &name;                         \
    }
#else
#define G3_DEFINE_CACHE(type, name)           \
    static type name;                         \
    static inline type *name##_get(void)      \
    {                                         \
        return &name;                         \
    }
#endif

#define G3_W           HUD_W
#define G3_H           HUD_H
#define G3_CX16        (233 * 16) /* pixel x center is 16*x + 8 -> 232.5 px = 3728 */
#define G3_CY16        (233 * 16)
#define G3_CX2         465        /* 2 * 232.5 */
#define G3_CY2         465

#define G3_RIBBON_W    (JR_G3_AETH_RIBBON_X1 - JR_G3_AETH_RIBBON_X0 + 1) /* 275 px */
#define G3_RIBBON_CY16 (310 * 16 + 8) /* vertical center of [286, 334] */

#if defined(__GNUC__) || defined(__clang__)
#define G3_NOINLINE __attribute__((noinline))
#else
#define G3_NOINLINE
#endif

/* ---- Compact .rodata LUTs (~1.9 KB total in Flash, zero RAM) ------------- */

/* sqrt((i + 32) / 32) * 16384 for i = 0..96 (covers m in [1.0, 4.0]). */
static const uint16_t k_g3_sqrt_q14[97] = {
    16384, 16638, 16888, 17135, 17378, 17618, 17854, 18087, 18318, 18545, 18770, 18992,
    19212, 19429, 19644, 19856, 20066, 20274, 20480, 20684, 20886, 21085, 21283, 21480,
    21674, 21867, 22058, 22247, 22435, 22621, 22806, 22989, 23170, 23351, 23530, 23707,
    23884, 24059, 24232, 24405, 24576, 24746, 24915, 25083, 25249, 25415, 25580, 25743,
    25905, 26067, 26227, 26387, 26545, 26703, 26859, 27015, 27170, 27324, 27477, 27629,
    27780, 27931, 28081, 28230, 28378, 28525, 28672, 28818, 28963, 29108, 29251, 29394,
    29537, 29678, 29819, 29960, 30099, 30238, 30377, 30515, 30652, 30788, 30924, 31059,
    31194, 31328, 31462, 31595, 31727, 31859, 31991, 32122, 32252, 32382, 32511, 32640,
    32768,
};

/* atan(i / 32) in Q16 turns (0..8192 = 0..45 deg). */
static const uint16_t k_g3_atan_q16_33[33] = {
        0,   326,   651,   975,  1297,  1617,  1933,  2246,  2555,  2860,  3159,
     3453,  3742,  4025,  4302,  4572,  4836,  5094,  5344,  5589,  5826,  6058,
     6282,  6500,  6712,  6917,  7117,  7310,  7498,  7679,  7856,  8026,  8192,
};

/* atan(i / 32) in 512-step turns (0..64 = 0..45 deg). */
static const uint8_t k_g3_atan_512_33[33] = {
      0,   3,   5,   8,  10,  13,  15,  18,  20,  22,  25,
     27,  29,  31,  34,  36,  38,  40,  42,  44,  46,  47,
     49,  51,  52,  54,  56,  57,  59,  60,  61,  63,  64,
};

/* Floor((2^21 - 1) / m) for m in [33, 465]. Guarantees mn * recip < 2^21. */
static const uint16_t k_g3_recip_q21[433] = {
    63550, 61680, 59918, 58254, 56679, 55188, 53773, 52428, 51150, 49932, 48770, 47662,
    46603, 45590, 44620, 43690, 42799, 41943, 41120, 40329, 39568, 38836, 38130, 37449,
    36792, 36157, 35544, 34952, 34379, 33825, 33288, 32767, 32263, 31775, 31300, 30840,
    30393, 29959, 29537, 29127, 28728, 28339, 27962, 27594, 27235, 26886, 26546, 26214,
    25890, 25575, 25266, 24966, 24672, 24385, 24105, 23831, 23563, 23301, 23045, 22795,
    22550, 22310, 22075, 21845, 21620, 21399, 21183, 20971, 20763, 20560, 20360, 20164,
    19972, 19784, 19599, 19418, 19239, 19065, 18893, 18724, 18558, 18396, 18236, 18078,
    17924, 17772, 17623, 17476, 17331, 17189, 17050, 16912, 16777, 16644, 16513, 16383,
    16256, 16131, 16008, 15887, 15768, 15650, 15534, 15420, 15307, 15196, 15087, 14979,
    14873, 14768, 14665, 14563, 14463, 14364, 14266, 14169, 14074, 13981, 13888, 13797,
    13706, 13617, 13530, 13443, 13357, 13273, 13189, 13107, 13025, 12945, 12865, 12787,
    12710, 12633, 12557, 12483, 12409, 12336, 12264, 12192, 12122, 12052, 11983, 11915,
    11848, 11781, 11715, 11650, 11586, 11522, 11459, 11397, 11335, 11275, 11214, 11155,
    11096, 11037, 10979, 10922, 10866, 10810, 10754, 10699, 10645, 10591, 10538, 10485,
    10433, 10381, 10330, 10280, 10230, 10180, 10131, 10082, 10034,  9986,  9939,  9892,
     9845,  9799,  9754,  9709,  9664,  9619,  9576,  9532,  9489,  9446,  9404,  9362,
     9320,  9279,  9238,  9198,  9157,  9118,  9078,  9039,  9000,  8962,  8924,  8886,
     8848,  8811,  8774,  8738,  8701,  8665,  8630,  8594,  8559,  8525,  8490,  8456,
     8422,  8388,  8355,  8322,  8289,  8256,  8224,  8191,  8160,  8128,  8097,  8065,
     8035,  8004,  7973,  7943,  7913,  7884,  7854,  7825,  7796,  7767,  7738,  7710,
     7681,  7653,  7626,  7598,  7570,  7543,  7516,  7489,  7463,  7436,  7410,  7384,
     7358,  7332,  7307,  7281,  7256,  7231,  7206,  7182,  7157,  7133,  7108,  7084,
     7061,  7037,  7013,  6990,  6967,  6944,  6921,  6898,  6875,  6853,  6831,  6808,
     6786,  6765,  6743,  6721,  6700,  6678,  6657,  6636,  6615,  6594,  6574,  6553,
     6533,  6512,  6492,  6472,  6452,  6432,  6413,  6393,  6374,  6355,  6335,  6316,
     6297,  6278,  6260,  6241,  6223,  6204,  6186,  6168,  6150,  6132,  6114,  6096,
     6078,  6061,  6043,  6026,  6009,  5991,  5974,  5957,  5940,  5924,  5907,  5890,
     5874,  5857,  5841,  5825,  5809,  5793,  5777,  5761,  5745,  5729,  5714,  5698,
     5683,  5667,  5652,  5637,  5622,  5607,  5592,  5577,  5562,  5548,  5533,  5518,
     5504,  5489,  5475,  5461,  5447,  5433,  5418,  5405,  5391,  5377,  5363,  5349,
     5336,  5322,  5309,  5295,  5282,  5269,  5256,  5242,  5229,  5216,  5203,  5190,
     5178,  5165,  5152,  5140,  5127,  5115,  5102,  5090,  5077,  5065,  5053,  5041,
     5029,  5017,  5005,  4993,  4981,  4969,  4957,  4946,  4934,  4922,  4911,  4899,
     4888,  4877,  4865,  4854,  4843,  4832,  4821,  4809,  4798,  4788,  4777,  4766,
     4755,  4744,  4733,  4723,  4712,  4702,  4691,  4681,  4670,  4660,  4650,  4639,
     4629,  4619,  4609,  4599,  4588,  4578,  4568,  4559,  4549,  4539,  4529,  4519,
     4510,
};

/* Combined aura+core wave weights (zero nested dr<20 branches in inner loops!). */
static const uint8_t k_g3_wave_col0[92] = {
    195, 188, 181, 175, 168, 162, 156, 149, 211, 199, 187, 175, 163, 151, 139, 127,
    116, 104,  92,  80,  69,  67,  66,  63,  61,  59,  57,  56,  54,  53,  51,  49,
     48,  46,  44,  43,  41,  40,  38,  37,  36,  34,  33,  31,  30,  29,  28,  26,
     25,  24,  23,  22,  21,  20,  19,  17,  17,  16,  15,  14,  13,  12,  11,  11,
     10,   9,   8,   8,   7,   6,   6,   5,   4,   4,   4,   3,   3,   2,   2,   2,
      1,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};
static const uint8_t k_g3_wave_wht0[9] = {
    114, 108, 102,  96,  91,  85,  79,  73,   0,
};
static const uint8_t k_g3_wave_col1[92] = {
    159, 153, 147, 142, 137, 132, 127, 121, 170, 161, 151, 141, 132, 122, 113, 103,
     94,  84,  75,  66,  57,  55,  54,  52,  51,  49,  47,  46,  45,  43,  42,  41,
     39,  38,  37,  35,  34,  33,  31,  30,  30,  28,  27,  26,  25,  24,  23,  22,
     21,  20,  19,  18,  17,  16,  15,  14,  14,  13,  12,  11,  11,  10,   9,   9,
      8,   7,   7,   6,   6,   5,   5,   4,   4,   3,   3,   2,   2,   2,   1,   1,
      1,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};
static const uint8_t k_g3_wave_wht1[9] = {
     90,  86,  81,  76,  72,  67,  63,  58,   0,
};

/* Combined aura+core weights for BREATHE 3-ring liquid halo. */
static const uint8_t k_g3_br_col0[92] = {
    163, 159, 155, 152, 148, 145, 142, 255, 255, 244, 229, 213, 197, 181, 167, 151,
    135, 119, 105, 102,  99,  96,  94,  91,  88,  85,  83,  81,  78,  76,  73,  71,
     68,  66,  64,  61,  59,  58,  55,  53,  52,  49,  47,  45,  44,  42,  40,  38,
     36,  35,  33,  31,  30,  29,  27,  25,  24,  23,  21,  20,  19,  18,  16,  16,
     14,  13,  12,  11,  10,   9,   9,   8,   7,   6,   5,   5,   4,   3,   3,   3,
      2,   1,   1,   1,   0,   0,   0,   0,   0,   0,   0,   0,
};
static const uint8_t k_g3_br_wht0[8] = {
    225, 212, 200, 187, 175, 162, 150,   0,
};
static const uint8_t k_g3_br_col1[60] = {
    255, 251, 231, 214, 195, 179, 160, 144, 124, 108,  90,  73,  70,  68,  64,  63,
     59,  55,  54,  50,  48,  45,  44,  41,  39,  36,  35,  33,  31,  29,  28,  25,
     23,  22,  20,  19,  17,  16,  14,  13,  12,  11,   9,   9,   7,   7,   6,   5,
      4,   3,   3,   2,   2,   1,   0,   0,   0,   0,   0,   0,
};
static const uint8_t k_g3_br_col2[60] = {
    224, 209, 191, 176, 160, 144, 128, 113,  96,  81,  65,  63,  60,  59,  55,  54,
     51,  47,  46,  43,  42,  39,  38,  35,  34,  31,  30,  28,  27,  25,  24,  22,
     20,  19,  17,  16,  14,  14,  12,  11,  10,   9,   8,   7,   6,   6,   5,   4,
      3,   2,   2,   1,   1,   1,   0,   0,   0,   0,   0,   0,
};

/* Filament tangential coverage curve (tan16 in 0..23). */
static const uint8_t k_g3_fil_cov[24] = {
    255, 255, 255, 255, 255, 223, 191, 159, 127,  95,  63,  31,
     70,  64,  58,  52,  46,  40,  35,  29,  23,  17,  11,   5,
};

/* Exact Q15 quarter-circle cos for 180-tick (2-deg) & 60-tick (6-deg) rays. */
static const int16_t k_g3_cos180_q15[46] = {
    32767, 32747, 32687, 32587, 32448, 32269, 32051, 31794, 31498, 31163,
    30791, 30381, 29934, 29451, 28932, 28377, 27788, 27165, 26509, 25821,
    25101, 24351, 23571, 22762, 21925, 21062, 20173, 19260, 18323, 17364,
    16384, 15383, 14364, 13328, 12275, 11207, 10126,  9032,  7927,  6813,
     5690,  4560,  3425,  2286,  1144,     0,
};

/* Exact Q15 quarter-circle cos for 120-tick / 120-filament (3-deg) rays. */
static const int16_t k_g3_cos120_q15[31] = {
    32767, 32722, 32587, 32364, 32051, 31650, 31163, 30591, 29934, 29196,
    28377, 27481, 26509, 25465, 24351, 23170, 21925, 20621, 19260, 17846,
    16384, 14876, 13328, 11743, 10126,  8481,  6813,  5126,  3425,  1715,
        0,
};

/* Octant bases (0 = 12 o'clock clockwise) & sign masks (0 = +a, -1 = -a). */
static const int32_t k_g3_oct_base12_q16[8] = {
    16384, 49152, 16384, 49152, 32768, 32768, 0, 65536
};
static const int32_t k_g3_oct_base12_512[8] = {
    128, 384, 128, 384, 256, 256, 0, 512
};
static const int32_t k_g3_oct_mask[8] = {
    0, -1, -1, 0, -1, 0, 0, -1
};

/* ---- fast integer primitives --------------------------------------------- */

static inline int g3_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int32_t g3_abs32(int32_t v)
{
    return v < 0 ? -v : v;
}

static inline uint32_t g3_clz32(uint32_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_clz(v);
#else
    uint32_t n = 0u;
    if ((v & 0xFFFF0000u) == 0u) { n += 16u; v <<= 16; }
    if ((v & 0xFF000000u) == 0u) { n += 8u;  v <<= 8;  }
    if ((v & 0xF0000000u) == 0u) { n += 4u;  v <<= 4;  }
    if ((v & 0xC0000000u) == 0u) { n += 2u;  v <<= 2;  }
    if ((v & 0x80000000u) == 0u) { n += 1u; }
    return n;
#endif
}

/* O(1) division-free, loop-free fast integer sqrt (max err <= 1 LSB for n < 2^28). */
static inline uint32_t g3_isqrt_fast(uint32_t n)
{
    if (n == 0u) {
        return 0u;
    }
    const uint32_t k = (31u - g3_clz32(n)) & ~1u;
    const uint32_t u = (k >= 14u) ? (n >> (k - 14u)) : (n << (14u - k));
    const uint32_t idx = (u >> 9) - 32u;
    const uint32_t frac = u & 511u;
    const uint32_t s = (uint32_t)k_g3_sqrt_q14[idx] +
        (((uint32_t)(k_g3_sqrt_q14[idx + 1u] - k_g3_sqrt_q14[idx]) * frac) >> 9);
    const uint32_t sh = k >> 1;
    return (sh <= 14u) ? (s >> (14u - sh)) : (s << (sh - 14u));
}

/* Branchless Q4 radius r16 = sqrt(r2_sq << 6) for r in [16, 255] (r2_sq >= 1024). */
static inline int32_t g3_r16_from_r2sq(uint32_t r2_sq)
{
    const uint32_t v = r2_sq << 6;
    const uint32_t k = (31u - g3_clz32(v)) & ~1u;
    const uint32_t u = v >> (k - 14u);
    const uint32_t idx = (u >> 9) - 32u;
    const uint32_t frac = u & 511u;
    const uint32_t s = (uint32_t)k_g3_sqrt_q14[idx] +
        (((uint32_t)(k_g3_sqrt_q14[idx + 1u] - k_g3_sqrt_q14[idx]) * frac) >> 9);
    return (int32_t)(s >> (14u - (k >> 1)));
}

static inline unsigned g3_quad_alpha(int t255, unsigned peak_a, unsigned scale_a)
{
    const uint32_t t = (uint32_t)g3_clamp(t255, 0, 255);
    const uint32_t q = (t * t) >> 8;
    return (unsigned)((q * peak_a * scale_a) >> 16);
}

static inline uint16_t g3_to_nat(uint16_t p, bool swap_bytes)
{
    return swap_bytes ? glass_bswap(p) : p;
}

static inline uint16_t g3_add565_nat(uint16_t under, uint16_t over, unsigned a)
{
    if (a == 0u) {
        return under;
    }
    if (a > 255u) {
        a = 255u;
    }
    uint32_t r = ((under >> 11) & 31u) + ((((over >> 11) & 31u) * a + 127u) >> 8);
    uint32_t g = ((under >> 5)  & 63u) + ((((over >> 5)  & 63u) * a + 127u) >> 8);
    uint32_t b = ( under        & 31u) + ((( over        & 31u) * a + 127u) >> 8);
    if (r > 31u) { r = 31u; }
    if (g > 63u) { g = 63u; }
    if (b > 31u) { b = 31u; }
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t g3_add565(uint16_t under, uint16_t over, unsigned a,
                                 bool swap_bytes)
{
    if (a == 0u) {
        return under;
    }
    const uint16_t out = g3_add565_nat(g3_to_nat(under, swap_bytes), over, a);
    return g3_to_nat(out, swap_bytes);
}

static inline void g3_acc_add_q8(uint32_t *ar, uint32_t *ag, uint32_t *ab,
                                 uint16_t over, unsigned a)
{
    *ar += (uint32_t)(over >> 11) * a;
    *ag += (uint32_t)((over >> 5) & 63u) * a;
    *ab += (uint32_t)(over & 31u) * a;
}

/* Flush Q8 accumulator scaled by str (0..255) ONCE onto panel-order pixel. */
static inline uint16_t g3_acc_flush_str(uint16_t under, uint32_t ar, uint32_t ag,
                                        uint32_t ab, unsigned str, bool swap_bytes)
{
    const uint32_t u = swap_bytes ? glass_bswap(under) : under;
    uint32_t r, g, b;
    if (str >= 255u) {
        r = (u >> 11) + ((ar + 127u) >> 8);
        g = ((u >> 5) & 63u) + ((ag + 127u) >> 8);
        b = (u & 31u) + ((ab + 127u) >> 8);
    } else {
        r = (u >> 11) + (((ar * str) + 32767u) >> 16);
        g = ((u >> 5) & 63u) + (((ag * str) + 32767u) >> 16);
        b = (u & 31u) + (((ab * str) + 32767u) >> 16);
    }
    if (r > 31u) { r = 31u; }
    if (g > 63u) { g = 63u; }
    if (b > 31u) { b = 31u; }
    const uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return swap_bytes ? glass_bswap(out) : out;
}

static inline uint16_t g3_blend565_nat(uint16_t under, uint16_t over, unsigned a)
{
    if (a == 0u) {
        return under;
    }
    if (a >= 255u) {
        return over;
    }
    const uint32_t wo = (a + 2u) >> 2;
    const uint32_t wu = 64u - wo;
    const uint32_t rb = (((uint32_t)(under & 0xF81Fu) * wu + (uint32_t)(over & 0xF81Fu) * wo + 0x10020u) >> 6) & 0xF81Fu;
    const uint32_t g  = (((uint32_t)(under & 0x07E0u) * wu + (uint32_t)(over & 0x07E0u) * wo + (32u << 5)) >> 6) & 0x07E0u;
    return (uint16_t)(rb | g);
}

static inline uint16_t g3_lerp565(uint16_t c0, uint16_t c1, int t)
{
    if (t <= 0) {
        return c0;
    }
    if (t >= 255) {
        return c1;
    }
    const uint32_t w = ((uint32_t)t + 2u) >> 2;
    const uint32_t iw = 64u - w;
    const uint32_t rb = (((uint32_t)(c0 & 0xF81Fu) * iw + (uint32_t)(c1 & 0xF81Fu) * w) >> 6) & 0xF81Fu;
    const uint32_t g  = (((uint32_t)(c0 & 0x07E0u) * iw + (uint32_t)(c1 & 0x07E0u) * w) >> 6) & 0x07E0u;
    return (uint16_t)(rb | g);
}

/* Pre-composite (c2, a2) over (c1, a1) into exact equivalent single (c_out, a_out). */
static void g3_precomp_over(uint16_t c1, unsigned a1, uint16_t c2, unsigned a2,
                            uint16_t *c_out, uint8_t *a_out)
{
    if (a2 == 0u) {
        *c_out = c1;
        *a_out = (uint8_t)(a1 > 255u ? 255u : a1);
        return;
    }
    if (a1 == 0u || a2 >= 255u) {
        *c_out = c2;
        *a_out = (uint8_t)(a2 > 255u ? 255u : a2);
        return;
    }
    const uint32_t a1_eff = (a1 * (255u - a2) + 127u) / 255u;
    const uint32_t anet = a2 + a1_eff;
    const uint32_t r = (((c1 >> 11) & 31u) * a1_eff + ((c2 >> 11) & 31u) * a2 + (anet >> 1)) / anet;
    const uint32_t g = (((c1 >> 5)  & 63u) * a1_eff + ((c2 >> 5)  & 63u) * a2 + (anet >> 1)) / anet;
    const uint32_t b = (( c1        & 31u) * a1_eff + ( c2        & 31u) * a2 + (anet >> 1)) / anet;
    *c_out = (uint16_t)((r << 11) | (g << 5) | b);
    *a_out = (uint8_t)(anet > 255u ? 255u : anet);
}

/* Branch-free, division-free octant atan2 -> Q16 turns CW from 12 o'clock. */
static inline uint32_t g3_atan2_12_q16(int32_t dy2, int32_t dx2)
{
    const uint32_t ax = (uint32_t)g3_abs32(dx2);
    const uint32_t ay = (uint32_t)g3_abs32(dy2);
    const uint32_t swap = (ay > ax) ? 4u : 0u;
    const uint32_t mn = swap ? ax : ay;
    const uint32_t mx = swap ? ay : ax;
    uint32_t u;
    if (mx >= 33u) {
        u = (mn * (uint32_t)k_g3_recip_q21[mx - 33u]) >> 7;
    } else {
        u = mx ? ((mn << 14) / mx) : 0u;
        if (u > 16383u) { u = 16383u; }
    }
    const uint32_t idx = u >> 9;
    const uint32_t frac = u & 511u;
    const uint32_t a = (uint32_t)k_g3_atan_q16_33[idx] +
        (((uint32_t)(k_g3_atan_q16_33[idx + 1u] - k_g3_atan_q16_33[idx]) * frac) >> 9);
    const uint32_t oct = (dx2 < 0 ? 1u : 0u) | (dy2 < 0 ? 2u : 0u) | swap;
    const int32_t mask = k_g3_oct_mask[oct];
    const int32_t sa = ((int32_t)a ^ mask) - mask;
    return (uint32_t)(k_g3_oct_base12_q16[oct] + sa) & 0xFFFFu;
}

/* Branch-free, division-free octant atan2 -> Q16 turns CW from 12 o'clock
 * (ring fast-path: r >= 24 guarantees mx >= 33, ZERO branch!). */
static inline uint32_t g3_atan2_12_q16_ring(int32_t dy2, int32_t dx2)
{
    const uint32_t ax = (uint32_t)g3_abs32(dx2);
    const uint32_t ay = (uint32_t)g3_abs32(dy2);
    const uint32_t swap = (ay > ax) ? 4u : 0u;
    const uint32_t mn = swap ? ax : ay;
    const uint32_t mx = swap ? ay : ax;
    const uint32_t u = (mn * (uint32_t)k_g3_recip_q21[mx - 33u]) >> 7;
    const uint32_t idx = u >> 9;
    const uint32_t frac = u & 511u;
    const uint32_t a = (uint32_t)k_g3_atan_q16_33[idx] +
        (((uint32_t)(k_g3_atan_q16_33[idx + 1u] - k_g3_atan_q16_33[idx]) * frac) >> 9);
    const uint32_t oct = (dx2 < 0 ? 1u : 0u) | (dy2 < 0 ? 2u : 0u) | swap;
    const int32_t mask = k_g3_oct_mask[oct];
    const int32_t sa = ((int32_t)a ^ mask) - mask;
    return (uint32_t)(k_g3_oct_base12_q16[oct] + sa) & 0xFFFFu;
}

/* Branch-free, division-free octant atan2 -> 512-step turns CW from 12 o'clock (~6 cycles). */
static inline uint32_t g3_atan2_12_512_ring(int32_t dy2, int32_t dx2)
{
    const uint32_t ax = (uint32_t)g3_abs32(dx2);
    const uint32_t ay = (uint32_t)g3_abs32(dy2);
    const uint32_t swap = (ay > ax) ? 4u : 0u;
    const uint32_t mn = swap ? ax : ay;
    const uint32_t mx = swap ? ay : ax;
    const uint32_t idx = (mn * (uint32_t)k_g3_recip_q21[mx - 33u] + 32768u) >> 16;
    const uint32_t a = k_g3_atan_512_33[idx];
    const uint32_t oct = (dx2 < 0 ? 1u : 0u) | (dy2 < 0 ? 2u : 0u) | swap;
    const int32_t mask = k_g3_oct_mask[oct];
    const int32_t sa = ((int32_t)a ^ mask) - mask;
    return (uint32_t)(k_g3_oct_base12_512[oct] + sa) & 511u;
}

static inline uint32_t g3_atan2_12_512(int32_t dy2, int32_t dx2)
{
    const uint32_t ax = (uint32_t)g3_abs32(dx2);
    const uint32_t ay = (uint32_t)g3_abs32(dy2);
    const uint32_t swap = (ay > ax) ? 4u : 0u;
    const uint32_t mn = swap ? ax : ay;
    const uint32_t mx = swap ? ay : ax;
    uint32_t idx;
    if (mx >= 33u) {
        idx = (mn * (uint32_t)k_g3_recip_q21[mx - 33u] + 32768u) >> 16;
    } else {
        idx = mx ? ((mn * 32u + (mx >> 1)) / mx) : 0u;
        if (idx > 32u) { idx = 32u; }
    }
    const uint32_t a = k_g3_atan_512_33[idx];
    const uint32_t oct = (dx2 < 0 ? 1u : 0u) | (dy2 < 0 ? 2u : 0u) | swap;
    const int32_t mask = k_g3_oct_mask[oct];
    const int32_t sa = ((int32_t)a ^ mask) - mask;
    return (uint32_t)(k_g3_oct_base12_512[oct] + sa) & 511u;
}

static inline uint32_t g3_atan2_q16_fast(int32_t dy2, int32_t dx2)
{
    return (g3_atan2_12_q16(dy2, dx2) + GLASS_Q16_12_OCLOCK) & 0xFFFFu;
}

/* Quarter-table lookup for unit radial vector (ux, uy) in Q15, CW from 12 o'clock. */
static inline void g3_unit_vec_q15(int k, int q, const int16_t *cos_tbl,
                                   int32_t *ux, int32_t *uy)
{
    if (k < q) {
        *ux =  (int32_t)cos_tbl[q - k];
        *uy = -(int32_t)cos_tbl[k];
    } else if (k < 2 * q) {
        const int j = k - q;
        *ux =  (int32_t)cos_tbl[j];
        *uy =  (int32_t)cos_tbl[q - j];
    } else if (k < 3 * q) {
        const int j = k - 2 * q;
        *ux = -(int32_t)cos_tbl[q - j];
        *uy =  (int32_t)cos_tbl[j];
    } else {
        const int j = k - 3 * q;
        *ux = -(int32_t)cos_tbl[j];
        *uy = -(int32_t)cos_tbl[q - j];
    }
}

/* Compute up to 2 x-spans [lo, hi] covering annulus [r_in, r_out] on row y. */
typedef struct {
    int lo, hi;
} g3_xspan_t;

static int g3_annulus_row_spans(int y, int r_in, int r_out, g3_xspan_t out[2])
{
    const int dy2 = 2 * y - G3_CY2;
    const int dy2_sq = dy2 * dy2;
    const int ro2 = 2 * r_out;
    const int ro2_sq = ro2 * ro2;
    if (dy2_sq > ro2_sq) {
        return 0;
    }
    const int ho = (int)g3_isqrt_fast((uint32_t)(ro2_sq - dy2_sq));
    int x0 = (G3_CX2 - ho) / 2 - 2;
    int x1 = (G3_CX2 + ho) / 2 + 2;
    if (x0 < 0) { x0 = 0; }
    if (x1 >= G3_W) { x1 = G3_W - 1; }
    if (x0 > x1) {
        return 0;
    }

    const int ri2 = 2 * r_in;
    const int ri2_sq = ri2 * ri2;
    if (r_in <= 0 || dy2_sq >= ri2_sq) {
        out[0].lo = x0;
        out[0].hi = x1;
        return 1;
    }
    const int hi = (int)g3_isqrt_fast((uint32_t)(ri2_sq - dy2_sq));
    int xl_hi = (G3_CX2 - hi) / 2 + 2;
    int xr_lo = (G3_CX2 + hi) / 2 - 2;
    if (xl_hi >= xr_lo - 1) {
        out[0].lo = x0;
        out[0].hi = x1;
        return 1;
    }
    int n = 0;
    if (x0 <= xl_hi) {
        out[n].lo = x0;
        out[n].hi = xl_hi < x1 ? xl_hi : x1;
        n++;
    }
    if (xr_lo <= x1) {
        out[n].lo = xr_lo > x0 ? xr_lo : x0;
        out[n].hi = x1;
        n++;
    }
    return n;
}

/* Thin circular hairline ring at exact radius r_c (zero isqrt, zero atan2). */
static G3_NOINLINE void g3_hairline_ring_rows(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                              int r_c, int32_t hw16, uint16_t color,
                                              int peak_a, int strength,
                                              int clip_r_min, int clip_r_max)
{
    const int eff_a = (peak_a * strength) >> 8;
    if (eff_a <= 0 || hw16 <= 0) {
        return;
    }
    const int b_in = g3_clamp(r_c - 2, clip_r_min, clip_r_max);
    const int b_out = g3_clamp(r_c + 2, clip_r_min, clip_r_max);
    const int rmin2_sq = (2 * clip_r_min) * (2 * clip_r_min);
    const int rmax2_sq = (2 * clip_r_max) * (2 * clip_r_max);
    const int32_t rc2_sq = 4 * r_c * r_c;
    const int32_t inv_q15 = (int32_t)((1u << 20) / (uint32_t)(r_c * 16));
    const uint32_t inv_hw = (255u << 8) / (uint32_t)hw16;

    int ry0 = 232 - b_out;
    int ry1 = 233 + b_out;
    if (ry0 < y0) { ry0 = y0; }
    if (ry0 < 0)  { ry0 = 0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry1 >= G3_H) { ry1 = G3_H - 1; }

    for (int y = ry0; y <= ry1; ++y) {
        g3_xspan_t spans[2];
        const int nsp = g3_annulus_row_spans(y, b_in, b_out, spans);
        if (nsp == 0) {
            continue;
        }
        const int dy2 = 2 * y - G3_CY2;
        const int dy2_sq = dy2 * dy2;
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;
        for (int s = 0; s < nsp; ++s) {
            for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                const int dx2 = 2 * x - G3_CX2;
                const int r2_sq = dx2 * dx2 + dy2_sq;
                if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                    continue;
                }
                const int32_t dr = g3_abs32(((r2_sq - rc2_sq) * inv_q15) >> 15);
                if (dr >= hw16) {
                    continue;
                }
                const uint32_t cov = ((uint32_t)(hw16 - dr) * inv_hw) >> 8;
                const unsigned a = (unsigned)((cov * (uint32_t)eff_a) >> 8);
                if (a > 0u) {
                    row[x] = g3_add565(row[x], color, a, swap_bytes);
                }
            }
        }
    }
}

/* Soft quadratic-falloff additive OLED glow orb clipped strictly to rows
 * [y0, y1) and annulus r in [clip_r_min, clip_r_max]. Zero 64-bit div! */
static G3_NOINLINE void g3_glow_orb_annulus(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                            int32_t cx16, int32_t cy16, int r_px,
                                            uint16_t color, uint8_t peak_a,
                                            int clip_r_min, int clip_r_max)
{
    if (r_px <= 0 || peak_a == 0u) {
        return;
    }
    const int cx_i = (int)(cx16 >> 4);
    const int cy_i = (int)(cy16 >> 4);
    int ry0 = cy_i - r_px - 1;
    int ry1 = cy_i + r_px + 1;
    if (ry0 < y0) { ry0 = y0; }
    if (ry0 < 0)  { ry0 = 0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry1 >= G3_H) { ry1 = G3_H - 1; }
    if (ry0 > ry1) {
        return;
    }
    int rx0 = cx_i - r_px - 1;
    int rx1 = cx_i + r_px + 1;
    if (rx0 < 0) { rx0 = 0; }
    if (rx1 >= G3_W) { rx1 = G3_W - 1; }

    const int32_t r16 = r_px * 16;
    const uint32_t r16_sq = (uint32_t)(r16 * r16);
    const uint32_t inv_r16_sq = (255u << 16) / r16_sq;
    const int rmin2_sq = (2 * clip_r_min) * (2 * clip_r_min);
    const int rmax2_sq = (2 * clip_r_max) * (2 * clip_r_max);

    for (int y = ry0; y <= ry1; ++y) {
        const int dy2 = 2 * y - G3_CY2;
        const int dy2_sq = dy2 * dy2;
        if (dy2_sq > rmax2_sq) {
            continue;
        }
        const int32_t py16 = y * 16 + 8 - cy16;
        const uint32_t py_sq = (uint32_t)(py16 * py16);
        if (py_sq >= r16_sq) {
            continue;
        }
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;
        for (int x = rx0; x <= rx1; ++x) {
            const int dx2 = 2 * x - G3_CX2;
            const int r2_sq = dx2 * dx2 + dy2_sq;
            if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                continue;
            }
            const int32_t px16 = x * 16 + 8 - cx16;
            const uint32_t d2 = (uint32_t)(px16 * px16) + py_sq;
            if (d2 >= r16_sq) {
                continue;
            }
            const uint32_t t = ((r16_sq - d2) * inv_r16_sq) >> 16;
            const unsigned a = (unsigned)((t * t * (uint32_t)peak_a) >> 16);
            if (a > 0u) {
                row[x] = g3_add565(row[x], color, a, swap_bytes);
            }
        }
    }
}

/* Strip-invariant anti-aliased radial tick segment (ZERO atan2, ZERO isqrt!). */
static G3_NOINLINE void g3_radial_tick_rows(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                            int32_t ux, int32_t uy,
                                            int32_t r_lo16, int32_t r_hi16,
                                            int32_t hw16, int32_t glow_hw16,
                                            uint16_t col_in, uint16_t col_out, uint16_t col_core,
                                            unsigned blade_a, unsigned glow_a,
                                            int clip_r_min, int clip_r_max)
{
    const int32_t max_hw = glow_hw16 > hw16 ? glow_hw16 : hw16;
    const int32_t r0 = r_lo16 - 10;
    const int32_t r1 = r_hi16 + 10;
    const int32_t x_r0 = G3_CX16 + ((r0 * ux) >> 15);
    const int32_t y_r0 = G3_CY16 + ((r0 * uy) >> 15);
    const int32_t x_r1 = G3_CX16 + ((r1 * ux) >> 15);
    const int32_t y_r1 = G3_CY16 + ((r1 * uy) >> 15);
    const int32_t pad_x = ((g3_abs32(uy) * max_hw) >> 15) + 20;
    const int32_t pad_y = ((g3_abs32(ux) * max_hw) >> 15) + 20;

    int ty0 = ((y_r0 < y_r1 ? y_r0 : y_r1) - pad_y) >> 4;
    int ty1 = ((y_r0 > y_r1 ? y_r0 : y_r1) + pad_y) >> 4;
    if (ty1 < y0 || ty0 >= y1) {
        return;
    }
    if (ty0 < y0)    { ty0 = y0; }
    if (ty0 < 0)     { ty0 = 0; }
    if (ty1 >= y1)   { ty1 = y1 - 1; }
    if (ty1 >= G3_H) { ty1 = G3_H - 1; }
    if (ty0 > ty1) {
        return;
    }

    int tx0 = ((x_r0 < x_r1 ? x_r0 : x_r1) - pad_x) >> 4;
    int tx1 = ((x_r0 > x_r1 ? x_r0 : x_r1) + pad_x) >> 4;
    if (tx0 < 0)     { tx0 = 0; }
    if (tx1 >= G3_W) { tx1 = G3_W - 1; }

    const int32_t auy = g3_abs32(uy);
    const bool use_slope = (auy >= 8192);
    const int32_t slope_q12 = use_slope ? ((ux * 4096) / uy) : 0;
    const int half_x = use_slope ? (int)((((max_hw + 16) * 32768) / auy) >> 4) + 2 : 0;

    const int rmin2_sq = (2 * clip_r_min) * (2 * clip_r_min);
    const int rmax2_sq = (2 * clip_r_max) * (2 * clip_r_max);
    const bool grad = (col_in != col_out);
    const uint32_t inv_rlen = (grad && r_hi16 > r_lo16) ? ((255u << 8) / (uint32_t)(r_hi16 - r_lo16)) : 0u;
    const uint32_t inv_glow = (glow_hw16 > 0) ? ((255u << 8) / (uint32_t)glow_hw16) : 0u;
    const int32_t ux16 = ux * 16;
    const int32_t uy16 = uy * 16;

    for (int y = ty0; y <= ty1; ++y) {
        int xlo = tx0;
        int xhi = tx1;
        const int32_t dy16 = y * 16 + 8 - G3_CY16;
        if (use_slope) {
            const int xm = (G3_CX16 + ((dy16 * slope_q12) >> 12)) >> 4;
            if (xm - half_x > xlo) { xlo = xm - half_x; }
            if (xm + half_x < xhi) { xhi = xm + half_x; }
            if (xlo > xhi) {
                continue;
            }
        }
        const int dy2 = 2 * y - G3_CY2;
        const int dy2_sq = dy2 * dy2;
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;

        const int32_t dx16_0 = xlo * 16 + 8 - G3_CX16;
        int32_t r_acc = dx16_0 * ux + dy16 * uy;
        int32_t t_acc = dx16_0 * uy - dy16 * ux;

        for (int x = xlo; x <= xhi; ++x, r_acc += ux16, t_acc += uy16) {
            const int32_t tan16 = g3_abs32(t_acc >> 15);
            if (tan16 >= max_hw) {
                continue;
            }
            const int32_t r16 = r_acc >> 15;
            if (r16 < r_lo16 - 8 || r16 > r_hi16 + 8) {
                continue;
            }
            const int dx2 = 2 * x - G3_CX2;
            const int r2_sq = dx2 * dx2 + dy2_sq;
            if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                continue;
            }

            uint16_t pix = g3_to_nat(row[x], swap_bytes);
            if (glow_a > 0u && tan16 < glow_hw16) {
                const int gt = (int)(((uint32_t)(glow_hw16 - tan16) * inv_glow) >> 8);
                const unsigned ga = (unsigned)((((uint32_t)gt * (uint32_t)gt) >> 8) * glow_a >> 8);
                if (ga > 0u) {
                    pix = g3_add565_nat(pix, col_out, ga);
                }
            }
            if (tan16 < hw16) {
                int cov = ((hw16 - tan16) * 255) >> 3;
                if (cov > 255) { cov = 255; }
                if (r16 < r_lo16) {
                    cov = (cov * (r16 - (r_lo16 - 8))) >> 3;
                } else if (r16 > r_hi16) {
                    cov = (cov * ((r_hi16 + 8) - r16)) >> 3;
                }
                if (cov > 0) {
                    uint16_t col;
                    if (col_core != 0u && tan16 < 8) {
                        col = col_core;
                    } else if (grad) {
                        const int rt = g3_clamp((int)(((uint32_t)(r16 - r_lo16) * inv_rlen) >> 8), 0, 255);
                        col = g3_lerp565(col_in, col_out, rt);
                    } else {
                        col = col_in;
                    }
                    pix = g3_blend565_nat(pix, col, (unsigned)(((unsigned)cov * blade_a) >> 8));
                }
            }
            row[x] = g3_to_nat(pix, swap_bytes);
        }
    }
}

/* ---- Curved Rounded-Endcap Capsule Descriptor (Half-Plane + Radial LUT) -- */

typedef struct {
    uint16_t a0;
    uint16_t sweep;
    int16_t  cap0_x16, cap0_y16;
    int16_t  cap1_x16, cap1_y16;
    int16_t  c0, s0, c1, s1;     /* Q15 unit vectors of track start/end rays */

    uint16_t fill_a0;
    uint16_t fill_sweep;
    int16_t  fcap0_x16, fcap0_y16;
    int16_t  fcap1_x16, fcap1_y16;
    int16_t  fc0, fs0, fc1, fs1; /* Q15 unit vectors of fill start/end rays */
    int16_t  head_x16, head_y16;

    /* 3 Wi-Fi bar divider rays & shimmer center ray (zero atan2!) */
    int16_t  div_c[3], div_s[3];
    int16_t  shim_c, shim_s;
    int16_t  shim_hw_q2;
    uint16_t shim_inv_q8;

    int16_t  bx0, bx1, by0, by1;

    uint16_t col_deep;
    uint16_t col_core;
    uint16_t col_ridge;
    uint16_t col_glow;
    uint8_t  fill_a;
    uint8_t  glow_a;
    uint8_t  dividers;
    uint8_t  shimmer_pos;
    uint8_t  has_fill;

    /* Pre-composited 48-entry radial cross-section for r in [208, 231.5] (0.5 px):
     *   - trk_col/trk_a: dark recessed glass track + chamfered metallic rims
     *   - fil_col/fil_a/fil_bloom: track + rim + 3D cylindrical neon fill + bloom */
    uint16_t trk_col[48];
    uint8_t  trk_a[48];
    uint16_t fil_col[48];
    uint16_t fil_swp[48];
    uint8_t  fil_a[48];
    uint8_t  fil_bloom[48];
} g3_capsule_t;

static void g3_capsule_compute_caps16(int32_t r_mid16, uint32_t a0, uint32_t sweep,
                                      int16_t *x0, int16_t *y0,
                                      int16_t *x1, int16_t *y1,
                                      int16_t *oc0, int16_t *os0,
                                      int16_t *oc1, int16_t *os1)
{
    const uint32_t a1 = (a0 + sweep) & 0xFFFFu;
    const int32_t c0 = glass_cos_q15(a0);
    const int32_t s0 = glass_sin_q15(a0);
    const int32_t c1 = glass_cos_q15(a1);
    const int32_t s1 = glass_sin_q15(a1);
    *oc0 = (int16_t)c0; *os0 = (int16_t)s0;
    *oc1 = (int16_t)c1; *os1 = (int16_t)s1;
    *x0 = (int16_t)(G3_CX16 + (int32_t)(((int64_t)r_mid16 * c0) >> 15));
    *y0 = (int16_t)(G3_CY16 + (int32_t)(((int64_t)r_mid16 * s0) >> 15));
    *x1 = (int16_t)(G3_CX16 + (int32_t)(((int64_t)r_mid16 * c1) >> 15));
    *y1 = (int16_t)(G3_CY16 + (int32_t)(((int64_t)r_mid16 * s1) >> 15));
}

static void g3_capsule_compute_caps(int32_t r_mid16, uint32_t a0, uint32_t sweep,
                                    int32_t *x0, int32_t *y0,
                                    int32_t *x1, int32_t *y1)
{
    const uint32_t a1 = (a0 + sweep) & 0xFFFFu;
    *x0 = G3_CX16 + (int32_t)(((int64_t)r_mid16 * glass_cos_q15(a0)) >> 15);
    *y0 = G3_CY16 + (int32_t)(((int64_t)r_mid16 * glass_sin_q15(a0)) >> 15);
    *x1 = G3_CX16 + (int32_t)(((int64_t)r_mid16 * glass_cos_q15(a1)) >> 15);
    *y1 = G3_CY16 + (int32_t)(((int64_t)r_mid16 * glass_sin_q15(a1)) >> 15);
}

static void g3_arc_bbox(uint32_t a0, uint32_t sweep, uint32_t ang_pad,
                        int r_in, int r_out,
                        int16_t *bx0, int16_t *bx1, int16_t *by0, int16_t *by1)
{
    const uint32_t ea0 = (a0 - ang_pad) & 0xFFFFu;
    const uint32_t esw = sweep + 2u * ang_pad;
    int xlo = G3_W - 1, xhi = 0, ylo = G3_H - 1, yhi = 0;

    uint32_t pts[6];
    int npts = 0;
    pts[npts++] = ea0;
    pts[npts++] = (ea0 + esw) & 0xFFFFu;
    static const uint32_t k_card[4] = { 0u, 16384u, 32768u, 49152u };
    for (int i = 0; i < 4; ++i) {
        if (((k_card[i] - ea0) & 0xFFFFu) <= esw) {
            pts[npts++] = k_card[i];
        }
    }
    for (int i = 0; i < npts; ++i) {
        const int32_t c = glass_cos_q15(pts[i]);
        const int32_t s = glass_sin_q15(pts[i]);
        const int radii[2] = { r_in, r_out };
        for (int ri = 0; ri < 2; ++ri) {
            const int px = 233 + (int)((radii[ri] * c) >> 15);
            const int py = 233 + (int)((radii[ri] * s) >> 15);
            if (px < xlo) { xlo = px; }
            if (px > xhi) { xhi = px; }
            if (py < ylo) { ylo = py; }
            if (py > yhi) { yhi = py; }
        }
    }
    *bx0 = (int16_t)g3_clamp(xlo - 3, 0, G3_W - 1);
    *bx1 = (int16_t)g3_clamp(xhi + 3, 0, G3_W - 1);
    *by0 = (int16_t)g3_clamp(ylo - 3, 0, G3_H - 1);
    *by1 = (int16_t)g3_clamp(yhi + 3, 0, G3_H - 1);
}

static inline int32_t g3_arc_spine_dist16(int32_t px16, int32_t py16,
                                          int32_t r16, uint32_t ang,
                                          int32_t r_mid16, uint32_t a0, uint32_t sweep,
                                          int32_t c0x, int32_t c0y,
                                          int32_t c1x, int32_t c1y)
{
    const uint32_t rel = (ang - a0) & 0xFFFFu;
    if (rel <= sweep) {
        return g3_abs32(r16 - r_mid16);
    }
    const uint32_t d_end = rel - sweep;
    const uint32_t d_start = 65536u - rel;
    const uint32_t d_min = d_end < d_start ? d_end : d_start;
    if (d_min > 2400u) {
        return 4096;
    }
    const int32_t ex = d_end < d_start ? c1x : c0x;
    const int32_t ey = d_end < d_start ? c1y : c0y;
    const int32_t dx = px16 - ex;
    const int32_t dy = py16 - ey;
    const uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
    if (d2 >= 256u * 256u) {
        return 4096;
    }
    return (int32_t)g3_isqrt_fast(d2);
}

/* ==========================================================================
 * OPTION 1: CHRONO-ARC (Home Perimeter Micrometer Bezel + 4 Sculpted Capsules
 *                       + Whisper-Soft Concentric Sonar Ripples)
 * ========================================================================== */

static inline uint32_t g3_clk_deg_q16(int deg_tenths)
{
    int32_t q = (deg_tenths * 65536) / 3600;
    return (uint32_t)(GLASS_Q16_12_OCLOCK + q) & 0xFFFFu;
}

typedef struct {
    bool               valid;
    uint32_t           now_ms;
    jr_g3_chrono_env_t env;

    int32_t  ring_r16[3];
    int32_t  ring_r2_sq[3];
    int32_t  ring_inv_q15[3];
    int32_t  ring_hw[3];
    uint16_t ring_inv_hw[3];
    uint8_t  ring_a[3];
    uint16_t rip_col;

    uint16_t col_vernier_core;
    uint16_t col_vernier_body;
    uint16_t col_hour_inner;
    uint16_t col_hour_outer;

    g3_capsule_t caps[4];
} g3_chrono_cache_t;

G3_DEFINE_CACHE(g3_chrono_cache_t, s_chrono_cache)

static G3_NOINLINE void g3_chrono_prepare(g3_chrono_cache_t *cc, uint32_t now_ms,
                                          const jr_g3_chrono_env_t *env)
{
    if (cc->valid && cc->now_ms == now_ms &&
        cc->env.batt_pct == env->batt_pct &&
        cc->env.charging == env->charging &&
        cc->env.wifi_bars == env->wifi_bars &&
        cc->env.amp == env->amp &&
        cc->env.muted == env->muted &&
        cc->env.codex_active == env->codex_active &&
        cc->env.voice_busy == env->voice_busy) {
        return;
    }

    cc->valid = true;
    cc->now_ms = now_ms;
    cc->env = *env;

    const int breath_q15 = glass_sin_q15((now_ms * 18u) & 0xFFFFu);
    const int breath_01 = (breath_q15 + 32768) >> 8;

    /* 1. Sonar ripples */
    cc->rip_col = 0x04BFu;
    if (env->muted) {
        cc->rip_col = 0xC4C2u;
    } else if (env->codex_active) {
        cc->rip_col = 0x725Fu;
    }
    for (int k = 0; k < 2; ++k) {
        const uint32_t phase = (now_ms + (uint32_t)k * 2400u) % 4800u;
        cc->ring_r16[k] = 108 * 16 + (int32_t)((phase * (58u * 16u)) / 4800u);
        const uint32_t env_ang = (phase * 32768u) / 4800u;
        const int ev = (glass_sin_q15(env_ang) * 22) >> 15;
        cc->ring_a[k] = (uint8_t)g3_clamp(ev, 0, 22);
        cc->ring_hw[k] = 28;
        cc->ring_inv_hw[k] = (uint16_t)((255u << 8) / 28u);
    }
    cc->ring_r16[2] = 138 * 16;
    cc->ring_a[2] = 11u;
    cc->ring_hw[2] = 14;
    cc->ring_inv_hw[2] = (uint16_t)((255u << 8) / 14u);
    for (int k = 0; k < 3; ++k) {
        const uint32_t rc16 = (uint32_t)cc->ring_r16[k];
        cc->ring_r2_sq[k] = (int32_t)((rc16 * rc16) >> 6);
        cc->ring_inv_q15[k] = (int32_t)((1u << 20) / rc16);
    }

    /* 2. Bezel colors */
    cc->col_vernier_core = env->muted ? 0xFFE8u : 0xFFFFu;
    cc->col_vernier_body = env->muted ? 0xFCA2u
                         : (env->codex_active ? 0xB3FFu : 0x47FFu);
    cc->col_hour_inner   = env->muted ? 0xA3A1u
                         : (env->codex_active ? 0x621Du : 0x045Bu);
    cc->col_hour_outer   = env->muted ? 0xFDE5u
                         : (env->codex_active ? 0xB39Fu : 0x3EFFu);

    /* 3. Four sculpted curved capsules */
    const int32_t cap_r_mid16 = 219 * 16;
    memset(cc->caps, 0, sizeof(cc->caps));

    /* Capsule 0: Top (12 o'clock, -25..+25 deg) */
    {
        g3_capsule_t *c = &cc->caps[0];
        c->a0 = (uint16_t)g3_clk_deg_q16(-250);
        c->sweep = (uint16_t)((500 * 65536) / 3600);
        g3_capsule_compute_caps16(cap_r_mid16, c->a0, c->sweep,
                                  &c->cap0_x16, &c->cap0_y16, &c->cap1_x16, &c->cap1_y16,
                                  &c->c0, &c->s0, &c->c1, &c->s1);
        c->has_fill = 1u;
        if (env->muted) {
            const uint32_t inset = (uint32_t)c->sweep / 5u;
            c->fill_a0 = (uint16_t)((c->a0 + inset) & 0xFFFFu);
            c->fill_sweep = (uint16_t)(c->sweep - 2u * inset);
            c->col_deep  = 0x9280u; c->col_core = 0xFCA2u;
            c->col_ridge = 0xFFF0u; c->col_glow = 0xD3E0u;
            c->fill_a    = (uint8_t)(210 + (breath_01 >> 3));
            c->glow_a    = (uint8_t)(95 + (breath_01 >> 3));
        } else if (env->codex_active) {
            c->fill_a0 = c->a0; c->fill_sweep = c->sweep;
            c->col_deep  = 0x6119u; c->col_core = 0xB23Fu;
            c->col_ridge = 0xF5FFu; c->col_glow = 0x91DFu;
            c->fill_a    = (uint8_t)(205 + ((breath_01 * 50) >> 8));
            c->glow_a    = (uint8_t)(115 + ((breath_01 * 80) >> 8));
            c->shimmer_pos = (uint8_t)(((now_ms >> 3) % 254u) + 1u);
        } else if (env->voice_busy) {
            c->fill_a0 = c->a0; c->fill_sweep = c->sweep;
            c->col_deep  = 0x03BCu; c->col_core = 0x07FFu;
            c->col_ridge = 0xEFFFu; c->col_glow = 0x06FFu;
            c->fill_a    = (uint8_t)(215 + ((breath_01 * 40) >> 8));
            c->glow_a    = (uint8_t)(120 + ((breath_01 * 85) >> 8));
            c->shimmer_pos = (uint8_t)(((now_ms >> 3) % 254u) + 1u);
        } else {
            c->fill_a0 = c->a0; c->fill_sweep = c->sweep;
            c->col_deep  = 0x0276u; c->col_core = 0x05BFu;
            c->col_ridge = 0x7FFFu; c->col_glow = 0x04DFu;
            c->fill_a    = (uint8_t)(195 + ((breath_01 * 36) >> 8));
            c->glow_a    = (uint8_t)(78 + ((breath_01 * 38) >> 8));
        }
        g3_capsule_compute_caps16(cap_r_mid16, c->fill_a0, c->fill_sweep,
                                  &c->fcap0_x16, &c->fcap0_y16, &c->fcap1_x16, &c->fcap1_y16,
                                  &c->fc0, &c->fs0, &c->fc1, &c->fs1);
        c->head_x16 = c->fcap1_x16;
        c->head_y16 = c->fcap1_y16;
    }

    /* Capsule 1: Right (1:30..4:30) — Battery capsule */
    {
        g3_capsule_t *c = &cc->caps[1];
        c->a0 = (uint16_t)g3_clk_deg_q16(450);
        c->sweep = 16384u;
        g3_capsule_compute_caps16(cap_r_mid16, c->a0, c->sweep,
                                  &c->cap0_x16, &c->cap0_y16, &c->cap1_x16, &c->cap1_y16,
                                  &c->c0, &c->s0, &c->c1, &c->s1);
        if (env->batt_pct != 0xFFu) {
            const uint32_t pct = env->batt_pct > 100u ? 100u : env->batt_pct;
            if (pct > 0u) {
                c->has_fill = 1u;
                uint32_t fs = ((uint32_t)c->sweep * pct) / 100u;
                if (fs < 1u) { fs = 1u; }
                c->fill_sweep = (uint16_t)fs;
                c->fill_a0 = (uint16_t)(((uint32_t)c->a0 + c->sweep - fs) & 0xFFFFu);
                g3_capsule_compute_caps16(cap_r_mid16, c->fill_a0, c->fill_sweep,
                                          &c->fcap0_x16, &c->fcap0_y16, &c->fcap1_x16, &c->fcap1_y16,
                                          &c->fc0, &c->fs0, &c->fc1, &c->fs1);
                c->head_x16 = c->fcap0_x16;
                c->head_y16 = c->fcap0_y16;

                if (env->charging) {
                    c->col_deep  = 0x03A4u; c->col_core = 0x06E9u;
                    c->col_ridge = 0xB7F8u; c->col_glow = 0x05E7u;
                    c->fill_a    = (uint8_t)(215 + ((breath_01 * 40) >> 8));
                    c->glow_a    = (uint8_t)(105 + ((breath_01 * 65) >> 8));
                    c->shimmer_pos = (uint8_t)(255u - (((now_ms >> 3) % 254u) + 1u));
                } else if (pct < 20u) {
                    c->col_deep  = 0xA260u; c->col_core = 0xFC60u;
                    c->col_ridge = 0xFFE8u; c->col_glow = 0xEA60u;
                    c->fill_a    = (uint8_t)(200 + ((breath_01 * 55) >> 8));
                    c->glow_a    = (uint8_t)(105 + ((breath_01 * 75) >> 8));
                } else {
                    c->col_deep  = 0x0338u; c->col_core = 0x06FFu;
                    c->col_ridge = 0xBFFFu; c->col_glow = 0x05DFu;
                    c->fill_a    = 238u; c->glow_a = 98u;
                }
            }
        }
    }

    /* Capsule 2: Bottom-Left (6:30..9:00) — Acoustic / Mic capsule */
    {
        g3_capsule_t *c = &cc->caps[2];
        c->a0 = (uint16_t)g3_clk_deg_q16(1950);
        c->sweep = (uint16_t)((750 * 65536) / 3600);
        g3_capsule_compute_caps16(cap_r_mid16, c->a0, c->sweep,
                                  &c->cap0_x16, &c->cap0_y16, &c->cap1_x16, &c->cap1_y16,
                                  &c->c0, &c->s0, &c->c1, &c->s1);
        if (!env->muted) {
            const uint32_t lvl = 36u + (((uint32_t)env->amp * 219u) >> 8);
            c->has_fill = 1u;
            c->fill_a0 = c->a0;
            uint32_t fs = ((uint32_t)c->sweep * lvl) / 255u;
            if (fs < 1u) { fs = 1u; }
            c->fill_sweep = (uint16_t)fs;
            g3_capsule_compute_caps16(cap_r_mid16, c->fill_a0, c->fill_sweep,
                                      &c->fcap0_x16, &c->fcap0_y16, &c->fcap1_x16, &c->fcap1_y16,
                                      &c->fc0, &c->fs0, &c->fc1, &c->fs1);
            c->head_x16 = c->fcap1_x16;
            c->head_y16 = c->fcap1_y16;

            c->col_deep  = 0x9260u;
            c->col_core  = g3_lerp565(0xFB80u, 0xFDE4u, env->amp);
            c->col_ridge = 0xFFF2u;
            c->col_glow  = 0xE380u;
            c->fill_a    = (uint8_t)(200 + ((env->amp * 55) >> 8));
            c->glow_a    = (uint8_t)(72 + ((env->amp * 115) >> 8));
        }
    }

    /* Capsule 3: Top-Left (9:30..10:50) — Wi-Fi Link capsule */
    {
        g3_capsule_t *c = &cc->caps[3];
        c->a0 = (uint16_t)g3_clk_deg_q16(-750);
        c->sweep = (uint16_t)((400 * 65536) / 3600);
        c->dividers = 4u;
        g3_capsule_compute_caps16(cap_r_mid16, c->a0, c->sweep,
                                  &c->cap0_x16, &c->cap0_y16, &c->cap1_x16, &c->cap1_y16,
                                  &c->c0, &c->s0, &c->c1, &c->s1);
        for (int j = 0; j < 3; ++j) {
            const uint32_t da = (c->a0 + ((uint32_t)c->sweep * (uint32_t)(j + 1)) / 4u) & 0xFFFFu;
            c->div_c[j] = (int16_t)glass_cos_q15(da);
            c->div_s[j] = (int16_t)glass_sin_q15(da);
        }
        const uint32_t bars = env->wifi_bars > 4u ? 4u : env->wifi_bars;
        if (bars > 0u) {
            c->has_fill = 1u;
            c->fill_a0 = c->a0;
            c->fill_sweep = (uint16_t)(((uint32_t)c->sweep * bars) / 4u);
            g3_capsule_compute_caps16(cap_r_mid16, c->fill_a0, c->fill_sweep,
                                      &c->fcap0_x16, &c->fcap0_y16, &c->fcap1_x16, &c->fcap1_y16,
                                      &c->fc0, &c->fs0, &c->fc1, &c->fs1);
            c->head_x16 = c->fcap1_x16;
            c->head_y16 = c->fcap1_y16;

            c->col_deep  = 0x0384u; c->col_core = 0x06C9u;
            c->col_ridge = 0xAFF6u; c->col_glow = 0x05A7u;
            c->fill_a    = 236u; c->glow_a = 96u;
        }
    }

    for (int ci = 0; ci < 4; ++ci) {
        g3_capsule_t *c = &cc->caps[ci];
        g3_arc_bbox(c->a0, c->sweep, 900u, 208, JR_G3_CHRONO_BEZEL_R_MAX,
                    &c->bx0, &c->bx1, &c->by0, &c->by1);
        if (c->shimmer_pos > 0u && c->fill_sweep > 0u) {
            const uint32_t sa = (c->fill_a0 + ((uint32_t)c->fill_sweep * c->shimmer_pos) / 255u) & 0xFFFFu;
            c->shim_c = (int16_t)glass_cos_q15(sa);
            c->shim_s = (int16_t)glass_sin_q15(sa);
            /* Shimmer half-width in Q2 half-pixels (~42/255 of fill arc length at r=219) */
            const int32_t arc_px = (int32_t)(((uint32_t)c->fill_sweep * 219u * 6u) >> 16);
            c->shim_hw_q2 = (int16_t)g3_clamp((arc_px * 42 * 8) / 255, 16, 260);
            c->shim_inv_q8 = (uint16_t)((165u << 8) / (uint32_t)c->shim_hw_q2);
        }

        /* Pre-composite exact 48-entry radial cross-section (r in [208, 231.5]) */
        for (int i = 0; i < 48; ++i) {
            const int32_t r_off = i * 8 - 176; /* -176..+200 Q4 */
            const int32_t d = g3_abs32(r_off);

            /* Unfilled track + chamfered glass rim */
            uint16_t tc = 0x0842u;
            uint8_t ta = 0u;
            if (d < 120) {
                int tcov = (d > 104) ? g3_clamp(((120 - d) * 255) >> 4, 0, 255) : 255;
                ta = (uint8_t)((tcov * 218) >> 8);
                const int32_t drim = g3_abs32(d - 106);
                if (drim < 12) {
                    const unsigned ra = (unsigned)((((12 - drim) * 5440) >> 8) * 175 >> 8);
                    const uint16_t rc = (r_off < 0) ? 0x3A0Du : 0x1906u;
                    g3_precomp_over(tc, ta, rc, ra, &tc, &ta);
                }
            }
            c->trk_col[i] = tc;
            c->trk_a[i] = ta;

            /* Filled body = bloom + track + rim + 3D neon fill */
            uint16_t fc = tc;
            uint8_t fa = ta;
            uint8_t fb = 0u;
            if (c->has_fill) {
                if (d >= 70 && d < 182) {
                    const int32_t gd = d > 78 ? (d - 78) : 0;
                    const int gt = ((104 - gd) * 628) >> 8;
                    fb = (uint8_t)g3_quad_alpha(gt, c->glow_a, 255u);
                }
                if (d < 86) {
                    const int fcov = (d > 70) ? g3_clamp(((86 - d) * 255) >> 4, 0, 255) : 255;
                    const int edge_t = g3_clamp(((78 - d) * 837) >> 8, 0, 255);
                    uint16_t body = g3_lerp565(c->col_deep, c->col_core, edge_t);
                    const int32_t d_ridge = g3_abs32(r_off + 20);
                    if (d_ridge < 34) {
                        body = g3_lerp565(body, c->col_ridge, ((34 - d_ridge) * 1430) >> 8);
                    }
                    const unsigned ba = (unsigned)((fcov * c->fill_a) >> 8);
                    g3_precomp_over(tc, ta, body, ba, &fc, &fa);
                }
            }
            c->fil_col[i] = fc;
            c->fil_swp[i] = glass_bswap(fc);
            c->fil_a[i] = fa;
            c->fil_bloom[i] = fb;
        }
    }
}

/* Zero-isqrt linearized sonar ripples strictly in r in [106, 168]. */
static G3_NOINLINE void g3_chrono_sonar_ripples(uint16_t *dst, int y0, int y1,
                                                bool swap_bytes, const g3_chrono_cache_t *cc,
                                                int strength)
{
    const int rmin2_sq = (2 * JR_G3_CHRONO_RIPPLE_R_MIN) * (2 * JR_G3_CHRONO_RIPPLE_R_MIN);
    const int rmax2_sq = (2 * JR_G3_CHRONO_RIPPLE_R_MAX) * (2 * JR_G3_CHRONO_RIPPLE_R_MAX);
    const uint16_t ripple_col = cc->rip_col;

    for (int k = 0; k < 3; ++k) {
        const int peak_a = ((int)cc->ring_a[k] * strength) >> 8;
        if (peak_a <= 0) {
            continue;
        }
        const int32_t rc16 = cc->ring_r16[k];
        const int32_t rc2_sq = cc->ring_r2_sq[k];
        const int32_t inv_q15 = cc->ring_inv_q15[k];
        const int32_t hw16 = cc->ring_hw[k];
        const uint32_t inv_hw = cc->ring_inv_hw[k];
        const int r_c = (int)(rc16 >> 4);
        const int band_in = g3_clamp(r_c - 2, JR_G3_CHRONO_RIPPLE_R_MIN, JR_G3_CHRONO_RIPPLE_R_MAX);
        const int band_out = g3_clamp(r_c + 3, JR_G3_CHRONO_RIPPLE_R_MIN, JR_G3_CHRONO_RIPPLE_R_MAX);

        int ry0 = 232 - band_out;
        int ry1 = 233 + band_out;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, band_in, band_out, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t dr_signed = ((r2_sq - rc2_sq) * inv_q15) >> 15;
                    const int32_t dr = g3_abs32(dr_signed);
                    if (dr >= hw16) {
                        continue;
                    }
                    if (k == 2) {
                        const int32_t r16_approx = rc16 + dr_signed;
                        if (g3_abs32(r16_approx - cc->ring_r16[0]) < cc->ring_hw[0] ||
                            g3_abs32(r16_approx - cc->ring_r16[1]) < cc->ring_hw[1]) {
                            continue;
                        }
                    }
                    const uint32_t cov = ((uint32_t)(hw16 - dr) * inv_hw) >> 8;
                    unsigned a = (unsigned)((cov * (uint32_t)peak_a) >> 8);
                    if (a > 24u) {
                        a = 24u;
                    }
                    if (a > 0u) {
                        row[x] = g3_add565(row[x], ripple_col, a, swap_bytes);
                    }
                }
            }
        }
    }
}

/* Band 2: 360-deg precision micrometer tick bezel strictly in r in [186, 207]. */
static G3_NOINLINE void g3_chrono_bezel_rows(uint16_t *dst, int y0, int y1,
                                             bool swap_bytes, const g3_chrono_cache_t *cc,
                                             int strength)
{
    g3_hairline_ring_rows(dst, y0, y1, swap_bytes, 188, 12, 0x2189u, 52, strength,
                          JR_G3_CHRONO_BEZEL_R_MIN, 207);
    g3_hairline_ring_rows(dst, y0, y1, swap_bytes, 206, 12, 0x29AAu, 58, strength,
                          JR_G3_CHRONO_BEZEL_R_MIN, 207);

    for (int k = 0; k < 180; ++k) {
        int32_t ux, uy;
        g3_unit_vec_q15(k, 45, k_g3_cos180_q15, &ux, &uy);

        const int yc = 233 + (int)((197 * uy) >> 15);
        if (yc + 16 < y0 || yc - 16 >= y1) {
            continue;
        }

        if ((k % 45) == 0) {
            g3_radial_tick_rows(dst, y0, y1, swap_bytes, ux, uy,
                                189 * 16, 205 * 16, 22, 56,
                                cc->col_vernier_body, cc->col_vernier_body, cc->col_vernier_core,
                                (unsigned)strength, (unsigned)((92 * strength) >> 8),
                                JR_G3_CHRONO_BEZEL_R_MIN, 207);
        } else if ((k % 15) == 0) {
            g3_radial_tick_rows(dst, y0, y1, swap_bytes, ux, uy,
                                190 * 16, 204 * 16, 17, 28,
                                cc->col_hour_inner, cc->col_hour_outer, 0u,
                                (unsigned)((235 * strength) >> 8), (unsigned)((42 * strength) >> 8),
                                JR_G3_CHRONO_BEZEL_R_MIN, 207);
        } else {
            const bool mid = ((k % 5) == 0);
            const int sheen = 220 - (((yc - 233) * 70) / 466);
            const uint16_t gun = g3_lerp565(0x31C8u, 0x6B6Eu, g3_clamp((sheen - 170) * 3, 0, 255));
            const unsigned ba = (unsigned)(((mid ? 185 : 138) * strength) >> 8);
            g3_radial_tick_rows(dst, y0, y1, swap_bytes, ux, uy,
                                (mid ? 193 : 194) * 16, 202 * 16, 11, 0,
                                gun, gun, 0u, ba, 0u,
                                JR_G3_CHRONO_BEZEL_R_MIN, 207);
        }
    }
}

/* Band 3: 4 Sculpted Curved Status Capsules in r in [208, 231].
 * ZERO atan2! Uses linear half-plane stepping + pre-composited 48-entry radial LUT! */
static G3_NOINLINE void g3_chrono_capsules_rows(uint16_t *dst, int y0, int y1,
                                                bool swap_bytes, const g3_chrono_cache_t *cc,
                                                int strength)
{
    const int32_t cap_r_mid16 = 219 * 16;
    const int32_t track_hw16  = 112;
    const int32_t fill_hw16   = 78;
    const int rmin2_sq = (2 * 208) * (2 * 208);
    const int rmax2_sq = (2 * JR_G3_CHRONO_BEZEL_R_MAX) * (2 * JR_G3_CHRONO_BEZEL_R_MAX);
    const bool full_str = (strength >= 255);

    int ry0 = 232 - JR_G3_CHRONO_BEZEL_R_MAX;
    int ry1 = 233 + JR_G3_CHRONO_BEZEL_R_MAX;
    if (ry0 < y0) { ry0 = y0; }
    if (ry0 < 0)  { ry0 = 0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry1 >= G3_H) { ry1 = G3_H - 1; }

    for (int y = ry0; y <= ry1; ++y) {
        int active[4];
        int nact = 0;
        for (int ci = 0; ci < 4; ++ci) {
            if (y >= cc->caps[ci].by0 && y <= cc->caps[ci].by1) {
                active[nact++] = ci;
            }
        }
        if (nact == 0) {
            continue;
        }
        g3_xspan_t spans[2];
        const int nsp = g3_annulus_row_spans(y, 208, JR_G3_CHRONO_BEZEL_R_MAX, spans);
        if (nsp == 0) {
            continue;
        }
        const int dy2 = 2 * y - G3_CY2;
        const int dy2_sq = dy2 * dy2;
        const int32_t py16 = y * 16 + 8;
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;

        for (int ai = 0; ai < nact; ++ai) {
            const g3_capsule_t *hit = &cc->caps[active[ai]];
            const int bx0 = hit->bx0;
            const int bx1 = hit->bx1;
            const bool has_fill = hit->has_fill != 0u;
            const bool has_div  = hit->dividers != 0u;
            const bool has_shim = hit->shimmer_pos != 0u && hit->shim_hw_q2 > 0;
            const int32_t c0 = hit->c0, s0 = hit->s0, c1 = hit->c1, s1 = hit->s1;
            const int32_t fc0 = hit->fc0, fs0 = hit->fs0, fc1 = hit->fc1, fs1 = hit->fs1;
            const int32_t h0_y = dy2 * c0;
            const int32_t h1_y = -dy2 * c1;
            const int32_t fh0_y = dy2 * fc0;
            const int32_t fh1_y = -dy2 * fc1;
            const int32_t dh0 = -2 * s0;
            const int32_t dh1 =  2 * s1;
            const int32_t dfh0 = -2 * fs0;
            const int32_t dfh1 =  2 * fs1;
            const unsigned str = (unsigned)strength;

            for (int s = 0; s < nsp; ++s) {
                int xlo = spans[s].lo < bx0 ? bx0 : spans[s].lo;
                int xhi = spans[s].hi > bx1 ? bx1 : spans[s].hi;
                if (xlo > xhi) {
                    continue;
                }
                int dx2 = 2 * xlo - G3_CX2;
                int32_t h0  = h0_y  - dx2 * s0;
                int32_t h1  = h1_y  + dx2 * s1;
                int32_t fh0 = fh0_y - dx2 * fs0;
                int32_t fh1 = fh1_y + dx2 * fs1;

                for (int x = xlo; x <= xhi; ++x, dx2 += 2, h0 += dh0, h1 += dh1, fh0 += dfh0, fh1 += dfh1) {
                    /* Fast linear half-plane reject (13.7 px past either endcap -> skip!) */
                    if (h0 < -900000 || h1 < -900000) {
                        continue;
                    }
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    /* 2-term Taylor around r=219: max err 0.125 px, ZERO isqrt! */
                    const int32_t d = ((r2_sq - 191844) * 299) >> 15;
                    const int32_t r_off = d - ((d * d * 37) >> 18);

                    /* ULTRA-FAST PATH: interior body pixel (92% of capsule area!) */
                    if (h0 >= 0 && h1 >= 0) {
                        const int ri = g3_clamp((r_off + 176) >> 3, 0, 47);
                        if (has_fill && fh0 >= 0 && fh1 >= 0) {
                            unsigned fa = hit->fil_a[ri];
                            unsigned fb = hit->fil_bloom[ri];
                            if ((fa | fb) == 0u) {
                                continue;
                            }
                            uint16_t col = hit->fil_col[ri];
                            bool custom_col = false;
                            if (has_shim && g3_abs32(r_off) < fill_hw16 + 8) {
                                const int32_t ds = g3_abs32(dy2 * hit->shim_c - dx2 * hit->shim_s) >> 14;
                                if (ds < hit->shim_hw_q2) {
                                    const int st = (int)(((uint32_t)(hit->shim_hw_q2 - ds) * (uint32_t)hit->shim_inv_q8) >> 8);
                                    col = g3_lerp565(col, 0xFFFFu, st);
                                    custom_col = true;
                                }
                            } else if (has_div && g3_abs32(r_off) < fill_hw16 + 8) {
                                int32_t md = 256;
                                for (int j = 0; j < 3; ++j) {
                                    const int32_t dj = g3_abs32(dy2 * hit->div_c[j] - dx2 * hit->div_s[j]) >> 13;
                                    if (dj < md) { md = dj; }
                                }
                                if (md < 16) {
                                    uint16_t pix = g3_to_nat(row[x], swap_bytes);
                                    if (fb > 0u) {
                                        pix = g3_add565_nat(pix, hit->col_glow, full_str ? fb : ((fb * str) >> 8));
                                    }
                                    const unsigned ta_u = hit->trk_a[ri];
                                    if (ta_u > 0u) {
                                        pix = g3_blend565_nat(pix, hit->trk_col[ri], full_str ? ta_u : ((ta_u * str) >> 8));
                                    }
                                    fa = (fa * (unsigned)md) >> 4;
                                    if (fa > 0u) {
                                        pix = g3_blend565_nat(pix, col, full_str ? fa : ((fa * str) >> 8));
                                    }
                                    row[x] = g3_to_nat(pix, swap_bytes);
                                    continue;
                                }
                            }
                            if (full_str && fb == 0u && fa >= 250u) {
                                row[x] = custom_col ? g3_to_nat(col, swap_bytes)
                                                    : (swap_bytes ? hit->fil_swp[ri] : col);
                                continue;
                            }
                            uint16_t pix = g3_to_nat(row[x], swap_bytes);
                            if (fb > 0u) {
                                pix = g3_add565_nat(pix, hit->col_glow, full_str ? fb : ((fb * str) >> 8));
                            }
                            if (fa > 0u) {
                                pix = g3_blend565_nat(pix, col, full_str ? fa : ((fa * str) >> 8));
                            }
                            row[x] = g3_to_nat(pix, swap_bytes);
                            continue;
                        }
                        /* Unfilled track interior pixel (or near fill endcap inside track!) */
                        if (!has_fill || (fh0 < -900000 || fh1 < -900000)) {
                            const unsigned ta = hit->trk_a[ri];
                            if (ta > 0u) {
                                uint16_t pix = g3_to_nat(row[x], swap_bytes);
                                pix = g3_blend565_nat(pix, hit->trk_col[ri], full_str ? ta : ((ta * str) >> 8));
                                row[x] = g3_to_nat(pix, swap_bytes);
                            }
                            continue;
                        }
                    }

                    /* ENDCAP PATH (~8% of capsule area): exact semicircular caps, ZERO atan2! */
                    const int32_t px16 = x * 16 + 8;
                    int32_t d_tr;
                    if (h0 >= 0 && h1 >= 0) {
                        d_tr = g3_abs32(r_off);
                    } else {
                        const int32_t ex = (h0 < 0) ? hit->cap0_x16 : hit->cap1_x16;
                        const int32_t ey = (h0 < 0) ? hit->cap0_y16 : hit->cap1_y16;
                        const int32_t dx = px16 - ex;
                        const int32_t dy = py16 - ey;
                        const uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
                        if (d2 >= 208u * 208u) {
                            continue;
                        }
                        d_tr = (int32_t)g3_isqrt_fast(d2);
                    }
                    if (d_tr >= track_hw16 + 96) {
                        continue;
                    }

                    int32_t d_fl = 4096;
                    if (has_fill) {
                        if (fh0 >= 0 && fh1 >= 0) {
                            d_fl = g3_abs32(r_off);
                        } else if (fh0 >= -900000 && fh1 >= -900000) {
                            const int32_t ex = (fh0 < 0) ? hit->fcap0_x16 : hit->fcap1_x16;
                            const int32_t ey = (fh0 < 0) ? hit->fcap0_y16 : hit->fcap1_y16;
                            const int32_t dx = px16 - ex;
                            const int32_t dy = py16 - ey;
                            const uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
                            if (d2 < 182u * 182u) {
                                d_fl = (int32_t)g3_isqrt_fast(d2);
                            }
                        }
                    }

                    uint16_t pix = g3_to_nat(row[x], swap_bytes);
                    if (has_fill && d_fl < fill_hw16 + 104) {
                        const int32_t gd = d_fl > fill_hw16 ? (d_fl - fill_hw16) : 0;
                        const int gt = ((104 - gd) * 628) >> 8;
                        const unsigned ga = g3_quad_alpha(gt, hit->glow_a, str);
                        if (ga > 0u) {
                            pix = g3_add565_nat(pix, hit->col_glow, ga);
                        }
                    }
                    if (d_tr < track_hw16 + 8) {
                        int tcov = (d_tr > track_hw16 - 8) ? g3_clamp(((track_hw16 + 8 - d_tr) * 255) >> 4, 0, 255) : 255;
                        pix = g3_blend565_nat(pix, 0x0842u, (unsigned)((tcov * 218 * strength) >> 16));
                        const int32_t d_rim = g3_abs32(d_tr - (track_hw16 - 6));
                        if (d_rim < 12) {
                            const unsigned rcov = (unsigned)(((12 - d_rim) * 5440) >> 8);
                            const uint16_t rc = (r_off < 0) ? 0x3A0Du : 0x1906u;
                            pix = g3_blend565_nat(pix, rc, (rcov * 175u * str) >> 16);
                        }
                    }
                    if (has_fill && d_fl < fill_hw16 + 8) {
                        int fcov = (d_fl > fill_hw16 - 8) ? g3_clamp(((fill_hw16 + 8 - d_fl) * 255) >> 4, 0, 255) : 255;
                        const int edge_t = g3_clamp(((fill_hw16 - d_fl) * 837) >> 8, 0, 255);
                        uint16_t col = g3_lerp565(hit->col_deep, hit->col_core, edge_t);
                        const int32_t d_ridge = g3_abs32(r_off + 20);
                        if (d_ridge < 34) {
                            col = g3_lerp565(col, hit->col_ridge, ((34 - d_ridge) * 1430) >> 8);
                        }
                        pix = g3_blend565_nat(pix, col, (unsigned)((fcov * hit->fill_a * strength) >> 16));
                    }
                    row[x] = g3_to_nat(pix, swap_bytes);
                }
            }
        }
    }

    for (int ci = 1; ci <= 2; ++ci) {
        if (cc->caps[ci].has_fill && cc->caps[ci].fill_sweep > 128u) {
            g3_glow_orb_annulus(dst, y0, y1, swap_bytes,
                                cc->caps[ci].head_x16, cc->caps[ci].head_y16, 6,
                                cc->caps[ci].col_ridge,
                                (uint8_t)((135 * strength) >> 8),
                                208, JR_G3_CHRONO_BEZEL_R_MAX);
        }
    }
    (void)cap_r_mid16;
}

void jr_g3_chrono_arc_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                           uint32_t now_ms, const jr_g3_chrono_env_t *env,
                           int strength)
{
    if (dst == NULL || nrows <= 0 || strength <= 0) {
        return;
    }
    if (strength > 255) {
        strength = 255;
    }
    const int y1 = y0 + nrows;
    if (y1 <= 0 || y0 >= G3_H) {
        return;
    }

    jr_g3_chrono_env_t def_env;
    if (env == NULL) {
        memset(&def_env, 0, sizeof(def_env));
        def_env.batt_pct = 0xFFu;
        env = &def_env;
    }

    g3_chrono_cache_t *cc = s_chrono_cache_get();
    if (cc == NULL) {
        return;
    }
    g3_chrono_prepare(cc, now_ms, env);
    g3_chrono_sonar_ripples(dst, y0, y1, swap_bytes, cc, strength);
    g3_chrono_bezel_rows(dst, y0, y1, swap_bytes, cc, strength);
    g3_chrono_capsules_rows(dst, y0, y1, swap_bytes, cc, strength);
}

/* ==========================================================================
 * OPTION 2: AETHERIUM (Live Voice Harmonic Ring & Crystalline Vocal Ribbon)
 * ========================================================================== */

/* 16-byte power-of-2 cache-aligned per-angle wave entry (single cache line!). */
typedef struct {
    int16_t  rw[3];
    int16_t  r_lo;
    int16_t  r_hi;
    uint16_t wc[3];
} g3_aeth_ang_t;

typedef struct {
    bool     valid;
    uint32_t now_ms;
    int      mode;
    uint8_t  amp;
    uint8_t  bands[16];

    uint8_t  sym[32];
    uint8_t  energy;
    uint16_t phase[3];
    int16_t  wave_amp[3];

    uint16_t col_w[3];
    uint16_t col_peak;
    uint16_t col_fil_base;
    uint16_t col_fil_mid;
    uint16_t col_fil_tip;
    uint16_t col_rib_core;
    uint16_t col_rib_halo;
    uint16_t col_rib_sec;

    int16_t  r_wave_min;
    int16_t  r_wave_max;

    /* 512-entry per-frame angular wave & color LUT (8 KB .bss, ZERO per-pixel sin!) */
    g3_aeth_ang_t ang[512];

    /* 120 precomputed radial FFT filament endpoints & spectra */
    int16_t  fil_lo[120];
    int16_t  fil_hi[120];
    uint8_t  fil_spec[120];
    uint16_t fil_ramp[16];
    uint16_t fil_ramp_low[16];

    /* Lower-center Crystalline Vocal Ribbon precomputed per-column arrays */
    uint8_t  rib_h1[G3_RIBBON_W];
    uint8_t  rib_h2[G3_RIBBON_W];
    uint8_t  rib_win[G3_RIBBON_W];
    uint16_t rib_inv_h1[G3_RIBBON_W];
} g3_aeth_cache_t;

G3_DEFINE_CACHE(g3_aeth_cache_t, s_aeth_cache)

static G3_NOINLINE void g3_aeth_prepare(g3_aeth_cache_t *c, uint32_t now_ms,
                                        jr_g3_aeth_mode_t mode,
                                        const uint8_t bands[16], uint8_t amp)
{
    uint8_t zero[16] = { 0 };
    const uint8_t *b = bands ? bands : zero;

    if (c->valid && c->now_ms == now_ms && c->mode == (int)mode &&
        c->amp == amp && memcmp(c->bands, b, 16) == 0) {
        return;
    }

    c->valid = true;
    c->now_ms = now_ms;
    c->mode = (int)mode;
    c->amp = amp;
    memcpy(c->bands, b, 16);

    int sum = 0;
    for (int i = 0; i < 16; ++i) {
        sum += b[i];
    }
    const bool has_spec = (sum > 0);
    const int avg = has_spec ? (sum >> 4) : (int)amp;
    c->energy = (uint8_t)g3_clamp(avg, 0, 255);

    switch (mode) {
    case JR_G3_AETH_THINK:
        c->col_w[0] = 0x07FFu; c->col_w[1] = 0x12BFu; c->col_w[2] = 0xC1FFu;
        c->col_peak = 0xFD84u;
        c->col_fil_base = 0x1195u; c->col_fil_mid = 0x633Fu; c->col_fil_tip = 0xE6FFu;
        c->col_rib_core = 0xF7FFu; c->col_rib_halo = 0x54FFu; c->col_rib_sec = 0xC23Fu;
        c->phase[0] = (uint16_t)((now_ms * 46u) & 0xFFFFu);
        c->phase[1] = (uint16_t)((now_ms * 33u + 21845u) & 0xFFFFu);
        c->phase[2] = (uint16_t)((now_ms * 64u + 43690u) & 0xFFFFu);
        c->wave_amp[0] = 200; c->wave_amp[1] = 175; c->wave_amp[2] = 215;
        break;

    case JR_G3_AETH_SPEAK:
        c->col_w[0] = 0x07FFu; c->col_w[1] = 0x039Fu; c->col_w[2] = 0xD1DFu;
        c->col_peak = 0xFD63u;
        c->col_fil_base = 0x0276u; c->col_fil_mid = 0x07FFu; c->col_fil_tip = 0xFFE8u;
        c->col_rib_core = 0xFFFFu; c->col_rib_halo = 0x07FFu; c->col_rib_sec = 0xC25Fu;
        c->phase[0] = (uint16_t)((now_ms * 58u) & 0xFFFFu);
        c->phase[1] = (uint16_t)((now_ms * 41u + 20000u) & 0xFFFFu);
        c->phase[2] = (uint16_t)((now_ms * 78u + 41000u) & 0xFFFFu);
        c->wave_amp[0] = 235; c->wave_amp[1] = 195; c->wave_amp[2] = 240;
        break;

    case JR_G3_AETH_LISTEN:
    default:
        c->col_w[0] = 0x07FDu; c->col_w[1] = 0x039Eu; c->col_w[2] = 0x927Fu;
        c->col_peak = 0xFDC6u;
        c->col_fil_base = 0x0254u; c->col_fil_mid = 0x07FBu; c->col_fil_tip = 0xCFFFu;
        c->col_rib_core = 0xF7FFu; c->col_rib_halo = 0x07FBu; c->col_rib_sec = 0x45DFu;
        c->phase[0] = (uint16_t)((now_ms * 34u) & 0xFFFFu);
        c->phase[1] = (uint16_t)((now_ms * 25u + 22000u) & 0xFFFFu);
        c->phase[2] = (uint16_t)((now_ms * 48u + 44000u) & 0xFFFFu);
        c->wave_amp[0] = 190; c->wave_amp[1] = 160; c->wave_amp[2] = 195;
        break;
    }

    for (int i = 0; i < 16; ++i) {
        const int rt = (i * 255) / 15;
        c->fil_ramp_low[i] = g3_lerp565(c->col_fil_base, c->col_fil_mid, rt);
        c->fil_ramp[i] = (rt > 195)
            ? g3_lerp565(c->col_fil_mid, c->col_fil_tip, (rt - 195) * 4)
            : c->fil_ramp_low[i];
    }

    const uint32_t flow = (now_ms * 28u) & 0xFFFFu;
    for (int i = 0; i < 16; ++i) {
        const int s1 = glass_sin_q15(((uint32_t)i * 4096u + flow) & 0xFFFFu);
        const int s2 = glass_sin_q15(((uint32_t)i * 6144u - (flow >> 1)) & 0xFFFFu);
        int floor_v = 26 + (((s1 * 14) + (s2 * 10)) >> 15) + ((int)amp >> 2);
        int v = has_spec ? ((int)b[i] > floor_v ? (int)b[i] : ((int)b[i] * 3 + floor_v) >> 2)
                         : floor_v;
        v = g3_clamp(v, 10, 255);
        c->sym[i] = (uint8_t)v;
        c->sym[31 - i] = (uint8_t)v;
    }

    for (int k = 0; k < 120; ++k) {
        const uint32_t ang12 = ((uint32_t)k * 65536u) / 120u;
        const uint32_t sp_i = ang12 >> 11;
        const int sp_f = (int)((ang12 >> 3) & 0xFFu);
        const int s0 = c->sym[sp_i];
        const int s1 = c->sym[(sp_i + 1u) & 31u];
        const int spec = s0 + (((s1 - s0) * sp_f) >> 8);
        c->fil_spec[k] = (uint8_t)spec;
        c->fil_lo[k] = (int16_t)(184 * 16 - ((spec * 320) >> 8));
        c->fil_hi[k] = (int16_t)(189 * 16 + ((spec * 368) >> 8));
    }

    const uint32_t p0 = c->phase[0];
    const uint32_t p1 = c->phase[1];
    const uint32_t p2 = c->phase[2];
    int32_t min_r16 = 214 * 16;
    int32_t max_r16 = 170 * 16;

    for (uint32_t i = 0u; i < 512u; ++i) {
        const uint32_t ang12 = i << 7;
        const uint32_t sp_i = i >> 4;
        const int sp_f = (int)((i & 15u) << 4);
        const int s0 = c->sym[sp_i];
        const int s1 = c->sym[(sp_i + 1u) & 31u];
        const int spec = s0 + (((s1 - s0) * sp_f) >> 8);
        const int mod = 72 + ((spec * 128 + (int)amp * 55) >> 8);

        const int32_t w0_s = (glass_sin_q15((ang12 * 3u + p0) & 0xFFFFu) * 3 +
                              glass_sin_q15((ang12 * 5u - p0 * 2u) & 0xFFFFu) * 2) / 5;
        const int32_t w1_s = (glass_sin_q15((ang12 * 2u - p1) & 0xFFFFu) * 3 +
                              glass_sin_q15((ang12 * 4u + p1) & 0xFFFFu) * 2) / 5;
        const int32_t w2_s = (glass_sin_q15((ang12 * 4u + p2) & 0xFFFFu) * 3 +
                              glass_sin_q15((ang12 * 7u - p2) & 0xFFFFu) * 2) / 5;

        int32_t rw0 = g3_clamp(192 * 16 + (int32_t)(((int64_t)w0_s * c->wave_amp[0] * mod) >> 23), 176 * 16, 208 * 16);
        int32_t rw1 = g3_clamp(190 * 16 + (int32_t)(((int64_t)w1_s * c->wave_amp[1] * mod) >> 23), 176 * 16, 208 * 16);
        int32_t rw2 = g3_clamp(194 * 16 + (int32_t)(((int64_t)w2_s * c->wave_amp[2] * mod) >> 23), 176 * 16, 208 * 16);

        g3_aeth_ang_t *e = &c->ang[i];
        e->rw[0] = (int16_t)rw0;
        e->rw[1] = (int16_t)rw1;
        e->rw[2] = (int16_t)rw2;

        int32_t lo_i = rw0 < rw1 ? rw0 : rw1;
        if (rw2 < lo_i) { lo_i = rw2; }
        int32_t hi_i = rw0 > rw1 ? rw0 : rw1;
        if (rw2 > hi_i) { hi_i = rw2; }
        e->r_lo = (int16_t)(lo_i - 82);
        e->r_hi = (int16_t)(hi_i + 82);

        const int32_t d0 = g3_abs32(rw0 - 192 * 16);
        const int32_t d1 = g3_abs32(rw1 - 192 * 16);
        const int32_t d2 = g3_abs32(rw2 - 192 * 16);
        e->wc[0] = (spec > 155 || d0 > 150)
            ? g3_lerp565(c->col_w[0], c->col_peak, g3_clamp(((spec - 140) * 2) + ((d0 - 130) >> 1), 0, 210))
            : c->col_w[0];
        e->wc[1] = (spec > 155 || d1 > 150)
            ? g3_lerp565(c->col_w[1], c->col_peak, g3_clamp(((spec - 140) * 2) + ((d1 - 130) >> 1), 0, 210))
            : c->col_w[1];
        e->wc[2] = (spec > 155 || d2 > 150)
            ? g3_lerp565(c->col_w[2], c->col_peak, g3_clamp(((spec - 140) * 2) + ((d2 - 130) >> 1), 0, 210))
            : c->col_w[2];

        if (e->r_lo < min_r16) { min_r16 = e->r_lo; }
        if (e->r_hi > max_r16) { max_r16 = e->r_hi; }
    }
    c->r_wave_min = (int16_t)g3_clamp((min_r16 >> 4) - 1, 168, 215);
    c->r_wave_max = (int16_t)g3_clamp((max_r16 >> 4) + 2, 168, 215);

    const uint32_t r_p1 = (now_ms * 72u) & 0xFFFFu;
    const uint32_t r_p2 = (now_ms * 108u) & 0xFFFFu;
    const int base_drive = 46 + ((c->energy * 145 + (int)amp * 64) >> 8);

    for (int i = 0; i < G3_RIBBON_W; ++i) {
        const uint32_t w_ang = (uint32_t)((i * 32768) / (G3_RIBBON_W - 1));
        const int sw = glass_sin_q15(w_ang);
        const int win = (int)(((int64_t)sw * sw * 255) >> 30);
        c->rib_win[i] = (uint8_t)g3_clamp(win, 0, 255);

        const int dist_c = g3_abs32(i - (G3_RIBBON_W / 2));
        const int b_q8 = (dist_c * 15 * 256) / (G3_RIBBON_W / 2);
        const int bi = g3_clamp(b_q8 >> 8, 0, 14);
        const int bf = b_q8 & 255;
        const int bval = (int)c->sym[bi] + ((((int)c->sym[bi + 1] - (int)c->sym[bi]) * bf) >> 8);

        const uint32_t x_ang1 = (uint32_t)((i * 3 * 65536) / G3_RIBBON_W);
        const uint32_t x_ang2 = (uint32_t)((i * 5 * 65536) / G3_RIBBON_W);
        const int c1 = glass_sin_q15((x_ang1 + r_p1) & 0xFFFFu);
        const int c2 = glass_sin_q15((x_ang2 - r_p2) & 0xFFFFu);
        const int c3 = glass_sin_q15((x_ang1 * 2u - r_p1 + 16384u) & 0xFFFFu);

        const int mod1 = 155 + (((c1 * 62) + (c2 * 38)) >> 15);
        const int mod2 = 140 + (((c2 * 70) + (c3 * 45)) >> 15);
        const int drv1 = (bval * 170 + base_drive * 85) >> 8;
        const int drv2 = (bval * 135 + base_drive * 120) >> 8;

        int h1 = (int)(((int64_t)win * drv1 * mod1 * 82) >> 24);
        int h2 = (int)(((int64_t)win * drv2 * mod2 * 70) >> 24);
        if (win > 12 && h1 < 6) {
            h1 = 6;
        }
        h1 = g3_clamp(h1, 0, 82);
        h2 = g3_clamp(h2, 0, 72);
        c->rib_h1[i] = (uint8_t)h1;
        c->rib_h2[i] = (uint8_t)h2;
        c->rib_inv_h1[i] = (h1 > 2) ? (uint16_t)((255u << 8) / (uint32_t)(h1 * 4)) : 0u;
    }
}

/* Aetherium Layer A: Outer Millimeter Tick Ring (r in [217, 229], ZERO atan2/isqrt!). */
static G3_NOINLINE void g3_aeth_ticks_rows(uint16_t *dst, int y0, int y1,
                                           bool swap_bytes, int strength)
{
    g3_hairline_ring_rows(dst, y0, y1, swap_bytes, 223, 10, 0x0358u, 24, strength,
                          217, 229);

    for (int k = 0; k < 120; ++k) {
        if ((k % 30) == 0) {
            continue;
        }
        int32_t ux, uy;
        g3_unit_vec_q15(k, 30, k_g3_cos120_q15, &ux, &uy);
        const int yc = 233 + (int)((223 * uy) >> 15);
        if (yc + 10 < y0 || yc - 10 >= y1) {
            continue;
        }
        const bool major = ((k % 5) == 0);
        const uint16_t tc = major ? 0x3D5Eu : 0x296Bu;
        const unsigned ta = (unsigned)(((major ? 205 : 145) * strength) >> 8);
        g3_radial_tick_rows(dst, y0, y1, swap_bytes, ux, uy,
                            (major ? 218 : 220) * 16, (major ? 228 : 226) * 16,
                            major ? 13 : 9, 0,
                            tc, tc, 0u, ta, 0u, 217, 229);
    }
}

/* Aetherium Layer B: 120 Radial FFT Spectrum Filaments (direct rays, ZERO atan2/isqrt!). */
static G3_NOINLINE void g3_aeth_filaments_rows(uint16_t *dst, int y0, int y1,
                                               bool swap_bytes, const g3_aeth_cache_t *c,
                                               int strength)
{
    const int rmin2_sq = (2 * 164) * (2 * 164);
    const int rmax2_sq = (2 * 212) * (2 * 212);

    for (int k = 0; k < 120; ++k) {
        int32_t ux, uy;
        g3_unit_vec_q15(k, 30, k_g3_cos120_q15, &ux, &uy);

        const int32_t f_lo = c->fil_lo[k];
        const int32_t f_hi = c->fil_hi[k];
        const int32_t y_r0 = G3_CY16 + (((f_lo - 8) * uy) >> 15);
        const int32_t y_r1 = G3_CY16 + (((f_hi + 8) * uy) >> 15);
        const int32_t pad_y = ((g3_abs32(ux) * 24) >> 15) + 20;
        int ty0 = ((y_r0 < y_r1 ? y_r0 : y_r1) - pad_y) >> 4;
        int ty1 = ((y_r0 > y_r1 ? y_r0 : y_r1) + pad_y) >> 4;
        if (ty1 < y0 || ty0 >= y1) {
            continue;
        }
        if (ty0 < y0)    { ty0 = y0; }
        if (ty0 < 0)     { ty0 = 0; }
        if (ty1 >= y1)   { ty1 = y1 - 1; }
        if (ty1 >= G3_H) { ty1 = G3_H - 1; }
        if (ty0 > ty1) {
            continue;
        }

        const int32_t x_r0 = G3_CX16 + (((f_lo - 8) * ux) >> 15);
        const int32_t x_r1 = G3_CX16 + (((f_hi + 8) * ux) >> 15);
        const int32_t pad_x = ((g3_abs32(uy) * 24) >> 15) + 20;
        int tx0 = ((x_r0 < x_r1 ? x_r0 : x_r1) - pad_x) >> 4;
        int tx1 = ((x_r0 > x_r1 ? x_r0 : x_r1) + pad_x) >> 4;
        if (tx0 < 0)     { tx0 = 0; }
        if (tx1 >= G3_W) { tx1 = G3_W - 1; }

        const int32_t auy = g3_abs32(uy);
        const bool use_slope = (auy >= 8192);
        const int32_t slope_q12 = use_slope ? ((ux * 4096) / uy) : 0;
        const int half_x = use_slope ? (int)(((40 * 32768) / auy) >> 4) + 2 : 0;

        const int spec = c->fil_spec[k];
        const unsigned f_alpha_s = (unsigned)(((85 + ((spec * 135) >> 8)) * strength) >> 8);
        const uint16_t *ramp = (spec > 140) ? c->fil_ramp : c->fil_ramp_low;
        const int32_t flen = f_hi - f_lo;
        const uint32_t inv_flen = flen > 0 ? ((15u << 12) / (uint32_t)flen) : 0u;
        const int32_t ux16 = ux * 16;
        const int32_t uy16 = uy * 16;

        for (int y = ty0; y <= ty1; ++y) {
            int xlo = tx0;
            int xhi = tx1;
            const int32_t dy16 = y * 16 + 8 - G3_CY16;
            if (use_slope) {
                const int xm = (G3_CX16 + ((dy16 * slope_q12) >> 12)) >> 4;
                if (xm - half_x > xlo) { xlo = xm - half_x; }
                if (xm + half_x < xhi) { xhi = xm + half_x; }
                if (xlo > xhi) {
                    continue;
                }
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            const int32_t dx16_0 = xlo * 16 + 8 - G3_CX16;
            int32_t r_acc = dx16_0 * ux + dy16 * uy;
            int32_t t_acc = dx16_0 * uy - dy16 * ux;

            for (int x = xlo; x <= xhi; ++x, r_acc += ux16, t_acc += uy16) {
                const uint32_t tan16 = (uint32_t)g3_abs32(t_acc >> 15);
                if (tan16 >= 24u) {
                    continue;
                }
                const int32_t r16 = r_acc >> 15;
                if (r16 < f_lo - 8 || r16 > f_hi + 8) {
                    continue;
                }
                const int dx2 = 2 * x - G3_CX2;
                const int r2_sq = dx2 * dx2 + dy2_sq;
                if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                    continue;
                }
                int fcov = k_g3_fil_cov[tan16];
                if (r16 < f_lo) {
                    fcov = (fcov * (r16 - (f_lo - 8))) >> 3;
                } else if (r16 > f_hi) {
                    fcov = (fcov * ((f_hi + 8) - r16)) >> 3;
                }
                if (fcov > 0) {
                    int ri = (r16 <= f_lo) ? 0 : (int)(((uint32_t)(r16 - f_lo) * inv_flen) >> 12);
                    if (ri > 15) { ri = 15; }
                    const unsigned fa = ((unsigned)fcov * f_alpha_s) >> 8;
                    if (fa > 0u) {
                        row[x] = g3_add565(row[x], ramp[ri], fa, swap_bytes);
                    }
                }
            }
        }
    }
}

/* Aetherium Layer C: 3 Additive Iridescent Harmonic Wave Ribbons.
 * Tight per-span radial skipping + packed 16-B AoS LUT + 6-cycle atan2_12_512! */
static G3_NOINLINE void g3_aeth_waves_rows(uint16_t *dst, int y0, int y1,
                                           bool swap_bytes, const g3_aeth_cache_t *c,
                                           int strength)
{
    const int r_in = c->r_wave_min;
    const int r_out = c->r_wave_max;
    const int rmin2_sq = (2 * r_in) * (2 * r_in);
    const int rmax2_sq = (2 * r_out) * (2 * r_out);
    int ry0 = 232 - r_out;
    int ry1 = 233 + r_out;
    if (ry0 < y0) { ry0 = y0; }
    if (ry0 < 0)  { ry0 = 0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry1 >= G3_H) { ry1 = G3_H - 1; }
    if (ry0 > ry1) {
        return;
    }

    const unsigned str = (unsigned)strength;
    const g3_aeth_ang_t *ang_t = c->ang;

    for (int y = ry0; y <= ry1; ++y) {
        g3_xspan_t spans[2];
        const int nsp = g3_annulus_row_spans(y, r_in, r_out, spans);
        if (nsp == 0) {
            continue;
        }
        const int dy2 = 2 * y - G3_CY2;
        const int dy2_sq = dy2 * dy2;
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;

        for (int s = 0; s < nsp; ++s) {
            int xlo = spans[s].lo;
            int xhi = spans[s].hi;

            /* Local radial span tightening on left/right spans (~2x fewer pixels!) */
            if (nsp == 2) {
                const uint32_t a_s = g3_atan2_12_512_ring(dy2, 2 * xlo - G3_CX2);
                const uint32_t a_e = g3_atan2_12_512_ring(dy2, 2 * xhi - G3_CX2);
                const uint32_t diff = (a_e - a_s) & 511u;
                const uint32_t a0 = (diff > 256u) ? a_e : a_s;
                const uint32_t cnt = (diff > 256u) ? (512u - diff) : diff;
                if (cnt <= 40u) {
                    int32_t l_lo = 220 * 16;
                    int32_t l_hi = 160 * 16;
                    for (uint32_t k = 0u; k <= cnt + 4u; ++k) {
                        const g3_aeth_ang_t *e = &ang_t[(a0 - 2u + k) & 511u];
                        if (e->r_lo < l_lo) { l_lo = e->r_lo; }
                        if (e->r_hi > l_hi) { l_hi = e->r_hi; }
                    }
                    const int rl = g3_clamp((l_lo >> 4) - 1, r_in, r_out);
                    const int rh = g3_clamp((l_hi >> 4) + 2, r_in, r_out);
                    g3_xspan_t ts[2];
                    if (g3_annulus_row_spans(y, rl, rh, ts) == 2) {
                        xlo = ts[s].lo;
                        xhi = ts[s].hi;
                    }
                }
            }

            int dx2 = 2 * xlo - G3_CX2;
            for (int x = xlo; x <= xhi; ++x, dx2 += 2) {
                const int r2_sq = dx2 * dx2 + dy2_sq;
                if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                    continue;
                }
                const int32_t d = ((r2_sq - 144400) * 345) >> 15;
                const int32_t r16 = 190 * 16 + d - ((d * d * 43) >> 18);
                const uint32_t ang12 = g3_atan2_12_q16_ring(dy2, dx2);
                const uint32_t a512 = ang12 >> 7;
                const g3_aeth_ang_t *e = &ang_t[a512];
                const g3_aeth_ang_t *en = &ang_t[(a512 + 1u) & 511u];
                const int32_t rlo_b = e->r_lo < en->r_lo ? e->r_lo : en->r_lo;
                const int32_t rhi_b = e->r_hi > en->r_hi ? e->r_hi : en->r_hi;
                if (r16 < rlo_b || r16 > rhi_b) {
                    continue;
                }

                const int32_t frac = (int32_t)((ang12 >> 3) & 15u);
                uint32_t ar = 0u, ag = 0u, ab = 0u;
                const int32_t rw0 = e->rw[0] + (((en->rw[0] - e->rw[0]) * frac) >> 4);
                const uint32_t dr0 = (uint32_t)g3_abs32(r16 - rw0);
                if (dr0 < 82u) {
                    const uint32_t a0 = k_g3_wave_col0[dr0];
                    const uint16_t c0 = e->wc[0];
                    ar += (uint32_t)(c0 >> 11) * a0;
                    ag += (uint32_t)((c0 >> 5) & 63u) * a0;
                    ab += (uint32_t)(c0 & 31u) * a0;
                    if (dr0 < 8u) {
                        const uint32_t wb = k_g3_wave_wht0[dr0];
                        ar += 31u * wb; ag += 63u * wb; ab += 31u * wb;
                    }
                }
                const int32_t rw1 = e->rw[1] + (((en->rw[1] - e->rw[1]) * frac) >> 4);
                const uint32_t dr1 = (uint32_t)g3_abs32(r16 - rw1);
                if (dr1 < 82u) {
                    const uint32_t a1 = k_g3_wave_col1[dr1];
                    const uint16_t c1 = e->wc[1];
                    ar += (uint32_t)(c1 >> 11) * a1;
                    ag += (uint32_t)((c1 >> 5) & 63u) * a1;
                    ab += (uint32_t)(c1 & 31u) * a1;
                    if (dr1 < 8u) {
                        const uint32_t wb = k_g3_wave_wht1[dr1];
                        ar += 31u * wb; ag += 63u * wb; ab += 31u * wb;
                    }
                }
                const int32_t rw2 = e->rw[2] + (((en->rw[2] - e->rw[2]) * frac) >> 4);
                const uint32_t dr2 = (uint32_t)g3_abs32(r16 - rw2);
                if (dr2 < 82u) {
                    const uint32_t a2 = k_g3_wave_col1[dr2];
                    const uint16_t c2 = e->wc[2];
                    ar += (uint32_t)(c2 >> 11) * a2;
                    ag += (uint32_t)((c2 >> 5) & 63u) * a2;
                    ab += (uint32_t)(c2 & 31u) * a2;
                    if (dr2 < 8u) {
                        const uint32_t wb = k_g3_wave_wht1[dr2];
                        ar += 31u * wb; ag += 63u * wb; ab += 31u * wb;
                    }
                }
                if ((ar | ag | ab) != 0u) {
                    row[x] = g3_acc_flush_str(row[x], ar, ag, ab, str, swap_bytes);
                }
            }
        }
    }
}

/* Aetherium Pass 3: Lower-Center Crystalline Vocal Waveform Ribbon. */
static G3_NOINLINE void g3_aeth_ribbon_rows(uint16_t *dst, int y0, int y1,
                                            bool swap_bytes, const g3_aeth_cache_t *c,
                                            int strength)
{
    int ry0 = JR_G3_AETH_RIBBON_Y0;
    int ry1 = JR_G3_AETH_RIBBON_Y1;
    if (ry0 < y0) { ry0 = y0; }
    if (ry0 < 0)  { ry0 = 0; }
    if (ry1 >= y1) { ry1 = y1 - 1; }
    if (ry1 >= G3_H) { ry1 = G3_H - 1; }
    if (ry0 > ry1) {
        return;
    }

    const uint16_t col_halo = c->col_rib_halo;
    const uint16_t col_sec  = c->col_rib_sec;
    const uint16_t col_core = c->col_rib_core;

    for (int y = ry0; y <= ry1; ++y) {
        const int32_t ady16 = g3_abs32(y * 16 + 8 - G3_RIBBON_CY16);
        uint16_t *row = dst + (size_t)(y - y0) * G3_W;

        for (int x = JR_G3_AETH_RIBBON_X0; x <= JR_G3_AETH_RIBBON_X1; ++x) {
            const int idx = x - JR_G3_AETH_RIBBON_X0;
            const unsigned ws = ((unsigned)c->rib_win[idx] * (unsigned)strength) >> 8;
            if (ws == 0u) {
                continue;
            }
            const int32_t h1 = (int32_t)c->rib_h1[idx] * 4;
            const int32_t h2 = (int32_t)c->rib_h2[idx] * 4;
            const int32_t d1 = g3_abs32(ady16 - h1);
            const int32_t d2 = g3_abs32(ady16 - h2);

            if (ady16 >= h1 && ady16 >= 16 && d1 >= 64 && d2 >= 44) {
                continue;
            }

            uint32_t ar = 0u, ag = 0u, ab = 0u;
            unsigned halo_a = 0u;

            if (ady16 < h1 && h1 > 8) {
                const uint32_t it = ((uint32_t)(h1 - ady16) * (uint32_t)c->rib_inv_h1[idx]) >> 8;
                halo_a += (unsigned)((it * 38u * ws) >> 16);
            }
            if (ady16 < 16) {
                halo_a += (unsigned)((((uint32_t)(16 - ady16) * 44u) >> 4) * ws >> 8);
            }
            if (d1 < 64) {
                const int t1 = ((64 - d1) * 255) >> 6;
                halo_a += g3_quad_alpha(t1, 165u, ws);
                if (d1 < 20) {
                    const unsigned core1 = (unsigned)((((uint32_t)(20 - d1) * 3008u) >> 8) * ws >> 8);
                    g3_acc_add_q8(&ar, &ag, &ab, col_core, core1);
                }
            }
            if (halo_a > 0u) {
                g3_acc_add_q8(&ar, &ag, &ab, col_halo, halo_a);
            }
            if (d2 < 44) {
                const int t2 = ((44 - d2) * 1484) >> 8;
                const unsigned a2 = g3_quad_alpha(t2, 130u, ws);
                g3_acc_add_q8(&ar, &ag, &ab, col_sec, a2);
            }

            if ((ar | ag | ab) != 0u) {
                row[x] = g3_acc_flush_str(row[x], ar, ag, ab, 255u, swap_bytes);
            }
        }
    }
}

void jr_g3_aetherium_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                          uint32_t now_ms, jr_g3_aeth_mode_t mode,
                          const uint8_t bands[16], uint8_t amp,
                          int strength)
{
    if (dst == NULL || nrows <= 0 || strength <= 0) {
        return;
    }
    if (strength > 255) {
        strength = 255;
    }
    const int y1 = y0 + nrows;
    if (y1 <= 0 || y0 >= G3_H) {
        return;
    }

    g3_aeth_cache_t *c_mut = s_aeth_cache_get();
    if (c_mut == NULL) {
        return;
    }
    g3_aeth_prepare(c_mut, now_ms, mode, bands, amp);
    const g3_aeth_cache_t *c = c_mut;

    g3_aeth_ticks_rows(dst, y0, y1, swap_bytes, strength);
    g3_aeth_filaments_rows(dst, y0, y1, swap_bytes, c, strength);
    g3_aeth_waves_rows(dst, y0, y1, swap_bytes, c, strength);

    /* 4 Glowing Cyan Cardinal Jewels at 12/3/6/9 o'clock (r = 223) */
    {
        static const uint32_t k_card_q16[4] = {
            GLASS_Q16_12_OCLOCK, 0u, 16384u, 32768u
        };
        const uint8_t halo_a = (uint8_t)((g3_clamp(175 + ((int)amp >> 2), 0, 255) * strength) >> 8);
        const uint8_t core_a = (uint8_t)strength;
        for (int i = 0; i < 4; ++i) {
            const int32_t cx16 = G3_CX16 + ((223 * 16 * glass_cos_q15(k_card_q16[i])) >> 15);
            const int32_t cy16 = G3_CY16 + ((223 * 16 * glass_sin_q15(k_card_q16[i])) >> 15);
            g3_glow_orb_annulus(dst, y0, y1, swap_bytes, cx16, cy16, 6,
                                0x06FFu, halo_a, 214, JR_G3_AETH_RING_R_MAX);
            g3_glow_orb_annulus(dst, y0, y1, swap_bytes, cx16, cy16, 2,
                                0xFFFFu, core_a, 214, JR_G3_AETH_RING_R_MAX);
        }
    }

    g3_aeth_ribbon_rows(dst, y0, y1, swap_bytes, c, strength);
}

/* ==========================================================================
 * ROTARY BEZEL SCRUB OVERLAY (Luminous Rotary Dial Arc + Jewel + Tick Scale)
 * ========================================================================== */

void jr_g3_bezel_scrub_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                            uint32_t now_ms, uint8_t value_pct,
                            jr_g3_scrub_kind_t kind, int strength)
{
    if (dst == NULL || nrows <= 0 || strength <= 0) {
        return;
    }
    if (strength > 255) {
        strength = 255;
    }
    const int y1 = y0 + nrows;
    if (y1 <= 0 || y0 >= G3_H) {
        return;
    }

    const uint32_t pct = value_pct > 100u ? 100u : (uint32_t)value_pct;
    const uint32_t scrub_a0 = 24576u;
    const uint32_t scrub_sweep = 49152u;
    const uint32_t fill_sweep = (scrub_sweep * pct) / 100u;
    const uint32_t inv_fill_q16 = (fill_sweep > 0u) ? ((255u << 16) / fill_sweep) : 0u;

    uint16_t col_deep, col_core, col_hot, col_glow, col_tick_lit;
    switch (kind) {
    case JR_G3_SCRUB_BRIGHTNESS:
        col_deep = 0x035Au; col_core = 0x37FFu; col_hot = 0xFFFFu;
        col_glow = 0x06FFu; col_tick_lit = 0x5FFFu;
        break;
    case JR_G3_SCRUB_TIMER:
        col_deep = 0x5178u; col_core = 0xA33Fu; col_hot = 0xF7FFu;
        col_glow = 0x927Fu; col_tick_lit = 0xC47Fu;
        break;
    case JR_G3_SCRUB_VOLUME:
    default:
        col_deep = 0xB300u; col_core = 0xFC80u; col_hot = 0xFFF2u;
        col_glow = 0xEA80u; col_tick_lit = 0xFDA4u;
        break;
    }

    const int32_t r_mid16  = 219 * 16;
    const int32_t track_hw = 80;
    const int32_t fill_hw  = 64;

    int32_t tcap0_x, tcap0_y, tcap1_x, tcap1_y;
    g3_capsule_compute_caps(r_mid16, scrub_a0, scrub_sweep,
                            &tcap0_x, &tcap0_y, &tcap1_x, &tcap1_y);
    int32_t fcap0_x, fcap0_y, fcap1_x, fcap1_y;
    g3_capsule_compute_caps(r_mid16, scrub_a0, fill_sweep,
                            &fcap0_x, &fcap0_y, &fcap1_x, &fcap1_y);

    const int32_t jewel_x16 = fcap1_x;
    const int32_t jewel_y16 = fcap1_y;
    const int pulse = (glass_sin_q15((now_ms * 32u) & 0xFFFFu) + 32768) >> 8;

    /* 1. 41-Tick Precision Inner Scale via direct radial rays (ZERO atan2!) */
    for (int k = 0; k <= 40; ++k) {
        const uint32_t ang = (scrub_a0 + (uint32_t)((k * (int32_t)scrub_sweep) / 40)) & 0xFFFFu;
        const int32_t ux = glass_cos_q15(ang);
        const int32_t uy = glass_sin_q15(ang);
        const int yc = 233 + (int)((208 * uy) >> 15);
        if (yc + 10 < y0 || yc - 10 >= y1) {
            continue;
        }
        const bool major = ((k & 3) == 0);
        const bool lit = ((uint32_t)k * 5u <= pct * 2u);
        const uint16_t tcol = lit ? col_tick_lit : 0x39EBu;
        const unsigned ta = (unsigned)(((lit ? (major ? 235 : 175) : (major ? 145 : 95)) * strength) >> 8);
        g3_radial_tick_rows(dst, y0, y1, swap_bytes, ux, uy,
                            (major ? 205 : 207) * 16, 211 * 16,
                            major ? 14 : 9, 0,
                            tcol, tcol, 0u, ta, 0u,
                            JR_G3_SCRUB_R_MIN, 213);
    }

    /* 2. Recessed Rotary Track & Luminous Dial Arc strictly in r in [208, 231] */
    {
        const int rmin2_sq = (2 * 208) * (2 * 208);
        const int rmax2_sq = (2 * JR_G3_SCRUB_R_MAX) * (2 * JR_G3_SCRUB_R_MAX);
        int ry0 = 232 - JR_G3_SCRUB_R_MAX;
        int ry1 = 233 + JR_G3_SCRUB_R_MAX;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        const unsigned track_str = (unsigned)((218 * strength) >> 8);
        const unsigned rim_str   = (unsigned)((155 * strength) >> 8);
        const unsigned fill_str  = (unsigned)((245 * strength) >> 8);
        const unsigned glow_str  = (unsigned)((125 * strength) >> 8);

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, 208, JR_G3_SCRUB_R_MAX, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t py16 = y * 16 + 8;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const uint32_t ang = g3_atan2_q16_fast(dy2, dx2);
                    const uint32_t rel = (ang - scrub_a0) & 0xFFFFu;
                    if (rel > scrub_sweep + 1800u && rel < 65536u - 1800u) {
                        continue;
                    }
                    const int32_t d = ((r2_sq - 191844) * 299) >> 15;
                    const int32_t r16 = 219 * 16 + d - ((d * d * 37) >> 18);
                    const int32_t r_off = r16 - r_mid16;

                    const int32_t d_tr = (rel <= scrub_sweep)
                        ? g3_abs32(r_off)
                        : g3_arc_spine_dist16(x * 16 + 8, py16, r16, ang, r_mid16,
                                              scrub_a0, scrub_sweep,
                                              tcap0_x, tcap0_y, tcap1_x, tcap1_y);
                    if (d_tr >= track_hw + 96) {
                        continue;
                    }

                    uint16_t pix = g3_to_nat(row[x], swap_bytes);

                    if (d_tr < track_hw + 8) {
                        unsigned ta = track_str;
                        if (d_tr > track_hw - 8) {
                            const int tcov = g3_clamp(((track_hw + 8 - d_tr) * 255) >> 4, 0, 255);
                            ta = (unsigned)(((unsigned)tcov * track_str) >> 8);
                        }
                        if (ta > 0u) {
                            pix = g3_blend565_nat(pix, 0x0842u, ta);
                        }
                        const int32_t d_rim = g3_abs32(d_tr - (track_hw - 6));
                        if (d_rim < 12) {
                            const unsigned rcov = (unsigned)(((12 - d_rim) * 5440) >> 8);
                            pix = g3_blend565_nat(pix, 0x31ACu, (rcov * rim_str) >> 8);
                        }
                    }

                    if (pct > 0u) {
                        const int32_t d_fl = (rel <= fill_sweep)
                            ? g3_abs32(r_off)
                            : g3_arc_spine_dist16(x * 16 + 8, py16, r16, ang, r_mid16,
                                                  scrub_a0, fill_sweep,
                                                  fcap0_x, fcap0_y, fcap1_x, fcap1_y);

                        if (d_fl < fill_hw + 96 && d_fl >= fill_hw - 8) {
                            const int32_t gd = d_fl > fill_hw ? (d_fl - fill_hw) : 0;
                            const int gt = ((96 - gd) * 680) >> 8;
                            const unsigned ga = (unsigned)((((uint32_t)gt * (uint32_t)gt) >> 8) * glow_str >> 8);
                            if (ga > 0u) {
                                pix = g3_add565_nat(pix, col_glow, ga);
                            }
                        }

                        if (d_fl < fill_hw + 8) {
                            int fcov = 255;
                            if (d_fl > fill_hw - 8) {
                                fcov = g3_clamp(((fill_hw + 8 - d_fl) * 255) >> 4, 0, 255);
                            }
                            if (fcov > 0) {
                                uint32_t prog;
                                if (rel > 57344u) {
                                    prog = 0u;
                                } else if (rel <= fill_sweep && fill_sweep > 0u) {
                                    prog = (rel * inv_fill_q16) >> 16;
                                    if (prog > 255u) { prog = 255u; }
                                } else {
                                    prog = 255u;
                                }
                                uint16_t col = g3_lerp565(col_deep, col_core, (int)prog);
                                if (prog > 190u) {
                                    col = g3_lerp565(col, col_hot, (int)((prog - 190u) * 3u));
                                }
                                const int32_t d_ridge = g3_abs32(r_off + 16);
                                if (d_ridge < 28) {
                                    col = g3_lerp565(col, col_hot, ((28 - d_ridge) * 1508) >> 8);
                                }
                                pix = g3_blend565_nat(pix, col, (unsigned)(((unsigned)fcov * fill_str) >> 8));
                            }
                        }
                    }

                    row[x] = g3_to_nat(pix, swap_bytes);
                }
            }
        }
    }

    /* 3. Glowing Precision Faceted Jewel Indicator at r=219 */
    {
        const uint8_t halo_a = (uint8_t)(((175 + ((pulse * 55) >> 8)) * strength) >> 8);
        g3_glow_orb_annulus(dst, y0, y1, swap_bytes,
                            jewel_x16, jewel_y16, 11,
                            col_glow, halo_a,
                            JR_G3_SCRUB_R_MIN, JR_G3_SCRUB_R_MAX);

        const int jx_i = (int)(jewel_x16 >> 4);
        const int jy_i = (int)(jewel_y16 >> 4);
        int ry0 = jy_i - 8;
        int ry1 = jy_i + 8;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }
        int rx0 = jx_i - 8;
        int rx1 = jx_i + 8;
        if (rx0 < 0) { rx0 = 0; }
        if (rx1 >= G3_W) { rx1 = G3_W - 1; }

        const int rmin2_sq = (2 * JR_G3_SCRUB_R_MIN) * (2 * JR_G3_SCRUB_R_MIN);
        const int rmax2_sq = (2 * JR_G3_SCRUB_R_MAX) * (2 * JR_G3_SCRUB_R_MAX);
        const int32_t outer_r16 = 6 * 16 + 8;
        const int32_t core_r16  = 4 * 16 + 8;

        for (int y = ry0; y <= ry1; ++y) {
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t jy16 = y * 16 + 8 - jewel_y16;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int x = rx0; x <= rx1; ++x) {
                const int dx2 = 2 * x - G3_CX2;
                const int r2_sq = dx2 * dx2 + dy2_sq;
                if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                    continue;
                }
                const int32_t jx16 = x * 16 + 8 - jewel_x16;
                const uint32_t d2 = (uint32_t)(jx16 * jx16 + jy16 * jy16);
                if (d2 >= (uint32_t)((outer_r16 + 12) * (outer_r16 + 12))) {
                    continue;
                }
                const int32_t d16 = (int32_t)g3_isqrt_fast(d2);
                uint16_t pix = g3_to_nat(row[x], swap_bytes);

                if (d16 < outer_r16 + 8) {
                    int bcov = (d16 > outer_r16 - 8) ? g3_clamp(((outer_r16 + 8 - d16) * 255) >> 4, 0, 255) : 255;
                    const uint16_t collar_col = (d16 > core_r16 + 8) ? 0xCE79u : 0x0842u;
                    pix = g3_blend565_nat(pix, collar_col, (unsigned)((bcov * 240 * strength) >> 16));
                }
                if (d16 < core_r16 + 8) {
                    int ccov = (d16 > core_r16 - 8) ? g3_clamp(((core_r16 + 8 - d16) * 255) >> 4, 0, 255) : 255;
                    const int ct = g3_clamp(((core_r16 - d16) * 907) >> 8, 0, 255);
                    uint16_t jcol = g3_lerp565(col_core, 0xFFFFu, (ct * ct) >> 8);
                    pix = g3_blend565_nat(pix, jcol, (unsigned)((ccov * strength) >> 8));
                }

                row[x] = g3_to_nat(pix, swap_bytes);
            }
        }
    }
}

/* ==========================================================================
 * GLASS 3.5 NEW RENDERER 1: SILKY PROCEDURAL COLD-BOOT IGNITION SPLASH
 * ========================================================================== */

static G3_NOINLINE void g3_boot_pulse_rings(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                            uint32_t now_ms, uint16_t pulse_col, int strength)
{
    const int rmin2_sq = (2 * JR_G3_BOOT_PULSE_R_MIN) * (2 * JR_G3_BOOT_PULSE_R_MIN);
    const int rmax2_sq = (2 * JR_G3_BOOT_PULSE_R_MAX) * (2 * JR_G3_BOOT_PULSE_R_MAX);

    int32_t ring_r16[3];
    int32_t ring_hw[3];
    int     ring_a[3];

    for (int k = 0; k < 2; ++k) {
        const uint32_t ph = (now_ms + (uint32_t)k * 1200u) % 2400u;
        ring_r16[k] = 114 * 16 + (int32_t)((ph * (36u * 16u)) / 2400u);
        const int ev = (glass_sin_q15((ph * 32768u) / 2400u) * 38) >> 15;
        ring_a[k] = g3_clamp(ev, 0, 38);
        ring_hw[k] = 28;
    }
    ring_r16[2] = 132 * 16;
    ring_a[2] = 18;
    ring_hw[2] = 14;

    for (int k = 0; k < 3; ++k) {
        const int peak_a = (ring_a[k] * strength) >> 8;
        if (peak_a <= 0) {
            continue;
        }
        const uint32_t rc16 = (uint32_t)ring_r16[k];
        const int32_t rc2_sq = (int32_t)((rc16 * rc16) >> 6);
        const int32_t inv_q15 = (int32_t)((1u << 20) / rc16);
        const int32_t hw16 = ring_hw[k];
        const uint32_t inv_hw = (255u << 8) / (uint32_t)hw16;
        const int r_c = (int)(rc16 >> 4);
        const int b_in = g3_clamp(r_c - 2, JR_G3_BOOT_PULSE_R_MIN, JR_G3_BOOT_PULSE_R_MAX);
        const int b_out = g3_clamp(r_c + 3, JR_G3_BOOT_PULSE_R_MIN, JR_G3_BOOT_PULSE_R_MAX);

        int ry0 = 232 - b_out;
        int ry1 = 233 + b_out;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, b_in, b_out, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t dr = g3_abs32(((r2_sq - rc2_sq) * inv_q15) >> 15);
                    if (dr >= hw16) {
                        continue;
                    }
                    const uint32_t cov = ((uint32_t)(hw16 - dr) * inv_hw) >> 8;
                    const unsigned a = (unsigned)((cov * (uint32_t)peak_a) >> 8);
                    if (a > 0u) {
                        row[x] = g3_add565(row[x], pulse_col, a, swap_bytes);
                    }
                }
            }
        }
    }
}

void jr_g3_boot_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                     const jr_g3_boot_t *env)
{
    if (dst == NULL || nrows <= 0 || env == NULL || env->strength == 0u) {
        return;
    }
    const int y1 = y0 + nrows;
    if (y1 <= 0 || y0 >= G3_H) {
        return;
    }

    const int strength = (int)env->strength;
    const uint32_t pct = env->progress_pct > 100u ? 100u : (uint32_t)env->progress_pct;
    const uint32_t now_ms = env->now_ms;

    uint16_t col_deep, col_core, col_hot, col_glow, col_tick_lit, col_pulse;
    switch (env->stage) {
    case 1u:
        col_deep = 0x035Au; col_core = 0x07FDu; col_hot = 0xEFFFu;
        col_glow = 0x05DFu; col_tick_lit = 0x3FFFu; col_pulse = 0x05BFu;
        break;
    case 2u:
        col_deep = 0x127Cu; col_core = 0x45FFu; col_hot = 0xF7FFu;
        col_glow = 0x34DFu; col_tick_lit = 0x76FFu; col_pulse = 0x34DFu;
        break;
    case 3u:
        col_deep = 0x04D9u; col_core = 0x5FFFu; col_hot = 0xFFFFu;
        col_glow = 0x27FDu; col_tick_lit = 0xBFFFu; col_pulse = 0x17DCu;
        break;
    case 0u:
    default:
        col_deep = 0x0256u; col_core = 0x06FFu; col_hot = 0xDFFFu;
        col_glow = 0x04DFu; col_tick_lit = 0x2EFFu; col_pulse = 0x04BFu;
        break;
    }

    /* 1. Subtle concentric optical pulse rings strictly in r in [112, 152] */
    g3_boot_pulse_rings(dst, y0, y1, swap_bytes, now_ms, col_pulse, strength);

    /* 2. Sweeping Calibration Ring in [188, 209] (ZERO atan2 via half-plane + dot!) */
    const bool idle_orbit = (pct == 0u);
    const uint32_t sweep_q16 = idle_orbit ? 9830u : ((pct * 65536u) / 100u);
    const uint32_t arc_start12 = idle_orbit ? ((now_ms * 48u) & 0xFFFFu) : 0u;
    const uint32_t start_q16 = (arc_start12 + GLASS_Q16_12_OCLOCK) & 0xFFFFu;
    const uint32_t head12 = (arc_start12 + sweep_q16) & 0xFFFFu;
    const uint32_t head_q16 = (head12 + GLASS_Q16_12_OCLOCK) & 0xFFFFu;
    const uint32_t shim_q16 = (((now_ms * 56u) & 0xFFFFu) + GLASS_Q16_12_OCLOCK) & 0xFFFFu;

    const int32_t sc0 = glass_cos_q15(start_q16), ss0 = glass_sin_q15(start_q16);
    const int32_t hc  = glass_cos_q15(head_q16),  hs  = glass_sin_q15(head_q16);
    const int32_t shc = glass_cos_q15(shim_q16),  shs = glass_sin_q15(shim_q16);
    const bool reflex = (sweep_q16 > 32768u);

    {
        const int rmin2_sq = (2 * JR_G3_BOOT_RING_R_MIN) * (2 * JR_G3_BOOT_RING_R_MIN);
        const int rmax2_sq = (2 * 209) * (2 * 209);
        int ry0 = 232 - 209;
        int ry1 = 233 + 209;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        const int32_t r_mid16 = 199 * 16;
        const int32_t ring_hw = 104;
        const unsigned track_str = (unsigned)((185 * strength) >> 8);
        const unsigned glow_str  = (unsigned)((128 * strength) >> 8);
        const unsigned fill_str  = (unsigned)((245 * strength) >> 8);

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, JR_G3_BOOT_RING_R_MIN, 209, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t h0_y = dy2 * sc0;
            const int32_t h1_y = -dy2 * hc;
            const int32_t hd_y = dy2 * hs;
            const int32_t sd_y = dy2 * shs;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                int xlo = spans[s].lo;
                int xhi = spans[s].hi;
                int dx2 = 2 * xlo - G3_CX2;
                int32_t h0 = h0_y - dx2 * ss0;
                int32_t h1 = h1_y + dx2 * hs;
                for (int x = xlo; x <= xhi; ++x, dx2 += 2, h0 -= 2 * ss0, h1 += 2 * hs) {
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t d = ((r2_sq - 158404) * 329) >> 15;
                    const int32_t r16 = 199 * 16 + d - ((d * d * 41) >> 18);
                    const int32_t dr = g3_abs32(r16 - r_mid16);
                    if (dr >= 156) {
                        continue;
                    }

                    uint16_t pix = g3_to_nat(row[x], swap_bytes);

                    if (dr < ring_hw + 12) {
                        unsigned ta = track_str;
                        if (dr > ring_hw - 4) {
                            ta = (unsigned)(((unsigned)g3_clamp(((ring_hw + 12 - dr) * 255) >> 4, 0, 255) * track_str) >> 8);
                        }
                        pix = g3_blend565_nat(pix, 0x0842u, ta);
                        const int32_t d_rim = g3_abs32(dr - ring_hw);
                        if (d_rim < 10) {
                            pix = g3_add565_nat(pix, 0x1A6Bu, (unsigned)(((10 - d_rim) * 1400 * strength) >> 16));
                        }
                    }

                    const bool in_arc = (pct >= 100u) ? true : (reflex ? (h0 >= 0 || h1 >= 0) : (h0 >= 0 && h1 >= 0));
                    if (in_arc) {
                        if (dr >= 68) {
                            const int32_t gd = dr > 76 ? (dr - 76) : 0;
                            if (gd < 80) {
                                const int gt = ((80 - gd) * 816) >> 8;
                                const unsigned ga = (unsigned)((((uint32_t)gt * (uint32_t)gt) >> 8) * glow_str >> 8);
                                if (ga > 0u) {
                                    pix = g3_add565_nat(pix, col_glow, ga);
                                }
                            }
                        }
                        if (dr < 84) {
                            int fcov = (dr > 68) ? g3_clamp(((84 - dr) * 255) >> 4, 0, 255) : 255;
                            /* Dot-product comet tail & shimmer (ZERO atan2!) */
                            const int32_t dot_h = (dx2 * hc + hd_y) >> 15; /* -398..+398 */
                            const int32_t dot_s = (dx2 * shc + sd_y) >> 15;
                            int hot_t = (dot_h > 310) ? ((dot_h - 310) * 3) : 0;
                            if (dot_s > 340) {
                                hot_t += (dot_s - 340) * 2;
                            }
                            uint16_t col = g3_lerp565(col_deep, col_core, g3_clamp(((84 - dr) * 777) >> 8, 0, 255));
                            if (hot_t > 0) {
                                col = g3_lerp565(col, col_hot, hot_t > 255 ? 255 : hot_t);
                            }
                            unsigned fa = (unsigned)(((unsigned)fcov * fill_str) >> 8);
                            if (idle_orbit) {
                                const int tf = g3_clamp(40 + ((dot_h - 210) * 215) / 188, 40, 255);
                                fa = (fa * (unsigned)tf) >> 8;
                            }
                            pix = g3_blend565_nat(pix, col, fa);
                        }
                    }

                    row[x] = g3_to_nat(pix, swap_bytes);
                }
            }
        }
    }

    /* 3. 60 Progressive Radial Ignition Ticks (r in [210, 222]) via direct rays */
    {
        const uint32_t cur_x60 = pct * 60u;
        for (int k = 0; k < 60; ++k) {
            int32_t ux, uy;
            g3_unit_vec_q15(k * 3, 45, k_g3_cos180_q15, &ux, &uy);
            const int yc = 233 + (int)((216 * uy) >> 15);
            if (yc + 12 < y0 || yc - 12 >= y1) {
                continue;
            }
            const bool card = ((k % 15) == 0);
            const bool major = card || ((k % 5) == 0);
            const bool lit = !idle_orbit && ((uint32_t)k * 100u <= cur_x60);
            const uint16_t tcol = lit ? (card ? col_hot : col_tick_lit) : 0x2167u;
            const unsigned ta = (unsigned)(((lit ? (card ? 250 : 215) : (major ? 125 : 85)) * strength) >> 8);
            const unsigned ga = lit ? (unsigned)((75 * strength) >> 8) : 0u;
            g3_radial_tick_rows(dst, y0, y1, swap_bytes, ux, uy,
                                (major ? 210 : 212) * 16, (major ? 222 : 220) * 16,
                                card ? 18 : (major ? 14 : 10), lit ? 24 : 0,
                                tcol, lit ? col_glow : tcol, 0u, ta, ga,
                                209, JR_G3_BOOT_RING_R_MAX);
        }
    }

    /* 4. Glowing Leading Comet Orb at arc head (r = 199) */
    {
        const int32_t hx16 = G3_CX16 + ((199 * 16 * hc) >> 15);
        const int32_t hy16 = G3_CY16 + ((199 * 16 * hs) >> 15);
        g3_glow_orb_annulus(dst, y0, y1, swap_bytes, hx16, hy16, 11,
                            col_glow, (uint8_t)((215 * strength) >> 8),
                            JR_G3_BOOT_RING_R_MIN, JR_G3_BOOT_RING_R_MAX);
        g3_glow_orb_annulus(dst, y0, y1, swap_bytes, hx16, hy16, 5,
                            0xFFFFu, (uint8_t)strength,
                            JR_G3_BOOT_RING_R_MIN, JR_G3_BOOT_RING_R_MAX);
    }
}

/* ==========================================================================
 * GLASS 3.5 NEW RENDERER 2: UNIFIED DESK MODE (ON AIR / FOCUS / BREATHE)
 * ========================================================================== */

static G3_NOINLINE void g3_draw_bottom_pips(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                            int pip_r_px, uint8_t done, uint8_t total,
                                            uint16_t col_lit, uint16_t col_glow,
                                            uint16_t col_rim, int strength,
                                            int clip_r_min, int clip_r_max)
{
    const int n = (total == 0u) ? 4 : g3_clamp((int)total, 1, 8);
    const int d_cnt = ((int)done > n) ? n : (int)done;
    const int32_t step_q16 = (n <= 5) ? 2184 : 1638;
    const int32_t span_q16 = step_q16 * (n - 1);
    const int32_t start_q16 = 16384 + (span_q16 >> 1);
    const int rmin2_sq = (2 * clip_r_min) * (2 * clip_r_min);
    const int rmax2_sq = (2 * clip_r_max) * (2 * clip_r_max);

    for (int i = 0; i < n; ++i) {
        const uint32_t ang = (uint32_t)(start_q16 - i * step_q16) & 0xFFFFu;
        const int32_t cx16 = G3_CX16 + ((pip_r_px * 16 * glass_cos_q15(ang)) >> 15);
        const int32_t cy16 = G3_CY16 + ((pip_r_px * 16 * glass_sin_q15(ang)) >> 15);
        const bool lit = (i < d_cnt);

        if (lit) {
            g3_glow_orb_annulus(dst, y0, y1, swap_bytes, cx16, cy16, 10,
                                col_glow, (uint8_t)((165 * strength) >> 8),
                                clip_r_min, clip_r_max);
        }

        const int cx_i = (int)(cx16 >> 4);
        const int cy_i = (int)(cy16 >> 4);
        int ry0 = cy_i - 7;
        int ry1 = cy_i + 7;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }
        if (ry0 > ry1) {
            continue;
        }
        int rx0 = cx_i - 7;
        int rx1 = cx_i + 7;
        if (rx0 < 0) { rx0 = 0; }
        if (rx1 >= G3_W) { rx1 = G3_W - 1; }

        const int32_t outer16 = 5 * 16;
        for (int y = ry0; y <= ry1; ++y) {
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t py16 = y * 16 + 8 - cy16;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int x = rx0; x <= rx1; ++x) {
                const int dx2 = 2 * x - G3_CX2;
                const int r2_sq = dx2 * dx2 + dy2_sq;
                if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                    continue;
                }
                const int32_t px16 = x * 16 + 8 - cx16;
                const uint32_t d2 = (uint32_t)(px16 * px16 + py16 * py16);
                if (d2 >= (uint32_t)((outer16 + 12) * (outer16 + 12))) {
                    continue;
                }
                const int32_t d16 = (int32_t)g3_isqrt_fast(d2);
                uint16_t pix = g3_to_nat(row[x], swap_bytes);

                if (lit) {
                    if (d16 < outer16 + 8) {
                        int cov = (d16 > outer16 - 8) ? g3_clamp(((outer16 + 8 - d16) * 255) >> 4, 0, 255) : 255;
                        const int ct = g3_clamp(((outer16 - d16) * 816) >> 8, 0, 255);
                        const uint16_t col = g3_lerp565(col_lit, 0xFFFFu, (ct * ct) >> 8);
                        pix = g3_blend565_nat(pix, col, (unsigned)((cov * strength) >> 8));
                    }
                } else if (d16 < outer16 + 8) {
                    int cov = (d16 > outer16 - 8) ? g3_clamp(((outer16 + 8 - d16) * 255) >> 4, 0, 255) : 255;
                    const int32_t drim = g3_abs32(d16 - (outer16 - 10));
                    if (drim < 14) {
                        const int rcov = ((14 - drim) * 4663) >> 8;
                        pix = g3_blend565_nat(pix, col_rim, (unsigned)((rcov * 185 * strength) >> 16));
                    } else if (d16 < outer16 - 10) {
                        pix = g3_blend565_nat(pix, 0x0842u, (unsigned)((cov * 165 * strength) >> 16));
                    }
                }
                row[x] = g3_to_nat(pix, swap_bytes);
            }
        }
    }
}

/* Precomputed 70-entry radial profile cache for ON_AIR beacon [197..231]. */
typedef struct {
    bool     valid;
    uint32_t now_ms;
    bool     warn;
    uint8_t  strength;
    uint16_t col_core;
    uint16_t col_ridge;
    uint16_t col_glow;
    uint16_t col_vig;
    uint8_t  vig_peak;
    uint16_t bcol[70];
    uint16_t bswp[70];
    uint8_t  ba[70];
    uint8_t  bloom_a[70];
} g3_on_air_cache_t;

G3_DEFINE_CACHE(g3_on_air_cache_t, s_on_air_cache)

static void g3_on_air_prepare(g3_on_air_cache_t *oc, uint32_t now_ms,
                              bool warn, uint8_t strength)
{
    if (oc->valid && oc->now_ms == now_ms && oc->warn == warn && oc->strength == strength) {
        return;
    }
    oc->valid = true;
    oc->now_ms = now_ms;
    oc->warn = warn;
    oc->strength = strength;

    const uint32_t ang_b = warn ? ((now_ms * 98u) & 0xFFFFu) : ((now_ms * 33u) & 0xFFFFu);
    const int breath = (glass_sin_q15(ang_b) + 32768) >> 8;

    const uint16_t col_deep  = warn ? g3_lerp565(0x9002u, 0xB260u, breath) : 0x9002u;
    const uint16_t col_core  = warn ? g3_lerp565(0xF928u, 0xFC80u, breath) : 0xF928u;
    const uint16_t col_ridge = warn ? g3_lerp565(0xFDD6u, 0xFFF0u, breath) : 0xFDD6u;
    const uint16_t col_glow  = warn ? g3_lerp565(0xD805u, 0xEA60u, breath) : 0xD805u;
    oc->col_core  = col_core;
    oc->col_ridge = col_ridge;
    oc->col_glow  = col_glow;
    oc->col_vig   = warn ? g3_lerp565(0x8002u, 0xA200u, breath) : 0x8002u;
    oc->vig_peak  = (uint8_t)(((16 + ((breath * 14) >> 8)) * (int)strength) >> 8);

    const unsigned beacon_a = (unsigned)(192 + ((breath * 63) >> 8));
    const unsigned bloom_pk = (unsigned)(105 + ((breath * 95) >> 8));

    for (int i = 0; i < 70; ++i) {
        const int32_t r16 = 197 * 16 + i * 8;
        const int32_t db = g3_abs32(r16 - 214 * 16);
        oc->bcol[i] = 0u;
        oc->bswp[i] = 0u;
        oc->ba[i] = 0u;
        oc->bloom_a[i] = 0u;

        if (db < 196) {
            int bcov = (db > 180) ? g3_clamp(((196 - db) * 255) >> 4, 0, 255) : 255;
            const int et = g3_clamp(((196 - db) * 333) >> 8, 0, 255);
            uint16_t bcol = g3_lerp565(col_deep, col_core, et);
            const int32_t dridge = g3_abs32((r16 - 214 * 16) + 44);
            if (dridge < 56) {
                bcol = g3_lerp565(bcol, col_ridge, ((56 - dridge) * 960) >> 8);
            }
            /* Pre-bake additive core glow into bcol -> 1 operation per beacon pixel! */
            bcol = g3_add565_nat(bcol, col_glow, bloom_pk >> 1);
            oc->bcol[i] = bcol;
            oc->bswp[i] = glass_bswap(bcol);
            oc->ba[i] = (uint8_t)(((unsigned)bcov * beacon_a * (unsigned)strength) >> 16);
        } else if (db < 276) {
            const int32_t gd = db - 168;
            const int gt = ((108 - gd) * 604) >> 8;
            oc->bloom_a[i] = (uint8_t)g3_quad_alpha(gt, bloom_pk, (unsigned)strength);
        }
    }
}

/* Mode 1: ON AIR — 700-nit pulsing crimson-coral studio recording beacon.
 * ZERO atan2 and ZERO isqrt across the entire frame! */
static G3_NOINLINE void g3_dmode_on_air_rows(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                             const jr_g3_desk_mode_t *env)
{
    const int strength = (int)env->strength;
    const uint32_t pct = env->pct > 100u ? 100u : (uint32_t)env->pct;

    g3_on_air_cache_t *oc_mut = s_on_air_cache_get();
    if (oc_mut == NULL) {
        return;
    }
    g3_on_air_prepare(oc_mut, env->now_ms, env->warn_pulse, env->strength);
    const g3_on_air_cache_t *oc = oc_mut;

    /* 1. Subtle red optical vignette strictly in r in [162, 185] */
    if (oc->vig_peak > 0u) {
        const int rmin2_sq = (2 * JR_G3_DMODE_ON_AIR_SAFE_R) * (2 * JR_G3_DMODE_ON_AIR_SAFE_R);
        const int rmax2_sq = (2 * 185) * (2 * 185);
        const uint32_t inv_span = (255u << 16) / (uint32_t)(rmax2_sq - rmin2_sq);
        const uint16_t col_vig = oc->col_vig;
        const uint32_t vp = oc->vig_peak;
        int ry0 = 232 - 185;
        int ry1 = 233 + 185;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, 162, 185, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const uint32_t t = ((uint32_t)(r2_sq - rmin2_sq) * inv_span) >> 16;
                    const unsigned a = (unsigned)((t * t * vp) >> 16);
                    if (a > 0u) {
                        row[x] = g3_add565(row[x], col_vig, a, swap_bytes);
                    }
                }
            }
        }
    }

    /* 2. Inner Call-Time Remaining Arc [186, 196] via half-plane test (ZERO atan2!) */
    if (pct == 0u) {
        g3_hairline_ring_rows(dst, y0, y1, swap_bytes, 191, 14, 0x7002u, 56, strength, 186, 196);
    } else {
        const int rmin2_sq = (2 * 186) * (2 * 186);
        const int rmax2_sq = (2 * 196) * (2 * 196);
        const uint32_t arc_sweep = (pct * 65536u) / 100u;
        const uint32_t head_q16 = (arc_sweep + GLASS_Q16_12_OCLOCK) & 0xFFFFu;
        const int32_t hc = glass_cos_q15(head_q16), hs = glass_sin_q15(head_q16);
        const bool reflex = (arc_sweep > 32768u);
        const unsigned trk_s = (unsigned)((210 * strength) >> 8);
        const unsigned fil_s = (unsigned)((235 * strength) >> 8);

        int ry0 = 232 - 196;
        int ry1 = 233 + 196;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, 186, 196, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t h1_y = -dy2 * hc;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                int xlo = spans[s].lo;
                int xhi = spans[s].hi;
                int dx2 = 2 * xlo - G3_CX2;
                int32_t h1 = h1_y + dx2 * hs;
                for (int x = xlo; x <= xhi; ++x, dx2 += 2, h1 += 2 * hs) {
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t da = g3_abs32(((r2_sq - 4 * 191 * 191) * 343) >> 15);
                    if (da >= 80) {
                        continue;
                    }
                    uint16_t pix = g3_to_nat(row[x], swap_bytes);
                    unsigned ta = (da > 64) ? (unsigned)(((unsigned)g3_clamp(((80 - da) * 255) >> 4, 0, 255) * trk_s) >> 8) : trk_s;
                    pix = g3_blend565_nat(pix, 0x2001u, ta);
                    const int32_t drim = g3_abs32(da - 68);
                    if (drim < 10) {
                        pix = g3_add565_nat(pix, 0x5802u, (unsigned)(((10 - drim) * 1400 * strength) >> 16));
                    }
                    const bool in_arc = (pct >= 100u) ? true : (reflex ? (dx2 >= 0 || h1 >= 0) : (dx2 >= 0 && h1 >= 0));
                    if (da < 52 && in_arc) {
                        unsigned fa = (da > 36) ? (unsigned)(((unsigned)g3_clamp(((52 - da) * 255) >> 4, 0, 255) * fil_s) >> 8) : fil_s;
                        const uint16_t acol = g3_lerp565(oc->col_core, oc->col_ridge, ((52 - da) * 800) >> 8);
                        pix = g3_blend565_nat(pix, acol, fa);
                    }
                    row[x] = g3_to_nat(pix, swap_bytes);
                }
            }
        }
    }

    /* 3. Outer 700-nit Crimson-Coral Beacon Ring [197, 231] — 1 op/pixel! */
    {
        const int rmin2_sq = (2 * 197) * (2 * 197);
        const int rmax2_sq = (2 * JR_G3_DMODE_ON_AIR_R_MAX) * (2 * JR_G3_DMODE_ON_AIR_R_MAX);
        const uint16_t col_glow = oc->col_glow;
        int ry0 = 232 - JR_G3_DMODE_ON_AIR_R_MAX;
        int ry1 = 233 + JR_G3_DMODE_ON_AIR_R_MAX;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, 197, JR_G3_DMODE_ON_AIR_R_MAX, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const bool card_y = (g3_abs32(dy2) < 5);
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t d = ((r2_sq - 183184) * 306) >> 15;
                    const int32_t r16 = 214 * 16 + d - ((d * d * 38) >> 18);
                    const int idx = g3_clamp((r16 - 197 * 16) >> 3, 0, 69);

                    const unsigned ba = oc->ba[idx];
                    if (ba > 0u) {
                        const bool notch = card_y || (g3_abs32(dx2) < 5);
                        if (!notch && ba >= 252u) {
                            row[x] = swap_bytes ? oc->bswp[idx] : oc->bcol[idx];
                        } else {
                            uint16_t bc = notch ? g3_lerp565(oc->bcol[idx], 0xFFFFu, 96) : oc->bcol[idx];
                            uint16_t pix = g3_to_nat(row[x], swap_bytes);
                            pix = g3_blend565_nat(pix, bc, ba);
                            row[x] = g3_to_nat(pix, swap_bytes);
                        }
                    } else {
                        const unsigned ga = oc->bloom_a[idx];
                        if (ga > 0u) {
                            row[x] = g3_add565(row[x], col_glow, ga, swap_bytes);
                        }
                    }
                }
            }
        }
    }

    if (pct > 0u && pct < 100u) {
        const uint32_t head_q16 = (((pct * 65536u) / 100u) + GLASS_Q16_12_OCLOCK) & 0xFFFFu;
        const int32_t hx16 = G3_CX16 + ((191 * 16 * glass_cos_q15(head_q16)) >> 15);
        const int32_t hy16 = G3_CY16 + ((191 * 16 * glass_sin_q15(head_q16)) >> 15);
        g3_glow_orb_annulus(dst, y0, y1, swap_bytes, hx16, hy16, 6,
                            oc->col_ridge, (uint8_t)((195 * strength) >> 8),
                            184, 198);
    }
}

/* Mode 2: FOCUS — Sapphire-violet deep-work perimeter halo [194, 231] +
 * electric-cyan leading tip jewel + curved bottom daily sprint pips at r=166. */
static G3_NOINLINE void g3_dmode_focus_rows(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                            const jr_g3_desk_mode_t *env)
{
    const int strength = (int)env->strength;
    const uint32_t pct = env->pct > 100u ? 100u : (uint32_t)env->pct;
    const uint32_t fill_sweep = (pct * 65536u) / 100u;
    const uint32_t head_q16 = (fill_sweep + GLASS_Q16_12_OCLOCK) & 0xFFFFu;
    const int32_t hc = glass_cos_q15(head_q16), hs = glass_sin_q15(head_q16);
    const bool reflex = (fill_sweep > 32768u);
    const uint32_t inv_fill512 = (pct > 0u) ? ((255u << 8) / ((pct * 512u + 50u) / 100u)) : 0u;

    const int breath = (glass_sin_q15((env->now_ms * 16u) & 0xFFFFu) + 32768) >> 8;
    const uint32_t shim_q16 = (((env->now_ms * 22u) & 0xFFFFu) + GLASS_Q16_12_OCLOCK) & 0xFFFFu;
    const int32_t shc = glass_cos_q15(shim_q16), shs = glass_sin_q15(shim_q16);

    const uint16_t col_deep  = 0x317Au;
    const uint16_t col_mid   = 0x633Fu;
    const uint16_t col_tip   = 0x06FFu;
    const uint16_t col_ridge = 0xCFFFu;
    const uint16_t col_glow  = 0x43BFu;

    {
        const int rmin2_sq = (2 * JR_G3_DMODE_FOCUS_RING_R_MIN) * (2 * JR_G3_DMODE_FOCUS_RING_R_MIN);
        const int rmax2_sq = (2 * JR_G3_DMODE_FOCUS_RING_R_MAX) * (2 * JR_G3_DMODE_FOCUS_RING_R_MAX);
        int ry0 = 232 - JR_G3_DMODE_FOCUS_RING_R_MAX;
        int ry1 = 233 + JR_G3_DMODE_FOCUS_RING_R_MAX;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        const unsigned bloom_str = (unsigned)(((95 + ((breath * 45) >> 8)) * strength) >> 8);
        const unsigned track_str = (unsigned)((215 * strength) >> 8);
        const unsigned fill_str  = (unsigned)((242 * strength) >> 8);

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, JR_G3_DMODE_FOCUS_RING_R_MIN,
                                                 JR_G3_DMODE_FOCUS_RING_R_MAX, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            const int32_t h1_y = -dy2 * hc;
            const int32_t sd_y = dy2 * shs;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                int xlo = spans[s].lo;
                int xhi = spans[s].hi;
                int dx2 = 2 * xlo - G3_CX2;
                int32_t h1 = h1_y + dx2 * hs;
                for (int x = xlo; x <= xhi; ++x, dx2 += 2, h1 += 2 * hs) {
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t d = ((r2_sq - 178084) * 311) >> 15;
                    const int32_t r_off = d - ((d * d * 39) >> 18);
                    const int32_t df = g3_abs32(r_off);
                    if (df >= 268) {
                        continue;
                    }

                    const bool in_arc = (pct > 0u) && ((pct >= 100u) ? true : (reflex ? (dx2 >= 0 || h1 >= 0) : (dx2 >= 0 && h1 >= 0)));
                    uint16_t pix = g3_to_nat(row[x], swap_bytes);

                    /* Fast path: solid filled interior skips track blend (merges directly!) */
                    if (in_arc && df < 152) {
                        const uint32_t a512 = g3_atan2_12_512_ring(dy2, dx2);
                        uint32_t prog = (pct >= 100u) ? (a512 >> 1) : ((a512 * inv_fill512) >> 8);
                        if (prog > 255u) { prog = 255u; }
                        uint16_t col = (prog < 160u)
                            ? g3_lerp565(col_deep, col_mid, (int)((prog * 408u) >> 8))
                            : g3_lerp565(col_mid, col_tip, (int)(((prog - 160u) * 687u) >> 8));
                        const int32_t dridge = g3_abs32(r_off + 42);
                        if (dridge < 52) {
                            col = g3_lerp565(col, col_ridge, ((52 - dridge) * 860) >> 8);
                        }
                        const int32_t dot_s = (dx2 * shc + sd_y) >> 15;
                        if (dot_s > 380) {
                            col = g3_lerp565(col, 0xFFFFu, (dot_s - 380) * 3);
                        }
                        pix = g3_blend565_nat(pix, col, fill_str);
                        row[x] = g3_to_nat(pix, swap_bytes);
                        continue;
                    }

                    if (df < 216) {
                        unsigned ta = (df > 200) ? (unsigned)(((unsigned)g3_clamp(((216 - df) * 255) >> 4, 0, 255) * track_str) >> 8) : track_str;
                        pix = g3_blend565_nat(pix, 0x0843u, ta);
                        const int32_t drim = g3_abs32(df - 202);
                        if (drim < 12) {
                            const uint16_t rcol = (r_off < 0) ? 0x3252u : 0x1888u;
                            pix = g3_blend565_nat(pix, rcol, (unsigned)((((12 - drim) * 3500) >> 8) * strength >> 8));
                        }
                    }

                    if (in_arc) {
                        if (df >= 152) {
                            const int gt = ((268 - df) * 510) >> 8;
                            const unsigned ga = (unsigned)((((uint32_t)gt * (uint32_t)gt) >> 8) * bloom_str >> 8);
                            if (ga > 0u) {
                                pix = g3_add565_nat(pix, col_glow, ga);
                            }
                        }
                        if (df < 168) {
                            const unsigned fa = (unsigned)(((unsigned)g3_clamp(((168 - df) * 255) >> 4, 0, 255) * fill_str) >> 8);
                            const uint32_t a512 = g3_atan2_12_512_ring(dy2, dx2);
                            uint32_t prog = (pct >= 100u) ? (a512 >> 1) : ((a512 * inv_fill512) >> 8);
                            if (prog > 255u) { prog = 255u; }
                            uint16_t col = (prog < 160u)
                                ? g3_lerp565(col_deep, col_mid, (int)((prog * 408u) >> 8))
                                : g3_lerp565(col_mid, col_tip, (int)(((prog - 160u) * 687u) >> 8));
                            pix = g3_blend565_nat(pix, col, fa);
                        }
                    }

                    row[x] = g3_to_nat(pix, swap_bytes);
                }
            }
        }
    }

    if (pct > 0u) {
        const int32_t hx16 = G3_CX16 + ((211 * 16 * hc) >> 15);
        const int32_t hy16 = G3_CY16 + ((211 * 16 * hs) >> 15);
        g3_glow_orb_annulus(dst, y0, y1, swap_bytes, hx16, hy16, 12,
                            0x06FFu, (uint8_t)((210 * strength) >> 8),
                            JR_G3_DMODE_FOCUS_RING_R_MIN, JR_G3_DMODE_FOCUS_RING_R_MAX);
        g3_glow_orb_annulus(dst, y0, y1, swap_bytes, hx16, hy16, 5,
                            0xFFFFu, (uint8_t)strength,
                            JR_G3_DMODE_FOCUS_RING_R_MIN, JR_G3_DMODE_FOCUS_RING_R_MAX);
    }

    g3_draw_bottom_pips(dst, y0, y1, swap_bytes,
                        JR_G3_DMODE_FOCUS_PIP_R,
                        env->sessions_done, env->sessions_total,
                        0x3FFFu, 0x06FFu, 0x3252u, strength,
                        JR_G3_DMODE_FOCUS_SAFE_R, JR_G3_DMODE_FOCUS_RING_R_MIN - 1);
}

/* Mode 3: BREATHE — Liquid Aetherium Expanding/Contracting Breathing Halo. */
typedef struct {
    bool     valid;
    uint32_t now_ms;
    uint8_t  phase;
    uint8_t  pct;

    int16_t  r_base16;
    int16_t  r_min_px;
    int16_t  r_max_px;
    int16_t  rw[3][128];
    uint16_t col[3];
    uint16_t col_core;
} g3_breathe_cache_t;

G3_DEFINE_CACHE(g3_breathe_cache_t, s_breathe_cache)

static void g3_breathe_prepare(g3_breathe_cache_t *bc, uint32_t now_ms,
                               uint8_t phase, uint8_t pct)
{
    if (bc->valid && bc->now_ms == now_ms &&
        bc->phase == phase && bc->pct == pct) {
        return;
    }
    bc->valid = true;
    bc->now_ms = now_ms;
    bc->phase = phase;
    bc->pct = pct;

    const uint32_t e_ang = ((uint32_t)pct * 32768u) / 255u;
    const int ease = (32768 - glass_cos_q15(e_ang)) >> 8;
    const int shimmer = glass_sin_q15((now_ms * 44u) & 0xFFFFu);

    int32_t r_base16;
    switch (phase) {
    case 0u:
        r_base16 = 72 * 16 + ((ease * 110 * 16) >> 8);
        bc->col[0] = 0x07FFu; bc->col[1] = 0x07DCu; bc->col[2] = 0x25DFu;
        bc->col_core = 0xEFFFu;
        break;
    case 1u:
        r_base16 = 182 * 16 + ((shimmer * 24) >> 15);
        bc->col[0] = 0xA2FFu; bc->col[1] = 0x44FFu; bc->col[2] = 0xD27Fu;
        bc->col_core = 0xF5FFu;
        break;
    case 2u:
        r_base16 = 182 * 16 - ((ease * 110 * 16) >> 8);
        bc->col[0] = 0x055Du; bc->col[1] = 0x227Au; bc->col[2] = 0x04B5u;
        bc->col_core = 0x9F7Fu;
        break;
    case 3u:
    default:
        r_base16 = 72 * 16 + ((shimmer * 16) >> 15);
        bc->col[0] = 0x1278u; bc->col[1] = 0x03B7u; bc->col[2] = 0x3194u;
        bc->col_core = 0x76BFu;
        break;
    }
    bc->r_base16 = (int16_t)r_base16;

    const int32_t amp0 = 36 + ((r_base16 - 72 * 16) >> 5);
    const int32_t amp1 = (amp0 * 3) >> 2;
    const int32_t amp2 = (amp0 * 5) >> 3;
    const uint32_t p0 = (now_ms * 22u) & 0xFFFFu;
    const uint32_t p1 = (now_ms * 17u + 21845u) & 0xFFFFu;
    const uint32_t p2 = (now_ms * 29u + 43690u) & 0xFFFFu;

    int32_t min_r16 = 198 * 16;
    int32_t max_r16 = 54 * 16;

    for (uint32_t i = 0u; i < 128u; ++i) {
        const uint32_t a = i << 9;
        const int32_t s0 = (glass_sin_q15((a * 3u + p0) & 0xFFFFu) * 3 +
                            glass_sin_q15((a * 5u - p0) & 0xFFFFu) * 2) / 5;
        const int32_t s1 = (glass_sin_q15((a * 2u - p1) & 0xFFFFu) * 3 +
                            glass_sin_q15((a * 4u + p1) & 0xFFFFu) * 2) / 5;
        const int32_t s2 = (glass_sin_q15((a * 4u + p2) & 0xFFFFu) * 3 +
                            glass_sin_q15((a * 6u - p2) & 0xFFFFu) * 2) / 5;

        int32_t r0 = g3_clamp(r_base16 + ((s0 * amp0) >> 15), 62 * 16, 188 * 16);
        int32_t r1 = g3_clamp((r_base16 - 10 * 16) + ((s1 * amp1) >> 15), 60 * 16, 184 * 16);
        int32_t r2 = g3_clamp((r_base16 + 10 * 16) + ((s2 * amp2) >> 15), 66 * 16, 192 * 16);

        bc->rw[0][i] = (int16_t)r0;
        bc->rw[1][i] = (int16_t)r1;
        bc->rw[2][i] = (int16_t)r2;

        if (r0 - 168 < min_r16) { min_r16 = r0 - 168; }
        if (r1 - 108 < min_r16) { min_r16 = r1 - 108; }
        if (r2 - 108 < min_r16) { min_r16 = r2 - 108; }
        if (r0 + 168 > max_r16) { max_r16 = r0 + 168; }
        if (r1 + 108 > max_r16) { max_r16 = r1 + 108; }
        if (r2 + 108 > max_r16) { max_r16 = r2 + 108; }
    }
    bc->r_min_px = (int16_t)g3_clamp((min_r16 >> 4) - 1, JR_G3_DMODE_BREATHE_SAFE_R, JR_G3_DMODE_BREATHE_HALO_MAX);
    bc->r_max_px = (int16_t)g3_clamp((max_r16 >> 4) + 2, JR_G3_DMODE_BREATHE_SAFE_R, JR_G3_DMODE_BREATHE_HALO_MAX);
}

static G3_NOINLINE void g3_dmode_breathe_rows(uint16_t *dst, int y0, int y1, bool swap_bytes,
                                              const jr_g3_desk_mode_t *env)
{
    const int strength = (int)env->strength;
    g3_breathe_cache_t *bc_mut = s_breathe_cache_get();
    if (bc_mut == NULL) {
        return;
    }
    g3_breathe_prepare(bc_mut, env->now_ms, env->breathe_phase, env->pct);
    const g3_breathe_cache_t *bc = bc_mut;

    /* 1. Delicate inhale-target horizon guide hairline at r = 186 */
    g3_hairline_ring_rows(dst, y0, y1, swap_bytes, 186, 14, 0x1A52u, 18, strength, 184, 188);

    /* 2. 3 Concentric Liquid Aetherium Breathing Rings (branchless combined LUTs!) */
    {
        const int r_in = bc->r_min_px;
        const int r_out = bc->r_max_px;
        const int rmin2_sq = (2 * JR_G3_DMODE_BREATHE_SAFE_R) * (2 * JR_G3_DMODE_BREATHE_SAFE_R);
        const int rmax2_sq = (2 * JR_G3_DMODE_BREATHE_HALO_MAX) * (2 * JR_G3_DMODE_BREATHE_HALO_MAX);
        int ry0 = 232 - r_out;
        int ry1 = 233 + r_out;
        if (ry0 < y0) { ry0 = y0; }
        if (ry0 < 0)  { ry0 = 0; }
        if (ry1 >= y1) { ry1 = y1 - 1; }
        if (ry1 >= G3_H) { ry1 = G3_H - 1; }

        const unsigned str = (unsigned)strength;
        const uint32_t c0r = bc->col[0] >> 11, c0g = (bc->col[0] >> 5) & 63u, c0b = bc->col[0] & 31u;
        const uint32_t c1r = bc->col[1] >> 11, c1g = (bc->col[1] >> 5) & 63u, c1b = bc->col[1] & 31u;
        const uint32_t c2r = bc->col[2] >> 11, c2g = (bc->col[2] >> 5) & 63u, c2b = bc->col[2] & 31u;
        const uint32_t cr  = bc->col_core >> 11, cg = (bc->col_core >> 5) & 63u, cb = bc->col_core & 31u;

        for (int y = ry0; y <= ry1; ++y) {
            g3_xspan_t spans[2];
            const int nsp = g3_annulus_row_spans(y, r_in, r_out, spans);
            if (nsp == 0) {
                continue;
            }
            const int dy2 = 2 * y - G3_CY2;
            const int dy2_sq = dy2 * dy2;
            uint16_t *row = dst + (size_t)(y - y0) * G3_W;

            for (int s = 0; s < nsp; ++s) {
                for (int x = spans[s].lo; x <= spans[s].hi; ++x) {
                    const int dx2 = 2 * x - G3_CX2;
                    const int r2_sq = dx2 * dx2 + dy2_sq;
                    if (r2_sq < rmin2_sq || r2_sq > rmax2_sq) {
                        continue;
                    }
                    const int32_t r16 = g3_r16_from_r2sq((uint32_t)r2_sq);
                    const uint32_t ang12 = g3_atan2_12_q16_ring(dy2, dx2);
                    const uint32_t a128 = ang12 >> 9;
                    const uint32_t a128_n = (a128 + 1u) & 127u;
                    const int32_t frac = (int32_t)((ang12 >> 5) & 15u);

                    const int32_t r0 = bc->rw[0][a128] + (((bc->rw[0][a128_n] - bc->rw[0][a128]) * frac) >> 4);
                    const int32_t r1 = bc->rw[1][a128] + (((bc->rw[1][a128_n] - bc->rw[1][a128]) * frac) >> 4);
                    const int32_t r2 = bc->rw[2][a128] + (((bc->rw[2][a128_n] - bc->rw[2][a128]) * frac) >> 4);

                    const uint32_t i0 = (uint32_t)g3_abs32(r16 - r0) >> 1;
                    const uint32_t i1 = (uint32_t)g3_abs32(r16 - r1) >> 1;
                    const uint32_t i2 = (uint32_t)g3_abs32(r16 - r2) >> 1;

                    if (i0 >= 84u && i1 >= 54u && i2 >= 54u) {
                        continue;
                    }
                    uint32_t ar = 0u, ag = 0u, ab = 0u;
                    if (i0 < 84u) {
                        const uint32_t a0 = k_g3_br_col0[i0];
                        ar += c0r * a0; ag += c0g * a0; ab += c0b * a0;
                        if (i0 < 7u) {
                            const uint32_t aw = k_g3_br_wht0[i0];
                            ar += cr * aw; ag += cg * aw; ab += cb * aw;
                        }
                    }
                    if (i1 < 54u) {
                        const uint32_t a1 = k_g3_br_col1[i1];
                        ar += c1r * a1; ag += c1g * a1; ab += c1b * a1;
                    }
                    if (i2 < 54u) {
                        const uint32_t a2 = k_g3_br_col2[i2];
                        ar += c2r * a2; ag += c2g * a2; ab += c2b * a2;
                    }
                    row[x] = g3_acc_flush_str(row[x], ar, ag, ab, str, swap_bytes);
                }
            }
        }
    }

    /* 3. Bottom Breath Cycle Pips near r = 206 */
    g3_draw_bottom_pips(dst, y0, y1, swap_bytes,
                        JR_G3_DMODE_BREATHE_PIP_R,
                        env->sessions_done, env->sessions_total,
                        bc->col[0], bc->col[1], 0x29CCu, strength,
                        JR_G3_DMODE_BREATHE_HALO_MAX, JR_G3_DMODE_BREATHE_R_MAX);
}

void jr_g3_desk_mode_rows(uint16_t *dst, int y0, int nrows, bool swap_bytes,
                          const jr_g3_desk_mode_t *env)
{
    if (dst == NULL || nrows <= 0 || env == NULL ||
        env->strength == 0u || env->mode == JR_G3_DMODE_NONE) {
        return;
    }
    const int y1 = y0 + nrows;
    if (y1 <= 0 || y0 >= G3_H) {
        return;
    }

    switch (env->mode) {
    case JR_G3_DMODE_ON_AIR:
        g3_dmode_on_air_rows(dst, y0, y1, swap_bytes, env);
        break;
    case JR_G3_DMODE_FOCUS:
        g3_dmode_focus_rows(dst, y0, y1, swap_bytes, env);
        break;
    case JR_G3_DMODE_BREATHE:
        g3_dmode_breathe_rows(dst, y0, y1, swap_bytes, env);
        break;
    case JR_G3_DMODE_NONE:
    default:
        break;
    }
}
