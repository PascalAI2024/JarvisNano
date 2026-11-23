/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Host unit tests for jr_tools_project_json(): on-device JSON budget projector.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "jr_tools/jr_tools.h"
#include "jr_tools/jr_tools_project.h"

#define PCHECK(cond, what)                                                    \
    do {                                                                      \
        (*checks)++;                                                          \
        if (!(cond)) {                                                        \
            printf("FAIL project line %d: %s\n", __LINE__, what);             \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static bool is_valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p != '\0') {
        if (*p < 0x80U) {
            p++;
        } else if ((*p & 0xE0U) == 0xC0U) {
            if ((p[1] & 0xC0U) != 0x80U) return false;
            p += 2;
        } else if ((*p & 0xF0U) == 0xE0U) {
            if ((p[1] & 0xC0U) != 0x80U || (p[2] & 0xC0U) != 0x80U) return false;
            p += 3;
        } else if ((*p & 0xF8U) == 0xF0U) {
            if ((p[1] & 0xC0U) != 0x80U || (p[2] & 0xC0U) != 0x80U ||
                (p[3] & 0xC0U) != 0x80U) {
                return false;
            }
            p += 4;
        } else {
            return false;
        }
    }
    return true;
}

static char *make_repeated(char ch, size_t n)
{
    char *buf = (char *)malloc(n + 1U);
    if (buf != NULL) {
        memset(buf, ch, n);
        buf[n] = '\0';
    }
    return buf;
}

static int test_small_passthrough(int *checks)
{
    cJSON *src = cJSON_Parse("{\"ok\":true,\"count\":3,\"items\":[\"a\",\"b\"],\"raw\":\"tiny\"}");
    PCHECK(src != NULL, "small fixture parses");

    char out[JR_TOOLS_RESPONSE_CAP];
    jr_tools_project_info_t info;
    PCHECK(jr_tools_project_json(src, out, sizeof out, &info), "small payload succeeds");
    PCHECK(!info.truncated && !info.used_fallback && info.tier_index == -1,
           "small payload passes through untouched");
    PCHECK(strlen(out) < sizeof out, "small payload fits cap");

    cJSON *parsed = cJSON_Parse(out);
    PCHECK(parsed != NULL, "small output is valid JSON");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(parsed, "result");
    PCHECK(cJSON_IsObject(r), "wrapped in result object");
    PCHECK(cJSON_GetObjectItemCaseSensitive(r, "truncated") == NULL,
           "truncated key is absent on untouched payload");
    PCHECK(cJSON_GetObjectItemCaseSensitive(r, "hint") == NULL,
           "hint key is absent on untouched payload");
    PCHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok")), "ok preserved");
    PCHECK(strcmp(cJSON_GetObjectItemCaseSensitive(r, "raw")->valuestring, "tiny") == 0,
           "noise keys untouched when whole payload already fits");

    cJSON_Delete(parsed);
    cJSON_Delete(src);
    return 0;
}

static int test_noise_strip_only_pass(int *checks)
{
    cJSON *src = cJSON_CreateObject();
    PCHECK(src != NULL, "create noise-heavy object");
    cJSON_AddStringToObject(src, "title", "Release Checklist");
    cJSON_AddStringToObject(src, "summary",
                            "All 24 host test suites pass and PSRAM headroom is healthy.");
    char *big_html = make_repeated('H', 10000U);
    char *big_md = make_repeated('M', 5000U);
    PCHECK(big_html != NULL && big_md != NULL, "alloc noise strings");
    cJSON_AddStringToObject(src, "RAW_HTML", big_html);
    cJSON_AddStringToObject(src, "markdown_full", big_md);
    cJSON *emb = cJSON_AddArrayToObject(src, "embedding");
    for (int i = 0; i < 256; ++i) {
        cJSON_AddItemToArray(emb, cJSON_CreateNumber(0.123456));
    }
    free(big_html);
    free(big_md);

    char out[JR_TOOLS_RESPONSE_CAP];
    jr_tools_project_info_t info;
    PCHECK(jr_tools_project_json(src, out, sizeof out, &info), "noise-strip pass succeeds");
    PCHECK(info.truncated && !info.used_fallback && info.tier_index == 0,
           "noise-strip-only tier 0 suffices");
    PCHECK(strlen(out) < sizeof out, "noise-stripped output fits 3072 B");

    cJSON *parsed = cJSON_Parse(out);
    PCHECK(parsed != NULL, "noise-stripped output is valid JSON");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(parsed, "result");
    PCHECK(cJSON_IsObject(r), "result is an object");
    PCHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "truncated")),
           "truncated:true set after noise stripping");
    PCHECK(strcmp(cJSON_GetObjectItemCaseSensitive(r, "hint")->valuestring,
                  JR_TOOLS_PROJECT_HINT) == 0,
           "hint matches expected text");
    PCHECK(strcmp(cJSON_GetObjectItemCaseSensitive(r, "title")->valuestring,
                  "Release Checklist") == 0,
           "clean title preserved verbatim");
    PCHECK(cJSON_GetObjectItemCaseSensitive(r, "RAW_HTML") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "markdown_full") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "embedding") == NULL,
           "noise keys removed case-insensitively");

    cJSON_Delete(parsed);
    cJSON_Delete(src);
    return 0;
}

