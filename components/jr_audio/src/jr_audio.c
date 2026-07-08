/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_audio/src/jr_audio.c — device Audio Pipeline (capture + playback).
 *
 * A faithful first-cut harvest of v4's cap_gemini_live audio path (specs
 * audio-io-tdm, codec-bringup, aec-afe), refactored behind the pure
 * AudioSource / AudioSink ports. Lane/gain/AEC fine-tuning is an on-device step
 * (the board is not attached here); this compiles and links the real path.
 */
#include "jr_audio/jr_audio.h"

#include <string.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "dev_audio_codec.h"          /* dev_audio_codec_handles_t (double-cast) */
#include "esp_codec_dev.h"
#include "esp_codec_dev_types.h"

#include "esp_aec.h"                  /* esp-sr direct low-level AEC             */

static const char *TAG = "jr_audio";

/* ------------------------ sizing constants (spec) ----------------------- */
#define GL_TX_SAMPLE_RATE        16000
#define GL_RX_SAMPLE_RATE        24000
#define GL_CAP_SAMPLE_RATE       24000   /* == RX; shared duplex clock          */
#define GL_BITS                  16
#define GL_CHANNELS              1
#define GL_CAPTURE_CHANNELS      4       /* all TDM lanes, SW demux             */
#define GL_TX_CHUNK_MS           32

#define GL_TX_SAMPLES_PER_CHUNK  (GL_TX_SAMPLE_RATE  * GL_TX_CHUNK_MS / 1000)   /* 512 */
#define GL_CAP_SAMPLES_PER_CHUNK (GL_CAP_SAMPLE_RATE * GL_TX_CHUNK_MS / 1000)   /* 768 */
#define GL_CAP_RAW_BYTES         (GL_CAP_SAMPLES_PER_CHUNK * GL_CAPTURE_CHANNELS * 2) /* 6144 */
#define GL_TX_PCM_BYTES          (GL_TX_SAMPLES_PER_CHUNK * 2)                  /* 1024 */

/* Measured buffer lane order [REF][MIC][NC][MIC] (hw tone test 2026-06-12). */
#define GL_MIC_LANE              1
#define GL_REF_LANE              0
#define GL_REF_CHIP_MASK_BIT     2       /* ES7210 MIC3 gain (chip-mic index)   */

/* PGA / volume defaults (spec §codec-bringup §3). */
#define GL_MIC_PGA_DB            24
#define GL_REF_PGA_DB            12
#define GL_OUT_VOL_DEFAULT       100

/* Uplink digital make-up gain on the AEC-clean signal, with a soft knee. */
#define GL_OUT_GAIN_Q8           (6 * 256)  /* 6.0x in Q8 */

/* AEC config (spec §aec-afe). */
#define GL_AEC_FILTER_LENGTH     4
#define GL_AEC_MODE              AEC_MODE_FD_LOW_COST

/* Playback ring: ~2 s of 24 kHz mono, drop-newest. */
#define PB_RING_SAMPLES          (GL_RX_SAMPLE_RATE * 2)
#define PB_FEED_CHUNK            768        /* one 32 ms 24 kHz write */

/* --------------------------- module state ------------------------------ */
static esp_codec_dev_handle_t s_dac;
static esp_codec_dev_handle_t s_adc;
static bool                   s_dac_open;
static bool                   s_adc_open;

static aec_handle_t          *s_aec;
static int16_t               *s_aec_mic;     /* 16-byte aligned, 512 int16 */
static int16_t               *s_aec_ref;
static int16_t               *s_aec_clean;
static _Atomic uint32_t       s_last_aec_us;

static int16_t               *s_raw_cap;     /* 768*4 int16 (24k read)     */
static int16_t               *s_raw16;       /* 512*4 int16 (16k demuxed)  */

