/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gemini Live voice capability — all 5 phases:
 *   Phase 1: WSS+TLS handshake + setup/setupComplete
 *   Phase 2: Text round-trip (clientContent → modelTurn console print)
 *   Phase 3: Receive audio (base64 PCM24k → ES8311 DAC)
 *   Phase 4: Send audio (ES7210 ADC 16kHz → realtimeInput)
 *   Phase 5: Touch toggle + emote status overlay
 *
 * Half-duplex I2S constraint (SOC_I2S_HW_VERSION_1, shared clock):
 *   LISTEN: I2S @ 16kHz, TX+RX both open (TX silent, shared clock drives RX)
 *   SPEAK:  close ADC+DAC, reopen DAC @ 24kHz, play, then reopen for LISTEN
 *
 * NOTE: Gemini Live field names — the plan says realtimeInput.audio.data.
 * If the server rejects that format, switch GEMINI_AUDIO_KEY to "mediaChunks"
 * and wrap the chunk in an array: [{"mimeType":...,"data":...}].
 */

#include "cap_gemini_live.h"
#include "cmd_cap_gemini_live.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_task.h"
#include "emote.h"
#include "driver/i2s_common.h"
#include "esp_board_manager_includes.h"
#include "esp_board_periph.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "jarvis_brain.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "cap_gemini_live";

/* forward declarations */
static cJSON *gl_get_object_compat(cJSON *obj, const char *camel, const char *snake);
static bool   gl_enter_speaking(uint32_t sample_rate);
static void   gl_resume_listening(const char *reason);

/* ---- Configuration -------------------------------------------------------- */

/* gemini-3.1-flash-live-preview never existed in production API (404 from server).
 * gemini-2.0-flash-live-001 was shut down 2025-12-09.
 * Current live model confirmed via API models list 2026-05-23. */
#define GEMINI_LIVE_MODEL        "models/gemini-2.5-flash-native-audio-latest"
#define GEMINI_WS_HOST           "generativelanguage.googleapis.com"
#define GEMINI_WS_PATH           "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"
/* JarvisMCP gateway — one authenticated POST runs JS (jarvis.* SDK) and returns
 * JSON. Both the endpoint URL and bearer token come from app_config (NVS) and are
 * NEVER hardcoded here — set jarvis_mcp_url + jarvis_mcp_key via POST /api/config.
 * If either is empty the tool bridge is inert (function calls return an error). */
#define GL_ACT_RESP_MAX          4096
#define GEMINI_PERSONA           "You are JARVIS, a witty British AI butler speaking through a small smart-speaker. " \
                                 "Be extremely concise: answer in ONE short sentence whenever possible, never more than two. " \
                                 "When asked for live facts (weather, news, prices, time), use the search tool, then state just the answer. " \
                                 "No lists, no preamble, no markdown — this is spoken aloud."

/* If the server rejects realtimeInput.audio, set to "mediaChunks" */
#define GEMINI_AUDIO_KEY         "audio"

#define GL_RX_BUF_SIZE           (96 * 1024)   /* WS reassembly buffer (PSRAM) — audio turns can be ~60KB */
#define GL_RX_QUEUE_DEPTH        256           /* completed-frame queue depth: Gemini bursts a full reply faster than
                                                * real-time playback, so this must buffer the whole burst (~30s audio,
                                                * ~2MB PSRAM peak). At 16 it overflowed and dropped ~12s of a reply. */
#define GL_TX_CHUNK_MS           20             /* mic capture interval */
#define GL_TX_SAMPLE_RATE        16000
#define GL_RX_SAMPLE_RATE        24000
#define GL_CHANNELS              1
#define GL_BITS                  16
/* Make-up gain + soft-knee limiter for model audio. Measured PCM is speech with
 * a high crest factor: peaks ~80% full-scale but RMS only ~15%, so it sounds
 * quiet. A flat gain big enough to raise the average hard-clips the peaks into
 * distortion (that was the earlier "can't understand" at 4x). Instead apply
 * GL_OUT_GAIN to lift the body, then compress 4:1 above GL_LIMIT_KNEE so loud
 * syllables are limited smoothly, not squared off. */
#define GL_OUT_GAIN              4
#define GL_LIMIT_KNEE            24000
#define GL_TX_SAMPLES_PER_CHUNK  (GL_TX_SAMPLE_RATE * GL_TX_CHUNK_MS / 1000)  /* 320 */
#define GL_TX_PCM_BYTES          (GL_TX_SAMPLES_PER_CHUNK * GL_CHANNELS * (GL_BITS / 8)) /* 640 */
#define GL_TX_B64_BYTES          (((GL_TX_PCM_BYTES + 2) / 3) * 4 + 1)
#define GL_TOUCH_POLL_MS         100
/* Ignore taps within this window of the last accepted one. A session start runs
 * a multi-second WSS+TLS handshake; rapid taps otherwise toggle start/stop mid-
 * connect and wedge the session on "connecting". */
#define GL_TOGGLE_COOLDOWN_MS    2000
#define GL_WS_TIMEOUT_MS         5000
#define GL_SPEAK_WATCHDOG_MS     4500
#define GL_LISTEN_RECOVERY_MS    2000
#define GL_CODEC_HANDLE_RETRY_MS 250
#define GL_CODEC_HANDLE_RETRIES  6
#define GL_I2S_READ_TIMEOUT_MS   200
#define GL_AUDIO_WRITE_MARGIN_MS 250
#define GL_AUDIO_DAC_PERIPH      "i2s_audio_out"
#define GL_AUDIO_ADC_PERIPH      "i2s_audio_in"

/* ---- State ---------------------------------------------------------------- */

typedef enum {
    GL_STATE_IDLE = 0,
    GL_STATE_CONNECTING,
    GL_STATE_READY,        /* setupComplete received */
    GL_STATE_LISTENING,
    GL_STATE_THINKING,
    GL_STATE_SPEAKING,
} gl_state_t;

/* EventGroup bits */
#define GL_BIT_STOP         (1 << 0)
#define GL_BIT_SESSION_ON   (1 << 1)
#define GL_BIT_TX_STOP      (1 << 2)
#define GL_BIT_TX_DONE      (1 << 3)
#define GL_BIT_SETUP_OK     (1 << 4)
#define GL_BIT_SESSION_DONE (1 << 5)

typedef struct {
    char                         api_key[320];
    char                         mcp_key[192];      /* JarvisMCP /act bearer token (NVS) */
    char                         mcp_url[192];      /* JarvisMCP /act endpoint URL (NVS) */
    volatile gl_state_t          state;
    volatile bool                stop_requested;
    volatile bool                session_active;
    esp_websocket_client_handle_t ws_client;
    volatile bool                ws_connected;
    char                        *rx_buf;            /* GL_RX_BUF_SIZE reassembly scratch, PSRAM */
    QueueHandle_t                rx_queue;           /* completed frames (char*, PSRAM) → session task */
    volatile uint32_t            rx_drops;           /* frames dropped when queue full */
    volatile uint32_t            rx_frames;          /* parsed server frames */
    uint32_t                     text_part_hits;
    uint32_t                     audio_part_hits;
    uint32_t                     turn_complete_hits;
    uint32_t                     generation_complete_hits;
    uint32_t                     interrupted_hits;
    uint32_t                     tool_call_hits;
    uint32_t                     unhandled_hits;
    uint32_t                     resume_count;
    uint32_t                     watchdog_resume_count;
    int64_t                      last_frame_us;
    int64_t                      last_audio_us;
    int64_t                      last_resume_us;
    bool                         waiting_terminal;
    char                         last_resume_reason[32];
    char                         last_audio_error[96];
    EventGroupHandle_t           ev;
    TaskHandle_t                 session_task;
    TaskHandle_t                 touch_task;
    TaskHandle_t                 tx_task;
    SemaphoreHandle_t            ws_mutex;          /* serialises WS sends */
    esp_codec_dev_handle_t       dac;
    esp_codec_dev_handle_t       adc;
    i2s_chan_handle_t           adc_chan;
    i2s_chan_handle_t           dac_chan;
    bool                         adc_raw;
    bool                         dac_raw;
    bool                         dac_codec_failed;
    bool                         adc_codec_failed;
    esp_lcd_touch_handle_t       touch;
    /* I2S codec open-state tracking. esp_codec_dev_open/close are NOT idempotent:
     * a redundant close drives i2s_channel_disable on an already-disabled channel
     * ("channel has not been enabled yet") and wedges the audio path under rapid
     * start/stop. Track open state + the rate so open is rate-aware and close is
     * a no-op when already closed. */
    bool                         dac_open;
    uint32_t                     dac_rate;
    uint32_t                     dac_raw_rate_hz;
    bool                         adc_open;
    uint32_t                     adc_rate;
    uint32_t                     adc_raw_rate_hz;
    uint32_t                     last_audio_mime_rate;
    uint32_t                     rate_mismatch_chunks;
    uint32_t                     tx_frames_sent;
    uint32_t                     tx_send_failures;
    uint32_t                     tx_read_failures;
    uint32_t                     tx_codec_reads;
    uint32_t                     tx_raw_reads;
    int64_t                      last_input_end_us;
    bool                         activity_open;
} gl_ctx_t;

static gl_ctx_t s_gl;

static const char *gl_state_name(gl_state_t st)
{
    switch (st) {
    case GL_STATE_IDLE:
        return "IDLE";
    case GL_STATE_CONNECTING:
        return "CONNECTING";
    case GL_STATE_READY:
        return "READY";
    case GL_STATE_LISTENING:
        return "LISTENING";
    case GL_STATE_THINKING:
        return "THINKING";
    case GL_STATE_SPEAKING:
        return "SPEAKING";
    default:
        return "UNKNOWN";
    }
}

static void gl_mark_resume_reason(const char *reason)
{
    if (!reason) {
        return;
    }
    strlcpy(s_gl.last_resume_reason, reason, sizeof(s_gl.last_resume_reason));
    s_gl.last_resume_us = esp_timer_get_time();
}

static void gl_set_audio_error(const char *reason)
{
    if (!reason) {
        return;
    }
    strlcpy(s_gl.last_audio_error, reason, sizeof(s_gl.last_audio_error));
}

static void gl_clear_audio_error(void)
{
    s_gl.last_audio_error[0] = '\0';
}

static void gl_reset_diag_counters(void)
{
    s_gl.rx_frames             = 0;
    s_gl.text_part_hits        = 0;
    s_gl.audio_part_hits       = 0;
    s_gl.turn_complete_hits    = 0;
    s_gl.generation_complete_hits = 0;
    s_gl.interrupted_hits      = 0;
    s_gl.tool_call_hits        = 0;
    s_gl.unhandled_hits        = 0;
    s_gl.resume_count          = 0;
    s_gl.watchdog_resume_count = 0;
    s_gl.last_frame_us         = 0;
    s_gl.last_audio_us         = 0;
    s_gl.last_resume_us        = 0;
    s_gl.waiting_terminal      = false;
    s_gl.last_resume_reason[0] = '\0';
    s_gl.last_audio_mime_rate  = 0;
    s_gl.last_audio_error[0]   = '\0';
    s_gl.rate_mismatch_chunks  = 0;
    s_gl.tx_frames_sent        = 0;
    s_gl.tx_send_failures      = 0;
    s_gl.tx_read_failures      = 0;
    s_gl.tx_codec_reads        = 0;
    s_gl.tx_raw_reads          = 0;
    s_gl.last_input_end_us     = 0;
    s_gl.activity_open         = false;
}

/* ---- Audio level (RMS) for the reactive waveform -------------------------- *
 * Updated lock-free from the audio TX task (mic) and the playback path (out),
 * read by the display layer via the public getters. Stored as int16 RMS
 * (0..32767); getters normalise to 0..1 float. Plain atomics, no locks. */
static _Atomic uint16_t s_mic_rms;   /* ES7210 capture level (LISTENING) */
static _Atomic uint16_t s_out_rms;   /* decoded playback level (SPEAKING) */

/* RMS of a block of mono int16 PCM, clamped to uint16. */
static uint16_t gl_compute_rms(const int16_t *samples, size_t n)
{
    if (!samples || n == 0) {
        return 0;
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t s = samples[i];
        acc += (uint64_t)(s * s);
    }
    double rms = sqrt((double)acc / (double)n);
    if (rms > 32767.0) {
        rms = 32767.0;
    }
    return (uint16_t)rms;
}

/* ---- Helpers -------------------------------------------------------------- */

