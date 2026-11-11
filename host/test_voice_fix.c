/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_voice_fix.c - the policies behind the 2026-11-10 voice session:
 * the unanswered-utterance watch, the gateway/tool policy, and the setup
 * message that policy shapes. All pure; no clock, no socket, no device.
 */
#include "unity.h"
#include "jr_core/utt_watch.h"
#include "jr_core/echo_gate.h"
#include "jr_core/tool_policy.h"
#include "jr_transport/gemini_live.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------ utt_watch -------------------------------- */

/* One utterance of `len_ms`, ending at `end_ms`, heard by the local VAD while
 * the phase was Listening. */
static void speak(jr_utt_watch_t *w, uint32_t end_ms, uint32_t len_ms)
{
    jr_utt_speech_start(w, end_ms - len_ms);
    jr_utt_speech_end(w, end_ms, true, false);
}

static void test_utt_short_utterance_does_not_arm(void)
{
    jr_utt_watch_t w;
    jr_utt_init(&w);
    speak(&w, 10000u, 600u);
    TEST_ASSERT_FALSE(w.armed);
    TEST_ASSERT_EQUAL_INT(JR_UTT_NONE, jr_utt_poll(&w, 60000u));
}

static void test_utt_silence_after_a_real_utterance_is_a_pardon(void)
{
    jr_utt_watch_t w;
    jr_utt_init(&w);
    speak(&w, 10000u, 900u);
    TEST_ASSERT_TRUE(w.armed);
    TEST_ASSERT_EQUAL_INT(JR_UTT_NONE, jr_utt_poll(&w, 10000u + JR_UTT_REPLY_WAIT_MS - 1u));
    TEST_ASSERT_EQUAL_INT(JR_UTT_PARDON, jr_utt_poll(&w, 10000u + JR_UTT_REPLY_WAIT_MS));
    /* Once per miss. */
    TEST_ASSERT_EQUAL_INT(JR_UTT_NONE, jr_utt_poll(&w, 10000u + JR_UTT_REPLY_WAIT_MS + 1u));
}

static void test_utt_two_misses_in_a_row_end_the_session(void)
{
    jr_utt_watch_t w;
    jr_utt_init(&w);
    speak(&w, 10000u, 900u);
    TEST_ASSERT_EQUAL_INT(JR_UTT_PARDON, jr_utt_poll(&w, 20000u));
    speak(&w, 30000u, 900u);
    TEST_ASSERT_EQUAL_INT(JR_UTT_RECONNECT, jr_utt_poll(&w, 40000u));
    /* And the count starts over after a reconnect. */
    speak(&w, 50000u, 900u);
    TEST_ASSERT_EQUAL_INT(JR_UTT_PARDON, jr_utt_poll(&w, 60000u));
}

/* THE LOGGED DEFECT. A miss, seven answered utterances, a second miss minutes
 * later: that is not "two in a row", and it must not cost the conversation. */
static void test_utt_a_miss_then_answers_then_a_miss_is_not_in_a_row(void)
{
    jr_utt_watch_t w;
    jr_utt_init(&w);
    speak(&w, 8000u, 940u);
    TEST_ASSERT_EQUAL_INT(JR_UTT_PARDON, jr_utt_poll(&w, 15000u));
    TEST_ASSERT_EQUAL_UINT8(1u, w.misses);
    for (int i = 0; i < 7; ++i) {
        const uint32_t t = 20000u + (uint32_t)i * 10000u;
        speak(&w, t, 2000u);
        jr_utt_answered(&w);                 /* the server replied */
        TEST_ASSERT_EQUAL_INT(JR_UTT_NONE, jr_utt_poll(&w, t + 9000u));
    }
    TEST_ASSERT_EQUAL_UINT8(0u, w.misses);
    speak(&w, 120000u, 880u);
    TEST_ASSERT_EQUAL_INT(JR_UTT_PARDON, jr_utt_poll(&w, 127000u));
    TEST_ASSERT_EQUAL_UINT32(2u, w.misses_total);
    TEST_ASSERT_EQUAL_UINT32(7u, w.answered_total);
}

/* The model answering our own "Pardon, Sir?" turn proves it hears text, not
 * the owner; if that cleared the run, a server deaf to audio would be asked
 * "Pardon" for ever and never replaced. */
