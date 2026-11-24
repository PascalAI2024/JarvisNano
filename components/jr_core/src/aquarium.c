/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/aquarium.c — Nano Reef Persistent Virtual Aquarium Simulation.
 *
 * Pure C11: zero ESP-IDF includes, no heap allocation, no libm.
 */
#include "jr_core/aquarium.h"

#include <ctype.h>
#include <string.h>

#define JR_AQ_XP_CAP          65000u
#define JR_AQ_MAX_TICK_HOURS  720u        /* clamp huge RTC jumps to 30 days */
#define JR_AQ_REAL_EPOCH_MIN  1000000000u /* Sep 2001: detects first SNTP sync */

#define JR_AQ_BLOB_HDR_SIZE   24u
#define JR_AQ_BLOB_FISH_SIZE  28u
#define JR_AQ_BLOB_CRC_SIZE   4u

static const char *const kDefaultNames[JR_AQ_MAX_FISH] = {
    "CORAL", "KAI", "LUNA", "FINN", "MOCHI", "ZORA", "RAY", "PIPER",
};

static uint8_t add_u8_sat_u32(uint8_t cur, uint32_t delta, uint8_t cap)
{
    if (cur >= cap) {
        return cap;
    }
    const uint32_t room = (uint32_t)(cap - cur);
    if (delta >= room) {
        return cap;
    }
    return (uint8_t)(cur + (uint8_t)delta);
}

static uint8_t sub_u8_sat_u32(uint8_t cur, uint32_t delta, uint8_t floor_val)
{
    if (cur <= floor_val) {
        return cur;
    }
    const uint32_t avail = (uint32_t)(cur - floor_val);
    if (delta >= avail) {
        return floor_val;
    }
    return (uint8_t)(cur - (uint8_t)delta);
}

static uint16_t add_u16_sat_u32(uint16_t cur, uint32_t delta, uint16_t cap)
{
    if (cur >= cap) {
        return cap;
    }
    const uint32_t room = (uint32_t)(cap - cur);
    if (delta >= room) {
        return cap;
    }
    return (uint16_t)(cur + (uint16_t)delta);
}

static uint32_t fnv1a_32(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0u; i < len; ++i) {
        h ^= (uint32_t)data[i];
        h *= 16777619u;
    }
    return h;
}

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static size_t bounded_strlen(const char *s, size_t max_len)
{
    size_t n = 0u;
    while (n < max_len && s[n] != '\0') {
        ++n;
    }
    return n;
}

/* Sanitize fish name to uppercase ASCII A-Z, 0-9, and collapsed single spaces.
 * Max length JR_AQ_NAME_CAP - 1 (13) characters + NUL terminator. */
static void sanitize_fish_name(const char *src, char out[JR_AQ_NAME_CAP])
{
    memset(out, 0, JR_AQ_NAME_CAP);
    if (src == NULL) {
        return;
    }

    size_t len = 0u;
    bool pending_space = false;
    const size_t max_chars = JR_AQ_NAME_CAP - 1u;

    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; ++p) {
        const unsigned char ch = *p;
        if (isspace(ch)) {
            if (len > 0u) {
                pending_space = true;
            }
            continue;
        }
        unsigned char c = ch;
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - ('a' - 'A'));
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            continue;
        }
        const size_t need = (pending_space && len > 0u) ? 2u : 1u;
        if (len + need > max_chars) {
            break;
        }
        if (pending_space && len > 0u) {
            out[len++] = ' ';
        }
        out[len++] = (char)c;
        pending_space = false;
    }
    out[len] = '\0';
}

