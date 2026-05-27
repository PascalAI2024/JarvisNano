/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "touch_demo.h"

#include "sdkconfig.h"

#include <stdio.h>

#if CONFIG_APP_CLAW_CAP_GEMINI_LIVE
#include <stdint.h>
#include <string.h>

#include "cap_gemini_live.h"
#include "emote.h"
#include "esp_err.h"
#include "esp_board_manager_includes.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "touch_mon";

/* Polls the CST9217 touch panel every 80ms and calls cap_gemini_live_toggle()
 * on each rising edge (finger-down event). The I2C bus is shared with audio
 * codecs and GPIO expander, so >=50ms poll interval is required. */

typedef enum {
    LOCAL_SCENE_SHOWCASE = 0,
    LOCAL_SCENE_LISTEN,
    LOCAL_SCENE_THINK,
    LOCAL_SCENE_SPEAK,
} local_scene_t;

static TaskHandle_t s_local_demo_task;
static volatile local_scene_t s_local_demo_scene = LOCAL_SCENE_SHOWCASE;
static touch_demo_gemini_configured_cb_t s_gemini_configured;

typedef struct {
    bool started;
    bool touch_ready;
    bool touching;
    bool local_demo_running;
    uint32_t polls;
    uint32_t touch_edges;
    uint32_t taps;
    uint32_t long_presses;
    uint32_t local_scene_starts;
    uint32_t irq_events;
    uint32_t idle_skips;
    uint32_t read_attempts;
    uint32_t read_ok;
    uint32_t read_errors;
    uint32_t invalid_responses;
    int last_read_err;
    uint16_t last_x;
    uint16_t last_y;
    uint16_t last_strength;
    uint8_t last_point_num;
    uint64_t last_touch_ms;
    uint64_t last_action_ms;
    char last_action[32];
    char last_scene[16];
} touch_demo_diag_t;

static SemaphoreHandle_t s_diag_mx;
static touch_demo_diag_t s_diag = {
    .last_x = 233,
    .last_y = 233,
    .last_action = "boot",
    .last_scene = "idle",
};

static bool gemini_live_configured(void)
{
    return s_gemini_configured && s_gemini_configured();
}

static const char *local_scene_name(local_scene_t scene);

