/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass text: UTF-8 in, anti-aliased glyph rows out. The atlases live in
 * glass_font_data.c (generated); this file only looks glyphs up and blits
 * them one row at a time. See glass.h for the contract.
 */
#include "jr_display/glass.h"

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
    size_t lo = 0, hi = f->n_glyphs;
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
            for (int c = c0; c < c1; c++) {
                const unsigned idx = (unsigned)gy * g->w + (unsigned)c;
                const unsigned nib = (idx & 1u) ? (bits[idx >> 1] & 0x0Fu) : (bits[idx >> 1] >> 4);
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
