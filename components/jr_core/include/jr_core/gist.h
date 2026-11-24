/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/gist.h — Plain-language tool thinking mapper & 20-second Reply Gist
 * data model (Glass 3.7). Pure C11: zero ESP-IDF includes, no heap, no libm.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Plain-language description of a running tool (Glass 4 language:
 * "Says what it is doing in plain words, never a tool name, and whether
 * it needs the network.") */
typedef struct {
    const char *verb;     /* e.g. "CHECKING", "SETTING", "SAVING", "SEARCHING" */
    const char *subject;  /* e.g. "WEATHER", "TIMER", "DESK INBOX", "WEB" */
    const char *scope;    /* "ON DEVICE" or "GATEWAY" */
    bool        is_local;
} jr_tool_plain_t;

void jr_tool_plain_describe(const char *tool_name, jr_tool_plain_t *out);
size_t jr_tool_plain_caption(const char *tool_name, char *out, size_t cap);

/* Structured 20-second Reply Gist ("Every turn ends with something to look at") */
typedef enum {
    JR_GIST_NONE = 0,
    JR_GIST_NUMBER,    /* Price, score, metric: hero="$96,420", detail="+2.1% TODAY" */
    JR_GIST_EVENT,     /* Calendar event: eyebrow="NEXT IN 42M", hero="DENTIST", detail="3:30 PM" */
    JR_GIST_TIMER,     /* Timer set: eyebrow="PASTA", hero="9:00", footer="ENDS 6:31 PM" */
    JR_GIST_NOTE,      /* Note saved: eyebrow="SAVED TO WORK", hero="CALL LANDLORD", footer="4 IN WORK" */
    JR_GIST_WEATHER,   /* Forecast: eyebrow="TOMORROW", hero="64*", detail="RAIN FROM 3 PM" */
    JR_GIST_BRIEFING,  /* Multi-line summary (up to 3 compact lines + footer) */
    JR_GIST_ANSWER,    /* Concise spoken answer receipt */
} jr_gist_kind_t;

#define JR_GIST_EYEBROW_CAP 24U
#define JR_GIST_LINE_CAP    44U
#define JR_GIST_DEFAULT_MS  20000U
#define JR_GIST_MIN_MS       4000U
#define JR_GIST_MAX_MS      60000U

typedef struct {
    jr_gist_kind_t kind;
    char eyebrow[JR_GIST_EYEBROW_CAP];
    char hero[JR_GIST_LINE_CAP];
    char detail[JR_GIST_LINE_CAP];
    char detail2[JR_GIST_LINE_CAP];
    char footer[JR_GIST_EYEBROW_CAP];
    uint32_t ttl_ms;
} jr_gist_t;

jr_gist_kind_t jr_gist_parse_kind(const char *s);
const char *jr_gist_kind_name(jr_gist_kind_t kind);
void jr_gist_sanitize(char *dst, size_t cap, const char *src);
uint32_t jr_gist_clamp_ttl(uint32_t ttl_ms);

#ifdef __cplusplus
}
#endif
