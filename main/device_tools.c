/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * device_tools.c — the device-tool lane: queue, consent, weather, activity.
 *
 * What a tool call becomes on the device: the result queue the voice task
 * drains, the consent window, the weather refresh the device owns, and
 * the ACTIVITY notes. Split out of main.c on 2026-09-02, no behavior change.
 */
#include "app.h"

static const char *TAG = "jarvis_v5";


const char *device_tool_last_status(void)
{
    if (!atomic_load(&s_tool_diag.last_status_valid)) {
        return "idle";
    }
    return jr_tools_status_name(
        (jr_tool_status_t)atomic_load(&s_tool_diag.last_status));
}

static uint32_t device_tool_slot(const char *name)
{
    if (name == NULL) {
        return 0U;
    }
    for (uint32_t i = 0U; i < DEVICE_TOOL_DECL_COUNT; ++i) {
        if (strcmp(name, s_device_tool_fns[i].name) == 0) {
            return i + 1U;
        }
    }
    return 0U;
}

const char *device_tool_last_name(void)
{
    uint32_t slot = atomic_load(&s_tool_diag.last_tool_slot);
    return slot > 0U && slot <= DEVICE_TOOL_DECL_COUNT
        ? s_device_tool_fns[slot - 1U].name : "none";
}

/* ---- WEATHER: the glass's own fetch through the tool worker ---------------
 *
 * A device-originated job, not a Gemini one: it carries a reserved call id,
 * never reaches the orchestrator, and never produces a toolResponse. Fetched
 * once after the worker is ready and then only when the WEATHER screen is
 * entered and the data is over ten minutes old — never on a timer, which
 * would drag Wi-Fi out of min-modem during rest. The job is served by a
 * direct Open-Meteo GET in jr_tools (no gateway, no key). */
#define LOCAL_WEATHER_CALL_ID   0x4C574558u          /* "LWEX" */
#define LOCAL_WEATHER_CALL_TEXT "local:weather"
static uint32_t s_weather_last_fetch_ms;   /* app task only; 0 = never */
static bool     s_weather_inflight;        /* app task only */
static bool     s_weather_failed;          /* app task only; the last fetch brought nothing */
/* THE SUN AND THE HOURS (wave N13). The same glance carries today's
 * sunrise/sunset in minutes past local midnight and 36 hourly rain
 * probabilities from today's midnight; the watch draws the day arc from
 * the first, the rain latch reads the second. -1 / 255 mean "not in the
 * answer", never zero. */
int16_t  s_wx_sun_rise_min = -1;           /* app task writes; the shell publish reads */
int16_t  s_wx_sun_set_min  = -1;
uint8_t  s_wx_rain_pp[JR_RAIN_HOURS];      /* app task only */
_Static_assert(JR_DISPLAY_RAIN_HOURS == JR_RAIN_HOURS &&
               JR_RAIN_PP_UNKNOWN == 255, "the glass reads the glance's hours as-is");
static bool           s_wx_rain_valid;     /* app task only */
static jr_rain_warn_t s_rain_warn = { .armed = true };
jr_display_weather_t s_weather;     /* last good data; app task only */

static jr_display_sky_t weather_sky_from(const char *cond)
{
    char low[40];
    size_t i = 0;
    for (; cond != NULL && cond[i] != '\0' && i + 1U < sizeof low; ++i) {
        low[i] = (char)tolower((unsigned char)cond[i]);
    }
    low[i] = '\0';
    if (strstr(low, "thunder") || strstr(low, "storm")) return JR_DISPLAY_SKY_STORM;
    if (strstr(low, "snow") || strstr(low, "sleet") || strstr(low, "ice")) return JR_DISPLAY_SKY_SNOW;
    if (strstr(low, "rain") || strstr(low, "drizzle") || strstr(low, "shower")) return JR_DISPLAY_SKY_RAIN;
    if (strstr(low, "fog") || strstr(low, "mist") || strstr(low, "haze")) return JR_DISPLAY_SKY_FOG;
    if (strstr(low, "overcast")) return JR_DISPLAY_SKY_CLOUDS;
    if (strstr(low, "cloud")) return JR_DISPLAY_SKY_PARTLY;
    if (strstr(low, "clear") || strstr(low, "sun")) return JR_DISPLAY_SKY_CLEAR;
    return JR_DISPLAY_SKY_UNKNOWN;
}

static int16_t c_to_f(double c) { return (int16_t)lrint(c * 9.0 / 5.0 + 32.0); }

/* One line, once per front: an ACTIVITY row always, spoken when a session
 * is open with the microphone live, a caption otherwise — the same rule the
 * board's announcements follow. The hour is the first in the next three at
 * or above JR_RAIN_WARN_PCT; the latch in jr_core keeps it to one. */
static void rain_announce(int ahead_h, uint8_t pct)
{
    char row[40];                       /* the ACTIVITY row cuts at 24 glyphs */
    snprintf(row, sizeof row, "IN %dH %u%% LIKELY", ahead_h, (unsigned)pct);
    jr_display_activity_push("RAIN", row);
    const jr_state_t p = jr_orch_phase(&s_app.orch);
    const bool open = p == JR_ST_LISTENING || p == JR_ST_SPEAKING ||
                      p == JR_ST_THINKING;
    if (open && !atomic_load(&s_voice_privacy_paused)) {
        char line[120];
        snprintf(line, sizeof line,
                 "Rain is likely in about %d hour%s, %u percent chance.",
                 ahead_h, ahead_h == 1 ? "" : "s", (unsigned)pct);
        handle_say(line);
    } else {
        char cap[24];
        snprintf(cap, sizeof cap, "RAIN IN %dH", ahead_h);
        jr_display_caption_set(cap);
    }
    ESP_LOGI(TAG, "weather: rain in %d h (%u%%)", ahead_h, (unsigned)pct);
}

/* For the morning briefing: hours until the first hour at or above the
 * warning line within `horizon_h` hours of now, or 0. Reads the data, not
 * the latch, so a briefing can name rain the afternoon warning will later
 * repeat. */
int weather_rain_ahead_h(int horizon_h)
{
    if (!s_wx_rain_valid) {
        return 0;
    }
    struct tm tmv;
    time_t tt = time(NULL);
    localtime_r(&tt, &tmv);
    if (tmv.tm_year < 2020 - 1900) {
        return 0;
    }
    for (int k = 1; k <= horizon_h && tmv.tm_hour + k < JR_RAIN_HOURS; ++k) {
        const uint8_t v = s_wx_rain_pp[tmv.tm_hour + k];
        if (v != JR_RAIN_PP_UNKNOWN && v >= JR_RAIN_WARN_PCT) {
            return k;
        }
    }
    return 0;
}

static void weather_apply_result(const jr_tool_result_t *result, uint32_t now)
{
    s_weather_inflight = false;
    s_weather_last_fetch_ms = now;
    if (result->status != JR_TOOL_STATUS_OK) {
        s_weather_failed = true;
        ESP_LOGW(TAG, "weather: fetch failed (%s); glass keeps %s data",
                 jr_tools_status_name(result->status),
                 s_weather.valid ? "the previous" : "no");
        return;
    }
    cJSON *root = cJSON_Parse(result->response_json);
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (r == NULL) {
        r = root;
    }
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "t");
    if (!cJSON_IsNumber(t)) {
        s_weather_failed = true;
        ESP_LOGW(TAG, "weather: no temperature in result; keeping old data");
        cJSON_Delete(root);
        return;
    }
    s_weather_failed = false;
    jr_display_weather_t w = {0};
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(r, "f");
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(r, "h");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(r, "c");
    const cJSON *ws = cJSON_GetObjectItemCaseSensitive(r, "ws");
    const cJSON *hi = cJSON_GetObjectItemCaseSensitive(r, "hi");
    const cJSON *lo = cJSON_GetObjectItemCaseSensitive(r, "lo");
    const cJSON *rain = cJSON_GetObjectItemCaseSensitive(r, "r");
    w.valid = true;
    w.temp_f = c_to_f(t->valuedouble);
    w.feels_f = cJSON_IsNumber(f) ? c_to_f(f->valuedouble) : w.temp_f;
    w.hi_f = cJSON_IsNumber(hi) ? c_to_f(hi->valuedouble) : w.temp_f;
    w.lo_f = cJSON_IsNumber(lo) ? c_to_f(lo->valuedouble) : w.temp_f;
    w.humidity_pct = cJSON_IsNumber(h) && h->valuedouble >= 0 && h->valuedouble <= 100
                         ? (uint8_t)lrint(h->valuedouble) : 0U;
    w.wind_mph = cJSON_IsNumber(ws) && ws->valuedouble >= 0
                     ? (uint8_t)(lrint(ws->valuedouble * 0.621) > 255 ? 255 : lrint(ws->valuedouble * 0.621)) : 0U;
    double mm = cJSON_IsNumber(rain) && rain->valuedouble > 0 ? rain->valuedouble : 0.0;
    w.rain_pct = (uint8_t)(mm >= 25.0 ? 100 : lrint(mm * 4.0));
    const char *cond = cJSON_IsString(c) && c->valuestring ? c->valuestring : "";
    w.sky = weather_sky_from(cond);
    size_t n = 0;
    for (; cond[n] != '\0' && n + 1U < sizeof w.condition; ++n) {
        w.condition[n] = (char)toupper((unsigned char)cond[n]);
    }
    w.condition[n] = '\0';
    w.fetched_ms = now;
    /* The sun and the hours: absent when the extra fetch failed, and then
     * the previous answer's values stand (the sun moves a minute a day). */
    const cJSON *sr = cJSON_GetObjectItemCaseSensitive(r, "sr");
    const cJSON *ss = cJSON_GetObjectItemCaseSensitive(r, "ss");
    if (cJSON_IsNumber(sr) && cJSON_IsNumber(ss) && sr->valuedouble >= 0 &&
        ss->valuedouble >= 0 && sr->valuedouble < 1440 && ss->valuedouble < 1440) {
        s_wx_sun_rise_min = (int16_t)sr->valuedouble;
        s_wx_sun_set_min = (int16_t)ss->valuedouble;
    }
    /* The glass gets this answer's hours or none: an older array would be
     * anchored to a midnight the glass cannot check. */
    memset(w.rain_hours, JR_RAIN_PP_UNKNOWN, sizeof w.rain_hours);
    const cJSON *pp = cJSON_GetObjectItemCaseSensitive(r, "pp");
    if (cJSON_IsArray(pp) && cJSON_GetArraySize(pp) > 0) {
        memset(s_wx_rain_pp, JR_RAIN_PP_UNKNOWN, sizeof s_wx_rain_pp);
        int k = 0;
        const cJSON *v = NULL;
        cJSON_ArrayForEach(v, pp) {
            if (k >= JR_RAIN_HOURS) {
                break;
            }
            if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 100) {
                s_wx_rain_pp[k] = (uint8_t)lrint(v->valuedouble);
            }
            ++k;
        }
        s_wx_rain_valid = true;
        struct tm tmv;
        time_t tt = time(NULL);
        localtime_r(&tt, &tmv);
        if (tmv.tm_year >= 2020 - 1900) {    /* no clock, no anchor, no ring */
            memcpy(w.rain_hours, s_wx_rain_pp, sizeof w.rain_hours);
            w.rain_hours_mday = (uint8_t)tmv.tm_mday;
        }
    }
    cJSON_Delete(root);
    s_weather = w;
    jr_display_weather_set(&s_weather);
    if (s_wx_rain_valid) {
        struct tm tmv;
        time_t tt = time(NULL);
        localtime_r(&tt, &tmv);
        if (tmv.tm_year >= 2020 - 1900) {
            const int ahead = jr_rain_warning_step(&s_rain_warn, s_wx_rain_pp,
                                                   JR_RAIN_HOURS, tmv.tm_hour);
            if (ahead > 0) {
                rain_announce(ahead, s_wx_rain_pp[tmv.tm_hour + ahead]);
            }
        }
    }
    static bool s_rain_announced;
    if (!s_rain_announced && w.rain_pct >= 40U) {
        /* Once per boot, one line, no speech: the glass mentions the day. */
        s_rain_announced = true;
        jr_display_caption_set("RAIN TODAY");
    }
    ESP_LOGI(TAG, "weather: %d F (%s) hi %d lo %d rain %u%% wind %u mph",
             w.temp_f, w.condition, w.hi_f, w.lo_f, w.rain_pct, w.wind_mph);
}

