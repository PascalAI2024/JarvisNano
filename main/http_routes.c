/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * http_routes.c — the HTTP control plane of the composition root.
 *
 * Every route the desk, the dashboard and the bench talk to. Handlers parse
 * and authenticate, then post bounded commands into the voice task's lanes;
 * none of them owns realtime state (ARCHITECTURE.md, Ownership). Split out
 * of main.c on 2026-09-02 with no behavior change; the helpers that only
 * handlers use stay static here.
 */
#include "app.h"

static const char *TAG = "jarvis_v5";

extern const unsigned char diagnostics_html_start[]
    asm("_binary_diagnostics_html_start");
extern const unsigned char diagnostics_html_end[]
    asm("_binary_diagnostics_html_end");

/* ======================================================================== *
 *  diag HTTP: snapshot + /api/debug/say + /api/debug/gain                  *
 * ======================================================================== */
static bool agent_require_auth(httpd_req_t *req);

static esp_err_t diag_get_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    const jr_state_snapshot_t *s = jr_orch_snapshot(&s_app.orch);
    uint64_t now = jr_clock_now_ms(&s_app.clock);
    uint32_t now32 = (uint32_t)now;
    bool voice_alive = s_voice_task_running &&
                       (uint32_t)(now32 - s_voice_task_heartbeat_ms) < 1000;
    jr_state_t phase = jr_orch_phase(&s_app.orch);
    bool voice_armed = phase != JR_ST_IDLE && phase != JR_ST_DRAINING &&
                       phase != JR_ST_FATAL;
    /* Always 0: this firmware is always-ready listening (VOICE_ALWAYS_READY),
     * so there is no listen window to count down. The deadline it used to read
     * was assigned only ever 0 at four sites. Kept as a field so existing
     * tooling keeps parsing; do not build a countdown rim on it. */
    const uint32_t auto_idle_ms = 0U;
    uint32_t audio_until = atomic_load(&s_audio_diag_until_ms);
    bool audio_diag_running = (int32_t)(audio_until - now32) > 0;
    unsigned stack_hwm = s_voice_task ?
        (unsigned)uxTaskGetStackHighWaterMark(s_voice_task) : 0;
    bool tools_ready = atomic_load(&s_tool_diag.worker_ready);
    bool tools_configured = tools_ready && jr_tools_is_configured();
    /* Transcript tail, defanged for direct embedding: quotes, backslashes and
     * control bytes become spaces. Lossy on purpose — this is a diag peek, not
     * a transcript API. */
    char said_safe[128];   /* httpd stack is the tight one; clip, don't grow */
    for (size_t i = 0; i < sizeof said_safe; ++i) {
        char ch = s_last_said[i];
        said_safe[i] = (ch != '\0' && (ch == '"' || ch == '\\' ||
                        (unsigned char)ch < 0x20)) ? ' ' : ch;
        if (ch == '\0') break;
    }
    said_safe[sizeof said_safe - 1] = '\0';
    char buf[2304];
    int n = snprintf(buf, sizeof buf,
        "{\"phase\":\"%s\",\"transitions\":%u,\"deaths\":%u,\"reconnects\":%u,"
        "\"fail_count\":%u,\"last_reason\":\"%s\",\"last_error_kind\":%d,"
        "\"aec_us\":%u,\"ws_connected\":%s,\"capturing\":%s,"
        "\"voice_armed\":%s,\"always_ready\":true,"
        "\"privacy_paused\":%s,\"auto_idle_ms\":%u,\"shade_open\":%s,"
        "\"audio_diag_queued\":%s,\"audio_diag_running\":%s,"
        "\"uptime_ms\":%llu,\"voice_task_running\":%s,\"voice_task_alive\":%s,"
        "\"voice_stack_hwm\":%u,\"free_internal_heap\":%u,"
        "\"largest_internal_block\":%u,\"free_psram\":%u,"
        "\"tx_queue_depth\":%u,\"tx_would_block\":%u,\"tx_drops\":%u,"
        "\"rx_parse_errors\":%u,\"rx_alloc_failures\":%u,\"mic_rms\":%.1f,"
        "\"vad_floor\":%.1f,\"vad_starts\":%u,\"vad_ends\":%u,"
        "\"barge_enabled\":%s,\"barge_candidates\":%u,\"barge_events\":%u,"
        "\"activity_open\":%s,\"playback_pending\":%s,\"dac_muted\":%s,"
        "\"terminal_pending\":%s,"
        "\"rx_frames\":%u,\"audio_chunks\":%u,"
        "\"audio_samples\":%u,\"audio_dropped_samples\":%u,"
        "\"text_parts\":%u,\"turn_complete\":%u,"
        "\"generation_complete\":%u,\"server_errors\":%u,"
        "\"last_said\":\"%.120s\","
        "\"tools\":{\"execution\":\"on_device\",\"worker_ready\":%s,"
        "\"configured\":%s,\"declared\":%u,\"last_tool\":\"%s\","
        "\"last_status\":\"%s\","
        "\"last_http_status\":%d,\"last_duration_ms\":%u,"
        "\"calls_received\":%u,\"submitted\":%u,\"submit_rejected\":%u,"
        "\"completed\":%u,\"succeeded\":%u,\"failed\":%u,"
        "\"cancelled\":%u,\"stale_dropped\":%u,\"responses_sent\":%u,"
        "\"response_send_failed\":%u,\"consent_active\":%s,"
        "\"consent_prompted\":%u,\"consent_approved\":%u,"
        "\"consent_denied\":%u,\"consent_timed_out\":%u,"
        "\"consent_cancelled\":%u}}",
        jr_state_name(s->phase), (unsigned)s->transitions, (unsigned)s->deaths,
        (unsigned)s->reconnects, (unsigned)s->fail_count,
        jr_event_name(s->last_reason), (int)s->last_error_kind,
        (unsigned)jr_audio_last_aec_us(),
        s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN ? "true" : "false",
        s_app.io.capturing ? "true" : "false",
        voice_armed ? "true" : "false",
        atomic_load(&s_voice_privacy_paused) ? "true" : "false",
        (unsigned)auto_idle_ms,
        s_ui_shade_open ? "true" : "false",
        atomic_load(&s_audio_diag_requested) ? "true" : "false",
        audio_diag_running ? "true" : "false",
        (unsigned long long)now,
        s_voice_task_running ? "true" : "false",
        voice_alive ? "true" : "false",
        stack_hwm,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)jr_gemini_txq_depth(&s_app.client),
        (unsigned)s_app.client.live.tx_would_block,
        (unsigned)s_app.client.live.tx_drops,
        (unsigned)s_app.client.live.rx_parse_errors,
        (unsigned)s_app.client.live.rx_alloc_failures,
        (double)s_app.mic_rms,
        (double)s_app.turn.noise_floor,
        (unsigned)s_app.vad_starts,
        (unsigned)s_app.vad_ends,
        s_local_barge_enabled ? "true" : "false",
        (unsigned)s_app.barge_candidates,
        (unsigned)s_app.barge_events,
        s_app.io.activity_open ? "true" : "false",
        jr_audio_playback_pending() ? "true" : "false",
        jr_audio_dac_muted() ? "true" : "false",
        s_app.terminal_pending ? "true" : "false",
        (unsigned)s_app.rx_frames,
        (unsigned)s_app.rx_audio_chunks,
        (unsigned)s_app.rx_audio_samples,
        (unsigned)s_app.rx_audio_dropped_samples,
        (unsigned)s_app.rx_text_parts,
        (unsigned)s_app.rx_turn_complete,
        (unsigned)s_app.rx_generation_complete,
        (unsigned)s_app.rx_errors,
        said_safe,
        tools_ready ? "true" : "false",
        tools_configured ? "true" : "false",
        (unsigned)DEVICE_TOOL_DECL_COUNT,
        device_tool_last_name(),
        device_tool_last_status(),
        atomic_load(&s_tool_diag.last_http_status),
        (unsigned)atomic_load(&s_tool_diag.last_duration_ms),
        (unsigned)atomic_load(&s_tool_diag.calls_received),
        (unsigned)atomic_load(&s_tool_diag.submitted),
        (unsigned)atomic_load(&s_tool_diag.submit_rejected),
        (unsigned)atomic_load(&s_tool_diag.completed),
        (unsigned)atomic_load(&s_tool_diag.succeeded),
        (unsigned)atomic_load(&s_tool_diag.failed),
        (unsigned)atomic_load(&s_tool_diag.cancelled),
        (unsigned)atomic_load(&s_tool_diag.stale_dropped),
        (unsigned)atomic_load(&s_tool_diag.responses_sent),
        (unsigned)atomic_load(&s_tool_diag.response_send_failed),
        atomic_load(&s_tool_diag.consent_active) ? "true" : "false",
        (unsigned)atomic_load(&s_tool_diag.consent_prompted),
        (unsigned)atomic_load(&s_tool_diag.consent_approved),
        (unsigned)atomic_load(&s_tool_diag.consent_denied),
        (unsigned)atomic_load(&s_tool_diag.consent_timed_out),
        (unsigned)atomic_load(&s_tool_diag.consent_cancelled));
    if (n < 0 || (size_t)n >= sizeof buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status encoding failed");
        return ESP_OK;
    }
    size_t response_len = (size_t)n;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, response_len);
    return ESP_OK;
}

static esp_err_t dashboard_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'self'; style-src 'self' 'unsafe-inline'; "
        "script-src 'self' 'unsafe-inline'; connect-src 'self'; "
        "img-src 'none'; object-src 'none'; base-uri 'none'; "
        "form-action 'none'; frame-ancestors 'none'");
    size_t length = (size_t)(diagnostics_html_end - diagnostics_html_start);
    if (length > 0U && diagnostics_html_start[length - 1U] == '\0') {
        length--;
    }
    return httpd_resp_send(req, (const char *)diagnostics_html_start, length);
}

static bool query_int(httpd_req_t *req, const char *key, int *out)
{
    char q[256];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) {
        return false;
    }
    char v[32];
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) {
        return false;
    }
    *out = atoi(v);
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/* esp_http_server extracts the raw query value but does not percent-decode it.
 * Decode in place so the diagnostic endpoint sends the words the caller
 * supplied rather than making Gemini pronounce "%20". */
static bool url_decode_in_place(char *s)
{
    char *src = s;
    char *dst = s;
    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        if (*src == '%') {
            int hi = hex_nibble(src[1]);
            int lo = src[1] != '\0' ? hex_nibble(src[2]) : -1;
            if (hi < 0 || lo < 0) {
                return false;
            }
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    return true;
}

/* Mutating diagnostics remain intentionally trusted-LAN rather than bearer
 * authenticated, but they are POST-only and require a non-simple intent
 * header. That blocks link prefetchers and drive-by cross-origin form/fetch
 * requests while keeping the bench CLI and same-origin cockpit usable. */
static bool control_intent_required(httpd_req_t *req)
{
    static const char *header = "X-JarvisNano-Control";
    char value[8] = {0};
    size_t length = httpd_req_get_hdr_value_len(req, header);
    if (length != 1U ||
        httpd_req_get_hdr_value_str(req, header, value, sizeof value) != ESP_OK ||
        strcmp(value, "1") != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"explicit local control intent required\"}");
        return false;
    }
    if (atomic_load(&s_tool_diag.consent_active) &&
        strcmp(req->uri, "/api/brain/inbox") != 0) {
        httpd_resp_set_status(req, "423 Locked");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"physical consent owns controls\"}");
        return false;
    }
    return true;
}

static esp_err_t say_get_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    char q[256];
    char text[200] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK ||
        httpd_query_key_value(q, "text", text, sizeof text) != ESP_OK ||
        text[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?text=");
        return ESP_OK;
    }
    if (!url_decode_in_place(text) || text[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ?text= encoding");
        return ESP_OK;
    }
    /* Hand off to the single-writer app task. Never report success when the
     * queue/task is unavailable; diagnostics must describe reality. */
    if (s_say_q == NULL || !s_voice_task_running ||
        xQueueSend(s_say_q, text, 0) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"voice queue unavailable\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t gain_get_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    int mic = -1, ref = -1, vol = -1, barge = -1, vadclean = -1, pbgain = -1;
    int speakmic = -1;
    query_int(req, "mic", &mic);
    query_int(req, "ref", &ref);
    query_int(req, "vol", &vol);
    query_int(req, "barge", &barge);
    query_int(req, "vadclean", &vadclean);
    query_int(req, "pbgain", &pbgain);
    int preroll = -1, refill = -1;
    query_int(req, "preroll", &preroll);
    query_int(req, "refill", &refill);
    jr_audio_set_jitter_ms(preroll, refill);
    int cpu = -1;
    query_int(req, "cpu", &cpu);
    if (cpu == 0 || cpu == 80 || cpu == 160 || cpu == 240) {
        atomic_store(&s_cpu_force, cpu);   /* bench: force a gear, 0 = auto */
    }
    query_int(req, "speakmic", &speakmic);
    jr_audio_set_gains(mic, ref, vol);
    if (speakmic >= 0) {
        jr_audio_set_speak_mic_db(speakmic);
    }
    if (barge >= 0) {
        s_local_barge_enabled = barge != 0;
    }
    if (vadclean >= 0) {
        atomic_store(&s_vad_use_clean, vadclean != 0);
    }
    if (pbgain >= 0) {
        jr_audio_set_playback_gain_percent(pbgain);
    }
    /* Report DEVICE state, never the request: a read with no arguments used
     * to answer mic/ref/vol = -1, and a partial write echoed the requested
     * value even when nothing was applied. */
    char buf[256];
    int n = snprintf(buf, sizeof buf,
                     "{\"ok\":true,\"mic\":%d,\"ref\":%d,\"vol\":%d,"
                     "\"speakmic\":%d,"
                     "\"barge\":%s,\"vadclean\":%s,\"pbgain\":%d,"
                     "\"preroll\":%d,\"refill\":%d}",
                     jr_audio_mic_db(), jr_audio_ref_db(), jr_audio_out_vol(),
                     jr_audio_speak_mic_db(),
                     s_local_barge_enabled ? "true" : "false",
                     atomic_load(&s_vad_use_clean) ? "true" : "false",
                     jr_audio_playback_gain_percent(),
                     jr_audio_preroll_ms(), jr_audio_refill_ms());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t device_levels_get_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    char body[96];
    int n = snprintf(body, sizeof body,
                     "{\"volume\":%d,\"brightness\":%u}",
                     s_out_vol, (unsigned)s_brightness_cap);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t device_levels_post_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    int volume = -1;
    int brightness = -1;
    const bool have_volume = query_int(req, "volume", &volume);
    const bool have_brightness = query_int(req, "brightness", &brightness);
    if ((!have_volume && !have_brightness) ||
        (have_volume && (volume < 10 || volume > 100)) ||
        (have_brightness && (brightness < 10 || brightness > 100))) {
        httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "volume/brightness must be 10..100");
        return ESP_OK;
    }
    if (have_volume) {
        atomic_store(&s_level_volume_request, volume);
    }
    if (have_brightness) {
        atomic_store(&s_level_brightness_request, brightness);
    }
    char body[112];
    int n = snprintf(body, sizeof body,
        "{\"ok\":true,\"volume\":%d,\"brightness\":%d,"
        "\"privacy_unchanged\":true}",
        have_volume ? volume : -1,
        have_brightness ? brightness : -1);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static const char *ota_image_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

static esp_err_t device_health_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const jr_state_t phase = jr_orch_phase(&s_app.orch);
    const bool privacy = atomic_load(&s_voice_privacy_paused);
    const bool operator_active = operator_mode_active(now);
    const jr_state_snapshot_t *snapshot = jr_orch_snapshot(&s_app.orch);
    const bool voice_alive = s_voice_task_running &&
        (uint32_t)(now - s_voice_task_heartbeat_ms) < 2000U;
    const uint32_t largest = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t free_psram =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    jr_display_diag_t display = {0};
    (void)jr_display_get_diag(&display);
    const bool tool_status_valid =
        atomic_load(&s_tool_diag.last_status_valid);
    const jr_tool_status_t tool_status = tool_status_valid
        ? (jr_tool_status_t)atomic_load(&s_tool_diag.last_status)
        : JR_TOOL_STATUS_OK;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *last_invalid =
        esp_ota_get_last_invalid_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    const bool ota_state_known = running != NULL &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK;
    const int ota_last_error = atomic_load(&s_ota_last_error);
    const uint32_t last_tx_drop_ms = atomic_load(&s_last_tx_drop_ms);
    const bool recent_tx_drop = last_tx_drop_ms != 0U &&
        (uint32_t)(now - last_tx_drop_ms) < 10000U;

    jr_audio_playback_stats_t playback = {0};
    jr_audio_playback_stats(&playback);
    jr_ws_rx_diag_t rx = {0};
    jr_gemini_ws_rx_diag(&rx);

    const char *verdict = "ok";
    bool repairable = false;
    if (privacy || s_flip_muted) {
        verdict = "privacy-muted";
    } else if (operator_active) {
        verdict = "operator-active";
    } else if (!voice_alive ||
               ((phase == JR_ST_IDLE || phase == JR_ST_BACKOFF ||
                 phase == JR_ST_FATAL) && !s_mood_rest_disarmed)) {
        verdict = "session-dead";
        repairable = true;
    } else if (recent_tx_drop) {
        verdict = "uplink-dropping";
    } else if (largest < 8192U || free_psram < 2U * 1024U * 1024U) {
        verdict = "memory-critical";
    } else if (display.flush_errors > 0U || display.actual_fps < 10U) {
        /* 12 straddled the shell screens' real cadence (measured 12-14 fps
         * with JARVIS at 19), so a healthy ring screen read as a fault. */
        verdict = "display-fault";
    } else if (jr_audio_dac_muted()) {
        verdict = "audio-fault";
        repairable = true;
    } else if (tool_status_valid && tool_status != JR_TOOL_STATUS_OK) {
        verdict = "tool-fault";
    }

    char body[1280];
    int n = snprintf(body, sizeof body,
        "{\"verdict\":\"%s\",\"repairable\":%s,"
        "\"privacy\":%s,\"flip_muted\":%s,\"operator\":%s,"
        "\"phase\":\"%s\",\"voice_alive\":%s,"
        "\"levels\":{\"volume\":%d,\"brightness_cap\":%u,"
        "\"brightness_actual\":%u},"
        "\"memory\":{\"largest_internal\":%u,\"free_psram\":%u},"
        "\"transport\":{\"would_block\":%u,\"drops\":%u,\"deaths\":%u},"
        "\"playback\":{\"underruns\":%u,\"max_gap_ms\":%u,"
        "\"low_water_ms\":%d,\"dac_failures\":%u,\"replies\":%u,"
        "\"prerolls\":%u,\"preroll_short\":%u},"
        "\"rx\":{\"frames\":%u,\"max_gap_ms\":%u,\"queue_wait_max_ms\":%u,"
        "\"queue_hwm\":%u,\"drops\":%u},"
        "\"display\":{\"fps\":%u,\"flush_errors\":%u},"
        "\"ota\":{\"active\":%s,\"running\":\"%s\",\"boot\":\"%s\","
        "\"state\":\"%s\",\"received\":%u,\"total\":%u,"
        "\"last_error\":\"%s\",\"last_invalid\":\"%s\"},"
        "\"tools\":{\"status\":\"%s\",\"http\":%d}}",
        verdict, repairable ? "true" : "false",
        privacy ? "true" : "false",
        s_flip_muted ? "true" : "false",
        operator_active ? "true" : "false",
        jr_state_name(phase), voice_alive ? "true" : "false",
        s_out_vol, (unsigned)s_brightness_cap,
        (unsigned)atomic_load(&s_mood_brightness),
        (unsigned)largest, (unsigned)free_psram,
        (unsigned)s_app.client.live.tx_would_block,
        (unsigned)s_app.client.live.tx_drops,
        (unsigned)snapshot->deaths,
        (unsigned)playback.underruns, (unsigned)playback.max_gap_ms,
        playback.low_water_valid ? (int)playback.low_water_ms : -1,
        (unsigned)playback.dac_failures, (unsigned)playback.replies_ended,
        (unsigned)playback.prerolls, (unsigned)playback.preroll_timeouts,
        (unsigned)rx.frames, (unsigned)rx.max_gap_ms,
        (unsigned)rx.queue_wait_max_ms, (unsigned)rx.queue_hwm,
        (unsigned)rx.drops,
        (unsigned)display.actual_fps, (unsigned)display.flush_errors,
        atomic_load(&s_ota_active) ? "true" : "false",
        running != NULL ? running->label : "unknown",
        boot != NULL ? boot->label : "unknown",
        ota_state_known ? ota_image_state_name(ota_state) : "unknown",
        (unsigned)atomic_load(&s_ota_received_bytes),
        (unsigned)atomic_load(&s_ota_total_bytes),
        esp_err_to_name((esp_err_t)ota_last_error),
        last_invalid != NULL ? last_invalid->label : "none",
        jr_tools_status_name(tool_status),
        tool_status_valid ? atomic_load(&s_tool_diag.last_http_status) : 0);
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "health response too large");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t voice_control_handler(httpd_req_t *req)
{
    if (!control_intent_required(req) || !agent_require_auth(req)) {
        return ESP_OK;
    }
    int armed = -1;
    int resume = -1;
    const bool have_armed = query_int(req, "armed", &armed);
    const bool have_resume = query_int(req, "resume", &resume);
    if (have_resume && resume == 1) {
        atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"ok\":true,\"queued\":true,\"resume\":\"privacy-safe\"}");
        return ESP_OK;
    }
    if (!have_armed || (armed != 0 && armed != 1) || have_resume) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                           "use ?armed=0|1 or paired ?resume=1");
        return ESP_OK;
    }
    atomic_store(&s_voice_control_request,
                 armed ? VOICE_CONTROL_RESUME : VOICE_CONTROL_DISARM);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, armed
        ? "{\"ok\":true,\"queued\":true,\"resume\":\"privacy-safe\"}"
        : "{\"ok\":true,\"queued\":true,\"armed\":false}");
    return ESP_OK;
}

