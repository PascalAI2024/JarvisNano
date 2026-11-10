/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_frame_identity.c — the whole frame, byte for byte.
 *
 * WHY A WHOLE-FRAME HASH. This repo normally avoids pinning frame hashes: they
 * break on every deliberate look change and say nothing about why. This table
 * exists for one job only: the frame-cost work cut the time a frame takes
 * without being allowed to change a pixel, and the only honest proof of "not a
 * pixel" is the pixels. Each row is one scene of frame_scenes.inc composed
 * through the production overlay path (the frame-start latch, then the HUD,
 * shell and surface layers over a stand-in face, in 12-row strips) at one
 * clock value, hashed over all 466x466 RGB565 words.
 *
 * Scenes run back to back in ONE process, in table order, so a cache that keeps
 * state between frames (the glass geometry cache, any text memo) is also held
 * to "same pixels whatever was drawn before".
 *
 * REGENERATION. frame_hashes.inc is regenerated only when a look change is
 * intended and has been looked at: build the probe, run `probe frame list`,
 * then `probe frame SCENE clk=MS` for each scene and clock in the table, and
 * paste the hashes. A performance change that needs this file touched is a
 * change that altered pixels, and belongs in a different commit.
 */
#include <stdint.h>
#include "jr_display.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "idf_stubs.inc"
#include "frame_scenes.inc"

typedef struct {
    const char *scene;
    int64_t clk_ms;
    uint64_t hash;
} frame_ref_t;

static const frame_ref_t REFS[] = {
#include "frame_hashes.inc"
};
#define NREFS ((int)(sizeof REFS / sizeof REFS[0]))

static const fc_scene_t *find_scene(const char *name)
{
    for (int i = 0; i < FC_NSCENES; i++) {
        if (strcmp(FC_SCENES[i].name, name) == 0) return &FC_SCENES[i];
    }
    return NULL;
}

/* apply_dial copies only the columns inside the round glass of each row and
 * leaves the corners as the strip held them. It is not in the hashed frame (it
 * is a PSRAM copy), so it is held here against a full-row reference built in
 * this function: every pixel inside radius 233 equals the dial's, the far
 * corners (beyond 236) are untouched, and there are enough of them that the
 * span really skips something. */
static int dial_span_check(int *checks)
{
    static uint16_t frame[466 * 466], strip[466 * 12], ref[466 * 12];
    int bad = 0;
    for (int i = 0; i < 466 * 466; i++) frame[i] = (uint16_t)(1u + ((unsigned)i * 2654435761u >> 16) % 65000u);
    s_dial_frame = frame;
    s_dial_blit = true;
    s_display.board.width = 466;
    long far_skipped = 0;
    for (int y1 = 0; y1 < 466; y1 += 12) {
        const int y2 = y1 + 12 < 466 ? y1 + 12 : 466;
        for (int i = 0; i < 466 * 12; i++) strip[i] = 0;
        for (int y = y1; y < y2; y++) {                    /* the full-row copy, the reference */
            memcpy(ref + (y - y1) * 466, frame + y * 466, 466 * sizeof(uint16_t));
        }
        apply_dial(&s_display, 0, y1, 466, y2, strip);
        for (int y = y1; y < y2; y++) {
            for (int x = 0; x < 466; x++) {
                const int dx2 = (2 * x - 465), dy2 = (2 * y - 465);          /* twice the offset from the centre */
                const long r2 = (long)dx2 * dx2 + (long)dy2 * dy2;           /* 4 r^2 */
                const uint16_t got = strip[(y - y1) * 466 + x], want = ref[(y - y1) * 466 + x];
                (*checks)++;
                if (r2 <= 4L * 233 * 233 && got != want) bad++;
                if (r2 > 4L * 236 * 236) {
                    if (got != 0) bad++;
                    far_skipped++;
                }
            }
        }
    }
    if (far_skipped < 20000) {
        printf("FAIL: the span copy skipped only %ld corner pixels\n", far_skipped);
        bad++;
    }
    s_dial_blit = false;
    s_dial_frame = NULL;
    if (bad) printf("FAIL: dial span: %d pixels wrong\n", bad);
    return bad;
}

/* ---- THE FADE BITS: a frame in motion may differ, a settled one may not -------
 *
 * The fade bits (perf_knob.h) draw a part-strength screen at full strength and
 * dim the finished rows once, so a frame in motion differs from knob 0 by the
 * rounding of one multiply instead of the several of a blend per primitive. The
 * tolerance is the lane's contract: per pixel and channel in 5/6/5 steps, at most
 * 0.5 % of the glass pixels may differ by more than 2 steps in any channel and no
 * pixel by more than 6. Each transition is played through the production state
 * machine at the render cadence, once on knob 0 and once with the bit, and every
 * frame is compared. */
#define MP_FPS 24
#define MP_PRE 36
#define MP_MAX_FRAMES 120

typedef enum { MP_WAIT, MP_NEXT, MP_PREV, MP_OPEN, MP_CLOSE, MP_STYLE, MP_HOME } mp_op_t;
typedef struct { mp_op_t op; int arg; } mp_step_t;
typedef struct { const char *name; const char *scene; mp_step_t step[10]; } mp_script_t;

