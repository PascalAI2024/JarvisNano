/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * main.c — JarvisRobot v5 L6 App Orchestration / Composition Root.
 *
 * The ONLY site in the firmware that constructs concrete adapters and injects
 * them into the pure core. It builds the object graph:
 *
 *   NVS  ->  jr_net (Wi-Fi + config)  ->  jr_audio (codec/AEC/ring)
 *        ->  jr_gemini_ws (device WS byte transport)
 *        ->  jr_gemini_client_t (the host-tested framer/parser)  [L2]
 *        ->  jr_orch_t (the pure single-writer pump)             [L3]
 *
 * then spawns ONE pinned FreeRTOS task that loops jr_orch_step() at frame
 * cadence, streams mic frames up while capturing, and reflects the phase on the
 * idle face. A tiny diag HTTP server exposes the StateSnapshot + /api/debug/say
 * + /api/debug/gain. The core never calls a HAL/net/codec function — it only
 * ever receives these structs.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "jr_ports/ports.h"
#include "jr_core/session.h"
#include "jr_core/orchestrator.h"
#include "jr_core/snapshot.h"
#include "jr_hal/hal.h"
#include "jr_net/jr_net.h"
#include "jr_audio/jr_audio.h"
#include "jr_transport/gemini_live.h"
#include "jr_transport/gemini_device_ws.h"

static const char *TAG = "jarvis_v5";

/* Gemini Live WSS endpoint (v4-proven). The API key is appended at runtime from
 * NVS — NEVER hardcode it in-repo. */
#define GEMINI_WS_BASE \
    "wss://generativelanguage.googleapis.com/ws/" \
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"

#define VOICE_FRAME_SAMPLES  512   /* 32 ms @ 16 kHz — one AEC chunk / uplink frame */
#define INBOX_CAP            64

/* ---- mapped inbound event queue (single-threaded: only the app task) ---- */
typedef struct {
    jr_event_t inq[INBOX_CAP];
    size_t     head, count;

    /* ws-level synthetic transition tracking */
    jr_ws_state_t last_ws;
    bool          expect_up;

    /* resume handle bookkeeping (goAway/setupComplete) */
    uint32_t      last_resumable_token;

    bool          capturing;
} voice_io_t;

/* ---- the composition graph ---- */
typedef struct {
    jr_clock_t                  clock;
    jr_display_t                display;
    jr_input_t                  input;
    jr_audio_source_t           mic;
    jr_audio_sink_t             spk;

    jr_ws_transport_t           ws;
    jr_gemini_config_t          cfg;
    jr_gemini_client_t          client;
    jr_realtime_voice_client_t  rvc;

    jr_orch_t                   orch;
    voice_io_t                  io;

    char                        url[512];
    jr_face_t                   last_face;
} jr_app_t;

static jr_app_t s_app;

/* diag: say-mailbox drained by the app task */
static QueueHandle_t s_say_q;   /* of char[200] */

/* ======================================================================== *
 *  inbound event mapping (mirrors host/test_soak.c soak_rich_cb)           *
 * ======================================================================== */
static void inq_push(voice_io_t *io, jr_event_t e)
{
    if (io->count >= INBOX_CAP) {
        return;   /* bounded; drop under a pathological burst */
    }
    io->inq[(io->head + io->count) % INBOX_CAP] = e;
    io->count++;
}

static jr_error_kind_t map_gem_err(jr_gemini_error_kind_t k)
{
    switch (k) {
    case JR_GEMINI_ERRK_QUOTA:     return JR_ERRK_QUOTA;
    case JR_GEMINI_ERRK_AUTH:      return JR_ERRK_AUTH;
    case JR_GEMINI_ERRK_PROTOCOL:  return JR_ERRK_PROTOCOL;
    case JR_GEMINI_ERRK_TRANSIENT: return JR_ERRK_TRANSIENT;
    default:                       return JR_ERRK_UNKNOWN;
    }
}

/* Map ONE rich transport event to the L3 vocabulary and enqueue it. For audio
 * chunks we feed the playback ring DIRECTLY here (same thread) because the
 * parser frees its decoded pcm the moment jr_gemini_pump_rx returns — so the
 * queued event carries no pcm pointer. */
