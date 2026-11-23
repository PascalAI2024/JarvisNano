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
}

void notes_tests_run(void)
{
    RUN_TEST(test_notes_init_and_empty);
    RUN_TEST(test_notes_add_sanitize_and_monotonic_ids);
    RUN_TEST(test_notes_max_six_and_clear_modes);
    RUN_TEST(test_notes_format_briefing);
}
