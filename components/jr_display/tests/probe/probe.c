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
 *      probe.c ../../src/hud_render.c -o probe && ./probe [scene] [muted]
 *   sips -s format png probe_<scene>.ppm --out probe_<scene>.png   # macOS
 *
 * Scenes: shade (default), detail, status, weather, weather-stale,
 * weather-none, activity, activity-empty. A second argument "muted" turns
 * the privacy ring on, which is what turns every accent gold.
 *
 * NOT part of any automated suite: the output is a picture, and the assert is
 * a human (or agent) looking at it. The exhaustive pins live in
 * ../test_shell.c and ../test_hud_render.c; this exists because
 * jr_display.c's composition has no other executable host path. */
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
    const char *scene = argc > 1 ? argv[1] : "shade";
    const bool muted = argc > 2 && strcmp(argv[2], "muted") == 0;
    s_display.board.width = 466;
    s_display.board.height = 466;
    s_display.board.swap_color_bytes = false;

    /* The ring with nothing on DESK: four steps from home reach the last
     * screen, because DESK is stepped over while it is dark. */
    s_nav_word = 0U;
    jr_display_nav_next();
    assert(jr_display_nav_space() == JR_DISPLAY_SPACE_WATCH);
    jr_display_nav_next();
    jr_display_nav_next();
    jr_display_nav_next();
    assert(jr_display_nav_space() == JR_DISPLAY_SPACE_ACTIVITY);
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

    int space = JR_DISPLAY_SPACE_STATUS;
    int overlay = JR_DISPLAY_OVERLAY_SHADE;
    bool ota = false;
    if (strcmp(scene, "detail") == 0) {
        overlay = JR_DISPLAY_OVERLAY_DETAIL;
        ota = true;
    } else if (strcmp(scene, "status") == 0) {
        overlay = JR_DISPLAY_OVERLAY_NONE;
    } else if (strncmp(scene, "weather", 7) == 0) {
        space = JR_DISPLAY_SPACE_WEATHER;
        overlay = JR_DISPLAY_OVERLAY_NONE;
    } else if (strncmp(scene, "activity", 8) == 0) {
        space = JR_DISPLAY_SPACE_ACTIVITY;
        overlay = JR_DISPLAY_OVERLAY_NONE;
    } else if (strcmp(scene, "shade") != 0) {
        fprintf(stderr, "unknown scene %s\n", scene);
        return 2;
    }

    s_space_on = true;
    s_space_from = (uint8_t)space;
    s_space_to = (uint8_t)space;
    s_space_prog = 256;
    s_space_ease = 256;
    s_space_veil = 256;
    s_shade_ease = overlay == JR_DISPLAY_OVERLAY_SHADE ? 256 : 0;
    s_detail_ease = overlay == JR_DISPLAY_OVERLAY_DETAIL ? 256 : 0;
    s_detail_space = (uint8_t)space;
    s_nav_word = (uint32_t)space | ((uint32_t)overlay << NAV_OVL_SHIFT);
    s_hud_env_word = 74U | (muted ? (1U << 9) : 0U);
    s_orbit_a16 = sp_orbit_angle(space, false) * 16;
    jr_display_set_brightness(60U);
    jr_display_set_status(90U);
    jr_display_power_set(74U, 4020U, true, true);
    jr_display_jarvis_set_session(true, 12U, 3725U);
    /* Mid-update, staged app0 -> ota_1: the state that exercises the most
     * at once (the shell-wide ring, both STATUS rows). */
    jr_display_ota_set(ota ? JR_DISPLAY_OTA_RECEIVING : JR_DISPLAY_OTA_IDLE,
                       63U, 0U, ota ? 1U : 0xFFU, true);

    if (strcmp(scene, "weather-none") != 0) {
        jr_display_weather_t w;
        memset(&w, 0, sizeof w);
        w.valid = true;
        w.temp_f = 83;
        w.feels_f = 88;
        w.hi_f = 86;
        w.lo_f = 76;
        w.rain_pct = 40;
        w.humidity_pct = 72;
        w.wind_mph = 12;
        w.sky = JR_DISPLAY_SKY_CLEAR;
        strcpy(w.condition, "CLEAR");
        w.fetched_ms = 0;
        jr_display_weather_set(&w);
    }
    if (strcmp(scene, "weather-stale") == 0) {
        s_fake_us = 45LL * 60 * 1000 * 1000;
    }
    if (strcmp(scene, "activity-empty") != 0) {
        jr_display_activity_push("WEATHER", "83 CLEAR FORT LAUDERDALE");
        jr_display_activity_push("WEB", "FOUND THREE RESULTS");
        jr_display_activity_push("SAID", "VOLUME IS NOW FORTY");
    }
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

    char name[64];
    snprintf(name, sizeof name, "probe_%s%s.ppm", scene, muted ? "_muted" : "");
    FILE *f = fopen(name, "wb");
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
    printf("wrote %s\n", name);
    return 0;
}