static void gl_set_state(gl_state_t st, const char *detail)
{
    if (s_gl.state == GL_STATE_SPEAKING && st == GL_STATE_LISTENING) {
        s_gl.waiting_terminal = false;
    }
    if (st == GL_STATE_LISTENING) {
        s_gl.last_audio_us = 0;
    }
    s_gl.state = st;

    /* Settle the playback waveform whenever we are not speaking; the mic level
     * is zeroed by the TX task itself when capture stops. */
    if (st != GL_STATE_SPEAKING) {
        atomic_store(&s_out_rms, 0);
    }

    /* Drive the emote face from the single state-transition point so every
     * GL_STATE has a face (Connecting/Thinking were previously faceless).
     * Face setters are declared in emote.h (display teammate's voice states). */
    switch (st) {
    case GL_STATE_CONNECTING:
        emote_set_connecting();
        break;
    case GL_STATE_LISTENING:
        emote_set_listening();
        break;
    case GL_STATE_THINKING:
        emote_set_thinking();
        break;
    case GL_STATE_SPEAKING:
        ESP_LOGI(TAG, "gl_set_state: emote_set_speaking enter");
        emote_set_speaking();
        ESP_LOGI(TAG, "gl_set_state: emote_set_speaking done");
        break;
    case GL_STATE_IDLE:
        emote_set_voice_idle();
        break;
    case GL_STATE_READY:
    default:
        break;
    }

    emote_set_status_detail(detail ? detail : "");
}

static bool gl_ws_send_text(const char *json)
{
    if (!s_gl.ws_client || !s_gl.ws_connected) {
        return false;
    }
    int len = (int)strlen(json);
    xSemaphoreTake(s_gl.ws_mutex, portMAX_DELAY);
    int sent = esp_websocket_client_send_text(s_gl.ws_client, json, len, pdMS_TO_TICKS(GL_WS_TIMEOUT_MS));
    xSemaphoreGive(s_gl.ws_mutex);
    return sent == len;
}

/* Base64-encode pcm bytes into out_b64 (caller provides GL_TX_B64_BYTES+ buf) */
static bool gl_b64_encode(const uint8_t *in, size_t in_len, char *out_b64, size_t out_size, size_t *out_len)
{
    return mbedtls_base64_encode((unsigned char *)out_b64, out_size, out_len, in, in_len) == 0;
}

static esp_lcd_touch_handle_t gl_resolve_touch_handle(void *touch_h)
{
    /* Board-manager returns dev_lcd_touch_i2c_handles_t* (or dev_lcd_touch_handles_t*).
     * Both structs have esp_lcd_touch_handle_t as their first member, so dereference
     * once to get the actual driver handle rather than the wrapper struct pointer. */
    return *(esp_lcd_touch_handle_t *)touch_h;
}

static uint32_t gl_parse_audio_rate_mime(const char *mime_type)
{
    if (!mime_type) {
        return 0;
    }

    const char *p = strstr(mime_type, "rate=");
    if (!p) {
        return 0;
    }
    p += 5;

    uint32_t rate = 0;
    bool has_digit = false;
    while (*p && isdigit((unsigned char)*p)) {
        has_digit = true;
        rate = rate * 10 + (uint32_t)(*p - '0');
        p++;
    }

    return has_digit ? rate : 0;
}

static uint32_t gl_i2s_cfg_sample_rate(const periph_i2s_config_t *cfg, i2s_dir_t dir)
{
    if (!cfg) {
        return 0;
    }

    switch (cfg->mode) {
    case I2S_COMM_MODE_STD:
        if (cfg->i2s_cfg.std.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.std.clk_cfg.sample_rate_hz;
        }
        break;
    case I2S_COMM_MODE_PDM:
        if (dir == I2S_DIR_RX && cfg->i2s_cfg.pdm_rx.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.pdm_rx.clk_cfg.sample_rate_hz;
        }
        if (dir == I2S_DIR_TX && cfg->i2s_cfg.pdm_tx.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.pdm_tx.clk_cfg.sample_rate_hz;
        }
        break;
    case I2S_COMM_MODE_TDM:
        if (cfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz;
        }
        break;
    default:
        break;
    }

    if (dir == I2S_DIR_RX) {
        if (cfg->i2s_cfg.pdm_rx.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.pdm_rx.clk_cfg.sample_rate_hz;
        }
        if (cfg->i2s_cfg.std.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.std.clk_cfg.sample_rate_hz;
        }
    } else {
        if (cfg->i2s_cfg.pdm_tx.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.pdm_tx.clk_cfg.sample_rate_hz;
        }
        if (cfg->i2s_cfg.std.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.std.clk_cfg.sample_rate_hz;
        }
        if (cfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz > 0) {
            return cfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz;
        }
    }
    return 0;
}

static uint32_t gl_resolve_playback_rate(uint32_t model_rate)
{
    uint32_t source = model_rate ? model_rate : GL_RX_SAMPLE_RATE;
    if (s_gl.dac_raw && s_gl.dac_raw_rate_hz) {
        return s_gl.dac_raw_rate_hz;
    }
    return source;
}

static uint32_t gl_audio_write_timeout_ms(size_t bytes, uint32_t sample_rate)
{
    uint32_t rate = sample_rate ? sample_rate : GL_RX_SAMPLE_RATE;
    uint32_t bytes_per_sec = rate * GL_CHANNELS * (GL_BITS / 8);
    if (!bytes_per_sec) {
        return GL_TX_CHUNK_MS + GL_AUDIO_WRITE_MARGIN_MS;
    }
    uint32_t ms = (uint32_t)((bytes * 1000ULL) / bytes_per_sec) + GL_AUDIO_WRITE_MARGIN_MS;
    if (ms < GL_TX_CHUNK_MS + GL_AUDIO_WRITE_MARGIN_MS) {
        ms = GL_TX_CHUNK_MS + GL_AUDIO_WRITE_MARGIN_MS;
    }
    return ms;
}

static uint32_t gl_clamp_rate(uint32_t rate)
{
    if (rate == 0) {
        return GL_RX_SAMPLE_RATE;
    }
    return rate;
}

/* ---- Board device handles ------------------------------------------------- */

static void gl_reset_audio_path_state(void)
{
    s_gl.dac = NULL;
    s_gl.adc = NULL;
    s_gl.dac_chan = NULL;
    s_gl.adc_chan = NULL;
    s_gl.dac_raw = false;
    s_gl.adc_raw = false;
    s_gl.dac_raw_rate_hz = 0;
    s_gl.adc_raw_rate_hz = 0;
    s_gl.dac_rate = 0;
    s_gl.adc_rate = 0;
}

static esp_codec_dev_handle_t gl_extract_codec_handle(void *dev_h)
{
    if (!dev_h) {
        return NULL;
    }
    return ((dev_audio_codec_handles_t *)dev_h)->codec_dev;
}

static bool gl_acquire_codec_handles(void)
{
    /* esp_board_manager_get_device_handle() writes the device's INNER handle
     * struct pointer directly into the out-ptr (e.g. dev_audio_codec_handles_t*),
     * NOT the esp_board_device_handle_t wrapper. Reading ->device_handle off it
     * double-derefs and yields a flash-mapped junk pointer (0x420b...), which
     * crashes esp_codec_dev_open with LoadStoreError. Cast the out-ptr directly,
     * mirroring main.c's patch-0010 touch fix and the board_manager example
     * (record_and_play.c: dac_handle->codec_dev). */
    void *dac_h = NULL;
    void *adc_h = NULL;
    void *touch_h = NULL;
    void *raw_dac_h = NULL;
    void *raw_adc_h = NULL;
    void *raw_adc_cfg_h = NULL;
    void *raw_dac_cfg_h = NULL;

    gl_reset_audio_path_state();

    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, &dac_h) == ESP_OK &&
        dac_h) {
        s_gl.dac = gl_extract_codec_handle(dac_h);
    }
    if (!s_gl.dac && esp_board_manager_get_device_handle("fake_audio_dac", &dac_h) == ESP_OK &&
        dac_h) {
        s_gl.dac = gl_extract_codec_handle(dac_h);
    }
    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_ADC, &adc_h) == ESP_OK &&
        adc_h) {
        s_gl.adc = gl_extract_codec_handle(adc_h);
    }
    if (esp_board_periph_get_handle(GL_AUDIO_ADC_PERIPH, &raw_adc_h) == ESP_OK &&
        raw_adc_h) {
        s_gl.adc_raw = true;
        s_gl.adc_chan = (i2s_chan_handle_t)raw_adc_h;
    }
    if (esp_board_periph_get_config(GL_AUDIO_ADC_PERIPH, (void **)&raw_adc_cfg_h) == ESP_OK &&
        raw_adc_cfg_h) {
        s_gl.adc_raw_rate_hz = gl_i2s_cfg_sample_rate((const periph_i2s_config_t *)raw_adc_cfg_h,
                                                     I2S_DIR_RX);
    }
    if (esp_board_periph_get_handle(GL_AUDIO_DAC_PERIPH, &raw_dac_h) == ESP_OK &&
        raw_dac_h) {
        s_gl.dac_raw = true;
        s_gl.dac_chan = (i2s_chan_handle_t)raw_dac_h;
    }
    if (esp_board_periph_get_config(GL_AUDIO_DAC_PERIPH, (void **)&raw_dac_cfg_h) == ESP_OK &&
        raw_dac_cfg_h) {
        s_gl.dac_raw_rate_hz = gl_i2s_cfg_sample_rate((const periph_i2s_config_t *)raw_dac_cfg_h,
                                                     I2S_DIR_TX);
    }

    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_h) == ESP_OK &&
        touch_h) {
        s_gl.touch = gl_resolve_touch_handle(touch_h);
    }

    if (!s_gl.dac && !s_gl.adc && !s_gl.dac_raw && !s_gl.adc_raw) {
        gl_set_audio_error("no audio device handles");
    }

    ESP_LOGI(TAG, "Audio paths: dac=%p raw_dac=%p adc=%p raw_adc=%p touch=%p",
             (void *)s_gl.dac, (void *)s_gl.dac_chan, (void *)s_gl.adc,
             (void *)s_gl.adc_chan, (void *)s_gl.touch);
    return s_gl.dac || s_gl.adc || s_gl.dac_raw || s_gl.adc_raw;
}

static bool gl_ensure_codec_handles(const char *tag, bool require_dac, bool require_adc)
{
    bool need_dac = require_dac &&
                    !(s_gl.dac_raw || (s_gl.dac && !s_gl.dac_codec_failed));
    bool need_adc = require_adc &&
                    !(s_gl.adc_raw || (s_gl.adc && !s_gl.adc_codec_failed));

    if (!need_dac && !need_adc) {
        return true;
    }

    for (int i = 0; i < GL_CODEC_HANDLE_RETRIES; ++i) {
        if (gl_acquire_codec_handles() &&
            !need_dac && !need_adc) {
            return true;
        }
        if (!need_dac || s_gl.dac_raw || (s_gl.dac && !s_gl.dac_codec_failed)) {
            need_dac = false;
        }
        if (!need_adc || s_gl.adc_raw || (s_gl.adc && !s_gl.adc_codec_failed)) {
            need_adc = false;
        }
        if (!need_dac && !need_adc) {
            if (i > 0) {
                ESP_LOGI(TAG, "%s: audio handles ready after %d attempts",
                         tag ? tag : "codec", i + 1);
            }
            gl_clear_audio_error();
            return true;
        }

        if (i + 1 < GL_CODEC_HANDLE_RETRIES) {
            ESP_LOGW(TAG, "%s: audio handles unavailable (dac=%p raw_dac=%d adc=%p raw_adc=%d), retrying (%d/%d)",
                     tag ? tag : "codec", (void *)s_gl.dac, (int)s_gl.dac_raw,
                     (void *)s_gl.adc, (int)s_gl.adc_raw,
                     i + 1, GL_CODEC_HANDLE_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(GL_CODEC_HANDLE_RETRY_MS));
        }
    }

    if (need_dac && need_adc) {
        gl_set_audio_error("audio path incomplete (missing both input and output)");
    } else if (need_dac) {
        gl_set_audio_error("missing output path (audio_dac/fake_audio_dac or i2s_audio_out)");
    } else if (need_adc) {
        gl_set_audio_error("missing input path (audio_adc or i2s_audio_in)");
    }
    return false;
}

/* ---- I2S open/close helpers ----------------------------------------------- */

