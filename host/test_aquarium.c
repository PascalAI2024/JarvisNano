/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_aquarium.c — Unity unit tests for jr_core/aquarium (Nano Reef
 * persistent virtual aquarium simulation). Pure C11, no ESP-IDF.
 */
#include "unity.h"
#include "jr_core/aquarium.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint32_t test_fnv1a_32(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0u; i < len; ++i) {
        h ^= (uint32_t)data[i];
        h *= 16777619u;
    }
    return h;
}

static void test_put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static void test_aq_init_default_starter_tank(void)
{
    jr_aq_init_default(NULL, 1000u);

    jr_aq_tank_t t;
    memset(&t, 0xA5, sizeof(t));
    const uint32_t t0 = 1795000000u;
    jr_aq_init_default(&t, t0);

    TEST_ASSERT_EQUAL_UINT8(JR_AQ_BIOME_REEF, t.biome);
    TEST_ASSERT_EQUAL_UINT16(2u, t.pearls);
    TEST_ASSERT_EQUAL_UINT16(20u, t.coral_xp);
    TEST_ASSERT_EQUAL_UINT8(1u, jr_aq_coral_level(&t));
    TEST_ASSERT_EQUAL_UINT8(0u, t.algae);
    TEST_ASSERT_EQUAL_UINT32(0u, t.total_feeds);
    TEST_ASSERT_EQUAL_UINT32(t0, t.last_tick_epoch_s);
    TEST_ASSERT_EQUAL_UINT8(3u, t.fish_count);

    TEST_ASSERT_TRUE(t.fish[0].active);
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_SP_CLOWNFISH, t.fish[0].species);
    TEST_ASSERT_EQUAL_STRING("NEMO", t.fish[0].name);
    TEST_ASSERT_EQUAL_UINT16(20u, t.fish[0].xp);
    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_FRY, jr_aq_fish_stage(&t.fish[0]));
    TEST_ASSERT_EQUAL_UINT8(25u, t.fish[0].hunger);
    TEST_ASSERT_EQUAL_UINT8(85u, t.fish[0].happiness);
    TEST_ASSERT_EQUAL_UINT32(t0, t.fish[0].born_epoch_s);

    TEST_ASSERT_TRUE(t.fish[1].active);
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_SP_BLUE_TANG, t.fish[1].species);
    TEST_ASSERT_EQUAL_STRING("DORY", t.fish[1].name);
    TEST_ASSERT_EQUAL_UINT16(45u, t.fish[1].xp);
    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_JUVENILE, jr_aq_fish_stage(&t.fish[1]));

    TEST_ASSERT_TRUE(t.fish[2].active);
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_SP_NEON_TETRA, t.fish[2].species);
    TEST_ASSERT_EQUAL_STRING("NOVA", t.fish[2].name);
    TEST_ASSERT_EQUAL_UINT16(110u, t.fish[2].xp);
    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_ADULT, jr_aq_fish_stage(&t.fish[2]));

    TEST_ASSERT_NOT_EQUAL(0u, t.fish[0].seed);
    TEST_ASSERT_NOT_EQUAL(0u, t.fish[1].seed);
    TEST_ASSERT_NOT_EQUAL(0u, t.fish[2].seed);
    TEST_ASSERT_NOT_EQUAL(t.fish[0].seed, t.fish[1].seed);
    TEST_ASSERT_NOT_EQUAL(t.fish[1].seed, t.fish[2].seed);
    TEST_ASSERT_NOT_EQUAL(t.fish[0].seed, t.fish[2].seed);

    for (size_t i = 3u; i < JR_AQ_MAX_FISH; ++i) {
        TEST_ASSERT_FALSE(t.fish[i].active);
    }
}

