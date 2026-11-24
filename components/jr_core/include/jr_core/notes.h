/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/notes.h — pure fixed-capacity desk inbox / scratchpad (no ESP-IDF,
 * no heap).
 *
 * Holds up to JR_NOTES_MAX (6) short desk notes of up to 64 printable ASCII
 * characters with monotonic IDs, WORK/LIFE tags, and done check-off state.
 * Persisted to NVS ("jr_notes") via portable version-2 packing (with legacy
 * version-1 blob decode) and read into daily/morning briefings.
 */
#ifndef JR_CORE_NOTES_H
#define JR_CORE_NOTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JR_NOTES_MAX            6u
#define JR_NOTE_TEXT_MAX        64u
#define JR_NOTE_TEXT_CAP        (JR_NOTE_TEXT_MAX + 1u)

#define JR_NOTES_PACK_MAGIC_V1  0x4A524E31u /* "JRN1" legacy struct blob */
#define JR_NOTES_PACK_MAGIC_V2  0x4A524E32u /* "JRN2" portable packed blob */
#define JR_NOTES_PACK_VER_V1    1u
#define JR_NOTES_PACK_VER_V2    2u
#define JR_NOTES_PACK_MAX_SIZE  500u        /* 16 hdr + 6*(16+64) + 4 crc */

typedef enum {
    JR_NOTE_TAG_LIFE = 0,
    JR_NOTE_TAG_WORK = 1,
} jr_note_tag_t;

typedef struct {
    uint32_t id;
    int64_t  created_s;
    char     text[JR_NOTE_TEXT_CAP];
    bool     done;
    uint8_t  tag;        /* jr_note_tag_t: JR_NOTE_TAG_LIFE or JR_NOTE_TAG_WORK */
} jr_note_item_t;

typedef struct {
    uint32_t       next_id;
    uint8_t        count;
    uint8_t        reserved[3];
    jr_note_item_t items[JR_NOTES_MAX];
} jr_note_bank_t;

/* Initialize an empty note bank with monotonic ID starting at 1. */
void jr_notes_init(jr_note_bank_t *bank);

/* Sanitize and add a tagged note (trimmed, collapsed whitespace, JSON-safe
 * ASCII, max 64 chars, done=false). Returns the assigned monotonic ID (>0) on
 * success, or 0 if text is empty/invalid or the bank is full. */
uint32_t jr_notes_add_ex(jr_note_bank_t *bank, const char *text,
                         jr_note_tag_t tag, int64_t now_epoch_s);

/* Backwards-compatible wrapper forwarding JR_NOTE_TAG_LIFE to jr_notes_add_ex. */
uint32_t jr_notes_add(jr_note_bank_t *bank, const char *text,
                      int64_t now_epoch_s);

/* Delete a note by ID. Returns true if found and removed. */
bool jr_notes_delete(jr_note_bank_t *bank, uint32_t id);

/* Flip `done` state on the note matching `id`. Returns true if found and
 * toggled, false if not found. */
bool jr_notes_toggle(jr_note_bank_t *bank, uint32_t id);

/* Mark a note `done = true` by numeric ID OR case-insensitive substring on open
 * notes (`!done`). Writes the matched note ID to `*matched_id` (if non-NULL,
 * 0 on miss). Returns true on match. */
bool jr_notes_complete(jr_note_bank_t *bank, const char *target_or_id,
                       uint32_t *matched_id);

/* Clear notes matching `target`:
 *   - NULL, "", or "all" (case-insensitive) -> clears every note
 *   - positive decimal ID -> deletes that ID
 *   - otherwise -> deletes notes whose text contains `target` (case-insensitive)
 * Monotonic next_id is preserved across clears. Returns number removed. */
unsigned jr_notes_clear(jr_note_bank_t *bank, const char *target);

unsigned jr_notes_count(const jr_note_bank_t *bank);

/* Count total, open (`!done`), and done (`done`) notes. Either output pointer
 * may be NULL. Returns total note count. */
size_t jr_notes_counts(const jr_note_bank_t *bank, size_t *open_out,
                       size_t *done_out);

/* Returns pointer to item at 0 <= index < count, or NULL if out of range. */
const jr_note_item_t *jr_notes_get(const jr_note_bank_t *bank, unsigned index);

/* Returns pointer to item matching `id`, or NULL if not found. */
const jr_note_item_t *jr_notes_find(const jr_note_bank_t *bank, uint32_t id);

/* Tag helpers ("LIFE" / "WORK"). */
const char *jr_note_tag_name(jr_note_tag_t tag);
jr_note_tag_t jr_note_tag_parse(const char *s);

/* Serialize bank into a portable v2 NVS blob with FNV-1a checksum. Returns
 * bytes written (>0) on success, or 0 if bank/buf is NULL or cap too small. */
size_t jr_notes_pack(const jr_note_bank_t *bank, uint8_t *buf, size_t cap);

/* Deserialize a portable v2 blob (JRN2 v2) OR legacy v1 struct blob (JRN1 v1,
 * decoding items with done=false and tag=JR_NOTE_TAG_LIFE). Returns true on
 * success; leaves `bank` untouched on failure. */
bool jr_notes_unpack(jr_note_bank_t *bank, const uint8_t *buf, size_t len);

/* Format open (not-done) pinned notes into a concise spoken clause for
 * jr_briefing_compose(). Writes "" and returns 0 when no open notes remain.
 * Always NUL-terminated. */
int jr_notes_format_briefing(const jr_note_bank_t *bank, char *dst, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_NOTES_H */