static const char *audio_tap_name(jr_audio_tap_kind_t kind)
{
    switch (kind) {
    case JR_AUDIO_TAP_MIC_CLEAN: return "mic-clean";
    case JR_AUDIO_TAP_MIC_RAW:   return "mic-raw";
    case JR_AUDIO_TAP_REFERENCE: return "reference";
    case JR_AUDIO_TAP_PLAYBACK:  return "playback";
    default:                     return "unknown";
    }
}

static bool audio_tap_parse(const char *name, jr_audio_tap_kind_t *kind)
{
    for (int i = 0; i < JR_AUDIO_TAP_COUNT; ++i) {
        if (strcmp(name, audio_tap_name((jr_audio_tap_kind_t)i)) == 0) {
            *kind = (jr_audio_tap_kind_t)i;
            return true;
        }
    }
    return false;
}

static esp_err_t audio_self_test_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    if (atomic_load(&s_voice_privacy_paused) || s_flip_muted) {
        httpd_resp_set_status(req, "423 Locked");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"privacy blocks self-test\"}");
        return ESP_OK;
    }
    bool already_queued = atomic_exchange(&s_audio_diag_requested, true);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, already_queued
        ? "{\"ok\":true,\"queued\":true,\"already_queued\":true}"
        : "{\"ok\":true,\"queued\":true,\"capture_ms\":1800,"
          "\"evidence\":[\"electrical_reference\",\"acoustic_microphone\"]}");
    return ESP_OK;
}

static esp_err_t audio_taps_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    jr_audio_tap_info_t info[JR_AUDIO_TAP_COUNT];
    for (int i = 0; i < JR_AUDIO_TAP_COUNT; ++i) {
        esp_err_t err = jr_audio_diag_get_info((jr_audio_tap_kind_t)i, &info[i]);
        if (err != ESP_OK) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_set_type(req, "application/json");
            char body[128];
            int n = snprintf(body, sizeof body,
                             "{\"available\":false,\"error\":\"%s\"}",
                             esp_err_to_name(err));
            httpd_resp_send(req, body, n);
            return ESP_OK;
        }
    }

    char body[1024];
    size_t used = 0;
    int n = snprintf(body, sizeof body,
                     "{\"available\":true,\"capture_source\":"
                     "\"codec_single_owner_taps\",\"taps\":{");
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "encoding failed");
        return ESP_OK;
    }
    used = (size_t)n;
    for (int i = 0; i < JR_AUDIO_TAP_COUNT && used < sizeof body; ++i) {
        n = snprintf(body + used, sizeof body - used,
            "%s\"%s\":{\"sample_rate\":%u,\"available_samples\":%u,"
            "\"capacity_samples\":%u,"
            "\"total_samples\":%llu,\"peak\":%d,\"rms\":%.2f,"
            "\"clipped_samples\":%u}",
            i == 0 ? "" : ",", audio_tap_name((jr_audio_tap_kind_t)i),
            (unsigned)info[i].sample_rate,
            (unsigned)info[i].available_samples,
            (unsigned)info[i].capacity_samples,
            (unsigned long long)info[i].total_samples,
            (int)info[i].peak, (double)info[i].rms,
            (unsigned)info[i].clipped_samples);
        if (n < 0 || (size_t)n >= sizeof body - used) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "encoding overflow");
            return ESP_OK;
        }
        used += (size_t)n;
    }
    n = snprintf(body + used, sizeof body - used, "}}");
    if (n < 0 || (size_t)n >= sizeof body - used) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "encoding overflow");
        return ESP_OK;
    }
    used += (size_t)n;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, used);
    return ESP_OK;
}

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static esp_err_t audio_tap_wav_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    char query[96];
    char source[24] = {0};
    jr_audio_tap_kind_t kind;
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "source", source, sizeof source) != ESP_OK ||
        !audio_tap_parse(source, &kind)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "missing ?source=mic-clean|mic-raw|reference|playback");
        return ESP_OK;
    }

    jr_audio_tap_info_t info;
    esp_err_t err = jr_audio_diag_get_info(kind, &info);
    size_t capacity = err == ESP_OK ? info.capacity_samples : 0U;
    int16_t *pcm = capacity > 0
        ? heap_caps_malloc(capacity * sizeof(int16_t),
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : NULL;
    if (pcm == NULL ||
        jr_audio_diag_copy(kind, pcm, capacity, &info) != ESP_OK ||
        info.available_samples == 0U) {
        heap_caps_free(pcm);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"available\":false,\"error\":\"audio tap empty\"}");
        return ESP_OK;
    }

    size_t samples = info.available_samples;
    uint32_t data_bytes = (uint32_t)(samples * sizeof(int16_t));
    uint8_t header[44] = {0};
    memcpy(header + 0, "RIFF", 4);
    write_le32(header + 4, 36U + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_le32(header + 16, 16U);
    write_le16(header + 20, 1U);
    write_le16(header + 22, 1U);
    write_le32(header + 24, info.sample_rate);
    write_le32(header + 28, info.sample_rate * 2U);
    write_le16(header + 32, 2U);
    write_le16(header + 34, 16U);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_bytes);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Jarvis-Audio-Source", source);
    httpd_resp_set_hdr(req, "X-Jarvis-Evidence-Level",
        kind == JR_AUDIO_TAP_REFERENCE ? "electrical" :
        kind == JR_AUDIO_TAP_MIC_RAW || kind == JR_AUDIO_TAP_MIC_CLEAN
            ? "acoustic" : "software");
    err = httpd_resp_send_chunk(req, (const char *)header, sizeof header);
    const uint8_t *bytes = (const uint8_t *)pcm;
    size_t sent = 0;
    while (err == ESP_OK && sent < data_bytes) {
        size_t chunk = data_bytes - sent;
        if (chunk > 4096U) {
            chunk = 4096U;
        }
        err = httpd_resp_send_chunk(req, (const char *)bytes + sent, chunk);
        sent += chunk;
    }
    heap_caps_free(pcm);
    if (err != ESP_OK) {
        (void)httpd_resp_send_chunk(req, NULL, 0);
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static const char *touch_kind_name(jr_input_kind_t kind)
{
    switch (kind) {
    case JR_INPUT_TAP:        return "tap";
    case JR_INPUT_LONG_PRESS: return "long_press";
    case JR_INPUT_SWIPE:      return "swipe";
    case JR_INPUT_PRESS_DOWN: return "press_down";
    case JR_INPUT_PRESS_UP:   return "press_up";
    default:                  return "none";
    }
}

int touch_sector_from_point(uint16_t x, uint16_t y)
{
    int dx = 2 * (int)x - 465;
    int dy = 2 * (int)y - 465;
    int ax = abs(dx);
    int ay = abs(dy);
    if (ax * 1000 < ay * 414) {
        return dy < 0 ? 0 : 4;
    }
    if (ay * 1000 < ax * 414) {
        return dx > 0 ? 2 : 6;
    }
    if (dx > 0) {
        return dy < 0 ? 1 : 3;
    }
    return dy < 0 ? 7 : 5;
}


/* /api/diag/tasks — per-task stack high-water marks, for right-sizing stacks.
 *
 * "High-water" is FreeRTOS's minimum-ever-free figure in BYTES on ESP-IDF: the
 * closest that task has come to overflowing since it started. Slack = stack
 * size minus peak usage, and slack in INTERNAL RAM is exactly what the TLS
 * handshake is starving for (see docs/JARVISNANO_OS_PLAN.md "Internal RAM
 * budget"). Sorted by headroom so the biggest reclaim candidates come first.
 *
 * Read this AFTER exercising the device (a full voice turn, a display
 * transition) — a task that has not yet hit its worst case reports misleadingly
 * generous headroom, and shrinking a stack on that basis is how you get a
 * stack-overflow panic three weeks later. */
static esp_err_t tasks_diag_handler(httpd_req_t *req)
{
    const UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *snap = calloc(count, sizeof *snap);
    if (snap == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    uint32_t total_runtime = 0;
    const UBaseType_t got = uxTaskGetSystemState(snap, count, &total_runtime);

    char *body = malloc(4096);
    if (body == NULL) {
        free(snap);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    /* total_runtime and each task's run counter come from the esp_timer
     * (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS): two snapshots a few seconds
     * apart give each core's idle share from IDLE0/IDLE1 — the number the
     * CPU gears are chosen from. */
    int off = snprintf(body, 4096,
        "{\"free_internal\":%u,\"largest_internal_block\":%u,"
        "\"cpu_mhz\":%d,\"total_runtime\":%u,\"tasks\":[",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        atomic_load(&s_cpu_mhz), (unsigned)total_runtime);

    for (UBaseType_t i = 0; i < got && off > 0 && off < 4000; ++i) {
        off += snprintf(body + off, (size_t)(4096 - off),
            "%s{\"name\":\"%s\",\"prio\":%u,\"stack_free\":%u,\"run\":%u}",
            i ? "," : "",
            snap[i].pcTaskName ? snap[i].pcTaskName : "?",
            (unsigned)snap[i].uxCurrentPriority,
            (unsigned)snap[i].usStackHighWaterMark,
            (unsigned)snap[i].ulRunTimeCounter);
    }
    if (off > 0 && off < 4090) {
        off += snprintf(body + off, (size_t)(4096 - off), "]}");
    }
    free(snap);

    if (off <= 0 || off >= 4090) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "task encoding overflow");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, off);
    free(body);
    return ESP_OK;
}

/* /api/sensors — live IMU + battery telemetry. Both reads are non-blocking
 * snapshot copies (their sampler tasks own the shared I2C bus), so this handler
 * never parks the httpd task on a transaction. Phase 1 acceptance gate. */
/* POST /api/display/choices?n=3 — present test arcs, or n=0 to dismiss.
 *
 * Exists so the choice-arc RENDERING can be proven on glass independently of a
 * live Gemini ask_user call. The labels are static, so they satisfy the borrow
 * contract in jr_display.h without any lifetime games. */
static esp_err_t choices_debug_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    int n = 3;
    if (!query_int(req, "n", &n) || n < 0 || n > HUD_CHOICE_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "n must be 0..3");
        return ESP_OK;
    }
    /* MARSHALLED, not applied here: the choice statics are single-writer (the
     * app task), and the httpd task writing them raced the flush. The app
     * task drains this next loop; poll /api/display/choices/hit (or a
     * snapshot) for the applied state. A queued-but-undrained request is
     * simply replaced — last writer wins, matching the old semantics. */
    atomic_store(&s_debug_choices_req, (uint32_t)n + 1U);
    char body[96];
    int len = snprintf(body, sizeof body, "{\"queued\":%d,\"active\":%s}",
                       n, jr_display_choices_active() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, len);
    return ESP_OK;
}

/* POST /api/input/tap?x=..&y=.. — inject one synthetic tap through the real
 * input handler (see input_next). This is the finger-free end of the ask loop:
 * present arcs, sim-tap one, and the full CHOICE_PICKED -> functionResponse
 * path runs exactly as it would under glass. */
static esp_err_t tap_sim_handler(httpd_req_t *req)
{
    if (!control_intent_required(req) || !agent_require_auth(req)) {
        return ESP_OK;
    }
    int shake = 0;
    if (query_int(req, "shake", &shake) && shake == 1) {
        atomic_store(&s_sim_shake, 2U);   /* two 10 Hz polls = a real shake */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"queued\":true,\"gesture\":\"shake\"}");
        return ESP_OK;
    }
    int flip = 0;
    if (query_int(req, "flip", &flip) && flip == 1) {
        atomic_store(&s_sim_flip, 30U);   /* ~3 s face-down, then auto face-up */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"queued\":true,\"gesture\":\"flip\"}");
        return ESP_OK;
    }
    int x = -1, y = -1;
    if (!query_int(req, "x", &x) || !query_int(req, "y", &y) ||
        x < 0 || x >= 466 || y < 0 || y >= 466) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need x and y in 0..465");
        return ESP_OK;
    }
    /* CAS-from-zero: a second tap posted before the app task drains the first
     * must be REFUSED, not silently swallowed after being acknowledged. */
    uint32_t expected = 0U;
    const uint32_t packed = (((uint32_t)x + 1U) << 16) | ((uint32_t)y + 1U);
    bool queued = atomic_compare_exchange_strong(&s_sim_touch, &expected,
                                                 packed);
    if (!queued) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    char body[64];
    int len = snprintf(body, sizeof body,
                       "{\"queued\":%s,\"x\":%d,\"y\":%d}",
                       queued ? "true" : "false", x, y);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, len);
    return ESP_OK;
}

/* POST /api/demo — run the 27 s attract reel (POLISH-06). Starts only from a
 * quiet Listening/Idle; a live ask aborts it and any tap ends it. */
static esp_err_t demo_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    /* Answer what will actually happen. The consumer only starts the reel
     * from LISTENING or IDLE and only when one is not already running; every
     * other request was consumed and dropped while this said "queued". The
     * phase can still move before the app task looks, so a rare late drop
     * remains possible — it is now logged there too. */
    const jr_state_t p = jr_orch_phase(&s_app.orch);
    httpd_resp_set_type(req, "application/json");
    if (s_demo_start_ms != 0U) {
        httpd_resp_sendstr(req, "{\"queued\":false,\"reason\":\"running\"}");
        return ESP_OK;
    }
    if (p != JR_ST_LISTENING && p != JR_ST_IDLE) {
        char buf[96];
        int n = snprintf(buf, sizeof buf,
                         "{\"queued\":false,\"reason\":\"phase\","
                         "\"phase\":\"%s\"}", jr_state_name(p));
        httpd_resp_send(req, buf, n);
        return ESP_OK;
    }
    atomic_store(&s_demo_req, true);
    httpd_resp_sendstr(req, "{\"queued\":true,\"reel_s\":27}");
    return ESP_OK;
}

/* GET /api/display/choices/hit?x=..&y=.. — resolve a panel point to an arc.
 * Lets the hit geometry be checked against the rendered pixels without a finger. */
