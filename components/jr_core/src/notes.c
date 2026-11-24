/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/notes.c — pure fixed-capacity desk inbox / scratchpad. No ESP-IDF,
 * no heap.
 */
#include "jr_core/notes.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define JR_NOTES_V2_HDR_SIZE   16u
#define JR_NOTES_V2_ITEM_HDR   16u
#define JR_NOTES_V2_CRC_SIZE   4u
#define JR_NOTES_V1_HDR_SIZE   16u
#define JR_NOTES_V1_ITEM_SIZE  88u
#define JR_NOTES_V1_BLOB_SIZE  (JR_NOTES_V1_HDR_SIZE + JR_NOTES_MAX * JR_NOTES_V1_ITEM_SIZE)

static void sanitize_text(const char *src, char out[JR_NOTE_TEXT_CAP])
{
    out[0] = '\0';
    if (src == NULL) {
        return;
    }
    while (*src != '\0' && isspace((unsigned char)*src)) {
        ++src;
    }
    size_t len = 0u;
    bool prev_space = false;
    for (; *src != '\0' && len < JR_NOTE_TEXT_MAX; ++src) {
        const unsigned char ch = (unsigned char)*src;
        if (isspace(ch)) {
            if (!prev_space && len > 0u) {
                out[len++] = ' ';
                prev_space = true;
            }
            continue;
        }
        /* Keep printable ASCII, strip JSON-escaping hazards. */
        if (ch >= 0x20u && ch <= 0x7Eu && ch != '"' && ch != '\\') {
            out[len++] = (char)ch;
            prev_space = false;
        }
    }
    while (len > 0u && out[len - 1u] == ' ') {
        --len;
    }
    out[len] = '\0';
}