static const mp_script_t MP_SCRIPTS[] = {
    { "slide ring screens", "status-74",
      { { MP_NEXT, 0 }, { MP_WAIT, 700 }, { MP_NEXT, 0 }, { MP_WAIT, 700 }, { MP_PREV, 0 },
        { MP_WAIT, 700 }, { MP_HOME, 0 }, { MP_WAIT, 700 }, { MP_WAIT, 0 } } },
    { "status to desk and on", "desk",
      { { MP_PREV, 0 }, { MP_WAIT, 900 }, { MP_NEXT, 0 }, { MP_WAIT, 900 }, { MP_NEXT, 0 },
        { MP_WAIT, 900 }, { MP_WAIT, 0 } } },
    { "weather to status and back", "weather",
      { { MP_NEXT, 0 }, { MP_WAIT, 900 }, { MP_PREV, 0 }, { MP_WAIT, 900 }, { MP_WAIT, 0 } } },
    { "activity to home, the wrap", "activity",
      { { MP_NEXT, 0 }, { MP_WAIT, 900 }, { MP_WAIT, 0 } } },
    { "home to watch to weather", "home",
      { { MP_NEXT, 0 }, { MP_WAIT, 900 }, { MP_NEXT, 0 }, { MP_WAIT, 900 }, { MP_WAIT, 0 } } },
    { "style from dial to dial", "watch-diver",
      { { MP_STYLE, 1 }, { MP_WAIT, 700 }, { MP_STYLE, 1 }, { MP_WAIT, 700 }, { MP_STYLE, -1 },
        { MP_WAIT, 700 }, { MP_WAIT, 0 } } },
    { "style onto minimal", "watch-pilot",
      { { MP_STYLE, 1 }, { MP_WAIT, 700 }, { MP_STYLE, -1 }, { MP_WAIT, 700 }, { MP_WAIT, 0 } } },
    { "shade over home", "home",
      { { MP_OPEN, 0 }, { MP_WAIT, 800 }, { MP_CLOSE, 0 }, { MP_WAIT, 800 }, { MP_WAIT, 0 } } },
    { "shade over a ring screen", "status-74",
      { { MP_OPEN, 0 }, { MP_WAIT, 800 }, { MP_CLOSE, 0 }, { MP_WAIT, 800 }, { MP_WAIT, 0 } } },
    { "shade over a baked dial", "watch-diver",
      { { MP_OPEN, 0 }, { MP_WAIT, 800 }, { MP_CLOSE, 0 }, { MP_WAIT, 800 }, { MP_WAIT, 0 } } },
    { "shade over minimal", "watch-minimal",
      { { MP_OPEN, 0 }, { MP_WAIT, 800 }, { MP_CLOSE, 0 }, { MP_WAIT, 800 }, { MP_WAIT, 0 } } },
};
#define MP_NSCRIPTS ((int)(sizeof MP_SCRIPTS / sizeof MP_SCRIPTS[0]))

static uint16_t mp_dial[466 * 466];
static uint16_t mp_black[466 * 466];
static int64_t mp_frame_no;

/* A dial stand-in that is lit over half the glass in 6 px blocks, so the hands
 * and the dim pass meet colour, not black. */
static void mp_build_dial(void)
{
    for (int y = 0; y < 466; y++) {
        for (int x = 0; x < 466; x++) {
            const int dx = x - 232, dy = y - 232;
            const int r2 = dx * dx + dy * dy;
            uint16_t v = 0;
            if (r2 <= 214 * 214 && (((x / 6) + (y / 6)) & 1) == 0) {
                v = (uint16_t)((((x * 3 + y) & 31) << 11) | (((x + y * 5) & 63) << 5) | ((x ^ y) & 31));
                if (v == 0) v = 0x0821;
            }
            mp_dial[y * 466 + x] = v;
        }
    }
}

static void mp_frame(int64_t t_us, uint16_t *fb)
{
    jr_display_ctx_t *ctx = &s_display;
    const int64_t ms = t_us / 1000;
    s_fake_us = t_us;
    if (jr_display_nav_space() == JR_DISPLAY_SPACE_WATCH) {
        jr_display_clock_set(true, 10, 8, (int)((ms / 1000) % 60));
    } else {
        jr_display_clock_set(false, 0, 0, 0);
    }
    const jr_face_t dial = watch_dial_face();
    if (dial != JR_FACE_COUNT) {
        s_dial_frame = mp_dial;
        s_dial_frame_face = dial;
        s_dial_blit = true;
        ctx->shown_face = dial;
    } else {
        s_dial_blit = false;
        ctx->shown_face = JR_FACE_IDLE;
    }
    s_parked = !s_dial_blit && __atomic_load_n(&s_park_want, __ATOMIC_ACQUIRE) != 0;
    overlay_fade_tick();
    if (!s_parked) {
        fc_fill_face(fb);
    }
    for (int y = 0; y < 466; y += 12) {
        const int y2 = y + 12 > 466 ? 466 : y + 12;
        uint16_t *p = fb + (size_t)y * 466;
        if (s_parked) {
            memcpy(p, mp_black + (size_t)y * 466, (size_t)(y2 - y) * 466u * sizeof(uint16_t));
        }
        (void)rest_eligible(ctx);
        apply_dial(ctx, 0, y, 466, y2, p);
        apply_hud_overlay(ctx, 0, y, 466, y2, p);
        apply_shell_overlay(ctx, 0, y, 466, y2, p);
        apply_surface_overlay(ctx, 0, y, 466, y2, p);
    }
    mp_frame_no++;
}

