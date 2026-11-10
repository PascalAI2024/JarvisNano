/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass: anti-aliased row primitives for the round data screens.
 *
 * The compositor renders 12-row strips into an RGB565 buffer, one ROW at a
 * time, with no full framebuffer. Every primitive here is therefore a
 * `*_row(row, y, clip_lo, clip_hi, ...)` function that writes only row `y`
 * and only pixels x in [clip_lo, clip_hi] (inclusive; the caller folds the
 * round safe area in), further limited to [0, GLASS_PANEL_PX - 1]. Blending
 * reads the row back.
 *
 * Pure C11, integer-only: no float, no libm, no heap, no ESP-IDF. Compiles on
 * the host with a plain compiler. The only static state is a small cache of
 * prepared ring/arc geometry in glass_draw.c, which assumes one drawing task.
 *
 * Conventions
 *   - Geometry is in 1/16 px. Pixel x covers [16x, 16x + 16); its centre is
 *     16x + 8, so a circle centred on the middle of the 466 px panel has
 *     cx = 233 * 16.
 *   - Angles are Q16 turns (65536 = one turn), 0 at 3 o'clock, increasing
 *     clockwise with y down; 12 o'clock is 49152 (192/256 turn).
 *   - Colours are native RGB565 (red in the top bits), already tinted for any
 *     fade by the caller. `alpha` is 0..255 on top of the shape's own coverage.
 *     `swap_bytes` says the row holds panel-order (byte-swapped) pixels.
 *   - Spans: `sweep_q16 <= 0` draws nothing, `>= 65536` is the full ring.
 */
#ifndef JR_DISPLAY_GLASS_H
#define JR_DISPLAY_GLASS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GLASS_PANEL_PX 466
#define GLASS_TURN_Q16 65536
#define GLASS_Q16_12_OCLOCK 49152

/* ---- blending (header-inline so callers and tests share one definition) -- */