static int test_18kb_nested_payload_with_noise_and_50_items(int *checks)
{
    cJSON *src = cJSON_CreateObject();
    PCHECK(src != NULL, "create 18KB fixture");
    cJSON_AddStringToObject(src, "query", "esp32 glass display");
    cJSON_AddStringToObject(src, "raw", "top-level raw blob");
    cJSON_AddStringToObject(src, "html", "<html><body>ignored</body></html>");
    cJSON_AddStringToObject(src, "raw_html", "<html>raw</body></html>");
    cJSON_AddStringToObject(src, "debug", "debug info");
    cJSON_AddStringToObject(src, "trace", "trace info");
    cJSON_AddStringToObject(src, "stack", "Error: stack");
    cJSON_AddStringToObject(src, "headers", "x-req: 1");
    cJSON_AddStringToObject(src, "markdown_full", "# Full markdown");
    cJSON_AddArrayToObject(src, "embeddings");
    cJSON_AddArrayToObject(src, "vector");

    cJSON *items = cJSON_AddArrayToObject(src, "items");
    PCHECK(items != NULL, "create items array");
    for (int i = 0; i < 50; ++i) {
        cJSON *it = cJSON_CreateObject();
        char title[64];
        char snippet[200];
        snprintf(title, sizeof title, "Result Item #%d", i);
        snprintf(snippet, sizeof snippet,
                 "Structured snippet %d with useful context about JarvisNano audio and OLED.",
                 i);
        cJSON_AddNumberToObject(it, "id", i);
        cJSON_AddStringToObject(it, "title", title);
        cJSON_AddStringToObject(it, "snippet", snippet);
        cJSON_AddStringToObject(it, "html",
                                "<div class='card'><p>Bulky noisy HTML wrapper payload with extra markup and attributes</p></div>");
        cJSON *emb = cJSON_AddArrayToObject(it, "embedding");
        for (int j = 0; j < 40; ++j) {
            cJSON_AddItemToArray(emb, cJSON_CreateNumber(0.01 * j));
        }
        cJSON_AddItemToArray(items, it);
    }

    char *full = cJSON_PrintUnformatted(src);
    const size_t full_len = full != NULL ? strlen(full) : 0U;
    cJSON_free(full);
    PCHECK(full_len > 18000U, "fixture is > 18 KB raw JSON");

    char out[JR_TOOLS_RESPONSE_CAP];
    jr_tools_project_info_t info;
    PCHECK(jr_tools_project_json(src, out, sizeof out, &info),
           "18 KB fixture projects");
    PCHECK(strlen(out) < JR_TOOLS_RESPONSE_CAP,
           "projected output strictly < 3072 B");
    PCHECK(info.truncated && !info.used_fallback && info.tier_index >= 1,
           "projected via structured tier, not raw fallback");

    cJSON *parsed = cJSON_Parse(out);
    PCHECK(parsed != NULL, "projected output is valid JSON");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(parsed, "result");
    PCHECK(cJSON_IsObject(r), "result is structured object");
    PCHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "truncated")),
           "truncated:true present");
    PCHECK(strcmp(cJSON_GetObjectItemCaseSensitive(r, "hint")->valuestring,
                  JR_TOOLS_PROJECT_HINT) == 0,
           "hint present");
    PCHECK(strcmp(cJSON_GetObjectItemCaseSensitive(r, "query")->valuestring,
                  "esp32 glass display") == 0,
           "query field preserved");
    PCHECK(cJSON_GetObjectItemCaseSensitive(r, "html") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "raw_html") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "embeddings") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "vector") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "debug") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "trace") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "stack") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "headers") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "markdown_full") == NULL &&
               cJSON_GetObjectItemCaseSensitive(r, "raw") == NULL,
           "all noise keys stripped");

    const cJSON *proj_items = cJSON_GetObjectItemCaseSensitive(r, "items");
    PCHECK(cJSON_IsArray(proj_items) && cJSON_GetArraySize(proj_items) >= 2 &&
               cJSON_GetArraySize(proj_items) <= 8,
           "top items preserved in structured array");
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(r, "items_total");
    PCHECK(cJSON_IsNumber(total) && (int)total->valuedouble == 50,
           "items_total records original array length 50");

    const cJSON *first = cJSON_GetArrayItem(proj_items, 0);
    PCHECK(cJSON_IsObject(first), "first item is structured object");
    PCHECK((int)cJSON_GetObjectItemCaseSensitive(first, "id")->valuedouble == 0,
           "first item id preserved");
    PCHECK(strcmp(cJSON_GetObjectItemCaseSensitive(first, "title")->valuestring,
                  "Result Item #0") == 0,
           "first item title preserved");
    PCHECK(cJSON_GetObjectItemCaseSensitive(first, "html") == NULL &&
               cJSON_GetObjectItemCaseSensitive(first, "embedding") == NULL,
           "nested noise keys stripped inside items");

    cJSON_Delete(parsed);
    cJSON_Delete(src);
    return 0;
}

