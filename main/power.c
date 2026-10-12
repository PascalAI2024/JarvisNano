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
#ifdef JR_POWER_HOST_TEST
#include "power_app.h"
#else
#include "app.h"
#endif

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
/* Two nominal 1 Hz cache publications. A failed/delayed sampler is not proof
 * USB is absent. This bounds publication age, not the earlier STATUS1 read. */
#define SLEEP_POWER_MAX_AGE_MS 2000U
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

static bool sleep_usb_absence_known(void)
{
    jr_power_t power = {0};
    const esp_err_t err = jr_power_read(&power); /* cache only; no caller I2C */
    if (err != ESP_OK || power.sample_seq == 0U ||
        power.age_ms > SLEEP_POWER_MAX_AGE_MS || power.usb_present) {
        ESP_LOGW(TAG, "deep sleep refused: USB/power cache err=%s seq=%u age=%u USB=%d",
                 esp_err_to_name(err), (unsigned)power.sample_seq,
                 (unsigned)power.age_ms, power.usb_present);
        return false;
    }
    return true;
}

static bool sleep_owner_active(void)
{
    const bool updating = atomic_load(&s_ota_active);
    const bool leased = operator_lease_active(
        (uint32_t)(esp_timer_get_time() / 1000));
    if (updating || leased) {
        ESP_LOGW(TAG, "deep sleep refused: OTA=%d operator lease=%d",
                 updating, leased);
        return true;
    }
    return false;
}

static void sleep_entry_cancel(bool restore_panel)
{
    const esp_err_t err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep timer cleanup: %s", esp_err_to_name(err));
    }
    if (restore_panel) {
        /* Only gfx may touch the panel. This request supersedes a pending or
         * completed off; timeout keeps us awake, without claiming recovery. */
        jr_display_panel_on_request();
        for (int i = 0; i < 30 && !jr_display_panel_is_on(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!jr_display_panel_is_on()) {
            ESP_LOGW(TAG, "deep sleep refused: panel recovery unconfirmed");
        }
    }
    atomic_store(&s_sleep_force, false);
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
    /* Applies to forced entry too, including an OTA's borrowed raw lease. */
    if (sleep_owner_active()) {
        atomic_store(&s_sleep_force, false);
        return;
    }
    /* The USB rule applies even to the forced debug path. */
    if (!sleep_usb_absence_known()) {
        atomic_store(&s_sleep_force, false);
        return;
    }
    const esp_err_t timer_err =
        esp_sleep_enable_timer_wakeup((uint64_t)timer_s * 1000000ULL);
    if (timer_err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep refused: timer arm: %s",
                 esp_err_to_name(timer_err));
        sleep_entry_cancel(false);
        return;
    }
    ESP_LOGI(TAG, "deep sleep: %s (timer %u s)", why, (unsigned)timer_s);
    jr_display_caption_set("SLEEPING - LIFT TO WAKE");
    vTaskDelay(pdMS_TO_TICKS(1200));

    jr_display_panel_off_request();
    for (int i = 0; i < 30 && !jr_display_panel_is_off(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!jr_display_panel_is_off()) {
        ESP_LOGW(TAG, "deep sleep refused: panel off unconfirmed");
        sleep_entry_cancel(true);
        return;
    }
    /* Claims/renewals publish under this same lock. Retain it through sleep:
     * an unlocked final check would still race claims during audio/IMU work.
     * No lock is held across the caption or asynchronous panel hand-off. */
    if (s_brain_lock == NULL ||
        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "deep sleep refused: ownership admission unavailable");
        sleep_entry_cancel(true);
        return;
    }
    if (sleep_owner_active()) {
        xSemaphoreGive(s_brain_lock);
        sleep_entry_cancel(true);
        return;
    }
    /* Final cache observation before irreversible audio/IMU preparation.
     * A later physical plug can still fall between this observation and
     * sleep; the cache cannot prove instantaneous hardware absence. */
    if (!sleep_usb_absence_known()) {
        xSemaphoreGive(s_brain_lock);
        sleep_entry_cancel(true);
        return;
    }
    /* Deep sleep stops the S3, not the external codecs or speaker amp.
     * Successful quiescence latches sleeping: all recoverable refusals must
     * precede it. Its own failure restores audio state and leaves us awake. */
    const esp_err_t audio_err = jr_audio_prepare_sleep();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep refused: audio shutdown: %s",
                 esp_err_to_name(audio_err));
        xSemaphoreGive(s_brain_lock);
        sleep_entry_cancel(true);
        return;
    }
    uint8_t armed = SLEEP_ARMED_TIMER;
    /* The sampler must leave the bus before the wake engine is written. */
    jr_imu_stop();
    vTaskDelay(pdMS_TO_TICKS(80));
    const bool lift = jr_imu_arm_wake_on_motion(SLEEP_WOM_MG) == ESP_OK;
    if (!lift) {
        armed |= SLEEP_LIFT_ARM_FAILED;
    }

    if (lift) {
        rtc_gpio_init(SLEEP_WAKE_IMU_GPIO);
        rtc_gpio_set_direction(SLEEP_WAKE_IMU_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_dis(SLEEP_WAKE_IMU_GPIO);
        rtc_gpio_pulldown_en(SLEEP_WAKE_IMU_GPIO);
        vTaskDelay(pdMS_TO_TICKS(5));
        if (rtc_gpio_get_level(SLEEP_WAKE_IMU_GPIO) == 0) {
            if (esp_sleep_enable_ext0_wakeup(SLEEP_WAKE_IMU_GPIO, 1) == ESP_OK) {
                armed |= SLEEP_ARMED_LIFT;
            }
        } else {
            armed |= SLEEP_LIFT_LINE_HIGH;
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
            armed |= SLEEP_ARMED_TOUCH;
        }
    } else {
        ESP_LOGW(TAG, "deep sleep: touch INT already low, no touch wake");
    }
    (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    ESP_LOGI(TAG, "deep sleep: armed lift=%d touch=%d timer=%u s",
             (armed & SLEEP_ARMED_LIFT) != 0U,
             (armed & SLEEP_ARMED_TOUCH) != 0U, (unsigned)timer_s);
    /* A refused entry must not overwrite the previous RTC sleep history. */
    s_rtc_sleep_magic = SLEEP_RTC_MAGIC;
    s_rtc_sleeps++;
    s_rtc_was_listening = !atomic_load(&s_voice_privacy_paused);
    s_rtc_armed = armed;
    esp_deep_sleep_start();
}