static int gl_open_dac(uint32_t sample_rate)
{
    if (!gl_ensure_codec_handles("open_dac", true, false)) {
        ESP_LOGE(TAG, "DAC handle unavailable");
        gl_set_audio_error("output path missing while opening DAC");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_gl.dac && !s_gl.dac_codec_failed) {
        /* Already open at this rate → no-op. Open at a different rate (TX 16k vs RX
         * 24k) → close first so the codec re-inits cleanly. */
        if (s_gl.dac_open) {
            if (s_gl.dac_rate == sample_rate) {
                return ESP_CODEC_DEV_OK;
            }
            esp_codec_dev_close(s_gl.dac);
            s_gl.dac_open = false;
        }
        esp_codec_dev_sample_info_t fs = {
            .sample_rate     = sample_rate,
            .channel         = GL_CHANNELS,
            .bits_per_sample = GL_BITS,
        };
        int r = esp_codec_dev_open(s_gl.dac, &fs);
        ESP_LOGI(TAG, "esp_codec_dev_open(dac, %u Hz) = %d (%s)", (unsigned)sample_rate, r, esp_err_to_name(r));
        if (r == ESP_CODEC_DEV_OK) {
            s_gl.dac_codec_failed = false;
            esp_codec_dev_set_out_mute(s_gl.dac, false);
            esp_codec_dev_set_out_vol(s_gl.dac, 100);
            s_gl.dac_open = true;
            s_gl.dac_rate = sample_rate;
            return r;
        }
        s_gl.dac_codec_failed = true;
        ESP_LOGW(TAG, "DAC open failed rate=%u err=%d (%s), trying raw I2S if available",
                 (unsigned)sample_rate, r, esp_err_to_name(r));
    }

    if (s_gl.dac_raw) {
        s_gl.dac_open = true;
        s_gl.dac_rate = sample_rate;
        return ESP_CODEC_DEV_OK;
    }

    if (s_gl.dac) {
        gl_set_audio_error("DAC open failed (codec handle)");
    }
    return ESP_ERR_INVALID_STATE;
}

static int gl_open_adc(uint32_t sample_rate)
{
    if (!gl_ensure_codec_handles("open_adc", false, true)) {
        ESP_LOGE(TAG, "ADC handle unavailable");
        gl_set_audio_error("input path missing while opening ADC");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_gl.adc && !s_gl.adc_codec_failed) {
        if (s_gl.adc_open) {
            if (s_gl.adc_rate == sample_rate) {
                return ESP_CODEC_DEV_OK;
            }
            esp_codec_dev_close(s_gl.adc);
            s_gl.adc_open = false;
        }
        esp_codec_dev_sample_info_t fs = {
            .sample_rate     = sample_rate,
            .channel         = GL_CHANNELS,
            .bits_per_sample = GL_BITS,
        };
        int r = esp_codec_dev_open(s_gl.adc, &fs);
        if (r == ESP_CODEC_DEV_OK) {
            s_gl.adc_codec_failed = false;
            esp_codec_dev_set_in_gain(s_gl.adc, 30.0f);
            s_gl.adc_open = true;
            s_gl.adc_rate = sample_rate;
            return r;
        }
        s_gl.adc_codec_failed = true;
        ESP_LOGW(TAG, "ADC open failed rate=%u err=%d (%s), trying raw I2S if available",
                 (unsigned)sample_rate, r, esp_err_to_name(r));
    }

    if (s_gl.adc_raw) {
        s_gl.adc_open = true;
        s_gl.adc_rate = sample_rate;
        return ESP_CODEC_DEV_OK;
    }

    if (s_gl.adc) {
        gl_set_audio_error("ADC open failed (codec handle)");
    }
    return ESP_ERR_INVALID_STATE;
}

static void gl_close_dac(void)
{
    if (s_gl.dac && s_gl.dac_open && !s_gl.dac_codec_failed) {
        esp_codec_dev_close(s_gl.dac);
    }
    s_gl.dac_open = false;
    s_gl.dac_rate = 0;
}

static void gl_close_adc(void)
{
    if (s_gl.adc && s_gl.adc_open && !s_gl.adc_codec_failed) {
        esp_codec_dev_close(s_gl.adc);
    }
    s_gl.adc_open = false;
    s_gl.adc_rate = 0;
}

/* ---- JSON send helpers ---------------------------------------------------- */

static bool gl_send_setup(void)
{
    cJSON *root  = cJSON_CreateObject();
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON_AddStringToObject(setup, "model", GEMINI_LIVE_MODEL);

    /* Google Search grounding — lets the model fetch live data (weather, news,
     * facts) and speak a grounded answer with NO client-side tool handling. Raw
     * Live-API setup form: "tools":[{"googleSearch":{}}] (proto JSON accepts the
     * camelCase form; the cookbook uses google_search — both parse). */
    cJSON *tools = cJSON_AddArrayToObject(setup, "tools");
    cJSON *gs = cJSON_CreateObject();
    cJSON_AddItemToObject(gs, "googleSearch", cJSON_CreateObject());
    cJSON_AddItemToArray(tools, gs);

    /* Function calling → JarvisMCP /act bridge. DISCRIMINATOR STEP: one declared
     * function alongside googleSearch, to verify the two tool types coexist in
     * setup for this model (watch for setupComplete + a toolCall frame). The
     * handler + HTTPS call to the gateway are added once coexistence is proven. */
    cJSON *fd_tool = cJSON_CreateObject();
    cJSON *fdecls  = cJSON_AddArrayToObject(fd_tool, "functionDeclarations");
    cJSON *fn      = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", "crypto_price");
    cJSON_AddStringToObject(fn, "description",
                            "Get the current market price of a cryptocurrency by symbol or name.");
    cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *props  = cJSON_AddObjectToObject(params, "properties");
    cJSON *sym    = cJSON_AddObjectToObject(props, "symbol");
    cJSON_AddStringToObject(sym, "type", "string");
    cJSON_AddStringToObject(sym, "description", "Coin symbol or name, e.g. bitcoin, ethereum, solana");
    cJSON *reqd = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(reqd, cJSON_CreateString("symbol"));
    cJSON_AddItemToArray(fdecls, fn);
    cJSON_AddItemToArray(tools, fd_tool);

    cJSON *gc = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *rm = cJSON_AddArrayToObject(gc, "responseModalities");
    cJSON_AddItemToArray(rm, cJSON_CreateString("AUDIO"));
    /* thinkingLevel accepted by both gemini-2.5-flash-native-audio-latest and 3.x models.
     * "minimal" = lowest latency, appropriate for voice. Confirmed: setupComplete received. */
    cJSON *tc = cJSON_AddObjectToObject(gc, "thinkingConfig");
    cJSON_AddStringToObject(tc, "thinkingLevel", "minimal");

    /* Push-to-talk has explicit boundaries. Disable server-side VAD so quiet
     * speech or room noise cannot leave the turn stuck after button release. */
    cJSON *ric = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *aad = cJSON_AddObjectToObject(ric, "automaticActivityDetection");
    cJSON_AddBoolToObject(aad, "disabled", true);

    /* System instruction = JARVIS's persistent on-device self (identity +
     * recent memory from /sdcard/brain). Fail-soft: jarvis_brain_load_context
     * always returns a usable, NUL-terminated persona — the built-in default if
     * the SD store is unavailable — so this never breaks the WSS setup. */
    char persona[2048];
    jarvis_brain_load_context(persona, sizeof persona);

    cJSON *si   = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *parts = cJSON_AddArrayToObject(si, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", persona);
    cJSON_AddItemToArray(parts, part);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    bool ok = gl_ws_send_text(json);
    free(json);
    return ok;
}

static bool gl_send_text_turn(const char *text)
{
    cJSON *root   = cJSON_CreateObject();
    cJSON *cc     = cJSON_AddObjectToObject(root, "clientContent");
    cJSON *turns  = cJSON_AddArrayToObject(cc, "turns");
    cJSON *turn   = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "role", "user");
    cJSON *parts  = cJSON_AddArrayToObject(turn, "parts");
    cJSON *part   = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToArray(turns, turn);
    cJSON_AddBoolToObject(cc, "turnComplete", true);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    bool ok = gl_ws_send_text(json);
    free(json);
    return ok;
}

static bool gl_send_activity_start(void)
{
    return gl_ws_send_text("{\"realtimeInput\":{\"activityStart\":{}}}");
}

static bool gl_send_activity_end(void)
{
    return gl_ws_send_text("{\"realtimeInput\":{\"activityEnd\":{}}}");
}

static bool gl_begin_audio_activity(const char *reason)
{
    if (s_gl.activity_open) {
        return true;
    }
    bool ok = gl_send_activity_start();
    if (ok) {
        s_gl.activity_open = true;
        ESP_LOGI(TAG, "Audio activity start requested (%s)", reason ? reason : "listen");
    } else {
        s_gl.tx_send_failures++;
        ESP_LOGW(TAG, "Audio activity start send failed (%s)", reason ? reason : "listen");
    }
    return ok;
}

static bool gl_end_audio_activity(const char *reason)
{
    bool ok = true;
    if (s_gl.activity_open) {
        ok = gl_send_activity_end();
        if (ok) {
            ESP_LOGI(TAG, "Audio activity end requested (%s)", reason ? reason : "end_input");
        } else {
            s_gl.tx_send_failures++;
            ESP_LOGW(TAG, "Audio activity end send failed (%s)", reason ? reason : "end_input");
        }
    }
    s_gl.activity_open = false;
    return ok;
}

/* Send one PCM frame as a realtimeInput audio chunk */
static bool gl_send_audio_frame(const uint8_t *pcm, size_t len)
{
    static char b64[GL_TX_B64_BYTES];
    size_t b64_len = 0;

    if (!gl_b64_encode(pcm, len, b64, sizeof(b64), &b64_len)) {
        return false;
    }
    b64[b64_len] = '\0';

    /*
     * realtimeInput.audio.data — see GEMINI_AUDIO_KEY note at top.
     * Build the JSON without cJSON to avoid heap allocation in the hot path.
     */
    static char frame_json[GL_TX_B64_BYTES + 128];
    int n = snprintf(frame_json, sizeof(frame_json),
                     "{\"realtimeInput\":{\"%s\":{\"mimeType\":\"audio/pcm;rate=%d\","
                     "\"data\":\"%s\"}}}",
                     GEMINI_AUDIO_KEY, GL_TX_SAMPLE_RATE, b64);
    if (n <= 0 || n >= (int)sizeof(frame_json)) {
        return false;
    }

    return gl_ws_send_text(frame_json);
}

/* Convert PCM16 sample-rate using nearest-neighbour.
 * It's intentionally simple: it avoids extra memory copies and keeps latency low,
 * which is the important part for "raw" I2S playback in this stack. */
static int16_t *gl_resample_pcm16_nearest(const int16_t *in, size_t nsamp_in,
                                          uint32_t sample_rate_in, uint32_t sample_rate_out,
                                          size_t *nsamp_out)
{
    if (!in || nsamp_in == 0 || sample_rate_in == 0 || sample_rate_out == 0 ||
        sample_rate_in == sample_rate_out) {
        return NULL;
    }

    size_t out_samples = (size_t)(((uint64_t)nsamp_in * (uint64_t)sample_rate_out +
                                  (uint64_t)sample_rate_in - 1) / (uint64_t)sample_rate_in);
    if (out_samples == 0) {
        return NULL;
    }

    int16_t *out = heap_caps_malloc(out_samples * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        return NULL;
    }

    uint64_t step = ((uint64_t)sample_rate_in << 16) / (uint64_t)sample_rate_out;
    uint64_t pos = 0;
    for (size_t i = 0; i < out_samples; ++i) {
        size_t idx = (size_t)(pos >> 16);
        if (idx >= nsamp_in) {
            idx = nsamp_in - 1;
        }
        out[i] = in[idx];
        pos += step;
    }

    if (nsamp_out) {
        *nsamp_out = out_samples;
    }
    return out;
}

