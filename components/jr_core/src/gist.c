/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/gist.c — Plain-language tool thinking mapper & 20-second Reply Gist
 * helpers (Glass 3.7). Pure C11: zero ESP-IDF includes, no heap, no libm.
 */
#include "jr_core/gist.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *verb;
    const char *subject;
    bool        is_local;
} tool_plain_entry_t;

static const char kScopeLocal[]   = "ON DEVICE";
static const char kScopeGateway[] = "GATEWAY";

static const tool_plain_entry_t kToolPlainTable[] = {
    /* 24 on-device tools */
    { "current_time",     "CHECKING",     "THE TIME",       true  },
    { "weather_here",     "CHECKING",     "WEATHER",        true  },
    { "set_volume",       "ADJUSTING",    "VOLUME",         true  },
    { "set_brightness",   "ADJUSTING",    "BRIGHTNESS",     true  },
    { "ask_user",         "ASKING",       "YOUR CHOICE",    true  },
    { "set_timer",        "SETTING",      "TIMER",          true  },
    { "cancel_timer",     "CLEARING",     "TIMER",          true  },
    { "list_timers",      "CHECKING",     "TIMERS",         true  },
    { "set_glass",        "SWITCHING",    "DISPLAY",        true  },
    { "device_status",    "CHECKING",     "DEVICE STATUS",  true  },
    { "start_focus",      "STARTING",     "FOCUS SESSION",  true  },
    { "stopwatch",        "UPDATING",     "STOPWATCH",      true  },
    { "add_note",         "SAVING",       "DESK NOTE",      true  },
    { "list_notes",       "CHECKING",     "DESK INBOX",     true  },
    { "complete_note",    "CHECKING OFF", "DESK TASK",      true  },
    { "clear_notes",      "CLEARING",     "DESK INBOX",     true  },
    { "daily_briefing",   "PREPARING",    "BRIEFING",       true  },
    { "set_ui_sounds",    "UPDATING",     "UI SOUNDS",      true  },
    { "set_sunlight",     "ADJUSTING",    "SUNLIGHT BOOST", true  },
    { "set_desk_mode",    "SWITCHING",    "DESK MODE",      true  },
    { "start_breathe",    "STARTING",     "BREATHE",        true  },
    { "set_battery_care", "UPDATING",     "BATTERY CARE",   true  },
    { "show_gist",        "PINNING",      "SUMMARY",        true  },
    { "aquarium",         "UPDATING",     "NANO REEF",      true  },
    /* 6 gateway tools */
    { "search_tools",     "FINDING",      "TOOL",           false },
    { "execute_tool",     "CHECKING",     "THE WEB",        false },
    { "recall_memory",    "CHECKING",     "MEMORY",         false },
    { "remember",         "SAVING",       "TO MEMORY",      false },
    { "delegate_task",    "QUEUING",      "AGENT JOB",      false },
    { "delegated_tasks",  "CHECKING",     "AGENT BOARD",    false },
};

void jr_tool_plain_describe(const char *tool_name, jr_tool_plain_t *out)
{
    if (out == NULL) {
        return;
    }
    if (tool_name != NULL) {
        for (size_t i = 0u; i < sizeof(kToolPlainTable) / sizeof(kToolPlainTable[0]); ++i) {
            if (strcmp(tool_name, kToolPlainTable[i].name) == 0) {
                out->verb     = kToolPlainTable[i].verb;
                out->subject  = kToolPlainTable[i].subject;
                out->scope    = kToolPlainTable[i].is_local ? kScopeLocal : kScopeGateway;
                out->is_local = kToolPlainTable[i].is_local;
                return;
            }
        }
    }
    out->verb     = "WORKING";
    out->subject  = "REQUEST";
    out->scope    = kScopeGateway;
    out->is_local = false;
}

size_t jr_tool_plain_caption(const char *tool_name, char *out, size_t cap)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    jr_tool_plain_t desc;
    jr_tool_plain_describe(tool_name, &desc);
    const int n = snprintf(out, cap, "%s %s - %s", desc.verb, desc.subject, desc.scope);
    if (n < 0) {
        out[0] = '\0';
        return 0u;
    }
    return (size_t)n >= cap ? (cap - 1u) : (size_t)n;
}

