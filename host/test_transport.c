/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_transport.c — Phase-1 Run-3 host suite for the L2 Gemini transport.
 *
 * Proves, on a laptop with NO ESP-IDF / NO socket / NO Gemini quota:
 *   - the SETUP/message BUILDERS (model pin, nested thinkingLevel, manual PTT,
 *     tools coexistence, and the NO-audioStreamEnd-in-manual guarantee);
 *   - the would-block-as-backpressure FRAMER (connection survives would-block /
 *     partial / soft-fail; bounded memory; drop-newest; a real close is death);
 *   - the inbound PARSER (server audio, turn signals, the nasty sequences:
 *     interrupted-without-generationComplete, toolCallCancellation, goAway
 *     Duration, sessionResumptionUpdate mid-stream);
 *   - the independent uplink/downlink liveness feed (split-brain detectable).
 *
 * Registered from test_core.c's main() via transport_tests_run() so the whole
 * suite is one ./build/jr_host_tests binary.
 */
#include "unity.h"
#include "jr_transport/gemini_live.h"
#include "jr_core/monitors.h"
#include "fake_ws_transport.h"
#include "fake_clock.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

/* ---- small helpers ---- */
static jr_gemini_config_t manual_cfg(void)
{
    jr_gemini_config_t c;
    memset(&c, 0, sizeof(c));
    c.vad_mode = JR_VAD_MANUAL_LOCAL_RMS;
    return c;
}

/* first event of a single-frame parse */
static jr_gemini_event_kind_t first_kind(jr_gemini_parse_t *p)
{
    return p->count ? p->events[0].kind : JR_GEV_UNKNOWN;
}

/* ===================================================================== *
 *  BUILDERS                                                             *
 * ===================================================================== */
static void test_setup_model_pinned(void)
{
    jr_gemini_config_t cfg = manual_cfg();
    char *j = jr_gemini_build_setup(&cfg);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = cJSON_Parse(j);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *model = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"), "model");
    TEST_ASSERT_TRUE(cJSON_IsString(model));
    TEST_ASSERT_EQUAL_STRING(JR_GEMINI_MODEL_PRIMARY, model->valuestring);
    TEST_ASSERT_EQUAL_STRING("models/gemini-3.1-flash-live-preview", model->valuestring);
    cJSON_Delete(root);
    free(j);
}

/* thinkingLevel MUST nest under generationConfig.thinkingConfig, NOT flat. */
static void test_setup_thinkingLevel_nested_not_flat(void)
{
    jr_gemini_config_t cfg = manual_cfg();
    cfg.thinking_level = "minimal";
    char *j = jr_gemini_build_setup(&cfg);
    cJSON *root = cJSON_Parse(j);
    cJSON *gc = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"), "generationConfig");
    TEST_ASSERT_NOT_NULL(gc);
    cJSON *tcfg = cJSON_GetObjectItem(gc, "thinkingConfig");
    TEST_ASSERT_NOT_NULL(tcfg);                                   /* nested exists */
    cJSON *tl = cJSON_GetObjectItem(tcfg, "thinkingLevel");
    TEST_ASSERT_TRUE(cJSON_IsString(tl));
    TEST_ASSERT_EQUAL_STRING("minimal", tl->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(gc, "thinkingLevel"));   /* NOT flat */
    /* responseModalities == [AUDIO] */
    cJSON *rm = cJSON_GetObjectItem(gc, "responseModalities");
    TEST_ASSERT_TRUE(cJSON_IsArray(rm));
    TEST_ASSERT_EQUAL_STRING("AUDIO", cJSON_GetArrayItem(rm, 0)->valuestring);
    cJSON_Delete(root);
    free(j);
}

static void test_setup_manual_ptt_config(void)
{
    jr_gemini_config_t cfg = manual_cfg();
    char *j = jr_gemini_build_setup(&cfg);
    cJSON *root = cJSON_Parse(j);
    cJSON *ric = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"), "realtimeInputConfig");
    cJSON *aad = cJSON_GetObjectItem(ric, "automaticActivityDetection");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(aad, "disabled")));
    TEST_ASSERT_EQUAL_STRING("NO_INTERRUPTION",
                             cJSON_GetObjectItem(ric, "activityHandling")->valuestring);
    /* setup NEVER contains audioStreamEnd */
    TEST_ASSERT_NULL(strstr(j, "audioStreamEnd"));
    cJSON_Delete(root);
    free(j);
}

static void test_setup_auto_vad_config(void)
{
    jr_gemini_config_t cfg = manual_cfg();
    cfg.vad_mode = JR_VAD_SERVER;
    char *j = jr_gemini_build_setup(&cfg);
    cJSON *root = cJSON_Parse(j);
    cJSON *ric = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"), "realtimeInputConfig");
    cJSON *aad = cJSON_GetObjectItem(ric, "automaticActivityDetection");
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(aad, "disabled")));
    TEST_ASSERT_EQUAL_STRING("START_OF_ACTIVITY_INTERRUPTS",
                             cJSON_GetObjectItem(ric, "activityHandling")->valuestring);
    cJSON_Delete(root);
    free(j);
}

/* functionDeclarations + googleSearch COEXIST in tools. */
static void test_setup_tools_coexist(void)
{
    jr_gemini_fn_decl_t fns[] = {
        { .name = "crypto_price", .description = "price of a coin",
          .arg_name = "symbol", .arg_desc = "coin symbol" },
    };
    jr_gemini_config_t cfg = manual_cfg();
    cfg.google_search = true;
    cfg.fns = fns;
    cfg.fn_count = 1;
    char *j = jr_gemini_build_setup(&cfg);
    cJSON *root = cJSON_Parse(j);
    cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"), "tools");
    TEST_ASSERT_TRUE(cJSON_IsArray(tools));
    int saw_gs = 0, saw_fd = 0;
    cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        if (cJSON_GetObjectItem(t, "googleSearch")) { saw_gs = 1; }
        if (cJSON_GetObjectItem(t, "functionDeclarations")) { saw_fd = 1; }
    }
    TEST_ASSERT_TRUE(saw_gs);
    TEST_ASSERT_TRUE(saw_fd);
    cJSON_Delete(root);
    free(j);
}

/* ---- ask_user: a tool whose argument is a string ARRAY ------------------- *
 * The blocker this proves fixed: jr_gemini_fn_decl_t used to carry exactly ONE
 * string argument, and the builder hardcoded "string" + a one-element "required"
 * array (gemini_live.c:195-201 before this change). A tool taking options[] was
 * simply not expressible.                                                     */

/* GOLDEN OUTPUT. Asserted whole, not field-by-field: this is the exact byte
 * string that goes on the wire, and "items" is the field whose absence makes
 * Gemini reject an array property outright. */
