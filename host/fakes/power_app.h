/* SPDX-License-Identifier: Apache-2.0
 * Hardware seam for compiling the actual main/power.c sleep sequence on the
 * host. This header is selected only by JR_POWER_HOST_TEST; no IDF headers
 * enter the pure component include paths. The test supplies the side effects.
 */
#ifndef HOST_POWER_APP_H
#define HOST_POWER_APP_H
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>

typedef int esp_err_t;
typedef int esp_ota_img_states_t;
typedef struct { int unused; } esp_partition_t;
typedef struct {
    int max_freq_mhz;
    int min_freq_mhz;
    bool light_sleep_enable;
} esp_pm_config_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_OTA_IMG_UNDEFINED 0
#define ESP_OTA_IMG_PENDING_VERIFY 1
#define ESP_OTA_IMG_VALID 2
#define ESP_SLEEP_WAKEUP_EXT0 1
#define ESP_SLEEP_WAKEUP_EXT1 2
#define ESP_SLEEP_WAKEUP_TIMER 3
#define GPIO_NUM_21 21
#define GPIO_NUM_11 11
#define RTC_GPIO_MODE_INPUT_ONLY 1
#define ESP_EXT1_WAKEUP_ANY_LOW 1
#define ESP_PD_DOMAIN_RTC_PERIPH 1
#define ESP_PD_OPTION_ON 1
#define RTC_DATA_ATTR
#define pdMS_TO_TICKS(ms) (ms)
#define SLEEP_RTC_MAGIC 0x534C5031u
#define SLEEP_ARMED_LIFT 1U
#define SLEEP_ARMED_TOUCH 2U
#define SLEEP_LIFT_ARM_FAILED 8U
#define SLEEP_LIFT_LINE_HIGH 16U
static inline void power_fake_log(const char *tag, const char *fmt, ...)
{ (void)tag; (void)fmt; }
#define ESP_LOGI(...) power_fake_log(__VA_ARGS__)
#define ESP_LOGW(...) power_fake_log(__VA_ARGS__)

extern _Atomic bool s_voice_privacy_paused;
esp_err_t esp_pm_configure(const esp_pm_config_t *cfg);
const char *esp_err_to_name(esp_err_t err);
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,
                                     esp_ota_img_states_t *state);
esp_err_t jr_audio_prepare_sleep(void);
void jr_display_caption_set(const char *text);
void vTaskDelay(unsigned ticks);
void jr_imu_stop(void);
esp_err_t jr_imu_arm_wake_on_motion(unsigned mg);
void jr_display_panel_off_request(void);
bool jr_display_panel_is_off(void);
void rtc_gpio_init(int gpio);
void rtc_gpio_set_direction(int gpio, int direction);
void rtc_gpio_pullup_dis(int gpio);
void rtc_gpio_pulldown_en(int gpio);
void rtc_gpio_pulldown_dis(int gpio);
void rtc_gpio_pullup_en(int gpio);
int rtc_gpio_get_level(int gpio);
esp_err_t esp_sleep_enable_ext0_wakeup(int gpio, int level);
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask, int mode);
esp_err_t esp_sleep_pd_config(int domain, int option);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t usec);
void esp_deep_sleep_start(void);
#endif
