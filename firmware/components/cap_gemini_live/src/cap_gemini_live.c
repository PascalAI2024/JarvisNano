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
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
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

/* ---- Configuration -------------------------------------------------------- */

/* Verified 2026-05-21 against ai.google.dev/gemini-api/docs/models/gemini-3.1-flash-live-preview
 * (docs updated 2026-05-13). The 2.5-era native-audio preview is superseded by 3.1 Flash Live;
 * the migration guide also moves thinkingBudget -> thinkingLevel (see gl_send_setup). */
#define GEMINI_LIVE_MODEL        "models/gemini-3.1-flash-live-preview"
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
#define GL_RX_QUEUE_DEPTH        128           /* completed-frame queue depth: Gemini bursts a full reply faster than
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
    EventGroupHandle_t           ev;
    TaskHandle_t                 session_task;
    TaskHandle_t                 touch_task;
    TaskHandle_t                 tx_task;
    SemaphoreHandle_t            ws_mutex;          /* serialises WS sends */
    esp_codec_dev_handle_t       dac;
    esp_codec_dev_handle_t       adc;
    esp_lcd_touch_handle_t       touch;
    /* I2S codec open-state tracking. esp_codec_dev_open/close are NOT idempotent:
     * a redundant close drives i2s_channel_disable on an already-disabled channel
     * ("channel has not been enabled yet") and wedges the audio path under rapid
     * start/stop. Track open state + the rate so open is rate-aware and close is
     * a no-op when already closed. */
    bool                         dac_open;
    uint32_t                     dac_rate;
    bool                         adc_open;
    uint32_t                     adc_rate;
} gl_ctx_t;

static gl_ctx_t s_gl;

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
    s_gl.state = st;
    emote_set_status_detail(detail ? detail : "");

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
        emote_set_speaking();
        break;
    case GL_STATE_IDLE:
        emote_set_voice_idle();
        break;
    case GL_STATE_READY:
    default:
        break;
    }
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

/* ---- Board device handles ------------------------------------------------- */

static void gl_acquire_codec_handles(void)
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

    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC,
                                            &dac_h) == ESP_OK && dac_h) {
        s_gl.dac = ((dev_audio_codec_handles_t *)dac_h)->codec_dev;
    }
    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_ADC,
                                            &adc_h) == ESP_OK && adc_h) {
        s_gl.adc = ((dev_audio_codec_handles_t *)adc_h)->codec_dev;
    }
    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH,
                                            &touch_h) == ESP_OK && touch_h) {
        s_gl.touch = ((dev_lcd_touch_i2c_handles_t *)touch_h)->touch_handle;
    }

    ESP_LOGI(TAG, "Codec handles: dac=%p adc=%p touch=%p",
             s_gl.dac, s_gl.adc, s_gl.touch);
}

/* ---- I2S open/close helpers ----------------------------------------------- */

static int gl_open_dac(uint32_t sample_rate)
{
    if (!s_gl.dac) {
        return 0;
    }
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
    if (r == ESP_CODEC_DEV_OK) {
        esp_codec_dev_set_out_vol(s_gl.dac, 100);
        s_gl.dac_open = true;
        s_gl.dac_rate = sample_rate;
    }
    return r;
}

static int gl_open_adc(uint32_t sample_rate)
{
    if (!s_gl.adc) {
        return 0;
    }
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
        esp_codec_dev_set_in_gain(s_gl.adc, 30.0f);
        s_gl.adc_open = true;
        s_gl.adc_rate = sample_rate;
    }
    return r;
}

static void gl_close_dac(void)
{
    if (s_gl.dac && s_gl.dac_open) {
        esp_codec_dev_close(s_gl.dac);
        s_gl.dac_open = false;
    }
}