static void test_setup_ask_user_array_schema_golden(void)
{
    jr_gemini_fn_decl_t fns[] = { JR_GEMINI_ASK_USER_DECL };
    jr_gemini_config_t cfg = manual_cfg();
    cfg.fns = fns;
    cfg.fn_count = 1;
    char *j = jr_gemini_build_setup(&cfg);
    TEST_ASSERT_NOT_NULL(j);

    static const char kGolden[] =
        "{\"setup\":{\"model\":\"models/gemini-3.1-flash-live-preview\","
        "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
        "\"thinkingConfig\":{\"thinkingLevel\":\"low\"}},"
        "\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":true},"
        "\"activityHandling\":\"NO_INTERRUPTION\"},"
        "\"tools\":[{\"functionDeclarations\":[{\"name\":\"ask_user\","
        "\"description\":\"Ask Pascal a short multiple-choice question and wait "
        "for the tap. Use at most three options; keep each option under 16 "
        "characters.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"question\":{\"type\":\"string\","
        "\"description\":\"The question to show, at most 40 characters.\"},"
        "\"options\":{\"type\":\"array\","
        "\"description\":\"Two or three short answer choices, each under 16 "
        "characters.\",\"items\":{\"type\":\"string\"}}},"
        "\"required\":[\"question\",\"options\"]}}]}]}}";
    TEST_ASSERT_EQUAL_STRING(kGolden, j);

    /* Same claim structurally, so a future golden edit cannot quietly drop the
     * one field that makes an array property legal. */
    cJSON *root = cJSON_Parse(j);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *fdecls = cJSON_GetObjectItem(
        cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "setup"),
                                               "tools"), 0),
        "functionDeclarations");
    cJSON *params = cJSON_GetObjectItem(cJSON_GetArrayItem(fdecls, 0), "parameters");
    cJSON *opts = cJSON_GetObjectItem(cJSON_GetObjectItem(params, "properties"),
                                      "options");
    TEST_ASSERT_EQUAL_STRING("array", cJSON_GetObjectItem(opts, "type")->valuestring);
    cJSON *items = cJSON_GetObjectItem(opts, "items");       /* THE assertion */
    TEST_ASSERT_NOT_NULL(items);
    TEST_ASSERT_TRUE(cJSON_IsObject(items));
    TEST_ASSERT_EQUAL_STRING("string", cJSON_GetObjectItem(items, "type")->valuestring);
    /* both properties required, question first */
    cJSON *reqd = cJSON_GetObjectItem(params, "required");
    TEST_ASSERT_TRUE(cJSON_IsArray(reqd));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(reqd));
    TEST_ASSERT_EQUAL_STRING("question", cJSON_GetArrayItem(reqd, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("options", cJSON_GetArrayItem(reqd, 1)->valuestring);
    cJSON_Delete(root);
    free(j);
}

/* BACKWARD COMPAT, the load-bearing half. The struct grew; every tool declared
 * in main.c still uses the legacy single-string form, and its emitted schema
 * must be byte-for-byte what it was BEFORE the struct grew. This golden string
 * was captured from the pre-change emitter and pasted verbatim.
 *
 * The initializer below is deliberately POSITIONAL and four-wide — the exact
 * pre-change source form — so this also proves old call sites still compile. */
static void test_setup_legacy_single_string_decl_byte_identical(void)
{
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
    jr_gemini_fn_decl_t fns[] = {
        { "crypto_price", "price of a coin", "symbol", "coin symbol" },
    };
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    jr_gemini_config_t cfg = manual_cfg();
    cfg.fns = fns;
    cfg.fn_count = 1;
    char *j = jr_gemini_build_setup(&cfg);
    TEST_ASSERT_NOT_NULL(j);

    static const char kPreChangeGolden[] =
        "{\"setup\":{\"model\":\"models/gemini-3.1-flash-live-preview\","
        "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
        "\"thinkingConfig\":{\"thinkingLevel\":\"low\"}},"
        "\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":true},"
        "\"activityHandling\":\"NO_INTERRUPTION\"},"
        "\"tools\":[{\"functionDeclarations\":[{\"name\":\"crypto_price\","
        "\"description\":\"price of a coin\",\"parameters\":{\"type\":\"object\","
        "\"properties\":{\"symbol\":{\"type\":\"string\","
        "\"description\":\"coin symbol\"}},\"required\":[\"symbol\"]}}]}]}}";
    TEST_ASSERT_EQUAL_STRING(kPreChangeGolden, j);
    /* no array machinery leaked into a plain string tool */
    TEST_ASSERT_NULL(strstr(j, "items"));
    TEST_ASSERT_NULL(strstr(j, "array"));
    free(j);
}

/* PARSER: a toolCall carrying an options ARRAY must surface the question and
 * every option to the caller without the caller re-parsing tool_args. */
static void test_parse_toolCall_ask_user_options(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"ask_user\","
        "\"id\":\"call_ask_7\",\"args\":{\"question\":\"Which mood?\","
        "\"options\":[\"Awake\",\"Ambient\",\"Whisper\"]}}]}}", &p));

    TEST_ASSERT_EQUAL_size_t(1, p.count);
    const jr_gemini_event_t *ev = &p.events[0];
    TEST_ASSERT_EQUAL_INT(JR_GEV_TOOL_CALL, ev->kind);
    TEST_ASSERT_EQUAL_STRING(JR_GEMINI_ASK_USER_TOOL, ev->tool_name);
    TEST_ASSERT_EQUAL_STRING("call_ask_7", ev->call_id_text);   /* echoed in the response */
    TEST_ASSERT_NOT_EQUAL(0, ev->call_id);

    /* the question */
    const char *q = jr_gemini_tool_arg_string(ev, JR_GEMINI_ASK_USER_ARG_QUESTION);
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_STRING("Which mood?", q);

    /* ALL the options, in order */
    const char *opts[JR_GEMINI_ASK_USER_MAX_CHOICES];
    size_t n = jr_gemini_tool_arg_string_array(ev, JR_GEMINI_ASK_USER_ARG_OPTIONS,
                                               opts, JR_GEMINI_ASK_USER_MAX_CHOICES);
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_STRING("Awake",   opts[0]);
    TEST_ASSERT_EQUAL_STRING("Ambient", opts[1]);
    TEST_ASSERT_EQUAL_STRING("Whisper", opts[2]);

    /* absent / wrong-typed keys are NULL-or-0, never a crash */
    TEST_ASSERT_NULL(jr_gemini_tool_arg_string(ev, "nope"));
    TEST_ASSERT_NULL(jr_gemini_tool_arg_string(ev, JR_GEMINI_ASK_USER_ARG_OPTIONS));
    TEST_ASSERT_EQUAL_size_t(0, jr_gemini_tool_arg_string_array(
        ev, JR_GEMINI_ASK_USER_ARG_QUESTION, opts, 3));
    /* the raw printed args still work for every existing consumer */
    TEST_ASSERT_NOT_NULL(strstr(ev->tool_args, "Ambient"));
    jr_gemini_parse_free(&p);

    /* Over-long and dirty lists: bounded to `max`, non-strings skipped, and a
     * legacy no-array tool reports 0 rather than reading garbage. */
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"ask_user\",\"id\":\"c8\","
        "\"args\":{\"options\":[\"a\",7,\"b\",null,\"c\",\"d\"]}}]}}", &p));
    const char *few[2];
    TEST_ASSERT_EQUAL_size_t(2, jr_gemini_tool_arg_string_array(
        &p.events[0], JR_GEMINI_ASK_USER_ARG_OPTIONS, few, 2));   /* capped */
    TEST_ASSERT_EQUAL_STRING("a", few[0]);
    TEST_ASSERT_EQUAL_STRING("b", few[1]);                        /* 7 skipped */
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"weather\",\"id\":\"c9\","
        "\"args\":{\"location\":\"Fort Lauderdale\"}}]}}", &p));
    TEST_ASSERT_EQUAL_size_t(0, jr_gemini_tool_arg_string_array(
        &p.events[0], JR_GEMINI_ASK_USER_ARG_OPTIONS, opts, 3));
    TEST_ASSERT_EQUAL_STRING("Fort Lauderdale",
                             jr_gemini_tool_arg_string(&p.events[0], "location"));
    jr_gemini_parse_free(&p);
}

