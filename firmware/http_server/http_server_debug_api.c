/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * POST /api/debug/crash
 *   Requires confirm=1 as a query parameter, or a body of either
 *   "confirm=1" (form style) or {"confirm": true} / {"confirm": 1} (JSON).
 *   Without confirmation → 400, no side effects.
 *
 * Deliberate crash hook (STABILITY_PLAN.md P0 crash-evidence chain):
 * logs a warning, answers the request, waits 100 ms so the warning can
 * drain to the SD/serial loggers, then calls abort() on the httpd task.
 * Used to prove end-to-end that a real panic produces a logged reset
 * reason on the next boot and a decodable coredump (once
 * CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH lands, P0.2).
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Forward decl instead of #include "cap_gemini_live.h": the http_server
 * component's compile path only carries cap_gemini_live's include dir when the
 * gemini HTTP API needs it, and that API reaches the capability through an
 * app_claw service struct rather than this header — so http_server has no
 * direct include dependency. The symbol is provided by the cap_gemini_live
 * component already in the link graph (via app_claw). Keep this prototype in
 * sync with cap_gemini_live.h. */
void cap_gemini_live_set_in_gains(int mic_db, int ref_db, int out_vol);
void cap_gemini_live_set_barge_rms(unsigned short rms);
esp_err_t cap_gemini_live_start(void);
esp_err_t cap_gemini_live_send_text(const char *text);
bool      cap_gemini_live_is_active(void);
void      cap_gemini_live_set_activity_interrupts(int enable);
void      cap_gemini_live_set_speak_gain(int db);
void      cap_gemini_live_set_vad(int speech_rms, int silence_rms);

static const char *TAG = "http_debug";

#define DEBUG_CRASH_BODY_MAX 256

static bool debug_crash_body_confirms(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= DEBUG_CRASH_BODY_MAX) {
        return false;
    }

    char body[DEBUG_CRASH_BODY_MAX];
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) {
        return false;
    }
    body[got] = '\0';

    if (strstr(body, "confirm=1") != NULL) {
        return true;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return false;
    }
    cJSON *confirm = cJSON_GetObjectItem(root, "confirm");
    bool ok = cJSON_IsTrue(confirm) ||
              (cJSON_IsNumber(confirm) && confirm->valueint == 1) ||
              (cJSON_IsString(confirm) && confirm->valuestring &&
               strcmp(confirm->valuestring, "1") == 0);
    cJSON_Delete(root);
    return ok;
}

static esp_err_t debug_crash_handler(httpd_req_t *req)
{
    char confirm[8] = {0};
    bool confirmed =
        (http_server_query_get(req, "confirm", confirm, sizeof(confirm)) == ESP_OK &&
         strcmp(confirm, "1") == 0) ||
        debug_crash_body_confirms(req);

    if (!confirmed) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "confirm=1 required (query param or body)");
    }

    ESP_LOGW(TAG,
             "deliberate crash requested via /api/debug/crash — abort() in 100 ms "
             "(crash-evidence chain test)");

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", true);
        http_server_json_add_string(root, "message",
                                    "aborting in 100 ms — expect panic + reboot");
        http_server_send_json_response(req, root);
    }

    /* Let the warning reach the SD-log writer / serial console. */
    vTaskDelay(pdMS_TO_TICKS(100));
    abort();

    return ESP_OK; /* unreachable */
}

/* GET /api/debug/gain?mic=<dB>&ref=<dB>&vol=<0-100>
 * Runtime AEC gain-staging sweep (2026-06-12 hardware calibration). Any omitted
 * param is left unchanged. Lets the echo-vs-reference levels be tuned live —
 * read the result back from /api/gemini/live (aec_atten_db, mic_pga_db, ...)
 * and the lane_rms log lines — without a 10-minute reflash per step. */
