/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/notes.h — pure fixed-capacity desk scratchpad (no ESP-IDF, no heap).
 *
 * Holds up to JR_NOTES_MAX (6) short desk notes of up to 64 printable ASCII
 * characters with monotonic IDs. Persisted to NVS ("jr_notes") by the
 * composition root and read into daily/morning briefings.
 */
#ifndef JR_CORE_NOTES_H
#define JR_CORE_NOTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_NOTES_MAX        6u
#define JR_NOTE_TEXT_MAX    64u
#define JR_NOTE_TEXT_CAP    (JR_NOTE_TEXT_MAX + 1u)

typedef struct {
    uint32_t id;
    int64_t  created_s;
    char     text[JR_NOTE_TEXT_CAP];
} jr_note_item_t;

typedef struct {
    uint32_t       next_id;
    uint8_t        count;
    uint8_t        reserved[3];
    jr_note_item_t items[JR_NOTES_MAX];
} jr_note_bank_t;

/* Initialize an empty note bank with monotonic ID starting at 1. */
void jr_notes_init(jr_note_bank_t *bank);

/* Sanitize and add a note (trimmed, collapsed whitespace, JSON-safe ASCII,
 * max 64 chars). Returns the assigned monotonic ID (>0) on success, or 0 if
 * text is empty/invalid or the bank is full. */
uint32_t jr_notes_add(jr_note_bank_t *bank, const char *text,
                      int64_t now_epoch_s);

/* Delete a note by ID. Returns true if found and removed. */
bool jr_notes_delete(jr_note_bank_t *bank, uint32_t id);

/* Clear notes matching `target`:
 *   - NULL, "", or "all" (case-insensitive) -> clears every note
 *   - positive decimal ID -> deletes that ID
 *   - otherwise -> deletes notes whose text contains `target` (case-insensitive)
 * Monotonic next_id is preserved across clears. Returns number removed. */
unsigned jr_notes_clear(jr_note_bank_t *bank, const char *target);

unsigned jr_notes_count(const jr_note_bank_t *bank);

/* Returns pointer to item at 0 <= index < count, or NULL if out of range. */
const jr_note_item_t *jr_notes_get(const jr_note_bank_t *bank, unsigned index);

/* Format pinned notes into a concise spoken clause for jr_briefing_compose().
 * Writes "" and returns 0 when bank is empty. Always NUL-terminated. */
int jr_notes_format_briefing(const jr_note_bank_t *bank, char *dst, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_NOTES_H */