static bool gl_extract_audio_data_chunk(cJSON *audio, const char **out_data, uint32_t *out_rate)
{
    if (!audio || !out_data) {
        return false;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(audio, "data");
    cJSON *mime = cJSON_GetObjectItemCaseSensitive(audio, "mimeType");
    if (!cJSON_IsString(data)) {
        cJSON *inline_obj = gl_get_object_compat(audio, "inlineData", "inline_data");
        if (!inline_obj) {
            return false;
        }
        data = cJSON_GetObjectItemCaseSensitive(inline_obj, "data");
        mime = cJSON_GetObjectItemCaseSensitive(inline_obj, "mimeType");
    }

    if (!cJSON_IsString(data)) {
        return false;
    }

    *out_data = data->valuestring;
    if (out_rate) {
        if (cJSON_IsString(mime)) {
            *out_rate = gl_parse_audio_rate_mime(mime->valuestring);
        } else {
            *out_rate = 0;
        }
    }
    return true;
}

/* ---- Audio playback ------------------------------------------------------- */

/* Decode base64 PCM and play back to speaker (codec or raw I2S path). */
static void gl_play_audio_b64(const char *b64_str, uint32_t sample_rate)
{
    if ((!s_gl.dac && !s_gl.dac_raw) || !s_gl.dac_open) {
        gl_set_audio_error("playback: DAC unavailable");
        ESP_LOGW(TAG, "Speaking: no DAC handle/opened, dropping audio chunk");
        return;
    }

    uint32_t model_rate = gl_clamp_rate(sample_rate);
    uint32_t playback_rate = gl_resolve_playback_rate(model_rate);
    s_gl.last_audio_mime_rate = model_rate;
    if (s_gl.dac_raw && playback_rate == 0) {
        playback_rate = model_rate;
    }

    if (model_rate != playback_rate && s_gl.dac_raw) {
        s_gl.rate_mismatch_chunks++;
        ESP_LOGD(TAG, "audio rate mismatch: model=%u playback=%u; resampling",
                 model_rate, playback_rate);
    }

    size_t b64_len = strlen(b64_str);
    size_t pcm_max = (b64_len / 4) * 3 + 4;
    uint8_t *pcm = heap_caps_malloc(pcm_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        ESP_LOGE(TAG, "OOM for PCM decode buf");
        return;
    }
    if (gl_open_dac(playback_rate) != ESP_CODEC_DEV_OK) {
        free(pcm);
        return;
    }

    size_t pcm_len = 0;
    if (mbedtls_base64_decode(pcm, pcm_max, &pcm_len,
                              (const unsigned char *)b64_str, b64_len) != 0) {
        ESP_LOGE(TAG, "base64 decode failed");
        free(pcm);
        return;
    }
    if (pcm_len == 0 || (pcm_len & 0x1)) {
        ESP_LOGW(TAG, "PCM decode invalid len=%u", (unsigned)pcm_len);
        free(pcm);
        return;
    }

    /* Make-up gain + soft-knee limiter (see GL_OUT_GAIN comment). Also measure
     * the pre-gain peak/RMS and log it so the real signal level stays visible. */
    int16_t *s16 = (int16_t *)pcm;
    size_t   nsamp = pcm_len / sizeof(int16_t);
    int32_t  peak_in = 0;
    int64_t  sumsq = 0;
    for (size_t i = 0; i < nsamp; i++) {
        int32_t s = s16[i];
        int32_t a0 = s < 0 ? -s : s;
        if (a0 > peak_in) {
            peak_in = a0;
        }
        sumsq += (int64_t)s * s;

        int32_t v = s * GL_OUT_GAIN;
        int32_t a = v < 0 ? -v : v;
        if (a > GL_LIMIT_KNEE) {
            a = GL_LIMIT_KNEE + ((a - GL_LIMIT_KNEE) >> 2);   /* 4:1 above knee */
            if (a > 32767) {
                a = 32767;
            }
            v = (v < 0) ? -a : a;
        }
        s16[i] = (int16_t)v;
    }
    if (nsamp > 10) {
        int32_t rms_in = (int32_t)sqrt((double)(sumsq / (int64_t)nsamp));
        ESP_LOGD(TAG, "audio level: peak=%d rms=%d (full-scale=32767) gain=%d samples=%u",
                 (int)peak_in, (int)rms_in, GL_OUT_GAIN, (unsigned)nsamp);
    }

    int16_t  *out_pcm = s16;
    size_t    out_samples = nsamp;
    size_t    out_bytes = pcm_len;
    int16_t  *resampled = NULL;

    if (s_gl.dac_raw && model_rate != playback_rate) {
        resampled = gl_resample_pcm16_nearest((const int16_t *)s16, nsamp,
                                             model_rate, playback_rate, &out_samples);
        if (resampled) {
            out_pcm = resampled;
            out_bytes = out_samples * sizeof(int16_t);
            atomic_store(&s_out_rms, gl_compute_rms(out_pcm, out_samples));
        }
    }

    /* Publish playback level for the SPEAKING waveform (before the blocking write). */
    if (!resampled) {
        atomic_store(&s_out_rms, gl_compute_rms((const int16_t *)pcm, nsamp));
    }

    if (s_gl.stop_requested || !s_gl.session_active) {
        if (resampled) {
            free(resampled);
        }
        free(pcm);
        return;
    }

    if (s_gl.dac && !s_gl.dac_codec_failed) {
        if (esp_codec_dev_write(s_gl.dac, out_pcm, (int)out_bytes) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "DAC write failed: codec bytes=%u rate=%u",
                     (unsigned)out_bytes, (unsigned)playback_rate);
        }
    } else if (s_gl.dac_raw) {
        size_t bytes_written = 0;
        uint32_t timeout_ms = gl_audio_write_timeout_ms(out_bytes, playback_rate);
        esp_err_t err = i2s_channel_write(s_gl.dac_chan, out_pcm, out_bytes, &bytes_written,
                                          pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DAC write failed: i2s err=%d bytes=%u timeout_ms=%u rate=%u",
                     (int)err, (unsigned)out_bytes, (unsigned)timeout_ms, (unsigned)playback_rate);
        } else if (bytes_written != out_bytes) {
            ESP_LOGW(TAG, "DAC write short: %u != %u timeout_ms=%u",
                     (unsigned)bytes_written, (unsigned)out_bytes, (unsigned)timeout_ms);
        }
    }

    if (resampled) {
        free(resampled);
    }
    free(pcm);
}

/* ---- Audio TX task (Phase 4) --------------------------------------------- */

static void gl_audio_tx_task(void *arg)
{
    (void)arg;
    static uint8_t pcm[GL_TX_PCM_BYTES];

    /* Codec lifetime is owned by the session task. This worker only reads from
     * the already-open ADC so teardown cannot double-close codec/I2S handles
     * from two tasks. */
    ESP_LOGI(TAG, "Audio TX: started (16kHz capture)");
    if (!s_gl.adc && !s_gl.adc_raw) {
        gl_set_audio_error("TX: missing ADC path");
        ESP_LOGE(TAG, "Audio TX: missing ADC handle");
        xEventGroupSetBits(s_gl.ev, GL_BIT_TX_DONE);
        if (s_gl.tx_task == xTaskGetCurrentTaskHandle()) {
            s_gl.tx_task = NULL;
        }
        claw_task_delete(NULL);
    }

    while (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_TX_STOP)) {
        if (s_gl.state != GL_STATE_LISTENING) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (xEventGroupGetBits(s_gl.ev) & GL_BIT_TX_STOP) {
            break;
        }

        int r = ESP_FAIL;
        if (s_gl.adc && s_gl.adc_open && !s_gl.adc_codec_failed) {
            r = esp_codec_dev_read(s_gl.adc, pcm, GL_TX_PCM_BYTES);
            if (r == ESP_CODEC_DEV_OK) {
                s_gl.tx_codec_reads++;
            }
        }
        if (r != ESP_CODEC_DEV_OK && s_gl.adc_raw) {
            size_t bytes_read = 0;
            esp_err_t read_err = i2s_channel_read(s_gl.adc_chan, pcm, GL_TX_PCM_BYTES,
                                                  &bytes_read, pdMS_TO_TICKS(GL_I2S_READ_TIMEOUT_MS));
            if (read_err != ESP_OK || bytes_read != GL_TX_PCM_BYTES) {
                s_gl.tx_read_failures++;
                vTaskDelay(pdMS_TO_TICKS(GL_TX_CHUNK_MS));
                continue;
            }
            r = ESP_CODEC_DEV_OK;
            s_gl.tx_raw_reads++;
        }
        if (r != ESP_CODEC_DEV_OK) {
            s_gl.tx_read_failures++;
            vTaskDelay(pdMS_TO_TICKS(GL_TX_CHUNK_MS));
            continue;
        }

        /* Publish mic level for the LISTENING waveform. */
        atomic_store(&s_mic_rms,
                     gl_compute_rms((const int16_t *)pcm, GL_TX_PCM_BYTES / sizeof(int16_t)));

        if (gl_send_audio_frame(pcm, GL_TX_PCM_BYTES)) {
            s_gl.tx_frames_sent++;
        } else {
            s_gl.tx_send_failures++;
        }
    }

    /* Mic is quiet once capture stops. */
    atomic_store(&s_mic_rms, 0);

    ESP_LOGI(TAG, "Audio TX: stopped");
    xEventGroupSetBits(s_gl.ev, GL_BIT_TX_DONE);
    if (s_gl.tx_task == xTaskGetCurrentTaskHandle()) {
        s_gl.tx_task = NULL;
    }
    claw_task_delete(NULL);
}

static void gl_stop_tx_task(void)
{
    if (!s_gl.tx_task) {
        return;
    }
    xEventGroupClearBits(s_gl.ev, GL_BIT_TX_DONE);
    xEventGroupSetBits(s_gl.ev, GL_BIT_TX_STOP);
    EventBits_t bits = xEventGroupWaitBits(s_gl.ev, GL_BIT_TX_DONE,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
    if (!(bits & GL_BIT_TX_DONE)) {
        ESP_LOGE(TAG, "Audio TX: stop timed out; deleting wedged task");
        vTaskDelete(s_gl.tx_task);
        xEventGroupSetBits(s_gl.ev, GL_BIT_TX_DONE);
        atomic_store(&s_mic_rms, 0);
    }
    s_gl.tx_task = NULL;
}

static void gl_start_tx_task(void)
{
    if (s_gl.tx_task) {
        ESP_LOGW(TAG, "Audio TX: start ignored; task already running");
        return;
    }
    xEventGroupClearBits(s_gl.ev, GL_BIT_TX_STOP | GL_BIT_TX_DONE);
    static const claw_task_config_t tx_cfg = {
        .name         = "gl_audio_tx",
        .stack_size   = 8192,
        .priority     = 6,
        .core_id      = tskNO_AFFINITY,
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };
    if (claw_task_create(&tx_cfg, gl_audio_tx_task, NULL, &s_gl.tx_task) != pdPASS) {
        s_gl.tx_task = NULL;
        ESP_LOGE(TAG, "Audio TX: failed to create task");
    }
}

static void gl_ensure_listening_capture(void)
{
    static int64_t last_recover_us;
    if (s_gl.state != GL_STATE_LISTENING && s_gl.state != GL_STATE_READY) {
        return;
    }
    if (s_gl.tx_task) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if ((uint64_t)(now_us - last_recover_us) < GL_LISTEN_RECOVERY_MS * 1000LL) {
        return;
    }

    int r = gl_open_adc(GL_TX_SAMPLE_RATE);
    if (r != ESP_CODEC_DEV_OK) {
        gl_set_audio_error("capture retry: ADC open failed");
        ESP_LOGW(TAG, "Listening recovery: ADC open failed (err=%d)", r);
        last_recover_us = now_us;
        return;
    }
    ESP_LOGW(TAG, "Listening recovery: restarting capture task");
    last_recover_us = now_us;
    gl_begin_audio_activity("capture recovery");
    gl_start_tx_task();
}

static cJSON *gl_get_object_compat(cJSON *obj, const char *camel, const char *snake)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, camel);
    return item ? item : cJSON_GetObjectItemCaseSensitive(obj, snake);
}

static bool gl_play_model_audio_from_json(cJSON *audio_obj, uint32_t default_rate)
{
    const char *b64 = NULL;
    uint32_t sample_rate = default_rate;
    bool extracted = gl_extract_audio_data_chunk(audio_obj, &b64, &sample_rate);
    ESP_LOGD(TAG, "play_audio: obj=%s extracted=%d b64=%s rate=%u state=%s",
             audio_obj ? "present" : "null", (int)extracted,
             (b64 && extracted) ? "ok" : "null", (unsigned)sample_rate,
             gl_state_name(s_gl.state));
    if (!extracted) {
        return false;
    }

    if (!b64) {
        return false;
    }

    if (s_gl.state == GL_STATE_LISTENING || s_gl.state == GL_STATE_READY ||
        s_gl.state == GL_STATE_THINKING) {
        if (!gl_enter_speaking(sample_rate)) {
            gl_resume_listening("output-open-failed");
            return false;
        }
    }
    s_gl.audio_part_hits++;
    s_gl.last_audio_us = esp_timer_get_time();
    s_gl.last_audio_mime_rate = sample_rate;
    s_gl.waiting_terminal = true;
    gl_play_audio_b64(b64, sample_rate);
    return true;
}