/* REGRESSION (major, cross-layer UAF): the Asking state holds the question and
 * options for up to 120 s, but the parse tree dies the moment the rich callback
 * returns. Every accessor pointer ALIASES that tree. jr_gemini_event_to_ask()
 * must produce storage that owes the tree nothing.
 *
 * The test would have caught a consumer holding the borrow two ways:
 *   1. structurally — every string must live INSIDE the caller's struct;
 *   2. dynamically  — the snapshot is re-read after jr_gemini_parse_free() and
 *      after the source JSON buffer is poisoned and freed. A borrowed pointer
 *      reads freed heap here (a hard fault under ASAN, garbage without it). */
static const jr_gemini_ask_t *g_ask_seen;   /* what a callback stashed */
static jr_gemini_ask_t g_ask_slot;
static bool g_ask_ok;

static void ask_snapshot_cb(void *user, const jr_gemini_event_t *ev)
{
    (void)user;
    if (ev->kind != JR_GEV_TOOL_CALL) { return; }
    g_ask_ok   = jr_gemini_event_to_ask(ev, &g_ask_slot);
    g_ask_seen = &g_ask_slot;
}

static bool inside(const void *p, const void *base, size_t n)
{
    const char *c = (const char *)p, *b = (const char *)base;
    return c >= b && c < b + n;
}

static void test_ask_snapshot_outlives_the_parse_tree(void)
{
    /* heap-allocated source so the poison-after-free is real memory the
     * borrowed pointers would still be aiming at */
    const char *src =
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"ask_user\","
        "\"id\":\"call_ask_7\",\"args\":{\"question\":\"Which mood?\","
        "\"options\":[\"Awake\",\"Ambient\",\"Whisper\"]}}]}}";
    char *json = (char *)malloc(strlen(src) + 1);
    TEST_ASSERT_NOT_NULL(json);
    memcpy(json, src, strlen(src) + 1);

    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(json, &p));
    jr_gemini_ask_t ask;
    TEST_ASSERT_TRUE(jr_gemini_event_to_ask(&p.events[0], &ask));

    /* (1) structural: nothing points out of the caller's own struct */
    TEST_ASSERT_TRUE(inside(ask.question, &ask, sizeof(ask)));
    TEST_ASSERT_TRUE(inside(ask.options[0], &ask, sizeof(ask)));
    TEST_ASSERT_TRUE(inside(ask.call_id, &ask, sizeof(ask)));

    /* (2) dynamic: burn the tree and the source bytes, then read again */
    jr_gemini_parse_free(&p);
    memset(json, 'X', strlen(src));
    free(json);

    TEST_ASSERT_EQUAL_STRING("Which mood?", ask.question);
    TEST_ASSERT_EQUAL_STRING("call_ask_7", ask.call_id);
    TEST_ASSERT_EQUAL_UINT8(3, ask.count);
    TEST_ASSERT_EQUAL_STRING("Awake",   ask.options[0]);
    TEST_ASSERT_EQUAL_STRING("Ambient", ask.options[1]);
    TEST_ASSERT_EQUAL_STRING("Whisper", ask.options[2]);
    TEST_ASSERT_FALSE(ask.truncated);
    /* the id round-trips into the functionResponse the core will send later */
    char *j = jr_gemini_build_ask_user_response(ask.call_id, ask.options[1]);
    TEST_ASSERT_NOT_NULL(strstr(j, "call_ask_7"));
    TEST_ASSERT_NOT_NULL(strstr(j, "Ambient"));
    free(j);
}

/* Same guarantee through the REAL pump path: the pump frees the tree right
 * after the callback returns, so a snapshot taken inside the callback is the
 * only thing a 120 s Asking state may keep. */
static void test_ask_snapshot_survives_pump_rx_free(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN;
    fake_ws_push_inbox(&f,
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"ask_user\",\"id\":\"c42\","
        "\"args\":{\"question\":\"Lights?\",\"options\":[\"On\",\"Off\"]}}]}}");
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);
    jr_gemini_client_set_event_cb(&c, ask_snapshot_cb, NULL);

    memset(&g_ask_slot, 0, sizeof(g_ask_slot));
    g_ask_seen = NULL; g_ask_ok = false;
    TEST_ASSERT_TRUE(jr_gemini_pump_rx(&c));

    /* tree is gone by now; the stashed snapshot must still read clean */
    TEST_ASSERT_TRUE(g_ask_ok);
    TEST_ASSERT_NOT_NULL(g_ask_seen);
    TEST_ASSERT_EQUAL_STRING("Lights?", g_ask_seen->question);
    TEST_ASSERT_EQUAL_STRING("c42",     g_ask_seen->call_id);
    TEST_ASSERT_EQUAL_UINT8(2, g_ask_seen->count);
    TEST_ASSERT_EQUAL_STRING("On",  g_ask_seen->options[0]);
    TEST_ASSERT_EQUAL_STRING("Off", g_ask_seen->options[1]);
    TEST_ASSERT_EQUAL_STRING("",    g_ask_seen->options[2]);   /* unused arc is empty */

    jr_gemini_client_deinit(&c);
}