static inline uint16_t glass_bswap(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

/* Mix native colour `over` onto the panel-order pixel `under` with alpha
 * 0..255. Extends hud_mix565 to 6-bit weights (65 levels) with rounding, so
 * a=0 returns `under` and a=255 returns `over` bit-exactly. Same split-field
 * trick: red+blue ride one multiply (31*64 < 2^11), green rides alone. */
static inline uint16_t glass_blend565(uint16_t under, uint16_t over, unsigned a,
                                      bool swap_bytes)
{
    if (a == 0u) {
        return under;
    }
    if (a >= 255u) {
        return swap_bytes ? glass_bswap(over) : over;
    }
    const uint32_t wo = (a * 64u + 127u) / 255u;
    const uint32_t wu = 64u - wo;
    const uint32_t u = swap_bytes ? glass_bswap(under) : under;
    uint32_t rb = ((u & 0xF81Fu) * wu + (uint32_t)(over & 0xF81Fu) * wo + 0x10020u) >> 6;
    uint32_t g = ((u & 0x07E0u) * wu + (uint32_t)(over & 0x07E0u) * wo + (32u << 5)) >> 6;
    uint16_t out = (uint16_t)((rb & 0xF81Fu) | (g & 0x07E0u));
    return swap_bytes ? glass_bswap(out) : out;
}

/* ---- trig ---------------------------------------------------------------- */

/* Q15 sine / cosine of a Q16 turn angle (any value; wraps). Linear
 * interpolation of the 65-entry quarter table the spatial shell uses. */
int32_t glass_sin_q15(uint32_t angle_q16);
int32_t glass_cos_q15(uint32_t angle_q16);

/* ---- text ---------------------------------------------------------------- */

typedef enum {
    GLASS_FONT_LABEL = 0, /* SemiBold, cap 11 px: small tracked labels      */
    GLASS_FONT_TEXT,      /* Medium,   cap 15 px: body text                 */
    GLASS_FONT_VALUE,     /* Medium,   cap 24 px: units and short values    */
    GLASS_FONT_HERO,      /* Medium,   cap 59 px: digits - minus colon only */
    GLASS_FONT_COUNT
} glass_font_id_t;

/* One glyph in an atlas. `off` indexes the font's alpha bytes: 4-bit alpha,
 * two pixels per byte (high nibble first), row-major, w*h pixels. `xo` is
 * relative to the pen origin, `yo` to the top of the cap height. */
typedef struct {
    uint16_t cp;
    uint16_t off;
    uint8_t w, h;
    int8_t xo, yo;
    uint8_t adv;
} glass_glyph_t;

typedef struct {
    const glass_glyph_t *glyphs; /* sorted by code point */
    const uint8_t *bits;
    uint16_t n_glyphs;
    uint8_t cap_h;
    uint8_t line_h; /* line spacing for stacking lines */
    int8_t ink_top; /* highest ink, relative to the cap top (<= 0) */
    int8_t ink_bottom; /* lowest ink, relative to the cap top */
    uint8_t space_adv;
} glass_font_t;

/* Generated (scripts/gen-glass-font.py) into glass_font_data.c. */
extern const glass_font_t glass_fonts[GLASS_FONT_COUNT];

typedef struct {
    int cap_h;      /* height of capitals and digits: centre on this      */
    int line_h;     /* line spacing                                       */
    int ink_top;    /* highest ink above/below the cap top (<= 0)         */
    int ink_bottom; /* lowest ink below the cap top (descenders, commas)  */
    int space_adv;
} glass_metrics_t;

/* All zero for an unknown font. */
glass_metrics_t glass_font_metrics(glass_font_id_t font);

/* Code point -> glyph, or NULL. Lowercase folds to uppercase and '*' is the
 * degree sign (the 5x7 font's spelling). */
const glass_glyph_t *glass_font_glyph(glass_font_id_t font, uint32_t cp);

/* Width of `len` bytes of UTF-8 (stops at NUL): the sum of the glyph advances
 * plus `tracking_px` between neighbours (none after the last). Characters the
 * font lacks count as a space. */
int glass_text_width(glass_font_id_t font, const char *text, size_t len, int tracking_px);

/* Draw one row of the text whose pen starts at x0 and whose cap-height top is
 * at y_top (so a caller centres with y_top = cy - cap_h / 2). */
void glass_text_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    glass_font_id_t font, const char *text, size_t len,
                    int x0, int y_top, int tracking_px,
                    uint16_t color565, uint8_t alpha, bool swap_bytes);

/* ---- shapes -------------------------------------------------------------- */

/* Filled circle. */
void glass_disc_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    int32_t cx16, int32_t cy16, int32_t r16,
                    uint16_t color565, uint8_t alpha, bool swap_bytes);

/* Annulus [r_in16, r_out16], optionally limited to a span with flat ends. */
void glass_ring_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                    int32_t start_q16, int32_t sweep_q16,
                    uint16_t color565, uint8_t alpha, bool swap_bytes);

/* A full-turn ring that never changes (the orbit rail), recorded once and
 * replayed. glass_ring_record() runs the real glass_ring_row over every panel
 * row and keeps what it would do: the solid runs it fills and the edge pixels
 * with their coverage. glass_ring_replay_row() does exactly those fills and
 * blends, so the pixels are the same as glass_ring_row's for any colour, alpha
 * and clip. With buf == NULL it only measures. Returns the bytes used (the
 * caller allocates that many, 2-byte aligned) or 0 if cap is too small. */
size_t glass_ring_record(uint8_t *buf, size_t cap, int32_t cx16, int32_t cy16,
                         int32_t r_in16, int32_t r_out16);
void glass_ring_replay_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                           const uint8_t *rec, uint16_t color565, uint8_t alpha,
                           bool swap_bytes);

/* The same for any ring, sector or arc that holds still for a while (a ring
 * screen's track and its filled arc). glass_shape_record() keeps what
 * glass_ring_row (round_caps false) or glass_arc_row (round_caps true) would do
 * for these parameters; glass_ring_replay_row() then stands in for either call
 * with any colour, alpha and clip. glass_glow_record() keeps the halo of
 * glass_glow_row the same way, as the distance of each lit pixel; the replay
 * takes the glow's width, peak, colour and alpha. All three return the bytes
 * used (0 if cap is too small, the shape is empty or, for the glow, the width
 * is past the lookup table), and with buf == NULL only measure. */