static esp_err_t choices_hit_handler(httpd_req_t *req)
{
    int x = -1, y = -1;
    if (!query_int(req, "x", &x) || !query_int(req, "y", &y)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need x and y");
        return ESP_OK;
    }
    const int idx = jr_display_choice_hit(x, y);
    char body[96];
    int len = snprintf(body, sizeof body, "{\"x\":%d,\"y\":%d,\"index\":%d}", x, y, idx);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, len);
    return ESP_OK;
}

/* POST /api/display/hud?on=0|1 — A/B the HUD's frame-rate cost on real glass. */
static esp_err_t hud_toggle_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    int on = 1;
    if (!query_int(req, "on", &on) || (on != 0 && on != 1)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "on must be 0 or 1");
        return ESP_OK;
    }
    jr_display_set_hud_enabled(on != 0);
    char body[64];
    int n = snprintf(body, sizeof body, "{\"hud\":%s}", on ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t sensors_handler(httpd_req_t *req)
{
    /* Start the samplers on first use rather than at boot — see the note in
     * app_main(). Both calls are idempotent. The first request after boot
     * therefore reports available=false while the samplers warm up (one 10 ms
     * period for the IMU, one 5 s period for the battery); the next is live. */
    (void)jr_imu_start();
    (void)jr_power_start();

    jr_imu_t imu = {0};
    jr_power_t bat = {0};
    const bool have_imu = jr_imu_read(&imu) == ESP_OK;
    const bool have_bat = jr_power_read(&bat) == ESP_OK;

    char body[768];
    int n = snprintf(body, sizeof body,
        "{\"imu\":{\"available\":%s,\"present\":%s,\"i2c_addr\":\"0x%02X\","
        "\"raw\":{\"ax\":%d,\"ay\":%d,\"az\":%d},"
        "\"g\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
        "\"pitch_deg\":%.1f,\"roll_deg\":%.1f,\"orientation\":\"%s\","
        "\"motion_mg\":%.1f,\"moving\":%s,\"shake\":%s,"
        "\"sample_seq\":%u,\"age_ms\":%u},"
        "\"battery\":{\"available\":%s,\"present\":%s,\"charging\":%s,"
        "\"usb_present\":%s,\"percent\":%d,\"millivolts\":%u,"
        "\"sample_seq\":%u,\"age_ms\":%u}}",
        have_imu ? "true" : "false",
        imu.present ? "true" : "false",
        (unsigned)imu.i2c_addr,
        (int)imu.ax, (int)imu.ay, (int)imu.az,
        (double)imu.gx, (double)imu.gy, (double)imu.gz,
        (double)imu.pitch_deg, (double)imu.roll_deg,
        imu.orientation ? imu.orientation : "unknown",
        (double)imu.motion_mg,
        imu.moving ? "true" : "false",
        imu.shake ? "true" : "false",
        (unsigned)imu.sample_seq, (unsigned)imu.age_ms,
        have_bat ? "true" : "false",
        bat.present ? "true" : "false",
        bat.charging ? "true" : "false",
        bat.usb_present ? "true" : "false",
        bat.percent == 0xFF ? -1 : (int)bat.percent,
        (unsigned)bat.millivolts,
        (unsigned)bat.sample_seq, (unsigned)bat.age_ms);
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "sensor status encoding failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t touch_status_handler(httpd_req_t *req)
{
    bool active = atomic_load(&s_touch_challenge_active);
    bool verified = atomic_load(&s_touch_challenge_verified);
    char body[768];
    int n = snprintf(body, sizeof body,
        "{\"available\":true,\"events\":%u,\"taps\":%u,"
        "\"long_presses\":%u,\"swipes\":%u,\"last\":{"
        "\"kind\":\"%s\",\"x\":%u,\"y\":%u,\"dx\":%d,"
        "\"dy\":%d,\"duration_ms\":%u},\"shade_open\":%s,"
        "\"panel_touch_challenge\":{\"pending\":%s,\"active\":%s,"
        "\"verified\":%s,\"evidence_level\":\"%s\","
        "\"expected_sector\":%u,\"correct_rounds\":%u,"
        "\"attempts\":%u,\"wrong\":%u,\"last_mapped_sector\":%u,"
        "\"last_latency_ms\":%u}}",
        (unsigned)atomic_load(&s_touch_events),
        (unsigned)atomic_load(&s_touch_taps),
        (unsigned)atomic_load(&s_touch_long_presses),
        (unsigned)atomic_load(&s_touch_swipes),
        touch_kind_name((jr_input_kind_t)atomic_load(&s_touch_last_kind)),
        (unsigned)atomic_load(&s_touch_last_x),
        (unsigned)atomic_load(&s_touch_last_y),
        atomic_load(&s_touch_last_dx), atomic_load(&s_touch_last_dy),
        (unsigned)atomic_load(&s_touch_last_duration_ms),
        s_ui_shade_open ? "true" : "false",
        atomic_load(&s_touch_challenge_start_requested) ? "true" : "false",
        active ? "true" : "false", verified ? "true" : "false",
        verified ? "physical_human_challenge" :
                   active ? "control_path_waiting_for_human" : "software",
        (unsigned)atomic_load(&s_touch_challenge_expected),
        (unsigned)atomic_load(&s_touch_challenge_correct),
        (unsigned)atomic_load(&s_touch_challenge_attempts),
        (unsigned)atomic_load(&s_touch_challenge_wrong),
        (unsigned)atomic_load(&s_touch_challenge_last_mapped),
        (unsigned)atomic_load(&s_touch_challenge_last_latency_ms));
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "touch status encoding failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t panel_touch_control_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    char query[64];
    char action[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "action", action, sizeof action) != ESP_OK) {
        return touch_status_handler(req);
    }
    if (strcmp(action, "start") == 0) {
        atomic_store(&s_touch_challenge_cancel_requested, false);
        atomic_store(&s_touch_challenge_start_requested, true);
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":true,\"queued\":true,\"rounds_required\":3}"
        );
        return ESP_OK;
    }
    if (strcmp(action, "cancel") == 0) {
        atomic_store(&s_touch_challenge_start_requested, false);
        atomic_store(&s_touch_challenge_cancel_requested, true);
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"cancelled\":true}");
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "?action=start|cancel");
    return ESP_OK;
}

static esp_err_t ui_shade_control_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    char query[64];
    char action[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "action", action, sizeof action) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "?action=open|close|toggle");
        return ESP_OK;
    }
    bool open = atomic_load(&s_ui_shade_open);
    if (strcmp(action, "open") == 0) {
        open = true;
    } else if (strcmp(action, "close") == 0) {
        open = false;
    } else if (strcmp(action, "toggle") == 0) {
        open = !open;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "?action=open|close|toggle");
        return ESP_OK;
    }
    atomic_store(&s_ui_shade_open, open);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, open
        ? "{\"ok\":true,\"shade_open\":true}"
        : "{\"ok\":true,\"shade_open\":false}");
    return ESP_OK;
}

void secure_zero(void *ptr, size_t size)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (size-- > 0U) {
        *p++ = 0U;
    }
}

/* cJSON's parser is recursive. Bound structural depth before parsing so a
 * paired LAN client cannot turn a small (<=1536-byte) but pathological body
 * into an HTTP-task stack overflow. The accepted Agent Link schema needs only
 * root -> evidence array -> evidence object (depth three). */
static bool json_depth_within(const char *json, size_t length,
                              unsigned max_depth)
{
    unsigned depth = 0U;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < length; ++i) {
        char ch = json[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            if (++depth > max_depth) {
                return false;
            }
        } else if (ch == '}' || ch == ']') {
            if (depth == 0U) {
                return false;
            }
            depth--;
        }
    }
    return !in_string && depth == 0U;
}

static esp_err_t pairing_claim_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t until = atomic_load(&s_pairing_claim_until_ms);
    if (until == 0U || (int32_t)(until - now) <= 0) {
        atomic_store(&s_pairing_claim_until_ms, 0U);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"hold BOOT 1.5-5 seconds; retry within 60 seconds\"}");
        return ESP_OK;
    }

    int rotate = 0;
    (void)query_int(req, "rotate", &rotate);
    if (rotate != 0) {
        esp_err_t clear_err = jr_cfg_set(JR_CFG_PAIRING_TOKEN, "");
        if (clear_err != ESP_OK) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "pairing rotation failed");
            return ESP_OK;
        }
    }
    char token[JR_CFG_PAIRING_TOKEN_CAP] = {0};
    bool created = false;
    esp_err_t err = jr_net_pairing_token_ensure(token, sizeof token, &created);
    if (err != ESP_OK || !created || token[0] == '\0') {
        secure_zero(token, sizeof token);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, rotate == 0
            ? "{\"ok\":false,\"error\":\"token already exists; claim with rotate=1\"}"
            : "{\"ok\":false,\"error\":\"new token unavailable\"}");
        return ESP_OK;
    }
    atomic_store(&s_pairing_claim_until_ms, 0U);
    s_ui_shade_open = false;
    if (VOICE_ALWAYS_READY) {
        atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
    }
    char body[128];
    int n = snprintf(body, sizeof body,
                     "{\"ok\":true,\"token\":\"%s\",\"one_time\":true}",
                     token);
    secure_zero(token, sizeof token);
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "pairing response failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    secure_zero(body, sizeof body);
    return ESP_OK;
}

/* ── DEV MODE ────────────────────────────────────────────────────────────────
 * Set to 0 before shipping. One digit, and grep for JR_DEV_OPEN_DIAGNOSTICS.
 *
 * While this is 1 the pairing token is NOT required on the diagnostic and
 * control endpoints — /api/logs, /api/cockpit, the /api/audio routes,
 * /api/debug/input and the rest answer any caller that can reach the device
 * on the LAN. That is
 * the whole point: during bring-up the token was pure friction, it lives in a
 * keychain entry that may not exist on a fresh machine, and claiming a new one
 * needs a physical long-press on the very device you are trying to debug
 * remotely.
 *
 * What it costs: anything on your network can read the logs, hear the mic taps,
 * drive the display and inject input. On a home LAN behind a router that is a
 * considered trade; on any shared or public network it is not. The boot log
 * says so loudly on every boot so this cannot ship unnoticed.
 *
 * The X-JarvisNano-Control header gate on mutating POSTs is deliberately NOT
 * bypassed — it costs a caller nothing and still blocks drive-by cross-origin
 * requests and link prefetchers. */
#define JR_DEV_OPEN_DIAGNOSTICS 1

static bool agent_require_auth(httpd_req_t *req)
{
#if JR_DEV_OPEN_DIAGNOSTICS
    (void)req;
    return true;
#else
    static const char *header = "X-JarvisNano-Token";
    size_t length = httpd_req_get_hdr_value_len(req, header);
    if (length == 0U || length >= JR_CFG_PAIRING_TOKEN_CAP) {
        httpd_resp_set_status(req, length == 0U
            ? "401 Unauthorized" : "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"pairing token required\"}");
        return false;
    }
    char token[JR_CFG_PAIRING_TOKEN_CAP] = {0};
    bool matches = false;
    esp_err_t err = httpd_req_get_hdr_value_str(req, header, token,
                                                 sizeof token);
    if (err == ESP_OK) {
        err = jr_net_pairing_token_verify(token, &matches);
    }
    secure_zero(token, sizeof token);
    if (err != ESP_OK || !matches) {
        httpd_resp_set_status(req, err == ESP_OK
            ? "403 Forbidden" : "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, err == ESP_OK
            ? "{\"ok\":false,\"error\":\"pairing token rejected\"}"
            : "{\"ok\":false,\"error\":\"pairing not initialised\"}");
        return false;
    }
    return true;
#endif /* JR_DEV_OPEN_DIAGNOSTICS */
}

static bool url_ends_with(const char *url, const char *suffix)
{
    if (url == NULL || suffix == NULL) return false;
    size_t url_len = strlen(url);
    size_t suffix_len = strlen(suffix);
    return suffix_len <= url_len &&
        strcmp(url + url_len - suffix_len, suffix) == 0;
}

static const char *device_tool_route_kind(const char *url)
{
    if (url_ends_with(url, "/device/v1/invoke")) return "typed_device";
    if (url_ends_with(url, "/act")) return "legacy_fixed_template";
    return "none";
}

static void device_tool_config_snapshot(bool *configured, bool *typed,
                                        bool *legacy)
{
    jr_net_config_t stored = {0};
    bool ready = atomic_load(&s_tool_diag.worker_ready);
    bool loaded = jr_cfg_load(&stored, JR_CFG_VIEW_INTERNAL) == ESP_OK;
    const char *kind = loaded
        ? device_tool_route_kind(stored.jarvis_mcp_url) : "none";
    *configured = ready && jr_tools_is_configured();
    *typed = *configured && strcmp(kind, "typed_device") == 0;
    *legacy = *configured && strcmp(kind, "legacy_fixed_template") == 0;
    secure_zero(&stored, sizeof(stored));
}

static void device_tool_config_reply(httpd_req_t *req)
{
    bool configured = false;
    bool typed = false;
    bool legacy = false;
    device_tool_config_snapshot(&configured, &typed, &legacy);
    const char *kind = typed ? "typed_device" :
        legacy ? "legacy_fixed_template" : "none";
    char body[256];
    int n = snprintf(body, sizeof(body),
        "{\"configured\":%s,\"route_kind\":\"%s\","
        "\"typed_device\":%s,\"legacy_fixed_template\":%s,"
        "\"project_id\":\"%s\"}",
        configured ? "true" : "false", kind,
        typed ? "true" : "false", legacy ? "true" : "false",
        jr_tools_board_project());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body,
                    n > 0 && (size_t)n < sizeof(body) ? n : 0);
}

static esp_err_t device_tool_config_get_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    device_tool_config_reply(req);
    return ESP_OK;
}

static esp_err_t device_tool_config_post_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    enum { TOOL_CONFIG_BODY_CAP = 768 };
    size_t length = req->content_len;
    if (length == 0U || length >= TOOL_CONFIG_BODY_CAP) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid tools config length");
        return ESP_OK;
    }
    char *raw = heap_caps_malloc(TOOL_CONFIG_BODY_CAP,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tools config buffer unavailable");
        return ESP_OK;
    }
    size_t received = 0U;
    while (received < length) {
        int got = httpd_req_recv(req, raw + received, length - received);
        if (got <= 0) {
            secure_zero(raw, TOOL_CONFIG_BODY_CAP);
            heap_caps_free(raw);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "tools config body read failed");
            return ESP_OK;
        }
        received += (size_t)got;
    }
    raw[length] = '\0';
    cJSON *root = json_depth_within(raw, length, 3U)
        ? cJSON_ParseWithLengthOpts(raw, length + 1U, NULL, true) : NULL;
    secure_zero(raw, TOOL_CONFIG_BODY_CAP);
    heap_caps_free(raw);

    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    /* Optional third field: the coordination project the device queues
     * spoken jobs on. Not a secret; "" restores the default. */
    cJSON *project = cJSON_GetObjectItemCaseSensitive(root, "project_id");
    bool valid = cJSON_IsObject(root) && cJSON_IsString(url) &&
        cJSON_IsString(key) && url->valuestring != NULL &&
        key->valuestring != NULL &&
        (project == NULL ||
         (cJSON_IsString(project) && project->valuestring != NULL));
    unsigned fields = 0U;
    bool saw_url = false;
    bool saw_key = false;
    bool saw_project = false;
    for (cJSON *item = valid ? root->child : NULL;
         item != NULL; item = item->next) {
        fields++;
        if (item->string != NULL && strcmp(item->string, "url") == 0 &&
            !saw_url) saw_url = true;
        else if (item->string != NULL && strcmp(item->string, "key") == 0 &&
                 !saw_key) saw_key = true;
        else if (item->string != NULL &&
                 strcmp(item->string, "project_id") == 0 && !saw_project)
            saw_project = true;
        else valid = false;
    }
    valid = valid && fields == (saw_project ? 3U : 2U) && saw_url && saw_key;

    jr_net_config_t next = {0};
    if (valid) {
        size_t url_len = strnlen(url->valuestring, sizeof(next.jarvis_mcp_url));
        size_t key_len = strnlen(key->valuestring, sizeof(next.jarvis_mcp_key));
        bool clearing = url_len == 0U && key_len == 0U;
        valid = url_len < sizeof(next.jarvis_mcp_url) &&
            key_len < sizeof(next.jarvis_mcp_key) &&
            ((clearing) ||
             (url_len > 8U && key_len >= 32U &&
              strncmp(url->valuestring, "https://", 8U) == 0 &&
              strcmp(device_tool_route_kind(url->valuestring), "none") != 0));
        if (valid) {
            strlcpy(next.jarvis_mcp_url, url->valuestring,
                    sizeof(next.jarvis_mcp_url));
            strlcpy(next.jarvis_mcp_key, key->valuestring,
                    sizeof(next.jarvis_mcp_key));
            valid = jr_cfg_validate(JR_CFG_JARVIS_MCP_URL,
                                    next.jarvis_mcp_url) == ESP_OK &&
                jr_cfg_validate(JR_CFG_JARVIS_MCP_KEY,
                                next.jarvis_mcp_key) == ESP_OK;
        }
        if (valid && saw_project) {
            valid = strnlen(project->valuestring, sizeof(next.board_project)) <
                        sizeof(next.board_project) &&
                    jr_cfg_validate(JR_CFG_BOARD_PROJECT,
                                    project->valuestring) == ESP_OK;
            if (valid) {
                strlcpy(next.board_project, project->valuestring,
                        sizeof(next.board_project));
            }
        }
    }
    if (cJSON_IsString(key) && key->valuestring != NULL) {
        secure_zero(key->valuestring, strlen(key->valuestring));
    }
    cJSON_Delete(root);
    if (!valid) {
        secure_zero(&next, sizeof(next));
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"invalid bounded tools config\"}");
        return ESP_OK;
    }

    esp_err_t applied = jr_cfg_apply(&next,
        JR_CFG_F_JARVIS_MCP_URL | JR_CFG_F_JARVIS_MCP_KEY |
        (saw_project ? JR_CFG_F_BOARD_PROJECT : 0U));
    secure_zero(&next, sizeof(next));
    if (applied == ESP_OK) {
        applied = jr_tools_reload_config();
    }
    if (applied != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"tools config unavailable\"}");
        return ESP_OK;
    }
    device_tool_config_reply(req);
    return ESP_OK;
}