static void test_utt_the_reply_to_our_own_pardon_is_not_an_answer(void)
{
    jr_utt_watch_t w;
    jr_utt_init(&w);
    speak(&w, 10000u, 900u);
    TEST_ASSERT_EQUAL_INT(JR_UTT_PARDON, jr_utt_poll(&w, 20000u));
    jr_utt_answered(&w);                     /* "Pardon, Sir?" comes back */
    TEST_ASSERT_EQUAL_UINT8(1u, w.misses);
    speak(&w, 30000u, 900u);                 /* the owner says it again */
    TEST_ASSERT_EQUAL_INT(JR_UTT_RECONNECT, jr_utt_poll(&w, 40000u));
}

static void test_utt_the_owner_answered_after_a_pardon_clears_the_run(void)
{
    jr_utt_watch_t w;
    jr_utt_init(&w);
    speak(&w, 10000u, 900u);
    TEST_ASSERT_EQUAL_INT(JR_UTT_PARDON, jr_utt_poll(&w, 20000u));
    jr_utt_answered(&w);                     /* the pardon's own reply */
    speak(&w, 30000u, 2000u);                /* the owner repeats it */
    jr_utt_answered(&w);                     /* and now it answers */
    TEST_ASSERT_EQUAL_UINT8(0u, w.misses);
    speak(&w, 50000u, 900u);
    TEST_ASSERT_EQUAL_INT(JR_UTT_PARDON, jr_utt_poll(&w, 60000u));
}

static void test_utt_a_bare_frame_cancels_the_clock_but_is_not_an_answer(void)
{
    jr_utt_watch_t w;
    jr_utt_init(&w);
    speak(&w, 10000u, 900u);
    TEST_ASSERT_EQUAL_INT(JR_UTT_PARDON, jr_utt_poll(&w, 20000u));
    speak(&w, 30000u, 900u);
    jr_utt_server_alive(&w);                 /* e.g. a go-away or an error frame */
    TEST_ASSERT_EQUAL_INT(JR_UTT_NONE, jr_utt_poll(&w, 50000u));
    TEST_ASSERT_EQUAL_UINT8(1u, w.misses);   /* the run is still one miss deep */
}

static void test_utt_not_armed_in_other_phases_or_with_a_text_turn_queued(void)
{
    jr_utt_watch_t w;
    jr_utt_init(&w);
    jr_utt_speech_start(&w, 1000u);
    jr_utt_speech_end(&w, 3000u, false, false);   /* not Listening */
    TEST_ASSERT_FALSE(w.armed);
    jr_utt_speech_end(&w, 3000u, true, true);     /* a text turn is its own wait */
    TEST_ASSERT_FALSE(w.armed);
    jr_utt_speech_end(&w, 3000u, true, false);
    TEST_ASSERT_TRUE(w.armed);
}


/* THE LOGGED DEFECT (2026-11-10): "answered":62 after four utterances. A reply
 * is dozens of reply-bearing frames; the count is turns. */
static void test_utt_answered_counts_turns_not_frames(void)
{
    jr_utt_watch_t w;
    jr_utt_init(&w);
    for (int turn = 0; turn < 4; ++turn) {
        speak(&w, 10000u + (uint32_t)turn * 20000u, 2000u);
        for (int frame = 0; frame < 15; ++frame) {
            jr_utt_answered(&w);             /* audio chunks, text, turnComplete */
        }
    }
    TEST_ASSERT_EQUAL_UINT32(4u, w.answered_total);
    TEST_ASSERT_EQUAL_UINT8(0u, w.misses);
}

/* Counting turns must not change when a miss is cleared: every answering frame
 * still clears the run, not only the first. */
static void test_utt_every_answering_frame_still_clears_the_run(void)
{
    jr_utt_watch_t w;
    jr_utt_init(&w);
    speak(&w, 10000u, 900u);
    TEST_ASSERT_EQUAL_INT(JR_UTT_PARDON, jr_utt_poll(&w, 20000u));
    TEST_ASSERT_EQUAL_UINT8(1u, w.misses);
    speak(&w, 30000u, 2000u);
    jr_utt_answered(&w);                     /* the turn is counted here */
    w.misses = 1u;                           /* a later frame of the same turn... */
    jr_utt_answered(&w);                     /* ...still clears the run */
    TEST_ASSERT_EQUAL_UINT8(0u, w.misses);
    TEST_ASSERT_EQUAL_UINT32(1u, w.answered_total);
}

