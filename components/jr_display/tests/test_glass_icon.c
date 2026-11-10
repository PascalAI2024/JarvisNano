/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Property tests for the glass icon set (glass_icon.h): every icon has ink at
 * both sizes, never leaves its box, honours the clip, alpha and byte order, and
 * the muted microphone is the microphone plus a slash. No pixel hashes.
 *
 * The generator's own check (python scripts/gen-glass-icons.py --check) needs
 * Pillow and the source sheet, so it is run by hand, not here.
 *
 *     cd components/jr_display/tests && cmake -B build && cmake --build build \
 *       --target jr_display_icon_tests && ./build/jr_display_icon_tests
 */
#include "jr_display/glass.h"
#include "jr_display/glass_icon.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define W GLASS_PANEL_PX
#define POISON 0x1234u

static const char *const k_name[GLASS_ICON_COUNT] = {
    "sun", "moon", "partly", "cloud", "rain", "thunder", "snow", "fog",
    "mic", "speaker", "bolt", "wifi", "mic-muted",
};

/* Draw a whole icon over a poisoned strip of `rows` rows starting at y0; return
 * the pixels that changed and the box they span. */
typedef struct {
    int ink;
    int minx, maxx, miny, maxy;
} ink_t;

static ink_t draw_all(glass_icon_id_t id, glass_icon_size_t size, int x0, int y_top,
                      int lo, int hi, uint8_t alpha, bool swap)
{
    static uint16_t fb[80][W];
    const int y0 = y_top - 8;
    ink_t t = {0, W, -1, 1000, -1};
    for (int r = 0; r < 80; r++) {
        for (int x = 0; x < W; x++) {
            fb[r][x] = POISON;
        }
    }
    for (int r = 0; r < 80; r++) {
        glass_icon_row(fb[r], y0 + r, lo, hi, id, size, x0, y_top, 0xFFFFu, alpha, swap);
    }
    for (int r = 0; r < 80; r++) {
        for (int x = 0; x < W; x++) {
            if (fb[r][x] != POISON) {
                t.ink++;
                if (x < t.minx) t.minx = x;
                if (x > t.maxx) t.maxx = x;
                if (y0 + r < t.miny) t.miny = y0 + r;
                if (y0 + r > t.maxy) t.maxy = y0 + r;
            }
        }
    }
    return t;
}

static void test_sizes(void)
{
    CHECK(glass_icon_px(GLASS_ICON_SM) == 28, "small is 28 px");
    CHECK(glass_icon_px(GLASS_ICON_LG) == 44, "large is 44 px");
    CHECK(glass_icon_px(GLASS_ICON_SIZES) == 0, "an unknown size has no box");
    CHECK(glass_icon_px((glass_icon_size_t)-1) == 0, "a negative size has no box");
}

static void test_every_icon_has_ink_inside_its_box(void)
{
    const int x0 = 100, y_top = 200;
    int ink[GLASS_ICON_SIZES][GLASS_ICON_COUNT];
    for (int s = 0; s < GLASS_ICON_SIZES; s++) {
        const int px = glass_icon_px((glass_icon_size_t)s);
        for (int i = 0; i < GLASS_ICON_COUNT; i++) {
            const ink_t t = draw_all((glass_icon_id_t)i, (glass_icon_size_t)s, x0, y_top, 0, W - 1,
                                     255, false);
            ink[s][i] = t.ink;
            CHECK(t.ink >= px * 3, "%s at %d px has ink (%d pixels)", k_name[i], px, t.ink);
            CHECK(t.minx >= x0 && t.maxx < x0 + px && t.miny >= y_top && t.maxy < y_top + px,
                  "%s at %d px stays in its box: x %d..%d y %d..%d", k_name[i], px, t.minx, t.maxx,
                  t.miny, t.maxy);
            /* line art, not a blob: it never fills more than 60% of its box */
            CHECK(t.ink * 100 < px * px * 60, "%s at %d px is line art (%d of %d)", k_name[i], px,
                  t.ink, px * px);
            /* and it is centred: the ink box leaves a margin on every side */
            CHECK(t.minx - x0 <= px / 2 && x0 + px - 1 - t.maxx <= px / 2,
                  "%s at %d px is not pushed to one side", k_name[i], px);
        }
    }
    for (int i = 0; i < GLASS_ICON_COUNT; i++) {
        CHECK(ink[GLASS_ICON_LG][i] > ink[GLASS_ICON_SM][i], "%s has more ink at the large size",
              k_name[i]);
    }
    CHECK(ink[GLASS_ICON_LG][GLASS_ICON_MIC_MUTED] != ink[GLASS_ICON_LG][GLASS_ICON_MIC],
          "the slash changes the microphone");
}

