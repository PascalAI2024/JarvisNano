/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_wake — esp-sr WakeNet9 wrapper. See jr_wake.h for the design contract.
 *
 * Data path: voice task -> jr_wake_push -> ring (PSRAM, single producer,
 * single consumer, free-running sample counters) -> worker -> detect().
 * No locks: the producer owns s_head, the worker owns s_tail, and a flush is
 * "tail may not be behind s_flush_pos" plus an epoch bump that voids any
 * detection whose chunk was read before the flush.
 */
#include "jr_wake/jr_wake.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "jr_wake";

/* Core 0 keeps the radio and the playback feeder (prio 19); the voice task
 * (prio 7) owns core 1. Priority 5: under the voice task, so a slow detect can
 * never take a frame slot from the canceller even if the cores were ever
 * rebalanced, and over the display tasks (3 and 4) because a wake hit that
 * waits behind a redraw is a late hit. */
#define JR_WAKE_TASK_CORE      0
#define JR_WAKE_TASK_PRIORITY  5
/* PSRAM stack: internal RAM's largest block is the scarce resource. detect()
 * keeps its tensors on the model's own heap, so the stack only carries the
 * call chain; voiceperf reports the high-water mark to confirm. */
#define JR_WAKE_TASK_STACK     8192
#define JR_WAKE_BENCH_CHUNKS   64

static const esp_wn_iface_t *s_iface;
static model_iface_data_t   *s_model;
static char                  s_model_name[48];
static int                   s_chunk_size;  /* samples per detect() call */
static int16_t              *s_chunk;       /* worker-private copy */
static int16_t              *s_ring;        /* PSRAM */
static uint32_t              s_mask;        /* ring capacity - 1 (power of 2) */
static TaskHandle_t          s_task;

static _Atomic uint32_t s_head;       /* samples ever pushed (producer) */
static _Atomic uint32_t s_tail;       /* samples ever consumed (worker) */
static _Atomic uint32_t s_flush_pos;  /* tail must not be behind this */
static _Atomic uint32_t s_epoch;      /* bumped by every flush */
static _Atomic uint32_t s_dropped;

static _Atomic bool     s_hit_pending;
static _Atomic uint32_t s_hit_ms;
static _Atomic uint32_t s_stale_hits;
static _Atomic uint32_t s_detections;
static _Atomic uint32_t s_last_detect_ms;

static _Atomic uint32_t s_chunks;
static _Atomic uint32_t s_detect_last_us;
static _Atomic uint32_t s_detect_max_us;

static _Atomic bool     s_bench_req;
static _Atomic bool     s_bench_have;
static _Atomic uint32_t s_bench_mean_us;
static _Atomic uint32_t s_bench_max_us;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void bench_run(void)
{
    uint32_t seed = 0x2545F491u;
    uint64_t sum = 0;
    uint32_t max = 0;
    for (int c = 0; c < JR_WAKE_BENCH_CHUNKS; ++c) {
        for (int i = 0; i < s_chunk_size; ++i) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            s_chunk[i] = (int16_t)((int32_t)(seed & 0x1FFFu) - 0x1000);
        }
        const int64_t t0 = esp_timer_get_time();
        (void)s_iface->detect(s_model, s_chunk);
        const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        sum += us;
        if (us > max) max = us;
        vTaskDelay(1);   /* the bench is not allowed to starve IDLE0 either */
    }
    atomic_store(&s_bench_mean_us, (uint32_t)(sum / JR_WAKE_BENCH_CHUNKS));
    atomic_store(&s_bench_max_us, max);
    atomic_store(&s_bench_have, true);
    /* Whatever the noise made the detector think is not a wake word. */
    atomic_store(&s_hit_pending, false);
}