static void test_aq_add_fish_sanitization_defaults_and_cap(void)
{
    jr_aq_tank_t t;
    const uint32_t t0 = 1795000000u;
    jr_aq_init_default(&t, t0);

    /* Invalid tank or species rejected cleanly. */
    TEST_ASSERT_EQUAL_INT(-1, jr_aq_add_fish(NULL, JR_AQ_SP_GOLDEN_KOI, "KOI", t0));
    TEST_ASSERT_EQUAL_INT(-1, jr_aq_add_fish(&t, JR_AQ_SP_COUNT, "BAD", t0));
    TEST_ASSERT_EQUAL_INT(-1, jr_aq_add_fish(&t, (jr_aq_species_t)99, "BAD", t0));

    /* Lowercase, whitespace collapse, punctuation stripping, and 13-char cap. */
    const int i3 = jr_aq_add_fish(&t, JR_AQ_SP_GOLDEN_KOI, "  mochi-99!  ", t0 + 10u);
    TEST_ASSERT_EQUAL_INT(3, i3);
    TEST_ASSERT_EQUAL_STRING("MOCHI99", t.fish[3].name);
    TEST_ASSERT_EQUAL_UINT16(0u, t.fish[3].xp);
    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_FRY, jr_aq_fish_stage(&t.fish[3]));
    TEST_ASSERT_EQUAL_UINT8(25u, t.fish[3].hunger);
    TEST_ASSERT_EQUAL_UINT8(85u, t.fish[3].happiness);

    const int i4 = jr_aq_add_fish(&t, JR_AQ_SP_MANDARIN, "  captain   fishbeard  ", t0 + 20u);
    TEST_ASSERT_EQUAL_INT(4, i4);
    TEST_ASSERT_EQUAL_STRING("CAPTAIN FISHB", t.fish[4].name);
    TEST_ASSERT_EQUAL_UINT(JR_AQ_NAME_CAP - 1u, (unsigned)strlen(t.fish[4].name));

    /* NULL, empty, and punctuation-only names auto-pick cute unique defaults. */
    const int i5 = jr_aq_add_fish(&t, JR_AQ_SP_JELLYFISH, NULL, t0 + 30u);
    const int i6 = jr_aq_add_fish(&t, JR_AQ_SP_MANTA_RAY, "", t0 + 40u);
    const int i7 = jr_aq_add_fish(&t, JR_AQ_SP_SEAHORSE, "  !!!  ", t0 + 50u);
    TEST_ASSERT_EQUAL_INT(5, i5);
    TEST_ASSERT_EQUAL_INT(6, i6);
    TEST_ASSERT_EQUAL_INT(7, i7);
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_MAX_FISH, t.fish_count);

    TEST_ASSERT_TRUE(t.fish[5].name[0] != '\0');
    TEST_ASSERT_TRUE(t.fish[6].name[0] != '\0');
    TEST_ASSERT_TRUE(t.fish[7].name[0] != '\0');
    TEST_ASSERT_NOT_EQUAL(0, strcmp(t.fish[5].name, t.fish[6].name));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(t.fish[6].name, t.fish[7].name));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(t.fish[5].name, t.fish[7].name));

    /* Every active fish has a unique non-zero visual seed. */
    for (size_t i = 0u; i < JR_AQ_MAX_FISH; ++i) {
        TEST_ASSERT_TRUE(t.fish[i].active);
        TEST_ASSERT_NOT_EQUAL(0u, t.fish[i].seed);
        for (size_t j = i + 1u; j < JR_AQ_MAX_FISH; ++j) {
            TEST_ASSERT_NOT_EQUAL(t.fish[i].seed, t.fish[j].seed);
        }
    }

    /* Tank full -> -1. */
    TEST_ASSERT_EQUAL_INT(-1, jr_aq_add_fish(&t, JR_AQ_SP_CLOWNFISH, "EXTRA", t0 + 60u));
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_MAX_FISH, t.fish_count);
}

static void test_aq_remove_fish_by_index_and_name(void)
{
    jr_aq_tank_t t;
    jr_aq_init_default(&t, 1000u);

    TEST_ASSERT_FALSE(jr_aq_remove_fish(NULL, 0u));
    TEST_ASSERT_FALSE(jr_aq_remove_fish(&t, 3u));
    TEST_ASSERT_FALSE(jr_aq_remove_fish(&t, 99u));

    /* Remove middle fish ("DORY") -> "NOVA" compacts into index 1. */
    TEST_ASSERT_TRUE(jr_aq_remove_fish(&t, 1u));
    TEST_ASSERT_EQUAL_UINT8(2u, t.fish_count);
    TEST_ASSERT_EQUAL_STRING("NEMO", t.fish[0].name);
    TEST_ASSERT_EQUAL_STRING("NOVA", t.fish[1].name);
    TEST_ASSERT_FALSE(t.fish[2].active);

    /* Remove by name is case-insensitive and trims whitespace. */
    TEST_ASSERT_FALSE(jr_aq_remove_fish_by_name(NULL, "NEMO"));
    TEST_ASSERT_FALSE(jr_aq_remove_fish_by_name(&t, NULL));
    TEST_ASSERT_FALSE(jr_aq_remove_fish_by_name(&t, "   "));
    TEST_ASSERT_FALSE(jr_aq_remove_fish_by_name(&t, "DORY"));
    TEST_ASSERT_TRUE(jr_aq_remove_fish_by_name(&t, "  nova  "));
    TEST_ASSERT_EQUAL_UINT8(1u, t.fish_count);
    TEST_ASSERT_EQUAL_STRING("NEMO", t.fish[0].name);
    TEST_ASSERT_FALSE(t.fish[1].active);

    TEST_ASSERT_TRUE(jr_aq_remove_fish_by_name(&t, "nemo"));
    TEST_ASSERT_EQUAL_UINT8(0u, t.fish_count);
    TEST_ASSERT_FALSE(t.fish[0].active);
    TEST_ASSERT_FALSE(jr_aq_remove_fish(&t, 0u));

    /* Adding after emptying starts cleanly at index 0. */
    TEST_ASSERT_EQUAL_INT(0, jr_aq_add_fish(&t, JR_AQ_SP_SEAHORSE, "PIPER", 2000u));
    TEST_ASSERT_EQUAL_UINT8(1u, t.fish_count);
    TEST_ASSERT_EQUAL_STRING("PIPER", t.fish[0].name);
}