static void test_clip_alpha_and_byte_order(void)
{
    const int x0 = 100, y_top = 200;
    /* clip: nothing outside [lo, hi], and the clipped part is the same pixels */
    const ink_t full = draw_all(GLASS_ICON_SUN, GLASS_ICON_LG, x0, y_top, 0, W - 1, 255, false);
    const ink_t part = draw_all(GLASS_ICON_SUN, GLASS_ICON_LG, x0, y_top, 110, 125, 255, false);
    CHECK(part.ink > 0 && part.ink < full.ink, "a clip keeps part of the icon (%d of %d)", part.ink,
          full.ink);
    CHECK(part.minx >= 110 && part.maxx <= 125, "the clip is honoured: x %d..%d", part.minx, part.maxx);
    const ink_t none = draw_all(GLASS_ICON_SUN, GLASS_ICON_LG, x0, y_top, 300, 100, 255, false);
    CHECK(none.ink == 0, "an empty clip draws nothing");
    const ink_t off = draw_all(GLASS_ICON_SUN, GLASS_ICON_LG, -30, y_top, 0, W - 1, 255, false);
    CHECK(off.ink > 0 && off.minx == 0, "an icon off the left edge is clipped, not wrapped");
    const ink_t right = draw_all(GLASS_ICON_SUN, GLASS_ICON_LG, W - 10, y_top, 0, W - 1, 255, false);
    CHECK(right.ink > 0 && right.maxx == W - 1, "an icon off the right edge is clipped, not wrapped");

    CHECK(draw_all(GLASS_ICON_SUN, GLASS_ICON_SM, x0, y_top, 0, W - 1, 0, false).ink == 0,
          "alpha 0 draws nothing");
    CHECK(draw_all((glass_icon_id_t)GLASS_ICON_COUNT, GLASS_ICON_SM, x0, y_top, 0, W - 1, 255, false).ink == 0, "an unknown icon draws nothing");
    CHECK(draw_all(GLASS_ICON_SUN, GLASS_ICON_SIZES, x0, y_top, 0, W - 1, 255, false).ink == 0,
          "an unknown size draws nothing");

    /* alpha scales coverage: a half-alpha icon over black is dimmer everywhere */
    uint16_t row_full[W], row_half[W];
    memset(row_full, 0, sizeof row_full);
    memset(row_half, 0, sizeof row_half);
    int dimmer = 0, same = 0;
    for (int y = y_top; y < y_top + 44; y++) {
        memset(row_full, 0, sizeof row_full);
        memset(row_half, 0, sizeof row_half);
        glass_icon_row(row_full, y, 0, W - 1, GLASS_ICON_SUN, GLASS_ICON_LG, x0, y_top, 0xFFFFu, 255, false);
        glass_icon_row(row_half, y, 0, W - 1, GLASS_ICON_SUN, GLASS_ICON_LG, x0, y_top, 0xFFFFu, 128, false);
        for (int x = x0; x < x0 + 44; x++) {
            if (row_half[x] != row_full[x]) {
                dimmer += (row_half[x] & 0x07E0u) < (row_full[x] & 0x07E0u);
            } else {
                same += row_full[x] != 0;
            }
        }
    }
    CHECK(dimmer > 50 && same == 0, "half alpha dims every inked pixel (%d dimmer, %d equal)", dimmer, same);

    /* byte order: a swapped row is the byte-swap of the native one */
    int bad = 0;
    for (int y = y_top; y < y_top + 28; y++) {
        uint16_t a[W], b[W];
        for (int x = 0; x < W; x++) {
            a[x] = (uint16_t)(0x0410u + x);
            b[x] = glass_bswap(a[x]);
        }
        glass_icon_row(a, y, 0, W - 1, GLASS_ICON_RAIN, GLASS_ICON_SM, x0, y_top, 0x07FFu, 200, false);
        glass_icon_row(b, y, 0, W - 1, GLASS_ICON_RAIN, GLASS_ICON_SM, x0, y_top, 0x07FFu, 200, true);
        for (int x = 0; x < W; x++) {
            bad += glass_bswap(a[x]) != b[x];
        }
    }
    CHECK(bad == 0, "swap_bytes is the byte-swap of the native draw (%d pixels differ)", bad);
}

int main(void)
{
    test_sizes();
    test_every_icon_has_ink_inside_its_box();
    test_clip_alpha_and_byte_order();
    if (g_failures) {
        printf("%d failure(s) of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("glass icon tests passed (%d checks)\n", g_checks);
    return 0;
}
