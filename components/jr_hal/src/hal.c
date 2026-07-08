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
/* Phase 0 stub: log the presentation intent. Phase 3 harvests the CO5300 QSPI
 * blit + emote_gfx baked-AAF presenter behind these same callbacks. */

static const char *face_name(jr_face_t f)
{
    switch (f) {
    case JR_FACE_IDLE:      return "IDLE";
    case JR_FACE_LISTENING: return "LISTENING";
    case JR_FACE_THINKING:  return "THINKING";
    case JR_FACE_SPEAKING:  return "SPEAKING";
    case JR_FACE_ERROR:     return "ERROR";
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

/* ============================ Input port ============================== */
/* Phase 0 stub: no events. Phase 4 wires the CST9217 INT-edge ISR ->
 * semaphore -> event queue (hardware.md #9) behind this poll(). */

static bool hal_input_poll(void *ctx, jr_input_event_t *out)
{
    (void)ctx;
    (void)out;
    return false;
}

jr_input_t jr_hal_input(void)
{
    jr_input_t i;
    i.ctx = NULL;
    i.poll = hal_input_poll;
    return i;
}
