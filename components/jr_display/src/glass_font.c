/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass text: UTF-8 in, anti-aliased glyph rows out. The atlases live in
 * glass_font_data.c (generated); this file only looks glyphs up and blits
 * them one row at a time. See glass.h for the contract.
 */
#include "jr_display/glass.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define GLASS_DEGREE_CP 0x00B0u

/* Next code point of a UTF-8 string, advancing *i. Malformed or unsupported
 * (4-byte) sequences yield U+FFFD, which no atlas has, so they draw as a
 * space. Never reads past `len` or a NUL. */
static uint32_t utf8_next(const char *s, size_t len, size_t *i)
{
    const uint8_t c = (uint8_t)s[*i];
    size_t need;
    uint32_t cp;
    if (c < 0x80u) {
        (*i)++;
        return c;
    } else if ((c & 0xE0u) == 0xC0u) {
        need = 1;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0u) == 0xE0u) {
        need = 2;
        cp = c & 0x0Fu;
    } else {
        (*i)++;
        return 0xFFFDu;
    }
    (*i)++;
    while (need > 0) {
        if (*i >= len || ((uint8_t)s[*i] & 0xC0u) != 0x80u) {
            return 0xFFFDu;
        }
        cp = (cp << 6) | ((uint8_t)s[*i] & 0x3Fu);
        (*i)++;
        need--;
    }
    return cp;
}

static bool font_ok(glass_font_id_t font)
{
    return (unsigned)font < (unsigned)GLASS_FONT_COUNT;
}

glass_metrics_t glass_font_metrics(glass_font_id_t font)
{
    glass_metrics_t m = {0, 0, 0, 0, 0};
    if (font_ok(font)) {
        const glass_font_t *f = &glass_fonts[font];
        m.cap_h = f->cap_h;
        m.line_h = f->line_h;
        m.ink_top = f->ink_top;
        m.ink_bottom = f->ink_bottom;
        m.space_adv = f->space_adv;
    }
    return m;
}

/* Printable ASCII goes through a table built from the atlases on first use: the
 * atlases have gaps, so the "index = cp - first" guess below overshoots by more
 * than its six-step walk and most lookups ended in the binary search (about
 * 60 instructions a glyph, and this runs for every glyph of every text line on
 * every row). Entry = glyph index + 1; 0 = no such glyph (the atlas is sorted
 * and unique, so a code point absent from it is absent); 255 = index too large
 * for a byte, search. GLASS_FONT_COUNT * 96 bytes of .bss. Built by whichever
 * task asks first: two builders write the same values, and the ready flag is
 * published last, so a reader either sees a finished table or searches. */
#define GLYPH_LUT_FIRST 32u
#define GLYPH_LUT_N 96u
#ifdef ESP_PLATFORM
static uint8_t (*s_glyph_lut)[GLYPH_LUT_N];
static uint8_t s_glyph_lut_tried;

static const uint8_t (*glyph_lut_get(void))[GLYPH_LUT_N]
{
    uint8_t (*lut)[GLYPH_LUT_N] = __atomic_load_n(&s_glyph_lut, __ATOMIC_ACQUIRE);
    if (lut != NULL) {
        return (const uint8_t (*)[GLYPH_LUT_N])lut;
    }
    if (__atomic_exchange_n(&s_glyph_lut_tried, 1u, __ATOMIC_ACQ_REL) != 0u) {
        return NULL;
    }
    lut = heap_caps_calloc(GLASS_FONT_COUNT, GLYPH_LUT_N,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (lut == NULL) {
        return NULL;
    }
    for (int fi = 0; fi < (int)GLASS_FONT_COUNT; fi++) {
        const glass_font_t *f = &glass_fonts[fi];
        for (unsigned i = 0; i < f->n_glyphs; i++) {
            const unsigned cp = f->glyphs[i].cp;
            if (cp >= GLYPH_LUT_FIRST && cp < GLYPH_LUT_FIRST + GLYPH_LUT_N) {
                lut[fi][cp - GLYPH_LUT_FIRST] = i < 254u ? (uint8_t)(i + 1u) : 255u;
            }
        }
    }
    __atomic_store_n(&s_glyph_lut, lut, __ATOMIC_RELEASE);
    return (const uint8_t (*)[GLYPH_LUT_N])lut;
}
#else
static uint8_t s_glyph_lut[GLASS_FONT_COUNT][GLYPH_LUT_N];
static uint8_t s_glyph_lut_ready;

static void glyph_lut_build(void)
{
    for (int fi = 0; fi < (int)GLASS_FONT_COUNT; fi++) {
        const glass_font_t *f = &glass_fonts[fi];
        for (unsigned i = 0; i < f->n_glyphs; i++) {
            const unsigned cp = f->glyphs[i].cp;
            if (cp >= GLYPH_LUT_FIRST && cp < GLYPH_LUT_FIRST + GLYPH_LUT_N) {
                s_glyph_lut[fi][cp - GLYPH_LUT_FIRST] = i < 254u ? (uint8_t)(i + 1u) : 255u;
            }
        }
    }
    __atomic_store_n(&s_glyph_lut_ready, 1u, __ATOMIC_RELEASE);
}
#endif

const glass_glyph_t *glass_font_glyph(glass_font_id_t font, uint32_t cp)
{
    if (!font_ok(font)) {
        return NULL;
    }
    const glass_font_t *f = &glass_fonts[font];
    if (cp >= 'a' && cp <= 'z') {
        cp -= 'a' - 'A';
    } else if (cp == '*') {
        cp = GLASS_DEGREE_CP;
    }
    if (cp - GLYPH_LUT_FIRST < GLYPH_LUT_N) {
#ifdef ESP_PLATFORM
        const uint8_t (*lut)[GLYPH_LUT_N] = glyph_lut_get();
        if (lut != NULL) {
            const unsigned v = lut[font][cp - GLYPH_LUT_FIRST];
            if (v == 0u) {
                return NULL;
            }
            if (v != 255u) {
                return &f->glyphs[v - 1u];
            }
        }
#else
        if (!__atomic_load_n(&s_glyph_lut_ready, __ATOMIC_ACQUIRE)) {
            glyph_lut_build();
        }
        const unsigned v = s_glyph_lut[font][cp - GLYPH_LUT_FIRST];
        if (v == 0u) {
            return NULL;
        }
        if (v != 255u) {
            return &f->glyphs[v - 1u];
        }
#endif
    }
    if (f->n_glyphs == 0u || cp < f->glyphs[0].cp) {
        return NULL;
    }
    /* The atlas is sorted and unique, so glyph i has code point >= first + i:
     * cp can only sit at or below index cp - first. For text that is nearly
     * contiguous (ASCII) it is at that index, or a few gaps below it; try a
     * short walk down before the binary search. This runs for every glyph of
     * every text line on every row. */
    size_t i = (size_t)(cp - f->glyphs[0].cp);
    if (i >= f->n_glyphs) {
        i = (size_t)f->n_glyphs - 1u;
    }
    for (int steps = 0; steps < 6; steps++) {
        if (f->glyphs[i].cp <= cp) {
            return f->glyphs[i].cp == cp ? &f->glyphs[i] : NULL;
        }
        if (i == 0u) {
            return NULL;
        }
        i--;
    }
    size_t lo = 0, hi = i + 1u;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo < f->n_glyphs && f->glyphs[lo].cp == cp) ? &f->glyphs[lo] : NULL;
}