static esp_err_t debug_gain_handler(httpd_req_t *req)
{
    char buf[16];
    int mic = -1, ref = -1, vol = -1;
    if (http_server_query_get(req, "mic", buf, sizeof(buf)) == ESP_OK) mic = atoi(buf);
    if (http_server_query_get(req, "ref", buf, sizeof(buf)) == ESP_OK) ref = atoi(buf);
    if (http_server_query_get(req, "vol", buf, sizeof(buf)) == ESP_OK) vol = atoi(buf);

    cap_gemini_live_set_in_gains(mic, ref, vol);

    int barge = -1;
    if (http_server_query_get(req, "barge", buf, sizeof(buf)) == ESP_OK) {
        barge = atoi(buf);
        if (barge >= 0 && barge <= 65535) {
            cap_gemini_live_set_barge_rms((unsigned short)barge);
        }
    }

    int interrupt = -1;
    if (http_server_query_get(req, "interrupt", buf, sizeof(buf)) == ESP_OK) {
        interrupt = atoi(buf);
        /* applies on the next session — restart voice to take effect */
        cap_gemini_live_set_activity_interrupts(interrupt);
    }

    int speak = -1;
    if (http_server_query_get(req, "speak", buf, sizeof(buf)) == ESP_OK) {
        speak = atoi(buf);
        cap_gemini_live_set_speak_gain(speak);   /* during-SPEAKING mic gain, live */
    }

    int vadspeech = -1, vadsilence = -1;
    if (http_server_query_get(req, "vadspeech", buf, sizeof(buf)) == ESP_OK) vadspeech = atoi(buf);
    if (http_server_query_get(req, "vadsilence", buf, sizeof(buf)) == ESP_OK) vadsilence = atoi(buf);
    if (vadspeech >= 0 || vadsilence >= 0) {
        cap_gemini_live_set_vad(vadspeech, vadsilence);   /* hands-free turn-commit thresholds, live */
    }

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "mic", mic);
        cJSON_AddNumberToObject(root, "ref", ref);
        cJSON_AddNumberToObject(root, "vol", vol);
        cJSON_AddNumberToObject(root, "barge", barge);
        cJSON_AddNumberToObject(root, "interrupt", interrupt);
        cJSON_AddNumberToObject(root, "speak", speak);
        http_server_send_json_response(req, root);
    }
    return ESP_OK;
}

/* GET /api/debug/say?text=...
 * Drive a full Gemini turn from a text prompt so the AUDIO REPLY path can be
 * reproduced and instrumented without a person speaking: starts the session if
 * idle, waits up to ~8 s for it to come up, then sends the text (the model
 * replies with audio). Poll /api/gemini/live to watch the reply unfold
 * (audio_parts, first_audio_ms, ws_resume_count, drops, pcm_ring_drop_bytes).
 * Diagnostic + handy remote "make her say something" trigger. */
static esp_err_t debug_say_handler(httpd_req_t *req)
{
    char text[200] = {0};
    if (http_server_query_get(req, "text", text, sizeof(text)) != ESP_OK || text[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "missing ?text=\n");
    }

    bool started = false;
    if (!cap_gemini_live_is_active()) {
        cap_gemini_live_start();
        started = true;
        for (int i = 0; i < 80 && !cap_gemini_live_is_active(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    esp_err_t err = cap_gemini_live_send_text(text);

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
        cJSON_AddStringToObject(root, "result", esp_err_to_name(err));
        cJSON_AddBoolToObject(root, "started_session", started);
        cJSON_AddBoolToObject(root, "active", cap_gemini_live_is_active());
        cJSON_AddStringToObject(root, "sent", text);
        http_server_send_json_response(req, root);
    }
    return ESP_OK;
}

esp_err_t http_server_register_debug_routes(httpd_handle_t server)
{
    const httpd_uri_t crash = {
        .uri     = "/api/debug/crash",
        .method  = HTTP_POST,
        .handler = debug_crash_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &crash);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t gain = {
        .uri     = "/api/debug/gain",
        .method  = HTTP_GET,
        .handler = debug_gain_handler,
    };
    err = httpd_register_uri_handler(server, &gain);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t say = {
        .uri     = "/api/debug/say",
        .method  = HTTP_GET,
        .handler = debug_say_handler,
    };
    return httpd_register_uri_handler(server, &say);
}
