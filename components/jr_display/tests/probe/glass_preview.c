/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glass_preview: the production compositor, played through time on the host.
 *
 * It #includes jr_display.c over the same stubs the frame tests use, starts
 * from one of frame_scenes.inc's complete device states, then plays a script
 * of timed steps: input (swipes, taps, the control shade), state changes
 * (battery, agent rim, mute) and waits. Time is the stub clock, advanced one
 * frame interval per frame; every transition is the production state machine
 * (nav_step, sp_fade_tick, overlay_fade_tick) reacting to that clock and to
 * the input, never an interpolation made here. Each frame is composed in
 * 12-row strips exactly as panel_flush does and appended to the output as raw
 * 466x466 RGB888.
 *
 * What the host cannot do, and this stands in for:
 *   - the home face is a decoded EAF animation on the device; here it is the
 *     frames of firmware/mascot/reactive/preview/rwave_idle.png (a contact
 *     sheet), cut by scripts/glass-preview.py into bg=DIR/face.rgb565;
 *   - the six baked dials are decoded EAFs; here they are the
 *     dial_*_preview.png files, as bg=DIR/dial_NAME.rgb565;
 *   - input is not routed through main.c (it does not link on the host). The
 *     verbs below call the same jr_display_nav_* / watch / caption functions
 *     main.c calls, in the order main.c calls them; see docs/reference/
 *     glass-preview.md for the mapping and what it leaves out.
 *
 * Usage: glass_preview SCRIPT out=FILE [bg=DIR] [fps=N] [knob=WORD]
 *   knob=WORD  the perf knob word (perf_knob.h) the whole run uses; absent = 0, the shipped path.
 *              With JR_KNOB_PANEL_DIP (1) the panel dip runs: the brightness chain is pumped in 39
 *              sub-steps of every frame, as panel_flush does (one per strip), the frame is dimmed by
 *              the light the glass had over its scan (the mean of f(dip) at those sub-steps), and
 *              "DIP <frame> <ms> <dip> <sent>" lists every brightness write made inside a frame.
 *   stdout: "F <index> <ms> <hash>" per recorded frame, "NOTE ..." lines,
 *           "MARK <frame> <line>" for every script step. exit 2 on a bad script.
 *
 * Script: one step per line, '#' starts a comment.
 *   scene NAME            hard cut to a frame_scenes.inc state (settles 1.5 s unrecorded)
 *   wait MS               play MS of clock, frames recorded
 *   swipe up|down|left|right     the centre-glass swipe, routed as main.c does
 *   tap X Y               jr_display_hit(); the privacy button toggles mute
 *   open shade | close shade     the BOOT-button door into the shade and the way out
 *   home                  double tap: nav_home
 *   set battery N | set charging on|off | set mute on|off | set agent N | set caption TEXT
 */
#include <stdint.h>
#include "jr_display.c"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "idf_stubs.inc"
#include "frame_scenes.inc"

#define PV_PRE_FRAMES 36

static const char *const PV_DIAL_FILE[6] = { "diver", "dress", "pilot", "future", "skeleton", "celestial" };

static uint16_t *s_pv_face;          /* nface frames of 466*466 */
static int s_pv_nface;
static uint16_t *s_pv_dial[6];
static bool s_pv_dial_tried[6];
static int s_pv_fps = 24;
static bool s_pv_note_face, s_pv_note_dial;
static uint16_t *s_pv_black;
static bool s_pv_dip;                /* JR_KNOB_PANEL_DIP: the frames are dimmed by the panel */
static int s_pv_light = 256;         /* the light of the frame being recorded, 0..256 */
static long s_pv_dip_rows;

static int pv_dial_slot(jr_face_t f)
{
    switch (f) {
    case JR_FACE_DIAL_DIVER: return 0;
    case JR_FACE_DIAL_DRESS: return 1;
    case JR_FACE_DIAL_PILOT: return 2;
    case JR_FACE_DIAL_FUTURE: return 3;
    case JR_FACE_DIAL_SKELETON: return 4;
    case JR_FACE_DIAL_CELESTIAL: return 5;
    default: return -1;
    }
}

