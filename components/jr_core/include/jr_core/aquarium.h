/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/aquarium.h — Nano Reef Persistent Virtual Aquarium Simulation.
 *
 * Pure C11: zero ESP-IDF includes, no heap allocation, no libm. Holds up to
 * JR_AQ_MAX_FISH (8) living creatures across 3 biomes with cozy wall-clock
 * growth (fish never die), coral progression, productivity rewards, and
 * versioned FNV-1a checksummed binary NVS persistence.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_AQ_MAX_FISH     8U
#define JR_AQ_NAME_CAP    14U
#define JR_AQ_BLOB_MAGIC  0x4E524546u  /* 'NREF' */
#define JR_AQ_BLOB_VER    1U
#define JR_AQ_BLOB_MAX    512U

typedef enum {
    JR_AQ_SP_CLOWNFISH = 0,
    JR_AQ_SP_BLUE_TANG = 1,
    JR_AQ_SP_NEON_TETRA = 2,
    JR_AQ_SP_GOLDEN_KOI = 3,
    JR_AQ_SP_MANDARIN = 4,
    JR_AQ_SP_JELLYFISH = 5,
    JR_AQ_SP_MANTA_RAY = 6,
    JR_AQ_SP_SEAHORSE = 7,
    JR_AQ_SP_COUNT = 8,
} jr_aq_species_t;

typedef enum {
    JR_AQ_BIOME_REEF  = 0,  /* Tropical coral reef */
    JR_AQ_BIOME_ABYSS = 1,  /* Bioluminescent deep-sea jellyfish abyss */
    JR_AQ_BIOME_KOI   = 2,  /* Zen top-down koi pond */
    JR_AQ_BIOME_COUNT = 3,
} jr_aq_biome_t;

typedef enum {
    JR_AQ_STAGE_FRY      = 0,  /* xp < 30   (0.62x baby) */
    JR_AQ_STAGE_JUVENILE = 1,  /* xp 30..99 (0.82x) */
    JR_AQ_STAGE_ADULT    = 2,  /* xp 100..249 (1.0x) */
    JR_AQ_STAGE_ELDER    = 3,  /* xp >= 250 (1.22x majestic + shimmer) */
} jr_aq_stage_t;

typedef struct {
    bool     active;
    uint8_t  species;          /* jr_aq_species_t */
    char     name[JR_AQ_NAME_CAP];
    uint32_t born_epoch_s;
    uint16_t xp;               /* growth XP (wall time + feedings + sprints) */
    uint16_t feedings;
    uint8_t  hunger;           /* 0=full .. 100=hungry (cozy: fish never die!) */
    uint8_t  happiness;        /* 0..100 */
    uint16_t seed;             /* unique visual seed */
} jr_aq_fish_t;

typedef struct {
    uint8_t  biome;            /* jr_aq_biome_t */
    uint16_t pearls;           /* earned from Focus/Breathe/Inbox completions */
    uint16_t coral_xp;         /* grows coral_level 1..5 */
    uint8_t  algae;            /* 0..100 (builds slowly over days, wiped clean!) */
    uint32_t total_feeds;
    uint32_t last_tick_epoch_s;
    uint8_t  fish_count;
    jr_aq_fish_t fish[JR_AQ_MAX_FISH];
} jr_aq_tank_t;

void jr_aq_init_default(jr_aq_tank_t *t, uint32_t now_epoch_s);
int  jr_aq_add_fish(jr_aq_tank_t *t, jr_aq_species_t sp, const char *name, uint32_t now_epoch_s);
bool jr_aq_remove_fish(jr_aq_tank_t *t, uint8_t idx);
bool jr_aq_remove_fish_by_name(jr_aq_tank_t *t, const char *name);
uint8_t jr_aq_feed(jr_aq_tank_t *t, uint32_t now_epoch_s);
void jr_aq_clean(jr_aq_tank_t *t);
void jr_aq_reward_productivity(jr_aq_tank_t *t, uint16_t pearls, uint16_t coral_bonus, uint32_t now_epoch_s);
void jr_aq_advance_time(jr_aq_tank_t *t, uint32_t now_epoch_s);
void jr_aq_set_biome(jr_aq_tank_t *t, jr_aq_biome_t biome);
void jr_aq_step_biome(jr_aq_tank_t *t, int dir);

jr_aq_stage_t jr_aq_fish_stage(const jr_aq_fish_t *f);
uint8_t jr_aq_coral_level(const jr_aq_tank_t *t);  /* 1..5 */
uint32_t jr_aq_fish_age_hours(const jr_aq_fish_t *f, uint32_t now_epoch_s);

const char *jr_aq_species_name(jr_aq_species_t sp);
bool jr_aq_species_parse(const char *s, jr_aq_species_t *out);
const char *jr_aq_biome_name(jr_aq_biome_t b);
bool jr_aq_biome_parse(const char *s, jr_aq_biome_t *out);
const char *jr_aq_stage_name(jr_aq_stage_t st);

/* Versioned + FNV-1a checksummed binary NVS serialization. */
size_t jr_aq_blob_pack(const jr_aq_tank_t *t, uint8_t *out, size_t cap);
bool   jr_aq_blob_unpack(jr_aq_tank_t *t, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