static int test_root_array_and_deep_nesting(int *checks)
{
    /* Root array of 60 items -> wrapped as {items, total, truncated, hint}. */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < 60; ++i) {
        cJSON *it = cJSON_CreateObject();
        char label[96];
        snprintf(label, sizeof label, "entry-%02d-with-extra-description-text-here", i);
        cJSON_AddNumberToObject(it, "n", i);
        cJSON_AddStringToObject(it, "label", label);
        cJSON_AddItemToArray(arr, it);
    }

    char out[JR_TOOLS_RESPONSE_CAP];
    jr_tools_project_info_t info;
    PCHECK(jr_tools_project_json(arr, out, sizeof out, &info), "root array projects");
    PCHECK(info.truncated && !info.used_fallback && strlen(out) < sizeof out,
           "root array fits in structured tier");
    cJSON *parsed = cJSON_Parse(out);
    PCHECK(parsed != NULL, "root array output parses");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(parsed, "result");
    PCHECK(cJSON_IsObject(r) &&
               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "truncated")) &&
               cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r, "items")) &&
               (int)cJSON_GetObjectItemCaseSensitive(r, "total")->valuedouble == 60,
           "root array wrapped with items, total=60, truncated=true");
    cJSON_Delete(parsed);
    cJSON_Delete(arr);

    /* Deeply nested JSON (24 levels) + bulky leaf. */
    cJSON *deep_root = cJSON_CreateObject();
    cJSON *cur = deep_root;
    for (int d = 0; d < 24; ++d) {
        char k[16];
        snprintf(k, sizeof k, "lvl%d", d);
        cur = cJSON_AddObjectToObject(cur, k);
    }
    char *leaf = make_repeated('Z', 4096U);
    PCHECK(leaf != NULL, "alloc deep leaf");
    cJSON_AddStringToObject(cur, "payload", leaf);
    free(leaf);

    PCHECK(jr_tools_project_json(deep_root, out, sizeof out, &info),
           "deeply nested JSON projects without stack issue");
    PCHECK(info.truncated && !info.used_fallback && strlen(out) < sizeof out,
           "deeply nested JSON fits in structured tier");
    parsed = cJSON_Parse(out);
    PCHECK(parsed != NULL, "deeply nested output parses");
    r = cJSON_GetObjectItemCaseSensitive(parsed, "result");
    PCHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "truncated")),
           "deeply nested output marked truncated");
    PCHECK(strstr(out, "\"{...}\"") != NULL,
           "deep branch collapsed to {...} marker");
    cJSON_Delete(parsed);
    cJSON_Delete(deep_root);
    return 0;
}