static bool agent_text_safe(const char *value, size_t capacity,
                            bool allow_space)
{
    if (value == NULL) {
        return false;
    }
    size_t length = strnlen(value, capacity);
    if (length == 0U || length >= capacity) {
        return false;
    }
    if (strstr(value, "http://") != NULL || strstr(value, "https://") != NULL ||
        strstr(value, "<script") != NULL) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20U || c == 0x7fU || c == '<' || c == '>' ||
            c == '\"' || c == '\\' || (!allow_space && c == ' ')) {
            return false;
        }
    }
    return true;
}

/* Surface text must match the panel's deliberately tiny 5x7 glyph set. The
 * transport rejects characters that would otherwise turn into invisible
 * spaces on the physical display. */
bool brain_render_text_safe(const char *value, size_t capacity,
                                   bool allow_space)
{
    if (!agent_text_safe(value, capacity, allow_space)) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' ||
            *p == ':' || *p == '/' || *p == '?' || *p == '!' ||
            *p == '+' || *p == ',' || *p == '\'' || *p == '(' ||
            *p == ')' || *p == '&' || *p == '%' ||
            (allow_space && *p == ' ')) {
            continue;
        }
        return false;
    }
    return true;
}

static bool agent_state_valid(const char *state)
{
    return state != NULL &&
        (strcmp(state, "working") == 0 || strcmp(state, "verifying") == 0 ||
         strcmp(state, "waiting") == 0 || strcmp(state, "succeeded") == 0 ||
         strcmp(state, "failed") == 0);
}

static bool evidence_state_valid(const char *state)
{
    return state != NULL &&
        (strcmp(state, "pass") == 0 || strcmp(state, "working") == 0 ||
         strcmp(state, "wait") == 0 || strcmp(state, "fail") == 0);
}

static bool brain_surface_kind_parse(const char *name,
                                     jr_display_surface_kind_t *out)
{
    if (name == NULL || out == NULL) return false;
    if (strcmp(name, "notice") == 0) *out = JR_DISPLAY_SURFACE_NOTICE;
    else if (strcmp(name, "progress") == 0) *out = JR_DISPLAY_SURFACE_PROGRESS;
    else if (strcmp(name, "result") == 0) *out = JR_DISPLAY_SURFACE_RESULT;
    else if (strcmp(name, "choice") == 0) *out = JR_DISPLAY_SURFACE_CHOICE;
    else if (strcmp(name, "consent") == 0) *out = JR_DISPLAY_SURFACE_CONSENT;
    else return false;
    return true;
}

static const char *brain_surface_kind_name(jr_display_surface_kind_t kind)
{
    switch (kind) {
    case JR_DISPLAY_SURFACE_PROGRESS: return "progress";
    case JR_DISPLAY_SURFACE_RESULT:   return "result";
    case JR_DISPLAY_SURFACE_CHOICE:   return "choice";
    case JR_DISPLAY_SURFACE_CONSENT:  return "consent";
    case JR_DISPLAY_SURFACE_NOTICE:
    default:                          return "notice";
    }
}

void brain_surface_expire(uint32_t now)
{
    if (s_brain_lock != NULL &&
        xSemaphoreTake(s_brain_lock, 0) == pdTRUE) {
        if (s_brain_surface.active &&
            (int32_t)(now - s_brain_surface.expires_ms) >= 0) {
            if (s_brain_surface.local_owned && s_tool_consent.active) {
                device_tool_resolve_consent_locked(TOOL_CONSENT_TIMEOUT);
                xSemaphoreGive(s_brain_lock);
                return;
            }
            /* Brain state and the glass compositor are one transaction. Keep
             * the lock order brain -> display everywhere so a newer present
             * cannot be erased by an older deferred dismiss. */
            jr_display_surface_dismiss();
            s_brain_surface.active = false;
            configASSERT(!jr_display_surface_is_active());
        }
        xSemaphoreGive(s_brain_lock);
    }
}

/* Returns true whenever a Desk surface owns the tap, even if the tap missed a
 * choice button. This prevents a card interaction from accidentally toggling
 * the always-ready microphone underneath it. */
bool brain_surface_handle_tap(uint16_t x, uint16_t y, uint32_t now,
                                     bool physical, uint32_t emitted_ms)
{
    /* Read glass ownership before taking the Brain mutex. If the mutex is
     * contended, swallowing a tap is safer than letting a visible card toggle
     * the always-ready microphone underneath it. */
    bool glass_owned = jr_display_surface_is_active();
    bool owned = glass_owned;
    if (s_brain_lock == NULL) {
        return glass_owned;
    }
    if (xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        /* A contended Brain transaction may be installing a card right now.
         * Lose one tap rather than route it to the microphone underneath. */
        return true;
    }
    if (s_brain_surface.active &&
        (int32_t)(now - s_brain_surface.expires_ms) < 0) {
        owned = true;
        int action_index = jr_display_surface_hit_test(x, y);
        if (s_brain_surface.local_owned) {
            if (!physical || emitted_ms < s_tool_consent.presented_ms) {
                ESP_LOGW(TAG, "consent ignored non-physical/stale tap");
                xSemaphoreGive(s_brain_lock);
                return true;
            }
            if (action_index == 0) {
                device_tool_resolve_consent_locked(TOOL_CONSENT_DENY);
            } else if (action_index == 1) {
                device_tool_resolve_consent_locked(TOOL_CONSENT_ALLOW);
            }
            xSemaphoreGive(s_brain_lock);
            return true;
        }
        uint8_t count = s_brain_surface.view.action_count;
        const char *action_id = NULL;
        if (action_index >= 0 && action_index < count) {
            action_id = s_brain_surface.action_ids[action_index];
        } else if (count == 0U &&
                   s_brain_surface.view.kind != JR_DISPLAY_SURFACE_PROGRESS) {
            action_id = "dismiss";
        }
        if (action_id != NULL) {
            uint32_t seq = ++s_brain_event_seq;
            brain_action_event_t *event =
                &s_brain_events[(seq - 1U) % BRAIN_EVENT_CAP];
            memset(event, 0, sizeof *event);
            event->seq = seq;
            event->ts_ms = now;
            strlcpy(event->session, s_brain_surface.session,
                    sizeof event->session);
            strlcpy(event->id, s_brain_surface.id, sizeof event->id);
            strlcpy(event->action_id, action_id, sizeof event->action_id);
            jr_display_surface_dismiss();
            s_brain_surface.active = false;
            configASSERT(!jr_display_surface_is_active());
        }
    } else if (s_brain_surface.active) {
        /* The expiry edge still owns this physical tap. Otherwise the user can
         * touch a card that has not yet been repainted away and accidentally
         * stop or resume voice in the same input event. */
        owned = true;
        if (s_brain_surface.local_owned && s_tool_consent.active) {
            device_tool_resolve_consent_locked(TOOL_CONSENT_TIMEOUT);
        } else {
            jr_display_surface_dismiss();
            s_brain_surface.active = false;
        }
        configASSERT(!jr_display_surface_is_active());
    } else if (glass_owned) {
        /* Defensive repair for any pre-existing state/glass skew. */
        jr_display_surface_dismiss();
        configASSERT(!jr_display_surface_is_active());
    }
    xSemaphoreGive(s_brain_lock);
    return owned;
}


/* PANIC-HOME CLEARS THE WHOLE GLASS, NOT MOST OF IT.
 *
 * BOOT held past 5 s is the owner's last resort, so it has to win against
 * every tenant. The old service block cleared the overlays and left the two
 * lanes that replace the ENTIRE frame — a diagnostic test pattern and a
 * pushed canvas — plus the touch challenge, which consumes taps before
 * cards. A panic-home under any of those visibly did nothing.
 *
 * It also dismissed the brain surface without s_brain_lock and without
 * clearing s_brain_surface.active, the one invariant every other dismiss
 * site keeps (brain state and glass are one transaction, lock order brain ->
 * display). A local consent prompt is resolved as a timeout, i.e. denial —
 * a panic gesture must never count as approval. */
void panic_home_clear_glass(void)
{
    demo_stop();
    atomic_store(&s_touch_challenge_start_requested, false);
    atomic_store(&s_touch_challenge_cancel_requested, false);
    atomic_store(&s_touch_challenge_active, false);
    atomic_store(&s_touch_challenge_restore_ms, 0U);
    (void)jr_display_set_test_pattern(JR_DISPLAY_TEST_OFF);
    jr_display_canvas_clear();
    if (s_brain_lock != NULL &&
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_brain_surface.local_owned && s_tool_consent.active) {
            device_tool_resolve_consent_locked(TOOL_CONSENT_TIMEOUT);
        } else {
            jr_display_surface_dismiss();
            s_brain_surface.active = false;
        }
        configASSERT(!jr_display_surface_is_active());
        xSemaphoreGive(s_brain_lock);
    } else {
        /* The glass must clear even if the brain is busy; the state skew is
         * repaired by the next brain_surface_expire() pass. */
        jr_display_surface_dismiss();
        ESP_LOGW(TAG, "ui: panic-home cleared glass without brain lock");
    }
    jr_display_dismiss_choices();
    jr_display_nav_home();
    s_ui_shade_open = false;
    s_watch_peek_until_ms = 0U;
    s_hold_start_ms = 0U;
    jr_display_commit_ring(0U);
}

bool operator_mode_release(uint32_t now, const char *reason,
                                  bool physical_feedback,
                                  bool only_if_expired)
{
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (only_if_expired &&
        (!atomic_load(&s_operator_mode_active) ||
         operator_lease_active(now))) {
        xSemaphoreGive(s_brain_lock);
        return false;
    }

    atomic_store(&s_operator_mode_active, false);
    atomic_store(&s_operator_mode_entered_ms, 0U);
    atomic_store(&s_operator_lease_until_ms, 0U);
    if (s_brain_surface.active && !s_brain_surface.local_owned) {
        jr_display_surface_dismiss();
        s_brain_surface.active = false;
    }
    const bool privacy_held =
        atomic_load(&s_voice_privacy_paused) || s_flip_muted;
    atomic_store(&s_voice_control_request, VOICE_CONTROL_RESUME);
    if (privacy_held) {
        jr_display_caption_set("MUTED - HOLD TO RESUME");
    } else {
        jr_mood_poke_awake(&s_mood, now);
        jr_display_caption_set("LISTENING");
    }
    xSemaphoreGive(s_brain_lock);

    if (physical_feedback && !privacy_held) {
        jr_display_bloom();
        (void)jr_audio_diag_play_chirp(160U, 8U);
    }
    ESP_LOGI(TAG, "operator: Codex mode released (%s)", reason);
    return true;
}

static esp_err_t agent_link_get_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    agent_link_state_t state = {0};
    uint32_t revision_hwm = 0U;
    uint32_t next_revision = 0U;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_agent_link_lock == NULL ||
        xSemaphoreTake(s_agent_link_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "agent link unavailable");
        return ESP_OK;
    }
    if (s_agent_link.active &&
        (int32_t)(now - s_agent_link.expires_ms) >= 0) {
        s_agent_link.active = false;
    }
    state = s_agent_link;
    revision_hwm = s_agent_link_revision_hwm;
    next_revision = revision_hwm + 1U;
    xSemaphoreGive(s_agent_link_lock);

    char body[1024];
    size_t used = 0U;
    int n;
    if (!state.active) {
        n = snprintf(body, sizeof body,
            "{\"active\":false,\"revision_hwm\":%u,\"next_revision\":%u,"
            "\"updates\":%u,\"rejects\":%u}",
            (unsigned)revision_hwm, (unsigned)next_revision,
            (unsigned)state.updates, (unsigned)state.rejects);
    } else {
        uint32_t ttl_ms = (int32_t)(state.expires_ms - now) > 0
            ? state.expires_ms - now : 0U;
        n = snprintf(body, sizeof body,
            "{\"active\":true,\"task_id\":\"%s\",\"revision\":%u,"
            "\"state\":\"%s\",\"progress\":%u,\"title\":\"%s\","
            "\"summary\":\"%s\",\"ttl_ms\":%u,\"evidence\":[",
            state.task_id, (unsigned)state.revision, state.state,
            (unsigned)state.progress, state.title, state.summary,
            (unsigned)ttl_ms);
        if (n > 0 && (size_t)n < sizeof body) {
            used = (size_t)n;
            for (uint8_t i = 0; i < state.evidence_count; ++i) {
                n = snprintf(body + used, sizeof body - used,
                    "%s{\"label\":\"%s\",\"state\":\"%s\"}",
                    i == 0 ? "" : ",", state.evidence[i].label,
                    state.evidence[i].state);
                if (n < 0 || (size_t)n >= sizeof body - used) {
                    used = sizeof body;
                    break;
                }
                used += (size_t)n;
            }
            if (used < sizeof body) {
                n = snprintf(body + used, sizeof body - used,
                             "],\"revision_hwm\":%u,\"next_revision\":%u,"
                             "\"updates\":%u,\"rejects\":%u}",
                             (unsigned)revision_hwm,
                             (unsigned)next_revision,
                             (unsigned)state.updates,
                             (unsigned)state.rejects);
                if (n > 0 && (size_t)n < sizeof body - used) {
                    used += (size_t)n;
                    n = (int)used;
                } else {
                    n = -1;
                }
            } else {
                n = -1;
            }
        }
    }
    if (n < 0 || (size_t)n >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "agent link encoding failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t agent_link_post_handler(httpd_req_t *req)
{
    /* The one mutating POST that skipped the control-intent gate. With
     * JR_DEV_OPEN_DIAGNOSTICS on, agent_require_auth() is unconditionally
     * true, so this route had NO gate at all: any page the owner visited could
     * push agent-link state cross-origin, and it bypassed the 423 consent
     * lock every other mutating route honours. */
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 1536) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "JSON payload must be 1..1536 bytes");
        return ESP_OK;
    }
    size_t length = (size_t)req->content_len;
    char *payload = heap_caps_malloc(length + 1U,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "device busy");
        return ESP_OK;
    }
    size_t received = 0U;
    while (received < length) {
        int got = httpd_req_recv(req, payload + received, length - received);
        if (got <= 0) {
            secure_zero(payload, length + 1U);
            heap_caps_free(payload);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)got;
    }
    payload[length] = '\0';
    cJSON *root = json_depth_within(payload, length, 6U)
        ? cJSON_ParseWithLengthOpts(payload, length + 1U, NULL, true)
        : NULL;
    secure_zero(payload, length + 1U);
    heap_caps_free(payload);

    agent_link_state_t next = {0};
    uint32_t ttl_s = 900U;
    bool valid = root != NULL && cJSON_IsObject(root);
    cJSON *task_id = cJSON_GetObjectItemCaseSensitive(root, "task_id");
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    cJSON *progress = cJSON_GetObjectItemCaseSensitive(root, "progress");
    cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    cJSON *summary = cJSON_GetObjectItemCaseSensitive(root, "summary");
    cJSON *evidence = cJSON_GetObjectItemCaseSensitive(root, "evidence");
    cJSON *ttl = cJSON_GetObjectItemCaseSensitive(root, "ttl_s");
    enum {
        AGENT_FIELD_TASK_ID  = 1U << 0,
        AGENT_FIELD_REVISION = 1U << 1,
        AGENT_FIELD_STATE    = 1U << 2,
        AGENT_FIELD_PROGRESS = 1U << 3,
        AGENT_FIELD_TITLE    = 1U << 4,
        AGENT_FIELD_SUMMARY  = 1U << 5,
        AGENT_FIELD_EVIDENCE = 1U << 6,
        AGENT_FIELD_TTL      = 1U << 7,
    };
    const unsigned required_fields = AGENT_FIELD_TASK_ID |
        AGENT_FIELD_REVISION | AGENT_FIELD_STATE | AGENT_FIELD_PROGRESS |
        AGENT_FIELD_TITLE | AGENT_FIELD_SUMMARY;
    unsigned seen_fields = 0U;
    for (cJSON *it = valid ? root->child : NULL; it != NULL; it = it->next) {
        unsigned field = 0U;
        if (it->string == NULL) {
            valid = false;
        } else if (strcmp(it->string, "task_id") == 0) {
            field = AGENT_FIELD_TASK_ID;
        } else if (strcmp(it->string, "revision") == 0) {
            field = AGENT_FIELD_REVISION;
        } else if (strcmp(it->string, "state") == 0) {
            field = AGENT_FIELD_STATE;
        } else if (strcmp(it->string, "progress") == 0) {
            field = AGENT_FIELD_PROGRESS;
        } else if (strcmp(it->string, "title") == 0) {
            field = AGENT_FIELD_TITLE;
        } else if (strcmp(it->string, "summary") == 0) {
            field = AGENT_FIELD_SUMMARY;
        } else if (strcmp(it->string, "evidence") == 0) {
            field = AGENT_FIELD_EVIDENCE;
        } else if (strcmp(it->string, "ttl_s") == 0) {
            field = AGENT_FIELD_TTL;
        } else {
            valid = false;
        }
        if (field != 0U) {
            if ((seen_fields & field) != 0U) {
                valid = false;
            }
            seen_fields |= field;
        }
    }
    valid = valid && (seen_fields & required_fields) == required_fields &&
        cJSON_IsString(task_id) && cJSON_IsNumber(revision) &&
        revision->valuedouble == (double)revision->valueint &&
        revision->valueint >= 1 && cJSON_IsString(state) &&
        cJSON_IsNumber(progress) &&
        progress->valuedouble == (double)progress->valueint &&
        progress->valueint >= 0 && progress->valueint <= 100 &&
        cJSON_IsString(title) && cJSON_IsString(summary) &&
        agent_text_safe(task_id->valuestring, sizeof next.task_id, false) &&
        agent_text_safe(title->valuestring, sizeof next.title, true) &&
        agent_text_safe(summary->valuestring, sizeof next.summary, true) &&
        agent_state_valid(state->valuestring);
    if (valid && ttl != NULL) {
        valid = cJSON_IsNumber(ttl) &&
            ttl->valuedouble == (double)ttl->valueint &&
            ttl->valueint >= 30 && ttl->valueint <= 3600;
        if (valid) {
            ttl_s = (uint32_t)ttl->valueint;
        }
    }
    if (valid && evidence != NULL) {
        valid = cJSON_IsArray(evidence) &&
                cJSON_GetArraySize(evidence) <= AGENT_EVIDENCE_CAP;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, evidence) {
            cJSON *label = cJSON_GetObjectItemCaseSensitive(item, "label");
            cJSON *ev_state = cJSON_GetObjectItemCaseSensitive(item, "state");
            unsigned item_fields = 0U;
            for (cJSON *field = cJSON_IsObject(item) ? item->child : NULL;
                 field != NULL; field = field->next) {
                if (field->string == NULL ||
                    (strcmp(field->string, "label") != 0 &&
                     strcmp(field->string, "state") != 0)) {
                    valid = false;
                }
                item_fields++;
            }
            valid = valid && cJSON_IsObject(item) && item_fields == 2U &&
                    label != NULL && ev_state != NULL && label != ev_state &&
                    cJSON_IsString(label) && cJSON_IsString(ev_state) &&
                    agent_text_safe(label->valuestring,
                                    AGENT_LABEL_CAP, true) &&
                    evidence_state_valid(ev_state->valuestring);
            if (!valid) {
                break;
            }
            strlcpy(next.evidence[next.evidence_count].label,
                    label->valuestring, AGENT_LABEL_CAP);
            strlcpy(next.evidence[next.evidence_count].state,
                    ev_state->valuestring, AGENT_STATE_CAP);
            next.evidence_count++;
        }
    }
    if (!valid) {
        cJSON_Delete(root);
        if (s_agent_link_lock != NULL &&
            xSemaphoreTake(s_agent_link_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_agent_link.rejects++;
            xSemaphoreGive(s_agent_link_lock);
        }
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"invalid bounded Agent Link payload\"}");
        return ESP_OK;
    }

    strlcpy(next.task_id, task_id->valuestring, sizeof next.task_id);
    next.revision = (uint32_t)revision->valueint;
    strlcpy(next.state, state->valuestring, sizeof next.state);
    next.progress = (uint8_t)progress->valueint;
    strlcpy(next.title, title->valuestring, sizeof next.title);
    strlcpy(next.summary, summary->valuestring, sizeof next.summary);
    cJSON_Delete(root);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    next.active = true;
    next.updated_ms = now;
    next.expires_ms = now + ttl_s * 1000U;

    if (s_agent_link_lock == NULL ||
        xSemaphoreTake(s_agent_link_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "agent link busy");
        return ESP_OK;
    }
    uint32_t expected_revision = s_agent_link_revision_hwm + 1U;
    if (next.revision != expected_revision) {
        uint32_t revision_hwm = s_agent_link_revision_hwm;
        s_agent_link.rejects++;
        xSemaphoreGive(s_agent_link_lock);
        char conflict[160];
        int conflict_len = snprintf(conflict, sizeof conflict,
            "{\"ok\":false,\"error\":\"revision must equal next_revision\","
            "\"revision_hwm\":%u,\"next_revision\":%u}",
            (unsigned)revision_hwm, (unsigned)expected_revision);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        if (conflict_len > 0 && (size_t)conflict_len < sizeof conflict) {
            httpd_resp_send(req, conflict, conflict_len);
        } else {
            httpd_resp_sendstr(req,
                "{\"ok\":false,\"error\":\"invalid revision sequence\"}");
        }
        return ESP_OK;
    }
    next.updates = s_agent_link.updates + 1U;
    next.rejects = s_agent_link.rejects;
    s_agent_link_revision_hwm = next.revision;
    s_agent_link = next;
    xSemaphoreGive(s_agent_link_lock);

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"accepted\":true}");
    return ESP_OK;
}