static void gl_close_adc(void)
{
    if (s_gl.adc && s_gl.adc_open) {
        esp_codec_dev_close(s_gl.adc);
        s_gl.adc_open = false;
    }
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
    /* Gemini 3.1 Flash Live uses thinkingLevel (minimal|low|medium|high), NOT the
     * 2.5-era thinkingBudget integer. Setting both is a 400. "minimal" = lowest latency. */
    cJSON *tc = cJSON_AddObjectToObject(gc, "thinkingConfig");
    cJSON_AddStringToObject(tc, "thinkingLevel", "minimal");

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

/* ---- Audio playback ------------------------------------------------------- */

/* Decode base64 PCM, play via ES8311 @ 24kHz (SPEAKING state). */
static void gl_play_audio_b64(const char *b64_str)
{
    size_t b64_len = strlen(b64_str);
    size_t pcm_max = (b64_len / 4) * 3 + 4;
    uint8_t *pcm = heap_caps_malloc(pcm_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        ESP_LOGE(TAG, "OOM for PCM decode buf");
        return;
    }

    size_t pcm_len = 0;
    if (mbedtls_base64_decode(pcm, pcm_max, &pcm_len,
                              (const unsigned char *)b64_str, b64_len) != 0) {
        ESP_LOGE(TAG, "base64 decode failed");
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
        ESP_LOGI(TAG, "audio level: peak=%d rms=%d (full-scale=32767) gain=%d samples=%u",
                 (int)peak_in, (int)rms_in, GL_OUT_GAIN, (unsigned)nsamp);
    }

    /* Publish playback level for the SPEAKING waveform (before the blocking write). */
    atomic_store(&s_out_rms,
                 gl_compute_rms((const int16_t *)pcm, pcm_len / sizeof(int16_t)));

    if (esp_codec_dev_write(s_gl.dac, pcm, (int)pcm_len) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "DAC write failed");
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

    while (!(xEventGroupGetBits(s_gl.ev) & GL_BIT_TX_STOP)) {
        if (s_gl.state != GL_STATE_LISTENING) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (xEventGroupGetBits(s_gl.ev) & GL_BIT_TX_STOP) {
            break;
        }

        int r = esp_codec_dev_read(s_gl.adc, pcm, GL_TX_PCM_BYTES);
        if (r != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(GL_TX_CHUNK_MS));
            continue;
        }

        /* Publish mic level for the LISTENING waveform. */
        atomic_store(&s_mic_rms,
                     gl_compute_rms((const int16_t *)pcm, GL_TX_PCM_BYTES / sizeof(int16_t)));

        gl_send_audio_frame(pcm, GL_TX_PCM_BYTES);
    }

    /* Mic is quiet once capture stops. */
    atomic_store(&s_mic_rms, 0);

    ESP_LOGI(TAG, "Audio TX: stopped");
    xEventGroupSetBits(s_gl.ev, GL_BIT_TX_DONE);
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
    xEventGroupClearBits(s_gl.ev, GL_BIT_TX_STOP | GL_BIT_TX_DONE);
    static const claw_task_config_t tx_cfg = {
        .name         = "gl_audio_tx",
        .stack_size   = 8192,
        .priority     = 6,
        .core_id      = tskNO_AFFINITY,
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };
    claw_task_create(&tx_cfg, gl_audio_tx_task, NULL, &s_gl.tx_task);
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
            /* Raw-frame diagnostic: dump the first 200 bytes of every complete
             * server frame. Decisive for setup failures — shows whether Gemini
             * returned setupComplete, an error frame, or something unexpected. */
            ESP_LOGI(TAG, "WS RX (%d B): %.200s", ev->payload_len, s_gl.rx_buf);
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

/* ---- Frame dispatch ------------------------------------------------------- */

static void gl_dispatch_frame(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "Parse failed: %.80s", json);
        return;
    }

    /* setupComplete */
    cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "setupComplete");
    if (sc) {
        ESP_LOGI(TAG, "setupComplete received");
        s_gl.state = GL_STATE_READY;
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
            if (s_gl.state == GL_STATE_SPEAKING || s_gl.state == GL_STATE_THINKING) {
                gl_close_dac();
                gl_set_state(GL_STATE_LISTENING, "Listening");
                gl_open_dac(GL_TX_SAMPLE_RATE);
                gl_open_adc(GL_TX_SAMPLE_RATE);
                gl_start_tx_task();
            }
        }

        /* modelTurn parts */
        cJSON *turn = cJSON_GetObjectItemCaseSensitive(svc, "modelTurn");
        if (turn) {
            cJSON *parts = cJSON_GetObjectItemCaseSensitive(turn, "parts");
            cJSON *part;
            cJSON_ArrayForEach(part, parts) {
                /* Text part — print to console (Phase 2) */
                cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
                if (cJSON_IsString(text)) {
                    printf("[Gemini] %s\n", text->valuestring);
                }

                /* Audio part (Phase 3) */
                cJSON *id = cJSON_GetObjectItemCaseSensitive(part, "inlineData");
                if (id) {
                    cJSON *data = cJSON_GetObjectItemCaseSensitive(id, "data");
                    if (cJSON_IsString(data)) {
                        if (s_gl.state == GL_STATE_LISTENING || s_gl.state == GL_STATE_READY) {
                            /* First audio chunk: stop TX, switch to 24kHz */
                            gl_stop_tx_task();
                            gl_close_adc();
                            gl_set_state(GL_STATE_SPEAKING, "Speaking");
                            gl_open_dac(GL_RX_SAMPLE_RATE);
                        }
                        gl_play_audio_b64(data->valuestring);
                    }
                }
            }
        }

        /* turnComplete */
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(svc, "turnComplete");
        if (cJSON_IsTrue(tc) && s_gl.state == GL_STATE_SPEAKING) {
            /* Finish playback, switch back to LISTENING */
            gl_close_dac();
            gl_set_state(GL_STATE_LISTENING, "Listening");
            gl_open_dac(GL_TX_SAMPLE_RATE);
            gl_open_adc(GL_TX_SAMPLE_RATE);
            gl_start_tx_task();
        }
        cJSON_Delete(root);
        return;
    }

    /* Function calling — run the tool via JarvisMCP and reply with toolResponse. */
    if (gl_handle_tool_call(root)) {
        cJSON_Delete(root);
        return;
    }

    /* Unhandled top-level frame — surface it instead of silently dropping.
     * On a bad setup Gemini may send an error/goAway frame; without this it
     * would vanish and look like a plain timeout. */
    ESP_LOGW(TAG, "Unhandled server frame: %.200s", json);
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
        ESP_LOGI(TAG, "rx queue depth=%u", (unsigned)depth);
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

        /* Build WSS URL */
        char url[512];
        snprintf(url, sizeof(url),
                 "wss://%s%s?key=%s", GEMINI_WS_HOST, GEMINI_WS_PATH, s_gl.api_key);

        ESP_LOGI(TAG, "Connecting to Gemini Live...");
        gl_set_state(GL_STATE_CONNECTING, "Connecting");

        esp_websocket_client_config_t ws_cfg = {
            .uri              = url,
            .buffer_size      = 4096,
            .task_stack       = 8192,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        s_gl.ws_client = esp_websocket_client_init(&ws_cfg);
        esp_websocket_register_events(s_gl.ws_client, WEBSOCKET_EVENT_ANY,
                                      gl_ws_event_handler, NULL);
        esp_websocket_client_start(s_gl.ws_client);

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
        gl_open_dac(GL_TX_SAMPLE_RATE);
        gl_open_adc(GL_TX_SAMPLE_RATE);
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
        gl_set_state(GL_STATE_IDLE, "");
        if (s_gl.ws_client) {
            esp_websocket_client_stop(s_gl.ws_client);
            esp_websocket_client_destroy(s_gl.ws_client);
            s_gl.ws_client = NULL;
        }
        s_gl.ws_connected = false;
        s_gl.session_active = false;
        xEventGroupClearBits(s_gl.ev, GL_BIT_SESSION_ON);
    }

    ESP_LOGI(TAG, "Session task exiting");
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
    xEventGroupClearBits(s_gl.ev, GL_BIT_STOP | GL_BIT_TX_STOP | GL_BIT_TX_DONE);

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

    /* Touch toggle (Phase 5) is driven by cap_gemini_live_toggle(), called from
     * the emote tap callback — not by a separate polling task that would race
     * the emote system on the shared I2C bus. */

    ESP_LOGI(TAG, "Gemini Live gateway started");
    return ESP_OK;
}