static void test_aq_feed_growth_hunger_happiness_and_caps(void)
{
    jr_aq_tank_t t;
    const uint32_t t0 = 1795000000u;
    jr_aq_init_default(&t, t0);

    TEST_ASSERT_EQUAL_UINT8(0u, jr_aq_feed(NULL, t0));

    /* First feed promotes NEMO from FRY (20) -> JUVENILE (32). */
    const uint8_t fed = jr_aq_feed(&t, t0);
    TEST_ASSERT_EQUAL_UINT8(3u, fed);
    TEST_ASSERT_EQUAL_UINT16(32u, t.fish[0].xp);
    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_JUVENILE, jr_aq_fish_stage(&t.fish[0]));
    TEST_ASSERT_EQUAL_UINT16(1u, t.fish[0].feedings);
    TEST_ASSERT_EQUAL_UINT8(0u, t.fish[0].hunger);
    TEST_ASSERT_EQUAL_UINT8(95u, t.fish[0].happiness);
    TEST_ASSERT_EQUAL_UINT16(57u, t.fish[1].xp);
    TEST_ASSERT_EQUAL_UINT16(122u, t.fish[2].xp);
    TEST_ASSERT_EQUAL_UINT16(24u, t.coral_xp);
    TEST_ASSERT_EQUAL_UINT32(1u, t.total_feeds);

    /* Hunger floor and happiness/XP caps hold under repeated feeding. */
    t.fish[0].xp = 64995u;
    t.fish[0].hunger = 50u;
    t.fish[0].happiness = 96u;
    TEST_ASSERT_EQUAL_UINT8(3u, jr_aq_feed(&t, t0));
    TEST_ASSERT_EQUAL_UINT16(65000u, t.fish[0].xp);
    TEST_ASSERT_EQUAL_UINT8(15u, t.fish[0].hunger);
    TEST_ASSERT_EQUAL_UINT8(100u, t.fish[0].happiness);
    TEST_ASSERT_EQUAL_UINT16(28u, t.coral_xp);
    TEST_ASSERT_EQUAL_UINT32(2u, t.total_feeds);

    /* Feeding an empty tank returns 0 and leaves totals unchanged. */
    while (t.fish_count > 0u) {
        TEST_ASSERT_TRUE(jr_aq_remove_fish(&t, 0u));
    }
    TEST_ASSERT_EQUAL_UINT8(0u, jr_aq_feed(&t, t0));
    TEST_ASSERT_EQUAL_UINT32(2u, t.total_feeds);
    TEST_ASSERT_EQUAL_UINT16(28u, t.coral_xp);
}

