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
 *      probe.c ../../src/hud_render.c ../../src/companion.c ../../src/glass_font.c \
 *      ../../src/glass_draw.c ../../src/glass_font_data.c -o probe \
 *      && ./probe [scene] [muted] [low] [nogauge] [ent=MS] [dir=DIR]
 *
 * Scenes: shade (default), detail, status, weather, weather-stale,
 * weather-none, activity, activity-empty. "muted" turns the privacy ring on,
 * which is what turns every accent gold; "low" and "nogauge" set the battery;
 * "ent=MS" sets the entry clock of the data screens (omitted: settled);
 * "parked" renders the data screen on true black with the face parked;
 * "dir=DIR" is where the ppm goes. Every run prints the pixels the screen
 * itself writes in one frame (the veil excluded).
 *
 * NOT part of any automated suite: the output is a picture, and the assert is
 * a human (or agent) looking at it. The exhaustive pins live in
 * ../test_shell.c and ../test_hud_render.c; this exists because
 * jr_display.c's composition has no other executable host path. */
#ifndef NO_ENTRY
long g_wr;   /* pixels the shell's own primitives wrote: see the --wrap wrappers below */
#endif
#include "jr_display.c"
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>

/* ---- stub implementations for every extern jr_display.c references ---- */
#include "idf_stubs.inc"

#ifndef NO_ENTRY
/* The glass primitives are the only code that draws the data screens, and they
 * live in another translation unit: the linker wraps them (-Wl,--wrap=...), and
 * each wrapper counts the pixels its call changed. (The old 5x7 path counts at
 * its own write sites in the copy of jr_display.c the comparison builds.) */
#define WRAP_ROW(call_real, ...)                                                   \
    do {                                                                           \
        uint16_t before_[466];                                                     \
        memcpy(before_, row, sizeof before_);                                      \
        call_real(__VA_ARGS__);                                                    \
        for (int i_ = 0; i_ < 466; i_++) g_wr += row[i_] != before_[i_];           \
    } while (0)
void __real_glass_ring_row(uint16_t *, int, int, int, int32_t, int32_t, int32_t, int32_t, int32_t,
                           int32_t, uint16_t, uint8_t, bool);
void __wrap_glass_ring_row(uint16_t *row, int y, int lo, int hi, int32_t cx, int32_t cy, int32_t ri,
                           int32_t ro, int32_t st, int32_t sw, uint16_t c, uint8_t a, bool sb)
{
    WRAP_ROW(__real_glass_ring_row, row, y, lo, hi, cx, cy, ri, ro, st, sw, c, a, sb);
}
void __real_glass_arc_row(uint16_t *, int, int, int, int32_t, int32_t, int32_t, int32_t, int32_t,
                          int32_t, uint16_t, uint8_t, bool);
void __wrap_glass_arc_row(uint16_t *row, int y, int lo, int hi, int32_t cx, int32_t cy, int32_t ri,
                          int32_t ro, int32_t st, int32_t sw, uint16_t c, uint8_t a, bool sb)
{
    WRAP_ROW(__real_glass_arc_row, row, y, lo, hi, cx, cy, ri, ro, st, sw, c, a, sb);
}
void __real_glass_glow_row(uint16_t *, int, int, int, int32_t, int32_t, int32_t, int32_t, int32_t,
                           int32_t, bool, int32_t, uint8_t, uint16_t, uint8_t, bool);
void __wrap_glass_glow_row(uint16_t *row, int y, int lo, int hi, int32_t cx, int32_t cy, int32_t ri,
                           int32_t ro, int32_t st, int32_t sw, bool rc, int32_t w, uint8_t pk,
                           uint16_t c, uint8_t a, bool sb)
{
    WRAP_ROW(__real_glass_glow_row, row, y, lo, hi, cx, cy, ri, ro, st, sw, rc, w, pk, c, a, sb);
}
void __real_glass_text_row(uint16_t *, int, int, int, glass_font_id_t, const char *, size_t, int,
                           int, int, uint16_t, uint8_t, bool);