static int test_giant_strings_utf8_and_fallback(int *checks)
{
    /* Build a > 12 KB UTF-8 string with multi-byte chars (é = 2B, 🚀 = 4B)
     * so truncation boundaries fall across multi-byte sequences. */
    const char *unit = "caf\xc3\xa9-\xf0\x9f\x9a\x80-signal-";
    const size_t ulen = strlen(unit);
    const size_t reps = 800U;
    char *giant = (char *)malloc(ulen * reps + 1U);
    PCHECK(giant != NULL, "alloc giant utf8 string");
    for (size_t i = 0; i < reps; ++i) {
        memcpy(giant + i * ulen, unit, ulen);
    }
    giant[ulen * reps] = '\0';

    /* Case A: root string > 12 KB */
    cJSON *root_str = cJSON_CreateString(giant);
    char out[JR_TOOLS_RESPONSE_CAP];
    jr_tools_project_info_t info;
    PCHECK(jr_tools_project_json(root_str, out, sizeof out, &info),
           "giant root string projects");
    PCHECK(info.truncated && !info.used_fallback &&
               strlen(out) < JR_TOOLS_RESPONSE_CAP,
           "giant root string fits in structured tier");
    PCHECK(is_valid_utf8(out), "giant root string output is valid UTF-8");
    cJSON *parsed = cJSON_Parse(out);
    PCHECK(parsed != NULL, "giant root string output is valid JSON");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(parsed, "result");
    PCHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "truncated")),
           "root string marked truncated");
    const cJSON *txt = cJSON_GetObjectItemCaseSensitive(r, "text");
    PCHECK(cJSON_IsString(txt) && strstr(txt->valuestring, "...") != NULL &&
               is_valid_utf8(txt->valuestring),
           "root string text truncated cleanly on UTF-8 boundary");
    PCHECK((size_t)cJSON_GetObjectItemCaseSensitive(r, "total_chars")->valuedouble ==
               ulen * reps,
           "root string records total_chars");
    cJSON_Delete(parsed);

    /* Case B: tiny output cap forcing raw string-head fallback with escapes + UTF-8 */
    cJSON *hard = cJSON_CreateObject();
    cJSON_AddStringToObject(hard, "a", giant);
    cJSON_AddStringToObject(hard, "b", "\"quote\"\\slash\n\t\x01\x02\xc3\xa9\xf0\x9f\x9a\x80");
    char small_out[140];
    PCHECK(jr_tools_project_json(hard, small_out, sizeof small_out, &info),
           "tight cap triggers fallback cleanly");
    PCHECK(info.truncated && info.used_fallback &&
               strlen(small_out) < sizeof small_out,
           "fallback respects tight cap strictly");
    PCHECK(is_valid_utf8(small_out), "fallback output is valid UTF-8");
    parsed = cJSON_Parse(small_out);
    PCHECK(parsed != NULL, "fallback output is valid JSON");
    r = cJSON_GetObjectItemCaseSensitive(parsed, "result");
    PCHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "truncated")) &&
               cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "text")),
           "fallback carries truncated:true and text");
    cJSON_Delete(parsed);

    /* Case C: ultra-tiny cap (minimal envelope) and invalid caps */
    char min_out[32];
    PCHECK(jr_tools_project_json(hard, min_out, sizeof min_out, &info),
           "32-byte cap emits minimal truncated envelope");
    PCHECK(strcmp(min_out, "{\"result\":{\"truncated\":true}}") == 0,
           "minimal envelope exact shape");

    char too_small[16];
    PCHECK(!jr_tools_project_json(hard, too_small, sizeof too_small, &info) &&
               too_small[0] == '\0',
           "16-byte cap fails closed with empty output");
    PCHECK(!jr_tools_project_json(NULL, out, sizeof out, &info) &&
               out[0] == '\0',
           "NULL upstream fails closed");

    cJSON_Delete(hard);
    cJSON_Delete(root_str);
    free(giant);
    return 0;
}

static int test_progressive_tier_escalation_and_dedup(int *checks)
{
    /* Wide dense object (22 keys, each an array of 10 x 160-byte strings)
     * exceeds tier 1 (8,240,20,4) and must escalate to a tighter tier while
     * staying structured JSON (never raw fallback). */
    cJSON *wide = cJSON_CreateObject();
    char *s160 = make_repeated('x', 160U);
    PCHECK(wide != NULL && s160 != NULL, "alloc wide fixture");
    for (int k = 0; k < 22; ++k) {
        char kname[24];
        snprintf(kname, sizeof kname, "bucket_%02d", k);
        cJSON *arr = cJSON_AddArrayToObject(wide, kname);
        for (int i = 0; i < 10; ++i) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(s160));
        }
    }
    /* Also verify an existing bucket_00_total does not duplicate. */
    cJSON_AddNumberToObject(wide, "bucket_00_total", 10);
    free(s160);

    char out[JR_TOOLS_RESPONSE_CAP];
    jr_tools_project_info_t info;
    PCHECK(jr_tools_project_json(wide, out, sizeof out, &info),
           "wide dense fixture projects");
    PCHECK(info.truncated && !info.used_fallback && info.tier_index >= 2,
           "wide dense fixture escalates past tier 1 cleanly");
    PCHECK(strlen(out) < JR_TOOLS_RESPONSE_CAP, "wide output fits 3072 B");

    cJSON *parsed = cJSON_Parse(out);
    PCHECK(parsed != NULL, "wide output is valid JSON");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(parsed, "result");
    int total_occurrences = 0;
    for (const cJSON *c = r->child; c != NULL; c = c->next) {
        if (c->string != NULL && strcmp(c->string, "bucket_00_total") == 0) {
            total_occurrences++;
        }
    }
    PCHECK(total_occurrences == 1, "bucket_00_total appears exactly once");

    cJSON_Delete(parsed);
    cJSON_Delete(wide);
    return 0;
}

int project_tests(int *checks)
{
    if (test_small_passthrough(checks) ||
        test_noise_strip_only_pass(checks) ||
        test_18kb_nested_payload_with_noise_and_50_items(checks) ||
        test_root_array_and_deep_nesting(checks) ||
        test_giant_strings_utf8_and_fallback(checks) ||
        test_progressive_tier_escalation_and_dedup(checks)) {
        return 1;
    }
    return 0;
}