static bool ascii_ieq_span(const char *start, size_t len, const char *lit)
{
    for (size_t i = 0u; i < len; ++i) {
        if (lit[i] == '\0') {
            return false;
        }
        if (tolower((unsigned char)start[i]) != (int)(unsigned char)lit[i]) {
            return false;
        }
    }
    return lit[len] == '\0';
}

jr_gist_kind_t jr_gist_parse_kind(const char *s)
{
    if (s == NULL) {
        return JR_GIST_NONE;
    }
    while (*s != '\0' && isspace((unsigned char)*s)) {
        ++s;
    }
    size_t len = strlen(s);
    while (len > 0u && isspace((unsigned char)s[len - 1u])) {
        --len;
    }
    if (len == 0u) {
        return JR_GIST_NONE;
    }
    if (ascii_ieq_span(s, len, "number"))   return JR_GIST_NUMBER;
    if (ascii_ieq_span(s, len, "event"))    return JR_GIST_EVENT;
    if (ascii_ieq_span(s, len, "timer"))    return JR_GIST_TIMER;
    if (ascii_ieq_span(s, len, "note"))     return JR_GIST_NOTE;
    if (ascii_ieq_span(s, len, "weather"))  return JR_GIST_WEATHER;
    if (ascii_ieq_span(s, len, "briefing")) return JR_GIST_BRIEFING;
    if (ascii_ieq_span(s, len, "answer"))   return JR_GIST_ANSWER;
    return JR_GIST_NONE;
}

const char *jr_gist_kind_name(jr_gist_kind_t kind)
{
    switch (kind) {
    case JR_GIST_NUMBER:   return "number";
    case JR_GIST_EVENT:    return "event";
    case JR_GIST_TIMER:    return "timer";
    case JR_GIST_NOTE:     return "note";
    case JR_GIST_WEATHER:  return "weather";
    case JR_GIST_BRIEFING: return "briefing";
    case JR_GIST_ANSWER:   return "answer";
    case JR_GIST_NONE:
    default:               return "none";
    }
}

/* Glyphs present in Barlow GLASS_FONT_LABEL / GLASS_FONT_TEXT atlases
 * (see scripts/gen-glass-font.py TEXT_SET and glass_font.c '*' -> degree):
 * A-Z, 0-9, space, '*', and "-_.:/%?!+,'()&". */
static bool gist_ascii_glyph_ok(unsigned char ch)
{
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
        return true;
    }
    switch (ch) {
    case '-':
    case '_':
    case '.':
    case ':':
    case '/':
    case '%':
    case '?':
    case '!':
    case '+':
    case ',':
    case '\'':
    case '(':
    case ')':
    case '&':
    case '*':
        return true;
    default:
        return false;
    }
}

/* Emit one non-space character into dst (flushing any deferred single space
 * first only if both the space and ch fit). Returns false when dst is full. */
static bool gist_push(char *dst, size_t cap, size_t *len,
                      bool *pending_space, char ch)
{
    const size_t need = (*pending_space && *len > 0u) ? 2u : 1u;
    if (*len + need >= cap) {
        return false;
    }
    if (*pending_space && *len > 0u) {
        dst[(*len)++] = ' ';
    }
    dst[(*len)++] = ch;
    *pending_space = false;
    return true;
}

