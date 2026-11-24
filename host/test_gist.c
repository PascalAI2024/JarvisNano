/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * host/test_gist.c — Unity unit tests for jr_core/gist (plain-language tool
 * thinking mapper & 20-second Reply Gist data model). Pure C11, no ESP-IDF.
 */
#include "unity.h"
#include "jr_core/gist.h"
#include "jr_core/tool_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool is_barlow_text_char(char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') {
        return true;
    }
    return strchr("-_.:/%?!+,'()&*", c) != NULL;
}

static void assert_all_barlow_text(const char *s)
{
    TEST_ASSERT_NOT_NULL(s);
    for (size_t i = 0u; s[i] != '\0'; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(is_barlow_text_char(s[i]), s);
    }
}

static void assert_plain_phrase(const char *s, const char *ctx)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(s, ctx);
    TEST_ASSERT_TRUE_MESSAGE(s[0] != '\0', ctx);
    TEST_ASSERT_NULL_MESSAGE(strchr(s, '_'), ctx);
    for (size_t i = 0u; s[i] != '\0'; ++i) {
        const bool ok = (s[i] >= 'A' && s[i] <= 'Z') || s[i] == ' ';
        TEST_ASSERT_TRUE_MESSAGE(ok, ctx);
    }
}

static void test_tool_plain_describe_all_30_tools(void)
{
    typedef struct {
        const char *name;
        const char *verb;
        const char *subject;
        const char *scope;
        bool        is_local;
    } expect_t;

    static const expect_t kAll[] = {
        /* 24 local tools */
        { "current_time",     "CHECKING",     "THE TIME",       "ON DEVICE", true  },
        { "weather_here",     "CHECKING",     "WEATHER",        "ON DEVICE", true  },
        { "set_volume",       "ADJUSTING",    "VOLUME",         "ON DEVICE", true  },
        { "set_brightness",   "ADJUSTING",    "BRIGHTNESS",     "ON DEVICE", true  },
        { "ask_user",         "ASKING",       "YOUR CHOICE",    "ON DEVICE", true  },
        { "set_timer",        "SETTING",      "TIMER",          "ON DEVICE", true  },
        { "cancel_timer",     "CLEARING",     "TIMER",          "ON DEVICE", true  },
        { "list_timers",      "CHECKING",     "TIMERS",         "ON DEVICE", true  },
        { "set_glass",        "SWITCHING",    "DISPLAY",        "ON DEVICE", true  },
        { "device_status",    "CHECKING",     "DEVICE STATUS",  "ON DEVICE", true  },
        { "start_focus",      "STARTING",     "FOCUS SESSION",  "ON DEVICE", true  },
        { "stopwatch",        "UPDATING",     "STOPWATCH",      "ON DEVICE", true  },
        { "add_note",         "SAVING",       "DESK NOTE",      "ON DEVICE", true  },
        { "list_notes",       "CHECKING",     "DESK INBOX",     "ON DEVICE", true  },
        { "complete_note",    "CHECKING OFF", "DESK TASK",      "ON DEVICE", true  },
        { "clear_notes",      "CLEARING",     "DESK INBOX",     "ON DEVICE", true  },
        { "daily_briefing",   "PREPARING",    "BRIEFING",       "ON DEVICE", true  },
        { "set_ui_sounds",    "UPDATING",     "UI SOUNDS",      "ON DEVICE", true  },
        { "set_sunlight",     "ADJUSTING",    "SUNLIGHT BOOST", "ON DEVICE", true  },
        { "set_desk_mode",    "SWITCHING",    "DESK MODE",      "ON DEVICE", true  },
        { "start_breathe",    "STARTING",     "BREATHE",        "ON DEVICE", true  },
        { "set_battery_care", "UPDATING",     "BATTERY CARE",   "ON DEVICE", true  },
        { "show_gist",        "PINNING",      "SUMMARY",        "ON DEVICE", true  },
        { "aquarium",         "UPDATING",     "NANO REEF",      "ON DEVICE", true  },
        /* 6 gateway tools */
        { "search_tools",     "FINDING",      "TOOL",           "GATEWAY",   false },
        { "execute_tool",     "CHECKING",     "THE WEB",        "GATEWAY",   false },
        { "recall_memory",    "CHECKING",     "MEMORY",         "GATEWAY",   false },
        { "remember",         "SAVING",       "TO MEMORY",      "GATEWAY",   false },
        { "delegate_task",    "QUEUING",      "AGENT JOB",      "GATEWAY",   false },
        { "delegated_tasks",  "CHECKING",     "AGENT BOARD",    "GATEWAY",   false },
    };

    const size_t n = sizeof(kAll) / sizeof(kAll[0]);
    TEST_ASSERT_EQUAL_UINT(30u, (unsigned)n);

    unsigned local_count = 0u;
    unsigned gateway_count = 0u;
    for (size_t i = 0u; i < n; ++i) {
        jr_tool_plain_t d;
        memset(&d, 0xAA, sizeof(d));
        jr_tool_plain_describe(kAll[i].name, &d);

        TEST_ASSERT_EQUAL_STRING_MESSAGE(kAll[i].verb, d.verb, kAll[i].name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(kAll[i].subject, d.subject, kAll[i].name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(kAll[i].scope, d.scope, kAll[i].name);
        TEST_ASSERT_EQUAL_MESSAGE(kAll[i].is_local, d.is_local, kAll[i].name);

        /* Exact consistency with tool_policy classification. */
        TEST_ASSERT_EQUAL_MESSAGE(!jr_tool_needs_gateway(kAll[i].name),
                                  d.is_local, kAll[i].name);

        /* Glass 4 rule: plain words only, never a tool name or underscore. */
        assert_plain_phrase(d.verb, kAll[i].name);
        assert_plain_phrase(d.subject, kAll[i].name);
        assert_plain_phrase(d.scope, kAll[i].name);

        if (d.is_local) {
            ++local_count;
        } else {
            ++gateway_count;
        }
    }
    TEST_ASSERT_EQUAL_UINT(24u, local_count);
    TEST_ASSERT_EQUAL_UINT(6u, gateway_count);
}

static void test_tool_plain_describe_fallbacks_and_null(void)
{
    static const char *const kUnknown[] = {
        NULL, "", "unknown_tool", "CURRENT_TIME", "Weather_Here", "show_gist_v2"
    };
    for (size_t i = 0u; i < sizeof(kUnknown) / sizeof(kUnknown[0]); ++i) {
        jr_tool_plain_t d;
        jr_tool_plain_describe(kUnknown[i], &d);
        TEST_ASSERT_EQUAL_STRING("WORKING", d.verb);
        TEST_ASSERT_EQUAL_STRING("REQUEST", d.subject);
        TEST_ASSERT_EQUAL_STRING("GATEWAY", d.scope);
        TEST_ASSERT_FALSE(d.is_local);
        TEST_ASSERT_EQUAL(!jr_tool_needs_gateway(kUnknown[i]), d.is_local);
    }

    /* NULL out pointer is a safe no-op. */
    jr_tool_plain_describe("weather_here", NULL);
    jr_tool_plain_describe(NULL, NULL);
}

static void test_tool_plain_caption_formatting_and_truncation(void)
{
    char buf[64];

    TEST_ASSERT_EQUAL_UINT(28u, (unsigned)jr_tool_plain_caption("weather_here", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("CHECKING WEATHER - ON DEVICE", buf);
    assert_all_barlow_text(buf);

    TEST_ASSERT_EQUAL_UINT(26u, (unsigned)jr_tool_plain_caption("execute_tool", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("CHECKING THE WEB - GATEWAY", buf);
    assert_all_barlow_text(buf);

    TEST_ASSERT_EQUAL_UINT(27u, (unsigned)jr_tool_plain_caption("show_gist", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("PINNING SUMMARY - ON DEVICE", buf);
    assert_all_barlow_text(buf);

    TEST_ASSERT_EQUAL_UINT(30u, (unsigned)jr_tool_plain_caption("aquarium", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("UPDATING NANO REEF - ON DEVICE", buf);
    assert_all_barlow_text(buf);

    TEST_ASSERT_EQUAL_UINT(34u, (unsigned)jr_tool_plain_caption("complete_note", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("CHECKING OFF DESK TASK - ON DEVICE", buf);
    assert_all_barlow_text(buf);

    TEST_ASSERT_EQUAL_UINT(25u, (unsigned)jr_tool_plain_caption("not_a_tool", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("WORKING REQUEST - GATEWAY", buf);

    TEST_ASSERT_EQUAL_UINT(25u, (unsigned)jr_tool_plain_caption(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("WORKING REQUEST - GATEWAY", buf);

    /* Truncation & bounds checks. */
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)jr_tool_plain_caption("weather_here", NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)jr_tool_plain_caption("weather_here", buf, 0u));

    char tiny[1];
    tiny[0] = 'X';
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)jr_tool_plain_caption("weather_here", tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[0]);

    char short_buf[9];
    TEST_ASSERT_EQUAL_UINT(8u, (unsigned)jr_tool_plain_caption("weather_here", short_buf, sizeof(short_buf)));
    TEST_ASSERT_EQUAL_STRING("CHECKING", short_buf);
}

static void test_gist_kind_parse_and_name_roundtrip(void)
{
    static const struct {
        jr_gist_kind_t kind;
        const char    *name;
    } kKinds[] = {
        { JR_GIST_NONE,     "none"     },
        { JR_GIST_NUMBER,   "number"   },
        { JR_GIST_EVENT,    "event"    },
        { JR_GIST_TIMER,    "timer"    },
        { JR_GIST_NOTE,     "note"     },
        { JR_GIST_WEATHER,  "weather"  },
        { JR_GIST_BRIEFING, "briefing" },
        { JR_GIST_ANSWER,   "answer"   },
    };

    for (size_t i = 0u; i < sizeof(kKinds) / sizeof(kKinds[0]); ++i) {
        const char *nm = jr_gist_kind_name(kKinds[i].kind);
        TEST_ASSERT_EQUAL_STRING(kKinds[i].name, nm);
        TEST_ASSERT_EQUAL_INT(kKinds[i].kind, jr_gist_parse_kind(nm));
    }

    /* Case-insensitive & whitespace-trimmed parsing. */
    TEST_ASSERT_EQUAL_INT(JR_GIST_NUMBER,   jr_gist_parse_kind("NUMBER"));
    TEST_ASSERT_EQUAL_INT(JR_GIST_EVENT,    jr_gist_parse_kind("Event"));
    TEST_ASSERT_EQUAL_INT(JR_GIST_TIMER,    jr_gist_parse_kind("  timer  "));
    TEST_ASSERT_EQUAL_INT(JR_GIST_NOTE,     jr_gist_parse_kind("\tNoTe\n"));
    TEST_ASSERT_EQUAL_INT(JR_GIST_WEATHER,  jr_gist_parse_kind("WeAtHeR"));
    TEST_ASSERT_EQUAL_INT(JR_GIST_BRIEFING, jr_gist_parse_kind(" BRIEFING "));
    TEST_ASSERT_EQUAL_INT(JR_GIST_ANSWER,   jr_gist_parse_kind("Answer"));
    TEST_ASSERT_EQUAL_INT(JR_GIST_NONE,     jr_gist_parse_kind("NONE"));

    /* Invalid / unknown inputs fall back to JR_GIST_NONE. */
    TEST_ASSERT_EQUAL_INT(JR_GIST_NONE, jr_gist_parse_kind(NULL));
    TEST_ASSERT_EQUAL_INT(JR_GIST_NONE, jr_gist_parse_kind(""));
    TEST_ASSERT_EQUAL_INT(JR_GIST_NONE, jr_gist_parse_kind("   \t\n  "));
    TEST_ASSERT_EQUAL_INT(JR_GIST_NONE, jr_gist_parse_kind("numbers"));
    TEST_ASSERT_EQUAL_INT(JR_GIST_NONE, jr_gist_parse_kind("unknown"));
    TEST_ASSERT_EQUAL_STRING("none", jr_gist_kind_name((jr_gist_kind_t)99));
}

static void test_gist_sanitize_utf8_punctuation_and_whitespace(void)
{
    char out[64];

    /* Whitespace collapsing, trimming, and safe uppercasing. */
    jr_gist_sanitize(out, sizeof(out), "  \t Hello,\n  world!  \r\n");
    TEST_ASSERT_EQUAL_STRING("HELLO, WORLD!", out);
    assert_all_barlow_text(out);

    /* Supported Barlow punctuation preserved verbatim. */
    jr_gist_sanitize(out, sizeof(out), "+2.1% today (3:30) & a/b_c? 'ok'!");
    TEST_ASSERT_EQUAL_STRING("+2.1% TODAY (3:30) & A/B_C? 'OK'!", out);
    assert_all_barlow_text(out);

    /* UTF-8 degree sign (0xC2 0xB0) -> '*'. */
    jr_gist_sanitize(out, sizeof(out), "64\xC2\xB0 F tomorrow");
    TEST_ASSERT_EQUAL_STRING("64* F TOMORROW", out);
    assert_all_barlow_text(out);

    /* UTF-8 en-dash, em-dash, and Unicode minus -> '-'. */
    jr_gist_sanitize(out, sizeof(out), "3:00\xE2\x80\x93" "4:00 \xE2\x80\x94 \xE2\x88\x92" "5\xC2\xB0");
    TEST_ASSERT_EQUAL_STRING("3:00-4:00 - -5*", out);
    assert_all_barlow_text(out);

    /* UTF-8 middot, bullet, curly apostrophe, NBSP, ellipsis. */
    jr_gist_sanitize(out, sizeof(out), "a\xC2\xB7" "b \xE2\x80\xA2 it\xE2\x80\x99s\xC2\xA0ready\xE2\x80\xA6");
    TEST_ASSERT_EQUAL_STRING("A.B . IT'S READY...", out);
    assert_all_barlow_text(out);

    /* Control chars, unsupported ASCII, 4-byte emoji, and malformed UTF-8 dropped cleanly. */
    jr_gist_sanitize(out, sizeof(out), "\x01\"$96,420\" \xF0\x9F\x94\xA5 \x1B<ok>\x7F");
    TEST_ASSERT_EQUAL_STRING("96,420 OK", out);
    assert_all_barlow_text(out);

    /* Truncated / stray UTF-8 bytes at string end never overread. */
    jr_gist_sanitize(out, sizeof(out), "hi\xC2");
    TEST_ASSERT_EQUAL_STRING("HI", out);
    jr_gist_sanitize(out, sizeof(out), "hi\xE2\x80");
    TEST_ASSERT_EQUAL_STRING("HI", out);
    jr_gist_sanitize(out, sizeof(out), "\xFF\x80ok");
    TEST_ASSERT_EQUAL_STRING("OK", out);
}

static void test_gist_sanitize_truncation_inplace_and_bounds(void)
{
    char buf[32];

    /* Word-boundary truncation leaves no trailing space. */
    jr_gist_sanitize(buf, 5u, "ABC DEF");
    TEST_ASSERT_EQUAL_STRING("ABC", buf);

    jr_gist_sanitize(buf, 6u, "ABC DEF");
    TEST_ASSERT_EQUAL_STRING("ABC D", buf);

    /* Ellipsis stops cleanly at cap without overflow. */
    jr_gist_sanitize(buf, 7u, "WAIT\xE2\x80\xA6");
    TEST_ASSERT_EQUAL_STRING("WAIT..", buf);

    /* In-place sanitization (dst == src). */
    strcpy(buf, "  72\xC2\xB0  sunny\xE2\x80\xA6  ");
    jr_gist_sanitize(buf, sizeof(buf), buf);
    TEST_ASSERT_EQUAL_STRING("72* SUNNY...", buf);

    /* Null / zero / cap=1 safety. */
    jr_gist_sanitize(NULL, 16u, "abc");
    buf[0] = 'Z';
    jr_gist_sanitize(buf, 0u, "abc");
    TEST_ASSERT_EQUAL_CHAR('Z', buf[0]);
    jr_gist_sanitize(buf, 1u, "abc");
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
    buf[0] = 'Z';
    jr_gist_sanitize(buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);

    /* Struct capacities match public constants. */
    jr_gist_t g;
    TEST_ASSERT_EQUAL_UINT(24u, (unsigned)sizeof(g.eyebrow));
    TEST_ASSERT_EQUAL_UINT(44u, (unsigned)sizeof(g.hero));
    TEST_ASSERT_EQUAL_UINT(44u, (unsigned)sizeof(g.detail));
    TEST_ASSERT_EQUAL_UINT(44u, (unsigned)sizeof(g.detail2));
    TEST_ASSERT_EQUAL_UINT(24u, (unsigned)sizeof(g.footer));
}

static void test_gist_clamp_ttl(void)
{
    TEST_ASSERT_EQUAL_UINT32(JR_GIST_DEFAULT_MS, jr_gist_clamp_ttl(0u));
    TEST_ASSERT_EQUAL_UINT32(JR_GIST_MIN_MS,     jr_gist_clamp_ttl(1u));
    TEST_ASSERT_EQUAL_UINT32(JR_GIST_MIN_MS,     jr_gist_clamp_ttl(3999u));
    TEST_ASSERT_EQUAL_UINT32(4000u,              jr_gist_clamp_ttl(4000u));
    TEST_ASSERT_EQUAL_UINT32(15000u,             jr_gist_clamp_ttl(15000u));
    TEST_ASSERT_EQUAL_UINT32(20000u,             jr_gist_clamp_ttl(20000u));
    TEST_ASSERT_EQUAL_UINT32(60000u,             jr_gist_clamp_ttl(60000u));
    TEST_ASSERT_EQUAL_UINT32(JR_GIST_MAX_MS,     jr_gist_clamp_ttl(60001u));
    TEST_ASSERT_EQUAL_UINT32(JR_GIST_MAX_MS,     jr_gist_clamp_ttl(0xFFFFFFFFu));
}

void gist_tests_run(void)
{
    RUN_TEST(test_tool_plain_describe_all_30_tools);
    RUN_TEST(test_tool_plain_describe_fallbacks_and_null);
    RUN_TEST(test_tool_plain_caption_formatting_and_truncation);
    RUN_TEST(test_gist_kind_parse_and_name_roundtrip);
    RUN_TEST(test_gist_sanitize_utf8_punctuation_and_whitespace);
    RUN_TEST(test_gist_sanitize_truncation_inplace_and_bounds);
    RUN_TEST(test_gist_clamp_ttl);
}