static esp_err_t brain_inbox_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 1536) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "brain envelope must be 1..1536 bytes");
        return ESP_OK;
    }
    size_t length = (size_t)req->content_len;
    char *raw = heap_caps_malloc(length + 1U,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "brain inbox busy");
        return ESP_OK;
    }
    size_t received = 0U;
    while (received < length) {
        int got = httpd_req_recv(req, raw + received, length - received);
        if (got <= 0) {
            secure_zero(raw, length + 1U);
            heap_caps_free(raw);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "brain body read failed");
            return ESP_OK;
        }
        received += (size_t)got;
    }
    raw[length] = '\0';
    cJSON *root = json_depth_within(raw, length, 5U)
        ? cJSON_ParseWithLengthOpts(raw, length + 1U, NULL, true) : NULL;
    secure_zero(raw, length + 1U);
    heap_caps_free(raw);

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *ttl_ms = cJSON_GetObjectItemCaseSensitive(root, "ttl_ms");
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    bool valid = root != NULL && cJSON_IsObject(root);
    unsigned seen = 0U;
    enum {
        BF_VERSION = 1U << 0, BF_TYPE = 1U << 1, BF_SEQ = 1U << 2,
        BF_SESSION = 1U << 3, BF_ID = 1U << 4, BF_TTL = 1U << 5,
        BF_PAYLOAD = 1U << 6,
    };
    for (cJSON *it = valid ? root->child : NULL; it != NULL; it = it->next) {
        unsigned bit = 0U;
        if (it->string == NULL) valid = false;
        else if (strcmp(it->string, "v") == 0) bit = BF_VERSION;
        else if (strcmp(it->string, "type") == 0) bit = BF_TYPE;
        else if (strcmp(it->string, "seq") == 0) bit = BF_SEQ;
        else if (strcmp(it->string, "session") == 0) bit = BF_SESSION;
        else if (strcmp(it->string, "id") == 0) bit = BF_ID;
        else if (strcmp(it->string, "ttl_ms") == 0) bit = BF_TTL;
        else if (strcmp(it->string, "payload") == 0) bit = BF_PAYLOAD;
        else valid = false;
        if (bit != 0U) {
            if ((seen & bit) != 0U) valid = false;
            seen |= bit;
        }
    }
    const unsigned required = BF_VERSION | BF_TYPE | BF_SEQ | BF_SESSION |
        BF_ID | BF_TTL | BF_PAYLOAD;
    valid = valid && seen == required && cJSON_IsNumber(version) &&
        version->valuedouble == 1.0 && cJSON_IsString(type) &&
        cJSON_IsNumber(seq) && seq->valuedouble == (double)seq->valueint &&
        seq->valueint >= 1 && cJSON_IsString(session) && cJSON_IsString(id) &&
        agent_text_safe(session->valuestring, BRAIN_SESSION_CAP, false) &&
        agent_text_safe(id->valuestring, BRAIN_SURFACE_ID_CAP, false) &&
        cJSON_IsNumber(ttl_ms) &&
        ttl_ms->valuedouble == (double)ttl_ms->valueint &&
        ttl_ms->valueint >= 1000 && ttl_ms->valueint <= 600000 &&
        cJSON_IsObject(payload);

    bool dismiss = valid && strcmp(type->valuestring, "surface.dismiss") == 0;
    bool update = valid && strcmp(type->valuestring, "surface.update") == 0;
    bool present = valid &&
        (strcmp(type->valuestring, "surface.present") == 0 || update);
    valid = valid && (dismiss || present);

    brain_surface_state_t next = {0};
    if (valid && present) {
        cJSON *kind = cJSON_GetObjectItemCaseSensitive(payload, "kind");
        cJSON *title = cJSON_GetObjectItemCaseSensitive(payload, "title");
        cJSON *body = cJSON_GetObjectItemCaseSensitive(payload, "body");
        cJSON *actions = cJSON_GetObjectItemCaseSensitive(payload, "actions");
        unsigned p_seen = 0U;
        enum { PF_KIND=1U, PF_TITLE=2U, PF_BODY=4U, PF_ACTIONS=8U };
        for (cJSON *it = payload->child; it != NULL; it = it->next) {
            unsigned bit = 0U;
            if (it->string == NULL) valid = false;
            else if (strcmp(it->string, "kind") == 0) bit = PF_KIND;
            else if (strcmp(it->string, "title") == 0) bit = PF_TITLE;
            else if (strcmp(it->string, "body") == 0) bit = PF_BODY;
            else if (strcmp(it->string, "actions") == 0) bit = PF_ACTIONS;
            else valid = false;
            if (bit != 0U) {
                if ((p_seen & bit) != 0U) valid = false;
                p_seen |= bit;
            }
        }
        valid = valid && p_seen == (PF_KIND|PF_TITLE|PF_BODY|PF_ACTIONS) &&
            cJSON_IsString(kind) && cJSON_IsString(title) &&
            cJSON_IsString(body) && cJSON_IsArray(actions) &&
            cJSON_GetArraySize(actions) <= JR_DISPLAY_SURFACE_ACTION_CAP &&
            brain_surface_kind_parse(kind->valuestring, &next.view.kind) &&
            brain_render_text_safe(title->valuestring,
                                   JR_DISPLAY_SURFACE_TITLE_CAP, true) &&
            brain_render_text_safe(body->valuestring,
                                   JR_DISPLAY_SURFACE_BODY_CAP, true);
        cJSON *action = NULL;
        cJSON_ArrayForEach(action, actions) {
            cJSON *action_id = cJSON_GetObjectItemCaseSensitive(action, "id");
            cJSON *label = cJSON_GetObjectItemCaseSensitive(action, "label");
            unsigned fields = 0U;
            for (cJSON *field = cJSON_IsObject(action) ? action->child : NULL;
                 field != NULL; field = field->next) {
                if (field->string == NULL ||
                    (strcmp(field->string, "id") != 0 &&
                     strcmp(field->string, "label") != 0)) valid = false;
                fields++;
            }
            valid = valid && cJSON_IsObject(action) && fields == 2U &&
                cJSON_IsString(action_id) && cJSON_IsString(label) &&
                agent_text_safe(action_id->valuestring,
                                BRAIN_ACTION_ID_CAP, false) &&
                brain_render_text_safe(label->valuestring,
                                       JR_DISPLAY_SURFACE_LABEL_CAP, true);
            if (!valid) break;
            uint8_t index = next.view.action_count++;
            strlcpy(next.action_ids[index], action_id->valuestring,
                    sizeof next.action_ids[index]);
            strlcpy(next.view.action_labels[index], label->valuestring,
                    sizeof next.view.action_labels[index]);
        }
        if (valid) {
            if (next.view.kind == JR_DISPLAY_SURFACE_CONSENT) {
                valid = next.view.action_count == 2U;
            } else if (next.view.kind == JR_DISPLAY_SURFACE_CHOICE) {
                valid = next.view.action_count >= 2U;
            } else if (next.view.kind == JR_DISPLAY_SURFACE_PROGRESS) {
                valid = next.view.action_count == 0U;
            }
        }
        if (valid) {
            strlcpy(next.view.title, title->valuestring,
                    sizeof next.view.title);
            strlcpy(next.view.body, body->valuestring,
                    sizeof next.view.body);
        }
    } else if (valid && dismiss && payload->child != NULL) {
        valid = false;
    }

    if (!valid) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"invalid bounded brain envelope\"}");
        return ESP_OK;
    }
    uint32_t envelope_seq = (uint32_t)seq->valueint;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    strlcpy(next.session, session->valuestring, sizeof next.session);
    strlcpy(next.id, id->valuestring, sizeof next.id);
    next.inbox_seq = envelope_seq;
    next.expires_ms = now + (uint32_t)ttl_ms->valueint;
    next.active = present;
    cJSON_Delete(root);

    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "brain link unavailable");
        return ESP_OK;
    }
    if (s_brain_surface.local_owned) {
        /* A paired network client is still not a finger on this panel. Local
         * write consent cannot be replaced, updated, or dismissed remotely. */
        xSemaphoreGive(s_brain_lock);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"physical consent owns panel\"}");
        return ESP_OK;
    }
    uint32_t expected = s_brain_inbox_seq_hwm + 1U;
    if (envelope_seq != expected) {
        xSemaphoreGive(s_brain_lock);
        char conflict[128];
        int n = snprintf(conflict, sizeof conflict,
            "{\"ok\":false,\"error\":\"seq must equal next_inbox_seq\","
            "\"next_inbox_seq\":%u}", (unsigned)expected);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, conflict,
                        n > 0 && (size_t)n < sizeof conflict ? n : 0);
        return ESP_OK;
    }
    /* Updates and dismissals are conditional operations. A delayed client may
     * not mutate a newer surface merely because it owns the same pairing key. */
    if ((update || dismiss) &&
        (!s_brain_surface.active ||
         strcmp(s_brain_surface.session, next.session) != 0 ||
         strcmp(s_brain_surface.id, next.id) != 0)) {
        xSemaphoreGive(s_brain_lock);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"surface target is not active\"}");
        return ESP_OK;
    }
    esp_err_t surface_err = ESP_OK;
    if (present) {
        surface_err = jr_display_surface_present(&next.view);
    } else {
        jr_display_surface_dismiss();
    }
    if (surface_err != ESP_OK) {
        xSemaphoreGive(s_brain_lock);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"panel surface unavailable\"}");
        return ESP_OK;
    }
    s_brain_inbox_seq_hwm = envelope_seq;
    s_brain_last_seen_ms = now;
    if (present) {
        s_brain_surface = next;
    } else {
        s_brain_surface.active = false;
    }
    configASSERT(s_brain_surface.active == jr_display_surface_is_active());
    uint32_t next_seq = s_brain_inbox_seq_hwm + 1U;
    xSemaphoreGive(s_brain_lock);

    char accepted[96];
    int n = snprintf(accepted, sizeof accepted,
        "{\"ok\":true,\"accepted\":true,\"next_inbox_seq\":%u}",
        (unsigned)next_seq);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, accepted, n);
    return ESP_OK;
}

static esp_err_t brain_outbox_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) return ESP_OK;
    int after_query = 0;
    (void)query_int(req, "after", &after_query);
    uint32_t after = after_query > 0 ? (uint32_t)after_query : 0U;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    brain_surface_state_t surface = {0};
    brain_action_event_t events[BRAIN_EVENT_CAP] = {0};
    size_t event_count = 0U;
    uint32_t latest = 0U;
    uint32_t next_inbox = 1U;
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "brain link unavailable");
        return ESP_OK;
    }
    s_brain_last_seen_ms = now;
    if (s_brain_surface.active && !s_brain_surface.local_owned &&
        (int32_t)(now - s_brain_surface.expires_ms) >= 0) {
        jr_display_surface_dismiss();
        s_brain_surface.active = false;
        configASSERT(!jr_display_surface_is_active());
    }
    surface = s_brain_surface;
    latest = s_brain_event_seq;
    next_inbox = s_brain_inbox_seq_hwm + 1U;
    uint32_t earliest = latest >= BRAIN_EVENT_CAP
        ? latest - BRAIN_EVENT_CAP + 1U : 1U;
    uint32_t first = after + 1U;
    if (first < earliest) first = earliest;
    for (uint32_t seq_no = first;
         seq_no <= latest && event_count < BRAIN_EVENT_CAP; ++seq_no) {
        brain_action_event_t *event =
            &s_brain_events[(seq_no - 1U) % BRAIN_EVENT_CAP];
        if (event->seq == seq_no) events[event_count++] = *event;
    }
    xSemaphoreGive(s_brain_lock);

    char *body = heap_caps_malloc(3072U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "brain outbox busy");
        return ESP_OK;
    }
    int n = snprintf(body, 3072U,
        "{\"v\":1,\"voice_route\":\"cloud_gemini\","
        "\"desk_connected\":true,\"next_after\":%u,"
        "\"next_inbox_seq\":%u,\"surface\":{\"active\":%s",
        (unsigned)latest, (unsigned)next_inbox,
        surface.active ? "true" : "false");
    size_t used = n > 0 ? (size_t)n : 3072U;
    if (surface.active && used < 3072U) {
        n = snprintf(body + used, 3072U - used,
            ",\"session\":\"%s\",\"id\":\"%s\",\"kind\":\"%s\","
            "\"title\":\"%s\",\"body\":\"%s\",\"ttl_ms\":%u",
            surface.session, surface.id,
            brain_surface_kind_name(surface.view.kind), surface.view.title,
            surface.view.body,
            (unsigned)((int32_t)(surface.expires_ms - now) > 0
                ? surface.expires_ms - now : 0U));
        used += n > 0 ? (size_t)n : 3072U;
    }
    if (used < 3072U) {
        n = snprintf(body + used, 3072U - used, "},\"events\":[");
        used += n > 0 ? (size_t)n : 3072U;
    }
    for (size_t i = 0; i < event_count && used < 3072U; ++i) {
        n = snprintf(body + used, 3072U - used,
            "%s{\"v\":1,\"seq\":%u,\"type\":\"surface.action\","
            "\"session\":\"%s\",\"id\":\"%s\","
            "\"payload\":{\"action_id\":\"%s\",\"ts_ms\":%u}}",
            i == 0U ? "" : ",", (unsigned)events[i].seq,
            events[i].session, events[i].id, events[i].action_id,
            (unsigned)events[i].ts_ms);
        used += n > 0 ? (size_t)n : 3072U;
    }
    if (used < 3072U) {
        n = snprintf(body + used, 3072U - used, "]}");
        used += n > 0 ? (size_t)n : 3072U;
    }
    if (used >= 3072U) {
        heap_caps_free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "brain outbox encoding overflow");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, used);
    heap_caps_free(body);
    return err;
}