void jr_gist_sanitize(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0u) {
        return;
    }
    if (src == NULL || cap == 1u) {
        dst[0] = '\0';
        return;
    }

    size_t len = 0u;
    bool pending_space = false;
    const unsigned char *p = (const unsigned char *)src;

    while (*p != '\0') {
        const unsigned char b0 = *p;

        if (b0 < 0x80u) {
            ++p;
            if (isspace(b0)) {
                if (len > 0u) {
                    pending_space = true;
                }
                continue;
            }
            unsigned char out_ch = b0;
            if (out_ch >= 'a' && out_ch <= 'z') {
                out_ch = (unsigned char)(out_ch - ('a' - 'A'));
            }
            if (!gist_ascii_glyph_ok(out_ch)) {
                continue;
            }
            if (!gist_push(dst, cap, &len, &pending_space, (char)out_ch)) {
                break;
            }
            continue;
        }

        /* 2-byte UTF-8 sequence */
        if ((b0 & 0xE0u) == 0xC0u) {
            const unsigned char b1 = p[1];
            if ((b1 & 0xC0u) != 0x80u) {
                ++p;
                continue;
            }
            p += 2;
            if (b0 == 0xC2u && b1 == 0xB0u) {
                /* U+00B0 DEGREE SIGN -> '*' (glass_font.c degree alias) */
                if (!gist_push(dst, cap, &len, &pending_space, '*')) {
                    break;
                }
            } else if (b0 == 0xC2u && b1 == 0xB7u) {
                /* U+00B7 MIDDLE DOT -> '.' */
                if (!gist_push(dst, cap, &len, &pending_space, '.')) {
                    break;
                }
            } else if (b0 == 0xC2u && b1 == 0xA0u) {
                /* U+00A0 NO-BREAK SPACE -> collapsed space */
                if (len > 0u) {
                    pending_space = true;
                }
            }
            continue;
        }

        /* 3-byte UTF-8 sequence */
        if ((b0 & 0xF0u) == 0xE0u) {
            const unsigned char b1 = p[1];
            if ((b1 & 0xC0u) != 0x80u) {
                ++p;
                continue;
            }
            const unsigned char b2 = p[2];
            if ((b2 & 0xC0u) != 0x80u) {
                p += 2;
                continue;
            }
            p += 3;
            if (b0 == 0xE2u && b1 == 0x80u && (b2 == 0x93u || b2 == 0x94u)) {
                /* U+2013 EN DASH / U+2014 EM DASH -> '-' */
                if (!gist_push(dst, cap, &len, &pending_space, '-')) {
                    break;
                }
            } else if (b0 == 0xE2u && b1 == 0x88u && b2 == 0x92u) {
                /* U+2212 MINUS SIGN -> '-' */
                if (!gist_push(dst, cap, &len, &pending_space, '-')) {
                    break;
                }
            } else if (b0 == 0xE2u && b1 == 0x80u && b2 == 0xA2u) {
                /* U+2022 BULLET -> '.' */
                if (!gist_push(dst, cap, &len, &pending_space, '.')) {
                    break;
                }
            } else if (b0 == 0xE2u && b1 == 0x80u && (b2 == 0x98u || b2 == 0x99u)) {
                /* U+2018 / U+2019 curly single quotes -> '\'' */
                if (!gist_push(dst, cap, &len, &pending_space, '\'')) {
                    break;
                }
            } else if (b0 == 0xE2u && b1 == 0x80u && b2 == 0xA6u) {
                /* U+2026 HORIZONTAL ELLIPSIS -> "..." */
                for (int k = 0; k < 3; ++k) {
                    if (!gist_push(dst, cap, &len, &pending_space, '.')) {
                        goto done;
                    }
                }
            }
            continue;
        }

        /* 4-byte UTF-8 sequence (e.g. emoji): skip whole sequence cleanly */
        if ((b0 & 0xF8u) == 0xF0u) {
            ++p;
            for (int k = 0; k < 3 && (*p & 0xC0u) == 0x80u; ++k) {
                ++p;
            }
            continue;
        }

        /* Stray continuation / invalid byte */
        ++p;
    }

done:
    dst[len] = '\0';
}

uint32_t jr_gist_clamp_ttl(uint32_t ttl_ms)
{
    if (ttl_ms == 0u) {
        return JR_GIST_DEFAULT_MS;
    }
    if (ttl_ms < JR_GIST_MIN_MS) {
        return JR_GIST_MIN_MS;
    }
    if (ttl_ms > JR_GIST_MAX_MS) {
        return JR_GIST_MAX_MS;
    }
    return ttl_ms;
}