static uint16_t *pv_load(const char *dir, const char *name, long *bytes)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint16_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *bytes = n;
    return b;
}

static const char *s_pv_bg;

static uint16_t *pv_dial(jr_face_t f)
{
    const int s = pv_dial_slot(f);
    if (s < 0) return NULL;
    if (!s_pv_dial_tried[s]) {
        s_pv_dial_tried[s] = true;
        char name[64];
        long n = 0;
        snprintf(name, sizeof name, "dial_%s.rgb565", PV_DIAL_FILE[s]);
        if (s_pv_bg) s_pv_dial[s] = pv_load(s_pv_bg, name, &n);
        if (s_pv_dial[s] && n != 466L * 466L * 2L) { free(s_pv_dial[s]); s_pv_dial[s] = NULL; }
        if (!s_pv_dial[s]) fprintf(stderr, "preview: no %s, the watch clears its disc instead\n", name);
    }
    return s_pv_dial[s];
}

/* ---- the script ---- */
typedef struct { char verb[16]; char a[96]; int x, y; int line; } pv_step_t;

static int pv_parse(const char *path, pv_step_t **out)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    pv_step_t *st = calloc(2048, sizeof *st);
    int n = 0, ln = 0;
    char buf[256];
    while (fgets(buf, sizeof buf, f)) {
        ln++;
        char *h = strchr(buf, '#');
        if (h) *h = 0;
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == ' ' || p[l - 1] == '\t')) p[--l] = 0;
        if (!l) continue;
        pv_step_t *s = &st[n];
        s->line = ln;
        char rest[160] = "";
        if (sscanf(p, "%15s %159[^\n]", s->verb, rest) < 1) continue;
        snprintf(s->a, sizeof s->a, "%s", rest);
        const char *v = s->verb;
        bool ok = false;
        if (!strcmp(v, "scene")) ok = *rest != 0;
        else if (!strcmp(v, "wait")) ok = atoi(rest) > 0;
        else if (!strcmp(v, "swipe")) ok = !strcmp(rest, "up") || !strcmp(rest, "down") || !strcmp(rest, "left") || !strcmp(rest, "right");
        else if (!strcmp(v, "tap")) ok = sscanf(rest, "%d %d", &s->x, &s->y) == 2;
        else if (!strcmp(v, "open") || !strcmp(v, "close")) ok = !strcmp(rest, "shade");
        else if (!strcmp(v, "home")) ok = true;
        else if (!strcmp(v, "tilt")) ok = !strncmp(rest, "orbit ", 6) || sscanf(rest, "%d %d", &s->x, &s->y) == 2;
        else if (!strcmp(v, "set")) ok = strlen(rest) > 2;
        if (!ok) { fprintf(stderr, "%s:%d: bad step '%s'\n", path, ln, p); free(st); fclose(f); return -1; }
        if (++n >= 2048) break;
    }
    fclose(f);
    *out = st;
    return n;
}

/* ---- state the script owns ---- */
static int s_batt = 74;
static bool s_chg = true, s_mute;
static int64_t s_frame;     /* frames played since the first scene, recorded or not */
static FILE *s_outf;
static long s_recorded;

static void pv_env(void)
{
    jr_display_set_hud_env((uint8_t)s_batt, s_chg, s_mute, 0.0f, 0.0f);
    if (s_batt == 255) jr_display_power_set(255U, 0U, true, false);
    else jr_display_power_set((uint8_t)s_batt, s_batt < 20 ? 3500U : 4020U, true, s_chg);
}

/* main.c's shell_caption_space: the screen's name, as a label (it times itself out). */
static void pv_caption_space(void)
{
    static const char *const nm[JR_DISPLAY_SPACE_COUNT] = { "JARVIS", "", "WEATHER", "STATUS", "DESK", "ACTIVITY" };
    const jr_display_space_t sp = jr_display_nav_space();
    jr_display_caption_label(sp < JR_DISPLAY_SPACE_COUNT ? nm[sp] : "JARVIS", JR_DISPLAY_LABEL_SCREEN);
}