static int64_t mp_t(void) { return mp_frame_no * 1000000LL / MP_FPS; }

/* Play one script with the given knob word; frames[] gets the recorded ones. */
static int mp_play(const mp_script_t *sc, uint32_t knob, uint16_t **frames)
{
    const fc_scene_t *scene = find_scene(sc->scene);
    if (scene == NULL) return -1;
    fc_knob_extra = knob;
    mp_frame_no = 0;
    fc_setup(scene);
    s_fake_us = 0;
    s_fade_prev_ms = 0;
    static uint16_t fb[466 * 466];
    for (int i = 0; i < MP_PRE; i++) mp_frame(mp_t(), fb);
    int n = 0;
    for (int k = 0; sc->step[k].op != MP_WAIT || sc->step[k].arg != 0; k++) {
        const mp_step_t *st = &sc->step[k];
        switch (st->op) {
        case MP_NEXT:  jr_display_nav_next(); break;
        case MP_PREV:  jr_display_nav_prev(); break;
        case MP_OPEN:  jr_display_nav_down(); break;
        case MP_CLOSE: jr_display_nav_up(); break;
        case MP_HOME:  jr_display_nav_home(); break;
        case MP_STYLE:
            for (int i = 0; i < (st->arg < 0 ? -st->arg : st->arg); i++) {
                jr_display_watch_style_step(st->arg < 0 ? -1 : 1);
            }
            break;
        case MP_WAIT: {
            const int64_t t0 = mp_t();
            while (mp_t() < t0 + (int64_t)st->arg * 1000 && n < MP_MAX_FRAMES) {
                mp_frame(mp_t(), fb);
                memcpy(frames[n++], fb, sizeof fb);
            }
            break;
        }
        }
    }
    fc_knob_extra = 0;
    return n;
}

typedef struct { long glass; long over2; int max_step; int worst_frame; double worst_frac; int max_frame; } mp_stat_t;

/* a vs b over one frame: pixels inside the glass that differ by more than 2 steps
 * in any 5/6/5 channel, and the largest step anywhere */
static void mp_diff(const uint16_t *a, const uint16_t *b, int frame, mp_stat_t *st)
{
    long glass = 0, over = 0;
    int mx = 0;
    for (int y = 0; y < 466; y++) {
        for (int x = 0; x < 466; x++) {
            const uint16_t va = a[y * 466 + x], vb = b[y * 466 + x];
            const long dx = 2L * x + 1 - 466, dy = 2L * y + 1 - 466;
            const bool inside = dx * dx + dy * dy <= 466L * 466L;
            glass += inside;
            if (va == vb) continue;
            int dr = (va >> 11) - (vb >> 11), dg = ((va >> 5) & 63) - ((vb >> 5) & 63), db = (va & 31) - (vb & 31);
            if (dr < 0) dr = -dr;
            if (dg < 0) dg = -dg;
            if (db < 0) db = -db;
            int d = dr > dg ? dr : dg;
            if (db > d) d = db;
            if (d > mx) mx = d;
            if (inside && d > 2) {
                over++;
            }
        }
    }
    st->glass = glass;
    st->over2 += over;
    const double frac = glass ? (double)over / (double)glass : 0.0;
    if (frac > st->worst_frac) { st->worst_frac = frac; st->worst_frame = frame; }
    if (mx > st->max_step) { st->max_step = mx; st->max_frame = frame; }
}

static int fade_motion_check(int *checks)
{
    int bad = 0;
    mp_build_dial();
    uint16_t *base[MP_MAX_FRAMES], *cand[MP_MAX_FRAMES];
    for (int i = 0; i < MP_MAX_FRAMES; i++) {
        base[i] = malloc(466 * 466 * sizeof(uint16_t));
        cand[i] = malloc(466 * 466 * sizeof(uint16_t));
        if (base[i] == NULL || cand[i] == NULL) { printf("FAIL: motion buffers\n"); return 1; }
    }
    for (int sidx = 0; sidx < MP_NSCRIPTS; sidx++) {
        const mp_script_t *sc = &MP_SCRIPTS[sidx];
        const int n0 = mp_play(sc, 0u, base);
        const int n1 = mp_play(sc, 0u, cand);
        (*checks)++;
        if (n0 <= 0 || n0 != n1) { printf("FAIL motion %s: %d vs %d frames\n", sc->name, n0, n1); bad++; continue; }
        long same = 0;
        for (int f = 0; f < n0; f++) same += memcmp(base[f], cand[f], 466 * 466 * sizeof(uint16_t)) == 0;
        (*checks)++;
        if (same != n0) {
            printf("FAIL motion %s: knob 0 is not repeatable (%ld of %d frames equal)\n", sc->name, same, n0);
            bad++;
            continue;
        }
        for (unsigned b = 0; b < 32; b++) {
            const uint32_t bit = 1u << b;
            if ((JR_KNOB_KNOWN_MASK & bit) == 0u) continue;
            const int n2 = mp_play(sc, bit, cand);
            (*checks)++;
            if (n2 != n0) { printf("FAIL motion %s bit %u: %d frames, knob 0 had %d\n", sc->name, b, n2, n0); bad++; continue; }
            mp_stat_t st = {0};
            long moved = 0;
            for (int f = 0; f < n0; f++) {
                moved += memcmp(base[f], cand[f], 466 * 466 * sizeof(uint16_t)) != 0;
                mp_diff(base[f], cand[f], f, &st);
            }
            (*checks)++;
            printf("motion %-42s bit %u: %3d frames, %3ld differ, worst frame %3d has %.3f %% over 2 steps, largest step %d (frame %d)\n",
                   sc->name, b, n0, moved, st.worst_frame, st.worst_frac * 100.0, st.max_step, st.max_frame);
            if (st.worst_frac > 0.005 || st.max_step > 6) {
                printf("FAIL motion %s bit %u: outside the tolerance (0.5 %% over 2 steps, none over 6)\n", sc->name, b);
                bad++;
            }
        }
    }
    for (int i = 0; i < MP_MAX_FRAMES; i++) { free(base[i]); free(cand[i]); }
    return bad;
}