static bool gl_enter_speaking(uint32_t sample_rate)
{
    if (s_gl.state == GL_STATE_SPEAKING) {
        return true;
    }

    /* First audio chunk: stop TX before any codec rate swap. The session task is
     * the only codec owner; the TX task must be fully parked before ADC/DAC close
     * or the shared I2S clock can be changed under a blocking read. */
    gl_stop_tx_task();
    ESP_LOGI(TAG, "Speaking: paused capture");
    s_gl.waiting_terminal = true;
    gl_close_adc();
    gl_set_state(GL_STATE_SPEAKING, "Speaking");

    uint32_t playback_rate = gl_resolve_playback_rate(sample_rate);
    ESP_LOGI(TAG, "enter_speaking: model_rate=%u resolved=%u dac=%p dac_codec_failed=%d dac_raw=%p",
             (unsigned)sample_rate, (unsigned)playback_rate,
             s_gl.dac, (int)s_gl.dac_codec_failed, s_gl.dac_raw);
    int r = gl_open_dac(playback_rate);
    ESP_LOGI(TAG, "enter_speaking: gl_open_dac(%u) = %d dac_open=%d",
             (unsigned)playback_rate, r, (int)s_gl.dac_open);
    if (r != ESP_CODEC_DEV_OK) {
        gl_set_audio_error("speaking: failed to open DAC");
        ESP_LOGE(TAG, "Speaking: failed to open DAC @%u Hz (err=%d)",
                 (unsigned)playback_rate, r);
        gl_close_dac();
        return false;
    }
    return true;
}

static void gl_resume_listening(const char *reason)
{
    if (s_gl.state == GL_STATE_LISTENING && s_gl.tx_task) {
        gl_mark_resume_reason(reason ? reason : "already listening");
        s_gl.resume_count++;
        return;
    }

    gl_mark_resume_reason(reason ? reason : "turn complete");
    s_gl.resume_count++;

    gl_close_dac();
    gl_set_state(GL_STATE_LISTENING, "Listening");

    int adc_r = gl_open_adc(GL_TX_SAMPLE_RATE);
    if (adc_r != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Listening: ADC reopen failed after %s (adc=%d)",
                 reason ? reason : "turn", adc_r);
        return;
    }

    gl_begin_audio_activity(reason ? reason : "resume");
    gl_start_tx_task();
    ESP_LOGI(TAG, "Listening: resumed capture (%s)", reason ? reason : "turn complete");
}

/* ---- WS event handler ---------------------------------------------------- */

static void gl_ws_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "WS connected");
        s_gl.ws_connected = true;
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "WS disconnected");
        s_gl.ws_connected = false;
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        /* Accept TEXT and BINARY — Gemini Live may use either */
        if (ev->op_code != WS_TRANSPORT_OPCODES_TEXT &&
            ev->op_code != WS_TRANSPORT_OPCODES_BINARY) {
            return;
        }
        if (!s_gl.rx_buf) {
            return;
        }
        /* Reassemble fragmented frames — drop and warn if frame > buffer */
        if ((size_t)ev->payload_len >= GL_RX_BUF_SIZE) {
            if (ev->payload_offset == 0) {
                ESP_LOGW(TAG, "Frame too large (%d bytes), dropping", ev->payload_len);
            }
            return;
        }
        if (ev->payload_offset + ev->data_len <= GL_RX_BUF_SIZE - 1) {
            memcpy(s_gl.rx_buf + ev->payload_offset, ev->data_ptr, ev->data_len);
        }
        if (ev->payload_offset + ev->data_len >= (size_t)ev->payload_len) {
            s_gl.rx_buf[ev->payload_len] = '\0';
            if (strstr(s_gl.rx_buf, "\"setupComplete\"") ||
                strstr(s_gl.rx_buf, "\"error\"") ||
                strstr(s_gl.rx_buf, "\"goAway\"")) {
                ESP_LOGI(TAG, "WS RX (%d B): %.200s", ev->payload_len, s_gl.rx_buf);
            } else {
                ESP_LOGD(TAG, "WS RX (%d B): %.200s", ev->payload_len, s_gl.rx_buf);
            }
            /* Copy the completed frame to a right-sized PSRAM block and queue it,
             * so a slow (blocking-playback) consumer cannot lose frames to rx_buf
             * being overwritten by the next arrival. Drop (don't block the WS
             * task) if the queue is full — an audio gap is recoverable, a stalled
             * WS task is not. */
            if (s_gl.rx_queue) {
                size_t flen = (size_t)ev->payload_len + 1;
                char *frame = heap_caps_malloc(flen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (frame) {
                    memcpy(frame, s_gl.rx_buf, flen);
                    if (xQueueSend(s_gl.rx_queue, &frame, 0) != pdTRUE) {
                        heap_caps_free(frame);
                        if ((s_gl.rx_drops++ % 8) == 0) {
                            ESP_LOGW(TAG, "rx queue full, dropped frame (total %u)",
                                     (unsigned)s_gl.rx_drops);
                        }
                    }
                }
            }
        }
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGW(TAG, "WS error");
    } else if (event_id == WEBSOCKET_EVENT_CLOSED) {
        ESP_LOGW(TAG, "WS closed by server");
        s_gl.ws_connected = false;
    }
}

/* ---- JarvisMCP tool bridge ------------------------------------------------ */

/* POST {"code":<js>} to the JarvisMCP /act gateway with the bearer token; copy
 * the response body into `out`. Returns the HTTP status (200 ok) or -1 on a
 * transport error. Blocking — only called from a toolCall (model is paused). */
static int gl_act_call(const char *code, char *out, size_t out_sz)
{
    if (!s_gl.mcp_key[0] || !s_gl.mcp_url[0]) {
        ESP_LOGW(TAG, "JarvisMCP not configured (set jarvis_mcp_url + jarvis_mcp_key via /api/config)");
        return -1;
    }
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "code", code);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    if (!body) {
        return -1;
    }

    esp_http_client_config_t cfg = {
        .url               = s_gl.mcp_url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(body);
        return -1;
    }
    char auth[sizeof(s_gl.mcp_key) + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", s_gl.mcp_key);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "Authorization", auth);

    int    status  = -1;
    size_t blen    = strlen(body);
    int64_t t0     = esp_timer_get_time();
    esp_err_t err  = esp_http_client_open(cli, blen);
    if (err == ESP_OK) {
        if (esp_http_client_write(cli, body, blen) >= 0) {
            esp_http_client_fetch_headers(cli);
            status = esp_http_client_get_status_code(cli);
            int rd = esp_http_client_read_response(cli, out, (int)out_sz - 1);
            out[(rd > 0) ? rd : 0] = '\0';
        }
    } else {
        ESP_LOGW(TAG, "act open failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "act call: status=%d %lldms code=%.80s", status,
             (esp_timer_get_time() - t0) / 1000, code);
    esp_http_client_cleanup(cli);
    free(body);
    return status;
}

/* Handle a Gemini toolCall frame: run each declared function via JarvisMCP and
 * send a toolResponse back. Returns true if the frame was a toolCall. */
static bool gl_handle_tool_call(cJSON *root)
{
    cJSON *toolCall = cJSON_GetObjectItemCaseSensitive(root, "toolCall");
    if (!toolCall) {
        return false;
    }
    cJSON *fcs = cJSON_GetObjectItemCaseSensitive(toolCall, "functionCalls");
    cJSON *resp_root = cJSON_CreateObject();
    cJSON *tr  = cJSON_AddObjectToObject(resp_root, "toolResponse");
    cJSON *frs = cJSON_AddArrayToObject(tr, "functionResponses");

    cJSON *fc;
    cJSON_ArrayForEach(fc, fcs) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(fc, "name"));
        cJSON *idj  = cJSON_GetObjectItem(fc, "id");
        cJSON *args = cJSON_GetObjectItem(fc, "args");

        cJSON *fr = cJSON_CreateObject();
        if (idj && cJSON_IsString(idj)) {
            cJSON_AddStringToObject(fr, "id", idj->valuestring);
        }
        cJSON_AddStringToObject(fr, "name", name ? name : "");
        cJSON *response = cJSON_AddObjectToObject(fr, "response");

        /* Build the JS for this function. Args are sanitised into the template. */
        char code[192] = {0};
        if (name && strcmp(name, "crypto_price") == 0) {
            const char *symbol = args ? cJSON_GetStringValue(cJSON_GetObjectItem(args, "symbol")) : NULL;
            char sym[40];
            int j = 0;
            for (int i = 0; symbol && symbol[i] && j < (int)sizeof(sym) - 1; i++) {
                char c = symbol[i];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == ' ') {
                    sym[j++] = c;
                }
            }
            sym[j] = '\0';
            snprintf(code, sizeof(code), "return await jarvis.crypto('%s')", sym[0] ? sym : "bitcoin");
        }

        if (code[0]) {
            char *rbuf = malloc(GL_ACT_RESP_MAX);
            if (rbuf) {
                int st = gl_act_call(code, rbuf, GL_ACT_RESP_MAX);
                cJSON *actj = (st == 200) ? cJSON_Parse(rbuf) : NULL;
                cJSON *result = actj ? cJSON_GetObjectItem(actj, "result") : NULL;
                if (result) {
                    cJSON_AddItemToObject(response, "result", cJSON_Duplicate(result, 1));
                } else {
                    cJSON_AddStringToObject(response, "error",
                                            st == 200 ? "no result" : "jarvis call failed");
                }
                if (actj) {
                    cJSON_Delete(actj);
                }
                free(rbuf);
            } else {
                cJSON_AddStringToObject(response, "error", "out of memory");
            }
        } else {
            cJSON_AddStringToObject(response, "error", "unknown function");
        }
        cJSON_AddItemToArray(frs, fr);
    }

    char *out = cJSON_PrintUnformatted(resp_root);
    cJSON_Delete(resp_root);
    if (out) {
        gl_ws_send_text(out);
        free(out);
    }
    return true;
}

static void gl_maybe_resume_speaking_watchdog(void)
{
    if (s_gl.state == GL_STATE_THINKING && !s_gl.last_audio_us && s_gl.last_input_end_us) {
        int64_t now = esp_timer_get_time();
        if ((uint64_t)(now - s_gl.last_input_end_us) >= GL_SPEAK_WATCHDOG_MS * 1000ULL) {
            s_gl.watchdog_resume_count++;
            ESP_LOGW(TAG, "watchdog: no response for %lu ms after activityEnd",
                     (unsigned long)GL_SPEAK_WATCHDOG_MS);
            s_gl.last_input_end_us = 0;
            gl_resume_listening("input watchdog");
        }
        return;
    }

    if (!s_gl.waiting_terminal) {
        return;
    }
    if (!(s_gl.state == GL_STATE_SPEAKING || s_gl.state == GL_STATE_THINKING)) {
        s_gl.waiting_terminal = false;
        return;
    }
    if (!s_gl.last_audio_us) {
        return;
    }
    if (s_gl.rx_queue && uxQueueMessagesWaiting(s_gl.rx_queue) > 0) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if ((uint64_t)(now - s_gl.last_audio_us) < GL_SPEAK_WATCHDOG_MS * 1000ULL) {
        return;
    }

    s_gl.watchdog_resume_count++;
    ESP_LOGW(TAG, "watchdog: no terminal signal for %lu ms while %s", (unsigned long)GL_SPEAK_WATCHDOG_MS,
             gl_state_name(s_gl.state));
    gl_resume_listening("speaking watchdog");
}

/* ---- Frame dispatch ------------------------------------------------------- */

