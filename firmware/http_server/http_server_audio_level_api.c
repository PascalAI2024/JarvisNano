/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GET /api/audio/level
 *   { "rms_db": <float>, "peak_db": <float>, "ts": <uint64 ms> }
 *
 * Reads PCM samples directly from the on-board PDM-RX I2S channel
 * (the "i2s_audio_in" peripheral published by the board manager) in a
 * background task, computes RMS + peak over a ~100 ms window, and stores
 * the latest values in a mutex-guarded struct that the HTTP handler
 * snapshots on demand.
 *
 * On the Seeed XIAO ESP32-S3 Sense the audio_adc device is
 * `init_skip: true` (no codec IC), so esp_codec_dev_read() is not
 * available — but periph_i2s_init() already enables the PDM-RX channel
 * before any device init runs, so we can call i2s_channel_read() on the
 * raw handle without disturbing any other consumer.
 *
 * Gemini session gate (STABILITY_PLAN.md P1.6 / F14): the Gemini Live
 * session owns the same RX channel while a session is active, so this
 * sampler must never read concurrently. The task polls
 * gemini_live_is_active() (a) at the top of each cycle, and (b)
 * immediately before EVERY i2s_channel_read — the 100 ms window is
 * filled in ~20 ms chunks so an activating session is observed within
 * one chunk. After the session is last seen active, reads stay off for
 * a further AUDIO_LEVEL_SESSION_SETTLE_MS.
 *
 * Why no practical overlap window remains:
 *  - Session start: is_active() flips true when the session task enters
 *    CONNECTING — its first I2S access (gl_open_adc / mic capture) only
 *    happens after the TLS WebSocket connect + setupComplete, hundreds
 *    of ms later. The sampler re-checks before every ≤20 ms chunk, so it
 *    has long backed off by then. Residual theoretical window: a single
 *    chunk read already in flight when the flag flips (≤ ~20 ms of
 *    samples, and only if the flag flips in the µs between check and
 *    read entry — still far before the session touches the channel).
 *  - Session stop: session_cleanup closes the codec BEFORE clearing
 *    session_active/state (cap_gemini_live.c session_cleanup ordering),
 *    and the settle delay additionally covers teardown tails and rapid
 *    stop→start cycles.
 */
#include "http_server_priv.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_common.h"
#include "esp_board_periph.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "http_audio_level";

#define AUDIO_LEVEL_PERIPH_NAME      "i2s_audio_in"
#define AUDIO_LEVEL_SAMPLE_RATE_HZ   16000
#define AUDIO_LEVEL_WINDOW_MS        100
#define AUDIO_LEVEL_WINDOW_SAMPLES   ((AUDIO_LEVEL_SAMPLE_RATE_HZ * AUDIO_LEVEL_WINDOW_MS) / 1000)
#define AUDIO_LEVEL_DBFS_FLOOR       (-80.0f)
#define AUDIO_LEVEL_TASK_STACK       4096
#define AUDIO_LEVEL_TASK_PRIO        3
/* Read timeout per chunk; if the I2S subsystem is starved we just retry. */
#define AUDIO_LEVEL_READ_TIMEOUT_MS  500
/* The window is filled in short chunks so gemini_live_is_active() can be
 * re-checked immediately before every i2s_channel_read (P1.6). */
#define AUDIO_LEVEL_CHUNK_MS         20
#define AUDIO_LEVEL_CHUNK_SAMPLES    ((AUDIO_LEVEL_SAMPLE_RATE_HZ * AUDIO_LEVEL_CHUNK_MS) / 1000)
/* After a Gemini session was last observed active, stay off the RX channel
 * for this long before any read may resume (covers codec teardown tails and
 * rapid stop→start session cycles). */
#define AUDIO_LEVEL_SESSION_SETTLE_MS 750

typedef struct {
    float       rms_db;
    float       peak_db;
    uint64_t    ts_ms;
    bool        valid;
} audio_level_snapshot_t;

static SemaphoreHandle_t        s_lvl_mx;
static audio_level_snapshot_t   s_lvl;
static TaskHandle_t             s_lvl_task;
static i2s_chan_handle_t        s_rx_chan;

