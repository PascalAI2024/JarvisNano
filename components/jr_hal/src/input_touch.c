/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CST9217 -> jr_input adapter.
 *
 * The board factory installs the GPIO ISR and stores its binary semaphore in
 * esp_lcd_touch_config_t::user_data. This worker is the sole touch-driver
 * owner: it waits on that semaphore while idle, polls only while a gesture is
 * active, debounces release flicker, and sends discrete events to a bounded
 * queue consumed by the app task.
 */
#include "jr_hal/hal.h"

#include <inttypes.h>
#include <stdint.h>

#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_lcd_touch.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TOUCH_EVENT_QUEUE_LEN       8
/* Measured peak 2,132 B; 5120 leaves 2,988 B margin. Was 8192. */
#define TOUCH_TASK_STACK_BYTES      5120
#define TOUCH_TASK_PRIORITY         3
#define TOUCH_ACTIVE_POLL_MS        40
#define TOUCH_FALLBACK_POLL_MS      80
#define TOUCH_RETRY_INITIAL_MS      500
#define TOUCH_RETRY_MAX_MS          30000
#define TOUCH_PRESS_CONFIRM_SAMPLES 2
#define TOUCH_RELEASE_SAMPLES       2
#define TOUCH_LONG_PRESS_MS         850
/* Must equal TOUCH_SWIPE_MIN_TRAVEL_PX. The two thresholds are the two halves
 * of one decision: the classifier tries swipe FIRST and falls back to tap, so
 * any gap between them is a band where a real contact produces NO EVENT AT
 * ALL. At 30 vs 42 that band was 12 px wide and it swallowed gestures in
 * silence — the exact "no gesture attempt ends in silent nothing" failure in
 * PLAN.md wave 3 (W3). Keep them equal; if the swipe threshold moves, this
 * moves with it. */
#define TOUCH_TAP_SLOP_PX           42
#define TOUCH_HOLD_SLOP_PX          48
#define TOUCH_SWIPE_MIN_TRAVEL_PX   42
#define TOUCH_SWIPE_DOMINANCE_PCT   125
#define TOUCH_TOP_EDGE_MAX_Y        72
#define TOUCH_SHADE_MIN_TRAVEL_PX   70
#define TOUCH_SHADE_DOMINANCE_PCT   135

typedef enum {
    INPUT_NOT_STARTED = 0,
    INPUT_STARTING,
    INPUT_RUNNING,
} input_start_state_t;

typedef struct {
    QueueHandle_t queue;
    TaskHandle_t task;
    input_start_state_t start_state;
    uint32_t dropped_events;
} touch_input_ctx_t;

static const char *TAG = "jr_hal_touch";
static touch_input_ctx_t s_input;
static portMUX_TYPE s_input_lock = portMUX_INITIALIZER_UNLOCKED;

static uint16_t touch_abs_delta(int16_t value)
{
    return (uint16_t)(value < 0 ? -value : value);
}

static void touch_emit(touch_input_ctx_t *ctx, jr_input_kind_t kind,
                       uint16_t start_x, uint16_t start_y,
                       uint16_t end_x, uint16_t end_y,
                       uint32_t duration_ms,
                       jr_input_direction_t direction, uint8_t flags)
{
    jr_input_event_t event = {
        .kind = kind,
        .x = end_x,
        .y = end_y,
        .start_x = start_x,
        .start_y = start_y,
        .end_x = end_x,
        .end_y = end_y,
        .delta_x = (int16_t)end_x - (int16_t)start_x,
        .delta_y = (int16_t)end_y - (int16_t)start_y,
        .duration_ms = duration_ms,
        .emitted_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .direction = direction,
        .flags = flags,
    };

    if (xQueueSend(ctx->queue, &event, 0) == pdTRUE) {
        return;
    }

    /* Prefer the latest physical intent if the app has stopped draining the
     * queue. There is one producer and one consumer, so dropping one oldest
     * event and retrying is deterministic and remains strictly bounded. */
    jr_input_event_t discarded;
    (void)xQueueReceive(ctx->queue, &discarded, 0);
    if (xQueueSend(ctx->queue, &event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "input queue remained full after dropping oldest event");
    }

    ctx->dropped_events++;
    if (ctx->dropped_events == 1 || (ctx->dropped_events % 16U) == 0U) {
        ESP_LOGW(TAG, "input queue overflow; dropped=%" PRIu32,
                 ctx->dropped_events);
    }
}