void __wrap_glass_text_row(uint16_t *row, int y, int lo, int hi, glass_font_id_t f, const char *t,
                           size_t n, int x0, int yt, int tr, uint16_t c, uint8_t a, bool sb)
{
    WRAP_ROW(__real_glass_text_row, row, y, lo, hi, f, t, n, x0, yt, tr, c, a, sb);
}
void __real_glass_disc_row(uint16_t *, int, int, int, int32_t, int32_t, int32_t, uint16_t, uint8_t, bool);
void __wrap_glass_disc_row(uint16_t *row, int y, int lo, int hi, int32_t cx, int32_t cy, int32_t r,
                           uint16_t c, uint8_t a, bool sb)
{
    WRAP_ROW(__real_glass_disc_row, row, y, lo, hi, cx, cy, r, c, a, sb);
}
void __real_glass_icon_row(uint16_t *, int, int, int, glass_icon_id_t, glass_icon_size_t, int, int,
                           uint16_t, uint8_t, bool);
void __wrap_glass_icon_row(uint16_t *row, int y, int lo, int hi, glass_icon_id_t id, glass_icon_size_t sz,
                           int x0, int yt, uint16_t c, uint8_t a, bool sb)
{
    WRAP_ROW(__real_glass_icon_row, row, y, lo, hi, id, sz, x0, yt, c, a, sb);
}
#endif

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

static bool has_flag(int argc, char **argv, const char *f)
{
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], f) == 0) return true;
    }
    return false;
}

static const char *flag_val(int argc, char **argv, const char *key)
{
    const size_t n = strlen(key);
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], key, n) == 0) return argv[i] + n;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *scene = argc > 1 ? argv[1] : "shade";
    const bool muted = has_flag(argc, argv, "muted");
    const bool low = has_flag(argc, argv, "low");
    const bool nogauge = has_flag(argc, argv, "nogauge");
    const bool parked = has_flag(argc, argv, "parked");
    const char *ent_arg = flag_val(argc, argv, "ent=");
    const char *dir = flag_val(argc, argv, "dir=");
    const char *caption = flag_val(argc, argv, "caption=");
    const char *sky_arg = flag_val(argc, argv, "sky=");     /* JR_DISPLAY_SKY_* as a number */
    const char *hour_arg = flag_val(argc, argv, "hour=");   /* the clock's hour: 23 is night */
    const char *vol_arg = flag_val(argc, argv, "vol=");
    const char *light_arg = flag_val(argc, argv, "light=");
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
    } else if (strcmp(scene, "jarvis") == 0) {
        space = JR_DISPLAY_SPACE_JARVIS;
        overlay = JR_DISPLAY_OVERLAY_NONE;
    } else if (strcmp(scene, "shade") != 0) {
        fprintf(stderr, "unknown scene %s\n", scene);
        return 2;
    }

    s_space_on = space != JR_DISPLAY_SPACE_JARVIS;
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
    jr_display_set_brightness(light_arg ? (uint8_t)atoi(light_arg) : 60U);
    jr_display_set_status(vol_arg ? (uint8_t)atoi(vol_arg) : 90U);
    if (nogauge) {
        jr_display_power_set(255U, 0U, true, false);
    } else if (low) {
        jr_display_power_set(12U, 3500U, false, false);
    } else {
        jr_display_power_set(74U, 4020U, true, true);
    }
    {
        jr_display_links_t l;
        memset(&l, 0, sizeof l);
        l.wifi_up = true;
        l.rssi_dbm = -52;
        l.link_open = true;
        l.tools = 2U;
        l.chip_c = 52;
        l.chip_c_valid = true;
        l.cpu_mhz = 240;
        jr_display_links_set(&l);
    }
    jr_display_clock_set(true, hour_arg ? atoi(hour_arg) : 14, 5, 0);
    jr_display_clock_set_date(3, 23, 8);
    jr_display_sun_set(7 * 60 + 2, 19 * 60 + 35);
#ifndef NO_ENTRY
    s_entry_ms = ent_arg ? (uint32_t)atoi(ent_arg) : SP_ENTRY_TOTAL_MS;
#else
    (void)ent_arg;