static esp_err_t cockpit_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const jr_state_snapshot_t *voice = jr_orch_snapshot(&s_app.orch);
    jr_display_diag_t display = {0};
    jr_net_status_t net = {0};
    agent_link_state_t agent = {0};
    uint32_t agent_revision_hwm = 0U;
    uint32_t agent_next_revision = 0U;
    brain_surface_state_t brain_surface = {0};
    uint32_t brain_inbox_next = 1U;
    uint32_t brain_event_cursor = 0U;
    bool desk_connected = false;
    (void)jr_display_get_diag(&display);
    (void)jr_net_get_status(&net);
    if (s_agent_link_lock != NULL &&
        xSemaphoreTake(s_agent_link_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_agent_link.active &&
            (int32_t)(now - s_agent_link.expires_ms) >= 0) {
            s_agent_link.active = false;
        }
        agent = s_agent_link;
        agent_revision_hwm = s_agent_link_revision_hwm;
        agent_next_revision = agent_revision_hwm + 1U;
        xSemaphoreGive(s_agent_link_lock);
    }
    if (s_brain_lock != NULL &&
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_brain_surface.active && !s_brain_surface.local_owned &&
            (int32_t)(now - s_brain_surface.expires_ms) >= 0) {
            jr_display_surface_dismiss();
            s_brain_surface.active = false;
            configASSERT(!jr_display_surface_is_active());
        }
        brain_surface = s_brain_surface;
        brain_inbox_next = s_brain_inbox_seq_hwm + 1U;
        brain_event_cursor = s_brain_event_seq;
        desk_connected = s_brain_last_seen_ms != 0U &&
            (uint32_t)(now - s_brain_last_seen_ms) <= BRAIN_DESK_FRESH_MS;
        xSemaphoreGive(s_brain_lock);
    }

    char *body = heap_caps_malloc(4096U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "cockpit snapshot unavailable");
        return ESP_OK;
    }
    /* Always 0: this firmware is always-ready listening (VOICE_ALWAYS_READY),
     * so there is no listen window to count down. The deadline it used to read
     * was assigned only ever 0 at four sites. Kept as a field so existing
     * tooling keeps parsing; do not build a countdown rim on it. */
    const uint32_t auto_idle_ms = 0U;
    bool challenge_active = atomic_load(&s_touch_challenge_active);
    bool challenge_verified = atomic_load(&s_touch_challenge_verified);
    bool tools_ready = atomic_load(&s_tool_diag.worker_ready);
    bool tools_configured = tools_ready && jr_tools_is_configured();
    size_t used = 0U;
    int n = snprintf(body, 4096U,
        "{\"uptime_ms\":%u,\"memory\":{\"free_internal\":%u,"
        "\"largest_internal_block\":%u,\"free_psram\":%u},"
        "\"network\":{\"connected\":%s,"
        "\"ip\":\"%s\",\"rssi\":%d},\"voice\":{\"phase\":\"%s\","
        "\"voice_armed\":%s,\"always_ready\":true,"
        "\"privacy_paused\":%s,\"mood\":\"%s\",\"brightness\":%u,"
        "\"rtc\":%s,\"capturing\":%s,\"ws_connected\":%s,"
        "\"auto_idle_ms\":%u,\"mic_rms\":%.1f,\"clean_rms\":%.1f,"
        "\"vad_clean\":%s,"
        "\"vad_starts\":%u,"
        "\"audio_diag_running\":%s},\"tools\":{"
        "\"execution\":\"on_device\",\"worker_ready\":%s,"
        "\"configured\":%s,\"declared\":%u,\"last_tool\":\"%s\","
        "\"last_status\":\"%s\",\"last_http_status\":%d,"
        "\"last_duration_ms\":%u,"
        "\"calls_received\":%u,\"submitted\":%u,\"submit_rejected\":%u,"
        "\"completed\":%u,"
        "\"succeeded\":%u,\"failed\":%u,\"cancelled\":%u,"
        "\"stale_dropped\":%u,\"responses_sent\":%u,"
        "\"response_send_failed\":%u,\"consent_active\":%s,"
        "\"consent_prompted\":%u,\"consent_approved\":%u,"
        "\"consent_denied\":%u,\"consent_timed_out\":%u,"
        "\"consent_cancelled\":%u},\"display\":{\"init\":\"%s\","
        "\"actual_fps\":%u,\"flush_completions\":%u,\"flush_errors\":%u,"
        "\"requested_face\":%d,\"applied_face\":%d,"
        "\"choices_active\":%s},\"touch\":{"
        "\"events\":%u,\"last\":{\"kind\":\"%s\",\"x\":%u,\"y\":%u},"
        "\"shade_open\":%s,\"panel_touch_challenge\":{\"pending\":%s,"
        "\"active\":%s,\"verified\":%s,\"correct_rounds\":%u,"
        "\"wrong\":%u,\"expected_sector\":%u,\"last_latency_ms\":%u}},"
        "\"agent\":{\"active\":%s,\"revision_hwm\":%u,"
        "\"next_revision\":%u",
        (unsigned)now,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        net.sta_connected ? "true" : "false", net.sta_ip,
        (int)net.rssi, jr_state_name(voice->phase),
        voice->phase != JR_ST_IDLE && voice->phase != JR_ST_DRAINING &&
            voice->phase != JR_ST_FATAL ? "true" : "false",
        atomic_load(&s_voice_privacy_paused) ? "true" : "false",
        jr_mood_name((jr_mood_t)atomic_load(&s_mood_id)),
        (unsigned)atomic_load(&s_mood_brightness),
        jr_rtc_present() ? "true" : "false",
        s_app.io.capturing ? "true" : "false",
        s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN ? "true" : "false",
        (unsigned)auto_idle_ms, (double)s_app.mic_rms,
        (double)jr_audio_clean_rms(),
        atomic_load(&s_vad_use_clean) ? "true" : "false",
        (unsigned)s_app.vad_starts,
        (int32_t)(atomic_load(&s_audio_diag_until_ms) - now) > 0
            ? "true" : "false",
        tools_ready ? "true" : "false",
        tools_configured ? "true" : "false",
        (unsigned)DEVICE_TOOL_DECL_COUNT,
        device_tool_last_name(),
        device_tool_last_status(),
        atomic_load(&s_tool_diag.last_http_status),
        (unsigned)atomic_load(&s_tool_diag.last_duration_ms),
        (unsigned)atomic_load(&s_tool_diag.calls_received),
        (unsigned)atomic_load(&s_tool_diag.submitted),
        (unsigned)atomic_load(&s_tool_diag.submit_rejected),
        (unsigned)atomic_load(&s_tool_diag.completed),
        (unsigned)atomic_load(&s_tool_diag.succeeded),
        (unsigned)atomic_load(&s_tool_diag.failed),
        (unsigned)atomic_load(&s_tool_diag.cancelled),
        (unsigned)atomic_load(&s_tool_diag.stale_dropped),
        (unsigned)atomic_load(&s_tool_diag.responses_sent),
        (unsigned)atomic_load(&s_tool_diag.response_send_failed),
        atomic_load(&s_tool_diag.consent_active) ? "true" : "false",
        (unsigned)atomic_load(&s_tool_diag.consent_prompted),
        (unsigned)atomic_load(&s_tool_diag.consent_approved),
        (unsigned)atomic_load(&s_tool_diag.consent_denied),
        (unsigned)atomic_load(&s_tool_diag.consent_timed_out),
        (unsigned)atomic_load(&s_tool_diag.consent_cancelled),
        display.init_state == JR_DISPLAY_INIT_READY ? "ready" :
        display.init_state == JR_DISPLAY_INIT_STARTING ? "starting" :
        display.init_state == JR_DISPLAY_INIT_FAILED ? "failed" : "stopped",
        (unsigned)display.actual_fps,
        (unsigned)display.flush_completions,
        (unsigned)display.flush_errors,
        (int)display.requested_face, (int)display.applied_face,
        jr_display_choices_active() ? "true" : "false",
        (unsigned)atomic_load(&s_touch_events),
        touch_kind_name((jr_input_kind_t)atomic_load(&s_touch_last_kind)),
        (unsigned)atomic_load(&s_touch_last_x),
        (unsigned)atomic_load(&s_touch_last_y),
        s_ui_shade_open ? "true" : "false",
        atomic_load(&s_touch_challenge_start_requested) ? "true" : "false",
        challenge_active ? "true" : "false",
        challenge_verified ? "true" : "false",
        (unsigned)atomic_load(&s_touch_challenge_correct),
        (unsigned)atomic_load(&s_touch_challenge_wrong),
        (unsigned)atomic_load(&s_touch_challenge_expected),
        (unsigned)atomic_load(&s_touch_challenge_last_latency_ms),
        agent.active ? "true" : "false",
        (unsigned)agent_revision_hwm, (unsigned)agent_next_revision);
    if (n < 0 || (size_t)n >= 4096U) {
        heap_caps_free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "cockpit encoding failed");
        return ESP_OK;
    }
    used = (size_t)n;
    if (agent.active) {
        uint32_t ttl_ms = (int32_t)(agent.expires_ms - now) > 0
            ? agent.expires_ms - now : 0U;
        n = snprintf(body + used, 4096U - used,
            ",\"task_id\":\"%s\",\"revision\":%u,\"state\":\"%s\","
            "\"progress\":%u,\"title\":\"%s\",\"summary\":\"%s\","
            "\"ttl_ms\":%u,\"evidence\":[",
            agent.task_id, (unsigned)agent.revision, agent.state,
            (unsigned)agent.progress, agent.title, agent.summary,
            (unsigned)ttl_ms);
        if (n < 0 || (size_t)n >= 4096U - used) {
            used = 4096U;
        } else {
            used += (size_t)n;
            for (uint8_t i = 0; i < agent.evidence_count; ++i) {
                n = snprintf(body + used, 4096U - used,
                    "%s{\"label\":\"%s\",\"state\":\"%s\"}",
                    i == 0 ? "" : ",", agent.evidence[i].label,
                    agent.evidence[i].state);
                if (n < 0 || (size_t)n >= 4096U - used) {
                    used = 4096U;
                    break;
                }
                used += (size_t)n;
            }
            if (used < 4096U) {
                n = snprintf(body + used, 4096U - used, "]");
                used += n > 0 ? (size_t)n : 4096U;
            }
        }
    }
    if (used < 4096U) {
        uint32_t surface_ttl_ms = brain_surface.active &&
            (int32_t)(brain_surface.expires_ms - now) > 0
            ? brain_surface.expires_ms - now : 0U;
        n = snprintf(body + used, 4096U - used,
            "},\"brain\":{\"voice_route\":\"cloud_gemini\","
            "\"desk_connected\":%s,\"private_android_ready\":false,"
            "\"private_android_reason\":\"BLE firmware not enabled\","
            "\"next_inbox_seq\":%u,\"event_cursor\":%u,"
            "\"surface_active\":%s,\"surface_kind\":\"%s\","
            "\"surface_title\":\"%s\",\"surface_ttl_ms\":%u}}",
            desk_connected ? "true" : "false",
            (unsigned)brain_inbox_next, (unsigned)brain_event_cursor,
            brain_surface.active ? "true" : "false",
            brain_surface.active
                ? brain_surface_kind_name(brain_surface.view.kind) : "none",
            brain_surface.active ? brain_surface.view.title : "",
            (unsigned)surface_ttl_ms);
        if (n > 0 && (size_t)n < 4096U - used) {
            used += (size_t)n;
        } else {
            used = 4096U;
        }
    }
    if (used >= 4096U) {
        heap_caps_free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "cockpit encoding overflow");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, used);
    heap_caps_free(body);
    return err;
}

/* Presenter observability: init state, applied face/bucket, flush + asset
 * counters, actual fps. The "never ask the user what the screen is doing"
 * endpoint — pairs with the /api/gemini/live voice snapshot. */
/* Dump the VAD/barge ring as CSV, oldest -> newest. Columns:
 * t_ms,phase,event,barge_on,rms,floor,gate,peak_play. Streamed in chunks so a
 * 6000-row log never needs a big contiguous buffer. This is the barge-tuning
 * data source: have the user talk to the device, then GET this. */
static esp_err_t vadlog_csv_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    if (s_vadlog == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "vadlog unavailable");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "t_ms,phase,event,barge_on,rms,floor,gate,peak_play\n");

    uint32_t seq = atomic_load(&s_vadlog_seq);
    uint32_t total = seq < VADLOG_CAP ? seq : VADLOG_CAP;
    uint32_t start = seq < VADLOG_CAP ? 0 : seq % VADLOG_CAP;  /* oldest slot */
    char line[96];
    for (uint32_t k = 0; k < total; ++k) {
        const vadlog_entry_t *e = &s_vadlog[(start + k) % VADLOG_CAP];
        int n = snprintf(line, sizeof line, "%u,%u,%u,%u,%d,%d,%d,%d\n",
                         (unsigned)e->t_ms, (unsigned)e->phase, (unsigned)e->event,
                         (unsigned)e->barge_on, (int)e->rms, (int)e->floor,
                         (int)e->gate, (int)e->peak_play);
        if (n > 0 && httpd_resp_send_chunk(req, line, (size_t)n) != ESP_OK) {
            break;   /* client hung up */
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t display_diag_handler(httpd_req_t *req)
{
    jr_display_diag_t d;
    if (jr_display_get_diag(&d) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "diag failed");
        return ESP_OK;
    }
    static const char *init_names[] = { "stopped", "starting", "ready", "failed" };
    const char *init_name = d.init_state <= JR_DISPLAY_INIT_FAILED
                                ? init_names[d.init_state] : "?";
    char buf[512];
    int n = snprintf(buf, sizeof buf,
        "{\"init\":\"%s\",\"last_error\":\"%s\",\"task_running\":%s,"
        "\"blanked\":%s,\"requested_face\":%d,\"applied_face\":%d,"
        "\"requested_amplitude\":%u,\"applied_bucket\":%u,"
        "\"requests\":%u,\"state_changes\":%u,\"segment_sets\":%u,"
        "\"asset_load_failures\":%u,\"flush_submissions\":%u,"
        "\"flush_completions\":%u,\"flush_errors\":%u,\"actual_fps\":%u,"
        "\"current_asset_bytes\":%u,\"free_psram\":%u,\"stack_hwm\":%u}",
        init_name, esp_err_to_name(d.last_error),
        d.task_running ? "true" : "false",
        d.blanked ? "true" : "false",
        (int)d.requested_face, (int)d.applied_face,
        (unsigned)d.requested_amplitude, (unsigned)d.applied_bucket,
        (unsigned)d.requests, (unsigned)d.state_changes,
        (unsigned)d.segment_sets, (unsigned)d.asset_load_failures,
        (unsigned)d.flush_submissions, (unsigned)d.flush_completions,
        (unsigned)d.flush_errors, (unsigned)d.actual_fps,
        (unsigned)d.current_asset_bytes, (unsigned)d.free_psram_bytes,
        (unsigned)d.task_stack_hwm);
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encoding failed");
        return ESP_OK;
    }
    size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static const char *display_pattern_name(jr_display_test_pattern_t pattern)
{
    switch (pattern) {
    case JR_DISPLAY_TEST_OFF:        return "off";
    case JR_DISPLAY_TEST_COLOR_BARS: return "bars";
    case JR_DISPLAY_TEST_GRID:       return "grid";
    case JR_DISPLAY_TEST_WHITE:      return "white";
    case JR_DISPLAY_TEST_RED:        return "red";
    case JR_DISPLAY_TEST_GREEN:      return "green";
    case JR_DISPLAY_TEST_BLUE:       return "blue";
    case JR_DISPLAY_TEST_TOUCH_CHALLENGE: return "touch-challenge";
    default:                         return "unknown";
    }
}

static bool display_pattern_parse(const char *name,
                                  jr_display_test_pattern_t *out)
{
    if (strcmp(name, "off") == 0 || strcmp(name, "normal") == 0) {
        *out = JR_DISPLAY_TEST_OFF;
    } else if (strcmp(name, "bars") == 0 || strcmp(name, "color-bars") == 0) {
        *out = JR_DISPLAY_TEST_COLOR_BARS;
    } else if (strcmp(name, "grid") == 0) {
        *out = JR_DISPLAY_TEST_GRID;
    } else if (strcmp(name, "white") == 0) {
        *out = JR_DISPLAY_TEST_WHITE;
    } else if (strcmp(name, "red") == 0) {
        *out = JR_DISPLAY_TEST_RED;
    } else if (strcmp(name, "green") == 0) {
        *out = JR_DISPLAY_TEST_GREEN;
    } else if (strcmp(name, "blue") == 0) {
        *out = JR_DISPLAY_TEST_BLUE;
    } else {
        return false;
    }
    return true;
}

/* POST /api/display/canvas[?ttl=ms] — raw RGB565 little-endian, exactly
 * 466*466*2 bytes: the glass as a remote canvas (owner request 2026-08-27).
 * Body is streamed into a PSRAM staging buffer, handed to the display, and
 * freed; the display keeps its own copy with a TTL so an abandoned image can
 * never permanently cover the face. DELETE via ?clear=1. */
static esp_err_t display_canvas_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    char query[64];
    uint32_t ttl_ms = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "clear", val, sizeof val) == ESP_OK &&
            val[0] == '1') {
            jr_display_canvas_clear();
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":true,\"cleared\":true}");
            return ESP_OK;
        }
        if (httpd_query_key_value(query, "ttl", val, sizeof val) == ESP_OK) {
            ttl_ms = (uint32_t)strtoul(val, NULL, 10);
        }
    }
    const size_t want = 466U * 466U * 2U;
    if (req->content_len != want) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "body must be raw RGB565 466x466 (434312 bytes)");
        return ESP_OK;
    }
    uint8_t *buf = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return ESP_OK;
    }
    size_t got = 0;
    while (got < want) {
        int r = httpd_req_recv(req, (char *)buf + got, want - got);
        if (r <= 0) {
            heap_caps_free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
            return ESP_OK;
        }
        got += (size_t)r;
    }
    esp_err_t err = jr_display_canvas_show((const uint16_t *)buf, 466U, 466U,
                                           ttl_ms);
    heap_caps_free(buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "canvas: image pushed (ttl=%u ms)",
             (unsigned)(ttl_ms ? ttl_ms : 30000U));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/debug/audio-stats?reset=1 — zero the playback and receive
 * counters so a soak, or one spoken turn, is read from a clean slate. */