/* Rejection + truncation: a snapshot never half-fills, and clipping is LOUD. */
static void test_ask_snapshot_rejects_and_flags_truncation(void)
{
    jr_gemini_parse_t p;
    jr_gemini_ask_t ask;

    /* a different tool is not an ask */
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"weather\",\"id\":\"c9\","
        "\"args\":{\"location\":\"Fort Lauderdale\"}}]}}", &p));
    memset(&ask, 0xAB, sizeof(ask));
    TEST_ASSERT_FALSE(jr_gemini_event_to_ask(&p.events[0], &ask));
    TEST_ASSERT_EQUAL_UINT8(0, ask.count);          /* zeroed, not garbage */
    TEST_ASSERT_EQUAL_STRING("", ask.question);
    jr_gemini_parse_free(&p);

    /* ask_user with no options is unanswerable -> rejected */
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"ask_user\",\"id\":\"c1\","
        "\"args\":{\"question\":\"Well?\"}}]}}", &p));
    TEST_ASSERT_FALSE(jr_gemini_event_to_ask(&p.events[0], &ask));
    jr_gemini_parse_free(&p);

    /* ask_user with no question is undrawable -> rejected */
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"ask_user\",\"id\":\"c2\","
        "\"args\":{\"options\":[\"a\",\"b\"]}}]}}", &p));
    TEST_ASSERT_FALSE(jr_gemini_event_to_ask(&p.events[0], &ask));
    jr_gemini_parse_free(&p);

    /* a NULL event is inert, not a crash */
    TEST_ASSERT_FALSE(jr_gemini_event_to_ask(NULL, &ask));
    TEST_ASSERT_FALSE(jr_gemini_event_to_ask(NULL, NULL));

    /* four options: the fourth arc cannot be drawn -> kept 3 AND flagged */
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"ask_user\",\"id\":\"c3\","
        "\"args\":{\"question\":\"Pick\",\"options\":[\"a\",\"b\",\"c\",\"d\"]}}]}}", &p));
    TEST_ASSERT_TRUE(jr_gemini_event_to_ask(&p.events[0], &ask));
    TEST_ASSERT_EQUAL_UINT8(JR_GEMINI_ASK_USER_MAX_CHOICES, ask.count);
    TEST_ASSERT_TRUE(ask.truncated);
    jr_gemini_parse_free(&p);

    /* an over-long question is clipped, NUL-terminated, and flagged */
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"ask_user\",\"id\":\"c4\","
        "\"args\":{\"question\":\"" /* 60 chars, cap is 48 */
        "012345678901234567890123456789012345678901234567890123456789\","
        "\"options\":[\"yes\"]}}]}}", &p));
    TEST_ASSERT_TRUE(jr_gemini_event_to_ask(&p.events[0], &ask));
    TEST_ASSERT_TRUE(ask.truncated);
    TEST_ASSERT_EQUAL_size_t(JR_GEMINI_ASK_QUESTION_CAP - 1u, strlen(ask.question));
    TEST_ASSERT_EQUAL_UINT8(1, ask.count);
    jr_gemini_parse_free(&p);
}

/* REGRESSION (minor): an over-long declaration must never ship a schema that
 * silently lost a property. build_setup refuses the whole message instead. */
static void test_setup_refuses_overlong_declaration(void)
{
    jr_gemini_fn_decl_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.name = "too_many";
    bad.description = "d";
    bad.params[0].name = "a";
    bad.params[1].name = "b";
    bad.param_count = JR_GEMINI_MAX_FN_PARAMS + 1u;   /* the lie */

    jr_gemini_config_t cfg = manual_cfg();
    cfg.fns = &bad; cfg.fn_count = 1;
    TEST_ASSERT_NULL(jr_gemini_build_setup(&cfg));    /* loud, not truncated */

    /* an honest declaration at exactly the cap still builds */
    bad.param_count = JR_GEMINI_PARAM_COUNT(JR_GEMINI_MAX_FN_PARAMS);
    for (size_t i = 0; i < JR_GEMINI_MAX_FN_PARAMS; i++) { bad.params[i].name = "p"; }
    char *j = jr_gemini_build_setup(&cfg);
    TEST_ASSERT_NOT_NULL(j);
    free(j);
}

/* The answer goes home: a chosen option becomes a well-formed functionResponse
 * on the SAME toolResponse path every other tool uses. */
static void test_build_ask_user_response_for_chosen_option(void)
{
    char *j = jr_gemini_build_ask_user_response("call_ask_7", "Ambient");
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = cJSON_Parse(j);
    TEST_ASSERT_NOT_NULL(root);                    /* well-formed JSON */
    cJSON *fr = cJSON_GetArrayItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "toolResponse"),
                            "functionResponses"), 0);
    TEST_ASSERT_EQUAL_STRING("call_ask_7", cJSON_GetObjectItem(fr, "id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("ask_user", cJSON_GetObjectItem(fr, "name")->valuestring);
    cJSON *resp = cJSON_GetObjectItem(fr, "response");
    TEST_ASSERT_TRUE(cJSON_IsObject(resp));        /* an OBJECT, not a bare string */
    TEST_ASSERT_EQUAL_STRING("Ambient", cJSON_GetObjectItem(resp, "answer")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(resp, "error"));
    cJSON_Delete(root);
    free(j);

    /* Identical shape to a hand-built tool response — one emitter, one shape. */
    char *manual = jr_gemini_build_tool_response("call_ask_7", "ask_user",
                                                 "{\"answer\":\"Ambient\"}");
    TEST_ASSERT_EQUAL_STRING(manual, j = jr_gemini_build_ask_user_response(
                                 "call_ask_7", "Ambient"));
    free(manual);
    free(j);

    /* An option containing a quote is ESCAPED, not spliced — the frame survives
     * and the answer round-trips exactly. */
    j = jr_gemini_build_ask_user_response("c1", "Say \"yes\"\\now");
    TEST_ASSERT_NOT_NULL(j);
    root = cJSON_Parse(j);
    TEST_ASSERT_NOT_NULL(root);
    fr = cJSON_GetArrayItem(cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "toolResponse"), "functionResponses"), 0);
    TEST_ASSERT_EQUAL_STRING("Say \"yes\"\\now",
        cJSON_GetObjectItem(cJSON_GetObjectItem(fr, "response"), "answer")->valuestring);
    cJSON_Delete(root);
    free(j);

    /* No answer (dismissed arc) is still valid JSON, never NULL-deref. */
    j = jr_gemini_build_ask_user_response("c2", NULL);
    TEST_ASSERT_NOT_NULL(j);
    root = cJSON_Parse(j);
    TEST_ASSERT_NOT_NULL(root);
    fr = cJSON_GetArrayItem(cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "toolResponse"), "functionResponses"), 0);
    TEST_ASSERT_EQUAL_STRING("",
        cJSON_GetObjectItem(cJSON_GetObjectItem(fr, "response"), "answer")->valuestring);
    cJSON_Delete(root);
    free(j);
}