/* ------------------------------ tool_policy ------------------------------ */

static void test_tools_gateway_classification(void)
{
    static const char *const kGateway[] = {
        "recall_memory", "remember", "search_tools", "execute_tool",
        "delegate_task", "delegated_tasks",
    };
    static const char *const kLocal[] = {
        "current_time", "weather_here", "set_volume", "set_brightness",
        "ask_user",
    };
    for (size_t i = 0; i < sizeof kGateway / sizeof kGateway[0]; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(jr_tool_needs_gateway(kGateway[i]), kGateway[i]);
    }
    for (size_t i = 0; i < sizeof kLocal / sizeof kLocal[0]; ++i) {
        TEST_ASSERT_FALSE_MESSAGE(jr_tool_needs_gateway(kLocal[i]), kLocal[i]);
    }
    /* A tool nobody classified is not offered without a gateway. */
    TEST_ASSERT_TRUE(jr_tool_needs_gateway("brand_new_tool"));
    TEST_ASSERT_TRUE(jr_tool_needs_gateway(NULL));
}

static void test_gateway_is_unreachable_after_three_dead_calls(void)
{
    jr_gateway_t g;
    jr_gateway_init(&g);
    TEST_ASSERT_TRUE(jr_gateway_usable(&g, true));
    TEST_ASSERT_FALSE(jr_gateway_usable(&g, false));     /* no key stored */
    jr_gateway_note(&g, false);
    jr_gateway_note(&g, false);
    TEST_ASSERT_TRUE(jr_gateway_usable(&g, true));
    TEST_ASSERT_EQUAL_UINT32(JR_GW_POLL_MS_NORMAL, jr_gateway_poll_ms(&g));
    jr_gateway_note(&g, false);
    TEST_ASSERT_FALSE(jr_gateway_usable(&g, true));
    TEST_ASSERT_EQUAL_UINT32(JR_GW_POLL_MS_DEGRADED, jr_gateway_poll_ms(&g));
    /* One call that reached a server brings it back. */
    jr_gateway_note(&g, true);
    TEST_ASSERT_TRUE(jr_gateway_usable(&g, true));
    TEST_ASSERT_EQUAL_UINT32(JR_GW_POLL_MS_NORMAL, jr_gateway_poll_ms(&g));
}

static void test_gateway_says_why_not(void)
{
    jr_gateway_t g;
    jr_gateway_init(&g);
    TEST_ASSERT_EQUAL_STRING("ok", jr_gateway_why(&g, true, true));
    TEST_ASSERT_EQUAL_STRING("worker_not_ready", jr_gateway_why(&g, true, false));
    TEST_ASSERT_EQUAL_STRING("gateway_not_configured", jr_gateway_why(&g, false, true));
    for (int i = 0; i < (int)JR_GW_FAIL_LIMIT; ++i) jr_gateway_note(&g, false);
    TEST_ASSERT_EQUAL_STRING("gateway_unreachable", jr_gateway_why(&g, true, true));
    /* Not configured outranks unreachable: there is nothing to reach. */
    TEST_ASSERT_EQUAL_STRING("gateway_not_configured", jr_gateway_why(&g, false, true));
}

static void test_instruction_without_a_gateway_names_what_it_has(void)
{
    char buf[2048];
    const size_t n = jr_tool_instruction(false, "Testville", buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_UINT(100u, (unsigned)n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "not connected"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "current_time"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "weather_here"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Testville"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "never ask Sir where he is"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_volume"));
    /* No gateway tool may be named as one it has. */
    TEST_ASSERT_NULL(strstr(buf, "delegate_task"));
    TEST_ASSERT_NULL(strstr(buf, "delegated_tasks"));
    TEST_ASSERT_NULL(strstr(buf, "execute_tool"));
    TEST_ASSERT_NULL(strstr(buf, "search_tools"));
    TEST_ASSERT_NULL(strstr(buf, "hands elsewhere"));
    TEST_ASSERT_NULL(strstr(buf, "Capabilities"));
}

