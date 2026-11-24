/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_notes.c — unit tests for jr_core/notes (pure C, no ESP-IDF).
 */
#include "unity.h"
#include "jr_core/notes.h"
#include <string.h>

static void test_notes_init_and_empty(void)
{
    jr_note_bank_t b;
    jr_notes_init(&b);
    TEST_ASSERT_EQUAL_UINT(0u, jr_notes_count(&b));
    TEST_ASSERT_NULL(jr_notes_get(&b, 0u));
    TEST_ASSERT_FALSE(jr_notes_delete(&b, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, jr_notes_clear(&b, "all"));

    char buf[64];
    TEST_ASSERT_EQUAL_INT(0, jr_notes_format_briefing(&b, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_notes_add_sanitize_and_monotonic_ids(void)
{
    jr_note_bank_t b;
    jr_notes_init(&b);

    /* Reject empty/whitespace/quotes-only */
    TEST_ASSERT_EQUAL_UINT32(0u, jr_notes_add(&b, NULL, 100));
    TEST_ASSERT_EQUAL_UINT32(0u, jr_notes_add(&b, "   \t\n ", 100));
    TEST_ASSERT_EQUAL_UINT32(0u, jr_notes_add(&b, "\"\\\"", 100));

    /* Sanitizes spaces, strips quotes/backslashes, preserves case */
    const uint32_t id1 = jr_notes_add(&b, "  Call   \"Architect\"\\ at 3pm  ", 1732390000LL);
    TEST_ASSERT_EQUAL_UINT32(1u, id1);
    const jr_note_item_t *it1 = jr_notes_get(&b, 0u);
    TEST_ASSERT_NOT_NULL(it1);
    TEST_ASSERT_EQUAL_UINT32(1u, it1->id);
    TEST_ASSERT_EQUAL_INT64(1732390000LL, it1->created_s);
    TEST_ASSERT_EQUAL_STRING("Call Architect at 3pm", it1->text);

    /* 64-char cap */
    const char *long_txt =
        "0123456789012345678901234567890123456789012345678901234567890123456789";
    const uint32_t id2 = jr_notes_add(&b, long_txt, -5);
    TEST_ASSERT_EQUAL_UINT32(2u, id2);
    const jr_note_item_t *it2 = jr_notes_get(&b, 1u);
    TEST_ASSERT_NOT_NULL(it2);
    TEST_ASSERT_EQUAL_UINT(JR_NOTE_TEXT_MAX, strlen(it2->text));
    TEST_ASSERT_EQUAL_INT64(0, it2->created_s);

    /* Deleting ID 1 keeps ID 2 and next add gets ID 3 (monotonic) */
    TEST_ASSERT_TRUE(jr_notes_delete(&b, 1u));
    TEST_ASSERT_FALSE(jr_notes_delete(&b, 1u));
    TEST_ASSERT_EQUAL_UINT(1u, jr_notes_count(&b));
    TEST_ASSERT_EQUAL_UINT32(2u, jr_notes_get(&b, 0u)->id);

    const uint32_t id3 = jr_notes_add(&b, "Buy coffee", 1732390100LL);
    TEST_ASSERT_EQUAL_UINT32(3u, id3);
}

static void test_notes_max_six_and_clear_modes(void)
{
    jr_note_bank_t b;
    jr_notes_init(&b);

    for (unsigned i = 0u; i < JR_NOTES_MAX; ++i) {
        char tmp[24];
        snprintf(tmp, sizeof tmp, "Task %u alpha", i + 1u);
        TEST_ASSERT_EQUAL_UINT32(i + 1u, jr_notes_add(&b, tmp, 1000 + (int64_t)i));
    }
    TEST_ASSERT_EQUAL_UINT(JR_NOTES_MAX, jr_notes_count(&b));
    /* 7th note rejected while full */
    TEST_ASSERT_EQUAL_UINT32(0u, jr_notes_add(&b, "Seventh", 2000));

    /* Clear by numeric ID string */
    TEST_ASSERT_EQUAL_UINT(1u, jr_notes_clear(&b, "3"));
    TEST_ASSERT_EQUAL_UINT(5u, jr_notes_count(&b));

    /* Add a distinctive note and clear by case-insensitive keyword */
    TEST_ASSERT_EQUAL_UINT32(7u, jr_notes_add(&b, "CALL ARCHITECT AT 3", 2001));
    TEST_ASSERT_EQUAL_UINT(1u, jr_notes_clear(&b, "architect"));
    TEST_ASSERT_EQUAL_UINT(5u, jr_notes_count(&b));

    /* Clear all preserves monotonic next_id */
    TEST_ASSERT_EQUAL_UINT(5u, jr_notes_clear(&b, "ALL"));
    TEST_ASSERT_EQUAL_UINT(0u, jr_notes_count(&b));
    TEST_ASSERT_EQUAL_UINT32(8u, jr_notes_add(&b, "Fresh note", 2002));
}

static void test_notes_format_briefing(void)
{
    jr_note_bank_t b;
    jr_notes_init(&b);
    char out[256];

    TEST_ASSERT_EQUAL_UINT32(1u, jr_notes_add(&b, "Call architect at 3", 100));
    int n = jr_notes_format_briefing(&b, out, sizeof out);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_STRING("1 pinned note: Call architect at 3.", out);

    TEST_ASSERT_EQUAL_UINT32(2u, jr_notes_add(&b, "Ship Wave 2", 101));
    n = jr_notes_format_briefing(&b, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("2 pinned notes: Call architect at 3; Ship Wave 2.", out);
    TEST_ASSERT_EQUAL_INT((int)strlen(out), n);

    /* Short buffer truncates safely */
    char tiny[20];
    n = jr_notes_format_briefing(&b, tiny, sizeof tiny);
    TEST_ASSERT_EQUAL_INT((int)sizeof tiny - 1, n);
    TEST_ASSERT_EQUAL_INT((int)sizeof tiny - 1, (int)strlen(tiny));

    /* Completed notes are excluded from spoken briefing */
    TEST_ASSERT_TRUE(jr_notes_toggle(&b, 1u));
    n = jr_notes_format_briefing(&b, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("1 pinned note: Ship Wave 2.", out);
    TEST_ASSERT_TRUE(jr_notes_toggle(&b, 2u));
    n = jr_notes_format_briefing(&b, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(0, n);
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_notes_tags_toggle_complete_and_counts(void)
{
    jr_note_bank_t b;
    jr_notes_init(&b);

    TEST_ASSERT_EQUAL_STRING("LIFE", jr_note_tag_name(JR_NOTE_TAG_LIFE));
    TEST_ASSERT_EQUAL_STRING("WORK", jr_note_tag_name(JR_NOTE_TAG_WORK));
    TEST_ASSERT_EQUAL_INT(JR_NOTE_TAG_WORK, jr_note_tag_parse("  Work "));
    TEST_ASSERT_EQUAL_INT(JR_NOTE_TAG_LIFE, jr_note_tag_parse("life"));
    TEST_ASSERT_EQUAL_INT(JR_NOTE_TAG_LIFE, jr_note_tag_parse(NULL));

    const uint32_t id1 = jr_notes_add(&b, "Buy milk", 1000);
    const uint32_t id2 = jr_notes_add_ex(&b, "Review PR 42", JR_NOTE_TAG_WORK, 1001);
    const uint32_t id3 = jr_notes_add_ex(&b, "Buy coffee beans", JR_NOTE_TAG_LIFE, 1002);
    TEST_ASSERT_EQUAL_UINT32(1u, id1);
    TEST_ASSERT_EQUAL_UINT32(2u, id2);
    TEST_ASSERT_EQUAL_UINT32(3u, id3);

    TEST_ASSERT_EQUAL_UINT8(JR_NOTE_TAG_LIFE, jr_notes_find(&b, id1)->tag);
    TEST_ASSERT_FALSE(jr_notes_find(&b, id1)->done);
    TEST_ASSERT_EQUAL_UINT8(JR_NOTE_TAG_WORK, jr_notes_find(&b, id2)->tag);

    size_t open_n = 99u, done_n = 99u;
    TEST_ASSERT_EQUAL_UINT(3u, jr_notes_counts(&b, &open_n, &done_n));
    TEST_ASSERT_EQUAL_UINT(3u, open_n);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);

    /* Toggle flips done true -> false -> true and returns false for missing ID */
    TEST_ASSERT_FALSE(jr_notes_toggle(&b, 0u));
    TEST_ASSERT_FALSE(jr_notes_toggle(&b, 99u));
    TEST_ASSERT_TRUE(jr_notes_toggle(&b, id1));
    TEST_ASSERT_TRUE(jr_notes_find(&b, id1)->done);
    TEST_ASSERT_EQUAL_UINT(3u, jr_notes_counts(&b, &open_n, &done_n));
    TEST_ASSERT_EQUAL_UINT(2u, open_n);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);

    /* Complete by substring skips already-done id1 ("Buy milk") and matches open id3 ("Buy coffee beans") */
    uint32_t matched = 0u;
    TEST_ASSERT_TRUE(jr_notes_complete(&b, "buy", &matched));
    TEST_ASSERT_EQUAL_UINT32(id3, matched);
    TEST_ASSERT_TRUE(jr_notes_find(&b, id3)->done);

    /* No more open notes match "buy" */
    TEST_ASSERT_FALSE(jr_notes_complete(&b, "buy", &matched));
    TEST_ASSERT_EQUAL_UINT32(0u, matched);

    /* Complete by numeric ID string */
    TEST_ASSERT_TRUE(jr_notes_complete(&b, "2", &matched));
    TEST_ASSERT_EQUAL_UINT32(id2, matched);
    TEST_ASSERT_TRUE(jr_notes_find(&b, id2)->done);
    TEST_ASSERT_EQUAL_UINT(3u, jr_notes_counts(&b, &open_n, &done_n));
    TEST_ASSERT_EQUAL_UINT(0u, open_n);
    TEST_ASSERT_EQUAL_UINT(3u, done_n);

    /* Uncheck via toggle */
    TEST_ASSERT_TRUE(jr_notes_toggle(&b, id2));
    TEST_ASSERT_FALSE(jr_notes_find(&b, id2)->done);
    TEST_ASSERT_EQUAL_UINT(3u, jr_notes_counts(&b, &open_n, &done_n));
    TEST_ASSERT_EQUAL_UINT(1u, open_n);
    TEST_ASSERT_EQUAL_UINT(2u, done_n);
}

static void put_u32_le_test(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static void put_u64_le_test(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0u; i < 8u; ++i) {
        p[i] = (uint8_t)((v >> (i * 8u)) & 0xFFu);
    }
}

static void test_notes_pack_v2_roundtrip_and_v1_legacy_decode(void)
{
    jr_note_bank_t b;
    jr_notes_init(&b);

    /* Empty bank packs & unpacks cleanly */
    uint8_t blob[JR_NOTES_PACK_MAX_SIZE];
    const size_t empty_n = jr_notes_pack(&b, blob, sizeof blob);
    TEST_ASSERT_GREATER_THAN_UINT(0u, empty_n);
    jr_note_bank_t out;
    jr_notes_init(&out);
    TEST_ASSERT_TRUE(jr_notes_unpack(&out, blob, empty_n));
    TEST_ASSERT_EQUAL_UINT(0u, jr_notes_count(&out));
    TEST_ASSERT_EQUAL_UINT32(1u, out.next_id);

    /* Full 6-note bank with mixed tags, done flags, and 64-char text */
    const char *max64 =
        "0123456789012345678901234567890123456789012345678901234567890123";
    TEST_ASSERT_EQUAL_UINT32(1u, jr_notes_add_ex(&b, max64, JR_NOTE_TAG_WORK, 1732400001LL));
    TEST_ASSERT_EQUAL_UINT32(2u, jr_notes_add_ex(&b, "Pick up groceries", JR_NOTE_TAG_LIFE, 1732400002LL));
    TEST_ASSERT_EQUAL_UINT32(3u, jr_notes_add_ex(&b, "Ship Glass 3.5", JR_NOTE_TAG_WORK, 1732400003LL));
    TEST_ASSERT_EQUAL_UINT32(4u, jr_notes_add_ex(&b, "Water plants", JR_NOTE_TAG_LIFE, 1732400004LL));
    TEST_ASSERT_EQUAL_UINT32(5u, jr_notes_add_ex(&b, "Prep standup", JR_NOTE_TAG_WORK, 1732400005LL));
    TEST_ASSERT_EQUAL_UINT32(6u, jr_notes_add_ex(&b, "Call mom", JR_NOTE_TAG_LIFE, 1732400006LL));
    TEST_ASSERT_TRUE(jr_notes_toggle(&b, 2u));
    TEST_ASSERT_TRUE(jr_notes_toggle(&b, 5u));

    const size_t full_n = jr_notes_pack(&b, blob, sizeof blob);
    TEST_ASSERT_GREATER_THAN_UINT(0u, full_n);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(JR_NOTES_PACK_MAX_SIZE, full_n);
    /* Short cap rejected */
    TEST_ASSERT_EQUAL_UINT(0u, jr_notes_pack(&b, blob, full_n - 1u));

    jr_notes_init(&out);
    TEST_ASSERT_TRUE(jr_notes_unpack(&out, blob, full_n));
    TEST_ASSERT_EQUAL_UINT32(7u, out.next_id);
    TEST_ASSERT_EQUAL_UINT(6u, jr_notes_count(&out));
    TEST_ASSERT_EQUAL_STRING(max64, jr_notes_get(&out, 0u)->text);
    TEST_ASSERT_EQUAL_UINT8(JR_NOTE_TAG_WORK, jr_notes_get(&out, 0u)->tag);
    TEST_ASSERT_FALSE(jr_notes_get(&out, 0u)->done);
    TEST_ASSERT_TRUE(jr_notes_get(&out, 1u)->done);
    TEST_ASSERT_EQUAL_UINT8(JR_NOTE_TAG_LIFE, jr_notes_get(&out, 1u)->tag);
    TEST_ASSERT_TRUE(jr_notes_get(&out, 4u)->done);
    TEST_ASSERT_EQUAL_INT64(1732400006LL, jr_notes_get(&out, 5u)->created_s);

    /* Corrupted CRC & truncated blob rejected without clobbering target */
    uint8_t bad[JR_NOTES_PACK_MAX_SIZE];
    memcpy(bad, blob, full_n);
    bad[full_n - 1u] ^= 0x01u;
    TEST_ASSERT_FALSE(jr_notes_unpack(&out, bad, full_n));
    TEST_ASSERT_EQUAL_UINT(6u, jr_notes_count(&out));
    TEST_ASSERT_FALSE(jr_notes_unpack(&out, blob, full_n - 1u));

    /* Synthesize legacy v1 struct blob (544 bytes: JRN1 v1) matching pre-3.5 NVS */
    uint8_t v1[544];
    memset(v1, 0, sizeof v1);
    put_u32_le_test(v1 + 0u, JR_NOTES_PACK_MAGIC_V1);
    put_u32_le_test(v1 + 4u, JR_NOTES_PACK_VER_V1);
    put_u32_le_test(v1 + 8u, 5u);  /* next_id */
    v1[12] = 2u;                   /* count */
    /* Item 0 at offset 16 */
    put_u32_le_test(v1 + 16u + 0u, 2u);
    put_u64_le_test(v1 + 16u + 8u, 1732390000ULL);
    memcpy(v1 + 16u + 16u, "Legacy note one", 15u);
    /* Item 1 at offset 16 + 88 = 104 */
    put_u32_le_test(v1 + 104u + 0u, 4u);
    put_u64_le_test(v1 + 104u + 8u, 1732390500ULL);
    memcpy(v1 + 104u + 16u, "Legacy note two", 15u);

    jr_note_bank_t migrated;
    jr_notes_init(&migrated);
    TEST_ASSERT_TRUE(jr_notes_unpack(&migrated, v1, sizeof v1));
    TEST_ASSERT_EQUAL_UINT32(5u, migrated.next_id);
    TEST_ASSERT_EQUAL_UINT(2u, jr_notes_count(&migrated));
    TEST_ASSERT_EQUAL_UINT32(2u, jr_notes_get(&migrated, 0u)->id);
    TEST_ASSERT_EQUAL_INT64(1732390000LL, jr_notes_get(&migrated, 0u)->created_s);
    TEST_ASSERT_EQUAL_STRING("Legacy note one", jr_notes_get(&migrated, 0u)->text);
    TEST_ASSERT_FALSE(jr_notes_get(&migrated, 0u)->done);
    TEST_ASSERT_EQUAL_UINT8(JR_NOTE_TAG_LIFE, jr_notes_get(&migrated, 0u)->tag);
    TEST_ASSERT_EQUAL_UINT32(4u, jr_notes_get(&migrated, 1u)->id);
    TEST_ASSERT_EQUAL_STRING("Legacy note two", jr_notes_get(&migrated, 1u)->text);
    TEST_ASSERT_FALSE(jr_notes_get(&migrated, 1u)->done);
    TEST_ASSERT_EQUAL_UINT8(JR_NOTE_TAG_LIFE, jr_notes_get(&migrated, 1u)->tag);
}

void notes_tests_run(void)
{
    RUN_TEST(test_notes_init_and_empty);
    RUN_TEST(test_notes_add_sanitize_and_monotonic_ids);
    RUN_TEST(test_notes_max_six_and_clear_modes);
    RUN_TEST(test_notes_format_briefing);
    RUN_TEST(test_notes_tags_toggle_complete_and_counts);
    RUN_TEST(test_notes_pack_v2_roundtrip_and_v1_legacy_decode);
}