static void test_build_control_frames(void)
{
    char *as = jr_gemini_build_activity_start();
    char *ae = jr_gemini_build_activity_end();
    char *se = jr_gemini_build_audio_stream_end();
    TEST_ASSERT_EQUAL_STRING("{\"realtimeInput\":{\"activityStart\":{}}}", as);
    TEST_ASSERT_EQUAL_STRING("{\"realtimeInput\":{\"activityEnd\":{}}}", ae);
    TEST_ASSERT_NOT_NULL(strstr(se, "audioStreamEnd"));
    free(as); free(ae); free(se);
}

/* Build an uplink audio chunk, re-embed its base64 in a downlink inlineData
 * frame, parse it back: proves PCM16 encode + decode round-trips and the parser
 * decodes server audio + the mimeType rate. */
static void test_build_audio_chunk_roundtrip(void)
{
    jr_pcm_t in[16];
    for (int i = 0; i < 16; ++i) { in[i] = (jr_pcm_t)((i * 4099) - 20000); }
    char *chunk = jr_gemini_build_audio_chunk(in, 16, JR_GEMINI_TX_RATE);
    TEST_ASSERT_NOT_NULL(chunk);
    cJSON *root = cJSON_Parse(chunk);
    cJSON *audio = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "realtimeInput"), "audio");
    TEST_ASSERT_NOT_NULL(strstr(cJSON_GetObjectItem(audio, "mimeType")->valuestring, "rate=16000"));
    const char *b64 = cJSON_GetObjectItem(audio, "data")->valuestring;

    char frame[512];
    snprintf(frame, sizeof(frame),
             "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":"
             "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"%s\"}}]}}}", b64);
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(frame, &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_AUDIO_CHUNK, first_kind(&p));
    TEST_ASSERT_EQUAL_size_t(16, p.events[0].pcm_len);
    TEST_ASSERT_EQUAL_UINT32(24000, p.events[0].sample_rate);
    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_INT16(in[i], p.events[0].pcm[i]);
    }
    jr_gemini_parse_free(&p);
    cJSON_Delete(root);
    free(chunk);
}

static void test_build_tool_response_preserves_original_id(void)
{
    char *j = jr_gemini_build_tool_response(
        "call_original_42", "current_time", "{\"result\":\"12:34\"}");
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = cJSON_Parse(j);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *responses = cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "toolResponse"), "functionResponses");
    cJSON *fr = cJSON_GetArrayItem(responses, 0);
    TEST_ASSERT_EQUAL_STRING("call_original_42",
                             cJSON_GetObjectItem(fr, "id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("current_time",
                             cJSON_GetObjectItem(fr, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("12:34",
        cJSON_GetObjectItem(cJSON_GetObjectItem(fr, "response"), "result")->valuestring);
    cJSON_Delete(root);
    free(j);

    j = jr_gemini_build_tool_response("id", "tool", "not-json");
    root = cJSON_Parse(j);
    fr = cJSON_GetArrayItem(cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "toolResponse"), "functionResponses"), 0);
    TEST_ASSERT_EQUAL_STRING("not-json",
        cJSON_GetObjectItem(cJSON_GetObjectItem(fr, "response"), "error")->valuestring);
    cJSON_Delete(root);
    free(j);
}

/* ===================================================================== *
 *  FRAMER — would-block as backpressure (the load-bearing win)          *
 * ===================================================================== */

/* A socket that ALWAYS would-blocks: EVERY send is backpressure, the connection
 * NEVER tears down, memory stays bounded, and drop-newest kicks in past cap. */
static void test_framer_wouldblock_never_aborts(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_WOULD_BLOCK;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);

    for (int i = 0; i < 200; ++i) {
        jr_err_t r = jr_gemini_send_frame(&c, "audioframe", 10);
        TEST_ASSERT_EQUAL_INT(JR_ERR_WOULD_BLOCK, r);        /* backpressure */
        TEST_ASSERT_TRUE(jr_err_is_backpressure(r));
        TEST_ASSERT_TRUE(jr_gemini_txq_depth(&c) <= JR_GEMINI_TXQ_DEPTH); /* bounded */
    }
    TEST_ASSERT_EQUAL_size_t(JR_GEMINI_TXQ_DEPTH, jr_gemini_txq_depth(&c));
    TEST_ASSERT_TRUE(c.live.tx_drops > 0);                    /* drop-newest fired */
    TEST_ASSERT_EQUAL_UINT(0, f.close_calls);                /* NEVER aborted */
    TEST_ASSERT_EQUAL_INT(JR_WS_OPEN, f.state);              /* connection kept */
}

/* Bounded, drop-NEWEST, oldest-retained: overfill while blocked, then drain and
 * assert the first JR_GEMINI_TXQ_DEPTH frames survived in order, the newest were
 * dropped, and nothing crashed. */
static void test_framer_drop_newest_oldest_retained(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_WOULD_BLOCK;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);

    const int N = JR_GEMINI_TXQ_DEPTH + 3;
    char tag[8];
    for (int i = 0; i < N; ++i) {
        snprintf(tag, sizeof(tag), "F%02d", i);              /* distinct 3-byte frames */
        jr_gemini_send_frame(&c, tag, 3);
    }
    TEST_ASSERT_EQUAL_size_t(JR_GEMINI_TXQ_DEPTH, jr_gemini_txq_depth(&c));
    TEST_ASSERT_EQUAL_UINT32(3, c.live.tx_drops);            /* the 3 newest dropped */

    f.send_mode = FWS_OK;                                    /* socket drains */
    TEST_ASSERT_EQUAL_INT(JR_OK, jr_gemini_flush(&c));
    TEST_ASSERT_EQUAL_size_t(0, jr_gemini_txq_depth(&c));
    /* oldest retained, in order; the dropped newest are absent */
    for (int i = 0; i < JR_GEMINI_TXQ_DEPTH; ++i) {
        snprintf(tag, sizeof(tag), "F%02d", i);
        TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, tag));
    }
    snprintf(tag, sizeof(tag), "F%02d", JR_GEMINI_TXQ_DEPTH);
    TEST_ASSERT_FALSE(fake_ws_sent_contains(&f, tag));       /* newest dropped */
}