static void audio_level_store_invalid(void)
{
    if (s_lvl_mx && xSemaphoreTake(s_lvl_mx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_lvl.rms_db = AUDIO_LEVEL_DBFS_FLOOR;
        s_lvl.peak_db = AUDIO_LEVEL_DBFS_FLOOR;
        s_lvl.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
        s_lvl.valid = false;
        xSemaphoreGive(s_lvl_mx);
    }
}

static float audio_level_db_from_amp(float amplitude_unit)
{
    /* amplitude_unit is normalized RMS or peak in [0, 1]. */
    if (!(amplitude_unit > 1e-10f)) {
        return AUDIO_LEVEL_DBFS_FLOOR;
    }
    float db = 20.0f * log10f(amplitude_unit);
    if (db < AUDIO_LEVEL_DBFS_FLOOR) {
        db = AUDIO_LEVEL_DBFS_FLOOR;
    } else if (db > 0.0f) {
        db = 0.0f;
    }
    return db;
}

static bool audio_level_session_active(void)
{
    http_server_ctx_t *ctx = http_server_ctx();
    return ctx && ctx->services.gemini_live_is_active &&
           ctx->services.gemini_live_is_active();
}

static void audio_level_task(void *arg)
{
    (void)arg;

    int16_t *buf = (int16_t *)heap_caps_malloc(AUDIO_LEVEL_WINDOW_SAMPLES * sizeof(int16_t),
                                                MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "alloc %d-sample buffer failed", AUDIO_LEVEL_WINDOW_SAMPLES);
        s_lvl_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "audio level task running (window=%d ms / %d samples @ %d Hz)",
             AUDIO_LEVEL_WINDOW_MS, AUDIO_LEVEL_WINDOW_SAMPLES, AUDIO_LEVEL_SAMPLE_RATE_HZ);

    uint32_t read_warns = 0;
    int64_t  last_active_us = 0;   /* last time a Gemini session was observed active */
    bool     was_active = false;
    while (1) {
        /* P1.6 gate: never touch the shared I2S RX channel while a Gemini
         * Live session owns the mic. */
        if (audio_level_session_active()) {
            last_active_us = esp_timer_get_time();
            if (!was_active) {
                ESP_LOGI(TAG, "gemini session active — audio level sampling paused");
                was_active = true;
            }
            audio_level_store_invalid();
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        /* Settle window: after the session flag flips, hold off further so a
         * codec teardown tail or an immediate session restart can never
         * overlap a read. */
        if (last_active_us != 0 &&
            (esp_timer_get_time() - last_active_us) <
                (int64_t)AUDIO_LEVEL_SESSION_SETTLE_MS * 1000) {
            audio_level_store_invalid();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (was_active) {
            ESP_LOGI(TAG, "gemini session idle for %d ms — audio level sampling resumed",
                     AUDIO_LEVEL_SESSION_SETTLE_MS);
            was_active = false;
        }

        i2s_chan_info_t chan_info = {0};
        esp_err_t info_err = i2s_channel_get_info(s_rx_chan, &chan_info);
        if (info_err != ESP_OK || !chan_info.is_enabled) {
            audio_level_store_invalid();
            if (info_err != ESP_OK && ((read_warns++ % 20) == 0)) {
                ESP_LOGD(TAG, "i2s_channel_get_info err=%d", info_err);
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        /* Fill the window in ≤AUDIO_LEVEL_CHUNK_MS chunks, re-checking the
         * session flag immediately before EVERY read so an activating
         * session is observed within one chunk. */
        const size_t want_bytes = AUDIO_LEVEL_WINDOW_SAMPLES * sizeof(int16_t);
        size_t    have_bytes  = 0;
        bool      session_hit = false;
        esp_err_t err         = ESP_OK;
        while (have_bytes < want_bytes) {
            if (audio_level_session_active()) {
                last_active_us = esp_timer_get_time();
                session_hit = true;
                break;
            }
            size_t chunk = AUDIO_LEVEL_CHUNK_SAMPLES * sizeof(int16_t);
            if (chunk > want_bytes - have_bytes) {
                chunk = want_bytes - have_bytes;
            }
            size_t bytes_read = 0;
            err = i2s_channel_read(s_rx_chan,
                                   (uint8_t *)buf + have_bytes,
                                   chunk, &bytes_read,
                                   pdMS_TO_TICKS(AUDIO_LEVEL_READ_TIMEOUT_MS));
            if (err != ESP_OK || bytes_read == 0) {
                break;
            }
            have_bytes += bytes_read;
        }

        if (session_hit) {
            audio_level_store_invalid();
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (err != ESP_OK || have_bytes == 0) {
            audio_level_store_invalid();
            if (err != ESP_ERR_INVALID_STATE && ((read_warns++ % 20) == 0)) {
                ESP_LOGD(TAG, "i2s_channel_read err=%d bytes=%u", err, (unsigned)have_bytes);
            }
            vTaskDelay(pdMS_TO_TICKS(err == ESP_ERR_INVALID_STATE ? 250 : 50));
            continue;
        }
        read_warns = 0;

        size_t n = have_bytes / sizeof(int16_t);
        uint64_t sumsq = 0;
        int32_t  peak  = 0;
        for (size_t i = 0; i < n; i++) {
            int32_t s = buf[i];
            int32_t a = s < 0 ? -s : s;
            if (a > peak) {
                peak = a;
            }
            sumsq += (uint64_t)((int64_t)s * (int64_t)s);
        }

        float rms_amp  = sqrtf((float)sumsq / (float)n) / 32768.0f;
        float peak_amp = (float)peak / 32768.0f;

        float rms_db   = audio_level_db_from_amp(rms_amp);
        float peak_db  = audio_level_db_from_amp(peak_amp);

        if (s_lvl_mx && xSemaphoreTake(s_lvl_mx, portMAX_DELAY) == pdTRUE) {
            s_lvl.rms_db  = rms_db;
            s_lvl.peak_db = peak_db;
            s_lvl.ts_ms   = (uint64_t)(esp_timer_get_time() / 1000);
            s_lvl.valid   = true;
            xSemaphoreGive(s_lvl_mx);
        }
    }
}

static esp_err_t audio_level_ensure_started(void)
{
    if (s_lvl_task) {
        return ESP_OK;
    }

    if (!s_lvl_mx) {
        s_lvl_mx = xSemaphoreCreateMutex();
        if (!s_lvl_mx) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_rx_chan) {
        void *h = NULL;
        esp_err_t err = esp_board_periph_get_handle(AUDIO_LEVEL_PERIPH_NAME, &h);
        if (err != ESP_OK || h == NULL) {
            ESP_LOGE(TAG, "no '%s' peripheral handle (err=%d) — audio level disabled",
                     AUDIO_LEVEL_PERIPH_NAME, err);
            return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
        }
        s_rx_chan = (i2s_chan_handle_t)h;
        ESP_LOGI(TAG, "bound to PDM-RX chan handle %p via '%s'",
                 (void *)s_rx_chan, AUDIO_LEVEL_PERIPH_NAME);
    }

    BaseType_t ok = xTaskCreate(audio_level_task, "audio_lvl", AUDIO_LEVEL_TASK_STACK,
                                NULL, AUDIO_LEVEL_TASK_PRIO, &s_lvl_task);
    if (ok != pdPASS) {
        s_lvl_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t audio_level_get_handler(httpd_req_t *req)
{
    /* Lazily start the sampler on the first call; gracefully report
     * silence if the PDM RX channel isn't available on this board. */
    esp_err_t err = audio_level_ensure_started();

    audio_level_snapshot_t snap = {
        .rms_db  = AUDIO_LEVEL_DBFS_FLOOR,
        .peak_db = AUDIO_LEVEL_DBFS_FLOOR,
        .ts_ms   = (uint64_t)(esp_timer_get_time() / 1000),
        .valid   = false,
    };

    if (err == ESP_OK && s_lvl_mx) {
        if (xSemaphoreTake(s_lvl_mx, portMAX_DELAY) == pdTRUE) {
            snap = s_lvl;
            xSemaphoreGive(s_lvl_mx);
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "rms_db",  snap.rms_db);
    cJSON_AddNumberToObject(root, "peak_db", snap.peak_db);
    cJSON_AddNumberToObject(root, "ts",      (double)snap.ts_ms);
    cJSON_AddBoolToObject  (root, "valid",   snap.valid);
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_audio_level_routes(httpd_handle_t server)
{
    const httpd_uri_t handler = {
        .uri     = "/api/audio/level",
        .method  = HTTP_GET,
        .handler = audio_level_get_handler,
    };
    return httpd_register_uri_handler(server, &handler);
}