static bool touch_moved_beyond_tap_slop(uint16_t start_x, uint16_t start_y,
                                        uint16_t end_x, uint16_t end_y)
{
    int16_t delta_x = (int16_t)end_x - (int16_t)start_x;
    int16_t delta_y = (int16_t)end_y - (int16_t)start_y;
    return touch_abs_delta(delta_x) > TOUCH_TAP_SLOP_PX ||
           touch_abs_delta(delta_y) > TOUCH_TAP_SLOP_PX;
}

static bool touch_moved_beyond_hold_slop(uint16_t start_x, uint16_t start_y,
                                         uint16_t end_x, uint16_t end_y)
{
    int16_t delta_x = (int16_t)end_x - (int16_t)start_x;
    int16_t delta_y = (int16_t)end_y - (int16_t)start_y;
    return touch_abs_delta(delta_x) > TOUCH_HOLD_SLOP_PX ||
           touch_abs_delta(delta_y) > TOUCH_HOLD_SLOP_PX;
}


static bool touch_classify_swipe(uint16_t start_x, uint16_t start_y,
                                 uint16_t end_x, uint16_t end_y,
                                 jr_input_direction_t *direction,
                                 uint8_t *flags)
{
    int16_t delta_x = (int16_t)end_x - (int16_t)start_x;
    int16_t delta_y = (int16_t)end_y - (int16_t)start_y;
    uint16_t abs_x = touch_abs_delta(delta_x);
    uint16_t abs_y = touch_abs_delta(delta_y);

    *direction = JR_INPUT_DIRECTION_NONE;
    *flags = 0;

    /* The system shade gets a deliberately generous, high-confidence gate:
     * it must begin at the top edge, travel down at least 70 px, and be much
     * more vertical than horizontal. This rule wins over the general swipe
     * classifier so the shell has a stable gesture contract. */
    if (start_y < TOUCH_TOP_EDGE_MAX_Y &&
        delta_y >= TOUCH_SHADE_MIN_TRAVEL_PX &&
        (uint32_t)abs_y * 100U >=
            (uint32_t)abs_x * TOUCH_SHADE_DOMINANCE_PCT) {
        *direction = JR_INPUT_DIRECTION_DOWN;
        *flags = JR_INPUT_FLAG_TOP_EDGE;
        return true;
    }

    if (abs_x < TOUCH_SWIPE_MIN_TRAVEL_PX &&
        abs_y < TOUCH_SWIPE_MIN_TRAVEL_PX) {
        return false;
    }

    /* Larger axis wins, no dominance gate. The old 125% dominance rule sent
     * any flick with natural drift (dx 80 / dy 70) to NOWHERE — the owner
     * read that silence as "swipes don't work" (2026-08-27). Every real
     * swipe now classifies; a 45-degree stroke goes to its larger axis, and
     * every downstream action is benign, so a misread costs a glance, not a
     * state. The shade keeps its own high-confidence gate above. */
    if (abs_x >= abs_y) {
        *direction = delta_x < 0 ? JR_INPUT_DIRECTION_LEFT
                                 : JR_INPUT_DIRECTION_RIGHT;
    } else {
        *direction = delta_y < 0 ? JR_INPUT_DIRECTION_UP
                                 : JR_INPUT_DIRECTION_DOWN;
    }
    return true;
}

