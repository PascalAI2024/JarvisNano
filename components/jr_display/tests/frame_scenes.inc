/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Whole-frame scenes for the frame-cost work. Included (not linked) by the
 * visual probe (`probe frame`, the callgrind driver) and by
 * test_frame_identity.c, AFTER jr_display.c and idf_stubs.inc, so both see the
 * same statics. One scene is a complete device state; fc_frame() composes one
 * whole frame exactly as panel_flush does: the frame-start latch
 * (overlay_fade_tick), then per 12-row strip the HUD layer, the shell rim and
 * the surface layer. The dial blit and the canvas are not here: they are
 * memcpy from PSRAM on the device and nothing on the host stands in for that.
 */
#ifndef FRAME_SCENES_INC
#define FRAME_SCENES_INC

typedef struct {
    const char *name;
    int space;        /* JR_DISPLAY_SPACE_* */
    int overlay;      /* JR_DISPLAY_OVERLAY_* */
    int style;        /* JR_WATCH_*, WATCH only */
    int batt;         /* percent, 255 = no gauge */
    bool charging;
    bool muted;
    int entry_ms;     /* -1 = settled, else the data screen's entry clock */
    int shade_ms;     /* -1 = settled */
    bool ota;
    int watch_in;     /* WATCH only: how far the slide in from home has run at the frame start, 1..255, 0 = settled */
    int agent;        /* agent rim progress percent, 0 = no agent rim (the shell word's AGENT bit) */
    int lease;        /* an operator lease caption (CODEX MODE - DOUBLE TAP TO EXIT) is up, settled */
    int dim;          /* the brightness the mood ladder has dimmed the panel to, percent; 0 = 60 */
} fc_scene_t;

#define FC_SETTLED (-1)