static bool ascii_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool ascii_icontains(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    const size_t nlen = strlen(needle);
    for (const char *p = haystack; *p != '\0'; ++p) {
        size_t i = 0u;
        while (i < nlen && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

static bool parse_positive_u32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint64_t acc = 0u;
    for (; *s != '\0'; ++s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        acc = acc * 10u + (uint64_t)(*s - '0');
        if (acc > 0xFFFFFFFFULL) {
            return false;
        }
    }
    if (acc == 0u) {
        return false;
    }
    *out = (uint32_t)acc;
    return true;
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

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static void put_u64_le(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0u; i < 8u; ++i) {
        p[i] = (uint8_t)((v >> (i * 8u)) & 0xFFu);
    }
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static uint64_t get_u64_le(const uint8_t *p)
{
    uint64_t v = 0u;
    for (unsigned i = 0u; i < 8u; ++i) {
        v |= ((uint64_t)p[i] << (i * 8u));
    }
    return v;
}

void jr_notes_init(jr_note_bank_t *bank)
{
    if (bank == NULL) {
        return;
    }
    memset(bank, 0, sizeof(*bank));
    bank->next_id = 1u;
}

uint32_t jr_notes_add_ex(jr_note_bank_t *bank, const char *text,
                         jr_note_tag_t tag, int64_t now_epoch_s)
{
    if (bank == NULL || bank->count >= JR_NOTES_MAX) {
        return 0u;
    }
    char clean[JR_NOTE_TEXT_CAP];
    sanitize_text(text, clean);
    if (clean[0] == '\0') {
        return 0u;
    }
    if (bank->next_id == 0u) {
        bank->next_id = 1u;
    }
    const uint32_t id = bank->next_id++;
    if (bank->next_id == 0u) {
        bank->next_id = 1u;
    }
    jr_note_item_t *slot = &bank->items[bank->count++];
    memset(slot, 0, sizeof(*slot));
    slot->id = id;
    slot->created_s = now_epoch_s > 0 ? now_epoch_s : 0;
    memcpy(slot->text, clean, sizeof(slot->text));
    slot->done = false;
    slot->tag = (tag == JR_NOTE_TAG_WORK)
        ? (uint8_t)JR_NOTE_TAG_WORK
        : (uint8_t)JR_NOTE_TAG_LIFE;
    return id;
}

uint32_t jr_notes_add(jr_note_bank_t *bank, const char *text,
                      int64_t now_epoch_s)
{
    return jr_notes_add_ex(bank, text, JR_NOTE_TAG_LIFE, now_epoch_s);
}

bool jr_notes_delete(jr_note_bank_t *bank, uint32_t id)
{
    if (bank == NULL || id == 0u || bank->count == 0u) {
        return false;
    }
    for (unsigned i = 0u; i < bank->count && i < JR_NOTES_MAX; ++i) {
        if (bank->items[i].id == id) {
            for (unsigned j = i + 1u; j < bank->count && j < JR_NOTES_MAX; ++j) {
                bank->items[j - 1u] = bank->items[j];
            }
            bank->count--;
            memset(&bank->items[bank->count], 0, sizeof(bank->items[0]));
            return true;
        }
    }
    return false;
}

bool jr_notes_toggle(jr_note_bank_t *bank, uint32_t id)
{
    if (bank == NULL || id == 0u || bank->count == 0u) {
        return false;
    }
    for (unsigned i = 0u; i < bank->count && i < JR_NOTES_MAX; ++i) {
        if (bank->items[i].id == id) {
            bank->items[i].done = !bank->items[i].done;
            return true;
        }
    }
    return false;
}

bool jr_notes_complete(jr_note_bank_t *bank, const char *target_or_id,
                       uint32_t *matched_id)
{
    if (matched_id != NULL) {
        *matched_id = 0u;
    }
    if (bank == NULL || bank->count == 0u) {
        return false;
    }
    char clean[JR_NOTE_TEXT_CAP];
    sanitize_text(target_or_id, clean);
    if (clean[0] == '\0') {
        return false;
    }

    uint32_t id = 0u;
    if (parse_positive_u32(clean, &id)) {
        for (unsigned i = 0u; i < bank->count && i < JR_NOTES_MAX; ++i) {
            if (bank->items[i].id == id) {
                bank->items[i].done = true;
                if (matched_id != NULL) {
                    *matched_id = bank->items[i].id;
                }
                return true;
            }
        }
        return false;
    }

    for (unsigned i = 0u; i < bank->count && i < JR_NOTES_MAX; ++i) {
        if (bank->items[i].done) {
            continue;
        }
        if (ascii_icontains(bank->items[i].text, clean)) {
            bank->items[i].done = true;
            if (matched_id != NULL) {
                *matched_id = bank->items[i].id;
            }
            return true;
        }
    }
    return false;
}

unsigned jr_notes_clear(jr_note_bank_t *bank, const char *target)
{
    if (bank == NULL || bank->count == 0u) {
        return 0u;
    }
    char clean[JR_NOTE_TEXT_CAP];
    sanitize_text(target, clean);

    if (clean[0] == '\0' || ascii_ieq(clean, "all")) {
        const unsigned n = bank->count;
        memset(bank->items, 0, sizeof(bank->items));
        bank->count = 0u;
        return n;
    }

    uint32_t id = 0u;
    if (parse_positive_u32(clean, &id)) {
        return jr_notes_delete(bank, id) ? 1u : 0u;
    }

    unsigned removed = 0u;
    unsigned dst = 0u;
    for (unsigned i = 0u; i < bank->count && i < JR_NOTES_MAX; ++i) {
        if (ascii_icontains(bank->items[i].text, clean)) {
            removed++;
        } else {
            if (dst != i) {
                bank->items[dst] = bank->items[i];
            }
            dst++;
        }
    }
    for (unsigned i = dst; i < bank->count && i < JR_NOTES_MAX; ++i) {
        memset(&bank->items[i], 0, sizeof(bank->items[0]));
    }
    bank->count = (uint8_t)dst;
    return removed;
}

unsigned jr_notes_count(const jr_note_bank_t *bank)
{
    if (bank == NULL) {
        return 0u;
    }
    return bank->count <= JR_NOTES_MAX ? bank->count : JR_NOTES_MAX;
}

size_t jr_notes_counts(const jr_note_bank_t *bank, size_t *open_out,
                       size_t *done_out)
{
    size_t open_n = 0u;
    size_t done_n = 0u;
    const unsigned n = jr_notes_count(bank);
    for (unsigned i = 0u; i < n; ++i) {
        if (bank->items[i].done) {
            done_n++;
        } else {
            open_n++;
        }
    }
    if (open_out != NULL) {
        *open_out = open_n;
    }
    if (done_out != NULL) {
        *done_out = done_n;
    }
    return (size_t)n;
}

const jr_note_item_t *jr_notes_get(const jr_note_bank_t *bank, unsigned index)
{
    if (bank == NULL || index >= jr_notes_count(bank)) {
        return NULL;
    }
    return &bank->items[index];
}

const jr_note_item_t *jr_notes_find(const jr_note_bank_t *bank, uint32_t id)
{
    if (bank == NULL || id == 0u) {
        return NULL;
    }
    const unsigned n = jr_notes_count(bank);
    for (unsigned i = 0u; i < n; ++i) {
        if (bank->items[i].id == id) {
            return &bank->items[i];
        }
    }
    return NULL;
}

const char *jr_note_tag_name(jr_note_tag_t tag)
{
    return (tag == JR_NOTE_TAG_WORK) ? "WORK" : "LIFE";
}

jr_note_tag_t jr_note_tag_parse(const char *s)
{
    if (s == NULL) {
        return JR_NOTE_TAG_LIFE;
    }
    char clean[16];
    size_t len = 0u;
    while (*s != '\0' && isspace((unsigned char)*s)) {
        ++s;
    }
    for (; *s != '\0' && !isspace((unsigned char)*s) && len + 1u < sizeof(clean);
         ++s) {
        clean[len++] = *s;
    }
    clean[len] = '\0';
    return ascii_ieq(clean, "work") ? JR_NOTE_TAG_WORK : JR_NOTE_TAG_LIFE;
}

static size_t bounded_strlen(const char *s, size_t max_len)
{
    size_t n = 0u;
    while (n < max_len && s[n] != '\0') {
        ++n;
    }
    return n;
}

size_t jr_notes_pack(const jr_note_bank_t *bank, uint8_t *buf, size_t cap)
{
    if (bank == NULL || buf == NULL) {
        return 0u;
    }
    const unsigned n = jr_notes_count(bank);
    size_t needed = JR_NOTES_V2_HDR_SIZE + JR_NOTES_V2_CRC_SIZE;
    for (unsigned i = 0u; i < n; ++i) {
        const size_t tlen = bounded_strlen(bank->items[i].text, JR_NOTE_TEXT_MAX);
        needed += JR_NOTES_V2_ITEM_HDR + tlen;
    }
    if (cap < needed) {
        return 0u;
    }

    const uint32_t next_id = bank->next_id != 0u ? bank->next_id : 1u;
    put_u32_le(buf + 0u, JR_NOTES_PACK_MAGIC_V2);
    put_u32_le(buf + 4u, JR_NOTES_PACK_VER_V2);
    put_u32_le(buf + 8u, next_id);
    buf[12] = (uint8_t)n;
    buf[13] = 0u;
    buf[14] = 0u;
    buf[15] = 0u;

    size_t pos = JR_NOTES_V2_HDR_SIZE;
    for (unsigned i = 0u; i < n; ++i) {
        const jr_note_item_t *it = &bank->items[i];
        const size_t tlen = bounded_strlen(it->text, JR_NOTE_TEXT_MAX);
        put_u32_le(buf + pos + 0u, it->id);
        put_u64_le(buf + pos + 4u, (uint64_t)it->created_s);
        buf[pos + 12u] = it->done ? 1u : 0u;
        buf[pos + 13u] = (it->tag == JR_NOTE_TAG_WORK)
            ? (uint8_t)JR_NOTE_TAG_WORK
            : (uint8_t)JR_NOTE_TAG_LIFE;
        buf[pos + 14u] = (uint8_t)tlen;
        buf[pos + 15u] = 0u;
        pos += JR_NOTES_V2_ITEM_HDR;
        if (tlen > 0u) {
            memcpy(buf + pos, it->text, tlen);
            pos += tlen;
        }
    }

    const uint32_t crc = fnv1a_32(buf, pos);
    put_u32_le(buf + pos, crc);
    pos += JR_NOTES_V2_CRC_SIZE;
    return pos;
}

static bool unpack_v2(jr_note_bank_t *out, const uint8_t *buf, size_t len)
{
    if (len < JR_NOTES_V2_HDR_SIZE + JR_NOTES_V2_CRC_SIZE) {
        return false;
    }
    const size_t payload_len = len - JR_NOTES_V2_CRC_SIZE;
    const uint32_t expect_crc = fnv1a_32(buf, payload_len);
    if (get_u32_le(buf + payload_len) != expect_crc) {
        return false;
    }

    const uint32_t next_id = get_u32_le(buf + 8u);
    const uint8_t count = buf[12];
    if (count > JR_NOTES_MAX) {
        return false;
    }

    jr_note_bank_t tmp;
    jr_notes_init(&tmp);
    tmp.next_id = next_id != 0u ? next_id : 1u;
    tmp.count = count;

    size_t pos = JR_NOTES_V2_HDR_SIZE;
    for (unsigned i = 0u; i < count; ++i) {
        if (pos + JR_NOTES_V2_ITEM_HDR > payload_len) {
            return false;
        }
        jr_note_item_t *dst = &tmp.items[i];
        dst->id = get_u32_le(buf + pos + 0u);
        dst->created_s = (int64_t)get_u64_le(buf + pos + 4u);
        dst->done = (buf[pos + 12u] != 0u);
        dst->tag = (buf[pos + 13u] == JR_NOTE_TAG_WORK)
            ? (uint8_t)JR_NOTE_TAG_WORK
            : (uint8_t)JR_NOTE_TAG_LIFE;
        const size_t tlen = (size_t)buf[pos + 14u];
        pos += JR_NOTES_V2_ITEM_HDR;
        if (tlen > JR_NOTE_TEXT_MAX || pos + tlen > payload_len) {
            return false;
        }
        if (tlen > 0u) {
            memcpy(dst->text, buf + pos, tlen);
            pos += tlen;
        }
        dst->text[tlen] = '\0';
    }
    if (pos != payload_len) {
        return false;
    }
    *out = tmp;
    return true;
}

static bool unpack_v1_legacy(jr_note_bank_t *out, const uint8_t *buf, size_t len)
{
    if (len != JR_NOTES_V1_BLOB_SIZE) {
        return false;
    }
    const uint32_t next_id = get_u32_le(buf + 8u);
    const uint8_t count = buf[12];
    if (count > JR_NOTES_MAX) {
        return false;
    }

    jr_note_bank_t tmp;
    jr_notes_init(&tmp);
    tmp.next_id = next_id != 0u ? next_id : 1u;
    tmp.count = count;

    for (unsigned i = 0u; i < count; ++i) {
        const uint8_t *src = buf + JR_NOTES_V1_HDR_SIZE +
                             (size_t)i * JR_NOTES_V1_ITEM_SIZE;
        jr_note_item_t *dst = &tmp.items[i];
        dst->id = get_u32_le(src + 0u);
        dst->created_s = (int64_t)get_u64_le(src + 8u);
        size_t tlen = 0u;
        while (tlen < JR_NOTE_TEXT_MAX && src[16u + tlen] != '\0') {
            dst->text[tlen] = (char)src[16u + tlen];
            ++tlen;
        }
        dst->text[tlen] = '\0';
        dst->done = false;
        dst->tag = (uint8_t)JR_NOTE_TAG_LIFE;
    }
    *out = tmp;
    return true;
}

bool jr_notes_unpack(jr_note_bank_t *bank, const uint8_t *buf, size_t len)
{
    if (bank == NULL || buf == NULL || len < 8u) {
        return false;
    }
    const uint32_t magic = get_u32_le(buf + 0u);
    const uint32_t ver = get_u32_le(buf + 4u);
    if (magic == JR_NOTES_PACK_MAGIC_V2 && ver == JR_NOTES_PACK_VER_V2) {
        return unpack_v2(bank, buf, len);
    }
    if (magic == JR_NOTES_PACK_MAGIC_V1 && ver == JR_NOTES_PACK_VER_V1) {
        return unpack_v1_legacy(bank, buf, len);
    }
    return false;
}

int jr_notes_format_briefing(const jr_note_bank_t *bank, char *dst, size_t cap)
{
    if (dst == NULL || cap == 0u) {
        return 0;
    }
    dst[0] = '\0';
    size_t open_n = 0u;
    (void)jr_notes_counts(bank, &open_n, NULL);
    if (open_n == 0u) {
        return 0;
    }
    int len = snprintf(dst, cap, "%u pinned note%s: ",
                       (unsigned)open_n, open_n == 1u ? "" : "s");
    if (len < 0) {
        dst[0] = '\0';
        return 0;
    }
    if ((size_t)len >= cap) {
        return (int)cap - 1;
    }
    const unsigned total = jr_notes_count(bank);
    unsigned emitted = 0u;
    for (unsigned i = 0u; i < total; ++i) {
        if (bank->items[i].done) {
            continue;
        }
        const int w = snprintf(dst + len, cap - (size_t)len, "%s%s",
                               emitted == 0u ? "" : "; ", bank->items[i].text);
        if (w < 0) {
            break;
        }
        if ((size_t)len + (size_t)w >= cap) {
            len = (int)cap - 1;
            break;
        }
        len += w;
        emitted++;
    }
    if ((size_t)len + 1u < cap) {
        dst[len++] = '.';
        dst[len] = '\0';
    }
    return len;
}