static void gl_dispatch_frame(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "Parse failed: %.80s", json);
        return;
    }

    s_gl.rx_frames++;
    s_gl.last_frame_us = esp_timer_get_time();

    /* setupComplete */
    cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "setupComplete");
    if (sc) {
        ESP_LOGI(TAG, "setupComplete received");
        gl_set_state(GL_STATE_READY, "Setup complete");
        xEventGroupSetBits(s_gl.ev, GL_BIT_SETUP_OK);
        cJSON_Delete(root);
        return;
    }

    /* serverContent */
    cJSON *svc = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (svc) {
        /* interrupted */
        cJSON *intr = cJSON_GetObjectItemCaseSensitive(svc, "interrupted");
        if (cJSON_IsTrue(intr)) {
            ESP_LOGI(TAG, "Model interrupted");
            s_gl.interrupted_hits++;
            gl_resume_listening("interrupted");
        }

        /* modelTurn parts */
        cJSON *turn = cJSON_GetObjectItemCaseSensitive(svc, "modelTurn");
        if (turn) {
            cJSON *parts = cJSON_GetObjectItemCaseSensitive(turn, "parts");
            if (cJSON_IsArray(parts)) {
                cJSON *part;
                cJSON_ArrayForEach(part, parts) {
                    /* Text part — print to console (Phase 2) */
                    cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
                    if (cJSON_IsString(text)) {
                        s_gl.text_part_hits++;
                        printf("[Gemini] %s\n", text->valuestring);
                    }

                    /* Audio part (Phase 3) */
                    cJSON *id = gl_get_object_compat(part, "inlineData", "inline_data");
                    {
                        /* Debug: log what fields this part contains */
                        char part_keys[128] = {0};
                        int pk = 0;
                        cJSON *c = part ? part->child : NULL;
                        while (c && pk < (int)sizeof(part_keys) - 2) {
                            int n = snprintf(part_keys + pk, sizeof(part_keys) - pk - 1, "%s ", c->string ? c->string : "?");
                            pk += (n > 0 ? n : 0);
                            c = c->next;
                        }
                        ESP_LOGD(TAG, "part keys=[%s] id=%s", part_keys, id ? "found" : "null");
                    }
                    if (gl_play_model_audio_from_json(id, GL_RX_SAMPLE_RATE)) {
                        /* audio played (inlineData / inline_data) */
                    }

                    cJSON *chunks = cJSON_GetObjectItemCaseSensitive(part, "mediaChunks");
                    if (cJSON_IsArray(chunks)) {
                        cJSON *chunk_item;
                        cJSON_ArrayForEach(chunk_item, chunks) {
                            if (gl_play_model_audio_from_json(chunk_item, GL_RX_SAMPLE_RATE)) {
                                /* audio played (mediaChunks array item) */
                            }
                        }
                    } else if (gl_play_model_audio_from_json(chunks, GL_RX_SAMPLE_RATE)) {
                        /* audio played (mediaChunks object) */
                    }
                }
            }
        }

        /* turnComplete */
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(svc, "turnComplete");
        const bool tc_true = cJSON_IsTrue(tc);
        if (tc_true) {
            s_gl.turn_complete_hits++;
        }

        /* generationComplete */
        cJSON *gc = cJSON_GetObjectItemCaseSensitive(svc, "generationComplete");
        const bool gc_true = cJSON_IsTrue(gc);
        if (gc_true) {
            s_gl.generation_complete_hits++;
        }
        if (tc_true || gc_true) {
            gl_resume_listening(tc_true ? "turn complete" : "generation complete");
        }
        cJSON_Delete(root);
        return;
    }

    /* sessionResumptionUpdate — protocol heartbeat for resumable sessions. Harmless,
     * but log occasionally so we can confirm the frame shape without warning spam. */
    cJSON *sru = cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate");
    if (sru) {
        static uint32_t sru_hits;
        cJSON *new_handle = cJSON_GetObjectItemCaseSensitive(sru, "newHandle");
        cJSON *resumable  = cJSON_GetObjectItemCaseSensitive(sru, "resumable");
        sru_hits++;
        if ((sru_hits % 32) == 1) {
            ESP_LOGI(TAG, "sessionResumptionUpdate: handle=%s resumable=%s",
                     cJSON_IsString(new_handle) ? new_handle->valuestring : "n/a",
                     cJSON_IsTrue(resumable) ? "true" : "false");
        }
        cJSON_Delete(root);
        return;
    }

    /* Function calling — run the tool via JarvisMCP and reply with toolResponse. */
    if (gl_handle_tool_call(root)) {
        s_gl.tool_call_hits++;
        cJSON_Delete(root);
        return;
    }

    cJSON *ga = cJSON_GetObjectItemCaseSensitive(root, "goAway");
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (ga || err) {
        cJSON *code = ga ? cJSON_GetObjectItemCaseSensitive(ga, "code")
                         : cJSON_GetObjectItemCaseSensitive(err, "code");
        cJSON *msg = ga ? cJSON_GetObjectItemCaseSensitive(ga, "message")
                        : cJSON_GetObjectItemCaseSensitive(err, "message");
        ESP_LOGW(TAG, "%s: code=%s msg=%s",
                 ga ? "goAway" : "error",
                 cJSON_IsString(code) ? code->valuestring : "n/a",
                 cJSON_IsString(msg) ? msg->valuestring : "n/a");
        gl_resume_listening("server signal");
        cJSON_Delete(root);
        return;
    }

    /* Unhandled top-level frame — surface it instead of silently dropping.
     * Upstream protocol drift happens. Visible logs win over silent ignorance. */
    ESP_LOGW(TAG, "Unhandled server frame: %.200s", json);
    s_gl.unhandled_hits++;
    cJSON_Delete(root);
}

/* Pop one queued server frame (waiting up to `wait`), dispatch it, free it.
 * Returns true if a frame was processed. Used by both the setup-wait and the
 * main receive loops so neither path can lose frames. */
static bool gl_process_rx_queue(TickType_t wait)
{
    char *frame = NULL;
    if (!s_gl.rx_queue || xQueueReceive(s_gl.rx_queue, &frame, wait) != pdTRUE) {
        return false;
    }
    if (!frame) {
        return false;
    }
    UBaseType_t depth = uxQueueMessagesWaiting(s_gl.rx_queue);
    if (depth > 3) {
        ESP_LOGD(TAG, "rx queue depth=%u", (unsigned)depth);
    }
    gl_dispatch_frame(frame);
    heap_caps_free(frame);
    return true;
}

/* Drop and free any frames left in the queue (session teardown). */
static void gl_drain_rx_queue(void)
{
    char *frame = NULL;
    if (!s_gl.rx_queue) {
        return;
    }
    while (xQueueReceive(s_gl.rx_queue, &frame, 0) == pdTRUE) {
        heap_caps_free(frame);
    }
}

/* ---- Session task --------------------------------------------------------- */

static void gl_ws_cleanup_task(void *arg)
{
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)arg;
    if (client) {
        ESP_LOGI(TAG, "WS cleanup: stop/destroy begin");
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        ESP_LOGI(TAG, "WS cleanup: stop/destroy done");
    }
    vTaskDelete(NULL);
}