static esp_err_t audio_stats_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    int reset = 0;
    query_int(req, "reset", &reset);
    if (reset) {
        jr_audio_playback_stats_reset();
        jr_gemini_ws_rx_diag_reset();
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, reset ? "{\"ok\":true,\"reset\":true}"
                                  : "{\"ok\":true,\"reset\":false}");
    return ESP_OK;
}

/* GET  /api/debug/sleep            -> how the device last woke, how often it
 *                                     has slept, which wake sources were armed
 * POST /api/debug/sleep?now=1[&wake_s=N]
 *                                  -> sleep on the next mood tick with an
 *                                     N-second timer (default 60) as the
 *                                     guaranteed way back. Proves the wake
 *                                     lines from a desk: a wake before the
 *                                     timer means a line was at its wake
 *                                     level, a timer wake means both were
 *                                     quiet. */
static esp_err_t debug_sleep_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    if (req->method == HTTP_POST) {
        int now = 0, wake_s = 60, off = 0;
        query_int(req, "now", &now);
        query_int(req, "wake_s", &wake_s);
        query_int(req, "off", &off);
        if (off) {
            /* The same road PWR long takes: rails off, hold PWR to start. */
            atomic_store(&s_power_off_req, true);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":true,\"off\":true}");
            return ESP_OK;
        }
        if (now && image_in_probation()) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"image in probation; a sleep "
                                    "would roll it back\"}");
            return ESP_OK;
        }
        if (now) {
            atomic_store(&s_sleep_timer_s, (uint32_t)(wake_s < 5 ? 5 : wake_s));
            atomic_store(&s_sleep_force, true);
        }
    }
    char body[224];
    snprintf(body, sizeof body,
             "{\"wake\":\"%s\",\"sleeps\":%u,\"armed\":{\"lift\":%s,"
             "\"touch\":%s},\"lift_fail\":\"%s\",\"force\":%s,"
             "\"after_dream_ms\":%u}",
             wake_cause_name(s_boot_wake_cause), (unsigned)s_rtc_sleeps,
             (s_rtc_armed & SLEEP_ARMED_LIFT) ? "true" : "false",
             (s_rtc_armed & SLEEP_ARMED_TOUCH) ? "true" : "false",
             (s_rtc_armed & SLEEP_LIFT_ARM_FAILED) ? "arm"
                 : (s_rtc_armed & SLEEP_LIFT_LINE_HIGH) ? "high" : "none",
             atomic_load(&s_sleep_force) ? "true" : "false",
             (unsigned)JR_MOOD_SLEEP_MS);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* POST /api/debug/input?kind=tap|double|long|swipe
 * [&dir=left|right|up|down] [&x=&y=&edge=1] synthesizes an event through the
 * real queue. "double" enqueues two taps inside the double-tap window. */
static esp_err_t debug_input_handler(httpd_req_t *req)
{
    if (!control_intent_required(req) || !agent_require_auth(req)) {
        return ESP_OK;
    }
    char query[96], kind[12] = {0}, dirs[12] = {0}, val[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "kind", kind, sizeof kind) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "kind=tap|double|long|swipe required");
        return ESP_OK;
    }
    jr_input_event_t ev = { .kind = JR_INPUT_TAP, .x = 233, .y = 233,
                            .start_x = 233, .start_y = 233,
                            .end_x = 233, .end_y = 233,
                            .duration_ms = 80,
                            .direction = JR_INPUT_DIRECTION_NONE, .flags = 0 };
    if (httpd_query_key_value(query, "x", val, sizeof val) == ESP_OK) {
        ev.x = ev.start_x = ev.end_x = (uint16_t)strtoul(val, NULL, 10);
    }
    if (httpd_query_key_value(query, "y", val, sizeof val) == ESP_OK) {
        ev.y = ev.start_y = ev.end_y = (uint16_t)strtoul(val, NULL, 10);
    }
    int repeats = 1;
    if (strcmp(kind, "long") == 0) {
        ev.kind = JR_INPUT_LONG_PRESS;
        ev.duration_ms = 1300;
    } else if (strcmp(kind, "double") == 0) {
        repeats = 2;
    } else if (strcmp(kind, "swipe") == 0) {
        ev.kind = JR_INPUT_SWIPE;
        ev.duration_ms = 250;
        (void)httpd_query_key_value(query, "dir", dirs, sizeof dirs);
        if (strcmp(dirs, "left") == 0) {
            ev.direction = JR_INPUT_DIRECTION_LEFT;
            ev.start_x = 400; ev.end_x = 60; ev.delta_x = -340;
        } else if (strcmp(dirs, "right") == 0) {
            ev.direction = JR_INPUT_DIRECTION_RIGHT;
            ev.start_x = 60; ev.end_x = 400; ev.delta_x = 340;
        } else if (strcmp(dirs, "up") == 0) {
            ev.direction = JR_INPUT_DIRECTION_UP;
            ev.start_y = 400; ev.end_y = 60; ev.delta_y = -340;
        } else if (strcmp(dirs, "down") == 0) {
            ev.direction = JR_INPUT_DIRECTION_DOWN;
            ev.start_y = 90; ev.end_y = 400; ev.delta_y = 310;
            if (httpd_query_key_value(query, "edge", val, sizeof val)
                    == ESP_OK && val[0] == '1') {
                ev.start_y = 20; ev.delta_y = 380;
                ev.flags = JR_INPUT_FLAG_TOP_EDGE;
            }
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "dir=left|right|up|down required for swipe");
            return ESP_OK;
        }
    } else if (strcmp(kind, "tap") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown kind");
        return ESP_OK;
    }
    esp_err_t err = ESP_OK;
    for (int i = 0; i < repeats && err == ESP_OK; ++i) {
        err = jr_hal_input_inject(&ev);
        if (repeats > 1 && i == 0) {
            vTaskDelay(pdMS_TO_TICKS(120));   /* inside the 400 ms window */
        }
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "debug: injected input kind=%s dir=%s", kind, dirs);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* GET /api/logs?tail=N — last N bytes of the log ring, chronological, plain
 * text. Reads are chunked with the mux held only per-chunk, so a concurrent
 * writer can at worst garble the OLDEST lines of a snapshot mid-read —
 * acceptable for a diagnostic tail, and it never stalls logging. */
static esp_err_t logs_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    if (s_logring == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "log ring unavailable");
        return ESP_OK;
    }
    size_t tail = 16384;
    char query[48], val[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "tail", val, sizeof val) == ESP_OK) {
        tail = (size_t)strtoul(val, NULL, 10);
    }
    portENTER_CRITICAL(&s_logring_mux);
    size_t len = s_logring_len;
    size_t head = s_logring_head;
    portEXIT_CRITICAL(&s_logring_mux);
    if (tail > len) {
        tail = len;
    }
    /* oldest byte of the requested window */
    size_t start = (head + LOGRING_CAP - tail) % LOGRING_CAP;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char chunk[1024];
    size_t sent = 0;
    while (sent < tail) {
        size_t n = tail - sent < sizeof chunk ? tail - sent : sizeof chunk;
        portENTER_CRITICAL(&s_logring_mux);
        for (size_t i = 0; i < n; ++i) {
            chunk[i] = s_logring[(start + sent + i) % LOGRING_CAP];
        }
        portEXIT_CRITICAL(&s_logring_mux);
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
            return ESP_OK;
        }
        sent += n;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void ota_restore_control_state(bool was_privacy_paused,
                                      uint32_t previous_lease_until_ms)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    /* Privacy is fail-closed: never arm if voice was private before OTA or a
     * physical gesture made it private while the upload was running. */
    const bool keep_private =
        was_privacy_paused || atomic_load(&s_voice_privacy_paused);
    atomic_store(&s_voice_control_request,
                 keep_private ? VOICE_CONTROL_PAUSE : VOICE_CONTROL_RESUME);

    /* A physical owner tap clears the OTA lease and must win. Otherwise restore
     * a still-live pre-existing lease rather than inventing a new one. */
    if (atomic_load(&s_operator_lease_until_ms) != 0U) {
        atomic_store(&s_operator_lease_until_ms,
                     (int32_t)(previous_lease_until_ms - now) > 0
                         ? previous_lease_until_ms : 0U);
    }
}

bool ota_confirm_running_image_if_healthy(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running == NULL) {
        return false;
    }
    esp_err_t state_err = esp_ota_get_state_partition(running, &state);
    if (state_err != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return state_err == ESP_OK;
    }

    static uint32_t observed_flush_errors = UINT32_MAX;
    static uint32_t observed_flush_completions;
    static uint32_t flush_stable_since_ms;
    static uint32_t last_flush_progress_ms;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now >= 120000U) {
        ESP_LOGE(TAG, "ota: probation deadline failed; rolling back");
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
        return false;
    }
    const bool voice_alive = s_voice_task_running &&
        (uint32_t)(now - s_voice_task_heartbeat_ms) < 2000U;
    const bool subsystems_healthy =
        voice_alive && jr_net_is_connected() &&
        atomic_load(&s_tool_diag.worker_ready) &&
        atomic_load(&s_http_ready) && jr_wake_ready();
    if (!subsystems_healthy) {
        flush_stable_since_ms = now;
        return false;
    }

    jr_display_diag_t display = {0};
    if (jr_display_get_diag(&display) != ESP_OK ||
        display.init_state != JR_DISPLAY_INIT_READY ||
        !display.task_running) {
        return false;
    }
    /* Display stability must hold alongside every required subsystem. */
    if (display.flush_completions != observed_flush_completions) {
        observed_flush_completions = display.flush_completions;
        last_flush_progress_ms = now;
    }
    if (display.flush_errors != observed_flush_errors) {
        observed_flush_errors = display.flush_errors;
        flush_stable_since_ms = now;
        return false;
    }
    if (display.actual_fps < 12U ||
        last_flush_progress_ms == 0U ||
        (uint32_t)(now - last_flush_progress_ms) > 1000U) {
        flush_stable_since_ms = now;
        return false;
    }
    if ((uint32_t)(now - flush_stable_since_ms) < 10000U) {
        return false;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        uint8_t slot = strcmp(running->label, "ota_0") == 0 ? 0U :
                       strcmp(running->label, "ota_1") == 0 ? 1U : 0xFFU;
        jr_display_ota_set(JR_DISPLAY_OTA_VALID, 100U, slot, 0xFFU, true);
        persist_ota_attempt(-1);
        ESP_LOGI(TAG,
                 "ota: image valid after voice/display stable window");
        return true;
    }
    ESP_LOGE(TAG, "ota: could not mark running image valid: %s",
             esp_err_to_name(err));
    return false;
}