/* main.c's swipe handler for a centre swipe, in its order (overlay branch, then
 * the ring, the watch style, then the caption chain). */
static void pv_swipe(const char *dir)
{
    const bool up = !strcmp(dir, "up"), down = !strcmp(dir, "down");
    const bool horiz = !up && !down;
    const jr_display_overlay_t overlay = jr_display_nav_overlay();
    const jr_display_space_t space_before = jr_display_nav_space();
    bool style_changed = false;
    if (overlay == JR_DISPLAY_OVERLAY_SHADE) {
        if (!horiz) jr_display_nav_up();           /* either vertical leaves the shade */
        /* left/right on the shade moves the volume in production: not modelled */
    } else if (horiz) {
        if (overlay == JR_DISPLAY_OVERLAY_NONE && jr_display_nav_space() == JR_DISPLAY_SPACE_WATCH) {
            jr_display_watch_style_step(!strcmp(dir, "right") ? 1 : -1);
            style_changed = true;
        }
        /* a peek of any other screen arms a 10 s clock in production: not modelled */
    } else if (down) {
        if (overlay == JR_DISPLAY_OVERLAY_DETAIL) jr_display_nav_down();
        else jr_display_nav_next();
    } else {
        jr_display_nav_prev();
    }
    if (style_changed) {
        char cap[24];
        snprintf(cap, sizeof cap, "WATCH - %s", jr_display_watch_style_name(jr_display_watch_style()));
        jr_display_caption_label(cap, JR_DISPLAY_LABEL_WATCH_STYLE);
    } else if (jr_display_nav_space() == space_before && jr_display_nav_overlay() != overlay) {
        /* a sheet or the shade opened or closed over the same screen: each carries
         * its own hint line, and none lingers (main.c: the same condition) */
    } else if (!horiz) {
        pv_caption_space();
    }
}

/* ---- the scripted tilt (tilt parallax) ----
 * `tilt X Y` holds the sensor at (X, Y) milli-g, the in-plane UP vector in
 * screen axes. `tilt orbit PERIOD_MS AMP_MG` walks ONE circle of radius AMP
 * that starts and ends at the pose it was in (the origin), then holds there.
 * The render task asks for it once a frame, exactly as main.c's source does,
 * and shapes it with the real tilt.h. */
static int     s_pv_tx, s_pv_ty;
static bool    s_pv_orbit;
static int64_t s_pv_orbit_t0_us;
static int     s_pv_orbit_ms, s_pv_orbit_amp;

static bool pv_tilt_source(int *x, int *y)
{
    if (s_pv_orbit) {
        const int64_t el = (s_fake_us - s_pv_orbit_t0_us) / 1000;
        if (el >= s_pv_orbit_ms) {
            s_pv_orbit = false;
            s_pv_tx = s_pv_ty = 0;
        } else {
            const double th = 6.283185307179586 * (double)el / (double)s_pv_orbit_ms;
            s_pv_tx = (int)(s_pv_orbit_amp * (cos(th) - 1.0));
            s_pv_ty = (int)(s_pv_orbit_amp * sin(th));
        }
    }
    *x = s_pv_tx;
    *y = s_pv_ty;
    return true;
}

/* ---- one frame ---- */
static uint16_t *s_pv_fb;

#ifdef PV_COST
/* Built by hand with -DPV_COST under callgrind (docs/reference/glass-preview.md):
 * every recorded frame's instruction count is dumped on its own, so the worst
 * frame of a transition is read off, not guessed. */
#include <valgrind/callgrind.h>
#endif

/* panel_flush's pump at `us` of the frame: the brightness chain, and the line of the table when
 * it wrote. Returns the light the glass has after it. */