static bool tank_has_name(const jr_aq_tank_t *t, const char *name)
{
    for (size_t i = 0u; i < JR_AQ_MAX_FISH; ++i) {
        if (t->fish[i].active && strcmp(t->fish[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool tank_has_seed(const jr_aq_tank_t *t, uint16_t seed)
{
    for (size_t i = 0u; i < JR_AQ_MAX_FISH; ++i) {
        if (t->fish[i].active && t->fish[i].seed == seed) {
            return true;
        }
    }
    return false;
}

static void pick_default_name(const jr_aq_tank_t *t, size_t start_idx,
                              char out[JR_AQ_NAME_CAP])
{
    memset(out, 0, JR_AQ_NAME_CAP);
    for (size_t k = 0u; k < JR_AQ_MAX_FISH; ++k) {
        const char *cand = kDefaultNames[(start_idx + k) % JR_AQ_MAX_FISH];
        if (!tank_has_name(t, cand)) {
            const size_t n = bounded_strlen(cand, JR_AQ_NAME_CAP - 1u);
            memcpy(out, cand, n);
            out[n] = '\0';
            return;
        }
    }
    const char *fallback = kDefaultNames[start_idx % JR_AQ_MAX_FISH];
    const size_t n = bounded_strlen(fallback, JR_AQ_NAME_CAP - 1u);
    memcpy(out, fallback, n);
    out[n] = '\0';
}

static uint16_t make_fish_seed(const jr_aq_tank_t *t, jr_aq_species_t sp,
                               const char *name, uint32_t born_epoch_s,
                               uint8_t slot_idx)
{
    uint8_t buf[1u + 1u + 4u + JR_AQ_NAME_CAP];
    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)sp;
    buf[1] = slot_idx;
    put_u32_le(buf + 2u, born_epoch_s);
    const size_t nlen = bounded_strlen(name, JR_AQ_NAME_CAP - 1u);
    if (nlen > 0u) {
        memcpy(buf + 6u, name, nlen);
    }
    const uint32_t h = fnv1a_32(buf, 6u + nlen);
    uint16_t seed = (uint16_t)((h ^ (h >> 16u)) & 0xFFFFu);
    if (seed == 0u) {
        seed = 1u;
    }
    for (size_t tries = 0u; tries < 65535u && tank_has_seed(t, seed); ++tries) {
        seed = (uint16_t)(seed + 1u);
        if (seed == 0u) {
            seed = 1u;
        }
    }
    return seed;
}

static uint8_t safe_fish_count(const jr_aq_tank_t *t)
{
    return t->fish_count <= JR_AQ_MAX_FISH ? t->fish_count : (uint8_t)JR_AQ_MAX_FISH;
}

void jr_aq_init_default(jr_aq_tank_t *t, uint32_t now_epoch_s)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
    t->biome = (uint8_t)JR_AQ_BIOME_REEF;
    t->pearls = 2u;
    t->coral_xp = 20u;
    t->algae = 0u;
    t->total_feeds = 0u;
    t->last_tick_epoch_s = now_epoch_s;
    t->fish_count = 0u;

    const int i0 = jr_aq_add_fish(t, JR_AQ_SP_CLOWNFISH, "NEMO", now_epoch_s);
    if (i0 >= 0) {
        t->fish[i0].xp = 20u;  /* Fry, nearly Juvenile */
    }
    const int i1 = jr_aq_add_fish(t, JR_AQ_SP_BLUE_TANG, "DORY", now_epoch_s);
    if (i1 >= 0) {
        t->fish[i1].xp = 45u;  /* Juvenile */
    }
    const int i2 = jr_aq_add_fish(t, JR_AQ_SP_NEON_TETRA, "NOVA", now_epoch_s);
    if (i2 >= 0) {
        t->fish[i2].xp = 110u; /* Adult */
    }
}

int jr_aq_add_fish(jr_aq_tank_t *t, jr_aq_species_t sp, const char *name,
                   uint32_t now_epoch_s)
{
    if (t == NULL || (unsigned)sp >= (unsigned)JR_AQ_SP_COUNT) {
        return -1;
    }
    if (t->fish_count >= JR_AQ_MAX_FISH) {
        return -1;
    }

    const uint8_t idx = t->fish_count;
    char clean[JR_AQ_NAME_CAP];
    sanitize_fish_name(name, clean);
    if (clean[0] == '\0') {
        pick_default_name(t, idx, clean);
    }

    jr_aq_fish_t *f = &t->fish[idx];
    memset(f, 0, sizeof(*f));
    f->active = true;
    f->species = (uint8_t)sp;
    memcpy(f->name, clean, JR_AQ_NAME_CAP);
    f->born_epoch_s = now_epoch_s;
    f->xp = 0u;
    f->feedings = 0u;
    f->hunger = 25u;
    f->happiness = 85u;
    f->seed = make_fish_seed(t, sp, clean, now_epoch_s, idx);

    t->fish_count = (uint8_t)(idx + 1u);
    return (int)idx;
}

bool jr_aq_remove_fish(jr_aq_tank_t *t, uint8_t idx)
{
    if (t == NULL) {
        return false;
    }
    const uint8_t count = safe_fish_count(t);
    if (idx >= count || !t->fish[idx].active) {
        return false;
    }
    for (uint8_t j = (uint8_t)(idx + 1u); j < count; ++j) {
        t->fish[j - 1u] = t->fish[j];
    }
    t->fish_count = (uint8_t)(count - 1u);
    memset(&t->fish[t->fish_count], 0, sizeof(t->fish[0]));
    return true;
}

bool jr_aq_remove_fish_by_name(jr_aq_tank_t *t, const char *name)
{
    if (t == NULL || name == NULL) {
        return false;
    }
    char clean[JR_AQ_NAME_CAP];
    sanitize_fish_name(name, clean);
    if (clean[0] == '\0') {
        return false;
    }
    const uint8_t count = safe_fish_count(t);
    for (uint8_t i = 0u; i < count; ++i) {
        if (t->fish[i].active && strcmp(t->fish[i].name, clean) == 0) {
            return jr_aq_remove_fish(t, i);
        }
    }
    return false;
}

void jr_aq_advance_time(jr_aq_tank_t *t, uint32_t now_epoch_s)
{
    if (t == NULL) {
        return;
    }
    /* First SNTP sync after pre-sync boot (last_tick == 0 -> real Unix epoch):
     * anchor timestamps cleanly instead of synthesizing a huge time jump. */
    if (t->last_tick_epoch_s == 0u && now_epoch_s >= JR_AQ_REAL_EPOCH_MIN) {
        t->last_tick_epoch_s = now_epoch_s;
        for (size_t i = 0u; i < JR_AQ_MAX_FISH; ++i) {
            if (t->fish[i].active && t->fish[i].born_epoch_s == 0u) {
                t->fish[i].born_epoch_s = now_epoch_s;
            }
        }
        return;
    }
    if (now_epoch_s <= t->last_tick_epoch_s) {
        return;
    }

    const uint32_t dt_s = now_epoch_s - t->last_tick_epoch_s;
    const uint32_t raw_h = dt_s / 3600u;
    if (raw_h == 0u) {
        return;
    }

    const uint32_t old_tick = t->last_tick_epoch_s;
    const uint32_t new_tick = old_tick + raw_h * 3600u;
    t->last_tick_epoch_s = new_tick;

    const uint32_t dt_h = (raw_h > JR_AQ_MAX_TICK_HOURS)
                              ? JR_AQ_MAX_TICK_HOURS
                              : raw_h;

    /* +1 algae per 4 elapsed hours (epoch-bucket delta keeps 1h ticks exact). */
    const uint32_t algae_gain = (raw_h > JR_AQ_MAX_TICK_HOURS)
                                    ? (dt_h / 4u)
                                    : ((new_tick / 14400u) - (old_tick / 14400u));
    t->algae = add_u8_sat_u32(t->algae, algae_gain, 100u);

    /* Cozy invariant: fish NEVER die, even after weeks unattended. */
    for (size_t i = 0u; i < JR_AQ_MAX_FISH; ++i) {
        jr_aq_fish_t *f = &t->fish[i];
        if (!f->active) {
            continue;
        }
        f->xp = add_u16_sat_u32(f->xp, dt_h * 2u, JR_AQ_XP_CAP);
        const uint8_t old_hunger = f->hunger;
        f->hunger = add_u8_sat_u32(old_hunger, dt_h * 4u, 100u);

        const uint32_t hours_to_hungry =
            (old_hunger < 80u) ? ((80u - (uint32_t)old_hunger + 3u) / 4u) : 0u;
        if (dt_h > hours_to_hungry) {
            f->happiness = sub_u8_sat_u32(f->happiness, dt_h - hours_to_hungry, 30u);
        }
    }
}

uint8_t jr_aq_feed(jr_aq_tank_t *t, uint32_t now_epoch_s)
{
    if (t == NULL) {
        return 0u;
    }
    if (now_epoch_s > 0u) {
        jr_aq_advance_time(t, now_epoch_s);
    }

    uint8_t fed = 0u;
    for (size_t i = 0u; i < JR_AQ_MAX_FISH; ++i) {
        jr_aq_fish_t *f = &t->fish[i];
        if (!f->active) {
            continue;
        }
        f->xp = add_u16_sat_u32(f->xp, 12u, JR_AQ_XP_CAP);
        f->feedings = add_u16_sat_u32(f->feedings, 1u, 65535u);
        f->hunger = sub_u8_sat_u32(f->hunger, 35u, 0u);
        f->happiness = add_u8_sat_u32(f->happiness, 10u, 100u);
        ++fed;
    }

    if (fed > 0u) {
        t->coral_xp = add_u16_sat_u32(t->coral_xp, 4u, JR_AQ_XP_CAP);
        if (t->total_feeds < UINT32_MAX) {
            t->total_feeds++;
        }
    }
    return fed;
}

void jr_aq_clean(jr_aq_tank_t *t)
{
    if (t == NULL) {
        return;
    }
    if (t->algae > 0u) {
        for (size_t i = 0u; i < JR_AQ_MAX_FISH; ++i) {
            if (t->fish[i].active) {
                t->fish[i].happiness = add_u8_sat_u32(t->fish[i].happiness, 5u, 100u);
            }
        }
    }
    t->algae = 0u;
}

void jr_aq_reward_productivity(jr_aq_tank_t *t, uint16_t pearls,
                               uint16_t coral_bonus, uint32_t now_epoch_s)
{
    if (t == NULL) {
        return;
    }
    if (now_epoch_s > 0u) {
        jr_aq_advance_time(t, now_epoch_s);
    }
    t->pearls = add_u16_sat_u32(t->pearls, pearls, JR_AQ_XP_CAP);
    t->coral_xp = add_u16_sat_u32(t->coral_xp, coral_bonus, JR_AQ_XP_CAP);

    if (pearls > 0u || coral_bonus > 0u) {
        const uint32_t fish_xp = (coral_bonus > 0u)
                                     ? (((uint32_t)coral_bonus + 1u) / 2u)
                                     : 5u;
        for (size_t i = 0u; i < JR_AQ_MAX_FISH; ++i) {
            if (t->fish[i].active) {
                t->fish[i].xp = add_u16_sat_u32(t->fish[i].xp, fish_xp, JR_AQ_XP_CAP);
                t->fish[i].happiness = add_u8_sat_u32(t->fish[i].happiness, 5u, 100u);
            }
        }
    }
}

void jr_aq_set_biome(jr_aq_tank_t *t, jr_aq_biome_t biome)
{
    if (t == NULL || (unsigned)biome >= (unsigned)JR_AQ_BIOME_COUNT) {
        return;
    }
    t->biome = (uint8_t)biome;
}

void jr_aq_step_biome(jr_aq_tank_t *t, int dir)
{
    if (t == NULL) {
        return;
    }
    const int count = (int)JR_AQ_BIOME_COUNT;
    const int cur = (t->biome < JR_AQ_BIOME_COUNT) ? (int)t->biome : 0;
    const int rem = dir % count;
    const int next = (cur + rem + count) % count;
    t->biome = (uint8_t)next;
}

jr_aq_stage_t jr_aq_fish_stage(const jr_aq_fish_t *f)
{
    if (f == NULL) {
        return JR_AQ_STAGE_FRY;
    }
    if (f->xp >= 250u) {
        return JR_AQ_STAGE_ELDER;
    }
    if (f->xp >= 100u) {
        return JR_AQ_STAGE_ADULT;
    }
    if (f->xp >= 30u) {
        return JR_AQ_STAGE_JUVENILE;
    }
    return JR_AQ_STAGE_FRY;
}

uint8_t jr_aq_coral_level(const jr_aq_tank_t *t)
{
    if (t == NULL) {
        return 1u;
    }
    if (t->coral_xp >= 350u) {
        return 5u;
    }
    if (t->coral_xp >= 200u) {
        return 4u;
    }
    if (t->coral_xp >= 100u) {
        return 3u;
    }
    if (t->coral_xp >= 40u) {
        return 2u;
    }
    return 1u;
}

uint32_t jr_aq_fish_age_hours(const jr_aq_fish_t *f, uint32_t now_epoch_s)
{
    if (f == NULL || !f->active || now_epoch_s <= f->born_epoch_s) {
        return 0u;
    }
    return (now_epoch_s - f->born_epoch_s) / 3600u;
}

/* Normalize an identifier token: trim whitespace, lowercase ASCII, collapse
 * spaces/hyphens/underscores to a single '_'. Returns false if empty or too
 * long for cap. */
static bool normalize_token(const char *s, char *dst, size_t cap)
{
    if (s == NULL || dst == NULL || cap < 2u) {
        return false;
    }
    while (*s != '\0' && isspace((unsigned char)*s)) {
        ++s;
    }
    size_t len = 0u;
    bool pending_sep = false;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
        const unsigned char ch = *p;
        if (isspace(ch) || ch == '-' || ch == '_') {
            if (len > 0u) {
                pending_sep = true;
            }
            continue;
        }
        const size_t need = (pending_sep && len > 0u) ? 2u : 1u;
        if (len + need >= cap) {
            return false;
        }
        if (pending_sep && len > 0u) {
            dst[len++] = '_';
        }
        dst[len++] = (char)tolower(ch);
        pending_sep = false;
    }
    dst[len] = '\0';
    return len > 0u;
}

const char *jr_aq_species_name(jr_aq_species_t sp)
{
    switch (sp) {
    case JR_AQ_SP_CLOWNFISH:  return "clownfish";
    case JR_AQ_SP_BLUE_TANG:  return "blue_tang";
    case JR_AQ_SP_NEON_TETRA: return "neon_tetra";
    case JR_AQ_SP_GOLDEN_KOI: return "golden_koi";
    case JR_AQ_SP_MANDARIN:   return "mandarin";
    case JR_AQ_SP_JELLYFISH:  return "jellyfish";
    case JR_AQ_SP_MANTA_RAY:  return "manta_ray";
    case JR_AQ_SP_SEAHORSE:   return "seahorse";
    case JR_AQ_SP_COUNT:
    default:                  return "unknown";
    }
}

bool jr_aq_species_parse(const char *s, jr_aq_species_t *out)
{
    char tok[32];
    if (!normalize_token(s, tok, sizeof(tok))) {
        return false;
    }

    static const struct {
        const char     *alias;
        jr_aq_species_t sp;
    } kTable[] = {
        { "clown",             JR_AQ_SP_CLOWNFISH  },
        { "clownfish",         JR_AQ_SP_CLOWNFISH  },
        { "clown_fish",        JR_AQ_SP_CLOWNFISH  },
        { "tang",              JR_AQ_SP_BLUE_TANG  },
        { "blue_tang",         JR_AQ_SP_BLUE_TANG  },
        { "bluetang",          JR_AQ_SP_BLUE_TANG  },
        { "regal_tang",        JR_AQ_SP_BLUE_TANG  },
        { "regal_blue_tang",   JR_AQ_SP_BLUE_TANG  },
        { "tetra",             JR_AQ_SP_NEON_TETRA },
        { "neon_tetra",        JR_AQ_SP_NEON_TETRA },
        { "neontetra",         JR_AQ_SP_NEON_TETRA },
        { "neon",              JR_AQ_SP_NEON_TETRA },
        { "koi",               JR_AQ_SP_GOLDEN_KOI },
        { "golden_koi",        JR_AQ_SP_GOLDEN_KOI },
        { "goldenkoi",         JR_AQ_SP_GOLDEN_KOI },
        { "mandarin",          JR_AQ_SP_MANDARIN   },
        { "dragonet",          JR_AQ_SP_MANDARIN   },
        { "mandarin_dragonet", JR_AQ_SP_MANDARIN   },
        { "mandarinfish",      JR_AQ_SP_MANDARIN   },
        { "jelly",             JR_AQ_SP_JELLYFISH  },
        { "jellyfish",         JR_AQ_SP_JELLYFISH  },
        { "jelly_fish",        JR_AQ_SP_JELLYFISH  },
        { "moon_jelly",        JR_AQ_SP_JELLYFISH  },
        { "moon_jellyfish",    JR_AQ_SP_JELLYFISH  },
        { "manta",             JR_AQ_SP_MANTA_RAY  },
        { "manta_ray",         JR_AQ_SP_MANTA_RAY  },
        { "mantaray",          JR_AQ_SP_MANTA_RAY  },
        { "ray",               JR_AQ_SP_MANTA_RAY  },
        { "seahorse",          JR_AQ_SP_SEAHORSE   },
        { "sea_horse",         JR_AQ_SP_SEAHORSE   },
    };

    for (size_t i = 0u; i < sizeof(kTable) / sizeof(kTable[0]); ++i) {
        if (strcmp(tok, kTable[i].alias) == 0) {
            if (out != NULL) {
                *out = kTable[i].sp;
            }
            return true;
        }
    }
    return false;
}

const char *jr_aq_biome_name(jr_aq_biome_t b)
{
    switch (b) {
    case JR_AQ_BIOME_REEF:  return "reef";
    case JR_AQ_BIOME_ABYSS: return "abyss";
    case JR_AQ_BIOME_KOI:   return "koi";
    case JR_AQ_BIOME_COUNT:
    default:                return "unknown";
    }
}

bool jr_aq_biome_parse(const char *s, jr_aq_biome_t *out)
{
    char tok[32];
    if (!normalize_token(s, tok, sizeof(tok))) {
        return false;
    }

    static const struct {
        const char   *alias;
        jr_aq_biome_t biome;
    } kTable[] = {
        { "reef",          JR_AQ_BIOME_REEF  },
        { "tropical",      JR_AQ_BIOME_REEF  },
        { "tropical_reef", JR_AQ_BIOME_REEF  },
        { "coral",         JR_AQ_BIOME_REEF  },
        { "coral_reef",    JR_AQ_BIOME_REEF  },
        { "abyss",         JR_AQ_BIOME_ABYSS },
        { "jellyfish",     JR_AQ_BIOME_ABYSS },
        { "deep",          JR_AQ_BIOME_ABYSS },
        { "deep_abyss",    JR_AQ_BIOME_ABYSS },
        { "deep_sea",      JR_AQ_BIOME_ABYSS },
        { "koi",           JR_AQ_BIOME_KOI   },
        { "pond",          JR_AQ_BIOME_KOI   },
        { "zen",           JR_AQ_BIOME_KOI   },
        { "koi_pond",      JR_AQ_BIOME_KOI   },
        { "zen_koi",       JR_AQ_BIOME_KOI   },
        { "zen_koi_pond",  JR_AQ_BIOME_KOI   },
    };

    for (size_t i = 0u; i < sizeof(kTable) / sizeof(kTable[0]); ++i) {
        if (strcmp(tok, kTable[i].alias) == 0) {
            if (out != NULL) {
                *out = kTable[i].biome;
            }
            return true;
        }
    }
    return false;
}

const char *jr_aq_stage_name(jr_aq_stage_t st)
{
    switch (st) {
    case JR_AQ_STAGE_FRY:      return "fry";
    case JR_AQ_STAGE_JUVENILE: return "juvenile";
    case JR_AQ_STAGE_ADULT:    return "adult";
    case JR_AQ_STAGE_ELDER:    return "elder";
    default:                   return "unknown";
    }
}

size_t jr_aq_blob_pack(const jr_aq_tank_t *t, uint8_t *out, size_t cap)
{
    if (t == NULL || out == NULL) {
        return 0u;
    }

    uint8_t active_idx[JR_AQ_MAX_FISH];
    uint8_t count = 0u;
    for (uint8_t i = 0u; i < JR_AQ_MAX_FISH; ++i) {
        if (t->fish[i].active) {
            active_idx[count++] = i;
        }
    }

    const size_t payload_len = JR_AQ_BLOB_HDR_SIZE +
                               (size_t)count * JR_AQ_BLOB_FISH_SIZE;
    const size_t total_len = payload_len + JR_AQ_BLOB_CRC_SIZE;
    if (total_len > JR_AQ_BLOB_MAX || cap < total_len) {
        return 0u;
    }

    const uint8_t biome = (t->biome < JR_AQ_BIOME_COUNT) ? t->biome : 0u;
    const uint8_t algae = (t->algae <= 100u) ? t->algae : 100u;

    put_u32_le(out + 0u, JR_AQ_BLOB_MAGIC);
    put_u16_le(out + 4u, (uint16_t)JR_AQ_BLOB_VER);
    out[6] = biome;
    out[7] = algae;
    put_u16_le(out + 8u, t->pearls);
    put_u16_le(out + 10u, t->coral_xp);
    put_u32_le(out + 12u, t->total_feeds);
    put_u32_le(out + 16u, t->last_tick_epoch_s);
    out[20] = count;
    out[21] = 0u;
    out[22] = 0u;
    out[23] = 0u;

    size_t pos = JR_AQ_BLOB_HDR_SIZE;
    for (uint8_t k = 0u; k < count; ++k) {
        const jr_aq_fish_t *f = &t->fish[active_idx[k]];
        const uint8_t sp = (f->species < JR_AQ_SP_COUNT) ? f->species : 0u;
        const uint8_t hunger = (f->hunger <= 100u) ? f->hunger : 100u;
        const uint8_t happy = (f->happiness <= 100u) ? f->happiness : 100u;
        const size_t nlen = bounded_strlen(f->name, JR_AQ_NAME_CAP - 1u);

        uint8_t *rec = out + pos;
        memset(rec, 0, JR_AQ_BLOB_FISH_SIZE);
        rec[0] = sp;
        rec[1] = hunger;
        rec[2] = happy;
        rec[3] = (uint8_t)nlen;
        put_u32_le(rec + 4u, f->born_epoch_s);
        put_u16_le(rec + 8u, f->xp);
        put_u16_le(rec + 10u, f->feedings);
        put_u16_le(rec + 12u, f->seed);
        if (nlen > 0u) {
            memcpy(rec + 14u, f->name, nlen);
        }
        pos += JR_AQ_BLOB_FISH_SIZE;
    }

    const uint32_t crc = fnv1a_32(out, payload_len);
    put_u32_le(out + payload_len, crc);
    return total_len;
}

bool jr_aq_blob_unpack(jr_aq_tank_t *t, const uint8_t *buf, size_t len)
{
    if (t == NULL || buf == NULL) {
        return false;
    }
    if (len < JR_AQ_BLOB_HDR_SIZE + JR_AQ_BLOB_CRC_SIZE || len > JR_AQ_BLOB_MAX) {
        return false;
    }

    const size_t payload_len = len - JR_AQ_BLOB_CRC_SIZE;
    const uint32_t expect_crc = fnv1a_32(buf, payload_len);
    if (get_u32_le(buf + payload_len) != expect_crc) {
        return false;
    }

    const uint32_t magic = get_u32_le(buf + 0u);
    const uint16_t ver = get_u16_le(buf + 4u);
    if (magic != JR_AQ_BLOB_MAGIC || ver != JR_AQ_BLOB_VER) {
        return false;
    }

    const uint8_t biome = buf[6];
    const uint8_t algae = buf[7];
    const uint8_t count = buf[20];
    if (biome >= JR_AQ_BIOME_COUNT || algae > 100u || count > JR_AQ_MAX_FISH) {
        return false;
    }

    const size_t expect_len = JR_AQ_BLOB_HDR_SIZE +
                              (size_t)count * JR_AQ_BLOB_FISH_SIZE +
                              JR_AQ_BLOB_CRC_SIZE;
    if (len != expect_len) {
        return false;
    }

    jr_aq_tank_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.biome = biome;
    tmp.algae = algae;
    tmp.pearls = get_u16_le(buf + 8u);
    tmp.coral_xp = get_u16_le(buf + 10u);
    tmp.total_feeds = get_u32_le(buf + 12u);
    tmp.last_tick_epoch_s = get_u32_le(buf + 16u);
    tmp.fish_count = count;

    size_t pos = JR_AQ_BLOB_HDR_SIZE;
    for (uint8_t i = 0u; i < count; ++i) {
        const uint8_t *rec = buf + pos;
        const uint8_t sp = rec[0];
        const uint8_t hunger = rec[1];
        const uint8_t happy = rec[2];
        const uint8_t nlen = rec[3];
        if (sp >= JR_AQ_SP_COUNT || hunger > 100u || happy > 100u ||
            nlen >= JR_AQ_NAME_CAP) {
            return false;
        }

        jr_aq_fish_t *f = &tmp.fish[i];
        f->active = true;
        f->species = sp;
        f->hunger = hunger;
        f->happiness = happy;
        f->born_epoch_s = get_u32_le(rec + 4u);
        f->xp = get_u16_le(rec + 8u);
        f->feedings = get_u16_le(rec + 10u);
        f->seed = get_u16_le(rec + 12u);
        if (nlen > 0u) {
            memcpy(f->name, rec + 14u, nlen);
        }
        f->name[nlen] = '\0';
        pos += JR_AQ_BLOB_FISH_SIZE;
    }

    *t = tmp;
    return true;
}