int main(void)
{
    static uint16_t fb[466 * 466];
    int failures = 0, checks = 0;
    /* Passes over the whole table: the recorded orbit rail in use (as on
     * the device), then without it (the fallback when PSRAM is short), then with
     * the rail but without the recorded battery / desk ring, then without the
     * recorded shade arcs. All four must give the pixels the pre-cut code gave. */
    for (int pass = 0; pass < 4; pass++) {
        fc_orbit_rec_on = pass != 1;
        fc_fring_rec_on = pass != 2;
        fc_shade_rec_on = pass != 3;
        for (int i = 0; i < NREFS; i++) {
            const fc_scene_t *sc = find_scene(REFS[i].scene);
            if (sc == NULL) {
                printf("FAIL: scene %s is in the table but not in frame_scenes.inc\n", REFS[i].scene);
                failures++;
                continue;
            }
            fc_setup(sc);
            checks++;
            if ((s_orbit_rec != NULL) != fc_orbit_rec_on) {
                printf("FAIL %s: orbit record %s but pass wanted it %s\n", REFS[i].scene,
                       s_orbit_rec != NULL ? "built" : "absent", fc_orbit_rec_on ? "on" : "off");
                failures++;
            }
            fc_run(sc, REFS[i].clk_ms, fb);
            const uint64_t h = fc_hash(fb);
            checks++;
            if (h != REFS[i].hash) {
                printf("FAIL %s clk=%lld (orbit record %s, ring record %s, shade record %s): frame hash %016llx, expected %016llx\n",
                       REFS[i].scene, (long long)REFS[i].clk_ms, fc_orbit_rec_on ? "on" : "off",
                       fc_fring_rec_on ? "on" : "off", fc_shade_rec_on ? "on" : "off",
                       (unsigned long long)h, (unsigned long long)REFS[i].hash);
                failures++;
            }
        }
    }
    /* One more pass per perf-knob bit: the table again with only that bit on,
     * so the bit is what differs from the shipped path. No bits are defined at
     * present (JR_KNOB_KNOWN_MASK is 0), so the loop has nothing to run. */
    fc_orbit_rec_on = true;
    fc_fring_rec_on = true;
    fc_shade_rec_on = true;
    for (unsigned b = 0; b < 32; b++) {
        const uint32_t bit = 1u << b;
        if ((JR_KNOB_KNOWN_MASK & bit) == 0u) {
            continue;
        }
        fc_knob_extra = bit;
        for (int i = 0; i < NREFS; i++) {
            const fc_scene_t *sc = find_scene(REFS[i].scene);
            if (sc == NULL) continue;
            fc_setup(sc);
            checks++;
            if ((hud_perf_knob & bit) == 0u || (glass_perf_knob & bit) == 0u) {
                printf("FAIL %s: knob bit %u did not reach the drawing code\n", REFS[i].scene, b);
                failures++;
            }
            fc_run(sc, REFS[i].clk_ms, fb);
            checks++;
            if (sc->watch_in > 0) {
                /* a frame in the middle of a slide: held to the tolerance, not the hash */
                static uint16_t ref0[466 * 466];
                fc_knob_extra = 0;
                fc_setup(sc);
                fc_run(sc, REFS[i].clk_ms, ref0);
                fc_knob_extra = bit;
                mp_stat_t st = {0};
                mp_diff(ref0, fb, 0, &st);
                if (st.worst_frac > 0.005 || st.max_step > 6) {
                    printf("FAIL %s clk=%lld: knob bit %u moved a frame in motion past the tolerance (%.3f %%, step %d)\n",
                           REFS[i].scene, (long long)REFS[i].clk_ms, b, st.worst_frac * 100.0, st.max_step);
                    failures++;
                }
            } else if (fc_hash(fb) != REFS[i].hash) {
                printf("FAIL %s clk=%lld: knob bit %u changed the pixels\n", REFS[i].scene,
                       (long long)REFS[i].clk_ms, b);
                failures++;
            }
        }
    }
    failures += dial_span_check(&checks);
    fc_knob_extra = 0;
    failures += fade_motion_check(&checks);
    /* every scene must be pinned at least once: a scene added to the fixture
     * and not to the table would otherwise be unguarded */
    for (int s = 0; s < FC_NSCENES; s++) {
        bool pinned = false;
        for (int i = 0; i < NREFS; i++) pinned |= strcmp(REFS[i].scene, FC_SCENES[s].name) == 0;
        checks++;
        if (!pinned) {
            printf("FAIL: scene %s has no frame hash\n", FC_SCENES[s].name);
            failures++;
        }
    }
    /* The recorded rail against the glass ring it stands in for, over a busy
     * background, every row, with the alphas the fades produce, both byte orders
     * and clips that cut the ring (the scenes only draw it at one alpha, in
     * panel order, unclipped). */
    {
        orbit_rec_build();
        printf("orbit rail record: %u bytes (PSRAM)\n",
               (unsigned)glass_ring_record(NULL, 0, SP_CX * 16, SP_CY * 16,
                                           SP_ORB_TRACK_IN * 16, SP_ORB_TRACK_OUT * 16));
        static uint16_t a[466], b[466];
        static const int alphas[] = { 1, 37, 128, 200, 254, 255 };
        static const int clips[][2] = { { 0, 465 }, { 100, 300 }, { 0, 5 }, { 230, 236 }, { 440, 465 } };
        long touched = 0;
        for (unsigned ai = 0; ai < sizeof alphas / sizeof alphas[0]; ai++) {
            for (int sw = 0; sw < 2; sw++) {
                for (unsigned ci = 0; ci < sizeof clips / sizeof clips[0]; ci++) {
                    int bad = 0;
                    for (int y = 0; y < 466 && !bad; y++) {
                        for (int x = 0; x < 466; x++) {
                            a[x] = b[x] = (uint16_t)(x * 2654435761u + y * 40503u + 12345u);
                        }
                        glass_ring_row(a, y, clips[ci][0], clips[ci][1], SP_CX * 16, SP_CY * 16,
                                       SP_ORB_TRACK_IN * 16, SP_ORB_TRACK_OUT * 16, 0,
                                       GLASS_TURN_Q16, SP_C_PLATE, (uint8_t)alphas[ai], sw != 0);
                        glass_ring_replay_row(b, y, clips[ci][0], clips[ci][1], s_orbit_rec,
                                              SP_C_PLATE, (uint8_t)alphas[ai], sw != 0);
                        for (int x = 0; x < 466; x++) {
                            if (a[x] != b[x]) bad = 1;
                            if (a[x] != (uint16_t)(x * 2654435761u + y * 40503u + 12345u)) touched++;
                        }
                        if (bad) {
                            printf("FAIL orbit replay differs: alpha %d swap %d clip %d..%d row %d\n",
                                   alphas[ai], sw, clips[ci][0], clips[ci][1], y);
                            failures++;
                        }
                    }
                    checks++;
                }
            }
        }
        checks++;
        if (touched < 50000) {
            printf("FAIL orbit replay comparison drew only %ld pixels\n", touched);
            failures++;
        }
    }
    /* The recorded battery / desk ring against the three glass calls it stands in
     * for, over a busy background: every row, at sweeps from a sliver to full,
     * with the alphas the fades produce, both byte orders and clips that cut the
     * ring. The scenes only reach it at one alpha and two or three sweeps. */
    {
        static uint8_t rt[SG_FRING_CAP], rg[SG_FRING_CAP], ra[SG_FRING_CAP];
        static uint16_t a[466], b[466];
        static const int32_t sweeps[] = { 1, 40, 655, 3277, 16384, 32768, 48500, 64000, 65535, 65536 };
        static const int alphas[] = { 1, 90, 255 };
        static const int clips[][2] = { { 0, 465 }, { 120, 310 }, { 228, 240 } };
        const int32_t cx16 = SP_CX * 16, cy16 = (SP_CY + SG_RING_DY) * 16;
        long touched = 0, bytes_max = 0;
        for (unsigned si = 0; si < sizeof sweeps / sizeof sweeps[0]; si++) {
            const int32_t sw = sweeps[si];
            const size_t nt = sw < GLASS_TURN_Q16 ? glass_shape_record(rt, sizeof rt, cx16, cy16,
                SG_RING_R_IN16, SG_RING_R_OUT16, GLASS_Q16_12_OCLOCK + sw, GLASS_TURN_Q16 - sw, false) : 0u;
            const size_t ng = glass_glow_record(rg, sizeof rg, cx16, cy16, SG_RING_R_IN16,
                SG_RING_R_OUT16, GLASS_Q16_12_OCLOCK, sw, true, SG_GLOW16);
            const size_t na = glass_shape_record(ra, sizeof ra, cx16, cy16, SG_RING_R_IN16,
                SG_RING_R_OUT16, GLASS_Q16_12_OCLOCK, sw, true);
            checks++;
            if (ng == 0u || na == 0u || (sw < GLASS_TURN_Q16 && nt == 0u)) {
                printf("FAIL ring record for sweep %d is empty (track %zu glow %zu arc %zu)\n",
                       (int)sw, nt, ng, na);
                failures++;
                continue;
            }
            if ((long)nt > bytes_max) bytes_max = (long)nt;
            if ((long)ng > bytes_max) bytes_max = (long)ng;
            if ((long)na > bytes_max) bytes_max = (long)na;
            for (unsigned ai = 0; ai < sizeof alphas / sizeof alphas[0]; ai++) {
                for (int swp = 0; swp < 2; swp++) {
                    for (unsigned ci = 0; ci < sizeof clips / sizeof clips[0]; ci++) {
                        int bad = 0;
                        for (int y = 0; y < 466 && !bad; y++) {
                            for (int x = 0; x < 466; x++) {
                                a[x] = b[x] = (uint16_t)(x * 2654435761u + y * 40503u + 777u);
                            }
                            const int lo = clips[ci][0], hi = clips[ci][1];
                            const uint8_t at = (uint8_t)((alphas[ai] * 44) / 255);
                            if (sw < GLASS_TURN_Q16) {
                                glass_ring_row(a, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                                               GLASS_Q16_12_OCLOCK + sw, GLASS_TURN_Q16 - sw, SP_C_CYAN, at, swp != 0);
                                glass_ring_replay_row(b, y, lo, hi, rt, SP_C_CYAN, at, swp != 0);
                            }
                            glass_glow_row(a, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                                           GLASS_Q16_12_OCLOCK, sw, true, SG_GLOW16, 140, SP_C_CYAN,
                                           (uint8_t)alphas[ai], swp != 0);
                            glass_glow_replay_row(b, y, lo, hi, rg, SG_GLOW16, 140, SP_C_CYAN,
                                                  (uint8_t)alphas[ai], swp != 0);
                            glass_arc_row(a, y, lo, hi, cx16, cy16, SG_RING_R_IN16, SG_RING_R_OUT16,
                                          GLASS_Q16_12_OCLOCK, sw, SP_C_CYAN, (uint8_t)alphas[ai], swp != 0);
                            glass_ring_replay_row(b, y, lo, hi, ra, SP_C_CYAN, (uint8_t)alphas[ai], swp != 0);
                            for (int x = 0; x < 466; x++) {
                                if (a[x] != b[x]) bad = 1;
                                if (a[x] != (uint16_t)(x * 2654435761u + y * 40503u + 777u)) touched++;
                            }
                            if (bad) {
                                printf("FAIL ring replay differs: sweep %d alpha %d swap %d clip %d..%d row %d\n",
                                       (int)sw, alphas[ai], swp, lo, hi, y);
                                failures++;
                            }
                        }
                        checks++;
                    }
                }
            }
        }
        checks++;
        if (touched < 100000) {
            printf("FAIL ring replay comparison drew only %ld pixels\n", touched);
            failures++;
        }
        printf("battery / desk ring records: largest %ld bytes of %d (PSRAM, three blocks)\n",
               bytes_max, SG_FRING_CAP);
    }
    /* sp_clip() reads a table where it used to take a root: the same half width
     * on every row of the panel, and the same verdict for rows past the shell. */
    {
        int bad = 0;
        for (int y = 0; y < 466; y++) {
            const int dy = y - SP_CY, d2 = SP_R_SHELL * SP_R_SHELL - dy * dy;
            int lo = 0, hi = 465;
            const bool got = sp_clip(y, &lo, &hi);
            const int half = d2 > 0 ? sp_isqrt(d2) : 0;
            int wlo = SP_CX - half < 0 ? 0 : SP_CX - half, whi = SP_CX + half > 465 ? 465 : SP_CX + half;
            const bool want = d2 > 0 && wlo <= whi;
            checks++;
            if (got != want || (got && (lo != wlo || hi != whi))) {
                printf("FAIL sp_clip row %d: got %d [%d,%d], want %d [%d,%d]\n", y, got, lo, hi, want, wlo, whi);
                failures++;
                bad++;
            }
        }
        (void)bad;
    }
    /* The recorded shade arcs against glass_arc_row, at the fills a drag
     * produces (volume 0 to 100 %), every row, both byte orders. */
    {
        static uint8_t rec[SG_SH_REC_CAP];
        static uint16_t a[466], b[466];
        static const int pcts[] = { 1, 17, 50, 83, 100 };
        long touched = 0, bytes_max = 0;
        for (unsigned pi = 0; pi < sizeof pcts / sizeof pcts[0]; pi++) {
            const int32_t sw = (int32_t)((int64_t)2 * SG_SH_HALF_Q16 * pcts[pi] / 100);
            const int32_t starts[2] = { (GLASS_TURN_Q16 / 2 - SG_SH_HALF_Q16) & 0xFFFF,
                                        (SG_SH_HALF_Q16 - sw) & 0xFFFF };
            for (int si = 0; si < 2; si++) {
                const size_t n = glass_shape_record(rec, sizeof rec, SP_CX * 16, SP_CY * 16,
                                                    SG_SH_R_IN16, SG_SH_R_OUT16, starts[si], sw, true);
                checks++;
                if (n == 0u) {
                    printf("FAIL shade arc record for %d %% is empty\n", pcts[pi]);
                    failures++;
                    continue;
                }
                if ((long)n > bytes_max) bytes_max = (long)n;
                for (int swp = 0; swp < 2; swp++) {
                    int bad = 0;
                    for (int y = 0; y < 466 && !bad; y++) {
                        for (int x = 0; x < 466; x++) a[x] = b[x] = (uint16_t)(x * 2654435761u + y * 40503u + 99u);
                        glass_arc_row(a, y, 0, 465, SP_CX * 16, SP_CY * 16, SG_SH_R_IN16, SG_SH_R_OUT16,
                                      starts[si], sw, SP_C_AMBER, 173u, swp != 0);
                        glass_ring_replay_row(b, y, 0, 465, rec, SP_C_AMBER, 173u, swp != 0);
                        for (int x = 0; x < 466; x++) {
                            if (a[x] != b[x]) bad = 1;
                            if (a[x] != (uint16_t)(x * 2654435761u + y * 40503u + 99u)) touched++;
                        }
                        if (bad) {
                            printf("FAIL shade arc replay differs: %d %% start %d swap %d row %d\n", pcts[pi], si, swp, y);
                            failures++;
                        }
                    }
                    checks++;
                }
            }
        }
        checks++;
        if (touched < 20000) {
            printf("FAIL shade arc comparison drew only %ld pixels\n", touched);
            failures++;
        }
        printf("control shade arc records: largest %ld bytes of %d (PSRAM, four blocks)\n", bytes_max, SG_SH_REC_CAP);
    }
    /* glass_arc_rows(): a row outside it must be a row glass_arc_row leaves
     * untouched, for arcs of every sweep and start, thick and thin, with and
     * without round caps (WEATHER's marks are drawn only on the rows inside); and
     * the rows inside must not be all of them, or the bound would prove nothing. */
    {
        static uint16_t row[466];
        static const int32_t sweeps[] = { 546, 3641, 9000, 16384, 32768, 40000, 65000 };
        static const int32_t widths[] = { 16, 32, 80, 160 };
        long outside = 0, inside = 0;
        for (unsigned wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
            for (unsigned si = 0; si < sizeof sweeps / sizeof sweeps[0]; si++) {
                for (int32_t start = 0; start < 65536; start += 2731) {
                    for (int caps = 0; caps < 2; caps++) {
                        const int32_t rc = 118 * 16, ri = rc - widths[wi] / 2, ro = rc + (widths[wi] + 1) / 2;
                        int lo, hi;
                        const bool any = glass_arc_rows(SP_CX * 16, SP_CY * 16, ri, ro, start, sweeps[si],
                                                        caps != 0, &lo, &hi);
                        checks++;
                        if (!any) { lo = 1; hi = 0; }
                        for (int y = 0; y < 466; y++) {
                            memset(row, 0, sizeof row);
                            if (caps) {
                                glass_arc_row(row, y, 0, 465, SP_CX * 16, SP_CY * 16, ri, ro, start,
                                              sweeps[si], 0xFFFFu, 255u, false);
                            } else {
                                glass_ring_row(row, y, 0, 465, SP_CX * 16, SP_CY * 16, ri, ro, start,
                                               sweeps[si], 0xFFFFu, 255u, false);
                            }
                            bool drew = false;
                            for (int x = 0; x < 466 && !drew; x++) drew = row[x] != 0u;
                            if (y < lo || y > hi) {
                                outside++;
                                if (drew) {
                                    printf("FAIL arc rows %d..%d: row %d is drawn (start %d sweep %d width %d caps %d)\n",
                                           lo, hi, y, (int)start, (int)sweeps[si], (int)widths[wi], caps);
                                    failures++;
                                    break;
                                }
                            } else {
                                inside++;
                            }
                        }
                    }
                }
            }
        }
        checks++;
        if (outside < 100000 || inside < 10000) {
            printf("FAIL arc rows comparison is degenerate (outside %ld, inside %ld)\n", outside, inside);
            failures++;
        }
    }
    /* The orbit's lit mark is only asked about on the rows orbit_mark_rows()
     * allows: at every one of its 256 positions, every row outside the bound
     * must be a row the real arc leaves untouched, and the rows inside must not
     * be all of them (a bound that never excludes would pass the first check). */
    {
        static uint16_t row[466];
        int outside_total = 0;
        for (int a = 0; a < 256; a++) {
            int lo, hi;
            orbit_mark_rows(a, &lo, &hi);
            const int32_t start = (int32_t)((((a - 5) * 256)) & 0xFFFF);
            for (int y = 0; y < 466; y++) {
                memset(row, 0, sizeof row);
                glass_arc_row(row, y, 0, 465, SP_CX * 16, SP_CY * 16, SP_ORB_MARK_IN * 16,
                              SP_ORB_MARK_OUT * 16, start, 10 * 256, 0xFFFFu, 255u, false);
                bool drew = false;
                for (int x = 0; x < 466 && !drew; x++) drew = row[x] != 0u;
                if (y < lo || y > hi) {
                    outside_total++;
                    if (drew) {
                        printf("FAIL orbit mark at %d draws on row %d outside [%d, %d]\n", a, y, lo, hi);
                        failures++;
                        break;
                    }
                }
            }
            checks++;
        }
        checks++;
        if (outside_total < 256 * 300) {
            printf("FAIL orbit mark bound excludes only %d rows over 256 positions\n", outside_total);
            failures++;
        }
    }
    /* The split timers (GET /api/display?split=1). With the fake clock moving
     * on every read, a part that ran must have charged time and a part that
     * did not must not: "empty is not pass", so each row names the buckets
     * that must be positive and the ones that must stay zero. */
    static const struct {
        const char *scene;
        int64_t clk_ms;
        jr_display_split_t positive[6];
        jr_display_split_t zero[3];
    } SPLITS[] = {
        { "status-74", 20000,
          { JR_DISPLAY_SPLIT_HUD_FRAME, JR_DISPLAY_SPLIT_HUD_SPACE, JR_DISPLAY_SPLIT_SPACE_VEIL,
            JR_DISPLAY_SPLIT_SPACE_SCREEN, JR_DISPLAY_SPLIT_SPACE_ORBIT, JR_DISPLAY_SPLIT_HUD_OTA },
          { JR_DISPLAY_SPLIT_CLOCK_FACE, JR_DISPLAY_SPLIT_CLOCK_CELLS, JR_DISPLAY_SPLIT_SPACE_SHADE } },
        { "watch-dress", 20000,
          { JR_DISPLAY_SPLIT_HUD_CLOCK, JR_DISPLAY_SPLIT_CLOCK_CLEAR, JR_DISPLAY_SPLIT_CLOCK_FACE,
            JR_DISPLAY_SPLIT_CLOCK_CELLS, JR_DISPLAY_SPLIT_HUD_OTA, JR_DISPLAY_SPLIT_HUD_FRAME },
          /* the watch clears the dial itself, so the shell backdrop is skipped:
           * none of the space_* parts run (the paint-then-erase cut) */
          { JR_DISPLAY_SPLIT_SPACE_ORBIT, JR_DISPLAY_SPLIT_SPACE_SCREEN, JR_DISPLAY_SPLIT_HUD_ASK } },
        { "shade", 20000,
          { JR_DISPLAY_SPLIT_SPACE_SHADE, JR_DISPLAY_SPLIT_HUD_SPACE, JR_DISPLAY_SPLIT_HUD_FRAME,
            JR_DISPLAY_SPLIT_SPACE_ORBIT, JR_DISPLAY_SPLIT_HUD_OTA, JR_DISPLAY_SPLIT_HUD_OTA },
          { JR_DISPLAY_SPLIT_SPACE_SHEET, JR_DISPLAY_SPLIT_CLOCK_FACE, JR_DISPLAY_SPLIT_HUD_ASK } },
        { "detail", 20000,
          { JR_DISPLAY_SPLIT_SPACE_SHEET, JR_DISPLAY_SPLIT_HUD_SPACE, JR_DISPLAY_SPLIT_HUD_FRAME,
            JR_DISPLAY_SPLIT_SPACE_ORBIT, JR_DISPLAY_SPLIT_HUD_OTA, JR_DISPLAY_SPLIT_HUD_OTA },
          { JR_DISPLAY_SPLIT_SPACE_SHADE, JR_DISPLAY_SPLIT_CLOCK_FACE, JR_DISPLAY_SPLIT_HUD_ASK } },
    };
    for (unsigned r = 0; r < sizeof SPLITS / sizeof SPLITS[0]; r++) {
        const fc_scene_t *sc = find_scene(SPLITS[r].scene);
        if (sc == NULL) {
            printf("FAIL: split scene %s missing\n", SPLITS[r].scene);
            failures++;
            continue;
        }
        fc_setup(sc);
        s_fake_us_step = 3;
        for (int i = 0; i < JR_DISPLAY_SPLIT_COUNT; i++) s_split_us[i] = 0;
        /* unarmed (the default): nothing is timed, whatever ran */
        s_split_deadline_ms = 0u;
        fc_run(sc, SPLITS[r].clk_ms, fb);
        {
            unsigned total = 0;
            for (int i = 0; i < JR_DISPLAY_SPLIT_COUNT; i++) total += s_split_us[i];
            checks++;
            if (total != 0u) {
                printf("FAIL %s: split timers charged %u us with nobody asking\n", sc->name, total);
                failures++;
            }
        }
        /* armed, as ?split=1 leaves them for a few seconds */
        s_split_deadline_ms = (uint32_t)SPLITS[r].clk_ms + 4000u;
        fc_run(sc, SPLITS[r].clk_ms, fb);
        s_split_deadline_ms = 0u;
        s_fake_us_step = 0;
        for (int i = 0; i < 6; i++) {
            checks++;
            if (s_split_us[SPLITS[r].positive[i]] == 0u) {
                printf("FAIL %s: split %s charged nothing\n", sc->name,
                       jr_display_split_name((unsigned)SPLITS[r].positive[i]));
                failures++;
            }
        }
        for (int i = 0; i < 3; i++) {
            checks++;
            if (s_split_us[SPLITS[r].zero[i]] != 0u) {
                printf("FAIL %s: split %s charged %u but did not run\n", sc->name,
                       jr_display_split_name((unsigned)SPLITS[r].zero[i]),
                       (unsigned)s_split_us[SPLITS[r].zero[i]]);
                failures++;
            }
        }
        /* nested parts are inside their parent, never beside it */
        checks++;
        if (s_split_us[JR_DISPLAY_SPLIT_HUD_SPACE] <
            s_split_us[JR_DISPLAY_SPLIT_SPACE_VEIL] + s_split_us[JR_DISPLAY_SPLIT_SPACE_SCREEN] +
            s_split_us[JR_DISPLAY_SPLIT_SPACE_SHEET] + s_split_us[JR_DISPLAY_SPLIT_SPACE_SHADE] +
            s_split_us[JR_DISPLAY_SPLIT_SPACE_ORBIT]) {
            printf("FAIL %s: space_* exceeds hud_space\n", sc->name);
            failures++;
        }
    }
    checks++;
    if (jr_display_split_name(0) == NULL || jr_display_split_name(JR_DISPLAY_SPLIT_COUNT) != NULL) {
        printf("FAIL: split name table range\n");
        failures++;
    }
    if (failures) {
        printf("%d failure(s) of %d checks\n", failures, checks);
        return 1;
    }
    printf("frame identity passed (%d checks)\n", checks);
    return 0;
}