static void rich_cb(void *u, const jr_gemini_event_t *ge)
{
    jr_app_t *a = (jr_app_t *)u;
    voice_io_t *io = &a->io;
    jr_event_t e = jr_event(JR_EV_HEARTBEAT);
    switch (ge->kind) {
    case JR_GEV_SETUP_COMPLETE:
        e = jr_event(JR_EV_SETUP_COMPLETE);
        e.resumption_token = io->last_resumable_token;
        break;
    case JR_GEV_AUDIO_CHUNK:
        if (ge->pcm != NULL && ge->pcm_len > 0) {
            jr_audio_sink_write(&a->spk, ge->pcm, ge->pcm_len);   /* stage now */
        }
        e = jr_event(JR_EV_SERVER_AUDIO_CHUNK);
        e.pcm = NULL;                      /* already fed; core just transitions */
        e.pcm_len = ge->pcm_len;
        e.sample_rate = ge->sample_rate;
        break;
    case JR_GEV_INTERRUPTED:
        e = jr_event(JR_EV_SERVER_INTERRUPTED);
        break;
    case JR_GEV_TURN_COMPLETE:
    case JR_GEV_GENERATION_COMPLETE:
        e = jr_event(JR_EV_SERVER_TURN_COMPLETE);
        break;
    case JR_GEV_TOOL_CALL:
        e = jr_event(JR_EV_SERVER_TOOL_CALL);
        e.call_id = ge->call_id;
        e.tool_name = "tool";
        e.tool_args = "{}";
        break;
    case JR_GEV_TOOL_CANCEL:
        e = jr_event(JR_EV_SERVER_TOOL_CALL);
        e.is_cancellation = true;
        e.call_id = ge->call_id;
        break;
    case JR_GEV_GO_AWAY:
        e = jr_event(JR_EV_SERVER_GO_AWAY);
        e.resumption_token = io->last_resumable_token;
        break;
    case JR_GEV_RESUMPTION_UPDATE:
        if (ge->resumable && ge->resumption_token) {
            io->last_resumable_token = ge->resumption_token;
        }
        e = jr_event(JR_EV_HEARTBEAT);
        e.resumption_token = ge->resumption_token;
        break;
    case JR_GEV_ERROR:
        e = jr_event(JR_EV_SERVER_ERROR);
        e.error_kind = map_gem_err(ge->error_kind);
        e.code = ge->code;
        break;
    case JR_GEV_TEXT:
    case JR_GEV_UNKNOWN:
    default:
        e = jr_event(JR_EV_HEARTBEAT);
        break;
    }
    inq_push(io, e);
}

/* ======================================================================== *
 *  the injected jr_orch_io_t                                               *
 * ======================================================================== */

/* poll_inbound: synthesize ws connect/close transitions, pump ONE ws frame
 * through the real parser (rich_cb enqueues mapped events), then hand back the
 * first queued event. Returns false only when nothing is available. */
static bool voice_poll(void *ctx, jr_event_t *out)
{
    jr_app_t *a = (jr_app_t *)ctx;
    voice_io_t *io = &a->io;

    if (io->count == 0) {
        /* (a) ws-level state transitions -> synthetic Connected / TransportClosed */
        if (io->expect_up) {
            jr_ws_state_t st = a->ws.state(a->ws.ctx);
            if (st == JR_WS_OPEN && io->last_ws != JR_WS_OPEN) {
                io->last_ws = JR_WS_OPEN;
                inq_push(io, jr_event(JR_EV_CONNECTED));
            } else if (st == JR_WS_CLOSED || st == JR_WS_ERROR) {
                io->last_ws = st;
                io->expect_up = false;
                inq_push(io, jr_event(JR_EV_TRANSPORT_CLOSED));
            }
        }
        /* (b) drain one inbound server frame into mapped events */
        if (io->count == 0) {
            jr_gemini_pump_rx(&a->client);
        }
    }
    if (io->count == 0) {
        return false;
    }
    *out = io->inq[io->head];
    io->head = (io->head + 1) % INBOX_CAP;
    io->count--;
    return true;
}