static void test_instruction_with_a_gateway_is_the_old_one_plus_weather(void)
{
    char buf[2048];
    const size_t n = jr_tool_instruction(true, "Testville", buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_UINT(100u, (unsigned)n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "delegate_task"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "hands elsewhere"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "weather_here"));
    TEST_ASSERT_NULL(strstr(buf, "not connected"));
    /* The place name is a no-gateway detail; the keyed text does not need it. */
    TEST_ASSERT_NULL(strstr(buf, "Testville"));
}

static void test_instruction_that_does_not_fit_returns_zero(void)
{
    char buf[40];
    TEST_ASSERT_EQUAL_UINT(0u, jr_tool_instruction(false, "X", buf, sizeof buf));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
    TEST_ASSERT_EQUAL_UINT(0u, jr_tool_instruction(true, "X", buf, sizeof buf));
}

/* ---------------------- setup built with and without a key --------------- */

/* The device's eleven tools, by name, in catalog order (main/main.c). */
static const jr_gemini_fn_decl_t kCatalog[] = {
    { .name = "recall_memory", .description = "d", .arg_name = "query", .arg_desc = "q" },
    { .name = "remember", .description = "d", .arg_name = "note", .arg_desc = "n" },
    { .name = "current_time", .description = "d" },
    { .name = "weather_here", .description = "d" },
    { .name = "search_tools", .description = "d", .arg_name = "query", .arg_desc = "q" },
    { .name = "execute_tool", .description = "d" },
    { .name = "set_volume", .description = "d" },
    { .name = "set_brightness", .description = "d" },
    { .name = "delegate_task", .description = "d", .arg_name = "goal", .arg_desc = "g" },
    { .name = "delegated_tasks", .description = "d" },
    { .name = "ask_user", .description = "d" },
};
#define CATALOG_N (sizeof kCatalog / sizeof kCatalog[0])

/* Names declared in a setup message, comma-joined, in order. */
static void declared_names(const char *setup_json, char *out, size_t cap)
{
    out[0] = '\0';
    cJSON *root = cJSON_Parse(setup_json);
    TEST_ASSERT_NOT_NULL(root);
    const cJSON *tools = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "setup"), "tools");
    const cJSON *t = NULL;
    cJSON_ArrayForEach(t, tools) {
        const cJSON *decls = cJSON_GetObjectItemCaseSensitive(t, "functionDeclarations");
        const cJSON *d = NULL;
        cJSON_ArrayForEach(d, decls) {
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(d, "name");
            if (out[0] != '\0') { strncat(out, ",", cap - strlen(out) - 1u); }
            strncat(out, nm->valuestring, cap - strlen(out) - 1u);
        }
    }
    cJSON_Delete(root);
}

static void test_setup_with_a_usable_gateway_declares_every_tool(void)
{
    jr_gemini_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fns = kCatalog;
    cfg.fn_count = CATALOG_N;
    cfg.no_gateway = false;
    TEST_ASSERT_EQUAL_UINT(CATALOG_N, jr_gemini_declared_fn_count(&cfg));
    char *setup = jr_gemini_build_setup(&cfg);
    TEST_ASSERT_NOT_NULL(setup);
    char names[512];
    declared_names(setup, names, sizeof names);
    free(setup);
    TEST_ASSERT_EQUAL_STRING(
        "recall_memory,remember,current_time,weather_here,search_tools,"
        "execute_tool,set_volume,set_brightness,delegate_task,"
        "delegated_tasks,ask_user", names);
}

static void test_setup_without_a_gateway_declares_only_what_runs_here(void)
{
    jr_gemini_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fns = kCatalog;
    cfg.fn_count = CATALOG_N;
    cfg.no_gateway = true;
    TEST_ASSERT_EQUAL_UINT(5u, jr_gemini_declared_fn_count(&cfg));
    char *setup = jr_gemini_build_setup(&cfg);
    TEST_ASSERT_NOT_NULL(setup);
    char names[512];
    declared_names(setup, names, sizeof names);
    free(setup);
    TEST_ASSERT_EQUAL_STRING(
        "current_time,weather_here,set_volume,set_brightness,ask_user", names);
}