static int glyph_advance(const glass_font_t *f, const glass_glyph_t *g)
{
    return g ? g->adv : f->space_adv;
}

int glass_text_width(glass_font_id_t font, const char *text, size_t len, int tracking_px)
{
    if (!font_ok(font) || text == NULL) {
        return 0;
    }
    const glass_font_t *f = &glass_fonts[font];
    int w = 0, n = 0;
    size_t i = 0;
    while (i < len && text[i] != '\0') {
        w += glyph_advance(f, glass_font_glyph(font, utf8_next(text, len, &i)));
        n++;
    }
    return n > 0 ? w + tracking_px * (n - 1) : 0;
}

void glass_text_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    glass_font_id_t font, const char *text, size_t len,
                    int x0, int y_top, int tracking_px,
                    uint16_t color565, uint8_t alpha, bool swap_bytes)
{
    if (row == NULL || text == NULL || !font_ok(font) || alpha == 0u) {
        return;
    }
    if (clip_lo < 0) clip_lo = 0;
    if (clip_hi > GLASS_PANEL_PX - 1) clip_hi = GLASS_PANEL_PX - 1;
    const glass_font_t *f = &glass_fonts[font];
    if (y < 0 || y >= GLASS_PANEL_PX || clip_lo > clip_hi ||
        y < y_top + f->ink_top || y >= y_top + f->ink_bottom) {
        return;
    }
    const uint16_t solid = swap_bytes ? glass_bswap(color565) : color565;
    int pen = x0;
    size_t i = 0;
    while (i < len && text[i] != '\0') {
        const glass_glyph_t *g = glass_font_glyph(font, utf8_next(text, len, &i));
        const int gy = y - (y_top + (g ? g->yo : 0));
        if (g != NULL && g->w > 0 && gy >= 0 && gy < g->h) {
            const int gx0 = pen + g->xo;
            const uint8_t *bits = f->bits + g->off;
            int c0 = 0, c1 = g->w;
            if (gx0 + c0 < clip_lo) c0 = clip_lo - gx0;
            if (gx0 + c1 > clip_hi + 1) c1 = clip_hi + 1 - gx0;
            /* Two pixels to a byte, high nibble first; a zero byte (most of a
             * thin glyph's row) is skipped whole. */
            unsigned idx = (unsigned)gy * g->w + (unsigned)c0;
            for (int c = c0; c < c1; c++, idx++) {
                const unsigned byte = bits[idx >> 1];
                if (byte == 0u && (idx & 1u) == 0u) {
                    c++;
                    idx++;
                    continue;
                }
                const unsigned nib = (idx & 1u) ? (byte & 0x0Fu) : (byte >> 4);
                if (nib == 0u) {
                    continue;
                }
                const unsigned a = (nib * 17u * ((unsigned)alpha + 1u)) >> 8;
                uint16_t *p = &row[gx0 + c];
                *p = (a >= 255u) ? solid : glass_blend565(*p, color565, a, swap_bytes);
            }
        }
        pen += glyph_advance(f, g) + tracking_px;
        if (pen > clip_hi + 1 + 128) {
            break; /* past the clip by more than any glyph is wide */
        }
    }
}
