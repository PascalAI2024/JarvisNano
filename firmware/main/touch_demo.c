/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "touch_demo.h"

#include "sdkconfig.h"

#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#if CONFIG_APP_CLAW_CAP_GEMINI_LIVE
#include <stdint.h>
#include <string.h>

#include "cap_gemini_live.h"
#include "emote.h"
#include "esp_err.h"
#include "esp_board_manager.h"
#include "esp_board_manager_includes.h"
#include "esp_heap_caps.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
/* Status-panel data sources. wifi_manager is already in main's REQUIRES;
 * jarvis_pmic is added by apply_status_panel_main_require_patch in bootstrap. */
#include "jarvis_pmic.h"
#include "jarvis_imu.h"
#include "wifi_manager.h"

static const char *TAG = "touch_mon";

/* ui_layer forward decls — the main component does not carry ui_layer's include
 * dir on its compile path (it is not in main's REQUIRES), but ui_layer is in the
 * link graph via app_claw's REQUIRES, so the symbols resolve at link time. Same
 * forward-decl trick http_server_debug_api.c uses to reach cap_gemini_live.
 * Keep in sync with firmware/ui_layer/ui_layer.h. */
bool ui_layer_is_active(void);
int  ui_layer_on_tap(int x, int y);
esp_err_t ui_layer_show_data(const char *title, const char *const *labels,
                             const char *const *values, int n);
esp_err_t ui_layer_dismiss(void);
esp_err_t ui_layer_show_cockpit(void);  /* the Orbital Cockpit HUD viz */
esp_err_t ui_layer_show_menu(const char *const *items, int n);

/* Polls the CST9217 touch panel every 80ms and calls cap_gemini_live_toggle()
 * on each rising edge (finger-down event). The I2C bus is shared with audio
 * codecs and GPIO expander, so >=50ms poll interval is required.
 *
 * INTERACTIVE-UI DISPATCH: when a ui_layer scene owns the panel
 * (ui_layer_is_active()), a confirmed tap is routed to ui_layer_on_tap(x,y) for
 * hit-testing and MUST NOT start/stop a Gemini session — otherwise tapping a
 * choice arc would also toggle voice behind the UI. The debounce + shared-I2C
 * poll cadence is unchanged; only the action taken on a resolved tap differs. */

typedef enum {
    LOCAL_SCENE_SHOWCASE = 0,
    LOCAL_SCENE_LISTEN,
    LOCAL_SCENE_THINK,
    LOCAL_SCENE_SPEAK,
} local_scene_t;

static TaskHandle_t s_local_demo_task;
static volatile local_scene_t s_local_demo_scene = LOCAL_SCENE_SHOWCASE;
static touch_demo_gemini_configured_cb_t s_gemini_configured;

/* Swipe-up sleep: voice stopped + latched "tap to wake" overlay. Tap clears it
 * and wakes. Tracks whether the user explicitly put the device to sleep so a
 * tap on the dim face wakes instead of starting a fresh session by accident. */
static bool s_asleep = false;

/* Track when we opened a demo radial menu so a later tap pick can drive
 * quick actions (status, stop, etc.) without needing labels from ui_layer. */
static bool s_demo_radial_active = false;

/* ---- Hardware Power Moods (IMU + PMIC) ---------------------------------- *
 * Central tiny state. face_down + stable >3s → DREAM (stop voice, alert overlay).
 * Lift (moving or face_up change) → AWAKE (clear, bloom via idle+amp).
 * Shake → dismiss ui_layer or stop voice. Battery low/charge tunes visuals
 * in face amp. Polled lightly via esp_timer (on-demand I2C, no always bus). */
static volatile power_mood_t s_power_mood = POWER_MOOD_AWAKE;
static esp_timer_handle_t s_power_timer = NULL;

/* Bloom ramp counter for wake-from-dream lively amp (synthetic drives rwave
 * briefly on lift so idle breathing feels "picked up", then hands to IMU boost). */
