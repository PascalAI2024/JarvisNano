/* SPDX-License-Identifier: Apache-2.0 */
#include "unity.h"
#include "deck_projection.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned projections_exercised;
static const char *const keys[] = {
    "source", "workers", "live", "running", "remaining",
    "needs_owner", "owner_count"
};
static const char *const worker_keys[] = {"id", "label", "state"};

static bool parse(const cJSON *object, deck_projection_t *out)
{
    ++projections_exercised;
    return deck_projection_parse(object, out);
}

static cJSON *sample(unsigned live, unsigned running)
{
    cJSON *root = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(root);
    cJSON_AddStringToObject(root, "source", "live");
    cJSON *workers = cJSON_AddArrayToObject(root, "workers");
    const unsigned shown = live < 4 ? live : 4;
    for (unsigned i = 0; i < shown; ++i) {
        char id[3], label[9];
        snprintf(id, sizeof(id), "w%u", i + 1);
        snprintf(label, sizeof(label), "WORKER %u", i + 1);
        cJSON *worker = cJSON_CreateObject();
        cJSON_AddStringToObject(worker, "id", id);
        cJSON_AddStringToObject(worker, "label", label);
        cJSON_AddStringToObject(worker, "state", i < running ? "working" : "starting");
        cJSON_AddItemToArray(workers, worker);
    }
    cJSON_AddNumberToObject(root, "live", live);
    cJSON_AddNumberToObject(root, "running", running);
    cJSON_AddNumberToObject(root, "remaining", live - shown);
    cJSON_AddBoolToObject(root, "needs_owner", false);
    cJSON_AddNumberToObject(root, "owner_count", 0);
    return root;
}

static cJSON *worker(cJSON *root, unsigned index)
{
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "workers"),
                            (int)index);
}

static void replace(cJSON *object, const char *key, cJSON *item)
{
    TEST_ASSERT_TRUE(cJSON_ReplaceItemInObjectCaseSensitive(object, key, item));
}

static void number(cJSON *object, const char *key, double value)
{
    replace(object, key, cJSON_CreateNumber(value));
}

static void attention(cJSON *root, unsigned count)
{
    replace(root, "needs_owner", cJSON_CreateBool(count != 0));
    number(root, "owner_count", count);
}

/* Failed parsing must not partially publish a new or stale display frame. */
static void rejects(cJSON *root)
{
    deck_projection_t actual, expected;
    memset(&expected, 0xA5, sizeof(expected));
    memcpy(&actual, &expected, sizeof(actual));
    TEST_ASSERT_FALSE(parse(root, &actual));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &actual, sizeof(actual));
    cJSON_Delete(root);
}

static void accepts(cJSON *root, unsigned live, unsigned running)
{
    deck_projection_t actual;
    memset(&actual, 0xA5, sizeof(actual));
    TEST_ASSERT_TRUE(parse(root, &actual));
    TEST_ASSERT_EQUAL_UINT(live, actual.live);
    TEST_ASSERT_EQUAL_UINT(running, actual.running);
    TEST_ASSERT_EQUAL_UINT(live < 4 ? live : 4, actual.worker_count);
    TEST_ASSERT_EQUAL_UINT(live > 4 ? live - 4 : 0, actual.remaining);
    for (unsigned i = actual.worker_count; i < 4; ++i) {
        TEST_ASSERT_EQUAL(DECK_WORKER_NONE, actual.workers[i]);
    }
    cJSON_Delete(root);
}

static void test_deck_bridge_fixture(void)
{
    const char *json = "{\"live\":3,\"needs_owner\":true,\"owner_count\":1,"
        "\"remaining\":0,\"running\":3,\"source\":\"fixture\",\"workers\":["
        "{\"id\":\"w1\",\"label\":\"WORKER 1\",\"state\":\"working\"},"
        "{\"id\":\"w2\",\"label\":\"WORKER 2\",\"state\":\"working\"},"
        "{\"id\":\"w3\",\"label\":\"WORKER 3\",\"state\":\"working\"}]}";
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    deck_projection_t actual;
    TEST_ASSERT_TRUE(parse(root, &actual));
    cJSON_Delete(root);
    TEST_ASSERT_TRUE(actual.fixture);
    TEST_ASSERT_TRUE(actual.needs_owner);
    TEST_ASSERT_EQUAL_UINT(1, actual.owner_count);
    TEST_ASSERT_EQUAL_UINT(3, actual.live);
    TEST_ASSERT_EQUAL_UINT(3, actual.running);
    TEST_ASSERT_EQUAL_UINT(3, actual.worker_count);
    TEST_ASSERT_EQUAL_UINT(0, actual.remaining);
    for (unsigned i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL(DECK_WORKER_WORKING, actual.workers[i]);
    }
    TEST_ASSERT_EQUAL(DECK_WORKER_NONE, actual.workers[3]);
}

