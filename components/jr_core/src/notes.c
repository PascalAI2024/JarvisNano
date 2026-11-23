/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/notes.c — pure fixed-capacity desk scratchpad. No ESP-IDF, no heap.
 */
#include "jr_core/notes.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

void jr_notes_init(jr_note_bank_t *bank)
{
    if (bank == NULL) {
        return;
    }
    memset(bank, 0, sizeof(*bank));
    bank->next_id = 1u;
}

uint32_t jr_notes_add(jr_note_bank_t *bank, const char *text,
                      int64_t now_epoch_s)
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
    return id;
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

const jr_note_item_t *jr_notes_get(const jr_note_bank_t *bank, unsigned index)
{
    if (bank == NULL || index >= jr_notes_count(bank)) {
        return NULL;
    }
    return &bank->items[index];
}

int jr_notes_format_briefing(const jr_note_bank_t *bank, char *dst, size_t cap)
{
    if (dst == NULL || cap == 0u) {
        return 0;
    }
    dst[0] = '\0';
    const unsigned n = jr_notes_count(bank);
    if (n == 0u) {
        return 0;
    }
    int len = snprintf(dst, cap, "%u pinned note%s: ", n, n == 1u ? "" : "s");
    if (len < 0) {
        dst[0] = '\0';
        return 0;
    }
    if ((size_t)len >= cap) {
        return (int)cap - 1;
    }
    for (unsigned i = 0u; i < n; ++i) {
        const int w = snprintf(dst + len, cap - (size_t)len, "%s%s",
                               i == 0u ? "" : "; ", bank->items[i].text);
        if (w < 0) {
            break;
        }
        if ((size_t)len + (size_t)w >= cap) {
            len = (int)cap - 1;
            break;
        }
        len += w;
    }
    if ((size_t)len + 1u < cap) {
        dst[len++] = '.';
        dst[len] = '\0';
    }
    return len;
}
