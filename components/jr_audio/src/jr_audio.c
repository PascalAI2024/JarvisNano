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

#include <math.h>
#include <string.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "dev_audio_codec.h"          /* dev_audio_codec_handles_t (double-cast) */
#include "esp_codec_dev.h"
#include "esp_codec_dev_types.h"
#include "jr_dsp/dsp.h"

#include "esp_aec.h"                  /* esp-sr direct low-level AEC             */

static const char *TAG = "jr_audio";

static void speaker_pa_enable(void)
{
#if defined(CONFIG_ESP_BOARD_ESP32S3_TOUCH_AMOLED_1_75C)
    /* Board-manager/codec owns PA lifecycle; this idempotent board-specific
     * latch preserves the known C-board unmute behavior without remuxing GPIO. */
    (void)gpio_set_level(GPIO_NUM_46, 1);
#endif
}


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
/* SPEAKING mic gain — the v4-proven barge fix (commit 1ced7406). Dropping the
 * MEMS-mic PGA to 9 dB while the model speaks keeps the speaker echo UNCLIPPED,
 * which is the one thing the linear AEC needs to actually cancel it (~18 dB
 * suppression -> echo residual ~180 instead of a railed ~9000). Restored to 24
 * dB on LISTENING so the user's own voice is loud for the local VAD. */
#if defined(CONFIG_ESP_BOARD_ESP32S3_TOUCH_AMOLED_1_75C)
/* 21, not 9: at 9 dB the owner's talk-over read ~20 rms on the C — inaudible
 * to both the local barge gate and Gemini's server VAD, so barge-in was dead.
 * At 21 dB (vadlog 2026-08-27) the AEC still held the echo residual to a
 * median of ~20 rms, so the hotter capture costs nothing. Runtime knob:
 * /api/debug/gain?speakmic=. */
#define GL_MIC_PGA_SPEAK_DB      21
#else
#define GL_MIC_PGA_SPEAK_DB      9
#endif
#define GL_REF_PGA_DB            12
#define GL_OUT_VOL_DEFAULT       100

/* Gemini PCM is intentionally conservative. The field-proven v4 path applied
 * 4x make-up gain before the ES8311, then compressed peaks above 24k so normal
 * speech stayed present without hard clipping. Keep codec volume at unity and
 * condition the PCM here; volume=100 alone does not restore that lost gain.
 *
 * The 4x figure is SPEAKER-CHAIN DEPENDENT: it was tuned on the original 1.75.
 * On the 1.75C the chain runs hotter and 4x rails the PCM (measured 2026-08-27:
 * 902/48000 samples pinned at 32767, flat-top runs to 0.8 ms — heard as
 * crackle/chop). The C default is 2.75x, and the live value is runtime-tunable
 * via /api/debug/gain?pbgain=<percent> for on-device calibration. */
#if defined(CONFIG_ESP_BOARD_ESP32S3_TOUCH_AMOLED_1_75C)
/* 2.75x: the owner found 2.5x too quiet; 3x pushed loud passages into the
 * knee compressor and crackled. Q8 default below; GL_PLAYBACK_GAIN stays
 * integer for the original board. */
#define GL_PLAYBACK_GAIN_Q8_DEFAULT   704   /* 2.75x */
#else
#define GL_PLAYBACK_GAIN_Q8_DEFAULT   1024  /* 4.0x — field-proven v4 value */
#endif
#define GL_PLAYBACK_LIMIT_KNEE   24000

/* Uplink digital make-up gain on the AEC-clean signal, with a soft knee. */
#define GL_OUT_GAIN_Q8           (6 * 256)  /* 6.0x in Q8 */

/* AEC config (spec §aec-afe). */
#define GL_AEC_FILTER_LENGTH     4
#define GL_AEC_MODE              AEC_MODE_FD_LOW_COST

/* Playback ring: the field-proven v4 budget was 512 KiB (~10.9 s at 24 kHz
 * mono PCM16). Gemini arrives in bursts, so the former 2 s v5 ring could fill
 * even when average DAC throughput was healthy and would then truncate chunks. */
#define PB_RING_BYTES            (512U * 1024U)
#define PB_RING_SAMPLES          (PB_RING_BYTES / sizeof(int16_t))
#define PB_FEED_CHUNK            768        /* one 32 ms 24 kHz write */
#define DIAG_TAP_SECONDS         2U
#define DIAG_CLIP_THRESHOLD      32760
#define DIAG_CHIRP_CHUNK         256U

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
/* AEC-clean mic RMS in Q8, measured before the uplink make-up gain. */
static _Atomic uint32_t       s_clean_rms_q8;

static int16_t               *s_raw_cap;     /* 768*4 int16 (24k read)     */
static int16_t               *s_raw16;       /* 512*4 int16 (16k demuxed)  */

/* runtime-tunable gains (chip-mic dB / out vol) */
static _Atomic int            s_mic_db       = GL_MIC_PGA_DB;
static _Atomic int            s_mic_speak_db = GL_MIC_PGA_SPEAK_DB;
static _Atomic int            s_ref_db       = GL_REF_PGA_DB;
static _Atomic int            s_out_vol      = GL_OUT_VOL_DEFAULT;
/* True while the model is speaking — selects the low SPEAKING mic gain so the
 * echo stays unclipped for the AEC. Set by the app on phase transitions. */