static void test_aq_advance_time_cozy_growth_algae_and_rtc_clamp(void)
{
    jr_aq_advance_time(NULL, 1000u);

    jr_aq_tank_t t;
    const uint32_t t0 = 1795003200u; /* divisible by 14400 (4h) for clean bucket check */
    jr_aq_init_default(&t, t0);
    const int fry_idx = jr_aq_add_fish(&t, JR_AQ_SP_SEAHORSE, "BABY", t0);
    TEST_ASSERT_EQUAL_INT(3, fry_idx);

    /* Sub-hour ticks do not lose fractional time. */
    jr_aq_advance_time(&t, t0 + 1800u);
    TEST_ASSERT_EQUAL_UINT16(0u, t.fish[fry_idx].xp);
    TEST_ASSERT_EQUAL_UINT32(t0, t.last_tick_epoch_s);
    jr_aq_advance_time(&t, t0 + 3599u);
    TEST_ASSERT_EQUAL_UINT16(0u, t.fish[fry_idx].xp);

    /* Backward clock is a safe no-op. */
    jr_aq_advance_time(&t, t0 - 500u);
    TEST_ASSERT_EQUAL_UINT16(0u, t.fish[fry_idx].xp);

    /* 15 consecutive 1-hour ticks grow Fry (0 -> 30 = JUVENILE) and accumulate algae (+3). */
    for (uint32_t h = 1u; h <= 15u; ++h) {
        jr_aq_advance_time(&t, t0 + h * 3600u);
    }
    TEST_ASSERT_EQUAL_UINT16(30u, t.fish[fry_idx].xp);
    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_JUVENILE, jr_aq_fish_stage(&t.fish[fry_idx]));
    TEST_ASSERT_EQUAL_UINT8(85u, t.fish[fry_idx].hunger);
    TEST_ASSERT_EQUAL_UINT8(3u, t.algae);

    /* +1 more hour crosses the 16h mark -> algae = 4. */
    jr_aq_advance_time(&t, t0 + 16u * 3600u);
    TEST_ASSERT_EQUAL_UINT8(4u, t.algae);
    TEST_ASSERT_EQUAL_UINT16(32u, t.fish[fry_idx].xp);

    /* 50h total -> Adult (xp=100); 125h total -> Elder (xp=250). */
    jr_aq_advance_time(&t, t0 + 50u * 3600u);
    TEST_ASSERT_EQUAL_UINT16(100u, t.fish[fry_idx].xp);
    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_ADULT, jr_aq_fish_stage(&t.fish[fry_idx]));

    jr_aq_advance_time(&t, t0 + 125u * 3600u);
    TEST_ASSERT_EQUAL_UINT16(250u, t.fish[fry_idx].xp);
    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_ELDER, jr_aq_fish_stage(&t.fish[fry_idx]));

    /* Huge multi-year RTC jump clamps safely, never kills fish, never overflows. */
    const uint32_t huge_t = t0 + 100000000u;
    jr_aq_advance_time(&t, huge_t);
    TEST_ASSERT_EQUAL_UINT8(4u, t.fish_count);
    TEST_ASSERT_EQUAL_UINT8(100u, t.algae);
    for (size_t i = 0u; i < 4u; ++i) {
        TEST_ASSERT_TRUE(t.fish[i].active);
        TEST_ASSERT_EQUAL_UINT8(100u, t.fish[i].hunger);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(30u, t.fish[i].happiness);
        TEST_ASSERT_LESS_OR_EQUAL_UINT16(65000u, t.fish[i].xp);
    }
    /* Follow-up tick right after huge jump must NOT re-apply another clamp jump. */
    const uint16_t xp_after = t.fish[fry_idx].xp;
    jr_aq_advance_time(&t, huge_t + 60u);
    TEST_ASSERT_EQUAL_UINT16(xp_after, t.fish[fry_idx].xp);

    /* Pre-SNTP boot (epoch 0) syncing to real Unix epoch anchors cleanly without fake jump. */
    jr_aq_tank_t boot_tank;
    jr_aq_init_default(&boot_tank, 0u);
    jr_aq_advance_time(&boot_tank, 1795000000u);
    TEST_ASSERT_EQUAL_UINT32(1795000000u, boot_tank.last_tick_epoch_s);
    TEST_ASSERT_EQUAL_UINT32(1795000000u, boot_tank.fish[0].born_epoch_s);
    TEST_ASSERT_EQUAL_UINT16(20u, boot_tank.fish[0].xp);
    TEST_ASSERT_EQUAL_UINT8(0u, boot_tank.algae);
}

static void test_aq_clean_and_reward_productivity(void)
{
    jr_aq_clean(NULL);
    jr_aq_reward_productivity(NULL, 1u, 10u, 1000u);

    jr_aq_tank_t t;
    const uint32_t t0 = 1795000000u;
    jr_aq_init_default(&t, t0);
    t.algae = 42u;
    t.fish[0].happiness = 70u;

    jr_aq_clean(&t);
    TEST_ASSERT_EQUAL_UINT8(0u, t.algae);
    TEST_ASSERT_EQUAL_UINT8(75u, t.fish[0].happiness);

    /* Cleaning an already-clean tank is a safe no-op on happiness. */
    jr_aq_clean(&t);
    TEST_ASSERT_EQUAL_UINT8(0u, t.algae);
    TEST_ASSERT_EQUAL_UINT8(75u, t.fish[0].happiness);

    /* Productivity reward grows pearls, coral_xp (level 1 -> 2), and fish XP. */
    jr_aq_reward_productivity(&t, 3u, 20u, t0);
    TEST_ASSERT_EQUAL_UINT16(5u, t.pearls);
    TEST_ASSERT_EQUAL_UINT16(40u, t.coral_xp);
    TEST_ASSERT_EQUAL_UINT8(2u, jr_aq_coral_level(&t));
    TEST_ASSERT_EQUAL_UINT16(30u, t.fish[0].xp);
    TEST_ASSERT_EQUAL_UINT8(80u, t.fish[0].happiness);

    /* Saturates safely on large bonuses. */
    jr_aq_reward_productivity(&t, 65000u, 65000u, t0);
    TEST_ASSERT_EQUAL_UINT16(65000u, t.pearls);
    TEST_ASSERT_EQUAL_UINT16(65000u, t.coral_xp);
    TEST_ASSERT_EQUAL_UINT8(5u, jr_aq_coral_level(&t));
}