static const fc_scene_t FC_SCENES[] = {
    { "home",          JR_DISPLAY_SPACE_JARVIS,  JR_DISPLAY_OVERLAY_NONE,   0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "watch-jarvis",  JR_DISPLAY_SPACE_WATCH,   JR_DISPLAY_OVERLAY_NONE,   JR_WATCH_JARVIS,  74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "watch-diver",   JR_DISPLAY_SPACE_WATCH,   JR_DISPLAY_OVERLAY_NONE,   JR_WATCH_DIVER,   74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "watch-dress",   JR_DISPLAY_SPACE_WATCH,   JR_DISPLAY_OVERLAY_NONE,   JR_WATCH_DRESS,   74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "watch-pilot",   JR_DISPLAY_SPACE_WATCH,   JR_DISPLAY_OVERLAY_NONE,   JR_WATCH_PILOT,   74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "watch-minimal", JR_DISPLAY_SPACE_WATCH,   JR_DISPLAY_OVERLAY_NONE,   JR_WATCH_MINIMAL, 74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "watch-future",  JR_DISPLAY_SPACE_WATCH,   JR_DISPLAY_OVERLAY_NONE,   JR_WATCH_FUTURE,  74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "watch-skeleton",JR_DISPLAY_SPACE_WATCH,   JR_DISPLAY_OVERLAY_NONE,   JR_WATCH_SKELETON,74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "watch-celestial",JR_DISPLAY_SPACE_WATCH,  JR_DISPLAY_OVERLAY_NONE,   JR_WATCH_CELESTIAL,74,true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "watch-jarvis-in",  JR_DISPLAY_SPACE_WATCH, JR_DISPLAY_OVERLAY_NONE,  JR_WATCH_JARVIS, 150, true,  false, FC_SETTLED, FC_SETTLED, false, 60, 0, 0, 0 },
    { "watch-minimal-in", JR_DISPLAY_SPACE_WATCH, JR_DISPLAY_OVERLAY_NONE,  JR_WATCH_MINIMAL, 150, true,  false, FC_SETTLED, FC_SETTLED, false, 140, 0, 0, 0 },
    { "watch-dress-in",   JR_DISPLAY_SPACE_WATCH, JR_DISPLAY_OVERLAY_NONE,  JR_WATCH_DRESS, 150, true,  false, FC_SETTLED, FC_SETTLED, false, 100, 0, 0, 0 },
    { "weather",       JR_DISPLAY_SPACE_WEATHER, JR_DISPLAY_OVERLAY_NONE,   0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "weather-ent",   JR_DISPLAY_SPACE_WEATHER, JR_DISPLAY_OVERLAY_NONE,   0,                74, true,  false, 180,        FC_SETTLED, false, 0, 0, 0, 0 },
    { "status-5",      JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_NONE,   0,                5,  false, false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "status-74",     JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_NONE,   0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "status-100",    JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_NONE,   0,                100,false, false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "status-chg",    JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_NONE,   0,                74, true,  true,  FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "status-ent",    JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_NONE,   0,                74, true,  false, 180,        FC_SETTLED, false, 0, 0, 0, 0 },
    { "desk",          JR_DISPLAY_SPACE_DESK,    JR_DISPLAY_OVERLAY_NONE,   0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "activity",      JR_DISPLAY_SPACE_ACTIVITY,JR_DISPLAY_OVERLAY_NONE,   0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "shade",         JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_SHADE,  0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "detail",        JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_DETAIL, 0,                74, true,  false, FC_SETTLED, FC_SETTLED, true,  0, 0, 0, 0 },
    /* the recorded shapes' edges: the ring at a sliver and at nearly full, the desk ring and the shade part-way through their entry */
    { "status-1",      JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_NONE,   0,                1,  false, false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "status-99",     JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_NONE,   0,                99, false, false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 0 },
    { "desk-ent",      JR_DISPLAY_SPACE_DESK,    JR_DISPLAY_OVERLAY_NONE,   0,                74, true,  false, 400,        FC_SETTLED, false, 0, 0, 0, 0 },
    { "shade-ent",     JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_SHADE,  0,                74, true,  false, FC_SETTLED, 300,        false, 0, 0, 0, 0 },
    /* the agent rim (apply_shell_overlay): full, and part-way (4 of 8 sectors), over home and a ring screen */
    { "home-agent-100",   JR_DISPLAY_SPACE_JARVIS,  JR_DISPLAY_OVERLAY_NONE, 0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 100, 0, 0 },
    { "home-agent-40",    JR_DISPLAY_SPACE_JARVIS,  JR_DISPLAY_OVERLAY_NONE, 0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 40, 0, 0 },
    { "status-agent-100", JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_NONE, 0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 100, 0, 0 },
    { "status-agent-60",  JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_NONE, 0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 60, 0, 0 },
    { "desk-agent-13",    JR_DISPLAY_SPACE_DESK,    JR_DISPLAY_OVERLAY_NONE, 0,                74, true,  false, FC_SETTLED, FC_SETTLED, false, 0, 13, 0, 0 },
    /* the lease caption over the home face, over the control shade and over the detail sheet */
    { "home-lease",   JR_DISPLAY_SPACE_JARVIS,  JR_DISPLAY_OVERLAY_NONE,   0, 74, true, false, FC_SETTLED, FC_SETTLED, false, 0, 0, 1, 0 },
    { "shade-lease",  JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_SHADE,  0, 74, true, false, FC_SETTLED, FC_SETTLED, false, 0, 0, 1, 0 },
    { "detail-lease", JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_DETAIL, 0, 74, true, false, FC_SETTLED, FC_SETTLED, true,  0, 0, 1, 0 },
    /* the control shade while the mood ladder has the panel dimmed (the owner's setting is 80) */
    { "shade-dimmed", JR_DISPLAY_SPACE_STATUS,  JR_DISPLAY_OVERLAY_SHADE,  0, 74, true, false, FC_SETTLED, FC_SETTLED, false, 0, 0, 0, 30 },
};
#define FC_NSCENES ((int)(sizeof FC_SCENES / sizeof FC_SCENES[0]))