static int pv_pump(int64_t us, bool record)
{
    const unsigned calls = s_stub_brightness_calls;
    const int64_t keep = s_fake_us;
    s_fake_us = us;
    brightness_pump();
    if (record && s_stub_brightness_calls != calls) {
        printf("DIP %ld %lld %d %u\n", s_recorded, (long long)(us / 1000), s_dip,
               (unsigned)s_stub_panel_brightness);
        s_pv_dip_rows++;
    }
    s_fake_us = keep;
    return dip_light(s_dip);
}

static void pv_frame(int64_t t_us, bool record, uint16_t *fb)
{
    jr_display_ctx_t *ctx = &s_display;
    const int64_t ms = t_us / 1000;
    s_fake_us = t_us;

    const bool watch = jr_display_nav_space() == JR_DISPLAY_SPACE_WATCH;
    if (watch) jr_display_clock_set(true, 10, 8, (int)((ms / 1000) % 60));
    else jr_display_clock_set(false, 0, 0, 0);
#ifdef PV_COST
    if (record) CALLGRIND_ZERO_STATS;
#endif

    /* the presenter binds the dial the compositor asked for on its last frame
     * (watch_publish, via watch_dial_face): one frame of latency, no more */
    const jr_face_t dial = watch_dial_face();
    uint16_t *dbuf = dial != JR_FACE_COUNT ? pv_dial(dial) : NULL;
    if (dbuf) {
        s_dial_frame = dbuf;
        s_dial_frame_face = dial;
        s_dial_blit = true;
        ctx->shown_face = dial;
        s_pv_note_dial = true;
    } else {
        s_dial_blit = false;
        ctx->shown_face = JR_FACE_IDLE;
    }
    s_parked = !s_dial_blit && __atomic_load_n(&s_park_want, __ATOMIC_ACQUIRE) != 0;

    int light_sum = 0, light_n = 0;
    if (s_pv_dip) {
        light_sum += pv_pump(t_us, record);        /* panel_flush: the pump, then the frame-start latch */
        light_n++;
    }
    overlay_fade_tick();

    const uint16_t *face = s_parked || !s_pv_face
        ? s_pv_black
        : s_pv_face + (size_t)(((ms * 24) / 1000) % s_pv_nface) * 466u * 466u;
    if (!s_pv_face) s_pv_note_face = true;
    for (int y = 0; y < 466; y += 12) {
        const int y2 = y + 12 > 466 ? 466 : y + 12;
        uint16_t *p = fb + (size_t)y * 466;
        if (s_pv_dip && y > 0) {
            light_sum += pv_pump(t_us + (int64_t)(1000000LL / s_pv_fps) * (y / 12) / 39, record);
            light_n++;
        }
        memcpy(p, face + (size_t)y * 466, (size_t)(y2 - y) * 466u * sizeof(uint16_t));
        (void)rest_eligible(ctx);
        apply_dial(ctx, 0, y, 466, y2, p);
        apply_hud_overlay(ctx, 0, y, 466, y2, p);
        apply_shell_overlay(ctx, 0, y, 466, y2, p);
        apply_surface_overlay(ctx, 0, y, 466, y2, p);
    }
    if (s_pv_dip) {
        dip_frame_done();                /* panel_flush: the last strip of the frame is submitted */
        s_pv_light = light_n > 0 ? light_sum / light_n : 256;
        static uint32_t last_hold;
        if (record && s_dip_hold_ms != last_hold) {
            printf("HOLD %ld %u\n", s_recorded, (unsigned)s_dip_hold_ms);   /* the dark's length, as dip_hold_ms reports it */
            last_hold = s_dip_hold_ms;
        }
    }
    s_frame++;
    if (!record) return;
#ifdef PV_COST
    {
        char nm[32];
        snprintf(nm, sizeof nm, "f%ld", s_recorded);
        CALLGRIND_DUMP_STATS_AT(nm);
    }
#endif
    static unsigned char *rgb;
    if (!rgb) rgb = malloc(466u * 466u * 3u);
    for (int i = 0; i < 466 * 466; i++) {
        const uint16_t v = fb[i];
        rgb[i * 3 + 0] = (unsigned char)((((v >> 11) & 31) * 255 / 31) * s_pv_light / 256);
        rgb[i * 3 + 1] = (unsigned char)((((v >> 5) & 63) * 255 / 63) * s_pv_light / 256);
        rgb[i * 3 + 2] = (unsigned char)(((v & 31) * 255 / 31) * s_pv_light / 256);
    }
    fwrite(rgb, 1, 466u * 466u * 3u, s_outf);
    printf("F %ld %lld %016llx %d\n", s_recorded, (long long)ms, (unsigned long long)fc_hash(fb), s_pv_light);
    s_recorded++;
}