static void test_aq_biomes_stages_coral_and_age(void)
{
    jr_aq_tank_t t;
    const uint32_t t0 = 1795000000u;
    jr_aq_init_default(&t, t0);

    jr_aq_set_biome(NULL, JR_AQ_BIOME_ABYSS);
    jr_aq_set_biome(&t, JR_AQ_BIOME_ABYSS);
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_BIOME_ABYSS, t.biome);
    jr_aq_set_biome(&t, JR_AQ_BIOME_COUNT); /* invalid -> unchanged */
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_BIOME_ABYSS, t.biome);

    jr_aq_step_biome(NULL, 1);
    jr_aq_step_biome(&t, 1);
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_BIOME_KOI, t.biome);
    jr_aq_step_biome(&t, 1);
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_BIOME_REEF, t.biome);
    jr_aq_step_biome(&t, -1);
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_BIOME_KOI, t.biome);
    jr_aq_step_biome(&t, -7);
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_BIOME_ABYSS, t.biome);
    jr_aq_step_biome(&t, 0);
    TEST_ASSERT_EQUAL_UINT8(JR_AQ_BIOME_ABYSS, t.biome);

    /* Exact stage boundaries. */
    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_FRY, jr_aq_fish_stage(NULL));
    jr_aq_fish_t f;
    memset(&f, 0, sizeof(f));
    f.xp = 0u;     TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_FRY, jr_aq_fish_stage(&f));
    f.xp = 29u;    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_FRY, jr_aq_fish_stage(&f));
    f.xp = 30u;    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_JUVENILE, jr_aq_fish_stage(&f));
    f.xp = 99u;    TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_JUVENILE, jr_aq_fish_stage(&f));
    f.xp = 100u;   TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_ADULT, jr_aq_fish_stage(&f));
    f.xp = 249u;   TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_ADULT, jr_aq_fish_stage(&f));
    f.xp = 250u;   TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_ELDER, jr_aq_fish_stage(&f));
    f.xp = 65000u; TEST_ASSERT_EQUAL_INT(JR_AQ_STAGE_ELDER, jr_aq_fish_stage(&f));

    /* Exact coral level boundaries. */
    TEST_ASSERT_EQUAL_UINT8(1u, jr_aq_coral_level(NULL));
    t.coral_xp = 0u;   TEST_ASSERT_EQUAL_UINT8(1u, jr_aq_coral_level(&t));
    t.coral_xp = 39u;  TEST_ASSERT_EQUAL_UINT8(1u, jr_aq_coral_level(&t));
    t.coral_xp = 40u;  TEST_ASSERT_EQUAL_UINT8(2u, jr_aq_coral_level(&t));
    t.coral_xp = 99u;  TEST_ASSERT_EQUAL_UINT8(2u, jr_aq_coral_level(&t));
    t.coral_xp = 100u; TEST_ASSERT_EQUAL_UINT8(3u, jr_aq_coral_level(&t));
    t.coral_xp = 199u; TEST_ASSERT_EQUAL_UINT8(3u, jr_aq_coral_level(&t));
    t.coral_xp = 200u; TEST_ASSERT_EQUAL_UINT8(4u, jr_aq_coral_level(&t));
    t.coral_xp = 349u; TEST_ASSERT_EQUAL_UINT8(4u, jr_aq_coral_level(&t));
    t.coral_xp = 350u; TEST_ASSERT_EQUAL_UINT8(5u, jr_aq_coral_level(&t));

    /* Fish age in hours. */
    TEST_ASSERT_EQUAL_UINT32(0u, jr_aq_fish_age_hours(NULL, t0 + 7200u));
    TEST_ASSERT_EQUAL_UINT32(0u, jr_aq_fish_age_hours(&t.fish[0], t0));
    TEST_ASSERT_EQUAL_UINT32(0u, jr_aq_fish_age_hours(&t.fish[0], t0 - 10u));
    TEST_ASSERT_EQUAL_UINT32(0u, jr_aq_fish_age_hours(&t.fish[0], t0 + 3599u));
    TEST_ASSERT_EQUAL_UINT32(1u, jr_aq_fish_age_hours(&t.fish[0], t0 + 3600u));
    TEST_ASSERT_EQUAL_UINT32(48u, jr_aq_fish_age_hours(&t.fish[0], t0 + 48u * 3600u));
    TEST_ASSERT_EQUAL_UINT32(0u, jr_aq_fish_age_hours(&t.fish[7], t0 + 7200u)); /* inactive */
}