/* A partial write buffers the remainder; a later flush completes the frame with
 * no loss and no teardown. */
static void test_framer_partial_then_flush_completes(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN;
    f.send_mode = FWS_PARTIAL; f.partial_bytes = 4;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);

    const char *frame = "HELLO-WORLD";                       /* 11 bytes */
    jr_err_t r = jr_gemini_send_frame(&c, frame, strlen(frame));
    TEST_ASSERT_EQUAL_INT(JR_ERR_WOULD_BLOCK, r);            /* remainder buffered */
    TEST_ASSERT_EQUAL_size_t(1, jr_gemini_txq_depth(&c));

    f.send_mode = FWS_OK;
    TEST_ASSERT_EQUAL_INT(JR_OK, jr_gemini_flush(&c));
    TEST_ASSERT_EQUAL_size_t(0, jr_gemini_txq_depth(&c));
    TEST_ASSERT_EQUAL_STRING("HELLO-WORLD", f.sent_log);     /* delivered whole */
    TEST_ASSERT_EQUAL_UINT(0, f.close_calls);
}

static void test_framer_ok_passthrough(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_OK;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);
    TEST_ASSERT_EQUAL_INT(JR_OK, jr_gemini_send_frame(&c, "ping", 4));
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, "ping"));
    TEST_ASSERT_EQUAL_size_t(0, jr_gemini_txq_depth(&c));
}

/* A real close is NOT backpressure — it routes to the death path. */
static void test_framer_real_close_is_death(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_CLOSED;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);
    jr_err_t r = jr_gemini_send_frame(&c, "x", 1);
    TEST_ASSERT_EQUAL_INT(JR_ERR_CLOSED, r);
    TEST_ASSERT_FALSE(jr_err_is_backpressure(r));            /* death, not backpressure */
    TEST_ASSERT_FALSE(c.live.socket_open);
}

/* A reconnect is a protocol boundary: no frame buffered for the dead socket may
 * precede Setup on the replacement socket. */
static void test_framer_session_reset_drops_stale_tx(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_WOULD_BLOCK;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);

    TEST_ASSERT_EQUAL_INT(JR_ERR_WOULD_BLOCK,
                          jr_gemini_send_frame(&c, "stale-audio", 11));
    TEST_ASSERT_EQUAL_size_t(1, jr_gemini_txq_depth(&c));

    jr_gemini_reset_tx(&c);
    TEST_ASSERT_EQUAL_size_t(0, jr_gemini_txq_depth(&c));

    f.send_mode = FWS_OK;
    TEST_ASSERT_EQUAL_INT(JR_OK, jr_gemini_send_frame(&c, "setup", 5));
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, "setup"));
    TEST_ASSERT_FALSE(fake_ws_sent_contains(&f, "stale-audio"));
}

/* ===================================================================== *
 *  RVC control guarantee — audioStreamEnd is IMPOSSIBLE in manual mode  *
 * ===================================================================== */
static void test_manual_never_emits_audioStreamEnd(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_OK;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);
    jr_realtime_voice_client_t rvc = jr_gemini_client_as_rvc(&c);

    /* manual mode: audioStreamEnd is refused (no bytes sent) ... */
    TEST_ASSERT_EQUAL_INT(JR_OK, rvc.send_control(rvc.ctx, JR_RVC_CTRL_AUDIO_STREAM_END));
    TEST_ASSERT_FALSE(fake_ws_sent_contains(&f, "audioStreamEnd"));
    /* ... but activityEnd IS the manual turn-boundary signal */
    TEST_ASSERT_EQUAL_INT(JR_OK, rvc.send_control(rvc.ctx, JR_RVC_CTRL_ACTIVITY_END));
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, "activityEnd"));

    /* auto-VAD mode DOES emit audioStreamEnd (and no-ops activityStart) */
    fake_ws_t g; fake_ws_init(&g); g.state = JR_WS_OPEN; g.send_mode = FWS_OK;
    jr_gemini_config_t acfg = manual_cfg(); acfg.vad_mode = JR_VAD_SERVER;
    jr_gemini_client_t ac;
    jr_gemini_client_init(&ac, fake_ws_make(&g), fake_clock_make(), &acfg);
    jr_realtime_voice_client_t arvc = jr_gemini_client_as_rvc(&ac);
    TEST_ASSERT_EQUAL_INT(JR_OK, arvc.send_control(arvc.ctx, JR_RVC_CTRL_AUDIO_STREAM_END));
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&g, "audioStreamEnd"));
}

/* connect() opens the socket AND sends the setup frame (hidden behind the port). */
static void test_connect_sends_setup(void)
{
    fake_ws_t f; fake_ws_init(&f); f.send_mode = FWS_OK;
    jr_gemini_config_t cfg = manual_cfg();
    cfg.url = "wss://example/fake";
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);
    jr_realtime_voice_client_t rvc = jr_gemini_client_as_rvc(&c);
    TEST_ASSERT_EQUAL_INT(JR_OK, rvc.connect(rvc.ctx));
    TEST_ASSERT_EQUAL_INT(JR_WS_OPEN, f.state);
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, "\"setup\""));
    TEST_ASSERT_TRUE(fake_ws_sent_contains(&f, JR_GEMINI_MODEL_PRIMARY));
}

/* ===================================================================== *
 *  PARSER — server frame -> typed events                                *
 * ===================================================================== */
static void test_parse_setupComplete(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse("{\"setupComplete\":{}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_SETUP_COMPLETE, first_kind(&p));
    jr_gemini_parse_free(&p);
}

/* A single serverContent frame carrying audio AND turnComplete demuxes to two
 * ordered events. */
static void test_parse_audio_and_turnComplete(void)
{
    /* "AAAA" base64 = 3 zero bytes -> not int16-aligned; use "AAAAAAAA" = 6 zero
     * bytes -> 3 samples of 0. */
    const char *frame =
        "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":"
        "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAAAAAAA\"}}]},"
        "\"turnComplete\":true}}";
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(frame, &p));
    TEST_ASSERT_EQUAL_size_t(2, p.count);
    TEST_ASSERT_EQUAL_INT(JR_GEV_AUDIO_CHUNK, p.events[0].kind);
    TEST_ASSERT_EQUAL_size_t(3, p.events[0].pcm_len);
    TEST_ASSERT_EQUAL_INT(JR_GEV_TURN_COMPLETE, p.events[1].kind);
    jr_gemini_parse_free(&p);
}

