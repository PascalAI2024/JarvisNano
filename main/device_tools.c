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
 * would drag Wi-Fi out of min-modem during rest. */
#define LOCAL_WEATHER_CALL_ID   0x4C574558u          /* "LWEX" */
#define LOCAL_WEATHER_CALL_TEXT "local:weather"
#define WEATHER_REFRESH_MS      600000u
#define WEATHER_FIRST_FETCH_MS  20000u
static uint32_t s_weather_last_fetch_ms;   /* app task only; 0 = never */
static bool     s_weather_inflight;        /* app task only */
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

static void weather_apply_result(const jr_tool_result_t *result, uint32_t now)
{
    s_weather_inflight = false;
    s_weather_last_fetch_ms = now;
    if (result->status != JR_TOOL_STATUS_OK) {
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
        ESP_LOGW(TAG, "weather: no temperature in result; keeping old data");
        cJSON_Delete(root);
        return;
    }
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
    cJSON_Delete(root);
    s_weather = w;
    jr_display_weather_set(&s_weather);
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
    if (s_weather_inflight || !atomic_load(&s_tool_diag.worker_ready) ||
        !jr_tools_is_configured()) {
        return;
    }
    const bool first = s_weather_last_fetch_ms == 0U;
    const bool on_screen = jr_display_nav_space() == JR_DISPLAY_SPACE_WEATHER;
    if (first ? now < WEATHER_FIRST_FETCH_MS
              : !(on_screen && now - s_weather_last_fetch_ms >= WEATHER_REFRESH_MS)) {
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
        ESP_LOGI(TAG, "weather: fetch submitted (%s)", first ? "first" : "screen entered");
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

static void board_announce(const char *title, int terminal, const char *result)
{
    char row[25];
    title_shorten(row, sizeof row - 5U, title);      /* "TASK " + 19 glyphs */
    jr_display_activity_push("TASK", row);

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
        char cap[40];
        snprintf(cap, sizeof cap, "%s: %.30s", terminal == 2 ? "BLOCKED" : "DONE", title);
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
    if (first ? now < BOARD_FIRST_POLL_MS
              : now - s_board_last_poll_ms < BOARD_POLL_MS) {
        return;
    }
    if (atomic_load(&s_mood_id) == (uint8_t)JR_MOOD_DREAM) {
        return;                       /* a dark glass keeps Wi-Fi in min-modem */
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
        else if (strcmp(tool_name, "ask_user") == 0) kind = "ASK";
        else if (strcmp(tool_name, "search_tools") == 0) kind = "SEARCH";
        else if (strcmp(tool_name, "delegate_task") == 0) kind = "DELEGATE";
        else if (strcmp(tool_name, "delegated_tasks") == 0) kind = "TASKS";
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
    /* Skip leading spaces the transcript chunks carry, then cut to 24 glyphs
     * at a word boundary with the mark title_shorten() uses. */
    const char *src = s_turn_head;
    while (*src == ' ') {
        src++;
    }
    if (*src != '\0') {
        /* One 24-glyph row holds "KIND summary": the summary gets what the
         * tag and its space leave, cut at a word with the mark. */
        char summary[25];
        size_t cap = 24U - strlen(s_turn_kind) - 1U;
        if (cap < 8U) {
            cap = 8U;
        }
        title_shorten(summary, cap + 1U, src);
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

static void device_tool_queue_error(uint32_t call_id, const char *call_id_text,
                                    const char *name, uint32_t session_gen,
                                    jr_tool_status_t status, const char *code,
                                    const char *message)
{
    if (s_local_tool_count >= LOCAL_TOOL_RESULT_CAP) {
        atomic_fetch_add(&s_tool_diag.failed, 1U);
        atomic_fetch_add(&s_tool_diag.response_send_failed, 1U);
        return;
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
    (void)snprintf(slot->response_json, sizeof(slot->response_json),
                   "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                   code, message);
    s_local_tool_count++;
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
            board_apply_result(result);
            memset(result, 0, sizeof(*result));
            continue;
        }
        device_tool_record_result(result->name, result->status, result->duration_ms,
                                  result->http_status);
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