static void test_aq_species_biome_stage_names_and_parsers(void)
{
    /* Canonical species roundtrip. */
    for (int i = 0; i < (int)JR_AQ_SP_COUNT; ++i) {
        const jr_aq_species_t sp = (jr_aq_species_t)i;
        const char *name = jr_aq_species_name(sp);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_NOT_EQUAL(0, strcmp(name, "unknown"));
        jr_aq_species_t parsed = JR_AQ_SP_COUNT;
        TEST_ASSERT_TRUE_MESSAGE(jr_aq_species_parse(name, &parsed), name);
        TEST_ASSERT_EQUAL_INT(sp, parsed);
    }
    TEST_ASSERT_EQUAL_STRING("unknown", jr_aq_species_name(JR_AQ_SP_COUNT));

    /* Friendly species aliases. */
    static const struct {
        const char     *text;
        jr_aq_species_t expect;
    } kSpAliases[] = {
        { "clown",         JR_AQ_SP_CLOWNFISH  },
        { "CLOWNFISH",     JR_AQ_SP_CLOWNFISH  },
        { "tang",          JR_AQ_SP_BLUE_TANG  },
        { "  Blue Tang ",  JR_AQ_SP_BLUE_TANG  },
        { "blue-tang",     JR_AQ_SP_BLUE_TANG  },
        { "tetra",         JR_AQ_SP_NEON_TETRA },
        { "neon_tetra",    JR_AQ_SP_NEON_TETRA },
        { "koi",           JR_AQ_SP_GOLDEN_KOI },
        { "golden koi",    JR_AQ_SP_GOLDEN_KOI },
        { "mandarin",      JR_AQ_SP_MANDARIN   },
        { "dragonet",      JR_AQ_SP_MANDARIN   },
        { "jelly",         JR_AQ_SP_JELLYFISH  },
        { "jellyfish",     JR_AQ_SP_JELLYFISH  },
        { "manta",         JR_AQ_SP_MANTA_RAY  },
        { "manta_ray",     JR_AQ_SP_MANTA_RAY  },
        { "ray",           JR_AQ_SP_MANTA_RAY  },
        { "seahorse",      JR_AQ_SP_SEAHORSE   },
        { "sea-horse",     JR_AQ_SP_SEAHORSE   },
    };
    for (size_t i = 0u; i < sizeof(kSpAliases) / sizeof(kSpAliases[0]); ++i) {
        jr_aq_species_t sp = JR_AQ_SP_COUNT;
        TEST_ASSERT_TRUE_MESSAGE(jr_aq_species_parse(kSpAliases[i].text, &sp), kSpAliases[i].text);
        TEST_ASSERT_EQUAL_INT_MESSAGE(kSpAliases[i].expect, sp, kSpAliases[i].text);
    }
    TEST_ASSERT_TRUE(jr_aq_species_parse("clown", NULL));
    jr_aq_species_t keep_sp = JR_AQ_SP_MANDARIN;
    TEST_ASSERT_FALSE(jr_aq_species_parse(NULL, &keep_sp));
    TEST_ASSERT_FALSE(jr_aq_species_parse("", &keep_sp));
    TEST_ASSERT_FALSE(jr_aq_species_parse("   ", &keep_sp));
    TEST_ASSERT_FALSE(jr_aq_species_parse("shark", &keep_sp));
    TEST_ASSERT_FALSE(jr_aq_species_parse("this_is_a_very_long_unknown_species_identifier", &keep_sp));
    TEST_ASSERT_EQUAL_INT(JR_AQ_SP_MANDARIN, keep_sp);

    /* Canonical biome roundtrip + aliases. */
    for (int i = 0; i < (int)JR_AQ_BIOME_COUNT; ++i) {
        const jr_aq_biome_t b = (jr_aq_biome_t)i;
        const char *name = jr_aq_biome_name(b);
        jr_aq_biome_t parsed = JR_AQ_BIOME_COUNT;
        TEST_ASSERT_TRUE_MESSAGE(jr_aq_biome_parse(name, &parsed), name);
        TEST_ASSERT_EQUAL_INT(b, parsed);
    }
    TEST_ASSERT_EQUAL_STRING("unknown", jr_aq_biome_name(JR_AQ_BIOME_COUNT));

    static const struct {
        const char   *text;
        jr_aq_biome_t expect;
    } kBiomeAliases[] = {
        { "reef",          JR_AQ_BIOME_REEF  },
        { "Tropical",      JR_AQ_BIOME_REEF  },
        { "abyss",         JR_AQ_BIOME_ABYSS },
        { "jellyfish",     JR_AQ_BIOME_ABYSS },
        { "  DEEP  ",      JR_AQ_BIOME_ABYSS },
        { "koi",           JR_AQ_BIOME_KOI   },
        { "pond",          JR_AQ_BIOME_KOI   },
        { "zen",           JR_AQ_BIOME_KOI   },
        { "zen-koi-pond",  JR_AQ_BIOME_KOI   },
    };
    for (size_t i = 0u; i < sizeof(kBiomeAliases) / sizeof(kBiomeAliases[0]); ++i) {
        jr_aq_biome_t b = JR_AQ_BIOME_COUNT;
        TEST_ASSERT_TRUE_MESSAGE(jr_aq_biome_parse(kBiomeAliases[i].text, &b), kBiomeAliases[i].text);
        TEST_ASSERT_EQUAL_INT_MESSAGE(kBiomeAliases[i].expect, b, kBiomeAliases[i].text);
    }
    TEST_ASSERT_TRUE(jr_aq_biome_parse("zen", NULL));
    jr_aq_biome_t keep_b = JR_AQ_BIOME_KOI;
    TEST_ASSERT_FALSE(jr_aq_biome_parse(NULL, &keep_b));
    TEST_ASSERT_FALSE(jr_aq_biome_parse("", &keep_b));
    TEST_ASSERT_FALSE(jr_aq_biome_parse("desert", &keep_b));
    TEST_ASSERT_EQUAL_INT(JR_AQ_BIOME_KOI, keep_b);

    /* Stage names. */
    TEST_ASSERT_EQUAL_STRING("fry", jr_aq_stage_name(JR_AQ_STAGE_FRY));
    TEST_ASSERT_EQUAL_STRING("juvenile", jr_aq_stage_name(JR_AQ_STAGE_JUVENILE));
    TEST_ASSERT_EQUAL_STRING("adult", jr_aq_stage_name(JR_AQ_STAGE_ADULT));
    TEST_ASSERT_EQUAL_STRING("elder", jr_aq_stage_name(JR_AQ_STAGE_ELDER));
    TEST_ASSERT_EQUAL_STRING("unknown", jr_aq_stage_name((jr_aq_stage_t)99));
}