static void wake_task(void *arg)
{
    (void)arg;
    const uint32_t chunk = (uint32_t)s_chunk_size;
    for (;;) {
        /* Sleeps until the voice task pushes or a bench is requested: no
         * timeout, so an idle device wakes this task for nothing. */
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_exchange(&s_bench_req, false)) {
            bench_run();
        }
        for (;;) {
            const uint32_t epoch = atomic_load(&s_epoch);
            uint32_t tail = atomic_load(&s_tail);
            const uint32_t fp = atomic_load(&s_flush_pos);
            if ((int32_t)(fp - tail) > 0) {
                tail = fp;
                atomic_store(&s_tail, tail);
            }
            const uint32_t head = atomic_load(&s_head);
            if (head - tail < chunk) {
                break;
            }
            for (uint32_t i = 0; i < chunk; ++i) {
                s_chunk[i] = s_ring[(tail + i) & s_mask];
            }
            atomic_store(&s_tail, tail + chunk);

            const int64_t t0 = esp_timer_get_time();
            const bool hit = s_iface->detect(s_model, s_chunk) == WAKENET_DETECTED;
            const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
            atomic_store(&s_detect_last_us, us);
            if (us > atomic_load(&s_detect_max_us)) {
                atomic_store(&s_detect_max_us, us);
            }
            atomic_fetch_add(&s_chunks, 1U);

            /* A flush since this chunk was read voids it. Checked before and
             * after publishing so a flush landing in between still wins. */
            if (hit && atomic_load(&s_epoch) == epoch) {
                const uint32_t t = now_ms();
                atomic_store(&s_hit_ms, t);
                atomic_store(&s_hit_pending, true);
                if (atomic_load(&s_epoch) != epoch) {
                    atomic_store(&s_hit_pending, false);
                } else {
                    atomic_fetch_add(&s_detections, 1U);
                    atomic_store(&s_last_detect_ms, t);
                }
            }
        }
    }
}

static BaseType_t wake_task_start(void)
{
    BaseType_t ok;
#if defined(CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION) && \
    CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION && \
    defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && defined(CONFIG_SPIRAM) && \
    CONFIG_SPIRAM
    ok = xTaskCreatePinnedToCoreWithCaps(wake_task, "jr_wake",
                                         JR_WAKE_TASK_STACK, NULL,
                                         JR_WAKE_TASK_PRIORITY, &s_task,
                                         JR_WAKE_TASK_CORE,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) {
        return ok;
    }
    s_task = NULL;
#endif
    return xTaskCreatePinnedToCore(wake_task, "jr_wake", JR_WAKE_TASK_STACK,
                                   NULL, JR_WAKE_TASK_PRIORITY, &s_task,
                                   JR_WAKE_TASK_CORE);
}

esp_err_t jr_wake_init(void)
{
    if (s_model != NULL) {
        return ESP_OK;
    }

    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGW(TAG, "no `model` partition — wake word disabled");
        return ESP_ERR_NOT_FOUND;
    }
    char *name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (name == NULL) {
        ESP_LOGW(TAG, "model partition holds no WakeNet model — wake word disabled");
        return ESP_ERR_NOT_FOUND;
    }

    const esp_wn_iface_t *iface = esp_wn_handle_from_name(name);
    if (iface == NULL) {
        ESP_LOGW(TAG, "no engine for model %s — wake word disabled", name);
        return ESP_ERR_NOT_FOUND;
    }

    /* DET_MODE_90: on a desk device a missed "Jarvis" is a worse failure than
     * a rare false bloom — the false path only re-arms listening, it does not
     * speak or act. */
    model_iface_data_t *model = iface->create(name, DET_MODE_90);
    if (model == NULL) {
        ESP_LOGW(TAG, "WakeNet create failed — wake word disabled");
        return ESP_ERR_NO_MEM;
    }

    int chunk = iface->get_samp_chunksize(model);
    if (chunk <= 0 || chunk > 4096) {
        iface->destroy(model);
        ESP_LOGW(TAG, "implausible chunk size %d — wake word disabled", chunk);
        return ESP_ERR_INVALID_SIZE;
    }
    /* Ring: the next power of two at or above six chunks (4096 samples, 8 KB,
     * for the 512-sample chunk). Both buffers are PSRAM; the worker's private
     * chunk is the only hot one and 1 KB of PSRAM is fine for it. */
    uint32_t cap = 1;
    while (cap < (uint32_t)chunk * 6U) {
        cap <<= 1;
    }
    int16_t *ring = heap_caps_malloc((size_t)cap * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *work = heap_caps_malloc((size_t)chunk * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring == NULL || work == NULL) {
        heap_caps_free(ring);
        heap_caps_free(work);
        iface->destroy(model);
        ESP_LOGW(TAG, "buffer alloc failed — wake word disabled");
        return ESP_ERR_NO_MEM;
    }

    strlcpy(s_model_name, name, sizeof s_model_name);
    s_chunk_size = chunk;
    s_chunk = work;
    s_ring = ring;
    s_mask = cap - 1U;
    s_iface = iface;
    s_model = model;
    if (wake_task_start() != pdPASS) {
        s_model = NULL;
        s_iface = NULL;
        s_ring = NULL;
        s_chunk = NULL;
        heap_caps_free(ring);
        heap_caps_free(work);
        iface->destroy(model);
        ESP_LOGW(TAG, "worker task failed — wake word disabled");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WakeNet online: %s (chunk=%d samples, mode=90, ring=%u)",
             s_model_name, s_chunk_size, (unsigned)cap);
    return ESP_OK;
}