static esp_err_t gl_gateway_stop(void)
{
    s_gl.stop_requested = true;
    s_gl.session_active = false;
    xEventGroupSetBits(s_gl.ev, GL_BIT_STOP | GL_BIT_TX_STOP);
    /* Tasks delete themselves when stop_requested is set */
    s_gl.session_task = NULL;
    s_gl.touch_task   = NULL;
    s_gl.tx_task      = NULL;
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
    if (!text || !s_gl.session_active || s_gl.state == GL_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return gl_send_text_turn(text) ? ESP_OK : ESP_FAIL;
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
        ESP_LOGI(TAG, "Tap: stopping session");
        s_gl.session_active = false;
        xEventGroupClearBits(s_gl.ev, GL_BIT_SESSION_ON);
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

    char url[512];
    snprintf(url, sizeof(url),
             "wss://%s%s?key=%s", GEMINI_WS_HOST, GEMINI_WS_PATH, s_gl.api_key);

    printf("[gemini-live test] Connecting to %s%s\n", GEMINI_WS_HOST, GEMINI_WS_PATH);

    esp_websocket_client_config_t ws_cfg = {
        .uri               = url,
        .buffer_size       = 4096,
        .task_stack        = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, gl_ws_event_handler, NULL);
    esp_websocket_client_start(client);

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