/* runtime-tunable gains (chip-mic dB / out vol) */
static _Atomic int            s_mic_db  = GL_MIC_PGA_DB;
static _Atomic int            s_ref_db  = GL_REF_PGA_DB;
static _Atomic int            s_out_vol = GL_OUT_VOL_DEFAULT;

/* fast-kill mute flag (set from capture task; read by feeder) */
static _Atomic bool           s_muted;

/* playback ring (int16 mono @ 24 kHz) */
static int16_t               *s_pb;
static volatile size_t        s_pb_head;     /* read index  */
static volatile size_t        s_pb_tail;     /* write index */
static portMUX_TYPE           s_pb_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t           s_feeder;

static bool                   s_ready;

/* ======================================================================== *
 *  codec bring-up                                                          *
 * ======================================================================== */

static esp_codec_dev_handle_t extract_codec(void *dev_h)
{
    /* board manager writes the INNER handle struct ptr into out; cast directly
     * to dev_audio_codec_handles_t* and take ->codec_dev. NEVER ->device_handle
     * (double-deref -> flash-mapped junk -> LoadStoreError). */
    if (dev_h == NULL) {
        return NULL;
    }
    return ((dev_audio_codec_handles_t *)dev_h)->codec_dev;
}

static void acquire_handles(void)
{
    for (int attempt = 0; attempt < 6 && (s_dac == NULL || s_adc == NULL); ++attempt) {
        if (s_dac == NULL) {
            void *h = NULL;
            if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, &h) == ESP_OK) {
                s_dac = extract_codec(h);
            }
        }
        if (s_adc == NULL) {
            void *h = NULL;
            if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_ADC, &h) == ESP_OK) {
                s_adc = extract_codec(h);
            }
        }
        if (s_dac == NULL || s_adc == NULL) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    ESP_LOGI(TAG, "codec handles: dac=%p adc=%p", (void *)s_dac, (void *)s_adc);
}

static void apply_in_gains(void)
{
    if (s_adc == NULL) {
        return;
    }
    float mic = (float)atomic_load(&s_mic_db);
    float ref = (float)atomic_load(&s_ref_db);
    /* MEMS MIC1|MIC2 = chip mask bits 0|1; echo-ref MIC3 = chip mask bit 2. */
    esp_codec_dev_set_in_channel_gain(s_adc,
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1), mic);
    esp_codec_dev_set_in_channel_gain(s_adc,
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(GL_REF_CHIP_MASK_BIT), ref);
}

static esp_err_t open_adc(void)
{
    if (s_adc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t fs;
    memset(&fs, 0, sizeof fs);
    fs.sample_rate     = GL_CAP_SAMPLE_RATE;    /* 24000                */
    fs.channel         = GL_CAPTURE_CHANNELS;   /* 4 — all TDM lanes    */
    fs.bits_per_sample = GL_BITS;               /* 16                   */
    fs.channel_mask    = 0;                     /* never lane-pick here */
    int r = esp_codec_dev_open(s_adc, &fs);
    if (r != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "adc open failed: %d", r);
        return ESP_FAIL;
    }
    s_adc_open = true;
    apply_in_gains();                           /* es7210 resets gain on open */
    return ESP_OK;
}

static esp_err_t open_dac(void)
{
    if (s_dac == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t fs;
    memset(&fs, 0, sizeof fs);
    fs.sample_rate     = GL_RX_SAMPLE_RATE;     /* 24000 */
    fs.channel         = GL_CHANNELS;           /* 1     */
    fs.bits_per_sample = GL_BITS;               /* 16    */
    int r = esp_codec_dev_open(s_dac, &fs);
    if (r != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "dac open failed: %d", r);
        return ESP_FAIL;
    }
    s_dac_open = true;
    esp_codec_dev_set_out_mute(s_dac, false);
    esp_codec_dev_set_out_vol(s_dac, atomic_load(&s_out_vol));
    return ESP_OK;
}

/* ======================================================================== *
 *  capture DSP                                                             *
 * ======================================================================== */