static void test_parse_mediaChunks_array_and_object(void)
{
    const char *array_frame =
        "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"mediaChunks\":["
        "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAAAAAAA\"},"
        "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAAAAAAA\"}]}]}}}";
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(array_frame, &p));
    TEST_ASSERT_EQUAL_size_t(2, p.count);
    TEST_ASSERT_EQUAL_INT(JR_GEV_AUDIO_CHUNK, p.events[0].kind);
    TEST_ASSERT_EQUAL_INT(JR_GEV_AUDIO_CHUNK, p.events[1].kind);
    jr_gemini_parse_free(&p);

    const char *object_frame =
        "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"mediaChunks\":"
        "{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"AAAAAAAA\"}}]}}}";
    TEST_ASSERT_TRUE(jr_gemini_parse(object_frame, &p));
    TEST_ASSERT_EQUAL_size_t(1, p.count);
    TEST_ASSERT_EQUAL_INT(JR_GEV_AUDIO_CHUNK, p.events[0].kind);
    TEST_ASSERT_EQUAL_UINT32(16000, p.events[0].sample_rate);
    jr_gemini_parse_free(&p);
}

/* NASTY SEQUENCE: interrupted with NO generationComplete, then a bare
 * turnComplete — exactly the interrupted -> turnComplete path (generationComplete
 * is skipped on an interrupted turn). */
static void test_parse_interrupted_without_generationComplete(void)
{
    jr_gemini_parse_t a;
    TEST_ASSERT_TRUE(jr_gemini_parse("{\"serverContent\":{\"interrupted\":true}}", &a));
    TEST_ASSERT_EQUAL_size_t(1, a.count);
    TEST_ASSERT_EQUAL_INT(JR_GEV_INTERRUPTED, a.events[0].kind);
    for (size_t i = 0; i < a.count; ++i) {
        TEST_ASSERT_NOT_EQUAL(JR_GEV_GENERATION_COMPLETE, a.events[i].kind);
    }
    jr_gemini_parse_free(&a);

    jr_gemini_parse_t b;
    TEST_ASSERT_TRUE(jr_gemini_parse("{\"serverContent\":{\"turnComplete\":true}}", &b));
    TEST_ASSERT_EQUAL_INT(JR_GEV_TURN_COMPLETE, first_kind(&b));
    jr_gemini_parse_free(&b);
}

/* goAway.timeLeft is a protobuf Duration (string "60s", object {seconds}, or a
 * bare number) — decoded to whole seconds, never treated as a raw field. */
static void test_parse_goAway_duration(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse("{\"goAway\":{\"timeLeft\":\"60s\"}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_GO_AWAY, first_kind(&p));
    TEST_ASSERT_EQUAL_UINT32(60, p.events[0].go_away_seconds);
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse("{\"goAway\":{\"timeLeft\":{\"seconds\":30}}}", &p));
    TEST_ASSERT_EQUAL_UINT32(30, p.events[0].go_away_seconds);
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse("{\"goAway\":{\"timeLeft\":10}}", &p));
    TEST_ASSERT_EQUAL_UINT32(10, p.events[0].go_away_seconds);
    jr_gemini_parse_free(&p);
}

/* sessionResumptionUpdate mid-stream: a usable (resumable) handle yields a
 * nonzero token; a non-resumable update yields token 0 (only usable handles
 * are stored). Both are heartbeats. */
static void test_parse_sessionResumption_resumable_gate(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"sessionResumptionUpdate\":{\"newHandle\":\"H1\",\"resumable\":true}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_RESUMPTION_UPDATE, first_kind(&p));
    TEST_ASSERT_TRUE(p.events[0].resumable);
    TEST_ASSERT_NOT_EQUAL(0, p.events[0].resumption_token);
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"sessionResumptionUpdate\":{\"newHandle\":\"\",\"resumable\":false}}", &p));
    TEST_ASSERT_FALSE(p.events[0].resumable);
    TEST_ASSERT_EQUAL_UINT32(0, p.events[0].resumption_token);   /* unusable => 0 */
    jr_gemini_parse_free(&p);
}

/* toolCall dispatch + toolCallCancellation: the SAME id string maps to the SAME
 * uint32 call_id, so a cancellation matches its call. */
static void test_parse_toolCall_and_cancellation(void)
{
    jr_gemini_parse_t call;
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCall\":{\"functionCalls\":[{\"name\":\"get_weather\","
        "\"id\":\"call_42\",\"args\":{\"city\":\"Paris\"}}]}}", &call));
    TEST_ASSERT_EQUAL_INT(JR_GEV_TOOL_CALL, first_kind(&call));
    TEST_ASSERT_EQUAL_STRING("get_weather", call.events[0].tool_name);
    TEST_ASSERT_EQUAL_STRING("call_42", call.events[0].call_id_text);
    TEST_ASSERT_NOT_NULL(strstr(call.events[0].tool_args, "Paris"));
    uint32_t id = call.events[0].call_id;
    TEST_ASSERT_NOT_EQUAL(0, id);
    jr_gemini_parse_free(&call);

    jr_gemini_parse_t cancel;
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"toolCallCancellation\":{\"ids\":[\"call_42\"]}}", &cancel));
    TEST_ASSERT_EQUAL_INT(JR_GEV_TOOL_CANCEL, first_kind(&cancel));
    TEST_ASSERT_EQUAL_UINT32(id, cancel.events[0].call_id);      /* matches the call */
    TEST_ASSERT_EQUAL_STRING("call_42", cancel.events[0].call_id_text);
    jr_gemini_parse_free(&cancel);
}