static _Atomic bool           s_capture_speaking;

/* fast-kill mute flag (set from capture task; read by feeder) */
static _Atomic bool           s_muted;

/* playback ring (int16 mono @ 24 kHz) */
static int16_t               *s_pb;
static volatile size_t        s_pb_head;     /* read index  */
static volatile size_t        s_pb_tail;     /* write index */
static portMUX_TYPE           s_pb_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t           s_feeder;
static _Atomic bool           s_feeder_writing;
static _Atomic uint32_t       s_playback_tail_until_ms;
static _Atomic bool           s_diag_chirp_enqueuing;

/* PLAYBACK TELEMETRY (N6.2). A 60 s reply that stutters once leaves no trace
 * in the transport counters — the socket delivered every byte — so "choppy"
 * had to be diagnosed by ear. These count what the speaker actually got.
 *
 *   underruns    the feeder found the ring EMPTY mid-reply and audio resumed
 *                within PB_GAP_REPLY_MS: a real hole in the sound. The ring
 *                also empties at the end of every reply; that is followed by
 *                silence, not by more audio, so it is not counted.
 *   max_gap_ms   the longest such hole.
 *   low_water    the emptiest the ring has been when a NEW chunk arrived while
 *                a reply was in flight — how close the network came to
 *                starving the DAC. Sampled on the producer side because the
 *                consumer side trivially reaches zero at every reply's end.
 *   dac_failures esp_codec_dev_write() returning an error.
 * Reset via jr_audio_playback_stats_reset() so a soak starts from zero. */
#define PB_GAP_REPLY_MS 2500u
static _Atomic uint32_t       s_pb_underruns;
static _Atomic uint32_t       s_pb_max_gap_ms;
static _Atomic uint32_t       s_pb_low_water_samples;   /* UINT32_MAX = unset */
static _Atomic uint32_t       s_pb_dac_failures;
static _Atomic uint32_t       s_pb_gaps_ended_by_silence;
static uint32_t               s_pb_gap_start_ms;        /* feeder task only */
static bool                   s_pb_in_gap;              /* feeder task only */

/* START-OF-REPLY PRE-ROLL. The feeder used to play the first sample the
 * moment it landed, so the speaker was only ever as far ahead of the network
 * as the last chunk — and the network delivers Gemini's audio in bursts with
 * holes between them (measured on one turn: frames arriving 9 deep with gaps
 * to 1.2 s, the parser never more than 91 ms behind, and the ring down to
 * 8 ms). Each hole came straight out of the speaker as a stutter.
 *
 * A reply now starts only once PB_PREROLL_SAMPLES are buffered, or after
 * PB_PREROLL_MAX_WAIT_MS with whatever has arrived, so the first burst's
 * lead absorbs the holes that follow. It costs up to 300 ms before the first
 * word of a reply that already took seconds to think; it does not apply to
 * diagnostic tones, which are UI feedback and must be immediate. The ring is
 * 512 KiB (~11 s), so holding never drops anything. Priming re-arms once the
 * ring has been empty for PB_GAP_REPLY_MS: the next reply, not a hole. */
/* Measured 2026-09-01. Nine spoken turns: server stalls inside a reply ran
 * 0.8-1.34 s; at 300/700 one hole of 81-407 ms per turn survived, at 600/1000
 * none. Then a 30-minute soak (18 turns): clean for eleven turns, then seven
 * holes to 955 ms, every one at a reply START where only the pre-roll
 * applied (a 1313 ms stall minus 600 ms = the 713 ms hole measured). So the
 * pre-roll now matches the stalls actually seen, and the refill exceeds
 * them, for +0.4 s before a first word that already waits 2-4 s on
 * thinking. Tune live: /api/debug/gain?preroll=&refill=. */
#define PB_PREROLL_MS_DEFAULT     600u  /* lead before a reply's first word */
#define PB_REFILL_MS_DEFAULT     1500u  /* lead rebuilt after a hole         */
#define PB_PRIME_MAX_WAIT_MS     2000u  /* never hold longer than this       */
static _Atomic uint32_t       s_pb_preroll_ms = PB_PREROLL_MS_DEFAULT;
static _Atomic uint32_t       s_pb_refill_ms  = PB_REFILL_MS_DEFAULT;
/* ADAPTIVE PRE-ROLL. 600 ms is a second of latency won back; 1500 ms is
 * what the worst measured server stall (2.2 s) needs to stay seamless. The
 * feeder walks between them: any reply with a hole steps the lead up, three
 * clean replies in a row step it back down. A pin from the gain route
 * (preroll > 0) stops the walk; preroll 0 restarts it. */
#define PB_PREROLL_FLOOR_MS      600u
#define PB_PREROLL_CEIL_MS      1500u
#define PB_PREROLL_STEP_UP_MS    300u
#define PB_PREROLL_STEP_DOWN_MS  200u
#define PB_CLEAN_REPLIES_TO_STEP   3u
static _Atomic bool           s_pb_adaptive = true;
static bool                   s_pb_reply_open;      /* feeder task only */
static uint32_t               s_pb_reply_holes;     /* feeder task only */
static uint32_t               s_pb_clean_replies;   /* feeder task only */