static void test_deck_all_live_running_boundaries(void)
{
    for (unsigned live = 0; live <= 256; ++live) {
        for (unsigned running = 0; running <= live; ++running) {
            accepts(sample(live, running), live, running);
        }
    }
    rejects(sample(257, 0)); /* Structurally coherent, but beyond bridge cap. */
}

static void test_deck_enum_states_and_no_retained_text(void)
{
    const char *states[] = {"working", "starting", "needs-you", "stalled"};
    const deck_worker_state_t expected[] = {
        DECK_WORKER_WORKING, DECK_WORKER_STARTING,
        DECK_WORKER_NEEDS_OWNER, DECK_WORKER_STALLED
    };
    cJSON *root = sample(4, 1);
    attention(root, 1);
    for (unsigned i = 0; i < 4; ++i) {
        replace(worker(root, i), "state", cJSON_CreateString(states[i]));
    }
    char *before = cJSON_PrintUnformatted(root);
    deck_projection_t actual, snapshot;
    TEST_ASSERT_TRUE(parse(root, &actual));
    char *after = cJSON_PrintUnformatted(root);
    TEST_ASSERT_EQUAL_STRING(before, after); /* Parsing does not alter input. */
    cJSON_free(before);
    cJSON_free(after);
    memcpy(&snapshot, &actual, sizeof(actual));
    for (unsigned i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL(expected[i], actual.workers[i]);
        replace(worker(root, i), "state", cJSON_CreateString("PRIVATE CHANGED TEXT"));
        replace(worker(root, i), "label", cJSON_CreateString("PRIVATE CHANGED LABEL"));
    }
    cJSON_Delete(root); /* Output remains independent after the DOM is freed. */
    TEST_ASSERT_EQUAL_MEMORY(&snapshot, &actual, sizeof(actual));
    TEST_ASSERT_FALSE(actual.fixture);
}

static void test_deck_owner_count_boundaries(void)
{
    for (unsigned count = 0; count <= 512; ++count) {
        cJSON *root = sample(0, 0);
        attention(root, count);
        deck_projection_t actual;
        TEST_ASSERT_TRUE(parse(root, &actual));
        TEST_ASSERT_EQUAL_UINT(count, actual.owner_count);
        TEST_ASSERT_EQUAL(count != 0, actual.needs_owner);
        cJSON_Delete(root);
    }
    cJSON *root = sample(0, 0);
    attention(root, 513);
    rejects(root);
}

static void test_deck_missing_unknown_duplicate_root_keys(void)
{
    for (unsigned i = 0; i < 7; ++i) {
        cJSON *root = sample(1, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(root, keys[i]);
        rejects(root);
        root = sample(1, 1);
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
        cJSON_AddItemToObject(root, keys[i], cJSON_Duplicate(item, true));
        rejects(root);
    }
    cJSON *root = sample(1, 1);
    cJSON_AddStringToObject(root, "question", "PRIVATE OWNER QUESTION");
    rejects(root);
    root = sample(1, 1);
    cJSON *item = cJSON_DetachItemFromObjectCaseSensitive(root, "live");
    cJSON_AddItemToObject(root, "LIVE", item);
    rejects(root);
}

static void test_deck_missing_unknown_duplicate_worker_keys(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        cJSON *root = sample(1, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(worker(root, 0), worker_keys[i]);
        rejects(root);
        root = sample(1, 1);
        cJSON *item = cJSON_GetObjectItemCaseSensitive(worker(root, 0), worker_keys[i]);
        cJSON_AddItemToObject(worker(root, 0), worker_keys[i], cJSON_Duplicate(item, true));
        rejects(root);
    }
    cJSON *root = sample(1, 1);
    cJSON_AddStringToObject(worker(root, 0), "path", "PRIVATE PATH");
    rejects(root);
}

static void test_deck_count_types_are_numbers_only(void)
{
    const char *count_keys[] = {"live", "running", "remaining", "owner_count"};
    const char *invalid[] = {"true", "false", "null", "\"1\"", "[]", "{}"};
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < 6; ++j) {
            cJSON *root = sample(1, 1);
            replace(root, count_keys[i], cJSON_Parse(invalid[j]));
            rejects(root);
        }
    }
}