static esp_lcd_touch_handle_t touch_resolve_board_handle(void)
{
    void *board_handle = NULL;
    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH,
                                            &board_handle) != ESP_OK ||
        board_handle == NULL) {
        return NULL;
    }

    /* Both board-manager handle shapes put esp_lcd_touch_handle_t first:
     *
     *   dev_lcd_touch_handles_t      (generic type=lcd_touch, sub_type=i2c)
     *   dev_lcd_touch_i2c_handles_t  (legacy type=lcd_touch_i2c)
     *
     * Deliberately avoid including either wrapper header so this adapter stays
     * compatible while generated board metadata migrates to the generic type. */
    return *(esp_lcd_touch_handle_t *)board_handle;
}

static esp_lcd_touch_handle_t touch_acquire_with_retry(void)
{
    uint32_t retry_ms = TOUCH_RETRY_INITIAL_MS;
    uint32_t attempts = 0;

    for (;;) {
        esp_lcd_touch_handle_t touch = touch_resolve_board_handle();
        if (touch != NULL) {
            if (attempts > 0) {
                ESP_LOGI(TAG, "touch recovered after %" PRIu32 " retries",
                         attempts);
            }
            return touch;
        }

        attempts++;
        esp_err_t err = esp_board_manager_init_device_by_name(
            ESP_BOARD_DEVICE_NAME_LCD_TOUCH);
        if (err == ESP_OK) {
            touch = touch_resolve_board_handle();
            if (touch != NULL) {
                ESP_LOGI(TAG, "touch initialized on retry %" PRIu32, attempts);
                return touch;
            }
        }

        /* Touch failure must not take voice or networking down. Retry forever
         * with a capped backoff; log sparsely once the hardware is clearly
         * absent so a detached panel does not become a serial-log generator. */
        if (attempts <= 4U || (attempts % 16U) == 0U) {
            ESP_LOGW(TAG,
                     "touch unavailable (attempt=%" PRIu32 ", err=%s); retry in %" PRIu32 " ms",
                     attempts, esp_err_to_name(err), retry_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(retry_ms));
        retry_ms = (retry_ms < TOUCH_RETRY_MAX_MS / 2U)
                       ? retry_ms * 2U
                       : TOUCH_RETRY_MAX_MS;
    }
}

static bool touch_read_point(esp_lcd_touch_handle_t touch,
                             uint16_t *x, uint16_t *y)
{
    esp_err_t err = esp_lcd_touch_read_data(touch);
    if (err != ESP_OK) {
        return false;
    }

    esp_lcd_touch_point_data_t point = {0};
    uint8_t point_count = 0;
    err = esp_lcd_touch_get_data(touch, &point, &point_count, 1);
    if (err != ESP_OK || point_count == 0) {
        return false;
    }

    *x = point.x;
    *y = point.y;
    return true;
}

static void touch_worker(void *arg)
{
    touch_input_ctx_t *ctx = (touch_input_ctx_t *)arg;
    esp_lcd_touch_handle_t touch = touch_acquire_with_retry();
    SemaphoreHandle_t irq_sem = (SemaphoreHandle_t)touch->config.user_data;

    if (irq_sem != NULL) {
        ESP_LOGI(TAG, "touch ready; IRQ-driven input enabled");
    } else {
        /* The board setup should always provide this. Timed polling is a
         * fail-soft compatibility path, not the normal hardware mode. */
        ESP_LOGW(TAG, "touch IRQ semaphore missing; using timed fallback");
    }

    bool gesture_active = false;
    bool gesture_confirmed = false;
    bool action_sent = false;
    bool moved_beyond_tap = false;
    unsigned press_samples = 0;
    bool moved_beyond_hold = false;
    unsigned release_samples = 0;
    TickType_t press_tick = 0;
    TickType_t last_touch_tick = 0;
    uint16_t first_x = 0;
    uint16_t first_y = 0;
    uint16_t last_x = 0;
    uint16_t last_y = 0;

    for (;;) {
        if (!gesture_active) {
            if (irq_sem != NULL) {
                (void)xSemaphoreTake(irq_sem, portMAX_DELAY);
            } else {
                vTaskDelay(pdMS_TO_TICKS(TOUCH_FALLBACK_POLL_MS));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_ACTIVE_POLL_MS));
        }

        uint16_t x = last_x;
        uint16_t y = last_y;
        bool touching = touch_read_point(touch, &x, &y);

        if (touching) {
            TickType_t touch_tick = xTaskGetTickCount();
            release_samples = 0;
            if (!gesture_active) {
                gesture_active = true;
                gesture_confirmed = false;
                action_sent = false;
                moved_beyond_tap = false;
                press_samples = 0;
                moved_beyond_hold = false;
                press_tick = touch_tick;
                first_x = x;
                first_y = y;
            }

            last_x = x;
            last_y = y;
            last_touch_tick = touch_tick;
            if (!moved_beyond_tap) {
                moved_beyond_tap = touch_moved_beyond_tap_slop(
                    first_x, first_y, last_x, last_y);
            }
            if (!moved_beyond_hold) {
                moved_beyond_hold = touch_moved_beyond_hold_slop(
                    first_x, first_y, last_x, last_y);
            }
            if (press_samples < TOUCH_PRESS_CONFIRM_SAMPLES) {
                press_samples++;
                gesture_confirmed =
                    press_samples >= TOUCH_PRESS_CONFIRM_SAMPLES;
            }

            if (gesture_confirmed && !action_sent &&
                !moved_beyond_hold &&
                (touch_tick - press_tick) >=
                    pdMS_TO_TICKS(TOUCH_LONG_PRESS_MS)) {
                uint32_t duration_ms = (uint32_t)(
                    (touch_tick - press_tick) * portTICK_PERIOD_MS);
                touch_emit(ctx, JR_INPUT_LONG_PRESS,
                           first_x, first_y, last_x, last_y, duration_ms,
                           JR_INPUT_DIRECTION_NONE, 0);
                action_sent = true;
                ESP_LOGI(TAG,
                         "long press end=%u,%u start=%u,%u duration=%" PRIu32 "ms",
                         (unsigned)last_x, (unsigned)last_y,
                         (unsigned)first_x, (unsigned)first_y, duration_ms);
            }
            continue;
        }

        if (!gesture_active) {
            continue;
        }

        /* The CST9217 can sleep immediately on release and reject the idle I2C
         * read. A failed read is therefore release evidence, but two adjacent
         * samples are required so one transient bus failure cannot synthesize
         * a gesture edge. */
        release_samples++;
        if (release_samples < TOUCH_RELEASE_SAMPLES) {
            continue;
        }

        if (gesture_confirmed && !action_sent) {
            uint32_t duration_ms = (uint32_t)(
                (last_touch_tick - press_tick) * portTICK_PERIOD_MS);
            jr_input_direction_t direction = JR_INPUT_DIRECTION_NONE;
            uint8_t flags = 0;

            if (duration_ms >= TOUCH_LONG_PRESS_MS &&
                !moved_beyond_hold) {
                touch_emit(ctx, JR_INPUT_LONG_PRESS,
                           first_x, first_y, last_x, last_y, duration_ms,
                           JR_INPUT_DIRECTION_NONE, 0);
                action_sent = true;
                ESP_LOGI(TAG,
                         "long press release end=%u,%u start=%u,%u duration=%" PRIu32 "ms",
                         (unsigned)last_x, (unsigned)last_y,
                         (unsigned)first_x, (unsigned)first_y, duration_ms);
            } else if (touch_classify_swipe(first_x, first_y, last_x, last_y,
                                            &direction, &flags)) {
                touch_emit(ctx, JR_INPUT_SWIPE,
                           first_x, first_y, last_x, last_y, duration_ms,
                           direction, flags);
                action_sent = true;
                ESP_LOGI(TAG,
                         "swipe dir=%u start=%u,%u end=%u,%u delta=%d,%d duration=%" PRIu32 "ms flags=0x%02x",
                         (unsigned)direction,
                         (unsigned)first_x, (unsigned)first_y,
                         (unsigned)last_x, (unsigned)last_y,
                         (int)((int16_t)last_x - (int16_t)first_x),
                         (int)((int16_t)last_y - (int16_t)first_y),
                         duration_ms, (unsigned)flags);
            } else if (!moved_beyond_tap) {
                touch_emit(ctx, JR_INPUT_TAP,
                           first_x, first_y, last_x, last_y, duration_ms,
                           JR_INPUT_DIRECTION_NONE, 0);
                action_sent = true;
                ESP_LOGI(TAG,
                         "tap end=%u,%u start=%u,%u duration=%" PRIu32 "ms",
                         (unsigned)last_x, (unsigned)last_y,
                         (unsigned)first_x, (unsigned)first_y, duration_ms);
            } else {
                ESP_LOGD(TAG,
                         "gesture ignored: drag was not a directional swipe");
            }
        }

        gesture_active = false;
        gesture_confirmed = false;
        action_sent = false;
        moved_beyond_tap = false;
        press_samples = 0;
        release_samples = 0;
    }
}