static void pb_adapt_preroll(void)
{
    if (!atomic_load(&s_pb_adaptive)) {
        return;
    }
    uint32_t pre = atomic_load(&s_pb_preroll_ms);
    if (s_pb_reply_holes > 0u) {
        s_pb_clean_replies = 0u;
        const uint32_t next = pre + PB_PREROLL_STEP_UP_MS > PB_PREROLL_CEIL_MS
                                  ? PB_PREROLL_CEIL_MS : pre + PB_PREROLL_STEP_UP_MS;
        if (next != pre) {
            atomic_store(&s_pb_preroll_ms, next);
            ESP_LOGI(TAG, "pre-roll %u -> %u ms after %u hole(s)",
                     (unsigned)pre, (unsigned)next, (unsigned)s_pb_reply_holes);
        }
    } else if (++s_pb_clean_replies >= PB_CLEAN_REPLIES_TO_STEP) {
        s_pb_clean_replies = 0u;
        const uint32_t next = pre > PB_PREROLL_FLOOR_MS + PB_PREROLL_STEP_DOWN_MS
                                  ? pre - PB_PREROLL_STEP_DOWN_MS : PB_PREROLL_FLOOR_MS;
        if (next != pre) {
            atomic_store(&s_pb_preroll_ms, next);
            ESP_LOGI(TAG, "pre-roll %u -> %u ms after clean replies",
                     (unsigned)pre, (unsigned)next);
        }
    }
}
static bool                   s_pb_primed;              /* feeder task only */
static uint32_t               s_pb_prime_target;        /* samples; feeder  */
static uint32_t               s_pb_prime_start_ms;      /* feeder task only */
static uint32_t               s_pb_empty_since_ms;      /* feeder task only */
static _Atomic bool           s_pb_skip_prime;          /* diagnostic queued */
static _Atomic uint32_t       s_pb_prerolls;
static _Atomic uint32_t       s_pb_preroll_timeouts;

void jr_audio_set_jitter_ms(int preroll_ms, int refill_ms)
{
    if (preroll_ms == 0) {
        atomic_store(&s_pb_preroll_ms, PB_PREROLL_FLOOR_MS);
        atomic_store(&s_pb_adaptive, true);    /* back to the walk */
    } else if (preroll_ms > 0) {
        atomic_store(&s_pb_preroll_ms, preroll_ms > 3000 ? 3000u : (uint32_t)preroll_ms);
        atomic_store(&s_pb_adaptive, false);   /* pinned by hand */
    }
    if (refill_ms >= 0) {
        atomic_store(&s_pb_refill_ms, refill_ms > 3000 ? 3000u : (uint32_t)refill_ms);
    }
}

int jr_audio_preroll_ms(void) { return (int)atomic_load(&s_pb_preroll_ms); }
int jr_audio_refill_ms(void)  { return (int)atomic_load(&s_pb_refill_ms); }

static bool pb_tail_active(uint32_t now_ms)
{
    const uint32_t tail = atomic_load(&s_playback_tail_until_ms);
    return (int32_t)(tail - now_ms) > 0;
}

typedef struct {
    int16_t *samples;
    size_t capacity;
    size_t head;
    size_t count;
    uint64_t total;
    uint32_t sample_rate;
    SemaphoreHandle_t lock;
} audio_tap_t;

static audio_tap_t s_taps[JR_AUDIO_TAP_COUNT];
static _Atomic bool s_taps_ready;

static bool                   s_ready;

static void tap_write(jr_audio_tap_kind_t kind, const int16_t *samples,
                      size_t count)
{
    if (!atomic_load(&s_taps_ready) || kind < 0 || kind >= JR_AUDIO_TAP_COUNT ||
        samples == NULL || count == 0) {
        return;
    }

    audio_tap_t *tap = &s_taps[kind];
    /* Exports may copy a complete two-second window. Missing a diagnostic
     * chunk is preferable to delaying capture, AEC, or DAC feeding. */
    if (tap->lock == NULL || xSemaphoreTake(tap->lock, 0) != pdTRUE) {
        return;
    }

    size_t observed = count;
    if (count >= tap->capacity) {
        samples += count - tap->capacity;
        count = tap->capacity;
        memcpy(tap->samples, samples, count * sizeof(int16_t));
        tap->head = 0;
        tap->count = tap->capacity;
    } else {
        size_t first = tap->capacity - tap->head;
        if (first > count) {
            first = count;
        }
        memcpy(&tap->samples[tap->head], samples,
               first * sizeof(int16_t));
        size_t second = count - first;
        if (second > 0) {
            memcpy(tap->samples, &samples[first],
                   second * sizeof(int16_t));
        }
        tap->head = (tap->head + count) % tap->capacity;
        if (tap->count + count >= tap->capacity) {
            tap->count = tap->capacity;
        } else {
            tap->count += count;
        }
    }
    tap->total += observed;
    xSemaphoreGive(tap->lock);
}