static int fc_isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* The stand-in for the decoded face / baked dial behind the shell. */
static uint16_t fc_face_px(int x, int y)
{
    const int dx = x - 232, dy = y - 232;
    const int r2 = dx * dx + dy * dy;
    if (r2 > 232 * 232) return 0;
    const int r = fc_isqrt(r2);
    int lum = 0;       /* black between the rings, like the baked art */
    if ((r >= 60 && r <= 66) || (r >= 125 && r <= 134) ||
        (r >= 155 && r <= 179) || (r >= 200 && r <= 214)) lum = 180;
    return (uint16_t)((((lum * 63) / 255) << 5) | ((lum * 31) / 255));
}

static void fc_fill_face(uint16_t *fb)
{
    static uint16_t face[466 * 466];
    static bool built;
    if (!built) {
        for (int y = 0; y < 466; y++)
            for (int x = 0; x < 466; x++) face[y * 466 + x] = fc_face_px(x, y);
        built = true;
    }
    memcpy(fb, face, sizeof face);
}

static jr_face_t fc_dial_of_style(int style)
{
    switch (style) {
    case JR_WATCH_DIVER:     return JR_FACE_DIAL_DIVER;
    case JR_WATCH_DRESS:     return JR_FACE_DIAL_DRESS;
    case JR_WATCH_PILOT:     return JR_FACE_DIAL_PILOT;
    case JR_WATCH_FUTURE:    return JR_FACE_DIAL_FUTURE;
    case JR_WATCH_SKELETON:  return JR_FACE_DIAL_SKELETON;
    case JR_WATCH_CELESTIAL: return JR_FACE_DIAL_CELESTIAL;
    default:                 return JR_FACE_COUNT;
    }
}

/* Put the render side into the scene's settled state. The clock-dependent
 * parts (seconds hand, entry animation, light) are set per frame by fc_frame. */
#ifdef JR_ORBIT_REC
/* Whether the recorded orbit rail is in use (production: yes). The identity
 * suite runs every scene with it and a second time without. */
static bool fc_orbit_rec_on = true;
#endif

#ifdef JR_FRING_REC
/* Whether the recorded battery / desk ring is in use (production: yes). */
static bool fc_fring_rec_on = true;
#endif
#ifdef JR_DIAL_REC
/* Whether the recorded MINIMAL dial disc is in use (production: yes). */
static bool fc_dial_rec_on = true;
#endif
#ifdef JR_SHADE_REC
/* Whether the recorded control-shade arcs are in use (production: yes). */
static bool fc_shade_rec_on = true;
#endif

