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
    for (int i = 0; i < NREFS; i++) {
        const fc_scene_t *sc = find_scene(REFS[i].scene);
        if (sc == NULL) {
            printf("FAIL: scene %s is in the table but not in frame_scenes.inc\n", REFS[i].scene);
            failures++;
            continue;
        }
        fc_setup(sc);
        fc_run(sc, REFS[i].clk_ms, fb);
        const uint64_t h = fc_hash(fb);
        checks++;
        if (h != REFS[i].hash) {
            printf("FAIL %s clk=%lld: frame hash %016llx, expected %016llx\n", REFS[i].scene,
                   (long long)REFS[i].clk_ms, (unsigned long long)h,
                   (unsigned long long)REFS[i].hash);
            failures++;
        }
    }
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
        fc_run(sc, SPLITS[r].clk_ms, fb);
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