static void test_deck_fractional_negative_nonfinite_overflow_counts(void)
{
    const char *count_keys[] = {"live", "running", "remaining", "owner_count"};
    const double invalid[] = {-1, -0.5, 0.5, 1.25, 257, 513, 65536,
                              4294967296.0, 1e300, INFINITY, -INFINITY, NAN};
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < sizeof(invalid) / sizeof(invalid[0]); ++j) {
            if (i == 3 && invalid[j] == 257) continue; /* Valid owner count. */
            cJSON *root = sample(1, 1);
            number(root, count_keys[i], invalid[j]);
            rejects(root);
        }
    }
    cJSON *root = sample(1, 1);
    /* valueint saturation/truncation must never validate a bad valuedouble. */
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "running");
    item->valueint = 1;
    item->valuedouble = NAN;
    rejects(root);
}

static void test_deck_source_exact_enum(void)
{
    const char *invalid[] = {"\"LIVE\"", "\"Fixture\"", "\"\"", "\"private\"",
                            "true", "null", "0", "[]", "{}"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        cJSON *root = sample(0, 0);
        replace(root, "source", cJSON_Parse(invalid[i]));
        rejects(root);
    }
}

static void test_deck_attention_boolean_and_consistency(void)
{
    const char *invalid[] = {"0", "1", "null", "\"true\"", "[]", "{}"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        cJSON *root = sample(0, 0);
        replace(root, "needs_owner", cJSON_Parse(invalid[i]));
        rejects(root);
    }
    cJSON *root = sample(0, 0);
    replace(root, "needs_owner", cJSON_CreateTrue());
    rejects(root);
    root = sample(0, 0);
    number(root, "owner_count", 1);
    rejects(root);
    root = sample(1, 0);
    replace(worker(root, 0), "state", cJSON_CreateString("needs-you"));
    rejects(root);
    root = sample(1, 0);
    replace(worker(root, 0), "state", cJSON_CreateString("needs-you"));
    attention(root, 1);
    accepts(root, 1, 0);
}

static void test_deck_running_counts_must_match_shown_workers(void)
{
    cJSON *root = sample(3, 3);
    number(root, "running", 2);
    rejects(root);
    root = sample(3, 0);
    number(root, "running", 1);
    rejects(root);
    root = sample(6, 1);
    number(root, "running", 4); /* One shown + two omitted cannot be four. */
    rejects(root);
    root = sample(6, 1);
    number(root, "running", 3);
    accepts(root, 6, 3);
    root = sample(6, 6);
    number(root, "running", 7);
    rejects(root);
}

static void test_deck_remaining_count_and_worker_cardinality(void)
{
    for (unsigned live = 0; live <= 256; ++live) {
        cJSON *root = sample(live, 0);
        const unsigned expected = live > 4 ? live - 4 : 0;
        number(root, "remaining", expected + 1);
        rejects(root);
        if (expected) {
            root = sample(live, 0);
            number(root, "remaining", expected - 1);
            rejects(root);
        }
    }
    cJSON *root = sample(0, 0);
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root, "workers"),
                        cJSON_CreateObject());
    rejects(root);
    for (unsigned live = 1; live <= 5; ++live) {
        root = sample(live, 0);
        cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(root, "workers"), 0);
        rejects(root);
    }
    root = sample(5, 0);
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root, "workers"),
                        cJSON_Duplicate(worker(root, 0), true));
    rejects(root);
}

static void test_deck_generated_identities_only(void)
{
    const char *invalid_id[] = {"w0", "w2", "W1", "w1 ", "", "PRIVATE TASK"};
    const char *invalid_label[] = {"WORKER1", "WORKER 2", "worker 1", "WORKER 1 ", "", "PRIVATE NAME"};
    for (unsigned i = 0; i < 6; ++i) {
        cJSON *root = sample(1, 1);
        replace(worker(root, 0), "id", cJSON_CreateString(invalid_id[i]));
        rejects(root);
        root = sample(1, 1);
        replace(worker(root, 0), "label", cJSON_CreateString(invalid_label[i]));
        rejects(root);
    }
    for (unsigned i = 1; i < 4; ++i) {
        cJSON *root = sample(4, 4);
        replace(worker(root, i), "id", cJSON_CreateString("w1"));
        rejects(root);
    }
}

