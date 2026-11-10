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

int main(void)
{
    static uint16_t fb[466 * 466];
    int failures = 0, checks = 0;
    /* Two passes over the whole table: the recorded orbit rail in use (as on
     * the device), then without it (the fallback when PSRAM is short), then with
     * the rail but without the recorded battery / desk ring, then without the
     * recorded shade arcs, then without the recorded dial disc. All five must give
     * the pixels the pre-cut code gave. */
    for (int pass = 0; pass < 5; pass++) {
        fc_orbit_rec_on = pass != 1;
        fc_fring_rec_on = pass != 2;
        fc_shade_rec_on = pass != 3;
        fc_dial_rec_on = pass != 4;
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
                printf("FAIL %s clk=%lld (orbit record %s, ring record %s, shade record %s, dial record %s): frame hash %016llx, expected %016llx\n",
                       REFS[i].scene, (long long)REFS[i].clk_ms, fc_orbit_rec_on ? "on" : "off",
                       fc_fring_rec_on ? "on" : "off", fc_shade_rec_on ? "on" : "off", fc_dial_rec_on ? "on" : "off",
                       (unsigned long long)h, (unsigned long long)REFS[i].hash);
                failures++;
            }
        }
    }
    fc_orbit_rec_on = true;
    fc_fring_rec_on = true;
    fc_shade_rec_on = true;
    fc_dial_rec_on = true;
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
    /* The veil's word-at-a-time dim against hud_dim565 pixel by pixel: random
     * rows (black runs among them), every start and end parity, both byte
     * orders. The row is aligned as the strips are. */
    {
        static uint16_t a[468] __attribute__((aligned(4))), b[468] __attribute__((aligned(4)));
        uint32_t seed = 12345u;
        int bad = 0;
        for (int round = 0; round < 400 && !bad; round++) {
            for (int x = 0; x < 468; x++) {
                seed = seed * 1664525u + 1013904223u;
                a[x] = b[x] = (seed >> 24) < 60u ? 0u : (uint16_t)(seed >> 8);
            }
            const int lo = round % 5, hi = 465 - (round / 5) % 7;
            const bool swap = (round & 1) != 0;
            if (swap) veil_dim_row(a, lo, hi, true); else veil_dim_row(a, lo, hi, false);
            for (int x = 0; x < 468; x++) {
                const uint16_t want = (x >= lo && x <= hi) ? hud_dim565(b[x], swap) : b[x];
                if (a[x] != want) bad = 1;
            }
            checks++;
        }
        if (bad) {
            printf("FAIL veil_dim_row differs from hud_dim565\n");
            failures++;
        }
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