static bool hal_input_poll(void *arg, jr_input_event_t *out)
{
    touch_input_ctx_t *ctx = (touch_input_ctx_t *)arg;
    if (ctx == NULL || ctx->queue == NULL || out == NULL) {
        return false;
    }
    return xQueueReceive(ctx->queue, out, 0) == pdTRUE;
}

jr_input_t jr_hal_input(void)
{
    bool should_start = false;

    portENTER_CRITICAL(&s_input_lock);
    if (s_input.start_state == INPUT_NOT_STARTED) {
        s_input.start_state = INPUT_STARTING;
        should_start = true;
    }
    portEXIT_CRITICAL(&s_input_lock);

    if (should_start) {
        QueueHandle_t queue = xQueueCreate(TOUCH_EVENT_QUEUE_LEN,
                                           sizeof(jr_input_event_t));
        BaseType_t task_created = pdFAIL;
        if (queue != NULL) {
            s_input.queue = queue;
#if defined(CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION) && \
    CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION && \
    defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && defined(CONFIG_SPIRAM) && \
    CONFIG_SPIRAM && defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
            /* The ISR only gives the controller semaphore. This worker's
             * stack is task-context I2C/gesture state—never DMA or ISR data—
             * so keep its full 8 KB in PSRAM and leave internal SRAM for
             * codec DMA and hardware AES during live voice uplink. */
            task_created = xTaskCreateWithCaps(
                touch_worker, "jr_touch", TOUCH_TASK_STACK_BYTES, &s_input,
                TOUCH_TASK_PRIORITY, &s_input.task,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
            task_created = xTaskCreate(touch_worker, "jr_touch",
                                       TOUCH_TASK_STACK_BYTES, &s_input,
                                       TOUCH_TASK_PRIORITY, &s_input.task);
#endif
        }

        portENTER_CRITICAL(&s_input_lock);
        if (task_created == pdPASS) {
            s_input.start_state = INPUT_RUNNING;
        } else {
            s_input.queue = NULL;
            s_input.task = NULL;
            s_input.start_state = INPUT_NOT_STARTED;
        }
        portEXIT_CRITICAL(&s_input_lock);

        if (task_created != pdPASS) {
            if (queue != NULL) {
                vQueueDelete(queue);
            }
            ESP_LOGE(TAG, "touch input worker creation failed; input disabled");
        }
    }

    jr_input_t input = {
        .ctx = &s_input,
        .poll = hal_input_poll,
    };
    return input;
}

/* Synthetic events share the bounded queue for behavioral QA, but carry
 * explicit non-physical provenance. Security-sensitive consumers must reject
 * this flag; tests can still exercise navigation and harmless feedback. */
esp_err_t jr_hal_input_inject(const jr_input_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_input.queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    jr_input_event_t injected = *event;
    injected.flags |= JR_INPUT_FLAG_SYNTHETIC;
    injected.emitted_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return xQueueSend(s_input.queue, &injected, 0) == pdTRUE ? ESP_OK
                                                            : ESP_ERR_NO_MEM;
}