static bool diag_taps_init(void)
{
    static const uint32_t rates[JR_AUDIO_TAP_COUNT] = {
        [JR_AUDIO_TAP_MIC_CLEAN] = GL_TX_SAMPLE_RATE,
        [JR_AUDIO_TAP_MIC_RAW] = GL_TX_SAMPLE_RATE,
        [JR_AUDIO_TAP_REFERENCE] = GL_TX_SAMPLE_RATE,
        [JR_AUDIO_TAP_PLAYBACK] = GL_RX_SAMPLE_RATE,
    };
    for (int i = 0; i < JR_AUDIO_TAP_COUNT; ++i) {
        audio_tap_t *tap = &s_taps[i];
        tap->sample_rate = rates[i];
        tap->capacity = (size_t)rates[i] * DIAG_TAP_SECONDS;
        tap->lock = xSemaphoreCreateMutex();
        tap->samples = heap_caps_calloc(tap->capacity, sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (tap->lock == NULL || tap->samples == NULL) {
            for (int j = 0; j <= i; ++j) {
                heap_caps_free(s_taps[j].samples);
                if (s_taps[j].lock != NULL) {
                    vSemaphoreDelete(s_taps[j].lock);
                }
                memset(&s_taps[j], 0, sizeof s_taps[j]);
            }
            ESP_LOGW(TAG, "audio diagnostics taps unavailable (PSRAM)");
            return false;
        }
    }
    atomic_store(&s_taps_ready, true);
    ESP_LOGI(TAG, "audio diagnostics: four rolling 2 s taps ready");
    return true;
}

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
    /* State-aware: low mic gain while the model speaks (unclipped echo -> AEC
     * cancels), full mic gain while listening (loud user voice for the VAD). */
    float mic = (float)atomic_load(atomic_load(&s_capture_speaking)
                                   ? &s_mic_speak_db : &s_mic_db);
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
    speaker_pa_enable();
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

    /* Demux once at the sole ADC read point. The taps observe both measured
     * TDM lanes regardless of whether AEC happens to be active this frame. */
    for (int i = 0; i < GL_TX_SAMPLES_PER_CHUNK; ++i) {
        s_aec_mic[i] = s_raw16[i * GL_CAPTURE_CHANNELS + GL_MIC_LANE];
        s_aec_ref[i] = s_raw16[i * GL_CAPTURE_CHANNELS + GL_REF_LANE];
    }
    tap_write(JR_AUDIO_TAP_MIC_RAW, s_aec_mic,
              GL_TX_SAMPLES_PER_CHUNK);
    tap_write(JR_AUDIO_TAP_REFERENCE, s_aec_ref,
              GL_TX_SAMPLES_PER_CHUNK);

    /* Run AEC only while playback is pending — the SPEAKING-only budget
     * heuristic (spec §aec-afe §2). Otherwise pass the mic through. */
    bool run_aec = (s_aec != NULL) && jr_audio_playback_pending();
    int16_t *clean;
    if (run_aec) {
        int64_t t0 = esp_timer_get_time();
        aec_process(s_aec, s_aec_mic, s_aec_ref, s_aec_clean);
        atomic_store(&s_last_aec_us, (uint32_t)(esp_timer_get_time() - t0));
        clean = s_aec_clean;
    } else {
        clean = s_aec_mic;
    }

    size_t n = (max_samples < (size_t)GL_TX_SAMPLES_PER_CHUNK)
                   ? max_samples : (size_t)GL_TX_SAMPLES_PER_CHUNK;

    /* Measure the AEC-clean signal BEFORE the uplink make-up gain.
     *
     * `frame` below is amplified GL_OUT_GAIN_Q8 (6x) so Gemini can hear the
     * user, and until now the VAD judged that same amplified buffer — which
     * multiplies the room's noise bed by 6 along with the voice. That is why a
     * lived-in room reads 170-390 RMS against an onset of 85 and the device
     * believes someone is talking indefinitely. Publishing the pre-gain RMS
     * lets the turn policy judge the signal on its own scale. Measurement
     * only — nothing consumes this yet, and the uplink is untouched. */
    float clean_rms = jr_dsp_rms(clean, n);
    atomic_store(&s_clean_rms_q8, (uint32_t)(clean_rms * 256.0f));

    for (size_t i = 0; i < n; ++i) {
        frame[i] = soft_gain((int32_t)clean[i]);
    }
    tap_write(JR_AUDIO_TAP_MIC_CLEAN, frame, n);
    return (int)n;
}

float jr_audio_clean_rms(void)
{
    return (float)atomic_load(&s_clean_rms_q8) / 256.0f;
}

/* ======================================================================== *
 *  playback ring + feeder                                                  *
 * ======================================================================== */

static size_t pb_count_locked(void)
{
    return (s_pb_tail + PB_RING_SAMPLES - s_pb_head) % PB_RING_SAMPLES;
}

static size_t pb_enqueue(const int16_t *frame, size_t samples,
                         bool diagnostic)
{
    size_t accepted = 0;
    portENTER_CRITICAL(&s_pb_lock);
    /* Recheck under the ring lock so a normal writer that raced the chirp flag
     * cannot interleave with its all-or-nothing enqueue. */
    if (diagnostic) {
        atomic_store(&s_pb_skip_prime, true);
    }
    if (diagnostic || !atomic_load(&s_diag_chirp_enqueuing)) {
        const size_t level = pb_count_locked();
        size_t free_slots = PB_RING_SAMPLES - 1 - level;
        size_t take = (samples < free_slots) ? samples : free_slots;
        if (!diagnostic && level < atomic_load(&s_pb_low_water_samples) &&
            pb_tail_active((uint32_t)(esp_timer_get_time() / 1000))) {
            atomic_store(&s_pb_low_water_samples, (uint32_t)level);
        }
        size_t first = PB_RING_SAMPLES - s_pb_tail;
        if (first > take) {
            first = take;
        }
        memcpy(&s_pb[s_pb_tail], frame, first * sizeof(int16_t));
        size_t second = take - first;
        if (second > 0) {
            memcpy(s_pb, &frame[first], second * sizeof(int16_t));
        }
        s_pb_tail = (s_pb_tail + take) % PB_RING_SAMPLES;
        accepted = take;
    }
    portEXIT_CRITICAL(&s_pb_lock);
    return accepted;
}

