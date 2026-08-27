/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_wake — esp-sr WakeNet9 wrapper. See jr_wake.h for the design contract.
 */
#include "jr_wake/jr_wake.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "jr_wake";

static const esp_wn_iface_t *s_iface;
static model_iface_data_t   *s_model;
static char                  s_model_name[48];
static int16_t              *s_chunk;      /* model-native chunk accumulator */
static int                   s_chunk_size; /* samples per detect() call */
static int                   s_filled;
static _Atomic uint32_t      s_detections;
static _Atomic uint32_t      s_last_detect_ms;

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
    /* Small and hot: keep the accumulator internal, fall back to PSRAM. */
    int16_t *buf = heap_caps_malloc((size_t)chunk * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc((size_t)chunk * sizeof(int16_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        iface->destroy(model);
        ESP_LOGW(TAG, "chunk alloc failed — wake word disabled");
        return ESP_ERR_NO_MEM;
    }

    strlcpy(s_model_name, name, sizeof s_model_name);
    s_chunk_size = chunk;
    s_chunk = buf;
    s_filled = 0;
    s_iface = iface;
    s_model = model;
    ESP_LOGI(TAG, "WakeNet online: %s (chunk=%d samples, mode=90)",
             s_model_name, s_chunk_size);
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

bool jr_wake_feed(const int16_t *samples, size_t n)
{
    if (s_model == NULL || samples == NULL) {
        return false;
    }
    bool detected = false;
    while (n > 0) {
        size_t want = (size_t)(s_chunk_size - s_filled);
        size_t take = n < want ? n : want;
        memcpy(s_chunk + s_filled, samples, take * sizeof(int16_t));
        s_filled += (int)take;
        samples += take;
        n -= take;
        if (s_filled == s_chunk_size) {
            s_filled = 0;
            if (s_iface->detect(s_model, s_chunk) == WAKENET_DETECTED) {
                detected = true;
                atomic_fetch_add(&s_detections, 1U);
                atomic_store(&s_last_detect_ms,
                             (uint32_t)(esp_timer_get_time() / 1000));
            }
        }
    }
    return detected;
}

uint32_t jr_wake_detections(void)
{
    return atomic_load(&s_detections);
}

uint32_t jr_wake_last_detect_ms(void)
{
    return atomic_load(&s_last_detect_ms);
}