static void assert_tanks_equal(const jr_aq_tank_t *a, const jr_aq_tank_t *b)
{
    TEST_ASSERT_EQUAL_UINT8(a->biome, b->biome);
    TEST_ASSERT_EQUAL_UINT16(a->pearls, b->pearls);
    TEST_ASSERT_EQUAL_UINT16(a->coral_xp, b->coral_xp);
    TEST_ASSERT_EQUAL_UINT8(a->algae, b->algae);
    TEST_ASSERT_EQUAL_UINT32(a->total_feeds, b->total_feeds);
    TEST_ASSERT_EQUAL_UINT32(a->last_tick_epoch_s, b->last_tick_epoch_s);
    TEST_ASSERT_EQUAL_UINT8(a->fish_count, b->fish_count);
    for (size_t i = 0u; i < JR_AQ_MAX_FISH; ++i) {
        TEST_ASSERT_EQUAL(a->fish[i].active, b->fish[i].active);
        if (a->fish[i].active) {
            TEST_ASSERT_EQUAL_UINT8(a->fish[i].species, b->fish[i].species);
            TEST_ASSERT_EQUAL_STRING(a->fish[i].name, b->fish[i].name);
            TEST_ASSERT_EQUAL_UINT32(a->fish[i].born_epoch_s, b->fish[i].born_epoch_s);
            TEST_ASSERT_EQUAL_UINT16(a->fish[i].xp, b->fish[i].xp);
            TEST_ASSERT_EQUAL_UINT16(a->fish[i].feedings, b->fish[i].feedings);
            TEST_ASSERT_EQUAL_UINT8(a->fish[i].hunger, b->fish[i].hunger);
            TEST_ASSERT_EQUAL_UINT8(a->fish[i].happiness, b->fish[i].happiness);
            TEST_ASSERT_EQUAL_UINT16(a->fish[i].seed, b->fish[i].seed);
        }
    }
}

