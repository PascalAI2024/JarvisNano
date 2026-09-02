/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_hal/src/hal.c — L0 HAL adapters (Phase 0 skeleton).
 *
 * Includes ESP-IDF headers freely — this is the encapsulation boundary. The
 * pure core on the other side of these port structs includes none of it.
 */
#include "jr_hal/hal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_board_manager.h"

static const char *TAG = "jr_hal";

/* ============================ board bring-up ============================ */

esp_err_t jr_hal_init(void)
{
    ESP_LOGI(TAG, "board init: esp_board_manager_init()");
    esp_err_t err = esp_board_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_board_manager_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "board init OK");
    return ESP_OK;
}

/* ============================ Clock port =============================== */
/* esp_timer_get_time() is monotonic microseconds since boot. */

static uint64_t hal_now_ms(void *ctx)
{
    (void)ctx;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

jr_clock_t jr_hal_clock(void)
{
    jr_clock_t c;
    c.ctx = NULL;
    c.now_ms = hal_now_ms;
    return c;
}

/* ============================ Display port ============================= */
/* The headless FALLBACK presenter — it logs the presentation intent instead of
 * drawing. This is live code, not a leftover: the real CO5300 QSPI + emote_gfx
 * presenter now lives in components/jr_display (jr_display_start), and the
 * composition root prefers it, dropping back to this stub only when that
 * bring-up fails (main.c:4409-4414). Keeping the board bootable and diagnosable
 * without a working panel is the point. */

static const char *face_name(jr_face_t f)
{
    switch (f) {
    case JR_FACE_IDLE:      return "IDLE";
    case JR_FACE_LISTENING: return "LISTENING";
    case JR_FACE_THINKING:  return "THINKING";
    case JR_FACE_SPEAKING:  return "SPEAKING";
    case JR_FACE_ERROR:     return "ERROR";
    case JR_FACE_RESTING:   return "RESTING";
    case JR_FACE_MUTED:     return "MUTED";
    case JR_FACE_LINKING:   return "LINKING";
    default:                return "?";
    }
}

static void hal_display_blank(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "display: blank");
}

static void hal_display_present(void *ctx, jr_face_t face, uint8_t amplitude)
{
    (void)ctx;
    ESP_LOGI(TAG, "display: present face=%s amp=%u", face_name(face), (unsigned)amplitude);
}

jr_display_t jr_hal_display(void)
{
    jr_display_t d;
    d.ctx = NULL;
    d.blank = hal_display_blank;
    d.present = hal_display_present;
    return d;
}
