/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * power.c — CPU gears, deep sleep and the way back.
 *
 * The mood ladder names the state; this file is the mechanism under it:
 * the 240/160 MHz gears, the RTC-memory record that survives a sleep, the
 * wake sources, and the probation guard that keeps a sleep from rolling an
 * update back. Split out of main.c on 2026-09-02 with no behavior change.
 */
#include "app.h"

static const char *TAG = "jarvis_v5";


/* ---- deep sleep when not in use ------------------------------------------
 *
 * The owner: "if not being used it should deep sleep". The rest ladder names
 * the state (DREAM); this is the mechanism under it. Ten minutes into DREAM on
 * battery — never on USB, never mid-update, never with a companion in — the
 * chip sleeps. Three ways back: the QMI8658's own motion engine on INT1
 * (GPIO21: lift it), the CST9217 touch line (GPIO11: tap it), and a timer as
 * the guaranteed road home if both lines are ever wrong. What survives the
 * sleep lives in RTC memory: whether the mic was live, so a lifted device
 * comes back listening instead of asking to be held. */
#define SLEEP_WAKE_IMU_GPIO    GPIO_NUM_21
#define SLEEP_WAKE_TOUCH_GPIO  GPIO_NUM_11
#define SLEEP_WOM_MG           100U
#define SLEEP_ARMED_TIMER      4U
RTC_DATA_ATTR uint32_t s_rtc_sleep_magic;
RTC_DATA_ATTR uint32_t s_rtc_sleeps;
RTC_DATA_ATTR uint8_t  s_rtc_was_listening;
RTC_DATA_ATTR uint8_t  s_rtc_armed;

/* ---- CPU gears ------------------------------------------------------------
 *
 * The owner: "are we optimizing the megahertz for battery?" We were not: 240
 * from boot to deep sleep. Now the mood picks a gear, max pinned to min so
 * nothing scales underneath a running peripheral and light sleep stays out.
 * LIVE (240) whenever anything is happening — voice armed, an update, a
 * lease, USB present. REST_MHZ once the session is closed on the cell. */
_Atomic int s_cpu_mhz;
_Atomic int s_cpu_force;   /* bench: /api/debug/gain?cpu=160; 0 = auto */
_Atomic bool s_power_off_req;   /* PWR long, or /api/debug/sleep?off=1 */

void cpu_gear_set(int mhz)
{
    if (atomic_load(&s_cpu_mhz) == mhz) {
        return;
    }
    esp_pm_config_t cfg = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,
    };
    const esp_err_t err = esp_pm_configure(&cfg);
    if (err == ESP_OK) {
        atomic_store(&s_cpu_mhz, mhz);
        ESP_LOGI(TAG, "cpu gear: %d MHz", mhz);
    } else {
        ESP_LOGW(TAG, "cpu gear %d MHz refused: %s", mhz, esp_err_to_name(err));
    }
}
_Atomic bool     s_sleep_force;
_Atomic uint32_t s_sleep_timer_s;
int              s_boot_wake_cause;
const char *wake_cause_name(int cause)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0:  return "lift";
    case ESP_SLEEP_WAKEUP_EXT1:  return "touch";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    default:                     return "power";
    }
}

/* Does not return. Each wake source is armed only if its line is quiet right
 * now: a line already at its wake level would bring the chip straight back,
 * and a boot loop is worse than a missing wake — the timer is always armed. */
bool image_in_probation(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    return running != NULL &&
           esp_ota_get_state_partition(running, &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

void enter_deep_sleep(const char *why, uint32_t timer_s)
{
    /* A deep sleep is a reboot to the bootloader, and a freshly flashed image
     * that has not been confirmed yet is ROLLED BACK by that reboot. Learned
     * the hard way: a forced sleep at 40 s uptime woke on the old firmware.
     * The real path waits three minutes (validation is at 45 s); this is the
     * guard for everything else. */
    if (image_in_probation()) {
        ESP_LOGW(TAG, "deep sleep refused: image in probation");
        atomic_store(&s_sleep_force, false);
        return;
    }
    ESP_LOGI(TAG, "deep sleep: %s (timer %u s)", why, (unsigned)timer_s);
    s_rtc_sleep_magic = SLEEP_RTC_MAGIC;
    s_rtc_sleeps++;
    s_rtc_was_listening = !atomic_load(&s_voice_privacy_paused);
    s_rtc_armed = SLEEP_ARMED_TIMER;
    jr_display_caption_set("SLEEPING - LIFT TO WAKE");
    vTaskDelay(pdMS_TO_TICKS(1200));

    /* The sampler must leave the bus before the wake engine is written. */
    jr_imu_stop();
    vTaskDelay(pdMS_TO_TICKS(80));
    const bool lift = jr_imu_arm_wake_on_motion(SLEEP_WOM_MG) == ESP_OK;
    if (!lift) {
        s_rtc_armed |= SLEEP_LIFT_ARM_FAILED;
    }

    jr_display_panel_off_request();
    for (int i = 0; i < 30 && !jr_display_panel_is_off(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (lift) {
        rtc_gpio_init(SLEEP_WAKE_IMU_GPIO);
        rtc_gpio_set_direction(SLEEP_WAKE_IMU_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_dis(SLEEP_WAKE_IMU_GPIO);
        rtc_gpio_pulldown_en(SLEEP_WAKE_IMU_GPIO);
        vTaskDelay(pdMS_TO_TICKS(5));
        if (rtc_gpio_get_level(SLEEP_WAKE_IMU_GPIO) == 0) {
            if (esp_sleep_enable_ext0_wakeup(SLEEP_WAKE_IMU_GPIO, 1) == ESP_OK) {
                s_rtc_armed |= SLEEP_ARMED_LIFT;
            }
        } else {
            s_rtc_armed |= SLEEP_LIFT_LINE_HIGH;
            ESP_LOGW(TAG, "deep sleep: INT1 already high, no lift wake");
        }
    }
    rtc_gpio_init(SLEEP_WAKE_TOUCH_GPIO);
    rtc_gpio_set_direction(SLEEP_WAKE_TOUCH_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(SLEEP_WAKE_TOUCH_GPIO);
    rtc_gpio_pullup_en(SLEEP_WAKE_TOUCH_GPIO);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (rtc_gpio_get_level(SLEEP_WAKE_TOUCH_GPIO) != 0) {
        if (esp_sleep_enable_ext1_wakeup_io(1ULL << SLEEP_WAKE_TOUCH_GPIO,
                                            ESP_EXT1_WAKEUP_ANY_LOW) == ESP_OK) {
            s_rtc_armed |= SLEEP_ARMED_TOUCH;
        }
    } else {
        ESP_LOGW(TAG, "deep sleep: touch INT already low, no touch wake");
    }
    (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    (void)esp_sleep_enable_timer_wakeup((uint64_t)timer_s * 1000000ULL);
    ESP_LOGI(TAG, "deep sleep: armed lift=%d touch=%d timer=%u s",
             (s_rtc_armed & SLEEP_ARMED_LIFT) != 0U,
             (s_rtc_armed & SLEEP_ARMED_TOUCH) != 0U, (unsigned)timer_s);
    esp_deep_sleep_start();
}