static void test_setup_with_no_local_tools_left_has_no_declarations(void)
{
    static const jr_gemini_fn_decl_t only_gateway[] = {
        { .name = "execute_tool", .description = "d" },
    };
    jr_gemini_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fns = only_gateway;
    cfg.fn_count = 1u;
    cfg.no_gateway = true;
    TEST_ASSERT_EQUAL_UINT(0u, jr_gemini_declared_fn_count(&cfg));
    char *setup = jr_gemini_build_setup(&cfg);
    TEST_ASSERT_NOT_NULL(setup);
    TEST_ASSERT_NULL(strstr(setup, "functionDeclarations"));
    free(setup);
}


/* ------------------------------ echo_gate -------------------------------- */

/* Frame cadence of the uplink loop: one 32 ms capture frame per step. */
#define FRAME_MS 32u

static void test_echo_gate_silences_while_audible_and_for_the_tail(void)
{
    jr_echo_gate_t g;
    jr_echo_gate_init(&g);
    uint32_t t = 1000u;
    /* Quiet room: the microphone goes up. */
    TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, t, false, true));
    /* The model speaks for 10 frames. */
    for (int i = 0; i < 10; ++i) {
        t += FRAME_MS;
        TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, t, true, true));
    }
    const uint32_t last_audible = t;
    /* After it stops: silence for exactly JR_ECHO_TAIL_MS, then the microphone. */
    t = last_audible + JR_ECHO_TAIL_MS - 1u;
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, t, false, true));
    t = last_audible + JR_ECHO_TAIL_MS;
    TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, t, false, true));
    TEST_ASSERT_EQUAL_UINT32(11u, g.gated_frames);
    TEST_ASSERT_EQUAL_UINT32(2u, g.passed_frames);
}

/* The first words after a reply: the tail is bounded, so a sentence that starts
 * 200 ms after the last audible sample is heard whole. */
static void test_echo_gate_owner_speaking_200ms_after_the_reply_is_not_lost(void)
{
    jr_echo_gate_t g;
    jr_echo_gate_init(&g);
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, 5000u, true, true));
    for (uint32_t t = 5000u + FRAME_MS; t < 5000u + 200u; t += FRAME_MS) {
        (void)jr_echo_gate_silence(&g, t, false, true);
    }
    uint32_t passed_before = g.passed_frames;
    for (uint32_t t = 5000u + 200u; t < 5000u + 400u; t += FRAME_MS) {
        TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, t, false, true));
    }
    TEST_ASSERT_TRUE(g.passed_frames > passed_before);
}

static void test_echo_gate_off_switch_passes_everything_and_counts_nothing_gated(void)
{
    jr_echo_gate_t g;
    jr_echo_gate_init(&g);
    for (uint32_t t = 0u; t < 1000u; t += FRAME_MS) {
        TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, t, true, false));
    }
    TEST_ASSERT_EQUAL_UINT32(0u, g.gated_frames);
    /* Switched on mid-reply: gated at once, and the tail honoured after. */
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, 1000u, true, true));
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, 1000u + JR_ECHO_TAIL_MS - 1u, false, true));
    TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, 1000u + JR_ECHO_TAIL_MS, false, true));
}

static void test_echo_gate_a_quiet_room_is_never_gated_and_the_clock_wraps(void)
{
    jr_echo_gate_t g;
    jr_echo_gate_init(&g);
    for (uint32_t t = 0u; t < 2000u; t += FRAME_MS) {
        TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, t, false, true));
    }
    /* uint32 wrap: audible just before the wrap, tail straddles it. */
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, 0xFFFFFFF0u, true, true));
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, 0x00000020u, false, true));
    TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, 0x00000100u, false, true));
}

