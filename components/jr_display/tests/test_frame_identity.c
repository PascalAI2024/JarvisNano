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
    if (failures) {
        printf("%d failure(s) of %d checks\n", failures, checks);
        return 1;
    }
    printf("frame identity passed (%d checks)\n", checks);
    return 0;
}