static void test_aq_blob_pack_unpack_roundtrip_and_corruption(void)
{
    uint8_t buf[JR_AQ_BLOB_MAX];
    uint8_t buf2[JR_AQ_BLOB_MAX];
    jr_aq_tank_t src;
    const uint32_t t0 = 1795000000u;

    /* Full tank (8 distinct species, modified state). */
    jr_aq_init_default(&src, t0);
    jr_aq_set_biome(&src, JR_AQ_BIOME_KOI);
    jr_aq_add_fish(&src, JR_AQ_SP_GOLDEN_KOI, "SUNNY", t0 + 100u);
    jr_aq_add_fish(&src, JR_AQ_SP_MANDARIN, "PRISM", t0 + 200u);
    jr_aq_add_fish(&src, JR_AQ_SP_JELLYFISH, "LUNA", t0 + 300u);
    jr_aq_add_fish(&src, JR_AQ_SP_MANTA_RAY, "GLIDE", t0 + 400u);
    jr_aq_add_fish(&src, JR_AQ_SP_SEAHORSE, "PIP", t0 + 500u);
    jr_aq_feed(&src, t0 + 3600u * 6u);
    jr_aq_reward_productivity(&src, 7u, 55u, t0 + 3600u * 12u);

    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)jr_aq_blob_pack(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)jr_aq_blob_pack(&src, NULL, sizeof(buf)));

    const size_t n = jr_aq_blob_pack(&src, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_UINT(28u, (unsigned)n);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(JR_AQ_BLOB_MAX, (unsigned)n);
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)jr_aq_blob_pack(&src, buf2, n - 1u));

    /* Roundtrip unpack matches every field and re-packs byte-identically. */
    jr_aq_tank_t dst;
    memset(&dst, 0xCC, sizeof(dst));
    TEST_ASSERT_TRUE(jr_aq_blob_unpack(&dst, buf, n));
    assert_tanks_equal(&src, &dst);

    const size_t n2 = jr_aq_blob_pack(&dst, buf2, sizeof(buf2));
    TEST_ASSERT_EQUAL_UINT((unsigned)n, (unsigned)n2);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(buf, buf2, n);

    /* Empty tank (0 fish) roundtrips cleanly. */
    jr_aq_tank_t empty_src;
    jr_aq_init_default(&empty_src, t0);
    while (empty_src.fish_count > 0u) {
        TEST_ASSERT_TRUE(jr_aq_remove_fish(&empty_src, 0u));
    }
    const size_t ne = jr_aq_blob_pack(&empty_src, buf2, sizeof(buf2));
    TEST_ASSERT_EQUAL_UINT(28u, (unsigned)ne);
    jr_aq_tank_t empty_dst;
    TEST_ASSERT_TRUE(jr_aq_blob_unpack(&empty_dst, buf2, ne));
    assert_tanks_equal(&empty_src, &empty_dst);

    /* Sentinel tank must stay untouched on any unpack failure. */
    jr_aq_tank_t sentinel;
    jr_aq_init_default(&sentinel, 12345u);
    jr_aq_tank_t guard = sentinel;

    TEST_ASSERT_FALSE(jr_aq_blob_unpack(NULL, buf, n));
    TEST_ASSERT_FALSE(jr_aq_blob_unpack(&guard, NULL, n));
    TEST_ASSERT_FALSE(jr_aq_blob_unpack(&guard, buf, 0u));
    TEST_ASSERT_FALSE(jr_aq_blob_unpack(&guard, buf, n - 1u));
    TEST_ASSERT_FALSE(jr_aq_blob_unpack(&guard, buf, JR_AQ_BLOB_MAX + 1u));
    assert_tanks_equal(&sentinel, &guard);

    /* Flipping every single byte in the blob is caught by FNV-1a checksum. */
    for (size_t i = 0u; i < n; ++i) {
        buf[i] ^= 0x01u;
        TEST_ASSERT_FALSE(jr_aq_blob_unpack(&guard, buf, n));
        buf[i] ^= 0x01u;
    }
    assert_tanks_equal(&sentinel, &guard);

    /* Invalid version / invalid biome / invalid species with recomputed valid CRC still fail! */
    uint8_t bad[JR_AQ_BLOB_MAX];
    memcpy(bad, buf, n);
    bad[4] = (uint8_t)(JR_AQ_BLOB_VER + 1u);
    test_put_u32_le(bad + n - 4u, test_fnv1a_32(bad, n - 4u));
    TEST_ASSERT_FALSE(jr_aq_blob_unpack(&guard, bad, n));

    memcpy(bad, buf, n);
    bad[6] = (uint8_t)JR_AQ_BIOME_COUNT;
    test_put_u32_le(bad + n - 4u, test_fnv1a_32(bad, n - 4u));
    TEST_ASSERT_FALSE(jr_aq_blob_unpack(&guard, bad, n));

    memcpy(bad, buf, n);
    bad[24] = (uint8_t)JR_AQ_SP_COUNT; /* first fish species out of range */
    test_put_u32_le(bad + n - 4u, test_fnv1a_32(bad, n - 4u));
    TEST_ASSERT_FALSE(jr_aq_blob_unpack(&guard, bad, n));

    assert_tanks_equal(&sentinel, &guard);
}

void aquarium_tests_run(void)
{
    RUN_TEST(test_aq_init_default_starter_tank);
    RUN_TEST(test_aq_add_fish_sanitization_defaults_and_cap);
    RUN_TEST(test_aq_remove_fish_by_index_and_name);
    RUN_TEST(test_aq_feed_growth_hunger_happiness_and_caps);
    RUN_TEST(test_aq_advance_time_cozy_growth_algae_and_rtc_clamp);
    RUN_TEST(test_aq_clean_and_reward_productivity);
    RUN_TEST(test_aq_biomes_stages_coral_and_age);
    RUN_TEST(test_aq_species_biome_stage_names_and_parsers);
    RUN_TEST(test_aq_blob_pack_unpack_roundtrip_and_corruption);
}