/* 3:2 per-lane linear downsample 24 kHz -> 16 kHz, preserving 4-lane interleave.
 * in = 768*4 samples, out = 512*4 samples. */
static void downsample_24to16_4lane(const int16_t *in, int16_t *out)
{
    for (int j = 0; j < GL_TX_SAMPLES_PER_CHUNK; ++j) {
        int pos  = j * 3;                 /* j * 1.5 in half-steps */
        int lo   = pos >> 1;
        int hi   = lo + 1;
        if (hi >= GL_CAP_SAMPLES_PER_CHUNK) {
            hi = GL_CAP_SAMPLES_PER_CHUNK - 1;
        }
        int frac = (pos & 1) << 14;       /* 0 or 0.5 in Q15 */
        for (int l = 0; l < GL_CAPTURE_CHANNELS; ++l) {
            int a = in[lo * GL_CAPTURE_CHANNELS + l];
            int b = in[hi * GL_CAPTURE_CHANNELS + l];
            out[j * GL_CAPTURE_CHANNELS + l] = (int16_t)(a + (((b - a) * frac) >> 15));
        }
    }
}

/* Uplink digital make-up with a soft knee to keep the AEC-clean signal audible
 * without harsh clipping. */
static inline int16_t soft_gain(int32_t x)
{
    int32_t g = (x * GL_OUT_GAIN_Q8) >> 8;
    /* soft knee above ~85% FS */
    const int32_t knee = 28000;
    if (g > knee) {
        g = knee + ((g - knee) >> 2);
    } else if (g < -knee) {
        g = -knee + ((g + knee) >> 2);
    }
    if (g > 32767)  g = 32767;
    if (g < -32768) g = -32768;
    return (int16_t)g;
}

/* AudioSource.read: pull one 16 kHz mono frame (already demuxed/AEC'd). */
static int src_read(void *ctx, jr_pcm_t *frame, size_t max_samples)
{
    (void)ctx;
    if (!s_ready || !s_adc_open || frame == NULL || max_samples == 0) {
        return 0;
    }

    int r = esp_codec_dev_read(s_adc, s_raw_cap, GL_CAP_RAW_BYTES);
    if (r != ESP_CODEC_DEV_OK) {
        return 0;   /* nothing this tick */
    }
    downsample_24to16_4lane(s_raw_cap, s_raw16);

    /* demux + (conditional) AEC. Run AEC only while the DAC is un-muted (i.e.
     * playback active) — the SPEAKING-only budget heuristic (spec §aec-afe §2).
     * When muted (listening), pass the mic through demux+gain only. */
    bool run_aec = (s_aec != NULL) && !atomic_load(&s_muted);
    int16_t *clean;
    if (run_aec) {
        for (int i = 0; i < GL_TX_SAMPLES_PER_CHUNK; ++i) {
            s_aec_mic[i] = s_raw16[i * GL_CAPTURE_CHANNELS + GL_MIC_LANE];
            s_aec_ref[i] = s_raw16[i * GL_CAPTURE_CHANNELS + GL_REF_LANE];
        }
        int64_t t0 = esp_timer_get_time();
        aec_process(s_aec, s_aec_mic, s_aec_ref, s_aec_clean);
        atomic_store(&s_last_aec_us, (uint32_t)(esp_timer_get_time() - t0));
        clean = s_aec_clean;
    } else {
        for (int i = 0; i < GL_TX_SAMPLES_PER_CHUNK; ++i) {
            s_aec_mic[i] = s_raw16[i * GL_CAPTURE_CHANNELS + GL_MIC_LANE];
        }
        clean = s_aec_mic;
    }

    size_t n = (max_samples < (size_t)GL_TX_SAMPLES_PER_CHUNK)
                   ? max_samples : (size_t)GL_TX_SAMPLES_PER_CHUNK;
    for (size_t i = 0; i < n; ++i) {
        frame[i] = soft_gain((int32_t)clean[i]);
    }
    return (int)n;
}