/* exec: run one externally-visible command against the real ports. */
static void voice_exec(void *ctx, const jr_command_t *cmd)
{
    jr_app_t *a = (jr_app_t *)ctx;
    voice_io_t *io = &a->io;
    switch (cmd->kind) {
    case JR_CMD_CONNECT:
        io->last_ws = JR_WS_CONNECTING;
        io->expect_up = true;
        a->ws.connect(a->ws.ctx, a->url);
        break;
    case JR_CMD_SEND_SETUP: {
        char *setup = jr_gemini_build_setup(&a->cfg);
        if (setup) {
            (void)jr_gemini_send_frame(&a->client, setup, strlen(setup));
            free(setup);
        }
        break;
    }
    case JR_CMD_SEND_ACTIVITY_START:
        a->rvc.send_control(a->rvc.ctx, JR_RVC_CTRL_ACTIVITY_START);
        break;
    case JR_CMD_SEND_ACTIVITY_END:
        a->rvc.send_control(a->rvc.ctx, JR_RVC_CTRL_ACTIVITY_END);
        break;
    case JR_CMD_SEND_AUDIO_STREAM_END:
        a->rvc.send_control(a->rvc.ctx, JR_RVC_CTRL_AUDIO_STREAM_END);
        break;
    case JR_CMD_SEND_TEXT:
        if (cmd->text) {
            a->rvc.send_text(a->rvc.ctx, cmd->text);
        }
        break;
    case JR_CMD_SEND_AUDIO:
        if (cmd->pcm && cmd->pcm_len) {
            a->rvc.send_audio(a->rvc.ctx, cmd->pcm, cmd->pcm_len);
        }
        break;
    case JR_CMD_CLOSE_TRANSPORT:
        io->expect_up = false;
        io->last_ws = JR_WS_CLOSED;
        a->rvc.close(a->rvc.ctx);
        io->head = io->count = 0;   /* drop mapped-but-undrained events */
        break;
    case JR_CMD_START_CAPTURE:
        io->capturing = true;
        break;
    case JR_CMD_PAUSE_CAPTURE:
        io->capturing = false;
        break;
    case JR_CMD_MUTE_DAC_NOW:
        jr_audio_sink_mute_now(&a->spk);
        break;
    case JR_CMD_UNMUTE_DAC:
        jr_audio_dac_unmute();
        break;
    case JR_CMD_FLUSH_PLAYBACK_RING:
        jr_audio_flush_playback();
        break;
    case JR_CMD_FEED_PLAYBACK:
        if (cmd->pcm && cmd->pcm_len) {   /* usually NULL: audio staged in rich_cb */
            jr_audio_sink_write(&a->spk, cmd->pcm, cmd->pcm_len);
        }
        break;
    default:
        /* DispatchToolCall / CancelToolCall / EmitDiag / PublishSnapshot / timers:
         * timers are handled inside the orchestrator; the rest are no-ops here. */
        break;
    }
}

/* ======================================================================== *
 *  face presentation (phase -> coarse face)                                *
 * ======================================================================== */
static jr_face_t phase_to_face(jr_state_t p)
{
    switch (p) {
    case JR_ST_LISTENING: return JR_FACE_LISTENING;
    case JR_ST_THINKING:
    case JR_ST_CONNECTING:
    case JR_ST_HANDSHAKING:
    case JR_ST_RECONNECTING: return JR_FACE_THINKING;
    case JR_ST_SPEAKING:  return JR_FACE_SPEAKING;
    case JR_ST_BACKOFF:
    case JR_ST_FATAL:     return JR_FACE_ERROR;
    default:              return JR_FACE_IDLE;
    }
}

/* ======================================================================== *
 *  diag HTTP: snapshot + /api/debug/say + /api/debug/gain                  *
 * ======================================================================== */