static volatile int s_bloom_ticks = 0;
static uint64_t s_face_down_since_us = 0;
static bool s_last_shake = false;
static char s_last_orient[16] = "unknown";

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

/* ---- Gesture targets (the swipe grammar) --------------------------------- *
 * tap = talk/pick · swipe up = sleep · swipe down = status · swipe side =
 * dismiss a scene · long-press = stop/showcase. The swipe classification lives
 * in the touch loop; these are the actions it dispatches to. */

/* Swipe up: put the assistant to sleep. Stop the voice session and latch the
 * dim "tap to wake" overlay so it is unmistakable that it is NOT listening
 * (the user's #1 confusion). emote_set_alert is latched, so the idle-status
 * write from stop() can't quietly un-dim it. */
static void touch_enter_sleep(void)
{
    if (cap_gemini_live_is_active()) {
        cap_gemini_live_stop();
    }
    emote_set_alert(EMOTE_ALERT_GENERIC, "tap to wake");
    s_asleep = true;
    ESP_LOGI(TAG, "Swipe up: sleep (voice off; tap to wake)");
}

/* Tap while asleep: clear the latched overlay so the live face can paint, then
 * start listening. Clearing MUST precede start() or the latch hides the face. */
static void touch_exit_sleep(void)
{
    emote_clear_alert();
    s_asleep = false;
    if (gemini_live_configured() && !cap_gemini_live_is_active()) {
        cap_gemini_live_start();
    }
    ESP_LOGI(TAG, "Tap: wake from sleep");
}

/* Swipe down: show a live status panel. All sources are read-only and safe from
 * the touch task; ui_layer_show_data posts to the UI task. wifi_manager is a
 * main dep; jarvis_pmic is added to REQUIRES by bootstrap; heap/uptime are core.
 * The value strings are static so they outlive this call (ui_layer copies them
 * on the UI task, but keeping them static avoids any lifetime ambiguity). */
static void touch_show_status_panel(void)
{
    static char v_batt[32], v_wifi[48], v_ip[24], v_up[32], v_clk[24];
    const char *labels[5] = { "Battery", "Wi-Fi", "IP", "Uptime", "Clock" };
    const char *values[5] = { v_batt, v_wifi, v_ip, v_up, v_clk };

    jarvis_battery_t bat;
    if (jarvis_pmic_read_battery(&bat) == ESP_OK) {
        if (bat.present && bat.percent != 0xFF) {
            snprintf(v_batt, sizeof(v_batt), "%u%%%s", (unsigned)bat.percent,
                     bat.charging ? " chg" : (bat.usb_present ? " usb" : ""));
        } else if (bat.usb_present) {
            strlcpy(v_batt, "USB power", sizeof(v_batt));
        } else {
            strlcpy(v_batt, "n/a", sizeof(v_batt));
        }
    } else {
        strlcpy(v_batt, "n/a", sizeof(v_batt));
    }

    wifi_manager_status_t st;
    memset(&st, 0, sizeof(st));
    wifi_manager_get_status(&st);
    if (st.sta_connected) {
        /* SSID + signal come from esp_wifi (the status struct only carries the
         * IP/mode); esp_wifi is already on the link+include path via
         * wifi_manager. Fall back to "online" if the AP record isn't ready. */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(v_wifi, sizeof(v_wifi), "%.24s %ddBm", (const char *)ap.ssid,
                     (int)ap.rssi);
        } else {
            strlcpy(v_wifi, "online", sizeof(v_wifi));
        }
        strlcpy(v_ip, st.sta_ip ? st.sta_ip : "n/a", sizeof(v_ip));
    } else if (st.ap_active) {
        snprintf(v_wifi, sizeof(v_wifi), "AP %.24s", st.ap_ssid ? st.ap_ssid : "");
        strlcpy(v_ip, st.ap_ip ? st.ap_ip : "n/a", sizeof(v_ip));
    } else {
        strlcpy(v_wifi, "offline", sizeof(v_wifi));
        strlcpy(v_ip, "n/a", sizeof(v_ip));
    }

    uint64_t up_s = esp_timer_get_time() / 1000000ULL;
    unsigned uh = (unsigned)(up_s / 3600);
    unsigned um = (unsigned)((up_s % 3600) / 60);
    unsigned us = (unsigned)(up_s % 60);
    if (uh) {
        snprintf(v_up, sizeof(v_up), "%uh %um", uh, um);
    } else {
        snprintf(v_up, sizeof(v_up), "%um %us", um, us);
    }

    unsigned heap_k = (unsigned)(esp_get_free_heap_size() / 1024);
    unsigned ps_k = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    (void)heap_k; (void)ps_k; /* kept for json diags; clock replaces RAM row in panel */

    /* Basic clock for ambient watch / status panel. Prefers seeded wall time
     * (HH:MM) else monotonic uptime as fallback. Pairs with battery rim. */
    time_t tnow = 0;
    struct tm tminfo;
    if (time(&tnow) != (time_t)-1 && localtime_r(&tnow, &tminfo)) {
        snprintf(v_clk, sizeof(v_clk), "%02d:%02d", tminfo.tm_hour, tminfo.tm_min);
    } else {
        uint64_t ups = esp_timer_get_time() / 1000000ULL;
        snprintf(v_clk, sizeof(v_clk), "%02u:%02u", (unsigned)((ups / 3600) % 24), (unsigned)((ups / 60) % 60));
    }

    ui_layer_show_data("Status", labels, values, 5);
    s_demo_radial_active = false;
    ESP_LOGI(TAG, "Swipe down: status panel");
}

