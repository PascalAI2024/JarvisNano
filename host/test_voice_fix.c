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
        "ask_user", "set_timer", "cancel_timer", "list_timers",
        "set_glass", "device_status", "start_focus", "stopwatch",
        "add_note", "list_notes", "clear_notes", "daily_briefing",
        "set_ui_sounds", "set_sunlight",
        "set_desk_mode", "start_breathe", "complete_note",
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
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_timer"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "cancel_timer"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "list_timers"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "start_focus"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "stopwatch"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "add_note"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "list_notes"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "clear_notes"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "daily_briefing"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_glass"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_ui_sounds"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_sunlight"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_desk_mode"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "start_breathe"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "complete_note"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "device_status"));
    /* Fits inside main.c's 1600-byte buffer. */
    char device[1600];
    TEST_ASSERT_GREATER_THAN_UINT(100u, (unsigned)jr_tool_instruction(false, "Fort Lauderdale", device, sizeof device));
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
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_timer"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "start_focus"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "stopwatch"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "add_note"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "daily_briefing"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_ui_sounds"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_sunlight"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_desk_mode"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "start_breathe"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "complete_note"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "set_glass"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "device_status"));
    /* The board is delegated_tasks; execute_tool is for a method search_tools
     * found. The model once reached for execute_tool to read the board. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "alone answers anything about the board"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "execute_tool is for a method search_tools found, never for the board"));
    TEST_ASSERT_NULL(strstr(buf, "not connected"));
    /* main.c keeps this text in a 1600-byte buffer: it must still fit. */
    char device[1600];
    TEST_ASSERT_GREATER_THAN_UINT(100u, (unsigned)jr_tool_instruction(true, "X", device, sizeof device));
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

/* The device's twenty-seven tools, by name, in catalog order (main/main.c). */
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
    { .name = "set_timer", .description = "d" },
    { .name = "cancel_timer", .description = "d" },
    { .name = "list_timers", .description = "d" },
    { .name = "set_glass", .description = "d" },
    { .name = "device_status", .description = "d" },
    { .name = "start_focus", .description = "d" },
    { .name = "stopwatch", .description = "d" },
    { .name = "add_note", .description = "d" },
    { .name = "list_notes", .description = "d" },
    { .name = "clear_notes", .description = "d" },
    { .name = "daily_briefing", .description = "d" },
    { .name = "set_ui_sounds", .description = "d" },
    { .name = "set_sunlight", .description = "d" },
    { .name = "set_desk_mode", .description = "d" },
    { .name = "start_breathe", .description = "d" },
    { .name = "complete_note", .description = "d" },
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
        "delegated_tasks,ask_user,set_timer,cancel_timer,list_timers,"
        "set_glass,device_status,start_focus,stopwatch,add_note,list_notes,"
        "clear_notes,daily_briefing,set_ui_sounds,set_sunlight,"
        "set_desk_mode,start_breathe,complete_note", names);
}

static void test_setup_without_a_gateway_declares_only_what_runs_here(void)
{
    jr_gemini_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fns = kCatalog;
    cfg.fn_count = CATALOG_N;
    cfg.no_gateway = true;
    TEST_ASSERT_EQUAL_UINT(21u, jr_gemini_declared_fn_count(&cfg));
    char *setup = jr_gemini_build_setup(&cfg);
    TEST_ASSERT_NOT_NULL(setup);
    char names[512];
    declared_names(setup, names, sizeof names);
    free(setup);
    TEST_ASSERT_EQUAL_STRING(
        "current_time,weather_here,set_volume,set_brightness,ask_user,"
        "set_timer,cancel_timer,list_timers,set_glass,device_status,"
        "start_focus,stopwatch,add_note,list_notes,clear_notes,"
        "daily_briefing,set_ui_sounds,set_sunlight,"
        "set_desk_mode,start_breathe,complete_note", names);
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
/* ---- volume threshold, wake pass window, make-up gain (echo-fix) ---------- */

static void test_echo_gate_applies_only_above_the_volume_threshold(void)
{
    /* Default threshold 0: today's behaviour at any audible volume. */
    TEST_ASSERT_TRUE(jr_echo_gate_applies(true, 100, 0));
    TEST_ASSERT_TRUE(jr_echo_gate_applies(true, 1, 0));
    /* Threshold 70: applies strictly above it. */
    TEST_ASSERT_TRUE(jr_echo_gate_applies(true, 71, 70));
    TEST_ASSERT_FALSE(jr_echo_gate_applies(true, 70, 70));
    TEST_ASSERT_FALSE(jr_echo_gate_applies(true, 40, 70));
    /* echogate=0 disables it whatever the volume. */
    TEST_ASSERT_FALSE(jr_echo_gate_applies(false, 100, 0));
    TEST_ASSERT_FALSE(jr_echo_gate_applies(false, 100, 70));
}

static void test_echo_gate_below_the_threshold_the_microphone_goes_up(void)
{
    jr_echo_gate_t g;
    jr_echo_gate_init(&g);
    for (uint32_t t = 0u; t < 640u; t += FRAME_MS) {
        TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, t, true,
                              jr_echo_gate_applies(true, 60, 70)));
    }
    TEST_ASSERT_EQUAL_UINT32(0u, g.gated_frames);
    /* Above it the same audible frames are silenced, tail included. */
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, 1000u, true,
                         jr_echo_gate_applies(true, 100, 70)));
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, 1000u + JR_ECHO_TAIL_MS - 1u, false,
                         jr_echo_gate_applies(true, 100, 70)));
}