static int64_t pv_t_of(int64_t frame) { return frame * 1000000LL / s_pv_fps; }

static void pv_scene(const char *name)
{
    const fc_scene_t *sc = NULL;
    for (int i = 0; i < FC_NSCENES; i++) if (!strcmp(FC_SCENES[i].name, name)) sc = &FC_SCENES[i];
    if (!sc) { fprintf(stderr, "unknown scene %s (probe frame list shows them)\n", name); exit(2); }
    const int64_t keep = s_fake_us;
    fc_setup(sc);
    s_fake_us = keep;
    s_fade_prev_ms = (uint32_t)(keep / 1000);
    s_batt = sc->batt;
    s_chg = sc->charging;
    s_mute = sc->muted;
    pv_env();
    s_pv_dip = s_dip_on;
    if (s_pv_dip) {                      /* the mood at full light, the panel at rest */
        jr_display_set_brightness(100U);
        atomic_store(&s_brightness_shown, 100);
        atomic_store(&s_brightness_have, 100U);
        s_stub_panel_brightness = 100U;
    }
    s_pv_light = 256;
    for (int i = 0; i < PV_PRE_FRAMES; i++) pv_frame(pv_t_of(s_frame), false, s_pv_fb);
}

int main(int argc, char **argv)
{
    const char *script = argc > 1 ? argv[1] : NULL;
    const char *outp = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4)) outp = argv[i] + 4;
        else if (!strncmp(argv[i], "bg=", 3)) s_pv_bg = argv[i] + 3;
        else if (!strncmp(argv[i], "fps=", 4)) s_pv_fps = atoi(argv[i] + 4);
        else if (!strncmp(argv[i], "knob=", 5)) fc_knob_extra = (uint32_t)strtoul(argv[i] + 5, NULL, 0);
    }
    if (!script || !outp || s_pv_fps < 2 || s_pv_fps > 24) {
        fprintf(stderr, "usage: glass_preview SCRIPT out=FILE [bg=DIR] [fps=2..24] [knob=WORD]\n");
        return 2;
    }
    pv_step_t *st;
    const int n = pv_parse(script, &st);
    if (n < 0) return 2;
    if (n == 0 || strcmp(st[0].verb, "scene") != 0) { fprintf(stderr, "a script starts with 'scene NAME'\n"); return 2; }

    jr_display_set_tilt_source(pv_tilt_source);
    s_pv_black = calloc(466u * 466u, sizeof(uint16_t));
    s_pv_fb = malloc(466u * 466u * sizeof(uint16_t));
    if (s_pv_bg) {
        long bytes = 0;
        s_pv_face = pv_load(s_pv_bg, "face.rgb565", &bytes);
        if (s_pv_face) s_pv_nface = (int)(bytes / (466L * 466L * 2L));
        if (s_pv_nface < 1) { free(s_pv_face); s_pv_face = NULL; }
    }
    if (!s_pv_face) fprintf(stderr, "preview: no face.rgb565, the home face is black\n");
    s_outf = fopen(outp, "wb");
    if (!s_outf) { fprintf(stderr, "cannot write %s\n", outp); return 2; }

    int64_t frames_left_carry = 0;
    (void)frames_left_carry;
    for (int i = 0; i < n; i++) {
        const pv_step_t *s = &st[i];
        printf("MARK %ld %d\n", s_recorded, s->line);
        if (!strcmp(s->verb, "scene")) {
            pv_scene(s->a);
        } else if (!strcmp(s->verb, "wait")) {
            const int ms = atoi(s->a);
            const int64_t t0 = pv_t_of(s_frame);
            /* frames whose clock falls inside [t0, t0 + ms) */
            while (pv_t_of(s_frame) < t0 + (int64_t)ms * 1000) pv_frame(pv_t_of(s_frame), true, s_pv_fb);
        } else if (!strcmp(s->verb, "swipe")) {
            pv_swipe(s->a);
            printf("NOTE swipe %s -> space %d overlay %d style %d\n", s->a, (int)jr_display_nav_space(),
                   (int)jr_display_nav_overlay(), (int)jr_display_watch_style());
        } else if (!strcmp(s->verb, "tap")) {
            const jr_display_action_t a = jr_display_hit(s->x, s->y);
            if (a == JR_DISPLAY_ACT_PRIVACY_TOGGLE) { s_mute = !s_mute; pv_env(); }
            printf("NOTE tap %d %d -> action %d\n", s->x, s->y, (int)a);
        } else if (!strcmp(s->verb, "open")) {
            jr_display_nav_down();     /* the shade names its own way out: no caption */
        } else if (!strcmp(s->verb, "close")) {
            jr_display_nav_up();       /* and its leaving is the message */
        } else if (!strcmp(s->verb, "home")) {
            jr_display_nav_home();
            pv_caption_space();
        } else if (!strcmp(s->verb, "tilt")) {
            if (!strncmp(s->a, "orbit ", 6)) {
                sscanf(s->a + 6, "%d %d", &s_pv_orbit_ms, &s_pv_orbit_amp);
                s_pv_orbit_t0_us = s_fake_us;
                s_pv_orbit = s_pv_orbit_ms > 0;
                s_pv_tx = s_pv_ty = 0;
            } else {
                s_pv_orbit = false;
                s_pv_tx = s->x;
                s_pv_ty = s->y;
            }
            printf("NOTE tilt %s\n", s->a);
        } else if (!strcmp(s->verb, "set")) {
            char k[24] = "", v[96] = "";
            sscanf(s->a, "%23s %95[^\n]", k, v);
            if (!strcmp(k, "battery")) { s_batt = atoi(v); pv_env(); }
            else if (!strcmp(k, "charging")) { s_chg = !strcmp(v, "on"); pv_env(); }
            else if (!strcmp(k, "mute")) { s_mute = !strcmp(v, "on"); pv_env(); }
            else if (!strcmp(k, "agent")) {
                const int a = atoi(v);
                s_display.shell_word = a > 0
                    ? (JR_DISPLAY_SHELL_AGENT |
                       ((uint32_t)JR_DISPLAY_AGENT_WORKING << JR_DISPLAY_SHELL_STATE_SHIFT) | (uint32_t)a)
                    : 0U;
            } else if (!strcmp(k, "caption")) jr_display_caption_set(v);
            else { fprintf(stderr, "%s:%d: unknown 'set %s'\n", script, s->line, k); return 2; }
        }
    }
    fclose(s_outf);
    printf("MARK %ld 0\n", s_recorded);
    if (s_pv_note_face || !s_pv_face)
        printf("NOTE face: black (no face.rgb565)\n");
    else
        printf("NOTE face: stand-in, %d frames cut from the rwave_idle contact sheet, not the decoded EAF\n", s_pv_nface);
    if (s_pv_note_dial)
        printf("NOTE dials: stand-in, the dial_*_preview.png art, not the decoded EAF, hands drawn by the real compositor\n");
    if (s_pv_dip)
        printf("NOTE panel dip: %ld brightness writes inside recorded frames; frames are dimmed by the mean light over their scan, not by what the CO5300 does with a 0x51 write in mid-scan\n", s_pv_dip_rows);
    printf("DONE frames=%ld fps=%d\n", s_recorded, s_pv_fps);
    return 0;
}