static void test_deck_worker_identity_types_and_state_enum(void)
{
    const char *invalid[] = {"true", "false", "null", "1", "[]", "{}"};
    for (unsigned key = 0; key < 3; ++key) {
        for (unsigned i = 0; i < 6; ++i) {
            cJSON *root = sample(1, 1);
            replace(worker(root, 0), worker_keys[key], cJSON_Parse(invalid[i]));
            rejects(root);
        }
    }
    const char *invalid_states[] = {"WORKING", "idle", "done", "failed", "exited",
                                   "needs-owner", "PRIVATE REASON", ""};
    for (unsigned i = 0; i < 8; ++i) {
        cJSON *root = sample(1, 1);
        replace(worker(root, 0), "state", cJSON_CreateString(invalid_states[i]));
        rejects(root);
    }
}

static void test_deck_root_workers_and_entries_types(void)
{
    const char *invalid[] = {"true", "false", "null", "1", "\"raw\"", "[]"};
    for (unsigned i = 0; i < 6; ++i) rejects(cJSON_Parse(invalid[i]));
    const char *not_array[] = {"true", "null", "1", "\"raw\"", "{}"};
    for (unsigned i = 0; i < 5; ++i) {
        cJSON *root = sample(1, 1);
        replace(root, "workers", cJSON_Parse(not_array[i]));
        rejects(root);
    }
    for (unsigned i = 0; i < 6; ++i) {
        cJSON *root = sample(1, 1);
        TEST_ASSERT_TRUE(cJSON_ReplaceItemInArray(
            cJSON_GetObjectItemCaseSensitive(root, "workers"), 0, cJSON_Parse(invalid[i])));
        rejects(root);
    }
}

static void test_deck_null_arguments_and_missing_dom_strings(void)
{
    cJSON *root = sample(0, 0);
    TEST_ASSERT_FALSE(parse(root, NULL));
    cJSON_Delete(root);
    rejects(NULL);
    root = sample(0, 0);
    cJSON_free(root->child->string);
    root->child->string = NULL;
    rejects(root);
    root = sample(0, 0);
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "source");
    cJSON_free(item->valuestring);
    item->valuestring = NULL;
    rejects(root);
    for (unsigned i = 0; i < 3; ++i) {
        root = sample(1, 1);
        item = cJSON_GetObjectItemCaseSensitive(worker(root, 0), worker_keys[i]);
        cJSON_free(item->valuestring);
        item->valuestring = NULL;
        rejects(root);
    }
}

static void test_deck_field_order_does_not_change_projection(void)
{
    cJSON *root = sample(4, 4);
    deck_projection_t before, after;
    TEST_ASSERT_TRUE(parse(root, &before));
    cJSON *source = cJSON_DetachItemFromObjectCaseSensitive(root, "source");
    cJSON_AddItemToObject(root, "source", source);
    cJSON *id = cJSON_DetachItemFromObjectCaseSensitive(worker(root, 0), "id");
    cJSON_AddItemToObject(worker(root, 0), "id", id);
    TEST_ASSERT_TRUE(parse(root, &after));
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
    cJSON_Delete(root);
}

void test_deck_projection_register(void)
{
    RUN_TEST(test_deck_bridge_fixture);
    RUN_TEST(test_deck_all_live_running_boundaries);
    RUN_TEST(test_deck_enum_states_and_no_retained_text);
    RUN_TEST(test_deck_owner_count_boundaries);
    RUN_TEST(test_deck_missing_unknown_duplicate_root_keys);
    RUN_TEST(test_deck_missing_unknown_duplicate_worker_keys);
    RUN_TEST(test_deck_count_types_are_numbers_only);
    RUN_TEST(test_deck_fractional_negative_nonfinite_overflow_counts);
    RUN_TEST(test_deck_source_exact_enum);
    RUN_TEST(test_deck_attention_boolean_and_consistency);
    RUN_TEST(test_deck_running_counts_must_match_shown_workers);
    RUN_TEST(test_deck_remaining_count_and_worker_cardinality);
    RUN_TEST(test_deck_generated_identities_only);
    RUN_TEST(test_deck_worker_identity_types_and_state_enum);
    RUN_TEST(test_deck_root_workers_and_entries_types);
    RUN_TEST(test_deck_null_arguments_and_missing_dom_strings);
    RUN_TEST(test_deck_field_order_does_not_change_projection);
}

/* Parent may register the suite in test_core; this focused target needs no IDF. */
#ifdef DECK_PROJECTION_STANDALONE
void setUp(void) {}
void tearDown(void) {}
int main(void)
{
    UNITY_BEGIN();
    test_deck_projection_register();
    printf("deck projections exercised=%u\n", projections_exercised);
    return UNITY_END();
}
#endif