static void test_reply_cut_drops_until_the_server_boundary(void)
{
    jr_reply_cut_t c;
    jr_reply_cut_init(&c);
    TEST_ASSERT_FALSE(jr_reply_cut_drop_audio(&c, 100u));      /* no cut: plays */
    jr_reply_cut_begin(&c, 1000u);
    TEST_ASSERT_TRUE(jr_reply_cut_drop_audio(&c, 1100u));
    TEST_ASSERT_TRUE(jr_reply_cut_drop_audio(&c, 1600u));
    jr_reply_cut_boundary(&c);                                  /* turnComplete */
    TEST_ASSERT_FALSE(jr_reply_cut_drop_audio(&c, 1700u));      /* next reply plays */
    TEST_ASSERT_EQUAL_UINT32(2u, c.dropped_chunks);
    TEST_ASSERT_EQUAL_UINT32(1u, c.cuts);
}

/* A lost boundary must not swallow the next reply for ever. */
static void test_reply_cut_expires_when_no_chunk_arrives_for_a_while(void)
{
    jr_reply_cut_t c;
    jr_reply_cut_init(&c);
    jr_reply_cut_begin(&c, 1000u);
    TEST_ASSERT_TRUE(jr_reply_cut_drop_audio(&c, 1500u));
    TEST_ASSERT_FALSE(jr_reply_cut_drop_audio(&c, 1500u + JR_REPLY_CUT_GAP_MS + 1u));
    TEST_ASSERT_FALSE(c.cutting);
    TEST_ASSERT_FALSE(jr_reply_cut_drop_audio(&c, 1500u + JR_REPLY_CUT_GAP_MS + 100u));
}

/* Chunks arriving steadily keep the cut alive however long the reply runs. */
static void test_reply_cut_stays_while_chunks_keep_coming(void)
{
    jr_reply_cut_t c;
    jr_reply_cut_init(&c);
    jr_reply_cut_begin(&c, 0u);
    for (uint32_t t = 500u; t < 30000u; t += 500u) {
        TEST_ASSERT_TRUE(jr_reply_cut_drop_audio(&c, t));
    }
}

/* ---- registration (called from test_core.c main, inside UNITY_BEGIN/END) ---- */
void voice_fix_tests_run(void)
{
    RUN_TEST(test_utt_short_utterance_does_not_arm);
    RUN_TEST(test_utt_silence_after_a_real_utterance_is_a_pardon);
    RUN_TEST(test_utt_two_misses_in_a_row_end_the_session);
    RUN_TEST(test_utt_a_miss_then_answers_then_a_miss_is_not_in_a_row);
    RUN_TEST(test_utt_the_reply_to_our_own_pardon_is_not_an_answer);
    RUN_TEST(test_utt_the_owner_answered_after_a_pardon_clears_the_run);
    RUN_TEST(test_utt_a_bare_frame_cancels_the_clock_but_is_not_an_answer);
    RUN_TEST(test_utt_not_armed_in_other_phases_or_with_a_text_turn_queued);
    RUN_TEST(test_utt_answered_counts_turns_not_frames);
    RUN_TEST(test_utt_every_answering_frame_still_clears_the_run);
    RUN_TEST(test_echo_gate_silences_while_audible_and_for_the_tail);
    RUN_TEST(test_echo_gate_owner_speaking_200ms_after_the_reply_is_not_lost);
    RUN_TEST(test_echo_gate_off_switch_passes_everything_and_counts_nothing_gated);
    RUN_TEST(test_echo_gate_a_quiet_room_is_never_gated_and_the_clock_wraps);
    RUN_TEST(test_reply_cut_drops_until_the_server_boundary);
    RUN_TEST(test_reply_cut_expires_when_no_chunk_arrives_for_a_while);
    RUN_TEST(test_reply_cut_stays_while_chunks_keep_coming);
    RUN_TEST(test_tools_gateway_classification);
    RUN_TEST(test_gateway_is_unreachable_after_three_dead_calls);
    RUN_TEST(test_gateway_says_why_not);
    RUN_TEST(test_instruction_without_a_gateway_names_what_it_has);
    RUN_TEST(test_instruction_with_a_gateway_is_the_old_one_plus_weather);
    RUN_TEST(test_instruction_that_does_not_fit_returns_zero);
    RUN_TEST(test_setup_with_a_usable_gateway_declares_every_tool);
    RUN_TEST(test_setup_without_a_gateway_declares_only_what_runs_here);
    RUN_TEST(test_setup_with_no_local_tools_left_has_no_declarations);
}