/* ======================================================================== *
 *  playback ring + feeder                                                  *
 * ======================================================================== */

static size_t pb_count_locked(void)
{
    return (s_pb_tail + PB_RING_SAMPLES - s_pb_head) % PB_RING_SAMPLES;
}

/* AudioSink.write: enqueue 24 kHz mono; drop-newest when full; never blocks. */
static int sink_write(void *ctx, const jr_pcm_t *frame, size_t samples)
{
    (void)ctx;
    if (!s_ready || s_pb == NULL || frame == NULL || samples == 0) {
        return 0;
    }
    size_t accepted = 0;
    portENTER_CRITICAL(&s_pb_lock);
    size_t free_slots = PB_RING_SAMPLES - 1 - pb_count_locked();
    size_t take = (samples < free_slots) ? samples : free_slots;
    for (size_t i = 0; i < take; ++i) {
        s_pb[s_pb_tail] = frame[i];
        s_pb_tail = (s_pb_tail + 1) % PB_RING_SAMPLES;
    }
    accepted = take;
    portEXIT_CRITICAL(&s_pb_lock);
    return (int)accepted;   /* < samples == drop-newest backpressure */
}

/* AudioSink.mute_now: synchronous fast-kill. Non-blocking (flag + I2C mute +
 * ring flush). Safe from the capture task. */
static void sink_mute_now(void *ctx)
{
    (void)ctx;
    atomic_store(&s_muted, true);
    if (s_dac_open) {
        esp_codec_dev_set_out_mute(s_dac, true);
    }
    portENTER_CRITICAL(&s_pb_lock);
    s_pb_head = s_pb_tail = 0;
    portEXIT_CRITICAL(&s_pb_lock);
}

static void feeder_task(void *arg)
{
    (void)arg;
    int16_t chunk[PB_FEED_CHUNK];
    for (;;) {
        if (atomic_load(&s_muted) || !s_dac_open) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t got = 0;
        portENTER_CRITICAL(&s_pb_lock);
        size_t avail = pb_count_locked();
        size_t take = (avail < PB_FEED_CHUNK) ? avail : PB_FEED_CHUNK;
        for (size_t i = 0; i < take; ++i) {
            chunk[i] = s_pb[s_pb_head];
            s_pb_head = (s_pb_head + 1) % PB_RING_SAMPLES;
        }
        got = take;
        portEXIT_CRITICAL(&s_pb_lock);

        if (got == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        esp_codec_dev_write(s_dac, chunk, (int)(got * sizeof(int16_t)));
    }
}

/* ======================================================================== *
 *  public API                                                              *
 * ======================================================================== */

static int aligned16(int16_t **p, size_t bytes)
{
    *p = (int16_t *)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (*p == NULL) {
        *p = (int16_t *)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM);
    }
    return *p != NULL ? 0 : -1;
}

