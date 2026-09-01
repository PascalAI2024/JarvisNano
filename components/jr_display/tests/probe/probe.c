/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Visual probe: renders presenter overlays on the host, for eyes-on-the-glass
 * verification WITHOUT a flash cycle. It #includes jr_display.c itself over
 * the stub headers in ./stubs — statics and all — sets the render-side state
 * directly, composes the real apply_*_overlay functions in 12-row strips
 * exactly as panel_flush does, and writes a PPM. The rendered frame is the
 * expected-render reference to hold a hardware capture against.
 *
 * Build + run (from this directory; no cmake, no IDF):
 *
 *   cc -std=gnu17 -O2 -Wno-unused-function \
 *      -I../../include -I../../../jr_ports/include -I./stubs -I../../src \
 *      probe.c ../../src/hud_render.c -o probe && ./probe
 *   sips -s format png probe_out.ppm --out probe_out.png   # macOS
 *
 * NOT part of any automated suite: the output is a picture, and the assert is
 * a human (or agent) looking at it. The exhaustive pins live in
 * ../test_hud_render.c; this exists because jr_display.c's composition has no
 * other executable host path. Edit main() to stage whatever state you need. */
#include "jr_display.c"
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>

/* ---- stub implementations for every extern jr_display.c references ---- */
#include "idf_stubs.inc"

/* ---- fake face: concentric cyan rings on black, like the baked art ---- */
static int probe_isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static uint16_t face_px(int x, int y)
{
    const int dx = x - 232, dy = y - 232;
    const int r2 = dx * dx + dy * dy;
    if (r2 > 232 * 232) return 0;
    const int r = probe_isqrt(r2);
    int lum = 30;
    if ((r >= 60 && r <= 66) || (r >= 125 && r <= 134) ||
        (r >= 155 && r <= 179) || (r >= 200 && r <= 214)) lum = 180;
    return (uint16_t)(((0) << 11) | (((lum * 63) / 255) << 5) | ((lum * 31) / 255));
}

int main(int argc, char **argv)
{
    const bool detail = argc > 1 && strcmp(argv[1], "detail") == 0;
    s_display.board.width = 466;
    s_display.board.height = 466;
    s_display.board.swap_color_bytes = false;
    s_nav_word = 0U;
    jr_display_nav_next();
    assert(jr_display_nav_space() == JR_DISPLAY_SPACE_WATCH);
    jr_display_nav_next();
    jr_display_nav_next();
    jr_display_nav_next();  /* the last screen on the ring */
    assert(jr_display_nav_space() == JR_DISPLAY_SPACE_TOOLS);
    jr_display_nav_up();
    assert(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_DETAIL);
    jr_display_nav_down();
    assert(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE);
    jr_display_nav_down();
    assert(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_SHADE);
    assert(jr_display_hit(233, 292) == JR_DISPLAY_ACT_PRIVACY_TOGGLE);
    jr_display_nav_home();
    assert(jr_display_nav_space() == JR_DISPLAY_SPACE_JARVIS);
    assert(jr_display_nav_overlay() == JR_DISPLAY_OVERLAY_NONE);

    /* POWER under the shade by default; pass "detail" for the battery and
     * OTA rows. POWER is where the update's rows live now that SETTINGS is
     * gone; the ring itself draws on every space. */
    s_space_on = true;
    s_space_from = JR_DISPLAY_SPACE_POWER;
    s_space_to = JR_DISPLAY_SPACE_POWER;
    s_space_prog = 256;
    s_space_ease = 256;
    s_space_veil = 256;
    s_shade_ease = detail ? 0 : 256;
    s_detail_ease = detail ? 256 : 0;
    s_detail_space = JR_DISPLAY_SPACE_POWER;
    s_nav_word = JR_DISPLAY_SPACE_POWER |
        ((uint32_t)(detail ? JR_DISPLAY_OVERLAY_DETAIL
                          : JR_DISPLAY_OVERLAY_SHADE) << NAV_OVL_SHIFT);
    s_hud_env_word = 74U | (1U << 9);  /* battery 74, privacy gold */
    jr_display_set_brightness(60U);
    jr_display_set_status(90U);
    jr_display_power_set(74U, 4020U, true, true);
    /* Mid-update, staged app0 -> ota_1: the state that exercises the most
     * at once (the shell-wide ring, both POWER rows). */
    jr_display_ota_set(JR_DISPLAY_OTA_RECEIVING, 63U, 0U, 1U, true);
    sp_compose();

    uint16_t *fb = malloc(466u * 466u * sizeof(uint16_t));
    for (int y = 0; y < 466; y++)
        for (int x = 0; x < 466; x++)
            fb[y * 466 + x] = face_px(x, y);

    /* Compose in 12-row strips exactly like the flush does. */
    for (int y = 0; y < 466; y += 12) {
        int y2 = y + 12 > 466 ? 466 : y + 12;
        apply_space_overlay(&s_display, y, y2,
                            fb + (size_t)y * 466);
        apply_ota_ring(&s_display, y, y2, fb + (size_t)y * 466);
    }

    FILE *f = fopen("probe_out.ppm", "wb");
    fprintf(f, "P6 466 466 255\n");
    for (int i = 0; i < 466 * 466; i++) {
        uint16_t v = fb[i];
        unsigned char rgb[3] = {
            (unsigned char)(((v >> 11) & 31) * 255 / 31),
            (unsigned char)(((v >> 5) & 63) * 255 / 63),
            (unsigned char)((v & 31) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote probe_out.ppm\n");
    return 0;
}