/* Runtime playback make-up gain in Q8 (256 == 1.0x). Tunable live so the
 * speaker chain can be calibrated by ear without a reflash. */
static _Atomic int32_t s_pb_gain_q8 = GL_PLAYBACK_GAIN_Q8_DEFAULT;

void jr_audio_set_playback_gain_percent(int percent)
{
    if (percent < 25 || percent > 800) {
        return;
    }
    atomic_store(&s_pb_gain_q8, (int32_t)percent * 256 / 100);
}

int jr_audio_playback_gain_percent(void)
{
    return (int)(atomic_load(&s_pb_gain_q8) * 100 / 256);
}

static inline int16_t playback_gain(int16_t sample, int32_t gain_q8)
{
    int32_t value = ((int32_t)sample * gain_q8) >> 8;
    int32_t magnitude = value < 0 ? -value : value;
    if (magnitude > GL_PLAYBACK_LIMIT_KNEE) {
        magnitude = GL_PLAYBACK_LIMIT_KNEE +
                    ((magnitude - GL_PLAYBACK_LIMIT_KNEE) >> 2);
        if (magnitude > 32767) {
            magnitude = 32767;
        }
        value = value < 0 ? -magnitude : magnitude;
    }
    return (int16_t)value;
}

/* AudioSink.write: enqueue 24 kHz mono; drop-newest when full; never blocks. */
static int sink_write(void *ctx, const jr_pcm_t *frame, size_t samples)
{
    (void)ctx;
    if (!s_ready || s_pb == NULL || frame == NULL || samples == 0) {
        return 0;
    }
    int16_t conditioned[PB_FEED_CHUNK];
    size_t accepted_total = 0;
    const int32_t gain_q8 = atomic_load(&s_pb_gain_q8);
    while (accepted_total < samples) {
        size_t count = samples - accepted_total;
        if (count > PB_FEED_CHUNK) {
            count = PB_FEED_CHUNK;
        }
        for (size_t i = 0; i < count; ++i) {
            conditioned[i] = playback_gain(frame[accepted_total + i], gain_q8);
        }
        size_t accepted = pb_enqueue(conditioned, count, false);
        accepted_total += accepted;
        if (accepted < count) {
            break;
        }
    }
    return (int)accepted_total; /* < samples == drop-newest backpressure */
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
    atomic_store(&s_playback_tail_until_ms, 0);
}