static void gl_session_task(void *arg)
{
    (void)arg;

    while (!s_gl.stop_requested) {
        /* Wait for session_active */
        EventBits_t bits = xEventGroupWaitBits(s_gl.ev, GL_BIT_SESSION_ON | GL_BIT_STOP,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(200));
        if (bits & GL_BIT_STOP) {
            break;
        }
        if (!(bits & GL_BIT_SESSION_ON) || !s_gl.session_active) {
            continue;
        }
        if (!s_gl.api_key[0]) {
            ESP_LOGE(TAG, "No Gemini API key — set via dashboard (gemini_key)");
            xEventGroupClearBits(s_gl.ev, GL_BIT_SESSION_ON);
            s_gl.session_active = false;
            gl_set_state(GL_STATE_IDLE, "");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        char ws_path[512];
        snprintf(ws_path, sizeof(ws_path), "%s?key=%s", GEMINI_WS_PATH, s_gl.api_key);

        ESP_LOGI(TAG, "Connecting to Gemini Live...");
        gl_set_state(GL_STATE_CONNECTING, "Connecting");

        esp_websocket_client_config_t ws_cfg = {
            .host                   = GEMINI_WS_HOST,
            .path                   = ws_path,
            .port                   = 443,
            .transport              = WEBSOCKET_TRANSPORT_OVER_SSL,
            .buffer_size            = 4096,
            .task_stack             = 8192,
            .task_prio              = 5,
            .network_timeout_ms     = 10000,
            .reconnect_timeout_ms   = 5000,
            .disable_auto_reconnect = true,
            .crt_bundle_attach      = esp_crt_bundle_attach,
        };
        ESP_LOGI(TAG, "WS init host=%s path=%s", GEMINI_WS_HOST, GEMINI_WS_PATH);
        s_gl.ws_client = esp_websocket_client_init(&ws_cfg);
        if (!s_gl.ws_client) {
            ESP_LOGE(TAG, "WS init failed");
            goto session_cleanup;
        }
        esp_websocket_register_events(s_gl.ws_client, WEBSOCKET_EVENT_ANY,
                                      gl_ws_event_handler, NULL);
        ESP_LOGI(TAG, "WS heap before start: internal=%u largest_internal=%u dma=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        esp_err_t ws_start = esp_websocket_client_start(s_gl.ws_client);
        ESP_LOGI(TAG, "WS start returned %s", esp_err_to_name(ws_start));
        if (ws_start != ESP_OK) {
            goto session_cleanup;
        }

        /* Wait for connection */
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
        while (!s_gl.ws_connected && !s_gl.stop_requested && s_gl.session_active) {
            if (xTaskGetTickCount() > deadline) {
                ESP_LOGE(TAG, "WS connect timeout");
                goto session_cleanup;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_gl.stop_requested || !s_gl.session_active) {
            goto session_cleanup;
        }

        /* Send setup */
        if (!gl_send_setup()) {
            ESP_LOGE(TAG, "Failed to send setup");
            goto session_cleanup;
        }

        /* Wait for setupComplete — drain rx_buf ourselves (gl-setup-poll).
         * The WS handler only xTaskNotifyGive()s us; it does NOT set the event
         * group, and the main receive loop that dispatches frames hasn't started
         * yet. A pure xEventGroupWaitBits here would never see setupComplete (it
         * arrives in rx_buf, undispatched) and time out at 10s. So we poll +
         * dispatch here, mirroring cap_gemini_live_test(). */
        xEventGroupClearBits(s_gl.ev, GL_BIT_SETUP_OK);
        deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
        while (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_SETUP_OK) &&
               !s_gl.stop_requested && s_gl.session_active && s_gl.ws_connected) {
            if (xTaskGetTickCount() > deadline) {
                break;
            }
            /* Drain queued frames until setupComplete lands (sets GL_BIT_SETUP_OK). */
            gl_process_rx_queue(pdMS_TO_TICKS(100));
        }
        if (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_SETUP_OK)) {
            ESP_LOGE(TAG, "setupComplete timeout (see WS RX logs for server reply)");
            goto session_cleanup;
        }

        /* Phase 1 verified: WSS + setup handshake works. */
        ESP_LOGI(TAG, "Gemini Live session ready");
        gl_set_state(GL_STATE_LISTENING, "Listening");

        /* Phase 4: session task owns codec lifetime; TX task only captures. */
        if (gl_open_adc(GL_TX_SAMPLE_RATE) != ESP_CODEC_DEV_OK) {
            gl_set_audio_error("session: initial ADC open failed");
            ESP_LOGE(TAG, "Listening: ADC open failed, ending session");
            goto session_cleanup;
        }
        gl_begin_audio_activity("initial listen");
        gl_start_tx_task();

        /* Main receive loop */
        while (s_gl.ws_connected && !s_gl.stop_requested && s_gl.session_active) {
            /* Drain ALL queued frames before re-checking loop conditions, so a
             * burst of audio chunks plays back-to-back without gaps. */
        while (gl_process_rx_queue(pdMS_TO_TICKS(200))) {
            if (s_gl.stop_requested || !s_gl.session_active) {
                break;
            }
        }
        gl_maybe_resume_speaking_watchdog();
        gl_ensure_listening_capture();
        /* Check if WS dropped */
        if (!s_gl.ws_connected) {
            ESP_LOGW(TAG, "WS dropped, cleaning up session");
            break;
        }
        }

session_cleanup:
        /* Stop audio TX and close codecs from this task only. The TX worker
         * never closes codec handles, avoiding cross-task I2S mutex deadlocks. */
        ESP_LOGI(TAG, "Teardown: stopping tx task");
        gl_stop_tx_task();
        ESP_LOGI(TAG, "Teardown: tx task stopped");
        ESP_LOGI(TAG, "Teardown: closing adc");
        gl_close_adc();
        ESP_LOGI(TAG, "Teardown: closing dac");
        gl_close_dac();

        ESP_LOGI(TAG, "Teardown: closing websocket");
        ESP_LOGI(TAG, "Session cleanup");
        gl_drain_rx_queue();
        esp_websocket_client_handle_t ws_client = s_gl.ws_client;
        s_gl.ws_client = NULL;
        s_gl.ws_connected = false;
        s_gl.session_active = false;
        s_gl.activity_open = false;
        s_gl.state = GL_STATE_IDLE;
        atomic_store(&s_out_rms, 0);
        emote_set_voice_idle();
        xEventGroupClearBits(s_gl.ev, GL_BIT_SESSION_ON);
        if (ws_client) {
            if (xTaskCreate(gl_ws_cleanup_task, "gl_ws_cleanup", 4096, ws_client, 3, NULL) != pdPASS) {
                ESP_LOGW(TAG, "WS cleanup task create failed; destroying inline");
                esp_websocket_client_destroy(ws_client);
            }
        }
    }

    ESP_LOGI(TAG, "Session task exiting");
    s_gl.stop_requested = false;
    s_gl.session_active = false;
    s_gl.ws_connected = false;
    if (s_gl.session_task == xTaskGetCurrentTaskHandle()) {
        s_gl.session_task = NULL;
    }
    if (s_gl.ev) {
        xEventGroupSetBits(s_gl.ev, GL_BIT_SESSION_DONE);
    }
    claw_task_delete(NULL);
}

/* ---- Touch task (Phase 5) ------------------------------------------------- */

static void gl_touch_task(void *arg)
{
    (void)arg;
    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint16_t strength[1] = {0};
    uint8_t point_num = 0;
    bool was_touching = false;

    while (!s_gl.stop_requested) {
        if (!s_gl.touch) {
            vTaskDelay(pdMS_TO_TICKS(500));
            /* Retry acquiring touch handle once per second */
            gl_acquire_codec_handles();
            continue;
        }

        esp_lcd_touch_read_data(s_gl.touch);
        bool touching = (esp_lcd_touch_get_coordinates(s_gl.touch, x, y, strength,
                                                       &point_num, 1) && point_num > 0);

        if (touching && !was_touching) {
            /* Tap detected — toggle session */
            if (s_gl.session_active) {
                ESP_LOGI(TAG, "Tap: stopping session");
                s_gl.session_active = false;
                xEventGroupClearBits(s_gl.ev, GL_BIT_SESSION_ON);
            } else {
                ESP_LOGI(TAG, "Tap: starting session");
                s_gl.session_active = true;
                xEventGroupSetBits(s_gl.ev, GL_BIT_SESSION_ON);
            }
        }
        was_touching = touching;
        vTaskDelay(pdMS_TO_TICKS(GL_TOUCH_POLL_MS));
    }

    claw_task_delete(NULL);
}

/* ---- Lifecycle ------------------------------------------------------------ */

static esp_err_t gl_gateway_start(void)
{
    if (s_gl.session_task) {
        if (s_gl.stop_requested) {
            ESP_LOGW(TAG, "Gemini Live gateway stop still in progress");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }
    if (!s_gl.rx_buf) {
        s_gl.rx_buf = heap_caps_malloc(GL_RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_gl.rx_buf) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_gl.rx_buf[0] = '\0';
    if (!s_gl.rx_queue) {
        s_gl.rx_queue = xQueueCreate(GL_RX_QUEUE_DEPTH, sizeof(char *));
        if (!s_gl.rx_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_gl.stop_requested = false;
    gl_reset_diag_counters();
    s_gl.dac_codec_failed = false;
    s_gl.adc_codec_failed = false;

    if (!s_gl.ev) {
        s_gl.ev = xEventGroupCreate();
        if (!s_gl.ev) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_gl.ws_mutex) {
        s_gl.ws_mutex = xSemaphoreCreateMutex();
        if (!s_gl.ws_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Clear stop/done bits left over from any previous gateway run */
    xEventGroupClearBits(s_gl.ev,
                         GL_BIT_STOP | GL_BIT_TX_STOP | GL_BIT_TX_DONE | GL_BIT_SESSION_DONE);

    gl_acquire_codec_handles();

    static const claw_task_config_t sess_cfg = {
        .name         = "gl_session",
        .stack_size   = 12288,
        .priority     = 5,
        .core_id      = tskNO_AFFINITY,
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };
    if (claw_task_create(&sess_cfg, gl_session_task, NULL, &s_gl.session_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* Touch toggle is driven by touch_monitor_task in main.c (app_claw layer),
     * which calls cap_gemini_live_toggle() on rising-edge tap. Do not start a
     * second touch-polling task here — two tasks on the same I2C bus race the
     * controller and cause phantom double-taps + I2S codec churn. */

    ESP_LOGI(TAG, "Gemini Live gateway started");
    return ESP_OK;
}

static esp_err_t gl_gateway_stop(void)
{
    s_gl.stop_requested = true;
    s_gl.session_active = false;

    if (!s_gl.ev) {
        return ESP_OK;
    }

    TaskHandle_t session_task = s_gl.session_task;
    xEventGroupClearBits(s_gl.ev, GL_BIT_SESSION_ON);
    xEventGroupSetBits(s_gl.ev, GL_BIT_STOP | GL_BIT_TX_STOP);

    /* Stop may be called by HTTP. Do not wait here: the session task owns codec
     * and WebSocket teardown, and blocking the HTTP server makes the device look
     * dead while cleanup runs. */
    if (session_task == xTaskGetCurrentTaskHandle()) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Gateway stop requested");
    return ESP_OK;
}

/* ---- Public API ---------------------------------------------------------- */

esp_err_t cap_gemini_live_set_api_key(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_gl.api_key, api_key, sizeof(s_gl.api_key));
    return ESP_OK;
}

esp_err_t cap_gemini_live_set_mcp_key(const char *mcp_key)
{
    if (!mcp_key) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_gl.mcp_key, mcp_key, sizeof(s_gl.mcp_key));
    return ESP_OK;
}

esp_err_t cap_gemini_live_set_mcp_url(const char *mcp_url)
{
    if (!mcp_url) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_gl.mcp_url, mcp_url, sizeof(s_gl.mcp_url));
    return ESP_OK;
}

/* ---- Audio-level hooks for the reactive waveform (display layer) ----------
 * Normalised 0.0..1.0 (RMS of the int16 PCM block / 32768). The display layer's
 * amplitude-source adapter (patch 0032) reads these and scales to its 0..1000.
 * Lock-free (plain atomics) — safe to poll from the display task. */

float cap_gemini_live_get_mic_level(void)
{
    return (float)atomic_load(&s_mic_rms) / 32768.0f;
}

float cap_gemini_live_get_output_level(void)
{
    return (float)atomic_load(&s_out_rms) / 32768.0f;
}

void cap_gemini_live_print_diagnostics(void)
{
    EventBits_t bits = s_gl.ev ? xEventGroupGetBits(s_gl.ev) : 0;
    UBaseType_t rx_depth = s_gl.rx_queue ? uxQueueMessagesWaiting(s_gl.rx_queue) : 0;
    int64_t now_us = esp_timer_get_time();
    int64_t frame_age = s_gl.last_frame_us ? (now_us - s_gl.last_frame_us) / 1000 : -1;
    int64_t audio_age = s_gl.last_audio_us ? (now_us - s_gl.last_audio_us) / 1000 : -1;
    int64_t resume_age = s_gl.last_resume_us ? (now_us - s_gl.last_resume_us) / 1000 : -1;

    printf("Gemini Live diagnostics:\n");
    printf("  state=%s listening=%d connected=%d session_active=%d stop_requested=%d\n",
           gl_state_name(s_gl.state),
           (int)(s_gl.state == GL_STATE_LISTENING || s_gl.state == GL_STATE_READY),
           (int)s_gl.ws_connected,
           (int)s_gl.session_active,
           (int)s_gl.stop_requested);
    printf("  capture: tx_task=%d dac_open=%d adc_open=%d dac=%p raw_dac=%p adc=%p raw_adc=%p\n",
           (int)(s_gl.tx_task != NULL),
           (int)s_gl.dac_open,
           (int)s_gl.adc_open,
           (void *)s_gl.dac,
           (void *)s_gl.dac_chan,
           (void *)s_gl.adc,
           (void *)s_gl.adc_chan);
    printf("  raw cfg rates: dac=%u adc=%u\n",
           (unsigned)s_gl.dac_raw_rate_hz,
           (unsigned)s_gl.adc_raw_rate_hz);
    printf("  active dac rate=%u last_audio_mime_rate=%u rate_mismatch_chunks=%u\n",
           (unsigned)s_gl.dac_rate,
           (unsigned)s_gl.last_audio_mime_rate,
           (unsigned)s_gl.rate_mismatch_chunks);
    printf("  audio path state: dac_codec_failed=%d adc_codec_failed=%d\n",
           (int)s_gl.dac_codec_failed,
           (int)s_gl.adc_codec_failed);
    printf("  audio path: errors=%s\n", s_gl.last_audio_error[0] ? s_gl.last_audio_error : "-");
    printf("  bits=0x%08x queue_depth=%u drops=%u frames=%u text_parts=%u audio_parts=%u\n",
           (unsigned)bits, (unsigned)rx_depth,
           (unsigned)s_gl.rx_drops, (unsigned)s_gl.rx_frames,
           (unsigned)s_gl.text_part_hits, (unsigned)s_gl.audio_part_hits);
    printf("  turn_complete=%u generation_complete=%u interrupted=%u tool_calls=%u unhandled=%u\n",
           (unsigned)s_gl.turn_complete_hits,
           (unsigned)s_gl.generation_complete_hits,
           (unsigned)s_gl.interrupted_hits,
           (unsigned)s_gl.tool_call_hits,
           (unsigned)s_gl.unhandled_hits);
    printf("  resumes=%u watchdog_resumes=%u last_resume=%s (%lld ms ago) waiting_terminal=%d\n",
           (unsigned)s_gl.resume_count,
           (unsigned)s_gl.watchdog_resume_count,
           s_gl.last_resume_reason[0] ? s_gl.last_resume_reason : "-",
           (long long)resume_age,
           (int)s_gl.waiting_terminal);
    printf("  frame_age_ms=%lld audio_age_ms=%lld\n",
           (long long)frame_age, (long long)audio_age);
}

esp_err_t cap_gemini_live_get_diagnostics_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    EventBits_t bits = s_gl.ev ? xEventGroupGetBits(s_gl.ev) : 0;
    UBaseType_t rx_depth = s_gl.rx_queue ? uxQueueMessagesWaiting(s_gl.rx_queue) : 0;
    int64_t now_us = esp_timer_get_time();
    int64_t frame_age = s_gl.last_frame_us ? (now_us - s_gl.last_frame_us) / 1000 : -1;
    int64_t audio_age = s_gl.last_audio_us ? (now_us - s_gl.last_audio_us) / 1000 : -1;
    int64_t resume_age = s_gl.last_resume_us ? (now_us - s_gl.last_resume_us) / 1000 : -1;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "state", gl_state_name(s_gl.state));
    cJSON_AddBoolToObject(root, "listening", s_gl.state == GL_STATE_LISTENING || s_gl.state == GL_STATE_READY);
    cJSON_AddBoolToObject(root, "connected", s_gl.ws_connected);
    cJSON_AddBoolToObject(root, "session_active", s_gl.session_active);
    cJSON_AddBoolToObject(root, "stop_requested", s_gl.stop_requested);
    cJSON_AddBoolToObject(root, "tx_task", s_gl.tx_task != NULL);
    cJSON_AddBoolToObject(root, "activity_open", s_gl.activity_open);
    cJSON_AddBoolToObject(root, "dac_open", s_gl.dac_open);
    cJSON_AddBoolToObject(root, "adc_open", s_gl.adc_open);
    cJSON_AddBoolToObject(root, "dac_raw", s_gl.dac_raw);
    cJSON_AddBoolToObject(root, "adc_raw", s_gl.adc_raw);
    cJSON_AddNumberToObject(root, "dac_rate", (double)s_gl.dac_rate);
    cJSON_AddNumberToObject(root, "dac_raw_rate_hz", (double)s_gl.dac_raw_rate_hz);
    cJSON_AddNumberToObject(root, "adc_raw_rate_hz", (double)s_gl.adc_raw_rate_hz);
    cJSON_AddNumberToObject(root, "last_audio_mime_rate", (double)s_gl.last_audio_mime_rate);
    cJSON_AddNumberToObject(root, "rate_mismatch_chunks", (double)s_gl.rate_mismatch_chunks);
    cJSON_AddNumberToObject(root, "tx_frames_sent", (double)s_gl.tx_frames_sent);
    cJSON_AddNumberToObject(root, "tx_send_failures", (double)s_gl.tx_send_failures);
    cJSON_AddNumberToObject(root, "tx_read_failures", (double)s_gl.tx_read_failures);
    cJSON_AddNumberToObject(root, "tx_codec_reads", (double)s_gl.tx_codec_reads);
    cJSON_AddNumberToObject(root, "tx_raw_reads", (double)s_gl.tx_raw_reads);
    cJSON_AddBoolToObject(root, "dac_codec_failed", s_gl.dac_codec_failed);
    cJSON_AddBoolToObject(root, "adc_codec_failed", s_gl.adc_codec_failed);
    cJSON_AddStringToObject(root, "audio_error", s_gl.last_audio_error[0] ? s_gl.last_audio_error : "-");
    cJSON_AddNumberToObject(root, "bits", (double)bits);
    cJSON_AddNumberToObject(root, "queue_depth", (double)rx_depth);
    cJSON_AddNumberToObject(root, "drops", (double)s_gl.rx_drops);
    cJSON_AddNumberToObject(root, "frames", (double)s_gl.rx_frames);
    cJSON_AddNumberToObject(root, "text_parts", (double)s_gl.text_part_hits);
    cJSON_AddNumberToObject(root, "audio_parts", (double)s_gl.audio_part_hits);
    cJSON_AddNumberToObject(root, "turn_complete", (double)s_gl.turn_complete_hits);
    cJSON_AddNumberToObject(root, "generation_complete", (double)s_gl.generation_complete_hits);
    cJSON_AddNumberToObject(root, "interrupted", (double)s_gl.interrupted_hits);
    cJSON_AddNumberToObject(root, "tool_calls", (double)s_gl.tool_call_hits);
    cJSON_AddNumberToObject(root, "unhandled", (double)s_gl.unhandled_hits);
    cJSON_AddNumberToObject(root, "resumes", (double)s_gl.resume_count);
    cJSON_AddNumberToObject(root, "watchdog_resumes", (double)s_gl.watchdog_resume_count);
    cJSON_AddNumberToObject(root, "frame_age_ms", (double)frame_age);
    cJSON_AddNumberToObject(root, "audio_age_ms", (double)audio_age);
    cJSON_AddNumberToObject(root, "last_resume_ms", (double)resume_age);
    cJSON_AddStringToObject(root, "last_resume_reason",
                            s_gl.last_resume_reason[0] ? s_gl.last_resume_reason : "-");
    cJSON_AddNumberToObject(root, "mic_level", (double)cap_gemini_live_get_mic_level());
    cJSON_AddNumberToObject(root, "output_level", (double)cap_gemini_live_get_output_level());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    if (strlen(json) + 1 > out_size) {
        free(json);
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(out, json, out_size);
    free(json);
    return ESP_OK;
}

/* Optional: drive the levels with no live session (own audio-path testing). */
void cap_gemini_live_set_synthetic_levels(float mic, float out)
{
    if (mic < 0.0f) mic = 0.0f; else if (mic > 1.0f) mic = 1.0f;
    if (out < 0.0f) out = 0.0f; else if (out > 1.0f) out = 1.0f;
    atomic_store(&s_mic_rms, (uint16_t)(mic * 32767.0f));
    atomic_store(&s_out_rms, (uint16_t)(out * 32767.0f));
}

esp_err_t cap_gemini_live_start(void)
{
    esp_err_t err = gl_gateway_start();
    if (err != ESP_OK) {
        return err;
    }
    /* Activate the first session immediately */
    s_gl.session_active = true;
    xEventGroupSetBits(s_gl.ev, GL_BIT_SESSION_ON);
    return ESP_OK;
}

esp_err_t cap_gemini_live_stop(void)
{
    return gl_gateway_stop();
}

esp_err_t cap_gemini_live_send_text(const char *text)
{
    if (!text || !s_gl.session_active ||
        (s_gl.state != GL_STATE_READY && s_gl.state != GL_STATE_LISTENING && s_gl.state != GL_STATE_SPEAKING)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_gl.state == GL_STATE_LISTENING) {
        gl_stop_tx_task();
        gl_close_adc();
        gl_end_audio_activity("text turn");
        atomic_store(&s_mic_rms, 0);
        gl_set_state(GL_STATE_THINKING, "Thinking");
    }
    return gl_send_text_turn(text) ? ESP_OK : ESP_FAIL;
}

esp_err_t cap_gemini_live_end_input(void)
{
    if (!s_gl.session_active ||
        (s_gl.state != GL_STATE_READY && s_gl.state != GL_STATE_LISTENING)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_gl.state == GL_STATE_LISTENING) {
        gl_stop_tx_task();
        gl_close_adc();
        atomic_store(&s_mic_rms, 0);
        gl_set_state(GL_STATE_THINKING, "Thinking");
    }
    s_gl.last_input_end_us = esp_timer_get_time();
    s_gl.waiting_terminal = true;
    return gl_end_audio_activity("end_input") ? ESP_OK : ESP_FAIL;
}

bool cap_gemini_live_is_active(void)
{
    return s_gl.session_active && s_gl.state != GL_STATE_IDLE;
}

void cap_gemini_live_toggle(void)
{
    /* Debounce rapid taps so an in-flight connect is not torn down mid-handshake. */
    static int64_t last_toggle_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_toggle_us < (int64_t)GL_TOGGLE_COOLDOWN_MS * 1000) {
        ESP_LOGW(TAG, "Tap ignored (debounce %d ms)", GL_TOGGLE_COOLDOWN_MS);
        return;
    }
    last_toggle_us = now_us;

    /* Don't tear down a session that's mid-handshake — ignore the tap and let it
     * reach READY (or time out). Prevents half-connected sessions and audio-path
     * churn when the user taps impatiently during connect. */
    if (s_gl.session_active && s_gl.state == GL_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Tap ignored (session still connecting)");
        return;
    }

    if (!s_gl.session_task) {
        cap_gemini_live_start();
        return;
    }
    if (s_gl.session_active) {
        if (s_gl.state == GL_STATE_READY || s_gl.state == GL_STATE_LISTENING) {
            ESP_LOGI(TAG, "Tap: ending input stream");
            cap_gemini_live_end_input();
            return;
        }
        ESP_LOGI(TAG, "Tap: stopping session");
        cap_gemini_live_stop();
    } else {
        ESP_LOGI(TAG, "Tap: starting session");
        s_gl.session_active = true;
        xEventGroupSetBits(s_gl.ev, GL_BIT_SESSION_ON);
    }
}

/* Phase 1 test: connect → setup → wait setupComplete → disconnect */
esp_err_t cap_gemini_live_test(void)
{
    if (!s_gl.api_key[0]) {
        ESP_LOGE(TAG, "No API key — set via dashboard (gemini_key NVS field)");
        return ESP_ERR_INVALID_STATE;
    }

    /* Ensure event group exists */
    if (!s_gl.ev) {
        s_gl.ev = xEventGroupCreate();
        if (!s_gl.ev) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_gl.ws_mutex) {
        s_gl.ws_mutex = xSemaphoreCreateMutex();
        if (!s_gl.ws_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_gl.rx_buf) {
        s_gl.rx_buf = heap_caps_malloc(GL_RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_gl.rx_buf) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_gl.rx_buf[0] = '\0';

    char ws_path[512];
    snprintf(ws_path, sizeof(ws_path), "%s?key=%s", GEMINI_WS_PATH, s_gl.api_key);

    printf("[gemini-live test] Connecting to %s%s\n", GEMINI_WS_HOST, GEMINI_WS_PATH);

    esp_websocket_client_config_t ws_cfg = {
        .host                   = GEMINI_WS_HOST,
        .path                   = ws_path,
        .port                   = 443,
        .transport              = WEBSOCKET_TRANSPORT_OVER_SSL,
        .buffer_size            = 4096,
        .task_stack             = 8192,
        .task_prio              = 5,
        .network_timeout_ms     = 10000,
        .reconnect_timeout_ms   = 5000,
        .disable_auto_reconnect = true,
        .crt_bundle_attach      = esp_crt_bundle_attach,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        printf("[gemini-live test] FAIL: WS init failed\n");
        return ESP_FAIL;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, gl_ws_event_handler, NULL);
    esp_err_t ws_start = esp_websocket_client_start(client);
    if (ws_start != ESP_OK) {
        printf("[gemini-live test] FAIL: WS start failed: %s\n", esp_err_to_name(ws_start));
        esp_websocket_client_destroy(client);
        return ESP_FAIL;
    }

    /* Wait for connect */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    while (!s_gl.ws_connected && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_gl.ws_connected) {
        printf("[gemini-live test] FAIL: WS connect timeout\n");
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        s_gl.ws_client = NULL;
        return ESP_FAIL;
    }

    s_gl.ws_client = client;
    printf("[gemini-live test] Connected. Sending setup (model=%s)\n", GEMINI_LIVE_MODEL);

    xEventGroupClearBits(s_gl.ev, GL_BIT_SETUP_OK);
    if (!gl_send_setup()) {
        printf("[gemini-live test] FAIL: send_setup failed\n");
        goto test_cleanup;
    }

    /* Wait for setupComplete — dispatched via task notification then our event bit */
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    while (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_SETUP_OK) && xTaskGetTickCount() < deadline) {
        /* Poll rx_buf for incoming frames (no session task running) */
        if (s_gl.rx_buf && s_gl.rx_buf[0]) {
            gl_dispatch_frame(s_gl.rx_buf);
            s_gl.rx_buf[0] = '\0';
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_SETUP_OK)) {
        printf("[gemini-live test] FAIL: setupComplete timeout\n");
        goto test_cleanup;
    }

    printf("[gemini-live test] PASS: setupComplete received — WSS+TLS+auth OK\n");
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    s_gl.ws_client   = NULL;
    s_gl.ws_connected = false;
    s_gl.state       = GL_STATE_IDLE;
    return ESP_OK;

test_cleanup:
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    s_gl.ws_client   = NULL;
    s_gl.ws_connected = false;
    s_gl.state       = GL_STATE_IDLE;
    return ESP_FAIL;
}

/* ---- Capability descriptor ----------------------------------------------- */

static esp_err_t gl_cap_init(void)
{
    s_gl.ev       = xEventGroupCreate();
    s_gl.ws_mutex = xSemaphoreCreateMutex();
    if (!s_gl.ev || !s_gl.ws_mutex) {
        return ESP_ERR_NO_MEM;
    }
    cmd_cap_gemini_live_register();
    return ESP_OK;
}

static esp_err_t gl_cap_start(void)
{
    /* Do not auto-start tasks at boot — user must call cap_gemini_live_start()
     * or `gemini-live --start`. Touch polling would race the emote system. */
    return ESP_OK;
}

static esp_err_t gl_cap_stop(void)
{
    return gl_gateway_stop();
}

static esp_err_t gl_cap_execute(const char *input_json,
                                const claw_cap_call_context_t *ctx,
                                char *output, size_t output_size)
{
    (void)ctx;
    /* The CLI (cmd_cap_gemini_live.c) handles commands directly.
     * This execute path is a fallback for LLM-initiated calls (not exposed). */
    snprintf(output, output_size, "{\"ok\":false,\"error\":\"use gemini-live CLI\"}");
    return ESP_OK;
}

static const claw_cap_descriptor_t s_gl_descriptors[] = {
    {
        .id           = "gemini_live",
        .name         = "Gemini Live",
        .family       = "voice",
        .description  = "Toggle-on/off voice conversation via Gemini Live API",
        .kind         = CLAW_CAP_KIND_HYBRID,
        .cap_flags    = CLAW_CAP_FLAG_SUPPORTS_LIFECYCLE,
        .input_schema_json = NULL,
        .init         = gl_cap_init,
        .start        = gl_cap_start,
        .stop         = gl_cap_stop,
        .execute      = gl_cap_execute,
    },
};

static const claw_cap_group_t s_gl_group = {
    .group_id         = "cap_gemini_live",
    .plugin_name      = "cap_gemini_live",
    .version          = "1.0.0",
    .descriptors      = s_gl_descriptors,
    .descriptor_count = sizeof(s_gl_descriptors) / sizeof(s_gl_descriptors[0]),
};

esp_err_t cap_gemini_live_register_group(void)
{
    if (claw_cap_group_exists(s_gl_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_gl_group);
}