/* Power mood poll: on-demand IMU (burst ~40 ms) + PMIC at low rate. Central
 * transitions drive sleep/wake/dismiss exactly as swipe paths. */
static void power_mood_poll(void *arg)
{
    (void)arg;
    jarvis_imu_t imu = {0};
    if (jarvis_imu_read(&imu) != ESP_OK || !imu.present) {
        return;
    }
    jarvis_battery_t bat = {0};
    (void)jarvis_pmic_read_battery(&bat); /* best-effort; influences amp elsewhere */

    /* Wire battery + refresh ambient watch (clock rim + % label) on every poll.
     * Guards inside the emote helpers make this a no-op during voice or DREAM alert. */
    if (bat.present && bat.percent != 0xFF) {
        emote_set_battery_info(bat.percent, bat.charging || bat.usb_present);
    }
    emote_refresh_ambient_watch();

    /* Wake bloom decay: quick high amp on lift, ramp down synthetic so IMU "breathing"
     * boost (team work in bridge) takes over for lively idle. Dim hint on dream path. */
    if (s_bloom_ticks > 0) {
        s_bloom_ticks--;
        if (s_bloom_ticks > 0) {
            emote_face_set_synthetic_amplitude(s_bloom_ticks >= 3 ? 620 : 340);
        } else {
            emote_face_set_synthetic_amplitude(-1);
        }
    }

    bool is_face_down = (imu.orientation && strcmp(imu.orientation, "face_down") == 0);
    bool is_face_up   = (imu.orientation && strstr(imu.orientation, "face_up") != NULL);
    uint64_t now = (uint64_t)esp_timer_get_time();

    if (is_face_down) {
        if (s_face_down_since_us == 0) {
            s_face_down_since_us = now;
            strlcpy(s_last_orient, imu.orientation ? imu.orientation : "face_down", sizeof(s_last_orient));
        }
        uint64_t stable_us = now - s_face_down_since_us;
        if (stable_us > 3000000ULL && s_power_mood != POWER_MOOD_DREAM) {
            s_power_mood = POWER_MOOD_DREAM;
            if (cap_gemini_live_is_active()) {
                cap_gemini_live_stop();
            }
            emote_face_set_synthetic_amplitude(25); /* dim before alert hides face */
            emote_set_alert(EMOTE_ALERT_GENERIC, "dream");
            s_asleep = true;
            ESP_LOGI(TAG, "Power mood → DREAM (face_down stable >3s)");
        }
    } else {
        s_face_down_since_us = 0;
        bool lift = imu.moving || is_face_up;
        if (s_power_mood == POWER_MOOD_DREAM && lift) {
            s_power_mood = POWER_MOOD_AWAKE;
            emote_clear_alert();
            s_asleep = false;
            emote_set_voice_idle(); /* idle face + motion amp = bloom to life */
            /* Quick amp ramp bloom on wake (polish for lively idle when lifted).
             * Synthetic forces high energy briefly; poll will decay it to IMU-driven
             * subtle breathing boost (already in bridge gl_face_amp). */
            emote_face_set_synthetic_amplitude(620);
            s_bloom_ticks = 5;  /* ~2 s decay at 400 ms poll */
            ESP_LOGI(TAG, "Power mood → AWAKE (lift moving=%d orient=%s)", (int)imu.moving,
                     imu.orientation ? imu.orientation : "?");
        }
    }

    /* Shake: one-shot on rising edge → dismiss scene or barge (stop voice) */
    if (imu.shake && !s_last_shake) {
        if (ui_layer_is_active()) {
            ui_layer_dismiss();
            s_demo_radial_active = false;
            ESP_LOGI(TAG, "Power shake → dismiss ui_layer");
        } else if (cap_gemini_live_is_active()) {
            cap_gemini_live_stop();
            ESP_LOGI(TAG, "Power shake → stop Gemini");
        }
    }
    s_last_shake = imu.shake;
    if (imu.orientation) {
        strlcpy(s_last_orient, imu.orientation, sizeof(s_last_orient));
    }
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
     * both structs have esp_lcd_touch_handle_t as their first member — dereference once.
     *
     * Self-heal: the CST9217 probe intermittently fails at board bring-up (I2C
     * timing / power / bus-contention race), leaving device_handle==NULL with the
     * ref_count decremented (esp_board_device.c:137-141). The original loop only
     * READ the handle, so a failed boot spun on NULL forever and touch never came
     * up without a power cycle. Now, every ~3s of NULL we re-run the real init
     * path via esp_board_manager_init_device_by_name(), which re-enters
     * esp_board_device.c:128-142 and re-runs the full probe+reset+config. Cap the
     * re-init attempts so the task can't busy-spin indefinitely on dead hardware;
     * after the cap we fall through to IRQ/voice-only behaviour. */
    const int TOUCH_REINIT_MAX = 20;
    /* ~3s of NULL (6 ticks at the 500ms cadence) before we surface the fault
     * on-screen. The re-init above keeps running — this only tells the user WHY
     * taps do nothing instead of leaving the calm idle face up. Fired once. */
    const int TOUCH_ALERT_AFTER_TICKS = 6;
    int wait_ticks = 0;
    int reinit_attempts = 0;
    bool alert_raised = false;
    while (touch == NULL) {
        void *dev_h = NULL;
        if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH,
                                                &dev_h) == ESP_OK && dev_h) {
            touch = *(esp_lcd_touch_handle_t *)dev_h;
        }
        if (touch == NULL) {
            if ((wait_ticks % 4) == 0) {
                ESP_LOGW(TAG, "Waiting for LCD touch handle");
            }
            /* Every ~3s (6 ticks at the 500ms cadence) re-run device init to
             * recover from a failed boot-time probe. */
            if ((wait_ticks % 6) == 0 && reinit_attempts < TOUCH_REINIT_MAX) {
                esp_err_t r = esp_board_manager_init_device_by_name(
                    ESP_BOARD_DEVICE_NAME_LCD_TOUCH);
                ESP_LOGW(TAG, "Re-init lcd_touch: %s", esp_err_to_name(r));
                reinit_attempts++;
                /* Pick up the freshly-initialised handle immediately. */
                if (r == ESP_OK &&
                    esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH,
                                                        &dev_h) == ESP_OK && dev_h) {
                    touch = *(esp_lcd_touch_handle_t *)dev_h;
                }
            } else if (reinit_attempts >= TOUCH_REINIT_MAX) {
                ESP_LOGE(TAG, "lcd_touch re-init exhausted (%d attempts); "
                              "touch disabled, voice/IRQ only", reinit_attempts);
                if (s_diag_mx && xSemaphoreTake(s_diag_mx, pdMS_TO_TICKS(20)) == pdTRUE) {
                    strlcpy(s_diag.last_action, "touch_init_failed",
                            sizeof(s_diag.last_action));
                    s_diag.last_action_ms = touch_demo_now_ms();
                    xSemaphoreGive(s_diag_mx);
                }
                vTaskDelete(NULL);
            }
        }
        if (touch == NULL) {
            wait_ticks++;
            if (!alert_raised && wait_ticks >= TOUCH_ALERT_AFTER_TICKS) {
                ESP_LOGW(TAG, "Touch still NULL after ~3s; raising on-screen "
                              "alert (re-init continues)");
                emote_set_alert(EMOTE_ALERT_TOUCH, "Touch offline - reboot");
                alert_raised = true;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    /* Acquired (possibly via a late re-init) — drop the alert and resume. */
    if (alert_raised) {
        emote_clear_alert();
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
    uint16_t last_y = 233; /* captured for the ui_layer hit-test (needs x AND y) */
    uint16_t press_x0 = 233;
    uint16_t press_y0 = 233; /* touch-down origin, for swipe vs tap classification */
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
            if (touch_run == 0) {
                /* first poll of this press = the touch-down origin for swipes */
                press_x0 = x[0];
                press_y0 = y[0];
            }
            last_x = x[0];
            last_y = y[0];
            touch_run++;
            release_run = 0;
            if (touch_run >= PRESS_CONFIRM) {
                press_seen = true;
            }
            if (armed && !long_fired && touch_run == LONG_CONFIRM) {
                if (ui_layer_is_active()) {
                    /* A UI scene owns the panel — swallow the long-press so it
                     * neither stops Gemini nor launches the local showcase. The
                     * tap-up path below routes the coordinates to the hit-test. */
                    ESP_LOGI(TAG, "Long press swallowed: ui_layer scene active");
                } else if (cap_gemini_live_is_active()) {
                    ESP_LOGI(TAG, "Long press: stopping Gemini Live");
                    cap_gemini_live_stop();
                } else {
                    ESP_LOGI(TAG, "Long press: Orbital Cockpit HUD (live IMU + sensors on round bezel)");
                    ui_layer_show_cockpit();  /* the great creative data viz + animation layer */
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
                /* Classify the released gesture: a tap (little travel) vs a swipe
                 * (>= SWIPE_MIN px from the touch-down origin). Touch reads ~15%
                 * compressed, so 60 raw px is a clear, deliberate swipe well
                 * above tap jitter. Direction is the dominant axis (y origin is
                 * top-left, so dy<0 = up). */
                const int SWIPE_MIN = 60;
                int dx = (int)last_x - (int)press_x0;
                int dy = (int)last_y - (int)press_y0;
                int adx = dx < 0 ? -dx : dx;
                int ady = dy < 0 ? -dy : dy;

                if (adx >= SWIPE_MIN || ady >= SWIPE_MIN) {
                    /* SWIPE. While a UI scene owns the panel, any swipe just
                     * dismisses it. On the face: up = sleep, down = status,
                     * sideways = reserved. */
                    if (ui_layer_is_active()) {
                        ui_layer_dismiss();
                        s_demo_radial_active = false;
                        ESP_LOGI(TAG, "Swipe -> dismiss UI scene");
                        touch_diag_set_action("swipe_dismiss", scene);
                    } else if (ady > adx && dy < 0) {
                        touch_enter_sleep();
                        touch_diag_set_action("swipe_up_sleep", scene);
                    } else if (ady > adx && dy > 0) {
                        touch_show_status_panel();
                        touch_diag_set_action("swipe_down_status", scene);
                    } else {
                        /* Wave 3: side swipe on face triggers radial menu for
                         * quick actions (status, stop voice, etc.). Taps on the
                         * dial resolve via ui_layer and we act on the index. */
                        static const char *items[4] = {"Status", "Stop", "Info", "Dismiss"};
                        if (ui_layer_show_menu(items, 4) == ESP_OK) {
                            s_demo_radial_active = true;
                        }
                        ESP_LOGI(TAG, "Swipe side -> radial quick menu");
                        touch_diag_set_action("swipe_side_menu", scene);
                    }
                } else if (ui_layer_is_active()) {
                    /* TAP on an active scene: route to its hit-test, never touch
                     * the Gemini session behind it. */
                    int picked = ui_layer_on_tap((int)last_x, (int)last_y);
                    ESP_LOGI(TAG, "Tap -> ui_layer (x=%u y=%u) picked=%d",
                             (unsigned)last_x, (unsigned)last_y, picked);
                    if (picked >= 0 && s_demo_radial_active) {
                        /* Act on radial picks (demo quick actions). Choice arcs
                         * from Gemini are handled in cap_gemini_live poll path. */
                        s_demo_radial_active = false;
                        if (picked == 0) {
                            touch_show_status_panel();
                        } else if (picked == 1 && cap_gemini_live_is_active()) {
                            cap_gemini_live_stop();
                            ESP_LOGI(TAG, "Radial: stop Gemini");
                        } else if (picked == 3) {
                            ui_layer_dismiss();
                        }
                    } else if (s_demo_radial_active) {
                        s_demo_radial_active = false;
                    }
                    touch_diag_set_action(picked >= 0 ? "tap_ui_pick" : "tap_ui_miss",
                                          scene);
                } else if (s_asleep) {
                    /* TAP while asleep: wake + start listening. */
                    touch_exit_sleep();
                    touch_diag_set_action("tap_wake", scene);
                } else if (cap_gemini_live_is_active()) {
                /* Any tap toggles Gemini when configured — no x-zone routing.
                 * The legacy local-demo dispatch was the cause of "stuck on
                 * green / no response": tapping outside a narrow centre band
                 * played the SPEAK clip with no Gemini audio attached, which
                 * looked identical to a wedged voice session. Long-press still
                 * routes to the local showcase when Gemini is idle (above). */
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

    /* Seed a base wall time so emote's built-in clock_label (and any C time())
     * shows an advancing HH:MM for the ambient watch-face element. Real NTP
     * not required; this makes the rim clock live from boot. */
    struct timeval tv = { .tv_sec = 1735689600, .tv_usec = 0 }; /* 2025-01-01 base */
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "seeded demo wall clock for emote clock_label / ambient watch");
    /* 8192: tap handling posts requests to the Gemini session task instead of
     * running codec teardown + TLS WS sends here (STABILITY_PLAN F4), but the
     * bigger stack stays as belt-and-suspenders — cap_gemini_live_start() can
     * still allocate queues/tasks from this context. */
    esp_err_t t_err = xTaskCreate(touch_monitor_task, "touch_mon", 8192, NULL, 3, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;

    /* Start light periodic power-mood poll for IMU-driven DREAM/AWAKE.
     * 400 ms is responsive for 3 s stable yet gentle on shared I2C. Mood timer
     * independent of touch task so hardware sleep/wake works hands-free. */
    const esp_timer_create_args_t mood_args = {
        .callback = power_mood_poll,
        .name     = "power_mood",
    };
    if (esp_timer_create(&mood_args, &s_power_timer) == ESP_OK && s_power_timer) {
        esp_timer_start_periodic(s_power_timer, 400000);
        ESP_LOGI(TAG, "power mood timer started (400ms)");
    }
    return t_err;
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
                     "\"power_mood\":\"%s\","
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
                     touch_demo_get_power_mood_name(),
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

power_mood_t touch_demo_get_power_mood(void)
{
    return s_power_mood;
}

const char *touch_demo_get_power_mood_name(void)
{
    return (s_power_mood == POWER_MOOD_DREAM) ? "DREAM" : "AWAKE";
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

power_mood_t touch_demo_get_power_mood(void) { return POWER_MOOD_AWAKE; }
const char *touch_demo_get_power_mood_name(void) { return "AWAKE"; }
#endif