esp_err_t jr_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    acquire_handles();

    /* capture/downsample scratch (PSRAM ok) */
    s_raw_cap = (int16_t *)heap_caps_malloc(GL_CAP_RAW_BYTES, MALLOC_CAP_SPIRAM);
    s_raw16   = (int16_t *)heap_caps_malloc((size_t)GL_TX_SAMPLES_PER_CHUNK * GL_CAPTURE_CHANNELS * 2,
                                            MALLOC_CAP_SPIRAM);
    if (s_raw_cap == NULL || s_raw16 == NULL) {
        ESP_LOGE(TAG, "capture scratch alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* AEC frame buffers: 16-byte aligned mono int16, internal-RAM first. */
    if (aligned16(&s_aec_mic, GL_TX_PCM_BYTES) ||
        aligned16(&s_aec_ref, GL_TX_PCM_BYTES) ||
        aligned16(&s_aec_clean, GL_TX_PCM_BYTES)) {
        ESP_LOGE(TAG, "aec buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* playback ring in PSRAM */
    s_pb = (int16_t *)heap_caps_malloc(PB_RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_pb == NULL) {
        ESP_LOGE(TAG, "playback ring alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_pb_head = s_pb_tail = 0;

    /* open ADC first, then DAC (muted at rest) — spec §codec-bringup §2. */
    esp_err_t adc_err = open_adc();
    esp_err_t dac_err = open_dac();
    if (dac_err == ESP_OK) {
        esp_codec_dev_set_out_mute(s_dac, true);
        atomic_store(&s_muted, true);
    }

    /* create the AEC: FD_LOW_COST, filter 4, 16 kHz, 1 mic + 1 ref. */
    aec_config_t aec_cfg = {
        .mic_num       = 1,
        .ref_num       = 1,
        .out_num       = 1,
        .filter_length = GL_AEC_FILTER_LENGTH,
        .sample_rate   = GL_TX_SAMPLE_RATE,    /* MUST be 16000 per header */
        .caps          = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        .mode          = GL_AEC_MODE,
        .nlp_level     = AEC_NLP_LEVEL_AGGR,
    };
    s_aec = aec_create_from_config(&aec_cfg);
    if (s_aec == NULL) {
        aec_cfg.caps = MALLOC_CAP_SPIRAM;
        s_aec = aec_create_from_config(&aec_cfg);
    }
    if (s_aec != NULL) {
        int cs = aec_get_chunksize(s_aec);
        if (cs != GL_TX_SAMPLES_PER_CHUNK) {
            ESP_LOGW(TAG, "aec chunksize %d != %d — disabling AEC (pass-through)",
                     cs, GL_TX_SAMPLES_PER_CHUNK);
            aec_destroy(s_aec);
            s_aec = NULL;
        }
    } else {
        ESP_LOGW(TAG, "aec create failed — capture runs un-cancelled");
    }

    /* start the playback feeder */
    xTaskCreatePinnedToCore(feeder_task, "jr_pb_feed", 4096, NULL, 6, &s_feeder, 1);

    s_ready = true;
    ESP_LOGI(TAG, "audio init: adc=%s dac=%s aec=%s",
             adc_err == ESP_OK ? "ok" : "FAIL",
             dac_err == ESP_OK ? "ok" : "FAIL",
             s_aec ? "on" : "off");
    /* ADC is the load-bearing lane; report its status. */
    return adc_err;
}

jr_audio_source_t jr_audio_source(void)
{
    jr_audio_source_t s;
    s.ctx = NULL;
    s.read = src_read;
    return s;
}

jr_audio_sink_t jr_audio_sink(void)
{
    jr_audio_sink_t s;
    s.ctx = NULL;
    s.write = sink_write;
    s.mute_now = sink_mute_now;
    return s;
}

uint32_t jr_audio_last_aec_us(void)
{
    return atomic_load(&s_last_aec_us);
}

void jr_audio_dac_unmute(void)
{
    atomic_store(&s_muted, false);
    if (s_dac_open) {
        esp_codec_dev_set_out_mute(s_dac, false);
    }
}

void jr_audio_flush_playback(void)
{
    portENTER_CRITICAL(&s_pb_lock);
    s_pb_head = s_pb_tail = 0;
    portEXIT_CRITICAL(&s_pb_lock);
}

void jr_audio_set_gains(int mic_db, int ref_db, int out_vol)
{
    if (mic_db >= 0) atomic_store(&s_mic_db, mic_db);
    if (ref_db >= 0) atomic_store(&s_ref_db, ref_db);
    if (mic_db >= 0 || ref_db >= 0) {
        apply_in_gains();
    }
    if (out_vol >= 0) {
        atomic_store(&s_out_vol, out_vol);
        if (s_dac_open) {
            esp_codec_dev_set_out_vol(s_dac, out_vol);
        }
    }
}