#endif
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
        w.sky = sky_arg ? (jr_display_sky_t)atoi(sky_arg) : JR_DISPLAY_SKY_CLEAR;
        strcpy(w.condition, w.sky == JR_DISPLAY_SKY_PARTLY ? "PARTLY CLOUDY" : w.sky == JR_DISPLAY_SKY_RAIN ? "RAIN" : w.sky == JR_DISPLAY_SKY_STORM ? "THUNDER" : w.sky == JR_DISPLAY_SKY_SNOW ? "SNOW" : w.sky == JR_DISPLAY_SKY_FOG ? "FOG" : w.sky == JR_DISPLAY_SKY_CLOUDS ? "OVERCAST" : w.sky == JR_DISPLAY_SKY_UNKNOWN ? "WINDY" : (hour_arg && atoi(hour_arg) >= 20 ? "CLEAR NIGHT" : "CLEAR"));
        w.fetched_ms = 0;
        static const uint8_t rain[12] = {100, 70, 40, 15, 0, 0, 5, 0, 30, 60, 85, 20};
        memcpy(&w.rain_hours[14], rain, sizeof rain);
        w.rain_hours_mday = 23;
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
    if (caption) {
        /* "caption=A_B" prints "A B": the transient label, fully faded in */
        char text[64];
        snprintf(text, sizeof text, "%s", caption);
        for (char *c = text; *c; c++) if (*c == '_') *c = ' ';
        jr_display_caption_set(text);
        s_caption_ease = s_caption_prog = 256;
    }
    if (parked) {
        /* A data screen settled on true black: the face is not decoded, the
         * engine's background is black, and the veil fold is skipped. */
        s_space_black = 256;
        s_parked = true;
    }

    uint16_t *fb = malloc(466u * 466u * sizeof(uint16_t));
    uint16_t *ref = malloc(466u * 466u * sizeof(uint16_t));
    for (int y = 0; y < 466; y++)
        for (int x = 0; x < 466; x++)
            fb[y * 466 + x] = ref[y * 466 + x] = parked ? 0U : face_px(x, y);

    /* The screen's own cost: one frame with the veil off, counted against the
     * bare face. Pixels written, the proxy for per-frame work on the S3. */
    {
        const int veil = s_space_veil;
        s_space_veil = 0;
        uint16_t *cnt = malloc(466u * 466u * sizeof(uint16_t));
        memcpy(cnt, ref, 466u * 466u * sizeof(uint16_t));
        for (int y = 0; y < 466; y += 12) {
            int y2 = y + 12 > 466 ? 466 : y + 12;
            apply_space_overlay(&s_display, y, y2, cnt + (size_t)y * 466);
        }
        long changed = 0;
        for (int i = 0; i < 466 * 466; i++) changed += cnt[i] != ref[i];
        /* writes, overdraw included, minus the orbit rail every shell screen
         * pays for alone */
        memcpy(cnt, ref, 466u * 466u * sizeof(uint16_t));
        g_wr = 0;
        for (int y = 0; y < 466; y += 12) {
            int y2 = y + 12 > 466 ? 466 : y + 12;
            apply_space_overlay(&s_display, y, y2, cnt + (size_t)y * 466);
        }
        const long all = g_wr;
        g_wr = 0;
        for (int y = 0; y < 466; y += 12) {
            int y2 = y + 12 > 466 ? 466 : y + 12;
            sp_draw_orbit(&s_display, y, y2, cnt + (size_t)y * 466, 255);
        }
        printf("pixels changed: %ld, writes: %ld (orbit rail alone: %ld, screen: %ld)\n", changed,
               all, g_wr, all - g_wr);
        free(cnt);
        s_space_veil = veil;
    }

    /* Compose in 12-row strips exactly like the flush does. */
    for (int y = 0; y < 466; y += 12) {
        int y2 = y + 12 > 466 ? 466 : y + 12;
        apply_space_overlay(&s_display, y, y2,
                            fb + (size_t)y * 466);
        apply_ota_ring(&s_display, y, y2, fb + (size_t)y * 466);
        if (caption) apply_caption_overlay(&s_display, y, y2, fb + (size_t)y * 466);
    }

    char name[96];
    const char *tag = flag_val(argc, argv, "tag=");
    snprintf(name, sizeof name, "%s%sprobe_%s%s%s%s%s%s%s%s%s%s.ppm", dir ? dir : "", dir ? "/" : "", scene,
             caption ? "_cap" : "", parked ? "_parked" : "", muted ? "_muted" : "", low ? "_low" : "", nogauge ? "_nogauge" : "",
             ent_arg ? "_ent" : "", ent_arg ? ent_arg : "", tag ? "_" : "", tag ? tag : "");
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