static esp_err_t diag_get_handler(httpd_req_t *req)
{
    const jr_state_snapshot_t *s = jr_orch_snapshot(&s_app.orch);
    char buf[512];
    int n = snprintf(buf, sizeof buf,
        "{\"phase\":\"%s\",\"transitions\":%u,\"deaths\":%u,\"reconnects\":%u,"
        "\"fail_count\":%u,\"last_reason\":\"%s\",\"last_error_kind\":%d,"
        "\"aec_us\":%u,\"ws_connected\":%s,\"capturing\":%s}",
        jr_state_name(s->phase), (unsigned)s->transitions, (unsigned)s->deaths,
        (unsigned)s->reconnects, (unsigned)s->fail_count,
        jr_event_name(s->last_reason), (int)s->last_error_kind,
        (unsigned)jr_audio_last_aec_us(),
        s_app.ws.state(s_app.ws.ctx) == JR_WS_OPEN ? "true" : "false",
        s_app.io.capturing ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
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

static esp_err_t say_get_handler(httpd_req_t *req)
{
    char q[256];
    char text[200] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK ||
        httpd_query_key_value(q, "text", text, sizeof text) != ESP_OK ||
        text[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?text=");
        return ESP_OK;
    }
    /* hand off to the single-writer app task */
    if (s_say_q) {
        xQueueSend(s_say_q, text, 0);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t gain_get_handler(httpd_req_t *req)
{
    int mic = -1, ref = -1, vol = -1;
    query_int(req, "mic", &mic);
    query_int(req, "ref", &ref);
    query_int(req, "vol", &vol);
    jr_audio_set_gains(mic, ref, vol);
    char buf[96];
    int n = snprintf(buf, sizeof buf, "{\"ok\":true,\"mic\":%d,\"ref\":%d,\"vol\":%d}",
                     mic, ref, vol);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static void start_diag_http(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "diag httpd failed to start");
        return;
    }
    httpd_uri_t routes[] = {
        { .uri = "/api/gemini/live", .method = HTTP_GET, .handler = diag_get_handler },
        { .uri = "/api/debug/say",   .method = HTTP_GET, .handler = say_get_handler  },
        { .uri = "/api/debug/gain",  .method = HTTP_GET, .handler = gain_get_handler },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; ++i) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "diag http up: /api/gemini/live, /api/debug/say, /api/debug/gain");
}

/* ======================================================================== *
 *  the single-writer voice task                                            *
 * ======================================================================== */
static void handle_say(const char *text)
{
    /* ensure a session is up, then send the text turn (model replies w/ audio) */
    jr_state_t p = jr_orch_phase(&s_app.orch);
    uint64_t now = jr_clock_now_ms(&s_app.clock);
    if (p == JR_ST_IDLE || p == JR_ST_BACKOFF || p == JR_ST_FATAL) {
        jr_orch_inject(&s_app.orch, jr_event(JR_EV_USER_START), now);
        jr_orch_step(&s_app.orch, now);   /* drive Connect -> Handshaking -> Listening */
    }
    /* send the text turn through the transport (SendText path); the model
     * replies with audio that flows back through rich_cb -> playback ring. */
    s_app.rvc.send_text(s_app.rvc.ctx, text);
}

static void voice_task(void *arg)
{
    (void)arg;
    static jr_pcm_t mic_frame[VOICE_FRAME_SAMPLES];
    char say[200];

    for (;;) {
        uint64_t now = jr_clock_now_ms(&s_app.clock);

        /* 1) pump the pure orchestrator (drains inbound, executes commands) */
        jr_orch_step(&s_app.orch, now);

        /* 2) manual/PTT input (HAL touch; stub until the CST9217 path lands) */
        jr_input_event_t iev;
        while (jr_input_poll(&s_app.input, &iev)) {
            jr_state_t p = jr_orch_phase(&s_app.orch);
            if (iev.kind == JR_INPUT_LONG_PRESS) {
                jr_orch_inject(&s_app.orch, jr_event(JR_EV_USER_STOP), now);
            } else if (iev.kind == JR_INPUT_TAP) {
                if (p == JR_ST_IDLE || p == JR_ST_BACKOFF || p == JR_ST_FATAL) {
                    jr_orch_inject(&s_app.orch, jr_event(JR_EV_USER_START), now);
                } else if (p == JR_ST_LISTENING) {
                    jr_orch_inject(&s_app.orch, jr_event(JR_EV_SPEECH_ENDED), now);
                } else {
                    jr_orch_inject(&s_app.orch, jr_event(JR_EV_TAP), now);
                }
            }
        }

        /* 3) diag say-mailbox */
        if (s_say_q && xQueueReceive(s_say_q, say, 0) == pdTRUE) {
            handle_say(say);
        }

        /* 4) mic uplink while capturing (paces the loop via the codec read) */
        bool read_paced = false;
        if (s_app.io.capturing) {
            int n = jr_audio_source_read(&s_app.mic, mic_frame, VOICE_FRAME_SAMPLES);
            if (n > 0) {
                jr_err_t r = s_app.rvc.send_audio(s_app.rvc.ctx, mic_frame, (size_t)n);
                jr_orch_report_tx(&s_app.orch, r);
                read_paced = true;
            }
        }

        /* 5) reflect the phase on the face (minimal, on change only) */
        jr_face_t f = phase_to_face(jr_orch_phase(&s_app.orch));
        if (f != s_app.last_face) {
            s_app.last_face = f;
            jr_display_present(&s_app.display, f, 0);
        }

        if (!read_paced) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* ======================================================================== *
 *  boot                                                                    *
 * ======================================================================== */
static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=====================================================");
    ESP_LOGI(TAG, " JarvisRobot v5  |  hexagonal core  |  voice boot     ");
    ESP_LOGI(TAG, "=====================================================");

    init_nvs();

    /* L0 board bring-up */
    esp_err_t hal_err = jr_hal_init();
    if (hal_err != ESP_OK) {
        ESP_LOGE(TAG, "jr_hal_init failed: %s — continuing headless", esp_err_to_name(hal_err));
    }

    /* pull the concrete ports */
    s_app.clock   = jr_hal_clock();
    s_app.display = jr_hal_display();
    s_app.input   = jr_hal_input();

    /* idle face immediately (Phase-0 acceptance target) */
    s_app.last_face = JR_FACE_IDLE;
    jr_display_present(&s_app.display, JR_FACE_IDLE, 0);

    /* network + config */
    if (jr_net_init() == ESP_OK) {
        esp_err_t wc = jr_net_wifi_connect();
        if (wc != ESP_OK) {
            ESP_LOGW(TAG, "wifi not connected (%s) — diag/session will retry once provisioned",
                     esp_err_to_name(wc));
        }
    }

    /* audio path */
    if (jr_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "jr_audio_init degraded — capture may be silent until on-device tune");
    }
    s_app.mic = jr_audio_source();
    s_app.spk = jr_audio_sink();

    /* build the Gemini endpoint URL: base + key from NVS (never in-repo) */
    char key[320] = {0};
    if (jr_cfg_get_str("llm_api_key", key, sizeof key) == ESP_OK && key[0] != '\0') {
        snprintf(s_app.url, sizeof s_app.url, "%s?key=%s", GEMINI_WS_BASE, key);
        ESP_LOGI(TAG, "gemini endpoint configured (key len=%u)", (unsigned)strlen(key));
    } else {
        snprintf(s_app.url, sizeof s_app.url, "%s", GEMINI_WS_BASE);
        ESP_LOGW(TAG, "no llm_api_key in NVS 'app' — provision before a session will connect");
    }

    /* L2 transport: device WS byte transport under the host-tested framer */
    jr_gemini_ws_init(s_app.url);
    s_app.ws = jr_gemini_ws();

    memset(&s_app.cfg, 0, sizeof s_app.cfg);
    s_app.cfg.url          = s_app.url;
    s_app.cfg.model        = JR_GEMINI_MODEL_PRIMARY;
    s_app.cfg.thinking_level = "low";
    s_app.cfg.vad_mode     = JR_VAD_MANUAL_LOCAL_RMS;

    jr_gemini_client_init(&s_app.client, s_app.ws, s_app.clock, &s_app.cfg);
    jr_gemini_client_set_event_cb(&s_app.client, rich_cb, &s_app);
    s_app.rvc = jr_gemini_client_as_rvc(&s_app.client);

    /* L3 orchestrator: inject the real I/O port */
    jr_orch_io_t io;
    io.ctx = &s_app;
    io.poll_inbound = voice_poll;
    io.exec = voice_exec;
    jr_orch_init(&s_app.orch, s_app.clock, io, JR_VAD_MANUAL_LOCAL_RMS);

    /* diag + the single-writer pump task */
    s_say_q = xQueueCreate(4, sizeof(char[200]));
    start_diag_http();

    ESP_LOGI(TAG, "core state = %s — spawning voice task", jr_state_name(jr_orch_phase(&s_app.orch)));
    xTaskCreatePinnedToCore(voice_task, "jr_voice", 8192, NULL, 7, NULL, 1);

    ESP_LOGI(TAG, "boot complete — idle, awaiting arm (tap or /api/debug/say)");
}