bool jr_wake_ready(void)
{
    return s_model != NULL;
}

const char *jr_wake_model(void)
{
    return s_model != NULL ? s_model_name : "";
}

bool jr_wake_push(const int16_t *samples, size_t n)
{
    if (s_model == NULL || samples == NULL || n == 0) {
        return false;
    }
    const uint32_t head = atomic_load_explicit(&s_head, memory_order_relaxed);
    uint32_t tail = atomic_load(&s_tail);
    const uint32_t fp = atomic_load(&s_flush_pos);
    if ((int32_t)(fp - tail) > 0) {
        tail = fp;   /* flushed audio is already gone, whether or not the
                      * worker has caught up with the jump yet */
    }
    if (s_mask + 1U - (head - tail) < (uint32_t)n) {
        atomic_fetch_add(&s_dropped, 1U);
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        s_ring[(head + (uint32_t)i) & s_mask] = samples[i];
    }
    atomic_store(&s_head, head + (uint32_t)n);
    xTaskNotifyGive(s_task);
    return true;
}

uint32_t jr_wake_take_hit(uint32_t now, uint32_t max_age_ms)
{
    if (s_model == NULL || !atomic_exchange(&s_hit_pending, false)) {
        return 0;
    }
    const uint32_t hit = atomic_load(&s_hit_ms);
    /* Signed: a hit stamped a moment after the caller's own `now` is fresh. */
    if ((int32_t)(now - hit) > (int32_t)max_age_ms) {
        atomic_fetch_add(&s_stale_hits, 1U);
        return 0;
    }
    return hit != 0 ? hit : 1U;
}

void jr_wake_flush(void)
{
    if (s_model == NULL) {
        return;
    }
    atomic_store(&s_flush_pos, atomic_load(&s_head));
    atomic_fetch_add(&s_epoch, 1U);
    atomic_store(&s_hit_pending, false);
}

uint32_t jr_wake_detections(void)       { return atomic_load(&s_detections); }
uint32_t jr_wake_last_detect_ms(void)   { return atomic_load(&s_last_detect_ms); }
uint32_t jr_wake_dropped(void)          { return atomic_load(&s_dropped); }
uint32_t jr_wake_stale_hits(void)       { return atomic_load(&s_stale_hits); }
uint32_t jr_wake_chunks(void)           { return atomic_load(&s_chunks); }
uint32_t jr_wake_detect_last_us(void)   { return atomic_load(&s_detect_last_us); }
uint32_t jr_wake_detect_max_us(void)    { return atomic_load(&s_detect_max_us); }

uint32_t jr_wake_stack_free(void)
{
    return s_task != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0U;
}

void jr_wake_bench_request(void)
{
    if (s_model == NULL) {
        return;
    }
    atomic_store(&s_bench_req, true);
    xTaskNotifyGive(s_task);
}

bool jr_wake_bench_pending(void)
{
    return atomic_load(&s_bench_req);
}

bool jr_wake_bench_result(uint32_t *mean_us, uint32_t *max_us)
{
    if (!atomic_load(&s_bench_have)) {
        return false;
    }
    if (mean_us != NULL) *mean_us = atomic_load(&s_bench_mean_us);
    if (max_us != NULL) *max_us = atomic_load(&s_bench_max_us);
    return true;
}