/* POST /api/ota/upload — stream a jarvisrobot_v5.bin into the IDLE app slot
 * over Wi-Fi, set it as boot, reboot. The last cable-flash killer: the live
 * app keeps running (UI may stutter during erase bursts — flash-cache
 * physics — but Wi-Fi, voice state, and the glass survive), and a failed
 * write leaves the RUNNING slot untouched. Control-gated; auto-claims the
 * operator lease so the glass announces itself. */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    const size_t len = req->content_len;
    atomic_store(&s_ota_active, false);
    atomic_store(&s_ota_received_bytes, 0U);
    atomic_store(&s_ota_total_bytes, (uint32_t)len);
    if (len < 256U * 1024U || len > 0x400000U) {
        atomic_store(&s_ota_last_error, ESP_ERR_INVALID_SIZE);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "implausible app image size");
        return ESP_OK;
    }
    atomic_store(&s_ota_preflight_blocked, false);
    const ota_preflight_t preflight = ota_preflight();
    /* No navigation: the update ring is shell-wide and draws on whatever is
     * up. This used to jump the glass to SETTINGS and open its sheet, which
     * was the only production visit that screen ever had. */
    jr_display_ota_set(JR_DISPLAY_OTA_PREFLIGHT, 0U,
                       preflight.active_slot, preflight.target_slot,
                       preflight.ok);
    if (!preflight.ok) {
        atomic_store(&s_ota_preflight_blocked, true);
        atomic_store(&s_ota_last_error, ESP_OK);
        jr_display_ota_set(JR_DISPLAY_OTA_BLOCKED, 0U,
                           preflight.active_slot, preflight.target_slot,
                           false);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, preflight.reason);
        return ESP_OK;
    }
    const esp_partition_t *next = preflight.target;
    const uint32_t upload_started_ms =
        (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t upload_deadline_ms = upload_started_ms + 240000U;
    const uint32_t previous_lease_until_ms =
        atomic_load(&s_operator_lease_until_ms);
    const bool was_privacy_paused =
        atomic_load(&s_voice_privacy_paused);
    atomic_store(&s_ota_last_error, ESP_OK);
    atomic_store(&s_ota_active, true);
    atomic_store(&s_operator_lease_until_ms, upload_started_ms + 180000U);
    atomic_store(&s_voice_control_request, VOICE_CONTROL_PAUSE);
    jr_display_caption_pin("UPDATING - DO NOT UNPLUG");
    ESP_LOGI(TAG, "ota: receiving %u bytes into %s", (unsigned)len,
             next->label);

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(next, len, &ota);
    if (err != ESP_OK) {
        atomic_store(&s_ota_active, false);
        atomic_store(&s_ota_last_error, err);
        jr_display_caption_unpin();
        jr_display_caption_set("UPDATE FAILED - STILL ON OLD");
        ota_restore_control_state(was_privacy_paused,
                                  previous_lease_until_ms);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        ESP_LOGE(TAG, "ota: begin failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    const uint8_t active_slot = preflight.active_slot;
    jr_display_ota_set(JR_DISPLAY_OTA_RECEIVING, 0U, active_slot,
                       preflight.target_slot, true);
    /* Settings detail already owns the glass from preflight. */
    /* Keep the staging buffer internal: esp_ota_write performs flash
     * operations with the cache disabled, so this avoids depending on
     * external-memory cache behavior in the update path. */
    const size_t chunk_size = 4096U;
    char *buf = heap_caps_malloc(chunk_size,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t got = 0;
    unsigned recv_timeouts = 0U;
    if (buf == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    while (got < len && err == ESP_OK) {
        uint32_t receive_now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((int32_t)(receive_now - upload_deadline_ms) >= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        int r = httpd_req_recv(req, buf,
                               len - got < chunk_size
                                   ? len - got : chunk_size);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++recv_timeouts < 6U) {
                continue;
            }
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        recv_timeouts = 0U;
        err = esp_ota_write(ota, buf, (size_t)r);
        got += (size_t)r;
        atomic_store(&s_ota_received_bytes, (uint32_t)got);
        atomic_store(&s_operator_lease_until_ms,
                     (uint32_t)(esp_timer_get_time() / 1000) + 180000U);
    }
    heap_caps_free(buf);
    if (err == ESP_OK && got == len) {
        err = esp_ota_end(ota);
    } else {
        (void)esp_ota_abort(ota);
        err = err == ESP_OK ? ESP_FAIL : err;
    }
    if (err == ESP_OK) {
        esp_app_desc_t app_desc = {0};
        err = esp_ota_get_partition_description(next, &app_desc);
        if (err == ESP_OK &&
            strcmp(app_desc.project_name, "jarvisrobot_v5") != 0) {
            ESP_LOGE(TAG, "ota: refusing project \"%s\"", app_desc.project_name);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(next);
    }
    if (err != ESP_OK) {
        atomic_store(&s_ota_active, false);
        atomic_store(&s_ota_last_error, err);
        jr_display_ota_set(
            JR_DISPLAY_OTA_FAILED,
            len > 0U ? (uint8_t)((got * 100U) / len) : 0U,
            active_slot, strcmp(next->label, "ota_0") == 0 ? 0U : 1U, true);
        jr_display_caption_unpin();
        jr_display_caption_set("UPDATE FAILED - STILL ON OLD");
        ota_restore_control_state(was_privacy_paused,
                                  previous_lease_until_ms);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        ESP_LOGE(TAG, "ota: failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    persist_ota_attempt(strcmp(next->label, "ota_0") == 0 ? 0 : 1);
    ESP_LOGI(TAG, "ota: %u bytes verified into %s — rebooting to swap",
             (unsigned)got, next->label);
    jr_display_caption_unpin();
    jr_display_caption_set("UPDATE OK - RESTARTING");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

/* POST /api/ota/assets — the art image, whole, into the emote_assets
 * partition. The app image travels over /api/ota/upload and the faces do not
 * ride with it, so a firmware that names a new clip lands on a partition that
 * lacks it (it shows the parent face, see jr_display face_fallback) until this
 * runs. Whole-partition erase, streamed write, reboot to remount. Refused in
 * probation: that reboot would roll the app back. */
static esp_err_t ota_assets_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    if (image_in_probation()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"app image in probation; try again in a minute\"}");
        return ESP_OK;
    }
    if (atomic_load(&s_ota_active)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"an update is in flight\"}");
        return ESP_OK;
    }
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "emote_assets");
    if (part == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no emote_assets partition");
        return ESP_OK;
    }
    const size_t len = req->content_len;
    if (len < 1024U * 1024U || len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "art image must be between 1 MiB and the partition size");
        return ESP_OK;
    }
    const uint32_t started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t deadline_ms = started_ms + 420000U;
    const uint32_t previous_lease_until_ms =
        atomic_load(&s_operator_lease_until_ms);
    const bool was_privacy_paused = atomic_load(&s_voice_privacy_paused);
    atomic_store(&s_ota_active, true);
    atomic_store(&s_ota_received_bytes, 0U);
    atomic_store(&s_ota_total_bytes, (uint32_t)len);
    atomic_store(&s_operator_lease_until_ms, started_ms + 180000U);
    atomic_store(&s_voice_control_request, VOICE_CONTROL_PAUSE);
    jr_display_caption_pin("UPDATING ART - DO NOT UNPLUG");
    ESP_LOGI(TAG, "art: receiving %u bytes into %s (%u bytes)", (unsigned)len,
             part->label, (unsigned)part->size);

    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    const size_t chunk_size = 4096U;
    char *buf = err == ESP_OK
        ? heap_caps_malloc(chunk_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        : NULL;
    if (err == ESP_OK && buf == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    unsigned recv_timeouts = 0U;
    while (got < len && err == ESP_OK) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((int32_t)(now_ms - deadline_ms) >= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        int r = httpd_req_recv(req, buf,
                               len - got < chunk_size ? len - got : chunk_size);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++recv_timeouts < 6U) {
                continue;
            }
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        recv_timeouts = 0U;
        err = esp_partition_write(part, got, buf, (size_t)r);
        got += (size_t)r;
        atomic_store(&s_ota_received_bytes, (uint32_t)got);
        atomic_store(&s_operator_lease_until_ms, now_ms + 180000U);
    }
    heap_caps_free(buf);
    if (err == ESP_OK && got != len) {
        err = ESP_FAIL;
    }
    atomic_store(&s_ota_active, false);
    atomic_store(&s_ota_last_error, err);
    if (err != ESP_OK) {
        jr_display_caption_unpin();
        jr_display_caption_set("ART UPDATE FAILED - SEND IT AGAIN");
        ota_restore_control_state(was_privacy_paused, previous_lease_until_ms);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        ESP_LOGE(TAG, "art: failed after %u bytes: %s", (unsigned)got,
                 esp_err_to_name(err));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "art: %u bytes written into %s — rebooting to remount",
             (unsigned)got, part->label);
    jr_display_caption_unpin();
    jr_display_caption_set("ART OK - RESTARTING");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

/* POST /api/operator/lease?ttl=seconds — claim; ?release=1 — hand back. */
static esp_err_t operator_lease_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req) || !control_intent_required(req)) {
        return ESP_OK;
    }
    char query[64];
    char val[16] = {0};
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "release", val, sizeof val) == ESP_OK &&
        val[0] == '1') {
        (void)operator_mode_release(now, "remote", false, false);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"leased\":false}");
        return ESP_OK;
    }
    uint32_t ttl_s = 300U;
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "ttl", val, sizeof val) == ESP_OK) {
        ttl_s = (uint32_t)strtoul(val, NULL, 10);
    }
    if (ttl_s < 10U) ttl_s = 10U;
    if (ttl_s > 900U) ttl_s = 900U;
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "operator mode unavailable");
        return ESP_OK;
    }
    atomic_store(&s_operator_mode_entered_ms, now);
    atomic_store(&s_operator_lease_until_ms, now + ttl_s * 1000U);
    atomic_store(&s_voice_control_request, VOICE_CONTROL_PAUSE);
    jr_display_caption_set("CODEX MODE - DOUBLE TAP TO EXIT");
    atomic_store(&s_operator_mode_active, true); /* publish complete state last */
    xSemaphoreGive(s_brain_lock);
    ESP_LOGI(TAG, "operator: Codex mode claimed for %u s", (unsigned)ttl_s);
    char body[64];
    int n = snprintf(body, sizeof body,
                     "{\"ok\":true,\"leased\":true,\"mode\":\"codex\",\"ttl_s\":%u}",
                     (unsigned)ttl_s);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t operator_status_handler(httpd_req_t *req)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t until = atomic_load(&s_operator_lease_until_ms);
    const bool active = operator_mode_active(now);
    const uint32_t ttl_ms = active && (int32_t)(until - now) > 0
        ? until - now : 0U;
    char body[96];
    int n = snprintf(body, sizeof body,
                     "{\"active\":%s,\"mode\":\"%s\",\"ttl_ms\":%u}",
                     active ? "true" : "false",
                     active ? "codex" : "normal",
                     (unsigned)ttl_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t display_test_handler(httpd_req_t *req)
{
    if (!control_intent_required(req)) {
        return ESP_OK;
    }
    char query[96];
    char name[24] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "pattern", name, sizeof name) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                           "missing ?pattern=off|bars|grid|white|red|green|blue");
        return ESP_OK;
    }
    jr_display_test_pattern_t pattern;
    if (!display_pattern_parse(name, &pattern)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown pattern");
        return ESP_OK;
    }
    esp_err_t err = jr_display_set_test_pattern(pattern);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_OK;
    }
    char body[96];
    int n = snprintf(body, sizeof body,
                     "{\"ok\":true,\"pattern\":\"%s\"}",
                     display_pattern_name(pattern));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t display_snapshot_info_handler(httpd_req_t *req)
{
    jr_display_snapshot_info_t info;
    esp_err_t err = jr_display_snapshot_get_info(&info);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        char body[128];
        int n = snprintf(body, sizeof body,
                         "{\"available\":false,\"error\":\"%s\"}",
                         esp_err_to_name(err));
        httpd_resp_send(req, body, n);
        return ESP_OK;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool fresh = info.valid &&
        (uint32_t)(now_ms - (uint32_t)info.last_flush_ms) <= 1000U;
    char body[384];
    int n = snprintf(body, sizeof body,
        "{\"available\":true,\"capture_source\":\"panel_submission_mirror\","
        "\"panel_readback\":false,\"width\":%u,\"height\":%u,"
        "\"bytes\":%u,\"frame_id\":%llu,\"last_flush_ms\":%llu,"
        "\"valid\":%s,\"mirror_fresh\":%s,\"test_pattern\":\"%s\"}",
        (unsigned)info.width, (unsigned)info.height, (unsigned)info.bytes,
        (unsigned long long)info.frame_id,
        (unsigned long long)info.last_flush_ms,
        info.valid ? "true" : "false", fresh ? "true" : "false",
        display_pattern_name(info.test_pattern));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static void panel_rgb565_to_rgb888(uint16_t panel_px, uint8_t out[3])
{
    uint16_t px = __builtin_bswap16(panel_px);
    uint8_t r = (uint8_t)((px >> 11) & 0x1fU);
    uint8_t g = (uint8_t)((px >> 5) & 0x3fU);
    uint8_t b = (uint8_t)(px & 0x1fU);
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
}

static esp_err_t display_snapshot_ppm_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    enum { PPM_BATCH_ROWS = 8 };
    jr_display_snapshot_info_t info;
    esp_err_t err = jr_display_snapshot_get_info(&info);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display mirror unavailable");
        return ESP_OK;
    }
    if (!info.valid) {
        vTaskDelay(pdMS_TO_TICKS(120));
        err = jr_display_snapshot_get_info(&info);
    }
    uint16_t *frame = err == ESP_OK && info.bytes > 0
        ? heap_caps_malloc(info.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : NULL;
    if (frame == NULL ||
        jr_display_snapshot_copy_rgb565(frame, info.bytes, &info) != ESP_OK) {
        heap_caps_free(frame);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display mirror not ready");
        return ESP_OK;
    }

    size_t pixels = (size_t)info.width * (size_t)info.height;
    size_t lit = 0;
    uint32_t checksum = 2166136261U;
    for (size_t i = 0; i < pixels; ++i) {
        uint16_t native = __builtin_bswap16(frame[i]);
        if (native != 0U) {
            lit++;
        }
        checksum = (checksum ^ (uint8_t)frame[i]) * 16777619U;
        checksum = (checksum ^ (uint8_t)(frame[i] >> 8)) * 16777619U;
    }

    char header[64];
    int header_len = snprintf(header, sizeof header, "P6\n%u %u\n255\n",
                              (unsigned)info.width, (unsigned)info.height);
    size_t rgb_row_bytes = (size_t)info.width * 3U;
    size_t rgb_batch_bytes = rgb_row_bytes * PPM_BATCH_ROWS;
    uint8_t *rgb_rows = heap_caps_malloc(
        rgb_batch_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rgb_rows == NULL) {
        rgb_rows = heap_caps_malloc(
            rgb_batch_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (rgb_rows == NULL) {
        heap_caps_free(frame);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display conversion buffer unavailable");
        return ESP_OK;
    }
    char frame_value[32];
    char lit_value[32];
    char checksum_value[32];
    char width_value[16];
    char height_value[16];
    snprintf(frame_value, sizeof frame_value, "%llu",
             (unsigned long long)info.frame_id);
    snprintf(lit_value, sizeof lit_value, "%u", (unsigned)lit);
    snprintf(checksum_value, sizeof checksum_value, "%08x",
             (unsigned)checksum);
    snprintf(width_value, sizeof width_value, "%u", (unsigned)info.width);
    snprintf(height_value, sizeof height_value, "%u", (unsigned)info.height);
    httpd_resp_set_type(req, "image/x-portable-pixmap");
    err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Source", "panel_submission_mirror");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Owner", "jr_display");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Panel-Readback", "false");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Fresh", "true");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Frame", frame_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Width", width_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Height", height_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Lit-Pixels", lit_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Frame-Checksum", checksum_value);
    if (err != ESP_OK) {
        heap_caps_free(rgb_rows);
        heap_caps_free(frame);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display metadata unavailable");
        return ESP_OK;
    }

    err = httpd_resp_send_chunk(req, header, header_len);
    for (uint16_t y = 0; y < info.height && err == ESP_OK;
         y = (uint16_t)(y + PPM_BATCH_ROWS)) {
        uint16_t rows = (uint16_t)(info.height - y);
        if (rows > PPM_BATCH_ROWS) {
            rows = PPM_BATCH_ROWS;
        }
        for (uint16_t batch_y = 0; batch_y < rows; ++batch_y) {
            const uint16_t *src = frame +
                (size_t)(y + batch_y) * info.width;
            uint8_t *dst = rgb_rows + (size_t)batch_y * rgb_row_bytes;
            for (uint16_t x = 0; x < info.width; ++x) {
                panel_rgb565_to_rgb888(src[x], dst + (size_t)x * 3U);
            }
        }
        err = httpd_resp_send_chunk(req, (const char *)rgb_rows,
                                    (ssize_t)((size_t)rows * rgb_row_bytes));
    }
    heap_caps_free(rgb_rows);
    heap_caps_free(frame);
    if (err != ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Native RGB565 mirror for the cockpit and CLI. The snapshot buffer already
 * contains the exact bytes submitted to the panel; sending it once avoids the
 * 50% PPM expansion and conversion/network churn on-device. On this
 * little-endian S3 with panel byte swap enabled, the byte stream is native
 * RGB565 in big-endian order (high byte, low byte). */
static esp_err_t display_snapshot_rgb565_handler(httpd_req_t *req)
{
    if (!agent_require_auth(req)) {
        return ESP_OK;
    }
    jr_display_snapshot_info_t info;
    esp_err_t err = jr_display_snapshot_get_info(&info);
    uint16_t *frame = err == ESP_OK && info.bytes > 0U
        ? heap_caps_malloc(info.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : NULL;
    if (frame == NULL ||
        jr_display_snapshot_copy_rgb565(frame, info.bytes, &info) != ESP_OK) {
        heap_caps_free(frame);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display mirror not ready");
        return ESP_OK;
    }

    char frame_value[32];
    char width_value[16];
    char height_value[16];
    snprintf(frame_value, sizeof frame_value, "%llu",
             (unsigned long long)info.frame_id);
    snprintf(width_value, sizeof width_value, "%u", (unsigned)info.width);
    snprintf(height_value, sizeof height_value, "%u", (unsigned)info.height);
    httpd_resp_set_type(req, "application/octet-stream");
    err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Source", "panel_submission_mirror");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Owner", "jr_display");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Panel-Readback", "false");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Fresh", "true");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-RGB565-Order", "big-endian-native");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Frame", frame_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Width", width_value);
    if (err == ESP_OK) err = httpd_resp_set_hdr(req,
        "X-Jarvis-Display-Height", height_value);
    if (err != ESP_OK) {
        heap_caps_free(frame);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "display metadata unavailable");
        return ESP_OK;
    }
    err = httpd_resp_send(req, (const char *)frame, (ssize_t)info.bytes);
    heap_caps_free(frame);
    return err;
}

void start_diag_http(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    /* Live watermark: 3,880 B free at 7,168 after doctor/snapshots/tools.
     * 6,400 retains ~3.1 KB measured margin and returns another 768 B. */
    cfg.stack_size = 6400;
    cfg.max_uri_handlers = 48;
    cfg.max_resp_headers = 16;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "diag httpd failed to start");
        return;
    }
    httpd_uri_t routes[] = {
        { .uri = "/",                .method = HTTP_GET, .handler = dashboard_handler },
        { .uri = "/api/cockpit",     .method = HTTP_GET, .handler = cockpit_handler },
        { .uri = "/api/gemini/live", .method = HTTP_GET, .handler = diag_get_handler },
        { .uri = "/api/debug/say",   .method = HTTP_POST, .handler = say_get_handler  },
        { .uri = "/api/debug/gain",  .method = HTTP_POST, .handler = gain_get_handler },
        { .uri = "/api/voice/control", .method = HTTP_POST,
          .handler = voice_control_handler },
        { .uri = "/api/audio/self-test", .method = HTTP_POST,
          .handler = audio_self_test_handler },
        { .uri = "/api/audio/taps", .method = HTTP_GET,
          .handler = audio_taps_handler },
        { .uri = "/api/audio/tap.wav", .method = HTTP_GET,
          .handler = audio_tap_wav_handler },
        { .uri = "/api/touch", .method = HTTP_GET,
          .handler = touch_status_handler },
        { .uri = "/api/sensors", .method = HTTP_GET,
          .handler = sensors_handler },
        { .uri = "/api/diag/tasks", .method = HTTP_GET,
          .handler = tasks_diag_handler },
        { .uri = "/api/display/hud", .method = HTTP_POST,
          .handler = hud_toggle_handler },
        { .uri = "/api/display/choices", .method = HTTP_POST,
          .handler = choices_debug_handler },
        { .uri = "/api/display/choices/hit", .method = HTTP_GET,
          .handler = choices_hit_handler },
        { .uri = "/api/input/tap", .method = HTTP_POST,
          .handler = tap_sim_handler },
        { .uri = "/api/demo", .method = HTTP_POST,
          .handler = demo_handler },
        { .uri = "/api/diag/panel-touch", .method = HTTP_POST,
          .handler = panel_touch_control_handler },
        { .uri = "/api/ui/shade", .method = HTTP_POST,
          .handler = ui_shade_control_handler },
        { .uri = "/api/agent/link", .method = HTTP_GET,
          .handler = agent_link_get_handler },
        { .uri = "/api/agent/link", .method = HTTP_POST,
          .handler = agent_link_post_handler },
        { .uri = "/api/brain/outbox", .method = HTTP_GET,
          .handler = brain_outbox_handler },
        { .uri = "/api/brain/inbox", .method = HTTP_POST,
          .handler = brain_inbox_handler },
        { .uri = "/api/tools/config", .method = HTTP_GET,
          .handler = device_tool_config_get_handler },
        { .uri = "/api/tools/config", .method = HTTP_POST,
          .handler = device_tool_config_post_handler },
        { .uri = "/api/device/levels", .method = HTTP_GET,
          .handler = device_levels_get_handler },
        { .uri = "/api/device/levels", .method = HTTP_POST,
          .handler = device_levels_post_handler },
        { .uri = "/api/pairing/claim", .method = HTTP_POST,
          .handler = pairing_claim_handler },
        { .uri = "/api/display",     .method = HTTP_GET, .handler = display_diag_handler },
        { .uri = "/api/diag/vadlog", .method = HTTP_GET, .handler = vadlog_csv_handler },
        { .uri = "/api/device/health", .method = HTTP_GET,
          .handler = device_health_handler },
        { .uri = "/api/display/snapshot.json", .method = HTTP_GET,
          .handler = display_snapshot_info_handler },
        { .uri = "/api/display/snapshot.ppm", .method = HTTP_GET,
          .handler = display_snapshot_ppm_handler },
        { .uri = "/api/display/snapshot.rgb565", .method = HTTP_GET,
          .handler = display_snapshot_rgb565_handler },
        { .uri = "/api/display/test", .method = HTTP_POST,
          .handler = display_test_handler },
        { .uri = "/api/display/canvas", .method = HTTP_POST,
          .handler = display_canvas_handler },
        { .uri = "/api/operator/lease", .method = HTTP_GET,
          .handler = operator_status_handler },
        { .uri = "/api/operator/lease", .method = HTTP_POST,
          .handler = operator_lease_handler },
        { .uri = "/api/logs", .method = HTTP_GET,
          .handler = logs_handler },
        { .uri = "/api/debug/input", .method = HTTP_POST,
          .handler = debug_input_handler },
        { .uri = "/api/debug/audio-stats", .method = HTTP_POST,
          .handler = audio_stats_handler },
        { .uri = "/api/debug/sleep", .method = HTTP_GET,
          .handler = debug_sleep_handler },
        { .uri = "/api/debug/sleep", .method = HTTP_POST,
          .handler = debug_sleep_handler },
        { .uri = "/api/ota/upload", .method = HTTP_POST,
          .handler = ota_upload_handler },
        { .uri = "/api/ota/assets", .method = HTTP_POST,
          .handler = ota_assets_handler },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "diag route registration failed uri=%s err=%s",
                     routes[i].uri, esp_err_to_name(err));
        }
    }
    atomic_store(&s_http_ready, true);
    ESP_LOGI(TAG, "diag http up: voice control, audio taps, display mirror/test");
#if JR_DEV_OPEN_DIAGNOSTICS
    ESP_LOGW(TAG, "************************************************************");
    ESP_LOGW(TAG, "DEV MODE: pairing token NOT required on diagnostic endpoints");
    ESP_LOGW(TAG, "Anything on this LAN can read logs, hear mic taps, drive the");
    ESP_LOGW(TAG, "display and inject input. Set JR_DEV_OPEN_DIAGNOSTICS 0 to");
    ESP_LOGW(TAG, "restore auth before shipping.");
    ESP_LOGW(TAG, "************************************************************");
#endif
}