static void test_echo_gate_a_wake_pass_opens_it_for_a_bounded_time(void)
{
    jr_echo_gate_t g;
    jr_echo_gate_init(&g);
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, 1000u, true, true));
    jr_echo_gate_pass_for(&g, 1032u, JR_ECHO_PASS_MS);
    /* Whatever is audible, the microphone goes up inside the window ... */
    TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, 1064u, true, true));
    TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, 1032u + JR_ECHO_PASS_MS - 1u, true, true));
    /* ... and the gate is back the moment it closes. */
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, 1032u + JR_ECHO_PASS_MS, true, true));
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, 1032u + JR_ECHO_PASS_MS + FRAME_MS, true, true));
}

static void test_echo_gate_a_wake_pass_survives_the_clock_wrap(void)
{
    jr_echo_gate_t g;
    jr_echo_gate_init(&g);
    uint32_t t = 0xFFFFFFF0u;
    jr_echo_gate_pass_for(&g, t, 100u);
    TEST_ASSERT_FALSE(jr_echo_gate_silence(&g, t + 64u, true, true));    /* wrapped */
    TEST_ASSERT_TRUE(jr_echo_gate_silence(&g, t + 120u, true, true));
}

static void test_uplink_makeup_full_compensation_gives_back_the_listening_level(void)
{
    const uint32_t base = 6u * 256u;
    /* 24 dB listening, 6 dB speaking: 18 dB = 7.943x on top of 6x = 47.66x. */
    uint32_t q8 = jr_uplink_makeup_q8(base, 24, 6, 100);
    TEST_ASSERT_UINT32_WITHIN(20u, (uint32_t)(47.66 * 256.0), q8);
    /* No gain change, or the PGA above listening: the plain make-up. */
    TEST_ASSERT_EQUAL_UINT32(base, jr_uplink_makeup_q8(base, 24, 24, 100));
    TEST_ASSERT_EQUAL_UINT32(base, jr_uplink_makeup_q8(base, 24, 30, 100));
    /* speakgain 0 = none. */
    TEST_ASSERT_EQUAL_UINT32(base, jr_uplink_makeup_q8(base, 24, 6, 0));
    /* 50 percent is half way in linear terms: 6 * (1 + 6.943/2) = 26.83x. */
    TEST_ASSERT_UINT32_WITHIN(20u, (uint32_t)(26.83 * 256.0),
                              jr_uplink_makeup_q8(base, 24, 6, 50));
}

static void test_uplink_makeup_is_clamped_and_monotonic(void)
{
    const uint32_t base = 6u * 256u;
    TEST_ASSERT_EQUAL_UINT32(JR_UPLINK_MAKEUP_MAX_Q8,
                             jr_uplink_makeup_q8(base, 36, 0, 100));
    TEST_ASSERT_EQUAL_UINT32(JR_UPLINK_MAKEUP_MAX_Q8,
                             jr_uplink_makeup_q8(base, 24, 6, 400));
    uint32_t prev = 0u;
    for (int applied = 24; applied >= 0; --applied) {
        uint32_t q8 = jr_uplink_makeup_q8(base, 24, applied, 100);
        TEST_ASSERT_TRUE(q8 >= prev);
        prev = q8;
    }
}

void voice_fix_tests_run(void)
{
    RUN_TEST(test_echo_gate_applies_only_above_the_volume_threshold);
    RUN_TEST(test_echo_gate_below_the_threshold_the_microphone_goes_up);
    RUN_TEST(test_echo_gate_a_wake_pass_opens_it_for_a_bounded_time);
    RUN_TEST(test_echo_gate_a_wake_pass_survives_the_clock_wrap);
    RUN_TEST(test_uplink_makeup_full_compensation_gives_back_the_listening_level);
    RUN_TEST(test_uplink_makeup_is_clamped_and_monotonic);
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