static void feeder_task(void *arg)
{
    (void)arg;
    int16_t chunk[PB_FEED_CHUNK];
    s_pb_prime_target = atomic_load(&s_pb_preroll_ms) * 24u;
    for (;;) {
        if (atomic_load(&s_muted) || !s_dac_open) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t got = 0;
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool hold = false;
        portENTER_CRITICAL(&s_pb_lock);
        size_t avail = pb_count_locked();
        if (avail > 0 && !s_pb_primed) {
            if (atomic_exchange(&s_pb_skip_prime, false)) {
                s_pb_primed = true;                 /* a tone: play it now */
            } else if (avail >= s_pb_prime_target) {
                s_pb_primed = true;
                atomic_fetch_add(&s_pb_prerolls, 1u);
            } else if (s_pb_prime_start_ms == 0u) {
                s_pb_prime_start_ms = now_ms;
                hold = true;
            } else if (now_ms - s_pb_prime_start_ms >= PB_PRIME_MAX_WAIT_MS) {
                s_pb_primed = true;                 /* short reply: go */
                atomic_fetch_add(&s_pb_preroll_timeouts, 1u);
            } else {
                hold = true;
            }
        }
        size_t take = hold ? 0 : (avail < PB_FEED_CHUNK) ? avail : PB_FEED_CHUNK;
        for (size_t i = 0; i < take; ++i) {
            chunk[i] = s_pb[s_pb_head];
            s_pb_head = (s_pb_head + 1) % PB_RING_SAMPLES;
        }
        got = take;
        portEXIT_CRITICAL(&s_pb_lock);

        /* Data is back (held or not): close any hole that was open. The gap
         * is measured to ARRIVAL, so a refill hold is not counted as more
         * hole than the network actually left. */
        if (avail > 0) {
            s_pb_empty_since_ms = 0u;
            s_pb_reply_open = true;
            if (s_pb_in_gap) {
                const uint32_t gap = now_ms - s_pb_gap_start_ms;
                s_pb_in_gap = false;
                if (gap <= PB_GAP_REPLY_MS) {
                    atomic_fetch_add(&s_pb_underruns, 1u);
                    s_pb_reply_holes++;
                    if (gap > atomic_load(&s_pb_max_gap_ms)) {
                        atomic_store(&s_pb_max_gap_ms, gap);
                    }
                    ESP_LOGW(TAG, "playback underrun: %u ms hole mid-reply",
                             (unsigned)gap);
                }
            }
        }
        if (hold) {
            vTaskDelay(1);
            continue;
        }
        if (got == 0) {
            if (!s_pb_in_gap && pb_tail_active(now_ms)) {
                /* The ring ran dry while the tail of the last write was still
                 * audible: a hole. Whatever arrives next must rebuild a
                 * bigger lead before playing, so the NEXT hole is absorbed. */
                s_pb_in_gap = true;
                s_pb_gap_start_ms = now_ms;
                s_pb_primed = false;
                s_pb_prime_start_ms = 0u;
                s_pb_prime_target = atomic_load(&s_pb_refill_ms) * 24u;
            } else if (s_pb_in_gap &&
                       now_ms - s_pb_gap_start_ms > PB_GAP_REPLY_MS) {
                s_pb_in_gap = false;    /* silence followed: a reply ended */
                atomic_fetch_add(&s_pb_gaps_ended_by_silence, 1u);
            }
            /* Once the ring has been empty long enough that whatever comes
             * next is a new reply, re-arm the smaller start-of-reply lead. */
            if (s_pb_empty_since_ms == 0u) {
                s_pb_empty_since_ms = now_ms;
            } else if (now_ms - s_pb_empty_since_ms > PB_GAP_REPLY_MS) {
                if (s_pb_reply_open) {           /* once, as the reply ends */
                    s_pb_reply_open = false;
                    pb_adapt_preroll();
                    s_pb_reply_holes = 0u;
                }
                s_pb_primed = false;
                s_pb_prime_start_ms = 0u;
                s_pb_prime_target = atomic_load(&s_pb_preroll_ms) * 24u;
            }
            /* CONFIG_FREERTOS_HZ=100: pdMS_TO_TICKS(5) truncates to zero and
             * merely yields to equal-priority tasks, starving IDLE0 forever. */
            vTaskDelay(1);
            continue;
        }
        atomic_store(&s_feeder_writing, true);
        int wr = esp_codec_dev_write(s_dac, chunk,
                                     (int)(got * sizeof(int16_t)));
        atomic_store(&s_feeder_writing, false);
        if (wr == ESP_CODEC_DEV_OK) {
            /* codec-dev may apply software volume in place, so capture after
             * the successful write to reflect the submitted PCM exactly. */
            tap_write(JR_AUDIO_TAP_PLAYBACK, chunk, got);
        } else {
            atomic_fetch_add(&s_pb_dac_failures, 1u);
            ESP_LOGW(TAG, "DAC write failed: codec rc=%d samples=%u", wr,
                     (unsigned)got);
        }
        /* esp_codec_dev_write paces the feeder against the I2S/DMA path. Do not
         * add a one-tick delay here: at 100 Hz that is 10 ms after every 32 ms
         * of PCM, which guarantees gaps and makes the producer outrun playback.
         * The empty-ring path above still blocks for one tick so IDLE0 runs. */
        atomic_store(&s_playback_tail_until_ms,
                     atomic_load(&s_muted) || wr != ESP_CODEC_DEV_OK
                         ? 0u
                         : (uint32_t)(esp_timer_get_time() / 1000) + 80u);
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
    jr_audio_playback_stats_reset();

    /* Diagnostics are deliberately optional. A low-memory device still gets
     * the complete voice path; only its rolling evidence buffers disappear. */
    (void)diag_taps_init();

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
    /* Keep blocking DAC writes away from the capture/AEC/voice owner on core 1.
     *
     * Priority 19, not the historical 5: the feeder is the REALTIME consumer —
     * if it misses its DMA refill the speaker emits an audible hole that no
     * electrical tap can see (measured 2026-08-27: a 420 ms acoustic gap in
     * the device's own mic pickup while the write-seam PCM was continuous).
     * At prio 5 it lost the core to the Wi-Fi task (prio 23) for whole bursts,
     * made longer by the IRAM-opts-off change. 19 keeps Wi-Fi above it (no
     * net starvation) but everything ordinary below it. Its work per wake is
     * tiny (memcpy + blocking DMA write), so the high priority is cheap.
     *
     * Stack INTERNAL first (fast, no PSRAM-cache-miss jitter in the realtime
     * path), PSRAM fallback only if internal is exhausted at init. */
    BaseType_t feeder_ok = xTaskCreatePinnedToCore(feeder_task, "jr_pb_feed",
                                                   4096, NULL, 19, &s_feeder, 0);
#if defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && defined(CONFIG_SPIRAM) && \
    CONFIG_SPIRAM
    if (feeder_ok != pdPASS) {
        s_feeder = NULL;
        feeder_ok = xTaskCreatePinnedToCoreWithCaps(feeder_task, "jr_pb_feed",
                                                    4096, NULL, 19, &s_feeder,
                                                    0,
                                                    MALLOC_CAP_SPIRAM |
                                                    MALLOC_CAP_8BIT);
    }
#endif
    if (feeder_ok != pdPASS) {
        ESP_LOGE(TAG, "playback feeder task creation failed");
        s_feeder = NULL;
        return ESP_ERR_NO_MEM;
    }

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

void jr_audio_playback_stats(jr_audio_playback_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    const uint32_t low = atomic_load(&s_pb_low_water_samples);
    out->underruns = atomic_load(&s_pb_underruns);
    out->max_gap_ms = atomic_load(&s_pb_max_gap_ms);
    out->low_water_valid = low != UINT32_MAX;
    /* 24 kHz mono: 24 samples per millisecond. */
    out->low_water_ms = low == UINT32_MAX ? 0u : low / 24u;
    out->dac_failures = atomic_load(&s_pb_dac_failures);
    out->replies_ended = atomic_load(&s_pb_gaps_ended_by_silence);
    out->prerolls = atomic_load(&s_pb_prerolls);
    out->preroll_timeouts = atomic_load(&s_pb_preroll_timeouts);
}

void jr_audio_playback_stats_reset(void)
{
    atomic_store(&s_pb_underruns, 0u);
    atomic_store(&s_pb_max_gap_ms, 0u);
    atomic_store(&s_pb_low_water_samples, UINT32_MAX);
    atomic_store(&s_pb_dac_failures, 0u);
    atomic_store(&s_pb_gaps_ended_by_silence, 0u);
    atomic_store(&s_pb_prerolls, 0u);
    atomic_store(&s_pb_preroll_timeouts, 0u);
}

bool jr_audio_playback_pending(void)
{
    bool buffered = false;
    if (s_pb != NULL) {
        portENTER_CRITICAL(&s_pb_lock);
        buffered = pb_count_locked() > 0;
        portEXIT_CRITICAL(&s_pb_lock);
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t tail = atomic_load(&s_playback_tail_until_ms);
    bool tail_active = (int32_t)(tail - now) > 0;
    return buffered || atomic_load(&s_feeder_writing) || tail_active;
}

void jr_audio_dac_unmute(void)
{
    atomic_store(&s_muted, false);
    if (s_dac_open) {
        esp_codec_dev_set_out_mute(s_dac, false);
        speaker_pa_enable();
    }
}

bool jr_audio_dac_muted(void)
{
    return atomic_load(&s_muted);
}

void jr_audio_flush_playback(void)
{
    portENTER_CRITICAL(&s_pb_lock);
    s_pb_head = s_pb_tail = 0;
    portEXIT_CRITICAL(&s_pb_lock);
    atomic_store(&s_playback_tail_until_ms, 0);
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

int jr_audio_mic_db(void)       { return atomic_load(&s_mic_db); }
int jr_audio_ref_db(void)       { return atomic_load(&s_ref_db); }
int jr_audio_out_vol(void)      { return atomic_load(&s_out_vol); }
int jr_audio_speak_mic_db(void) { return atomic_load(&s_mic_speak_db); }

void jr_audio_set_speak_mic_db(int speak_db)
{
    if (speak_db >= 0) {
        atomic_store(&s_mic_speak_db, speak_db);
        apply_in_gains();
    }
}

void jr_audio_set_speaking(bool speaking)
{
    bool was = atomic_exchange(&s_capture_speaking, speaking);
    if (was != speaking) {
        /* Re-apply the PGA on the edge, exactly like v4's gl_set_state: the low
         * SPEAKING gain keeps the echo unclipped for the AEC; LISTENING restores
         * the loud gain for the user's voice. */
        apply_in_gains();
    }
}

static void tap_fill_info_locked(const audio_tap_t *tap,
                                 jr_audio_tap_info_t *out_info)
{
    memset(out_info, 0, sizeof *out_info);
    out_info->sample_rate = tap->sample_rate;
    out_info->available_samples = (uint32_t)tap->count;
    out_info->capacity_samples = (uint32_t)tap->capacity;
    out_info->total_samples = tap->total;
    if (tap->count == 0) {
        return;
    }

    uint32_t peak = 0;
    uint32_t clipped = 0;
    uint64_t sum_sq = 0;
    size_t pos = (tap->head + tap->capacity - tap->count) % tap->capacity;
    for (size_t i = 0; i < tap->count; ++i) {
        int32_t value = tap->samples[pos];
        uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
        if (magnitude > peak) {
            peak = magnitude;
        }
        if (magnitude >= DIAG_CLIP_THRESHOLD) {
            clipped++;
        }
        sum_sq += (uint64_t)((int64_t)value * (int64_t)value);
        pos = (pos + 1U) % tap->capacity;
    }
    /* int16_t cannot represent abs(INT16_MIN); clipping retains that detail. */
    out_info->peak = (int16_t)(peak > 32767U ? 32767U : peak);
    out_info->rms = sqrtf((float)sum_sq / (float)tap->count);
    out_info->clipped_samples = clipped;
}

void jr_audio_diag_reset(void)
{
    if (!atomic_load(&s_taps_ready)) {
        return;
    }
    for (int i = 0; i < JR_AUDIO_TAP_COUNT; ++i) {
        audio_tap_t *tap = &s_taps[i];
        if (tap->lock != NULL &&
            xSemaphoreTake(tap->lock, portMAX_DELAY) == pdTRUE) {
            tap->head = 0;
            tap->count = 0;
            tap->total = 0;
            xSemaphoreGive(tap->lock);
        }
    }
}

esp_err_t jr_audio_diag_get_info(jr_audio_tap_kind_t kind,
                                 jr_audio_tap_info_t *out_info)
{
    if (kind < 0 || kind >= JR_AUDIO_TAP_COUNT || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_taps_ready)) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_tap_t *tap = &s_taps[kind];
    if (tap->lock == NULL ||
        xSemaphoreTake(tap->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    tap_fill_info_locked(tap, out_info);
    xSemaphoreGive(tap->lock);
    return ESP_OK;
}

esp_err_t jr_audio_diag_copy(jr_audio_tap_kind_t kind, int16_t *dst,
                             size_t capacity_samples,
                             jr_audio_tap_info_t *out_info)
{
    if (kind < 0 || kind >= JR_AUDIO_TAP_COUNT || dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_taps_ready)) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_tap_t *tap = &s_taps[kind];
    if (tap->lock == NULL ||
        xSemaphoreTake(tap->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    jr_audio_tap_info_t info;
    tap_fill_info_locked(tap, &info);
    if (out_info != NULL) {
        *out_info = info;
    }
    if (capacity_samples < tap->count) {
        xSemaphoreGive(tap->lock);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t start = (tap->head + tap->capacity - tap->count) % tap->capacity;
    size_t first = tap->capacity - start;
    if (first > tap->count) {
        first = tap->count;
    }
    memcpy(dst, &tap->samples[start], first * sizeof(int16_t));
    size_t second = tap->count - first;
    if (second > 0) {
        memcpy(&dst[first], tap->samples, second * sizeof(int16_t));
    }
    xSemaphoreGive(tap->lock);
    return ESP_OK;
}

esp_err_t jr_audio_play_sweep(uint16_t start_hz, uint16_t end_hz,
                              uint32_t duration_ms, uint8_t level_percent)
{
    if (!s_ready || !s_dac_open || s_pb == NULL || s_feeder == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Never disturb a live response. Recheck after claiming the enqueue lane
     * to close the race with the normal sink writer. */
    if (jr_audio_playback_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_diag_chirp_enqueuing, &expected,
                                        true)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (jr_audio_playback_pending()) {
        atomic_store(&s_diag_chirp_enqueuing, false);
        return ESP_ERR_INVALID_STATE;
    }

    if (duration_ms < 100U) duration_ms = 100U;
    if (duration_ms > 1500U) duration_ms = 1500U;
    if (level_percent < 1U) level_percent = 1U;
    if (level_percent > 30U) level_percent = 30U;
    if (start_hz < 80U)   start_hz = 80U;
    if (start_hz > 8000U) start_hz = 8000U;
    if (end_hz   < 80U)   end_hz   = 80U;
    if (end_hz   > 8000U) end_hz   = 8000U;

    const size_t total = (size_t)GL_RX_SAMPLE_RATE * duration_ms / 1000U;
    portENTER_CRITICAL(&s_pb_lock);
    size_t free_slots = PB_RING_SAMPLES - 1U - pb_count_locked();
    portEXIT_CRITICAL(&s_pb_lock);
    if (total > free_slots) {
        atomic_store(&s_diag_chirp_enqueuing, false);
        return ESP_ERR_NO_MEM;
    }

    int16_t chunk[DIAG_CHIRP_CHUNK];
    const float duration_s = (float)duration_ms / 1000.0f;
    const float sweep_hz_per_s =
        ((float)end_hz - (float)start_hz) / duration_s;
    const float amplitude = 32767.0f * (float)level_percent / 100.0f;
    const size_t fade_samples = GL_RX_SAMPLE_RATE / 100U; /* 10 ms */

    size_t queued = 0;
    while (queued < total) {
        size_t count = total - queued;
        if (count > DIAG_CHIRP_CHUNK) {
            count = DIAG_CHIRP_CHUNK;
        }
        for (size_t i = 0; i < count; ++i) {
            size_t sample_index = queued + i;
            float t = (float)sample_index / (float)GL_RX_SAMPLE_RATE;
            float phase = 2.0f * (float)M_PI *
                          ((float)start_hz * t +
                           0.5f * sweep_hz_per_s * t * t);
            float envelope = 1.0f;
            if (sample_index < fade_samples) {
                envelope = (float)(sample_index + 1U) / (float)fade_samples;
            }
            size_t remaining = total - sample_index;
            if (remaining < fade_samples) {
                float fade_out = (float)remaining / (float)fade_samples;
                if (fade_out < envelope) {
                    envelope = fade_out;
                }
            }
            chunk[i] = (int16_t)(amplitude * envelope * sinf(phase));
        }
        size_t accepted = pb_enqueue(chunk, count, true);
        if (accepted != count) {
            /* Capacity was reserved and normal writers are excluded, so this
             * is defensive rather than expected. Do not release a fragment. */
            portENTER_CRITICAL(&s_pb_lock);
            s_pb_head = s_pb_tail = 0;
            portEXIT_CRITICAL(&s_pb_lock);
            atomic_store(&s_diag_chirp_enqueuing, false);
            return ESP_FAIL;
        }
        queued += count;
    }

    jr_audio_dac_unmute();
    atomic_store(&s_diag_chirp_enqueuing, false);
    return ESP_OK;
}

esp_err_t jr_audio_diag_play_chirp(uint32_t duration_ms, uint8_t level_percent)
{
    /* The original 400 -> 2000 Hz diagnostic sweep, unchanged. Every existing
     * caller (the attention beat, the audio self-test) keeps its exact sound;
     * jr_audio_play_sweep() only exposes the endpoints the body always had. */
    return jr_audio_play_sweep(400U, 2000U, duration_ms, level_percent);
}