/* error frames classify a kind; quota is detected by code AND by message. */
static void test_parse_error_quota_detection(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"error\":{\"code\":429,\"message\":\"Too many requests\"}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_ERROR, first_kind(&p));
    TEST_ASSERT_EQUAL_INT(JR_GEMINI_ERRK_QUOTA, p.events[0].error_kind);
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"error\":{\"message\":\"RESOURCE_EXHAUSTED: quota\"}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEMINI_ERRK_QUOTA, p.events[0].error_kind);
    jr_gemini_parse_free(&p);

    TEST_ASSERT_TRUE(jr_gemini_parse(
        "{\"error\":{\"code\":503,\"message\":\"backend unavailable\"}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEMINI_ERRK_TRANSIENT, p.events[0].error_kind);
    jr_gemini_parse_free(&p);
}

/* An unknown top-level frame is a valid parse with one UNKNOWN event; invalid
 * JSON is a parse failure (never a crash). */
static void test_parse_unknown_and_invalid(void)
{
    jr_gemini_parse_t p;
    TEST_ASSERT_TRUE(jr_gemini_parse("{\"somethingNew\":{}}", &p));
    TEST_ASSERT_EQUAL_INT(JR_GEV_UNKNOWN, first_kind(&p));
    jr_gemini_parse_free(&p);

    jr_gemini_parse_t bad;
    TEST_ASSERT_FALSE(jr_gemini_parse("{not valid json", &bad));
    jr_gemini_parse_free(&bad);   /* safe on a failed parse */
}

/* ===================================================================== *
 *  PUMP + independent liveness (split-brain)                            *
 * ===================================================================== */

/* The pump: recv -> parse -> emit both the rich and the neutral callbacks. */
static int g_rich_audio, g_neutral_audio;
static void rich_sink(void *u, const jr_gemini_event_t *e)
{
    (void)u;
    if (e->kind == JR_GEV_AUDIO_CHUNK) { g_rich_audio++; }
}
static void neutral_sink(void *u, const jr_rvc_event_t *e)
{
    (void)u;
    if (e->kind == JR_RVC_EV_AUDIO_CHUNK) { g_neutral_audio++; }
}
static void test_pump_emits_rich_and_neutral(void)
{
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN;
    fake_ws_push_inbox(&f,
        "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":"
        "{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AAAAAAAA\"}}]}}}");
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), fake_clock_make(), &cfg);

    g_rich_audio = g_neutral_audio = 0;
    jr_gemini_client_set_event_cb(&c, rich_sink, NULL);
    jr_realtime_voice_client_t rvc = jr_gemini_client_as_rvc(&c);
    rvc.set_event_cb(rvc.ctx, neutral_sink, NULL);

    TEST_ASSERT_TRUE(jr_gemini_pump_rx(&c));
    TEST_ASSERT_EQUAL_INT(1, g_rich_audio);
    TEST_ASSERT_EQUAL_INT(1, g_neutral_audio);
    TEST_ASSERT_FALSE(jr_gemini_pump_rx(&c));   /* inbox drained */
}

/* SPLIT-BRAIN: a dead uplink (soft-fails) while the downlink is alive
 * (heartbeats). The DeadUplinkMonitor fires at 25; the KeepaliveMonitor does NOT
 * (its clock is re-armed by every inbound frame) — the two liveness signals are
 * independent, so the split-brain is observable. */
static void test_split_brain_uplink_dead_downlink_alive(void)
{
    fake_clock_reset();
    jr_clock_t clk = fake_clock_make();
    fake_ws_t f; fake_ws_init(&f); f.state = JR_WS_OPEN; f.send_mode = FWS_SOFT_FAIL;
    jr_gemini_config_t cfg = manual_cfg();
    jr_gemini_client_t c;
    jr_gemini_client_init(&c, fake_ws_make(&f), clk, &cfg);

    jr_keepalive_monitor_t ka; memset(&ka, 0, sizeof ka);
    jr_keepalive_arm(&ka, jr_clock_now_ms(&clk), JR_LIVE_DEADLINE_MS);
    jr_dead_uplink_monitor_t du; memset(&du, 0, sizeof du);
    jr_dead_uplink_arm(&du, JR_TX_FAIL_RESUME);
    jr_gemini_client_set_monitors(&c, &ka, &du);

    for (unsigned i = 0; i < JR_TX_FAIL_RESUME; ++i) {   /* 25 */
        fake_ws_push_inbox(&f,
            "{\"sessionResumptionUpdate\":{\"newHandle\":\"h\",\"resumable\":true}}");
        TEST_ASSERT_EQUAL_INT(JR_ERR_WOULD_BLOCK,               /* soft-fail != death */
                              jr_gemini_send_frame(&c, "mic", 3));
        jr_gemini_pump_rx(&c);                                  /* downlink heartbeat */
    }

    TEST_ASSERT_EQUAL_UINT32(25, c.live.consecutive_tx_failures);
    TEST_ASSERT_TRUE(jr_dead_uplink_poll(&du).fired);          /* uplink dead */
    TEST_ASSERT_FALSE(jr_keepalive_poll(&ka, jr_clock_now_ms(&clk)).fired); /* downlink alive */
    TEST_ASSERT_EQUAL_UINT(0, f.close_calls);                  /* never torn down here */
}

/* ---- registration (called from test_core.c main, inside UNITY_BEGIN/END) ---- */
void transport_tests_run(void)
{
    /* builders */
    RUN_TEST(test_setup_model_pinned);
    RUN_TEST(test_setup_thinkingLevel_nested_not_flat);
    RUN_TEST(test_setup_manual_ptt_config);
    RUN_TEST(test_setup_auto_vad_config);
    RUN_TEST(test_setup_tools_coexist);
    RUN_TEST(test_setup_ask_user_array_schema_golden);
    RUN_TEST(test_setup_legacy_single_string_decl_byte_identical);
    RUN_TEST(test_build_control_frames);
    RUN_TEST(test_build_audio_chunk_roundtrip);
    RUN_TEST(test_build_tool_response_preserves_original_id);
    RUN_TEST(test_build_ask_user_response_for_chosen_option);
    RUN_TEST(test_setup_refuses_overlong_declaration);
    /* ask snapshot — the 120 s lifetime contract */
    RUN_TEST(test_ask_snapshot_outlives_the_parse_tree);
    RUN_TEST(test_ask_snapshot_survives_pump_rx_free);
    RUN_TEST(test_ask_snapshot_rejects_and_flags_truncation);
    /* framer */
    RUN_TEST(test_framer_wouldblock_never_aborts);
    RUN_TEST(test_framer_drop_newest_oldest_retained);
    RUN_TEST(test_framer_partial_then_flush_completes);
    RUN_TEST(test_framer_ok_passthrough);
    RUN_TEST(test_framer_real_close_is_death);
    RUN_TEST(test_framer_session_reset_drops_stale_tx);
    /* rvc control guarantee + connect */
    RUN_TEST(test_manual_never_emits_audioStreamEnd);
    RUN_TEST(test_connect_sends_setup);
    /* parser */
    RUN_TEST(test_parse_setupComplete);
    RUN_TEST(test_parse_audio_and_turnComplete);
    RUN_TEST(test_parse_mediaChunks_array_and_object);
    RUN_TEST(test_parse_interrupted_without_generationComplete);
    RUN_TEST(test_parse_goAway_duration);
    RUN_TEST(test_parse_sessionResumption_resumable_gate);
    RUN_TEST(test_parse_toolCall_and_cancellation);
    RUN_TEST(test_parse_toolCall_ask_user_options);
    RUN_TEST(test_parse_error_quota_detection);
    RUN_TEST(test_parse_unknown_and_invalid);
    /* pump + liveness */
    RUN_TEST(test_pump_emits_rich_and_neutral);
    RUN_TEST(test_split_brain_uplink_dead_downlink_alive);
}