const char *screen_voice_prompt(jr_display_space_t space)
{
    switch (space) {
    case JR_DISPLAY_SPACE_WEATHER:
        return "In two sentences, brief me on the weather in Fort Lauderdale "
               "right now, with today's high and low, in Fahrenheit.";
    case JR_DISPLAY_SPACE_WATCH:
        return "Tell me the time and today's date in one short sentence.";
    case JR_DISPLAY_SPACE_ACTIVITY:
        return "In two sentences, recap what we talked about most recently.";
    default:
        return NULL;
    }
}

/* LIFT TO GLANCE. Picked up after a rest, the glass shows the weather for a
 * few seconds and then goes home by itself — a device that noticed you,
 * without saying a word. Any input keeps whatever screen is up. */
uint32_t s_glance_until_ms;        /* voice task only; 0 = none */

void weather_maybe_fetch(uint32_t now)
{
    /* No gateway needed: the worker serves this job straight from Open-Meteo. */
    if (s_weather_inflight || !atomic_load(&s_tool_diag.worker_ready)) {
        return;
    }
    const bool first = s_weather_last_fetch_ms == 0U;
    const bool on_screen = jr_display_nav_space() == JR_DISPLAY_SPACE_WEATHER;
    /* All roads, including first hydration and retries, share the same rest
     * and network gates. A stale watch cell is cheaper than a sleeping radio
     * repeatedly attempting a fetch that cannot reach the network. */
    jr_net_status_t net = {0};
    const bool connected = jr_net_get_status(&net) == ESP_OK && net.sta_connected;
    if (!jr_mood_weather_due((jr_mood_t)atomic_load(&s_mood_id), connected,
                             now, s_weather_last_fetch_ms,
                             s_weather_failed, on_screen)) {
        return;
    }
    jr_tool_job_t job = {
        .call_id = LOCAL_WEATHER_CALL_ID,
        .call_id_text = LOCAL_WEATHER_CALL_TEXT,
        .name = "weather_glance",
        .args_json = "{}",
        .session_gen = JR_TOOLS_SESSION_ANY,   /* the glass's own fetch */
    };
    if (jr_tools_submit(&job) == ESP_OK) {
        s_weather_inflight = true;
        s_weather_last_fetch_ms = now;    /* also rate-limits a failing fetch */
        ESP_LOGI(TAG, "weather: fetch submitted (%s)",
                 first ? "first" : s_weather_failed ? "retry"
                       : on_screen ? "screen entered" : "half hour");
    }
}

/* ---- THE GATEWAY: is it usable, or only configured? -----------------------
 *
 * `configured` only says an address and a key are stored. The device also
 * keeps the outcome of its calls: three in a row that never reached a server
 * (DNS, connect, TLS) make the gateway unusable until one does. The policy is
 * jr_core/tool_policy.h; this is its one instance. Written on the voice task
 * (results are drained there), read by the voice task at SEND_SETUP and by
 * httpd for the cockpit, where a one-byte stale read is harmless. */
static jr_gateway_t s_gateway;

bool device_gateway_usable(void)
{
    return jr_gateway_usable(&s_gateway, jr_tools_is_configured());
}

uint32_t device_gateway_poll_ms(void)
{
    return jr_gateway_poll_ms(&s_gateway);
}

unsigned device_declared_tools(void)
{
    const bool gateway = device_gateway_usable();
    unsigned n = 0U;
    for (uint32_t i = 0U; i < DEVICE_TOOL_DECL_COUNT; ++i) {
        if (gateway || !jr_tool_needs_gateway(s_device_tool_fns[i].name)) {
            ++n;
        }
    }
    return n;
}

const char *device_gateway_why(void)
{
    return jr_gateway_why(&s_gateway, jr_tools_is_configured(),
                          atomic_load(&s_tool_diag.worker_ready));
}

/* A finished gateway job tells us whether the gateway answered. Only the two
 * outcomes that say so count; cancellations, stale results and bad arguments
 * say nothing about the network. The glass's own weather never goes through
 * the gateway and is not passed here. */
static void device_gateway_note_result(jr_tool_status_t status)
{
    switch (status) {
    case JR_TOOL_STATUS_NETWORK_ERROR:
        jr_gateway_note(&s_gateway, false);
        break;
    case JR_TOOL_STATUS_OK:
    case JR_TOOL_STATUS_HTTP_ERROR:
    case JR_TOOL_STATUS_BAD_RESPONSE:
        jr_gateway_note(&s_gateway, true);
        break;
    default:
        break;
    }
}

/* ---- THE BOARD: announcing what the hands elsewhere finished -------------
 *
 * Another device-owned job. Every BOARD_POLL_MS while the device is up, on
 * Wi-Fi, and not in DREAM, the six most recently touched work items on the
 * paired project come back as {i,s,n,r}. A completed or blocked item whose
 * id is not in the ring is announced once: an ACTIVITY row, and a spoken
 * line when a session is open with the mic live, else a caption. The first
 * poll only seeds the ring, so nothing finished before boot is re-announced
 * after every restart. */
#define LOCAL_BOARD_CALL_ID   0x4C425244u          /* "LBRD" */
#define LOCAL_BOARD_CALL_TEXT "local:board"
#define BOARD_POLL_MS         90000u
#define BOARD_FIRST_POLL_MS   30000u
#define BOARD_SEEN_CAP        16U
#define BOARD_ID_CAP          41U
static uint32_t s_board_last_poll_ms;             /* app task only; 0 = never */
static bool     s_board_inflight;                 /* app task only */
static bool     s_board_seeded;                   /* first poll landed */
static char     s_board_seen[BOARD_SEEN_CAP][BOARD_ID_CAP];
static uint8_t  s_board_seen_next;