size_t glass_shape_record(uint8_t *buf, size_t cap, int32_t cx16, int32_t cy16,
                          int32_t r_in16, int32_t r_out16, int32_t start_q16, int32_t sweep_q16,
                          bool round_caps);
size_t glass_glow_record(uint8_t *buf, size_t cap, int32_t cx16, int32_t cy16,
                         int32_t r_in16, int32_t r_out16, int32_t start_q16, int32_t sweep_q16,
                         bool round_caps, int32_t width16);
void glass_glow_replay_row(uint16_t *row, int y, int clip_lo, int clip_hi, const uint8_t *rec,
                           int32_t width16, uint8_t peak, uint16_t color565, uint8_t alpha,
                           bool swap_bytes);

/* The same span with round caps (cap radius = half the stroke). A sweep of a
 * full turn or more has no ends and equals the ring. */
void glass_arc_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                   int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                   int32_t start_q16, int32_t sweep_q16,
                   uint16_t color565, uint8_t alpha, bool swap_bytes);

/* The rows glass_ring_row / glass_arc_row can draw anything on for these
 * parameters (y_lo..y_hi, inclusive, before the panel clip): every other row
 * is a call that returns having changed nothing, so a caller with many arcs
 * can skip it. Exact, not a bound looser than the primitive's own early-out.
 * False when the shape is empty. */
bool glass_arc_rows(int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                    int32_t start_q16, int32_t sweep_q16, bool round_caps, int *y_lo, int *y_hi);

/* Soft halo around a ring or arc: coverage `peak` (0..255) at the stroke edge
 * (and over the stroke), falling quadratically to 0 at `width16` away, on
 * both sides. `round_caps` shapes the span ends like glass_arc_row, otherwise
 * like glass_ring_row. Adds light (saturating per channel, ordered-dithered)
 * so it never overflows a channel and shows no banding. Draw it BEFORE the
 * stroke. */
void glass_glow_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    int32_t cx16, int32_t cy16, int32_t r_in16, int32_t r_out16,
                    int32_t start_q16, int32_t sweep_q16, bool round_caps,
                    int32_t width16, uint8_t peak,
                    uint16_t color565, uint8_t alpha, bool swap_bytes);

/* ---- motion: pure timeline maths for the entry animation ----------------- */

/* Fractions are Q16: 0 is not started, GLASS_ONE_Q16 is finished. All of these
 * are integer-only and stateless, so a host test pins the whole timeline. */
#define GLASS_ONE_Q16 65536u

/* Ease-out-cubic of t over dur: fast first, settling last. t >= dur (or a zero
 * dur) is exactly GLASS_ONE_Q16, t = 0 is 0, and it never decreases. */
uint32_t glass_ease_out_cubic_q16(uint32_t t_ms, uint32_t dur_ms);

/* The same ease for an element that starts `start_ms` into the entry: 0 until
 * then. Staggering is just different start_ms for different elements. */
uint32_t glass_stagger_q16(uint32_t t_ms, uint32_t start_ms, uint32_t dur_ms);

/* target * frac, rounded; the count-up value of a number at that fraction.
 * Exactly `target` once frac reaches one (and for negatives too). */
int32_t glass_count_up(int32_t target, uint32_t frac_q16);

/* Pixels an element still has to rise: rise_px at frac 0, 0 at frac one. */
int glass_rise_px(int rise_px, uint32_t frac_q16);

/* alpha * frac, rounded. */
uint8_t glass_frac_alpha(uint8_t alpha, uint32_t frac_q16);

/* True while the entry clock is inside its total length: the screen asks for
 * frames exactly this long and then stays still. */
bool glass_entry_animating(uint32_t t_ms, uint32_t total_ms);

#ifdef __cplusplus
}
#endif

#endif /* JR_DISPLAY_GLASS_H */
