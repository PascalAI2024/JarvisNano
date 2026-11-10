/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass_icon: draws one row of a baked icon. See glass_icon.h. Pure C11, no
 * state; the coverage tables are generated (glass_icon_data.c).
 */
#include "jr_display/glass_icon.h"

#include "jr_display/glass.h"

int glass_icon_px(glass_icon_size_t size)
{
    switch (size) {
    case GLASS_ICON_SM: return GLASS_ICON_SM_PX;
    case GLASS_ICON_LG: return GLASS_ICON_LG_PX;
    default:            return 0;
    }
}

void glass_icon_row(uint16_t *row, int y, int clip_lo, int clip_hi,
                    glass_icon_id_t icon, glass_icon_size_t size,
                    int x0, int y_top, uint16_t color565, uint8_t alpha,
                    bool swap_bytes)
{
    const int px = glass_icon_px(size);
    if (row == NULL || alpha == 0u || px == 0 || (unsigned)icon >= (unsigned)GLASS_ICON_COUNT) {
        return;
    }
    const int gy = y - y_top;
    if (gy < 0 || gy >= px || y < 0 || y >= GLASS_PANEL_PX) {
        return;
    }
    if (clip_lo < 0) clip_lo = 0;
    if (clip_hi > GLASS_PANEL_PX - 1) clip_hi = GLASS_PANEL_PX - 1;
    int c0 = 0, c1 = px;
    if (x0 + c0 < clip_lo) c0 = clip_lo - x0;
    if (x0 + c1 > clip_hi + 1) c1 = clip_hi + 1 - x0;
    if (c0 >= c1) {
        return;
    }
    const uint8_t *bits = size == GLASS_ICON_SM ? glass_icon_sm[icon] : glass_icon_lg[icon];
    const uint16_t solid = swap_bytes ? glass_bswap(color565) : color565;
    for (int c = c0; c < c1; c++) {
        const unsigned idx = (unsigned)gy * (unsigned)px + (unsigned)c;
        const unsigned nib = (idx & 1u) ? (bits[idx >> 1] & 0x0Fu) : (bits[idx >> 1] >> 4);
        if (nib == 0u) {
            continue;
        }
        const unsigned a = (nib * 17u * ((unsigned)alpha + 1u)) >> 8;
        uint16_t *p = &row[x0 + c];
        *p = (a >= 255u) ? solid : glass_blend565(*p, color565, a, swap_bytes);
    }
}