static bool board_seen(const char *id)
{
    for (size_t i = 0; i < BOARD_SEEN_CAP; ++i) {
        if (s_board_seen[i][0] != '\0' && strcmp(s_board_seen[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static void board_mark_seen(const char *id)
{
    strlcpy(s_board_seen[s_board_seen_next], id, BOARD_ID_CAP);
    s_board_seen_next = (uint8_t)((s_board_seen_next + 1U) % BOARD_SEEN_CAP);
}

/* completed/done vs blocked; anything else is still in flight. */
static int board_terminal(const char *status)
{
    if (strncasecmp(status, "complet", 7) == 0 || strncasecmp(status, "done", 4) == 0) {
        return 1;
    }
    if (strncasecmp(status, "block", 5) == 0) {
        return 2;
    }
    return 0;
}

/* What finished since the last morning briefing took it: a count and the
 * newest title. Reset when the briefing reads it. */
static int  s_board_done_count;                   /* app task only */
static char s_board_done_title[64];               /* app task only */

void board_take_done(int *count, char *title, size_t cap)
{
    *count = s_board_done_count;
    if (title != NULL && cap > 0) {
        strlcpy(title, s_board_done_title, cap);
    }
    s_board_done_count = 0;
    s_board_done_title[0] = '\0';
}

static void board_announce(const char *title, int terminal, const char *result)
{
    char row[25];
    title_shorten(row, sizeof row - 5U, title);      /* "TASK " + 19 glyphs */
    jr_display_activity_push("TASK", row);
    if (terminal == 1) {
        if (s_board_done_count < 999) {
            s_board_done_count++;
        }
        strlcpy(s_board_done_title, title, sizeof s_board_done_title);
    }

    /* Three rising notes before the words: E5 G5 B5, 60 ms each, at half
     * volume in WHISPER. The chime refuses to talk over a reply, so while the
     * model is speaking the spoken line below carries the news alone. */
    static const uint16_t chime_hz[] = { 659U, 784U, 988U };
    const uint8_t chime_vol = s_mood.mood == JR_MOOD_WHISPER ? 4U : 8U;
    const esp_err_t chime = jr_audio_play_chime(chime_hz, 3U, 60U, chime_vol);
    ESP_LOGI(TAG, "chime: done%s", chime == ESP_OK ? "" : " (skipped)");

    const jr_state_t p = jr_orch_phase(&s_app.orch);
    const bool open = p == JR_ST_LISTENING || p == JR_ST_SPEAKING ||
                      p == JR_ST_THINKING;
    if (open && !atomic_load(&s_voice_privacy_paused)) {
        char line[200];
        snprintf(line, sizeof line, "%.60s is %s%s%.110s", title,
                 terminal == 2 ? "blocked" : "done",
                 result[0] != '\0' ? ": " : ".", result);
        handle_say(line);
    } else {
        /* The caption wraps by glyph count, so cut the title at a word
         * boundary: "DONE: WRITE A TWO-L / INE DESCRIPTION O" was the first
         * one the glass showed. */
        char cap[40];
        int n = snprintf(cap, sizeof cap, "%s: %.28s", terminal == 2 ? "BLOCKED" : "DONE", title);
        if (n > 0 && strlen(title) > 28U) {
            char *sp = strrchr(cap, ' ');
            if (sp != NULL && sp > cap + 8) {
                *sp = '\0';
            }
        }
        for (char *c = cap; *c != '\0'; ++c) {
            *c = (char)toupper((unsigned char)*c);
        }
        jr_display_caption_set(cap);
    }
    ESP_LOGI(TAG, "board: %s %s", terminal == 2 ? "blocked" : "done", title);
}

static void board_apply_result(const jr_tool_result_t *result)
{
    s_board_inflight = false;
    if (result->status != JR_TOOL_STATUS_OK) {
        ESP_LOGW(TAG, "board: poll failed (%s)", jr_tools_status_name(result->status));
        return;
    }
    cJSON *root = cJSON_Parse(result->response_json);
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (r == NULL) {
        r = root;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(r, "t");
    if (!cJSON_IsArray(items)) {
        ESP_LOGW(TAG, "board: poll had no items");
        cJSON_Delete(root);
        return;
    }
    const bool seeding = !s_board_seeded;
    const cJSON *w = NULL;
    cJSON_ArrayForEach(w, items) {
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(w, "i");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(w, "s");
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(w, "n");
        const cJSON *res = cJSON_GetObjectItemCaseSensitive(w, "r");
        if (!cJSON_IsString(i) || i->valuestring[0] == '\0' || !cJSON_IsString(s)) {
            continue;
        }
        const int terminal = board_terminal(s->valuestring);
        if (terminal == 0 || board_seen(i->valuestring)) {
            continue;
        }
        board_mark_seen(i->valuestring);
        if (!seeding) {
            board_announce(cJSON_IsString(n) ? n->valuestring : "a task", terminal,
                           cJSON_IsString(res) ? res->valuestring : "");
        }
    }
    s_board_seeded = true;
    cJSON_Delete(root);
}

void board_maybe_poll(uint32_t now)
{
    if (s_board_inflight || !atomic_load(&s_tool_diag.worker_ready) ||
        !jr_tools_is_configured()) {
        return;
    }
    const bool first = s_board_last_poll_ms == 0U;
    /* A gateway that has not answered the last few calls is probed once per
     * quarter hour instead of every 90 s (a host that does not resolve failed
     * the poll all boot long, 2026-11-10). */
    if (first ? now < BOARD_FIRST_POLL_MS
              : now - s_board_last_poll_ms < device_gateway_poll_ms()) {
        return;
    }
    const uint8_t mood = atomic_load(&s_mood_id);
    if (mood == (uint8_t)JR_MOOD_WHISPER || mood == (uint8_t)JR_MOOD_DREAM) {
        return;                       /* resting glass keeps Wi-Fi in min-modem */
    }
    jr_net_status_t net = {0};
    if (jr_net_get_status(&net) != ESP_OK || !net.sta_connected) {
        return;
    }
    jr_tool_job_t job = {
        .call_id = LOCAL_BOARD_CALL_ID,
        .call_id_text = LOCAL_BOARD_CALL_TEXT,
        .name = "board_poll",
        .args_json = "{}",
        .session_gen = JR_TOOLS_SESSION_ANY,   /* the device's own poll */
    };
    if (jr_tools_submit(&job) == ESP_OK) {
        s_board_inflight = true;
        s_board_last_poll_ms = now;        /* also rate-limits a failing poll */
    }
}

/* ---- ACTIVITY: what Jarvis actually did this turn ------------------------
 *
 * The kind is decided when a tool is dispatched (WEB, WEATHER, PRICE, ...),
 * the summary is the first words Jarvis spoke in reply, captured from the
 * transcript's HEAD — the caption accumulator keeps the newest words, which
 * is right for a live caption and wrong for a log line. Pushed at turn end. */
static char s_turn_kind[9] = "SAID";     /* voice task only */
static char s_turn_head[40];             /* voice task only */

void activity_note_tool(const char *tool_name, const char *args_json)
{
    const char *kind = NULL;
    char target[JR_TOOLS_NAME_CAP] = {0};
    if (tool_name != NULL && strcmp(tool_name, "execute_tool") == 0) {
        cJSON *args = cJSON_Parse(args_json != NULL ? args_json : "{}");
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(args, "tool");
        if (cJSON_IsString(t) && t->valuestring != NULL) {
            strlcpy(target, t->valuestring, sizeof target);
        }
        cJSON_Delete(args);
        if (strncmp(target, "websearch", 9) == 0 || strncmp(target, "research", 8) == 0 ||
            strncmp(target, "fetch", 5) == 0) kind = "WEB";
        else if (strncmp(target, "weather", 7) == 0) kind = "WEATHER";
        else if (strncmp(target, "wiki", 4) == 0) kind = "WIKI";
        else if (strncmp(target, "crypto", 6) == 0 || strncmp(target, "stocks", 6) == 0 ||
                 strncmp(target, "exchange", 8) == 0 || strncmp(target, "massive", 7) == 0) kind = "PRICE";
        else if (strncmp(target, "memory", 6) == 0) kind = "MEMORY";
        else if (strncmp(target, "time", 4) == 0) kind = "TIME";
        else if (target[0] != '\0') {
            /* the service name, uppercased, at most 8 glyphs */
            size_t n = 0;
            for (; target[n] != '\0' && target[n] != '.' && n < 8U; ++n) {
                s_turn_kind[n] = (char)toupper((unsigned char)target[n]);
            }
            s_turn_kind[n] = '\0';
            return;
        }
    } else if (tool_name != NULL) {
        if (strcmp(tool_name, "recall_memory") == 0 || strcmp(tool_name, "remember") == 0) kind = "MEMORY";
        else if (strcmp(tool_name, "current_time") == 0) kind = "TIME";
        else if (strcmp(tool_name, "weather_here") == 0) kind = "WEATHER";
        else if (strcmp(tool_name, "ask_user") == 0) kind = "ASK";
        else if (strcmp(tool_name, "search_tools") == 0) kind = "SEARCH";
        else if (strcmp(tool_name, "delegate_task") == 0) kind = "DELEGATE";
        else if (strcmp(tool_name, "delegated_tasks") == 0) kind = "TASKS";
        else if (strcmp(tool_name, "set_timer") == 0 ||
                 strcmp(tool_name, "cancel_timer") == 0 ||
                 strcmp(tool_name, "list_timers") == 0) kind = "TIMER";
        else if (strcmp(tool_name, "start_focus") == 0) kind = "FOCUS";
        else if (strcmp(tool_name, "stopwatch") == 0) kind = "CHRONO";
        else if (strcmp(tool_name, "add_note") == 0 ||
                 strcmp(tool_name, "list_notes") == 0 ||
                 strcmp(tool_name, "clear_notes") == 0) kind = "NOTES";
        else if (strcmp(tool_name, "daily_briefing") == 0) kind = "BRIEFING";
        else if (strcmp(tool_name, "set_ui_sounds") == 0) kind = "SOUNDS";
        else if (strcmp(tool_name, "set_sunlight") == 0) kind = "SUNLIGHT";
        else if (strcmp(tool_name, "set_glass") == 0) kind = "GLASS";
        else if (strcmp(tool_name, "device_status") == 0) kind = "STATUS";
    }
    if (kind != NULL) {
        strlcpy(s_turn_kind, kind, sizeof s_turn_kind);
    }
}

void activity_note_said(const char *text)
{
    const size_t have = strlen(s_turn_head);
    if (have + 1U >= sizeof s_turn_head || text == NULL) {
        return;
    }
    strlcpy(s_turn_head + have, text, sizeof s_turn_head - have);
}

void activity_note_turn_end(void)
{
    /* Skip leading spaces the transcript chunks carry, then fit the row:
     * a whole sentence if one fits, else a word boundary and "...". */
    const char *src = s_turn_head;
    while (*src == ' ') {
        src++;
    }
    if (*src != '\0') {
        /* One 24-glyph row holds "KIND summary": the summary gets what the
         * tag and its space leave. */
        char summary[25];
        size_t cap = 24U - strlen(s_turn_kind) - 1U;
        if (cap < 8U) {
            cap = 8U;
        }
        (void)jr_display_activity_fit(summary, cap + 1U, src);
        jr_display_activity_push(s_turn_kind, summary);
    }
    s_turn_head[0] = '\0';
    strlcpy(s_turn_kind, "SAID", sizeof s_turn_kind);
}

void device_tool_record_result(const char *name, jr_tool_status_t status,
                                      uint32_t duration_ms, int http_status)
{
    atomic_store(&s_tool_diag.last_tool_slot, device_tool_slot(name));
    atomic_store(&s_tool_diag.last_status, (int)status);
    atomic_store(&s_tool_diag.last_status_valid, true);
    atomic_store(&s_tool_diag.last_duration_ms, duration_ms);
    atomic_store(&s_tool_diag.last_http_status, http_status);
    atomic_fetch_add(&s_tool_diag.completed, 1U);
    if (status == JR_TOOL_STATUS_OK) {
        atomic_fetch_add(&s_tool_diag.succeeded, 1U);
    } else {
        atomic_fetch_add(&s_tool_diag.failed, 1U);
    }
}

void device_tool_drop_local_results(void)
{
    if (s_local_tool_count > 0U) {
        atomic_fetch_add(&s_tool_diag.stale_dropped,
                         (uint32_t)s_local_tool_count);
        memset(s_local_tool_results, 0, sizeof(s_local_tool_results));
        s_local_tool_head = 0U;
        s_local_tool_count = 0U;
    }
}

/* Queue a finished tool result for the voice task to hand to the model. The
 * JSON is built by the caller into the returned slot's buffer (cap
 * LOCAL_TOOL_ERROR_CAP); NULL when the queue is full. */
static local_tool_result_t *device_tool_queue_slot(uint32_t call_id,
                                                   const char *call_id_text,
                                                   const char *name,
                                                   uint32_t session_gen,
                                                   jr_tool_status_t status)
{
    if (s_local_tool_count >= LOCAL_TOOL_RESULT_CAP) {
        atomic_fetch_add(&s_tool_diag.failed, 1U);
        atomic_fetch_add(&s_tool_diag.response_send_failed, 1U);
        return NULL;
    }

    local_tool_result_t *slot = &s_local_tool_results[
        (s_local_tool_head + s_local_tool_count) % LOCAL_TOOL_RESULT_CAP];
    memset(slot, 0, sizeof(*slot));
    slot->call_id = call_id;
    slot->session_gen = session_gen;
    slot->status = status;
    strlcpy(slot->call_id_text, call_id_text ? call_id_text : "",
            sizeof(slot->call_id_text));
    strlcpy(slot->name, name ? name : "unknown",
            sizeof(slot->name));
    s_local_tool_count++;
    return slot;
}

static void device_tool_queue_error(uint32_t call_id, const char *call_id_text,
                                    const char *name, uint32_t session_gen,
                                    jr_tool_status_t status, const char *code,
                                    const char *message)
{
    local_tool_result_t *slot = device_tool_queue_slot(call_id, call_id_text,
                                                       name, session_gen, status);
    if (slot != NULL) {
        (void)snprintf(slot->response_json, sizeof(slot->response_json),
                       "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                       code, message);
    }
}

/* ---- tools the device answers itself --------------------------------------
 *
 * weather_here, set_timer, cancel_timer, list_timers, start_focus, stopwatch,
 * add_note, list_notes, clear_notes, daily_briefing, set_glass, set_ui_sounds,
 * set_sunlight, device_status (always), and current_time (when the gateway is
 * not usable). All run locally with zero network round-trips. */
const char *device_glass_space_name(jr_display_space_t space)
{
    switch (space) {
    case JR_DISPLAY_SPACE_JARVIS:   return "jarvis";
    case JR_DISPLAY_SPACE_WATCH:    return "watch";
    case JR_DISPLAY_SPACE_WEATHER:  return "weather";
    case JR_DISPLAY_SPACE_STATUS:   return "status";
    case JR_DISPLAY_SPACE_DESK:     return "desk";
    case JR_DISPLAY_SPACE_ACTIVITY: return "activity";
    default:                        return "jarvis";
    }
}

bool device_parse_glass_screen(const char *s, jr_display_space_t *out)
{
    if (s == NULL || s[0] == '\0') return false;
    if (strcasecmp(s, "jarvis") == 0 || strcasecmp(s, "home") == 0) {
        *out = JR_DISPLAY_SPACE_JARVIS; return true;
    }
    if (strcasecmp(s, "watch") == 0 || strcasecmp(s, "clock") == 0) {
        *out = JR_DISPLAY_SPACE_WATCH; return true;
    }
    if (strcasecmp(s, "weather") == 0) {
        *out = JR_DISPLAY_SPACE_WEATHER; return true;
    }
    if (strcasecmp(s, "status") == 0) {
        *out = JR_DISPLAY_SPACE_STATUS; return true;
    }
    if (strcasecmp(s, "activity") == 0) {
        *out = JR_DISPLAY_SPACE_ACTIVITY; return true;
    }
    return false;
}

bool device_parse_watch_style(const char *s, jr_watch_style_t *out)
{
    if (s == NULL || s[0] == '\0') return false;
    if (strcasecmp(s, "jarvis") == 0)    { *out = JR_WATCH_JARVIS;        return true; }
    if (strcasecmp(s, "diver") == 0)     { *out = JR_WATCH_DIVER;         return true; }
    if (strcasecmp(s, "dress") == 0)     { *out = JR_WATCH_DRESS;         return true; }
    if (strcasecmp(s, "pilot") == 0)     { *out = JR_WATCH_PILOT;         return true; }
    if (strcasecmp(s, "minimal") == 0)   { *out = JR_WATCH_MINIMAL;       return true; }
    if (strcasecmp(s, "future") == 0)    { *out = JR_WATCH_FUTURE;        return true; }
    if (strcasecmp(s, "skeleton") == 0)  { *out = JR_WATCH_SKELETON;      return true; }
    if (strcasecmp(s, "celestial") == 0) { *out = JR_WATCH_CELESTIAL;     return true; }
    if (strcasecmp(s, "orrery") == 0)    { *out = JR_WATCH_STYLE_ORRERY;  return true; }
    if (strcasecmp(s, "reactor") == 0)   { *out = JR_WATCH_STYLE_REACTOR; return true; }
    return false;
}

/* ---- NVS-persisted quick desk notes ("jr_notes", PSRAM bank) ------------ */
#define JR_NOTES_NVS_NS      "jr_notes"
#define JR_NOTES_NVS_KEY     "bank"
#define JR_NOTES_BLOB_MAGIC  0x4A524E31u /* "JRN1" */
#define JR_NOTES_BLOB_VER    1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    jr_note_bank_t bank;
} jr_notes_blob_t;

static jr_note_bank_t *s_notes;
static SemaphoreHandle_t s_notes_lock;

static int64_t device_wall_epoch_s(void)
{
    time_t tt = time(NULL);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    return (tmv.tm_year >= 2020 - 1900) ? (int64_t)tt : 0;
}

static void notes_persist_locked(void)
{
    if (s_notes == NULL) {
        return;
    }
    jr_notes_blob_t *blob = heap_caps_calloc(1, sizeof(*blob),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (blob == NULL) {
        blob = calloc(1, sizeof(*blob));
    }
    if (blob == NULL) {
        return;
    }
    blob->magic = JR_NOTES_BLOB_MAGIC;
    blob->version = JR_NOTES_BLOB_VER;
    blob->bank = *s_notes;

    nvs_handle_t h;
    if (nvs_open(JR_NOTES_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_set_blob(h, JR_NOTES_NVS_KEY, blob, sizeof(*blob));
        if (err == ESP_OK) {
            (void)nvs_commit(h);
        } else {
            ESP_LOGW(TAG, "notes: NVS write failed: %s", esp_err_to_name(err));
        }
        nvs_close(h);
    }
    free(blob);
}

void device_notes_init(void)
{
    if (s_notes_lock == NULL) {
        s_notes_lock = xSemaphoreCreateMutex();
    }
    if (s_notes == NULL) {
        s_notes = heap_caps_calloc(1, sizeof(*s_notes),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_notes == NULL) {
            s_notes = calloc(1, sizeof(*s_notes));
        }
    }
    if (s_notes == NULL) {
        ESP_LOGE(TAG, "notes: bank alloc failed");
        return;
    }
    jr_notes_init(s_notes);

    nvs_handle_t h;
    if (nvs_open(JR_NOTES_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        jr_notes_blob_t *blob = heap_caps_calloc(1, sizeof(*blob),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (blob == NULL) {
            blob = calloc(1, sizeof(*blob));
        }
        if (blob != NULL) {
            size_t len = sizeof(*blob);
            if (nvs_get_blob(h, JR_NOTES_NVS_KEY, blob, &len) == ESP_OK &&
                len == sizeof(*blob) &&
                blob->magic == JR_NOTES_BLOB_MAGIC &&
                blob->version == JR_NOTES_BLOB_VER &&
                blob->bank.count <= JR_NOTES_MAX) {
                *s_notes = blob->bank;
                for (unsigned i = 0U; i < s_notes->count; ++i) {
                    s_notes->items[i].text[JR_NOTE_TEXT_MAX] = '\0';
                }
                if (s_notes->next_id == 0U) {
                    s_notes->next_id = 1U;
                }
                ESP_LOGI(TAG, "notes: loaded %u note(s) from NVS",
                         (unsigned)s_notes->count);
            }
            free(blob);
        }
        nvs_close(h);
    }
}

uint32_t device_notes_add(const char *text)
{
    if (s_notes == NULL || s_notes_lock == NULL ||
        xSemaphoreTake(s_notes_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0U;
    }
    const uint32_t id = jr_notes_add(s_notes, text, device_wall_epoch_s());
    if (id != 0U) {
        notes_persist_locked();
    }
    xSemaphoreGive(s_notes_lock);
    return id;
}

bool device_notes_delete(uint32_t id)
{
    if (s_notes == NULL || s_notes_lock == NULL ||
        xSemaphoreTake(s_notes_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    const bool ok = jr_notes_delete(s_notes, id);
    if (ok) {
        notes_persist_locked();
    }
    xSemaphoreGive(s_notes_lock);
    return ok;
}

unsigned device_notes_clear(const char *target)
{
    if (s_notes == NULL || s_notes_lock == NULL ||
        xSemaphoreTake(s_notes_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0U;
    }
    const unsigned removed = jr_notes_clear(s_notes, target);
    if (removed > 0U) {
        notes_persist_locked();
    }
    xSemaphoreGive(s_notes_lock);
    return removed;
}

void device_notes_snapshot(jr_note_bank_t *out)
{
    if (out == NULL) {
        return;
    }
    jr_notes_init(out);
    if (s_notes == NULL || s_notes_lock == NULL ||
        xSemaphoreTake(s_notes_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    *out = *s_notes;
    xSemaphoreGive(s_notes_lock);
}

int device_notes_format_briefing(char *dst, size_t cap)
{
    if (dst == NULL || cap == 0U) {
        return 0;
    }
    dst[0] = '\0';
    if (s_notes == NULL || s_notes_lock == NULL ||
        xSemaphoreTake(s_notes_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }
    const int n = jr_notes_format_briefing(s_notes, dst, cap);
    xSemaphoreGive(s_notes_lock);
    return n;
}

/* ---- Chronograph stopwatch state + display publisher -------------------- */
static jr_stopwatch_t s_stopwatch;
static portMUX_TYPE s_stopwatch_mux = portMUX_INITIALIZER_UNLOCKED;

static void stopwatch_fill_snap_locked(uint32_t now_ms, stopwatch_snap_t *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->active = s_stopwatch.active;
    snap->running = s_stopwatch.running;
    snap->elapsed_ms = jr_stopwatch_elapsed_ms(&s_stopwatch, now_ms);
    snap->lap_count = s_stopwatch.lap_count;
    for (uint8_t i = 0U; i < s_stopwatch.lap_count && i < JR_STOPWATCH_MAX_LAPS; ++i) {
        snap->laps_ms[i] = s_stopwatch.laps_ms[i];
    }
}

void device_stopwatch_snapshot(uint32_t now_ms, stopwatch_snap_t *out_snap)
{
    if (out_snap == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_stopwatch_mux);
    stopwatch_fill_snap_locked(now_ms, out_snap);
    portEXIT_CRITICAL(&s_stopwatch_mux);
}

bool device_stopwatch_action(const char *action, uint32_t now_ms,
                             stopwatch_snap_t *out_snap)
{
    if (action == NULL || action[0] == '\0') {
        return false;
    }
    bool valid = true;
    bool mutated = false;
    stopwatch_snap_t snap = {0};

    portENTER_CRITICAL(&s_stopwatch_mux);
    if (strcasecmp(action, "start") == 0) {
        jr_stopwatch_start(&s_stopwatch, now_ms);
        mutated = true;
    } else if (strcasecmp(action, "stop") == 0 || strcasecmp(action, "pause") == 0) {
        jr_stopwatch_stop(&s_stopwatch, now_ms);
        mutated = true;
    } else if (strcasecmp(action, "lap") == 0) {
        (void)jr_stopwatch_lap(&s_stopwatch, now_ms, NULL);
        mutated = true;
    } else if (strcasecmp(action, "reset") == 0) {
        jr_stopwatch_reset(&s_stopwatch);
        mutated = true;
    } else if (strcasecmp(action, "status") == 0) {
        /* read-only */
    } else {
        valid = false;
    }
    if (valid) {
        stopwatch_fill_snap_locked(now_ms, &snap);
    }
    portEXIT_CRITICAL(&s_stopwatch_mux);

    if (!valid) {
        return false;
    }
    if (mutated) {
        jr_display_stopwatch_set(snap.active, snap.running,
                                 snap.elapsed_ms, snap.lap_count);
    }
    if (out_snap != NULL) {
        *out_snap = snap;
    }
    return true;
}

void device_stopwatch_tick(uint32_t now_ms)
{
    static uint32_t s_sw_next_pub_ms;
    stopwatch_snap_t snap = {0};
    portENTER_CRITICAL(&s_stopwatch_mux);
    const bool running = s_stopwatch.running;
    if (running && (int32_t)(now_ms - s_sw_next_pub_ms) >= 0) {
        stopwatch_fill_snap_locked(now_ms, &snap);
    }
    portEXIT_CRITICAL(&s_stopwatch_mux);
    if (running && (int32_t)(now_ms - s_sw_next_pub_ms) >= 0) {
        jr_display_stopwatch_set(snap.active, snap.running,
                                 snap.elapsed_ms, snap.lap_count);
        s_sw_next_pub_ms = now_ms + 100U;
    }
}

/* ---- CO5300 Sunlight Boost (HBM) policy & 5-minute auto-revert ---------- */
#define HBM_AUTO_REVERT_MS   300000U   /* 5 minutes */
#define HBM_MIN_BATTERY_PCT  25U       /* refused off USB below 25% */
#define HBM_MAX_CHIP_C       65.0f     /* refused above 65 C */

static _Atomic uint32_t s_hbm_expires_ms;

bool device_sunlight_set(bool enabled, uint32_t now_ms, const char **out_reason)
{
    if (out_reason != NULL) {
        *out_reason = "ok";
    }
    if (!enabled) {
        atomic_store(&s_hbm_expires_ms, 0U);
        jr_display_set_hbm(false);
        ESP_LOGI(TAG, "hbm: sunlight boost OFF");
        return true;
    }
    jr_power_t bat = {0};
    const bool have_bat = jr_power_read(&bat) == ESP_OK && bat.present;
    if (have_bat && !bat.usb_present && bat.percent < HBM_MIN_BATTERY_PCT) {
        if (out_reason != NULL) {
            *out_reason = "battery_low";
        }
        ESP_LOGW(TAG, "hbm: refused (battery %u%% off USB)", (unsigned)bat.percent);
        return false;
    }
    float chip_c = 0.0f;
    if (device_chip_temp_c(&chip_c) && chip_c > HBM_MAX_CHIP_C) {
        if (out_reason != NULL) {
            *out_reason = "thermal_high";
        }
        ESP_LOGW(TAG, "hbm: refused (chip %.1f C > 65 C)", (double)chip_c);
        return false;
    }
    uint32_t exp = now_ms + HBM_AUTO_REVERT_MS;
    if (exp == 0U) {
        exp = 1U;
    }
    atomic_store(&s_hbm_expires_ms, exp);
    jr_display_set_hbm(true);
    ESP_LOGI(TAG, "hbm: sunlight boost ON (auto-reverts in 5 min)");
    return true;
}

void device_sunlight_tick(uint32_t now_ms, const jr_power_t *bat, bool have_bat)
{
    if (!jr_display_get_hbm()) {
        return;
    }
    const uint32_t exp = atomic_load(&s_hbm_expires_ms);
    const char *why = NULL;
    if (exp != 0U && (int32_t)(now_ms - exp) >= 0) {
        why = "5-min timeout";
    } else if (have_bat && bat != NULL && bat->present &&
               !bat->usb_present && bat->percent < HBM_MIN_BATTERY_PCT) {
        why = "battery <25% off USB";
    } else {
        float chip_c = 0.0f;
        if (device_chip_temp_c(&chip_c) && chip_c > HBM_MAX_CHIP_C) {
            why = "chip >65 C";
        }
    }
    if (why != NULL) {
        atomic_store(&s_hbm_expires_ms, 0U);
        jr_display_set_hbm(false);
        ESP_LOGI(TAG, "hbm: auto-reverted (%s)", why);
    }
}

static bool json_get_bool_opt(const cJSON *item, bool *out)
{
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble != 0.0;
        return true;
    }
    return false;
}

static int format_stopwatch_json(char *out, size_t cap,
                                 const stopwatch_snap_t *snap)
{
    int n = snprintf(out, cap,
                     "{\"ok\":true,\"active\":%s,\"running\":%s,"
                     "\"elapsed_ms\":%u,\"laps\":[",
                     snap->active ? "true" : "false",
                     snap->running ? "true" : "false",
                     (unsigned)snap->elapsed_ms);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    for (uint8_t i = 0U; i < snap->lap_count && i < JR_STOPWATCH_MAX_LAPS; ++i) {
        int w = snprintf(out + n, cap - (size_t)n, "%s%u",
                         i == 0U ? "" : ",", (unsigned)snap->laps_ms[i]);
        if (w < 0 || (size_t)n + (size_t)w >= cap) {
            return -1;
        }
        n += w;
    }
    int tail = snprintf(out + n, cap - (size_t)n, "]}");
    if (tail < 0 || (size_t)n + (size_t)tail >= cap) {
        return -1;
    }
    return n + tail;
}

bool device_try_local_tool(const jr_command_t *cmd)
{
    if (cmd == NULL || cmd->tool_name == NULL) {
        return false;
    }
    const bool weather      = strcmp(cmd->tool_name, "weather_here") == 0;
    const bool clock_tool   = strcmp(cmd->tool_name, "current_time") == 0 &&
                              !device_gateway_usable();
    const bool set_timer    = strcmp(cmd->tool_name, "set_timer") == 0;
    const bool cancel_timer = strcmp(cmd->tool_name, "cancel_timer") == 0;
    const bool list_timers  = strcmp(cmd->tool_name, "list_timers") == 0;
    const bool set_glass    = strcmp(cmd->tool_name, "set_glass") == 0;
    const bool dev_status   = strcmp(cmd->tool_name, "device_status") == 0;
    const bool start_focus  = strcmp(cmd->tool_name, "start_focus") == 0;
    const bool sw_tool      = strcmp(cmd->tool_name, "stopwatch") == 0;
    const bool add_note     = strcmp(cmd->tool_name, "add_note") == 0;
    const bool list_notes   = strcmp(cmd->tool_name, "list_notes") == 0;
    const bool clear_notes  = strcmp(cmd->tool_name, "clear_notes") == 0;
    const bool briefing     = strcmp(cmd->tool_name, "daily_briefing") == 0;
    const bool ui_sounds    = strcmp(cmd->tool_name, "set_ui_sounds") == 0;
    const bool sunlight     = strcmp(cmd->tool_name, "set_sunlight") == 0;
    if (!weather && !clock_tool && !set_timer && !cancel_timer &&
        !list_timers && !set_glass && !dev_status && !start_focus &&
        !sw_tool && !add_note && !list_notes && !clear_notes &&
        !briefing && !ui_sounds && !sunlight) {
        return false;
    }
    local_tool_result_t *slot = device_tool_queue_slot(
        cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
        JR_TOOL_STATUS_OK);
    if (slot == NULL) {
        return true;                      /* counted as a failure; handled */
    }
    char *out = slot->response_json;
    const size_t cap = sizeof(slot->response_json);
    const uint32_t now_ms = (uint32_t)jr_clock_now_ms(&s_app.clock);

    if (weather) {
        if (!s_weather.valid) {
            slot->status = JR_TOOL_STATUS_NETWORK_ERROR;
            (void)snprintf(out, cap,
                "{\"error\":{\"code\":\"weather_unavailable\",\"message\":"
                "\"The device has no weather yet; it fetches on a timer.\"}}");
        } else {
            const uint32_t age_ms = now_ms - s_weather.fetched_ms;
            (void)snprintf(out, cap,
                "{\"place\":\"%s\",\"temp_f\":%d,\"feels_f\":%d,"
                "\"condition\":\"%s\",\"high_f\":%d,\"low_f\":%d,"
                "\"humidity_pct\":%u,\"wind_mph\":%u,\"age_min\":%u}",
                JR_WEATHER_PLACE, (int)s_weather.temp_f, (int)s_weather.feels_f,
                s_weather.condition, (int)s_weather.hi_f, (int)s_weather.lo_f,
                (unsigned)s_weather.humidity_pct, (unsigned)s_weather.wind_mph,
                (unsigned)(age_ms / 60000U));
        }
    } else if (clock_tool) {
        time_t tt = time(NULL);
        struct tm tmv;
        localtime_r(&tt, &tmv);
        if (tmv.tm_year < 2020 - 1900) {
            slot->status = JR_TOOL_STATUS_INTERNAL_ERROR;
            (void)snprintf(out, cap,
                "{\"error\":{\"code\":\"clock_unset\",\"message\":"
                "\"The device clock has not been set yet.\"}}");
        } else {
            char when[64];
            strftime(when, sizeof when, "%A %B %d, %I:%M %p", &tmv);
            (void)snprintf(out, cap, "{\"local_time\":\"%s\"}", when);
        }
    } else if (set_timer) {
        cJSON *args = cJSON_Parse(cmd->tool_args ? cmd->tool_args : "{}");
        const cJSON *sec_item = cJSON_GetObjectItemCaseSensitive(args, "seconds");
        const cJSON *lbl_item = cJSON_GetObjectItemCaseSensitive(args, "label");
        uint32_t seconds = 0U;
        bool bad_sec = false;
        if (sec_item != NULL && !cJSON_IsNull(sec_item)) {
            if (!cJSON_IsNumber(sec_item) || sec_item->valuedouble < 0.0 ||
                sec_item->valuedouble > (double)JR_TIMER_MAX_S) {
                bad_sec = true;
            } else {
                seconds = (uint32_t)lrint(sec_item->valuedouble);
            }
        }
        const char *label = (cJSON_IsString(lbl_item) && lbl_item->valuestring)
            ? lbl_item->valuestring : NULL;
        char norm[JR_TIMER_LABEL_CAP] = {0};
        uint32_t eff_s = 0U;
        if (!bad_sec &&
            jr_timer_set(&s_timers, now_ms, seconds, label,
                         norm, sizeof norm, &eff_s)) {
            timer_publish_now(now_ms);
            jr_mood_poke_awake(&s_mood, now_ms);
            char cap_txt[32];
            if (eff_s >= 3600U) {
                snprintf(cap_txt, sizeof cap_txt, "%.14s %uH%02uM",
                         norm, (unsigned)(eff_s / 3600U),
                         (unsigned)((eff_s % 3600U) / 60U));
            } else {
                snprintf(cap_txt, sizeof cap_txt, "%.14s %u:%02u",
                         norm, (unsigned)(eff_s / 60U), (unsigned)(eff_s % 60U));
            }
            jr_display_caption_set(cap_txt);
            (void)snprintf(out, cap,
                "{\"ok\":true,\"label\":\"%s\",\"seconds\":%u,\"active_timers\":%u}",
                norm, (unsigned)eff_s, (unsigned)jr_timer_active_count(&s_timers));
            ESP_LOGI(TAG, "local tool=set_timer label=%s sec=%u", norm, (unsigned)eff_s);
        } else {
            slot->status = JR_TOOL_STATUS_INVALID_ARGS;
            const bool full = jr_timer_active_count(&s_timers) >= JR_TIMER_MAX;
            (void)snprintf(out, cap,
                "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                full ? "timer_bank_full" : "invalid_timer",
                full ? "All 4 timer slots are already in use."
                     : "Duration must be 5 to 43200 seconds (or label focus/break).");
        }
        cJSON_Delete(args);
    } else if (cancel_timer) {
        cJSON *args = cJSON_Parse(cmd->tool_args ? cmd->tool_args : "{}");
        const cJSON *lbl_item = cJSON_GetObjectItemCaseSensitive(args, "label");
        const char *label = (cJSON_IsString(lbl_item) && lbl_item->valuestring)
            ? lbl_item->valuestring : NULL;
        const unsigned n = jr_timer_cancel(&s_timers, now_ms, label);
        if (n > 0U) {
            timer_publish_now(now_ms);
            jr_display_caption_set("TIMER CANCELLED");
        }
        (void)snprintf(out, cap,
            "{\"ok\":true,\"cancelled\":%u,\"active_timers\":%u}",
            n, (unsigned)jr_timer_active_count(&s_timers));
        ESP_LOGI(TAG, "local tool=cancel_timer cancelled=%u", n);
        cJSON_Delete(args);
    } else if (list_timers) {
        int n = snprintf(out, cap, "{\"count\":%u,\"timers\":[",
                         (unsigned)jr_timer_active_count(&s_timers));
        bool first = true;
        for (unsigned i = 0U; i < JR_TIMER_MAX && n > 0 && (size_t)n < cap - 24U; ++i) {
            const jr_timer_slot_t *s = &s_timers.slots[i];
            if (s->state == JR_TIMER_IDLE) {
                continue;
            }
            const bool ringing = (s->state == JR_TIMER_RINGING);
            const uint16_t rem_s = ringing ? 0U : jr_timer_slot_rem_s(s, now_ms);
            n += snprintf(out + n, cap - (size_t)n,
                "%s{\"label\":\"%s\",\"rem_s\":%u,\"ringing\":%s}",
                first ? "" : ",", s->label, (unsigned)rem_s,
                ringing ? "true" : "false");
            first = false;
        }
        if (n > 0 && (size_t)n + 3U < cap) {
            snprintf(out + n, cap - (size_t)n, "]}");
        }
    } else if (set_glass) {
        /* STRICT INVARIANT: set_glass switches ring screen and/or watch style
         * only. It NEVER touches privacy/mute or operator lease state. */
        cJSON *args = cJSON_Parse(cmd->tool_args ? cmd->tool_args : "{}");
        const cJSON *scr_item = cJSON_GetObjectItemCaseSensitive(args, "screen");
        const cJSON *ws_item  = cJSON_GetObjectItemCaseSensitive(args, "watch_style");
        const bool has_scr = cJSON_IsString(scr_item) && scr_item->valuestring &&
                             scr_item->valuestring[0] != '\0';
        const bool has_ws  = cJSON_IsString(ws_item) && ws_item->valuestring &&
                             ws_item->valuestring[0] != '\0';
        jr_display_space_t sp = jr_display_nav_space();
        jr_watch_style_t ws = jr_display_watch_style();
        const bool ok_scr = !has_scr || device_parse_glass_screen(scr_item->valuestring, &sp);
        const bool ok_ws  = !has_ws  || device_parse_watch_style(ws_item->valuestring, &ws);
        if ((!has_scr && !has_ws) || !ok_scr || !ok_ws) {
            slot->status = JR_TOOL_STATUS_INVALID_ARGS;
            (void)snprintf(out, cap,
                "{\"error\":{\"code\":\"invalid_glass\",\"message\":"
                "\"Provide screen (jarvis|watch|weather|status|activity) "
                "and/or watch_style (jarvis|diver|dress|pilot|minimal|future|skeleton|celestial|orrery|reactor).\"}}");
        } else {
            if (has_ws) {
                jr_display_watch_style_set(ws);
                persist_watch_style((uint8_t)ws);
            }
            if (has_scr) {
                jr_display_nav_set(sp);
            }
            jr_mood_poke_awake(&s_mood, now_ms);
            jr_display_bloom();
            device_confirm_cue();
            if (has_ws && !has_scr) {
                char cap_txt[28];
                snprintf(cap_txt, sizeof cap_txt, "WATCH - %s",
                         jr_display_watch_style_name(ws));
                jr_display_caption_label(cap_txt, JR_DISPLAY_LABEL_WATCH_STYLE);
            } else if (has_scr) {
                char cap_txt[16];
                const char *sn = device_glass_space_name(sp);
                size_t k = 0U;
                for (; sn[k] != '\0' && k + 1U < sizeof cap_txt; ++k) {
                    cap_txt[k] = (char)toupper((unsigned char)sn[k]);
                }
                cap_txt[k] = '\0';
                jr_display_caption_label(cap_txt, JR_DISPLAY_LABEL_SCREEN);
            }
            (void)snprintf(out, cap,
                "{\"ok\":true,\"screen\":\"%s\",\"watch_style\":\"%s\"}",
                device_glass_space_name(jr_display_nav_space()),
                jr_display_watch_style_name(jr_display_watch_style()));
            ESP_LOGI(TAG, "local tool=set_glass screen=%s watch_style=%s",
                     device_glass_space_name(jr_display_nav_space()),
                     jr_display_watch_style_name(jr_display_watch_style()));
        }
        cJSON_Delete(args);
    } else if (dev_status) {
        jr_power_t bat = {0};
        const bool have_bat = jr_power_read(&bat) == ESP_OK && bat.present;
        jr_net_status_t net = {0};
        const bool have_net = jr_net_get_status(&net) == ESP_OK && net.sta_connected;
        float temp_c = 0.0f;
        const bool have_temp = device_chip_temp_c(&temp_c);
        const int batt_pct = (have_bat && bat.percent <= 100U) ? (int)bat.percent : -1;
        const bool muted = atomic_load(&s_voice_privacy_paused) || s_flip_muted;
        (void)snprintf(out, cap,
            "{\"battery_pct\":%d,\"charging\":%s,\"usb\":%s,"
            "\"wifi_rssi\":%d,\"chip_temp_c\":%d,\"volume\":%d,"
            "\"brightness\":%u,\"watch_style\":\"%s\",\"screen\":\"%s\","
            "\"muted\":%s,\"hbm\":%s,\"holo\":%s,\"ui_sounds\":%s,"
            "\"uptime_s\":%u,\"active_timers\":%u}",
            batt_pct,
            (have_bat && bat.charging) ? "true" : "false",
            (have_bat && bat.usb_present) ? "true" : "false",
            have_net ? (int)net.rssi : 0,
            have_temp ? (int)lroundf(temp_c) : 0,
            s_out_vol,
            (unsigned)s_brightness_cap,
            jr_display_watch_style_name(jr_display_watch_style()),
            device_glass_space_name(jr_display_nav_space()),
            muted ? "true" : "false",
            jr_display_get_hbm() ? "true" : "false",
            jr_display_holo_get() ? "true" : "false",
            atomic_load(&s_ui_sounds) ? "true" : "false",
            (unsigned)(now_ms / 1000U),
            (unsigned)jr_timer_active_count(&s_timers));
    } else if (start_focus) {
        cJSON *args = cJSON_Parse(cmd->tool_args ? cmd->tool_args : "{}");
        const cJSON *min_item = cJSON_GetObjectItemCaseSensitive(args, "minutes");
        const cJSON *lbl_item = cJSON_GetObjectItemCaseSensitive(args, "label");
        uint32_t minutes = 25U;
        bool bad_min = false;
        if (min_item != NULL && !cJSON_IsNull(min_item)) {
            if (!cJSON_IsNumber(min_item) || min_item->valuedouble < 1.0 ||
                min_item->valuedouble > 720.0) {
                bad_min = true;
            } else {
                minutes = (uint32_t)lrint(min_item->valuedouble);
            }
        }
        const char *label = (cJSON_IsString(lbl_item) && lbl_item->valuestring &&
                             lbl_item->valuestring[0] != '\0')
            ? lbl_item->valuestring : "FOCUS";
        char norm[JR_TIMER_LABEL_CAP] = {0};
        uint32_t eff_s = 0U;
        if (!bad_min &&
            jr_timer_set(&s_timers, now_ms, minutes * 60U, label,
                         norm, sizeof norm, &eff_s)) {
            jr_display_nav_set(JR_DISPLAY_SPACE_WATCH);
            timer_publish_now(now_ms);
            jr_mood_poke_awake(&s_mood, now_ms);
            jr_display_bloom();
            device_confirm_cue();
            char cap_txt[32];
            snprintf(cap_txt, sizeof cap_txt, "%.14s %u:00", norm, (unsigned)(eff_s / 60U));
            jr_display_caption_set(cap_txt);
            (void)snprintf(out, cap,
                "{\"ok\":true,\"label\":\"%s\",\"minutes\":%u,\"seconds\":%u,\"screen\":\"watch\"}",
                norm, (unsigned)(eff_s / 60U), (unsigned)eff_s);
            ESP_LOGI(TAG, "local tool=start_focus label=%s min=%u", norm, (unsigned)(eff_s / 60U));
        } else {
            slot->status = JR_TOOL_STATUS_INVALID_ARGS;
            const bool full = jr_timer_active_count(&s_timers) >= JR_TIMER_MAX;
            (void)snprintf(out, cap,
                "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                full ? "timer_bank_full" : "invalid_focus",
                full ? "All 4 timer slots are already in use."
                     : "Focus minutes must be between 1 and 720.");
        }
        cJSON_Delete(args);
    } else if (sw_tool) {
        cJSON *args = cJSON_Parse(cmd->tool_args ? cmd->tool_args : "{}");
        const cJSON *act_item = cJSON_GetObjectItemCaseSensitive(args, "action");
        const char *action = (cJSON_IsString(act_item) && act_item->valuestring &&
                              act_item->valuestring[0] != '\0')
            ? act_item->valuestring : "status";
        stopwatch_snap_t snap = {0};
        if (device_stopwatch_action(action, now_ms, &snap)) {
            jr_mood_poke_awake(&s_mood, now_ms);
            if (strcasecmp(action, "status") != 0) {
                device_confirm_cue();
            }
            if (format_stopwatch_json(out, cap, &snap) < 0) {
                (void)snprintf(out, cap, "{\"ok\":true,\"active\":%s,\"running\":%s,\"elapsed_ms\":%u}",
                               snap.active ? "true" : "false",
                               snap.running ? "true" : "false",
                               (unsigned)snap.elapsed_ms);
            }
            ESP_LOGI(TAG, "local tool=stopwatch action=%s elapsed_ms=%u laps=%u",
                     action, (unsigned)snap.elapsed_ms, (unsigned)snap.lap_count);
        } else {
            slot->status = JR_TOOL_STATUS_INVALID_ARGS;
            (void)snprintf(out, cap,
                "{\"error\":{\"code\":\"invalid_action\",\"message\":"
                "\"Use start, stop, lap, reset, or status.\"}}");
        }
        cJSON_Delete(args);
    } else if (add_note) {
        cJSON *args = cJSON_Parse(cmd->tool_args ? cmd->tool_args : "{}");
        const cJSON *txt_item = cJSON_GetObjectItemCaseSensitive(args, "text");
        const char *txt = (cJSON_IsString(txt_item) && txt_item->valuestring)
            ? txt_item->valuestring : NULL;
        const uint32_t id = device_notes_add(txt);
        if (id != 0U) {
            jr_note_bank_t bank;
            device_notes_snapshot(&bank);
            jr_mood_poke_awake(&s_mood, now_ms);
            device_confirm_cue();
            jr_display_caption_set("NOTE PINNED");
            (void)snprintf(out, cap,
                "{\"ok\":true,\"id\":%u,\"count\":%u,\"max\":%u}",
                (unsigned)id, (unsigned)bank.count, (unsigned)JR_NOTES_MAX);
            ESP_LOGI(TAG, "local tool=add_note id=%u count=%u",
                     (unsigned)id, (unsigned)bank.count);
        } else {
            jr_note_bank_t bank;
            device_notes_snapshot(&bank);
            slot->status = JR_TOOL_STATUS_INVALID_ARGS;
            const bool full = bank.count >= JR_NOTES_MAX;
            (void)snprintf(out, cap,
                "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                full ? "notes_full" : "invalid_note",
                full ? "All 6 desk note slots are full; clear one first."
                     : "Provide non-empty note text (up to 64 chars).");
        }
        cJSON_Delete(args);
    } else if (list_notes) {
        jr_note_bank_t bank;
        device_notes_snapshot(&bank);
        int n = snprintf(out, cap, "{\"count\":%u,\"max\":%u,\"notes\":[",
                         (unsigned)bank.count, (unsigned)JR_NOTES_MAX);
        for (unsigned i = 0U; i < bank.count && n > 0 && (size_t)n < cap - 24U; ++i) {
            int w = snprintf(out + n, cap - (size_t)n,
                             "%s{\"id\":%u,\"text\":\"%s\"}",
                             i == 0U ? "" : ",",
                             (unsigned)bank.items[i].id, bank.items[i].text);
            if (w < 0 || (size_t)n + (size_t)w >= cap - 4U) {
                break;
            }
            n += w;
        }
        if (n > 0 && (size_t)n + 3U < cap) {
            snprintf(out + n, cap - (size_t)n, "]}");
        }
    } else if (clear_notes) {
        cJSON *args = cJSON_Parse(cmd->tool_args ? cmd->tool_args : "{}");
        const cJSON *tgt_item = cJSON_GetObjectItemCaseSensitive(args, "target");
        const char *target = (cJSON_IsString(tgt_item) && tgt_item->valuestring)
            ? tgt_item->valuestring : "all";
        const unsigned removed = device_notes_clear(target);
        jr_note_bank_t bank;
        device_notes_snapshot(&bank);
        if (removed > 0U) {
            jr_mood_poke_awake(&s_mood, now_ms);
            device_confirm_cue();
            jr_display_caption_set(bank.count == 0U ? "NOTES CLEARED" : "NOTE REMOVED");
        }
        (void)snprintf(out, cap,
            "{\"ok\":true,\"removed\":%u,\"remaining\":%u}",
            removed, (unsigned)bank.count);
        ESP_LOGI(TAG, "local tool=clear_notes removed=%u remaining=%u",
                 removed, (unsigned)bank.count);
        cJSON_Delete(args);
    } else if (briefing) {
        jr_power_t bat = {0};
        const bool have_bat = jr_power_read(&bat) == ESP_OK && bat.present;
        char summary[480];
        (void)briefing_compose_live(summary, sizeof summary,
                                    have_bat ? &bat : NULL);
        /* Sanitize any quotes/backslashes so summary embeds safely in JSON. */
        for (char *p = summary; *p != '\0'; ++p) {
            if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20U) {
                *p = ' ';
            }
        }
        jr_mood_poke_awake(&s_mood, now_ms);
        jr_display_caption_set("DAILY BRIEFING");
        (void)snprintf(out, cap, "{\"ok\":true,\"briefing\":\"%s\"}", summary);
        ESP_LOGI(TAG, "local tool=daily_briefing (%u chars)",
                 (unsigned)strlen(summary));
    } else if (ui_sounds) {
        cJSON *args = cJSON_Parse(cmd->tool_args ? cmd->tool_args : "{}");
        const cJSON *en_item = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        bool enabled = false;
        if (json_get_bool_opt(en_item, &enabled)) {
            ui_sounds_set(enabled);
            jr_mood_poke_awake(&s_mood, now_ms);
            if (enabled) {
                device_confirm_cue();
            }
            jr_display_caption_set(enabled ? "UI SOUNDS ON" : "UI SOUNDS OFF");
            (void)snprintf(out, cap, "{\"ok\":true,\"ui_sounds\":%s}",
                           enabled ? "true" : "false");
            ESP_LOGI(TAG, "local tool=set_ui_sounds enabled=%d", enabled ? 1 : 0);
        } else {
            slot->status = JR_TOOL_STATUS_INVALID_ARGS;
            (void)snprintf(out, cap,
                "{\"error\":{\"code\":\"invalid_arg\",\"message\":"
                "\"Provide boolean enabled (true or false).\"}}");
        }
        cJSON_Delete(args);
    } else if (sunlight) {
        cJSON *args = cJSON_Parse(cmd->tool_args ? cmd->tool_args : "{}");
        const cJSON *en_item = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        bool enabled = false;
        if (!json_get_bool_opt(en_item, &enabled)) {
            slot->status = JR_TOOL_STATUS_INVALID_ARGS;
            (void)snprintf(out, cap,
                "{\"error\":{\"code\":\"invalid_arg\",\"message\":"
                "\"Provide boolean enabled (true or false).\"}}");
        } else {
            const char *reason = "ok";
            if (device_sunlight_set(enabled, now_ms, &reason)) {
                jr_mood_poke_awake(&s_mood, now_ms);
                device_confirm_cue();
                jr_display_caption_set(enabled ? "SUNLIGHT BOOST" : "SUNLIGHT OFF");
                (void)snprintf(out, cap,
                    "{\"ok\":true,\"hbm\":%s,\"auto_revert_min\":%u}",
                    enabled ? "true" : "false", enabled ? 5U : 0U);
            } else {
                slot->status = JR_TOOL_STATUS_INVALID_ARGS;
                (void)snprintf(out, cap,
                    "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                    reason,
                    strcmp(reason, "thermal_high") == 0
                        ? "Sunlight boost refused: chip temperature is above 65 C."
                        : "Sunlight boost refused: battery is below 25 percent off USB.");
            }
        }
        cJSON_Delete(args);
    }
    return true;
}

void device_tool_queue_local_error(const jr_command_t *cmd,
                                          esp_err_t submit_error)
{
    const char *code = "worker_unavailable";
    const char *message = "On-device tool worker is unavailable";
    jr_tool_status_t status = JR_TOOL_STATUS_INTERNAL_ERROR;
    if (submit_error == ESP_ERR_TIMEOUT) {
        code = "worker_busy";
        message = "On-device tool queue is busy";
    } else if (submit_error == ESP_ERR_INVALID_ARG ||
               submit_error == ESP_ERR_INVALID_SIZE) {
        code = "invalid_call";
        message = "Tool call metadata is invalid";
        status = JR_TOOL_STATUS_INVALID_ARGS;
    }
    device_tool_queue_error(cmd->call_id, cmd->call_id_text, cmd->tool_name,
                            cmd->session_gen, status, code, message);
}

/* Runs only on the single voice owner task, after the orchestrator has
 * completed its current fixpoint. This avoids recursively injecting a result
 * while DispatchToolCall's command list is still being executed. */
void device_tool_drain_results(jr_app_t *a, uint64_t now)
{
    while (s_local_tool_count > 0U) {
        local_tool_result_t result =
            s_local_tool_results[s_local_tool_head];
        memset(&s_local_tool_results[s_local_tool_head], 0,
               sizeof(s_local_tool_results[s_local_tool_head]));
        s_local_tool_head = (s_local_tool_head + 1U) % LOCAL_TOOL_RESULT_CAP;
        s_local_tool_count--;

        device_tool_record_result(result.name, result.status, 0U, 0);
        if (jr_session_gen_is_stale(a->orch.session.session_gen,
                                    result.session_gen)) {
            atomic_fetch_add(&s_tool_diag.stale_dropped, 1U);
            memset(&result, 0, sizeof(result));
            continue;
        }
        jr_event_t event = jr_event(JR_EV_TOOL_RESULT_READY);
        event.call_id = result.call_id;
        event.call_id_text = result.call_id_text;
        event.tool_name = result.name;
        event.tool_response = result.response_json;
        event.session_gen = result.session_gen;
        jr_orch_inject(&a->orch, event, now);
        memset(&result, 0, sizeof(result));
    }

    if (s_tool_poll_result == NULL) {
        return;
    }
    while (jr_tools_poll(s_tool_poll_result)) {
        jr_tool_result_t *result = s_tool_poll_result;
        if (result->call_id == LOCAL_WEATHER_CALL_ID) {
            weather_apply_result(result, (uint32_t)now);
            memset(result, 0, sizeof(*result));
            continue;
        }
        if (result->call_id == LOCAL_BOARD_CALL_ID) {
            device_gateway_note_result(result->status);
            board_apply_result(result);
            memset(result, 0, sizeof(*result));
            continue;
        }
        device_tool_record_result(result->name, result->status, result->duration_ms,
                                  result->http_status);
        device_gateway_note_result(result->status);
        if (result->status == JR_TOOL_STATUS_CANCELLED) {
            atomic_fetch_add(&s_tool_diag.cancelled, 1U);
            memset(result, 0, sizeof(*result));
            continue;
        }
        if (result->status == JR_TOOL_STATUS_STALE ||
            jr_session_gen_is_stale(a->orch.session.session_gen,
                                    result->session_gen)) {
            atomic_fetch_add(&s_tool_diag.stale_dropped, 1U);
            memset(result, 0, sizeof(*result));
            continue;
        }

        jr_event_t event = jr_event(JR_EV_TOOL_RESULT_READY);
        event.call_id = result->call_id;
        event.call_id_text = result->call_id_text;
        event.tool_name = result->name;
        event.tool_response = result->response_json;
        event.session_gen = result->session_gen;
        jr_orch_inject(&a->orch, event, now);
        /* jr_orch_inject synchronously builds/sends the response before it
         * returns, so the owned buffer can be wiped immediately. */
        memset(result, 0, sizeof(*result));
    }
}


/* s_brain_lock must be held. The panel state and the pending authority are
 * cleared in the same transaction, so no remote writer can slip a replacement
 * card between a physical tap and submission. */
void device_tool_resolve_consent_locked(tool_consent_outcome_t outcome)
{
    if (!s_tool_consent.active || !s_brain_surface.local_owned) {
        return;
    }

    if (outcome == TOOL_CONSENT_ALLOW) {
        jr_tool_job_t job = {
            .call_id = s_tool_consent.call_id,
            .call_id_text = s_tool_consent.call_id_text,
            .name = s_tool_consent.name,
            .args_json = s_tool_consent.args_json,
            .session_gen = s_tool_consent.session_gen,
            .physical_confirmed = true,
        };
        esp_err_t submitted = atomic_load(&s_tool_diag.worker_ready)
            ? jr_tools_submit(&job) : ESP_ERR_INVALID_STATE;
        atomic_fetch_add(&s_tool_diag.consent_approved, 1U);
        if (submitted == ESP_OK) {
            atomic_fetch_add(&s_tool_diag.submitted, 1U);
        } else {
            jr_command_t failed = {
                .call_id = s_tool_consent.call_id,
                .call_id_text = s_tool_consent.call_id_text,
                .tool_name = s_tool_consent.name,
                .session_gen = s_tool_consent.session_gen,
            };
            atomic_fetch_add(&s_tool_diag.submit_rejected, 1U);
            device_tool_queue_local_error(&failed, submitted);
        }
    } else if (outcome == TOOL_CONSENT_DENY ||
               outcome == TOOL_CONSENT_TIMEOUT) {
        const bool timed_out = outcome == TOOL_CONSENT_TIMEOUT;
        device_tool_queue_error(
            s_tool_consent.call_id, s_tool_consent.call_id_text,
            s_tool_consent.name, s_tool_consent.session_gen,
            JR_TOOL_STATUS_CANCELLED,
            timed_out ? "consent_timeout" : "consent_denied",
            timed_out ? "Physical confirmation timed out"
                      : "User denied physical confirmation");
        atomic_fetch_add(&s_tool_diag.consent_denied, 1U);
        if (timed_out) {
            atomic_fetch_add(&s_tool_diag.consent_timed_out, 1U);
        }
    } else {
        /* Gemini cancelled the function or the session closed. Denial is the
         * safe outcome, but a cancelled Gemini call must not receive a late
         * toolResponse for an id the server has already withdrawn. */
        atomic_fetch_add(&s_tool_diag.consent_cancelled, 1U);
    }

    jr_display_surface_dismiss();
    s_brain_surface.active = false;
    s_brain_surface.local_owned = false;
    secure_zero(&s_tool_consent, sizeof(s_tool_consent));
    atomic_store(&s_tool_diag.consent_active, false);
    configASSERT(!jr_display_surface_is_active());
}

bool device_tool_present_consent(const jr_command_t *cmd, uint32_t now)
{
    char note[JR_DISPLAY_SURFACE_BODY_CAP] = {0};
    const char *parse_end = NULL;
    cJSON *args = cJSON_ParseWithOpts(cmd->tool_args ? cmd->tool_args : "{}",
                                     &parse_end, true);
    cJSON *note_item = cJSON_GetObjectItemCaseSensitive(args, "note");
    bool note_valid = cJSON_IsObject(args) && args->child == note_item &&
        note_item != NULL && note_item->next == NULL &&
        cJSON_IsString(note_item) && note_item->valuestring != NULL &&
        strnlen(note_item->valuestring, 48U) < 48U &&
        brain_render_text_safe(note_item->valuestring,
                               JR_DISPLAY_SURFACE_BODY_CAP, true);
    if (note_valid) {
        strlcpy(note, note_item->valuestring, sizeof(note));
    }
    if (cJSON_IsString(note_item) && note_item->valuestring != NULL) {
        secure_zero(note_item->valuestring, strlen(note_item->valuestring));
    }
    cJSON_Delete(args);
    if (!note_valid) {
        device_tool_queue_error(
            cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
            JR_TOOL_STATUS_INVALID_ARGS, "consent_unrenderable",
            "Note must fit exactly on the physical confirmation panel");
        secure_zero(note, sizeof(note));
        return true;
    }
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        device_tool_queue_error(
            cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
            JR_TOOL_STATUS_INTERNAL_ERROR, "consent_unavailable",
            "Physical confirmation is unavailable");
        secure_zero(note, sizeof(note));
        return true;
    }
    if (s_tool_consent.active || s_brain_surface.local_owned) {
        xSemaphoreGive(s_brain_lock);
        device_tool_queue_error(
            cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
            JR_TOOL_STATUS_CANCELLED, "consent_busy",
            "Another physical confirmation is pending");
        secure_zero(note, sizeof(note));
        return true;
    }

    /* A deterministic test frame suppresses the ordinary surface compositor,
     * and the touch challenge consumes taps before cards. A security prompt
     * must therefore take exclusive local ownership of both lanes. */
    atomic_store(&s_touch_challenge_start_requested, false);
    atomic_store(&s_touch_challenge_cancel_requested, false);
    atomic_store(&s_touch_challenge_active, false);
    atomic_store(&s_touch_challenge_restore_ms, 0U);
    s_ui_shade_open = false;
    if (jr_display_set_test_pattern(JR_DISPLAY_TEST_OFF) != ESP_OK) {
        xSemaphoreGive(s_brain_lock);
        device_tool_queue_error(
            cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
            JR_TOOL_STATUS_INTERNAL_ERROR, "consent_unavailable",
            "Physical confirmation display is unavailable");
        secure_zero(note, sizeof(note));
        return true;
    }

    /* Security prompts outrank optional Desk cards. Preemption is atomic under
     * the same Brain -> display lock order used by remote present/dismiss. */
    if (s_brain_surface.active || jr_display_surface_is_active()) {
        jr_display_surface_dismiss();
        s_brain_surface.active = false;
    }
    brain_surface_state_t surface = {0};
    surface.active = true;
    surface.local_owned = true;
    surface.expires_ms = now + TOOL_CONSENT_TIMEOUT_MS;
    surface.view.kind = JR_DISPLAY_SURFACE_CONSENT;
    surface.view.action_count = 2U;
    strlcpy(surface.session, "device", sizeof(surface.session));
    strlcpy(surface.id, "memory-consent", sizeof(surface.id));
    strlcpy(surface.view.title, "SAVE MEMORY?", sizeof(surface.view.title));
    strlcpy(surface.view.body, note, sizeof(surface.view.body));
    strlcpy(surface.action_ids[0], "deny", sizeof(surface.action_ids[0]));
    strlcpy(surface.action_ids[1], "allow", sizeof(surface.action_ids[1]));
    strlcpy(surface.view.action_labels[0], "DENY",
            sizeof(surface.view.action_labels[0]));
    strlcpy(surface.view.action_labels[1], "ALLOW",
            sizeof(surface.view.action_labels[1]));

    esp_err_t shown = jr_display_surface_present(&surface.view);
    if (shown != ESP_OK) {
        xSemaphoreGive(s_brain_lock);
        device_tool_queue_error(
            cmd->call_id, cmd->call_id_text, cmd->tool_name, cmd->session_gen,
            JR_TOOL_STATUS_INTERNAL_ERROR, "consent_unavailable",
            "Physical confirmation could not be displayed");
        secure_zero(note, sizeof(note));
        secure_zero(&surface, sizeof(surface));
        return true;
    }

    secure_zero(&s_tool_consent, sizeof(s_tool_consent));
    s_tool_consent.active = true;
    s_tool_consent.call_id = cmd->call_id;
    s_tool_consent.session_gen = cmd->session_gen;
    s_tool_consent.expires_ms = surface.expires_ms;
    s_tool_consent.presented_ms = now;
    strlcpy(s_tool_consent.call_id_text,
            cmd->call_id_text ? cmd->call_id_text : "",
            sizeof(s_tool_consent.call_id_text));
    strlcpy(s_tool_consent.name, cmd->tool_name ? cmd->tool_name : "remember",
            sizeof(s_tool_consent.name));
    strlcpy(s_tool_consent.args_json, cmd->tool_args ? cmd->tool_args : "{}",
            sizeof(s_tool_consent.args_json));
    s_brain_surface = surface;
    secure_zero(note, sizeof(note));
    secure_zero(&surface, sizeof(surface));
    atomic_store(&s_tool_diag.consent_active, true);
    atomic_fetch_add(&s_tool_diag.consent_prompted, 1U);
    configASSERT(s_brain_surface.active == jr_display_surface_is_active());
    xSemaphoreGive(s_brain_lock);
    return true;
}

bool device_tool_cancel_pending_consent(uint32_t call_id,
                                               const char *call_id_text)
{
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bool matches = false;
    if (s_tool_consent.active) {
        if (call_id_text != NULL && call_id_text[0] != '\0' &&
            s_tool_consent.call_id_text[0] != '\0') {
            matches = strcmp(call_id_text, s_tool_consent.call_id_text) == 0;
        } else {
            matches = call_id != 0U && call_id == s_tool_consent.call_id;
        }
    }
    if (matches) {
        device_tool_resolve_consent_locked(TOOL_CONSENT_CANCEL);
    }
    xSemaphoreGive(s_brain_lock);
    return matches;
}

void device_tool_abort_pending_consent(void)
{
    if (s_brain_lock != NULL &&
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_tool_consent.active) {
            device_tool_resolve_consent_locked(TOOL_CONSENT_CANCEL);
        }
        xSemaphoreGive(s_brain_lock);
    }
}