static uint64_t touch_demo_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void touch_diag_set_action(const char *action, local_scene_t scene)
{
    if (!s_diag_mx || xSemaphoreTake(s_diag_mx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    strlcpy(s_diag.last_action, action ? action : "-", sizeof(s_diag.last_action));
    strlcpy(s_diag.last_scene, local_scene_name(scene), sizeof(s_diag.last_scene));
    s_diag.last_action_ms = touch_demo_now_ms();
    s_diag.local_demo_running = s_local_demo_task != NULL;
    xSemaphoreGive(s_diag_mx);
}

static void touch_diag_mark_demo_running(bool running)
{
    if (!s_diag_mx || xSemaphoreTake(s_diag_mx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    s_diag.local_demo_running = running;
    if (!running) {
        strlcpy(s_diag.last_action, "local_done", sizeof(s_diag.last_action));
        s_diag.last_action_ms = touch_demo_now_ms();
    }
    xSemaphoreGive(s_diag_mx);
}

static void local_scene_listen(void)
{
    emote_set_status_detail("touch: listening field");
    emote_set_listening();
    for (int i = 0; i < 30; ++i) {
        float lvl = (float)((i % 10) + 1) / 10.0f;
        cap_gemini_live_set_synthetic_levels(lvl, 0.0f);
        vTaskDelay(pdMS_TO_TICKS(90));
    }
}

static void local_scene_think(void)
{
    emote_set_status_detail("touch: scanner");
    emote_set_thinking();
    for (int i = 0; i < 18; ++i) {
        float lvl = (i % 2) ? 0.35f : 0.0f;
        cap_gemini_live_set_synthetic_levels(lvl, lvl);
        vTaskDelay(pdMS_TO_TICKS(140));
    }
}

static void local_scene_speak(void)
{
    emote_set_status_detail("touch: voice pulse");
    emote_set_speaking();
    for (int i = 0; i < 34; ++i) {
        float lvl = (float)((i % 12) + 1) / 12.0f;
        cap_gemini_live_set_synthetic_levels(0.0f, lvl);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static void local_hardware_demo_task(void *arg)
{
    (void)arg;
    local_scene_t scene = s_local_demo_scene;
    ESP_LOGI(TAG, "Local hardware demo started scene=%d", (int)scene);

    emote_set_connecting();
    vTaskDelay(pdMS_TO_TICKS(scene == LOCAL_SCENE_SHOWCASE ? 700 : 350));

    switch (scene) {
    case LOCAL_SCENE_LISTEN:
        local_scene_listen();
        break;
    case LOCAL_SCENE_THINK:
        local_scene_think();
        break;
    case LOCAL_SCENE_SPEAK:
        local_scene_speak();
        break;
    case LOCAL_SCENE_SHOWCASE:
    default:
        local_scene_listen();
        local_scene_think();
        local_scene_speak();
        break;
    }

    cap_gemini_live_set_synthetic_levels(0.0f, 0.0f);
    emote_set_status_detail("");
    emote_set_voice_idle();
    ESP_LOGI(TAG, "Local hardware demo finished");
    s_local_demo_task = NULL;
    touch_diag_mark_demo_running(false);
    vTaskDelete(NULL);
}

static void local_hardware_demo_start(local_scene_t scene)
{
    if (cap_gemini_live_is_active()) {
        ESP_LOGI(TAG, "Local hardware demo suppressed while Gemini Live is active");
        touch_diag_set_action("local_suppressed", scene);
        return;
    }
    if (s_local_demo_task) {
        ESP_LOGI(TAG, "Local hardware demo already running");
        touch_diag_set_action("local_busy", scene);
        return;
    }
    s_local_demo_scene = scene;
    if (xTaskCreate(local_hardware_demo_task, "hw_demo", 4096, NULL, 3,
                    &s_local_demo_task) != pdPASS) {
        s_local_demo_task = NULL;
        ESP_LOGW(TAG, "Failed to start local hardware demo");
        touch_diag_set_action("local_failed", scene);
        return;
    }
    if (s_diag_mx && xSemaphoreTake(s_diag_mx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_diag.local_scene_starts++;
        s_diag.local_demo_running = true;
        strlcpy(s_diag.last_action, "local_start", sizeof(s_diag.last_action));
        strlcpy(s_diag.last_scene, local_scene_name(scene), sizeof(s_diag.last_scene));
        s_diag.last_action_ms = touch_demo_now_ms();
        xSemaphoreGive(s_diag_mx);
    }
}

static local_scene_t local_scene_from_touch(uint16_t tx)
{
    if (tx < 155) {
        return LOCAL_SCENE_LISTEN;
    }
    if (tx > 310) {
        return LOCAL_SCENE_SPEAK;
    }
    return LOCAL_SCENE_THINK;
}

static const char *local_scene_name(local_scene_t scene)
{
    switch (scene) {
    case LOCAL_SCENE_LISTEN: return "listen";
    case LOCAL_SCENE_THINK: return "think";
    case LOCAL_SCENE_SPEAK: return "speak";
    case LOCAL_SCENE_SHOWCASE:
    default: return "showcase";
    }
}

static bool local_scene_from_name(const char *name, local_scene_t *out_scene)
{
    if (!name || !out_scene) {
        return false;
    }
    if (strcmp(name, "listen") == 0) {
        *out_scene = LOCAL_SCENE_LISTEN;
        return true;
    }
    if (strcmp(name, "think") == 0) {
        *out_scene = LOCAL_SCENE_THINK;
        return true;
    }
    if (strcmp(name, "speak") == 0) {
        *out_scene = LOCAL_SCENE_SPEAK;
        return true;
    }
    if (strcmp(name, "showcase") == 0) {
        *out_scene = LOCAL_SCENE_SHOWCASE;
        return true;
    }
    return false;
}

static void touch_monitor_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Touch monitor task started");
    if (s_diag_mx && xSemaphoreTake(s_diag_mx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_diag.started = true;
        strlcpy(s_diag.last_action, "started", sizeof(s_diag.last_action));
        s_diag.last_action_ms = touch_demo_now_ms();
        xSemaphoreGive(s_diag_mx);
    }
    esp_lcd_touch_handle_t touch = NULL;
    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint16_t strength[1] = {0};
    uint8_t point_num = 0;
    bool was_touching = false;

    /* Wait until board manager has the touch handle ready.
     * Board-manager returns dev_lcd_touch_i2c_handles_t* (or dev_lcd_touch_handles_t*);
     * both structs have esp_lcd_touch_handle_t as their first member — dereference once. */
    int wait_ticks = 0;
    while (touch == NULL) {
        void *dev_h = NULL;
        if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH,
                                                &dev_h) == ESP_OK && dev_h) {
            touch = *(esp_lcd_touch_handle_t *)dev_h;
        }
        if (touch == NULL) {
            if ((wait_ticks++ % 4) == 0) {
                ESP_LOGW(TAG, "Waiting for LCD touch handle");
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGI(TAG, "Touch handle acquired, polling started");
    SemaphoreHandle_t irq_sem = (SemaphoreHandle_t)touch->config.user_data;
    if (irq_sem) {
        while (xSemaphoreTake(irq_sem, 0) == pdTRUE) {
        }
    } else {
        ESP_LOGW(TAG, "Touch IRQ semaphore unavailable; falling back to timed polling");
    }
    if (s_diag_mx && xSemaphoreTake(s_diag_mx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_diag.touch_ready = true;
        strlcpy(s_diag.last_action, "touch_ready", sizeof(s_diag.last_action));
        s_diag.last_action_ms = touch_demo_now_ms();
        xSemaphoreGive(s_diag_mx);
    }

    /* Debounced gesture detection. The CST9217 read flickers (point_num oscillates
     * 0/1 between adjacent polls), so a bare rising-edge fired a flood of taps
     * that thrashed the voice session and left it stuck on "Connecting". Require
     * a CONFIRMED press after a CLEAN sustained release, and disarm until that
     * clean release returns. One genuine finger-down = one tap; single-poll
     * flicker and stuck-on noise are filtered. cap_gemini_live_toggle keeps its
     * own 2s debounce as a backstop.
     *
     * Gestures:
     *   centre tap - Gemini Live toggle when ready, else local scanner
     *   left tap   - local listening field
     *   right tap  - local speaking pulse
     *   long press - local hardware showcase
     */
    const int PRESS_CONFIRM = 1;       /* one poll is enough for a deliberate tap */
    const int RELEASE_CONFIRM = 2;     /* ~160ms stable release to re-arm */
    const int LONG_CONFIRM = 15;       /* ~1.2s stable touch = local hardware demo */
    int touch_run = 0;                 /* consecutive touching polls */
    int release_run = RELEASE_CONFIRM; /* consecutive not-touching polls (armed) */
    bool armed = true;
    bool press_seen = false;
    bool long_fired = false;
    uint16_t last_x = 233;
    (void)was_touching;

    while (1) {
        bool waited_for_irq = false;
        bool read_now = true;
        bool irq_seen = false;
        esp_err_t read_err = ESP_OK;

        if (irq_sem && touch_run == 0 && release_run >= RELEASE_CONFIRM) {
            BaseType_t got_irq = xSemaphoreTake(irq_sem, pdMS_TO_TICKS(80));
            if (got_irq == pdTRUE) {
                irq_seen = true;
            } else {
                read_now = false;
                waited_for_irq = true;
            }
        }

        bool touching = false;
        point_num = 0;
        if (read_now) {
            read_err = esp_lcd_touch_read_data(touch);
            if (read_err == ESP_OK) {
                touching = (esp_lcd_touch_get_coordinates(touch, x, y, strength,
                                                          &point_num, 1) && point_num > 0);
            }
        }
        if (s_diag_mx && xSemaphoreTake(s_diag_mx, pdMS_TO_TICKS(5)) == pdTRUE) {
            s_diag.polls++;
            s_diag.touching = touching;
            s_diag.last_point_num = point_num;
            if (irq_seen) {
                s_diag.irq_events++;
            }
            if (!read_now) {
                s_diag.idle_skips++;
            } else {
                s_diag.read_attempts++;
                s_diag.last_read_err = (int)read_err;
                if (read_err == ESP_OK) {
                    s_diag.read_ok++;
                } else {
                    s_diag.read_errors++;
                    if (read_err == ESP_ERR_INVALID_RESPONSE) {
                        s_diag.invalid_responses++;
                    }
                }
            }
            if (touching) {
                s_diag.last_x = x[0];
                s_diag.last_y = y[0];
                s_diag.last_strength = strength[0];
                s_diag.last_touch_ms = touch_demo_now_ms();
                if (touch_run == 0) {
                    s_diag.touch_edges++;
                }
            }
            xSemaphoreGive(s_diag_mx);
        }
        if (touching) {
            last_x = x[0];
            touch_run++;
            release_run = 0;
            if (touch_run >= PRESS_CONFIRM) {
                press_seen = true;
            }
            if (armed && !long_fired && touch_run == LONG_CONFIRM) {
                if (cap_gemini_live_is_active()) {
                    ESP_LOGI(TAG, "Long press: stopping Gemini Live");
                    cap_gemini_live_stop();
                } else {
                    ESP_LOGI(TAG, "Long press: local hardware showcase");
                    local_hardware_demo_start(LOCAL_SCENE_SHOWCASE);
                }
                if (s_diag_mx && xSemaphoreTake(s_diag_mx, pdMS_TO_TICKS(20)) == pdTRUE) {
                    s_diag.long_presses++;
                    strlcpy(s_diag.last_action, "long_press", sizeof(s_diag.last_action));
                    strlcpy(s_diag.last_scene, local_scene_name(LOCAL_SCENE_SHOWCASE), sizeof(s_diag.last_scene));
                    s_diag.last_action_ms = touch_demo_now_ms();
                    xSemaphoreGive(s_diag_mx);
                }
                long_fired = true;
                armed = false;
            }
        } else {
            bool just_released = touch_run > 0;
            release_run++;
            if (just_released && armed && press_seen && !long_fired) {
                local_scene_t scene = local_scene_from_touch(last_x);
                /* Any tap toggles Gemini when configured — no x-zone routing.
                 * The legacy local-demo dispatch was the cause of "stuck on
                 * green / no response": tapping outside a narrow centre band
                 * played the SPEAK clip with no Gemini audio attached, which
                 * looked identical to a wedged voice session. Long-press still
                 * routes to the local showcase when Gemini is idle (above). */
                if (cap_gemini_live_is_active()) {
                    /* toggle() sends activityEnd while LISTENING (commit turn),
                     * or stops the session when speaking/thinking. */
                    ESP_LOGI(TAG, "Tap: commit/stop Gemini Live (x=%u)",
                             (unsigned)last_x);
                    cap_gemini_live_toggle();
                    touch_diag_set_action("tap_gemini_commit", scene);
                } else if (gemini_live_configured()) {
                    ESP_LOGI(TAG, "Tap: starting Gemini Live (x=%u)",
                             (unsigned)last_x);
                    cap_gemini_live_toggle();
                    touch_diag_set_action("tap_gemini_on", scene);
                } else {
                    ESP_LOGW(TAG, "Tap ignored: Gemini not configured (x=%u)",
                             (unsigned)last_x);
                    touch_diag_set_action("tap_unconfigured", scene);
                }
                if (s_diag_mx && xSemaphoreTake(s_diag_mx, pdMS_TO_TICKS(20)) == pdTRUE) {
                    s_diag.taps++;
                    xSemaphoreGive(s_diag_mx);
                }
                armed = false;
            }
            touch_run = 0;
            press_seen = false;
            long_fired = false;
        }

        if (release_run >= RELEASE_CONFIRM) {
            armed = true;
        }
        if (!waited_for_irq) {
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }
}

esp_err_t touch_demo_start(touch_demo_gemini_configured_cb_t gemini_configured)
{
    s_gemini_configured = gemini_configured;
    if (!s_diag_mx) {
        s_diag_mx = xSemaphoreCreateMutex();
        if (!s_diag_mx) {
            return ESP_ERR_NO_MEM;
        }
    }
    return xTaskCreate(touch_monitor_task, "touch_mon", 4096, NULL, 3, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t touch_demo_get_diagnostics_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    touch_demo_diag_t snap = {0};
    if (s_diag_mx && xSemaphoreTake(s_diag_mx, pdMS_TO_TICKS(50)) == pdTRUE) {
        snap = s_diag;
        snap.local_demo_running = s_local_demo_task != NULL;
        xSemaphoreGive(s_diag_mx);
    }

    int n = snprintf(out, out_size,
                     "{\"ok\":true,\"started\":%s,\"touch_ready\":%s,"
                     "\"touching\":%s,\"local_demo_running\":%s,"
                     "\"gemini_configured\":%s,\"gemini_active\":%s,"
                     "\"polls\":%lu,\"touch_edges\":%lu,\"taps\":%lu,"
                     "\"long_presses\":%lu,\"local_scene_starts\":%lu,"
                     "\"irq_events\":%lu,\"idle_skips\":%lu,"
                     "\"read_attempts\":%lu,\"read_ok\":%lu,"
                     "\"read_errors\":%lu,\"invalid_responses\":%lu,"
                     "\"last_read_err\":%d,"
                     "\"last_x\":%u,\"last_y\":%u,\"last_strength\":%u,"
                     "\"last_point_num\":%u,\"last_touch_ms\":%llu,"
                     "\"last_action_ms\":%llu,\"last_action\":\"%s\","
                     "\"last_scene\":\"%s\"}",
                     snap.started ? "true" : "false",
                     snap.touch_ready ? "true" : "false",
                     snap.touching ? "true" : "false",
                     snap.local_demo_running ? "true" : "false",
                     gemini_live_configured() ? "true" : "false",
                     cap_gemini_live_is_active() ? "true" : "false",
                     (unsigned long)snap.polls,
                     (unsigned long)snap.touch_edges,
                     (unsigned long)snap.taps,
                     (unsigned long)snap.long_presses,
                     (unsigned long)snap.local_scene_starts,
                     (unsigned long)snap.irq_events,
                     (unsigned long)snap.idle_skips,
                     (unsigned long)snap.read_attempts,
                     (unsigned long)snap.read_ok,
                     (unsigned long)snap.read_errors,
                     (unsigned long)snap.invalid_responses,
                     snap.last_read_err,
                     (unsigned)snap.last_x,
                     (unsigned)snap.last_y,
                     (unsigned)snap.last_strength,
                     (unsigned)snap.last_point_num,
                     (unsigned long long)snap.last_touch_ms,
                     (unsigned long long)snap.last_action_ms,
                     snap.last_action,
                     snap.last_scene);
    if (n < 0 || (size_t)n >= out_size) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t touch_demo_run_scene(const char *scene_name)
{
    local_scene_t scene = LOCAL_SCENE_SHOWCASE;
    if (!local_scene_from_name(scene_name ? scene_name : "showcase", &scene)) {
        return ESP_ERR_INVALID_ARG;
    }
    local_hardware_demo_start(scene);
    ESP_LOGI(TAG, "HTTP touch diagnostic scene=%s", local_scene_name(scene));
    return ESP_OK;
}
#else
esp_err_t touch_demo_start(touch_demo_gemini_configured_cb_t gemini_configured)
{
    (void)gemini_configured;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t touch_demo_get_diagnostics_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, out_size, "{\"ok\":false,\"error\":\"touch not enabled\"}");
    return (n < 0 || (size_t)n >= out_size) ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t touch_demo_run_scene(const char *scene_name)
{
    (void)scene_name;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