static void fc_setup(const fc_scene_t *sc)
{
#ifdef JR_DIAL_REC
    if (fc_dial_rec_on) {
        dial_rec_alloc();
    } else {
        hud_dial_cache_set(NULL, 0);
    }
#endif
#ifdef JR_SHADE_REC
    if (fc_shade_rec_on) {
        shade_rec_alloc();
    } else if (s_shade_rec != NULL) {
        heap_caps_free(s_shade_rec);
        s_shade_rec = NULL;
    }
#endif
#ifdef JR_FRING_REC
    if (fc_fring_rec_on) {
        fring_rec_alloc();
    } else if (s_fring_rec != NULL) {
        heap_caps_free(s_fring_rec);
        s_fring_rec = NULL;
    }
#endif
#ifdef JR_ORBIT_REC
    if (fc_orbit_rec_on) {
        orbit_rec_build();
    } else if (s_orbit_rec != NULL) {
        heap_caps_free(s_orbit_rec);
        s_orbit_rec = NULL;
    }
#endif
    s_display.board.width = 466;
    s_display.board.height = 466;
    s_display.board.swap_color_bytes = false;
    s_fake_us = 0;

    const int space = sc->space, overlay = sc->overlay;
    s_space_on = space != JR_DISPLAY_SPACE_JARVIS;
    s_space_from = (uint8_t)space;
    s_space_to = (uint8_t)space;
    s_space_prog = 256;
    s_space_ease = 256;
    s_space_veil = 256;
    s_space_black = 0;
    s_parked = false;
    s_shade_ease = overlay == JR_DISPLAY_OVERLAY_SHADE ? 256 : 0;
    s_detail_ease = overlay == JR_DISPLAY_OVERLAY_DETAIL ? 256 : 0;
    s_detail_space = (uint8_t)space;
    /* the sheet and the shade are SETTLED open: the tick eases toward the nav
     * word's overlay from these, so a zero here would render a half-open one */
    s_detail_prog = overlay == JR_DISPLAY_OVERLAY_DETAIL ? 256 : 0;
    s_shade_prog = overlay == JR_DISPLAY_OVERLAY_SHADE ? 256 : 0;
    s_shade_was_open = overlay == JR_DISPLAY_OVERLAY_SHADE;
    s_nav_word = (uint32_t)space | ((uint32_t)overlay << NAV_OVL_SHIFT) |
                 ((uint32_t)sc->style << NAV_STYLE_SHIFT);
    s_hud_env_word = (uint32_t)sc->batt | (sc->charging ? (1U << 8) : 0U) |
                     (sc->muted ? (1U << 9) : 0U);
    s_orbit_a16 = sp_orbit_angle(space, false) * 16;
    s_clock_ease = 0;
    s_clock_prog = 0;
    s_style_shown = s_style_seen = s_style_from = (uint8_t)sc->style;
    s_style_prog = 256;
    s_style_black = 0;
    s_watch_s = 0;
    s_dial_want = (uint32_t)JR_FACE_COUNT;
    s_caption_ease = s_caption_prog = 0;
    s_cap_p = 256;
    s_cap_dip = s_cap_pend = false;
    s_cap_hold_ms = 0U;
    s_cap_alpha = 0;
    jr_display_caption_clear();         /* a lease scene before this one must not leak its caption */
    s_ask_ease = 0;
    s_canvas_ease = 0;
    s_display.shown_face = space == JR_DISPLAY_SPACE_WATCH
                               ? fc_dial_of_style(sc->style) : JR_FACE_RESTING;
    if (s_display.shown_face == JR_FACE_COUNT) s_display.shown_face = JR_FACE_RESTING;
    jr_display_set_brightness(sc->dim ? (uint8_t)sc->dim : 60U);
    jr_display_set_status(90U, sc->dim ? 80U : 0U);   /* the dimmed scene's owner setting is 80 */
    if (sc->batt == 255) {
        jr_display_power_set(255U, 0U, true, false);
    } else {
        jr_display_power_set((uint8_t)sc->batt, 4020U, true, sc->charging);
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
    jr_display_clock_set_date(3, 23, 8);
    jr_display_sun_set(7 * 60 + 2, 19 * 60 + 35);
    jr_display_jarvis_set_session(true, 12U, 3725U);
    jr_display_ota_set(sc->ota ? JR_DISPLAY_OTA_RECEIVING : JR_DISPLAY_OTA_IDLE,
                       63U, 0U, sc->ota ? 1U : 0xFFU, true);
    {
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
        static const uint8_t rain[12] = {100, 70, 40, 15, 0, 0, 5, 0, 30, 60, 85, 20};
        memcpy(&w.rain_hours[14], rain, sizeof rain);
        w.rain_hours_mday = 23;
        jr_display_weather_set(&w);
    }
    jr_display_activity_push("WEATHER", "83 CLEAR FORT LAUDERDALE");
    jr_display_activity_push("WEB", "FOUND THREE RESULTS");
    jr_display_activity_push("SAID", "VOLUME IS NOW FORTY");
    jr_display_desk_set_task("FIX LOGIN BUG", 68U, JR_DISPLAY_AGENT_WORKING);
    /* the agent rim is the shell word's own bits; set directly so a scene never
     * inherits the previous one's (the nav strand in set_shell_state is not wanted) */
    s_display.shell_word = sc->agent > 0
        ? (JR_DISPLAY_SHELL_AGENT |
           ((uint32_t)JR_DISPLAY_AGENT_WORKING << JR_DISPLAY_SHELL_STATE_SHIFT) |
           (uint32_t)sc->agent)
        : 0U;
    if (sc->lease) {
        jr_display_caption_set("CODEX MODE - DOUBLE TAP TO EXIT");
        s_caption_prog = s_caption_ease = 256;      /* settled, as a held lease is */
    }
    sp_compose();
}

/* One frame at clock `ms`: the body of panel_flush's non-rest branch for the
 * three software layers, whole-frame, strips of 12 rows. */
static inline __attribute__((always_inline)) void fc_frame_body(const fc_scene_t *sc, int64_t ms,
                                                                 uint16_t *fb)
{
    jr_display_ctx_t *ctx = &s_display;
    /* inputs that move with the clock */
    s_fake_us = ms * 1000;
    jr_display_clock_set(true, 10, 8, (int)((ms / 1000) % 60));
    s_clock_set_ms = (uint32_t)((ms / 1000) * 1000);
    if (sc->space == JR_DISPLAY_SPACE_WATCH) {
        s_clock_prog = 256;
        if (sc->watch_in > 0) {
            /* a slide from home into WATCH, watch_in of 256 along when the frame starts */
            s_space_from = (uint8_t)JR_DISPLAY_SPACE_JARVIS;
            s_space_prog = sc->watch_in;
            s_space_dir = 1;
        }
    }
    s_entry_ms = sc->entry_ms < 0 ? SP_ENTRY_TOTAL_MS : (uint32_t)sc->entry_ms;
    s_shade_ms = sc->shade_ms < 0 ? SP_SHADE_ENTRY_MS : (uint32_t)sc->shade_ms;
    s_breath_ms = (uint32_t)(ms % 3000);
    s_fade_prev_ms = (uint32_t)ms - 42u;

    overlay_fade_tick();
    for (int y = 0; y < 466; y += 12) {
        const int y2 = y + 12 > 466 ? 466 : y + 12;
        uint16_t *p = fb + (size_t)y * 466;
        (void)rest_eligible(ctx);
        apply_hud_overlay(ctx, 0, y, 466, y2, p);
        apply_shell_overlay(ctx, 0, y, 466, y2, p);
        apply_surface_overlay(ctx, 0, y, 466, y2, p);
    }
}

static volatile int fc_warm_count;

/* The measured frame. callgrind --toggle-collect=fc_frame_measured counts
 * exactly this call and everything it calls. */
__attribute__((noinline)) static void fc_frame_measured(const fc_scene_t *sc, int64_t ms,
                                                       uint16_t *fb)
{
    fc_frame_body(sc, ms, fb);
}

__attribute__((noinline)) static void fc_frame_warm(const fc_scene_t *sc, int64_t ms, uint16_t *fb)
{
    fc_warm_count++;      /* keeps the compiler from folding this into fc_frame_measured */
    fc_frame_body(sc, ms, fb);
}

/* Set by the profiler, not by the hash suite: one more warm frame, because a
 * recorded shape (fring_prepare, shade_rec_get) waits for a value to hold for
 * a second frame and the measured frame should be the steady state, not the
 * one that records. The hash suite keeps one, which also checks that the frame
 * that records is the same pixels. (A second warm frame would move the hashes
 * of scenes caught mid-ease, which are pinned at one.) */
static bool fc_extra_warm;

/* Warm frames one period (and, for the profiler, two) earlier (the steady state the device runs in: caches
 * filled, eases settled), then the frame under test. */
static void fc_run(const fc_scene_t *sc, int64_t ms, uint16_t *fb)
{
    if (fc_extra_warm) {
        fc_fill_face(fb);
        fc_frame_warm(sc, ms - 84, fb);
    }
    fc_fill_face(fb);
    fc_frame_warm(sc, ms - 42, fb);
    fc_fill_face(fb);
    fc_frame_measured(sc, ms, fb);
}

static uint64_t fc_hash(const uint16_t *fb)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 466 * 466; i++) {
        h ^= (uint64_t)(fb[i] & 0xff); h *= 1099511628211ULL;
        h ^= (uint64_t)(fb[i] >> 8);   h *= 1099511628211ULL;
    }
    return h;
}

#endif
