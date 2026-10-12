/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the four-mood rest ladder. No ESP-IDF.
 */
#include "unity.h"
#include "jr_core/mood.h"
#include <string.h>

/* Execute the real sleep coordinator with fake peripherals. Guard ordering
 * matters: all fatal guards precede audio/IMU quiescence and every RTC write;
 * a refused panel/audio hand-off requests recovery instead of sleeping. */
#define JR_POWER_HOST_TEST 1
#include "../main/power.c"

_Atomic bool s_voice_privacy_paused;
_Atomic bool s_ota_active;
static unsigned power_lock_token;
SemaphoreHandle_t s_brain_lock = &power_lock_token;
static bool power_lock_held;
bool operator_lease_active(uint32_t now_ms) { (void)now_ms; return false; }
int64_t esp_timer_get_time(void) { return 0; }
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned ticks)
{
    (void)ticks;
    if (lock != s_brain_lock || power_lock_held) return pdFALSE;
    power_lock_held = true;
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t lock)
{ (void)lock; power_lock_held = false; }
enum { POWER_AUDIO = 1, POWER_CAPTION, POWER_IMU_STOP, POWER_IMU_ARM,
       POWER_PANEL_OFF, POWER_GPIO, POWER_WAKE, POWER_SLEEP,
       POWER_PANEL_ON, POWER_TIMER_CANCEL };
static struct {
    esp_ota_img_states_t ota_state;
    esp_err_t audio_result;
    esp_err_t imu_result;
    unsigned events[48];
    unsigned count;
    bool audio_saw_untouched_rtc;
    uint64_t timer_us;
} power_fake;
static void power_event(unsigned event)
{
    if (power_fake.count < sizeof power_fake.events / sizeof power_fake.events[0]) {
        power_fake.events[power_fake.count++] = event;
    }
}
esp_err_t esp_pm_configure(const esp_pm_config_t *cfg)
{ (void)cfg; return ESP_OK; }
const char *esp_err_to_name(esp_err_t err)
{ (void)err; return "fake"; }
const esp_partition_t *esp_ota_get_running_partition(void)
{ static const esp_partition_t p = {0}; return &p; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,
                                     esp_ota_img_states_t *state)
{ (void)p; *state = power_fake.ota_state; return ESP_OK; }
esp_err_t jr_audio_prepare_sleep(void)
{
    power_fake.audio_saw_untouched_rtc = s_rtc_sleep_magic == 0U &&
        s_rtc_sleeps == 7U && s_rtc_was_listening == 0U && s_rtc_armed == 0U;
    power_event(POWER_AUDIO);
    return power_fake.audio_result;
}
esp_err_t jr_power_read(jr_power_t *out)
{ memset(out, 0, sizeof(*out)); out->sample_seq = 1U; return ESP_OK; }
void jr_display_caption_set(const char *text)
{ (void)text; power_event(POWER_CAPTION); }
void vTaskDelay(unsigned ticks) { (void)ticks; }
void jr_imu_stop(void) { power_event(POWER_IMU_STOP); }
esp_err_t jr_imu_arm_wake_on_motion(unsigned mg)
{ (void)mg; power_event(POWER_IMU_ARM); return power_fake.imu_result; }
void jr_display_panel_off_request(void) { power_event(POWER_PANEL_OFF); }
bool jr_display_panel_is_off(void) { return true; }
void jr_display_panel_on_request(void) { power_event(POWER_PANEL_ON); }
bool jr_display_panel_is_on(void) { return true; }
void rtc_gpio_init(int gpio) { (void)gpio; power_event(POWER_GPIO); }
void rtc_gpio_set_direction(int gpio, int direction)
{ (void)gpio; (void)direction; }
void rtc_gpio_pullup_dis(int gpio) { (void)gpio; }
void rtc_gpio_pulldown_en(int gpio) { (void)gpio; }
void rtc_gpio_pulldown_dis(int gpio) { (void)gpio; }
void rtc_gpio_pullup_en(int gpio) { (void)gpio; }
int rtc_gpio_get_level(int gpio) { return gpio == GPIO_NUM_21 ? 0 : 1; }
esp_err_t esp_sleep_enable_ext0_wakeup(int gpio, int level)
{ (void)gpio; (void)level; power_event(POWER_WAKE); return ESP_OK; }
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask, int mode)
{ (void)mask; (void)mode; power_event(POWER_WAKE); return ESP_OK; }
esp_err_t esp_sleep_pd_config(int domain, int option)
{ (void)domain; (void)option; return ESP_OK; }
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t usec)
{ power_fake.timer_us = usec; power_event(POWER_WAKE); return ESP_OK; }
esp_err_t esp_sleep_disable_wakeup_source(int source)
{ (void)source; power_event(POWER_TIMER_CANCEL); return ESP_OK; }
void esp_deep_sleep_start(void) { power_event(POWER_SLEEP); }
static void power_fake_reset(void)
{
    memset(&power_fake, 0, sizeof power_fake);
    power_fake.ota_state = ESP_OTA_IMG_VALID;
    power_lock_held = false;
    atomic_store(&s_ota_active, false);
    s_rtc_sleep_magic = 0U;
    s_rtc_sleeps = 7U;
    s_rtc_was_listening = 0U;
    s_rtc_armed = 0U;
    atomic_store(&s_sleep_force, true);
    atomic_store(&s_voice_privacy_paused, true);
}

static void test_sleep_probation_refuses_before_audio_or_rtc(void)
{
    power_fake_reset();
    power_fake.ota_state = ESP_OTA_IMG_PENDING_VERIFY;
    enter_deep_sleep("test", 300U);
    TEST_ASSERT_EQUAL_UINT(0, power_fake.count);
    TEST_ASSERT_EQUAL_UINT32(0, s_rtc_sleep_magic);
    TEST_ASSERT_EQUAL_UINT32(7, s_rtc_sleeps);
    TEST_ASSERT_FALSE(atomic_load(&s_sleep_force));
}

static void test_sleep_failed_audio_preserves_rtc_and_recovers_panel(void)
{
    power_fake_reset();
    power_fake.audio_result = ESP_ERR_TIMEOUT;
    enter_deep_sleep("test", 300U);
    TEST_ASSERT_EQUAL_UINT(6, power_fake.count);
    TEST_ASSERT_EQUAL_UINT(POWER_WAKE, power_fake.events[0]);
    TEST_ASSERT_EQUAL_UINT(POWER_CAPTION, power_fake.events[1]);
    TEST_ASSERT_EQUAL_UINT(POWER_PANEL_OFF, power_fake.events[2]);
    TEST_ASSERT_EQUAL_UINT(POWER_AUDIO, power_fake.events[3]);
    TEST_ASSERT_EQUAL_UINT(POWER_TIMER_CANCEL, power_fake.events[4]);
    TEST_ASSERT_EQUAL_UINT(POWER_PANEL_ON, power_fake.events[5]);
    TEST_ASSERT_TRUE(power_fake.audio_saw_untouched_rtc);
    TEST_ASSERT_EQUAL_UINT32(0, s_rtc_sleep_magic);
    TEST_ASSERT_EQUAL_UINT32(7, s_rtc_sleeps);
    TEST_ASSERT_EQUAL_UINT8(0, s_rtc_armed);
    TEST_ASSERT_FALSE(atomic_load(&s_sleep_force));
}

static void test_sleep_quiesces_audio_then_records_and_arms_wake(void)
{
    power_fake_reset();
    enter_deep_sleep("test", 300U);
    TEST_ASSERT_TRUE(power_fake.audio_saw_untouched_rtc);
    TEST_ASSERT_EQUAL_UINT(POWER_WAKE, power_fake.events[0]);
    TEST_ASSERT_EQUAL_UINT(POWER_CAPTION, power_fake.events[1]);
    TEST_ASSERT_EQUAL_UINT(POWER_PANEL_OFF, power_fake.events[2]);
    TEST_ASSERT_EQUAL_UINT(POWER_AUDIO, power_fake.events[3]);
    TEST_ASSERT_EQUAL_UINT(POWER_IMU_STOP, power_fake.events[4]);
    TEST_ASSERT_EQUAL_UINT(POWER_IMU_ARM, power_fake.events[5]);
    TEST_ASSERT_EQUAL_UINT(POWER_SLEEP, power_fake.events[power_fake.count - 1U]);
    TEST_ASSERT_EQUAL_UINT32(SLEEP_RTC_MAGIC, s_rtc_sleep_magic);
    TEST_ASSERT_EQUAL_UINT32(8, s_rtc_sleeps);
    TEST_ASSERT_EQUAL_UINT8(0, s_rtc_was_listening);
    TEST_ASSERT_EQUAL_UINT8(SLEEP_ARMED_TIMER | SLEEP_ARMED_LIFT | SLEEP_ARMED_TOUCH,
                           s_rtc_armed);
    TEST_ASSERT_EQUAL_UINT64(300000000ULL, power_fake.timer_us);
    /* Unmuted sleep still remembers the owner's capability for the wake. */
    power_fake_reset();
    atomic_store(&s_voice_privacy_paused, false);
    power_fake.imu_result = ESP_ERR_TIMEOUT;
    enter_deep_sleep("test", 300U);
    TEST_ASSERT_EQUAL_UINT8(1, s_rtc_was_listening);
    TEST_ASSERT_EQUAL_UINT8(SLEEP_ARMED_TIMER | SLEEP_ARMED_TOUCH |
                           SLEEP_LIFT_ARM_FAILED, s_rtc_armed);
}

static void test_dream_glass_requires_genuine_idle(void)
{
    for (jr_mood_t mood = JR_MOOD_AWAKE; mood <= JR_MOOD_DREAM; ++mood) {
        TEST_ASSERT_EQUAL(mood == JR_MOOD_DREAM,
            jr_mood_dream_glass(mood, false, false, false, false));
    }
    TEST_ASSERT_FALSE(jr_mood_dream_glass(JR_MOOD_DREAM, true, false, false, false));
    TEST_ASSERT_FALSE(jr_mood_dream_glass(JR_MOOD_DREAM, false, true, false, false));
    TEST_ASSERT_FALSE(jr_mood_dream_glass(JR_MOOD_DREAM, false, false, true, false));
    TEST_ASSERT_FALSE(jr_mood_dream_glass(JR_MOOD_DREAM, false, false, false, true));
    TEST_ASSERT_FALSE(jr_mood_dream_glass((jr_mood_t)255, false, false, false, false));
}

static void test_weather_first_and_retries_respect_rest_and_network(void)
{
    for (jr_mood_t mood = JR_MOOD_AWAKE; mood <= JR_MOOD_DREAM; ++mood) {
        const bool lit = mood == JR_MOOD_AWAKE || mood == JR_MOOD_AMBIENT;
        TEST_ASSERT_EQUAL(lit, jr_mood_weather_due(mood, true, 20000, 0, false, false));
        TEST_ASSERT_EQUAL(lit, jr_mood_weather_due(mood, true, 140000, 20000, true, false));
        TEST_ASSERT_EQUAL(lit, jr_mood_weather_due(mood, true, 620000, 20000, false, true));
        TEST_ASSERT_EQUAL(lit, jr_mood_weather_due(mood, true, 1820000, 20000, false, false));
        TEST_ASSERT_FALSE(jr_mood_weather_due(mood, false, 20000, 0, false, false));
        TEST_ASSERT_FALSE(jr_mood_weather_due(mood, false, 140000, 20000, true, false));
        TEST_ASSERT_FALSE(jr_mood_weather_due(mood, false, 1820000, 20000, false, true));
    }
}

static void test_weather_cadence_and_wraparound_are_preserved(void)
{
    TEST_ASSERT_FALSE(jr_mood_weather_due(JR_MOOD_AWAKE, true, 19999, 0, false, true));
    TEST_ASSERT_FALSE(jr_mood_weather_due(JR_MOOD_AWAKE, true, 139999, 20000, true, true));
    TEST_ASSERT_FALSE(jr_mood_weather_due(JR_MOOD_AWAKE, true, 619999, 20000, false, true));
    TEST_ASSERT_FALSE(jr_mood_weather_due(JR_MOOD_AWAKE, true, 1819999, 20000, false, false));
    /* A screen refresh is not also a background refresh. */
    TEST_ASSERT_FALSE(jr_mood_weather_due(JR_MOOD_AWAKE, true, 620000, 20000, false, false));
    const uint32_t before_wrap = UINT32_MAX - 10000U;
    TEST_ASSERT_FALSE(jr_mood_weather_due(JR_MOOD_AMBIENT, true,
        before_wrap + 119999U, before_wrap, true, false));
    TEST_ASSERT_TRUE(jr_mood_weather_due(JR_MOOD_AMBIENT, true,
        before_wrap + 120000U, before_wrap, true, false));
}

static void test_mood_starts_awake(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = 100 };
    jr_mood_out_t o = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, o.mood);
    TEST_ASSERT_EQUAL_UINT8(100, o.brightness);
    TEST_ASSERT_TRUE(o.voice_armed);
    TEST_ASSERT_FALSE(o.clock_on);
}

static void test_mood_still_climbs_ambient_whisper_dream(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = {0};

    in.now_ms = JR_MOOD_AMBIENT_MS - 1;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);

    in.now_ms = JR_MOOD_AMBIENT_MS;
    jr_mood_out_t amb = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_AMBIENT, amb.mood);
    TEST_ASSERT_TRUE(amb.clock_on);
    TEST_ASSERT_TRUE(amb.voice_armed);
    TEST_ASSERT_EQUAL_UINT8(48, amb.brightness);
    TEST_ASSERT_TRUE(amb.changed);

    in.now_ms = JR_MOOD_WHISPER_MS;
    jr_mood_out_t wh = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, wh.mood);
    TEST_ASSERT_FALSE(wh.voice_armed);
    TEST_ASSERT_EQUAL_UINT8(22, wh.brightness);

    in.now_ms = JR_MOOD_DREAM_MS;
    jr_mood_out_t dr = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, dr.mood);
    TEST_ASSERT_FALSE(dr.voice_armed);
    TEST_ASSERT_EQUAL_UINT8(8, dr.brightness);
}

static void test_mood_motion_wakes(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = JR_MOOD_DREAM_MS };
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &in).mood);

    in.now_ms = JR_MOOD_DREAM_MS + 50;
    in.moving = true;
    jr_mood_out_t o = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, o.mood);
    TEST_ASSERT_TRUE(o.voice_armed);
    TEST_ASSERT_TRUE(o.changed);
}

static void test_mood_busy_holds_awake(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = {
        .now_ms = JR_MOOD_DREAM_MS,
        .user_busy = true,
    };
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
}

static void test_mood_face_down_is_dream(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = 200, .face_down = true };
    jr_mood_out_t o = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, o.mood);
    TEST_ASSERT_FALSE(o.voice_armed);
}

/* Sleep is due ten minutes into DREAM and not a tick sooner, by either road
 * (face-down, or still); a poke or a move cancels it. Both roads are walked
 * so a rule that only counted from boot, or only from face-down, fails. */
static void test_mood_sleep_is_due_ten_minutes_into_dream(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = 1000, .face_down = true };
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &in).mood);
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, 1000));
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, 1000 + JR_MOOD_SLEEP_MS - 1));
    TEST_ASSERT_TRUE(jr_mood_sleep_due(&s, 1000 + JR_MOOD_SLEEP_MS));
    /* Lifted: awake, and the clock starts over. */
    in.face_down = false;
    in.moving = true;
    in.now_ms = 1000 + JR_MOOD_SLEEP_MS;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, in.now_ms + JR_MOOD_SLEEP_MS));

    /* The still road: DREAM at 15 min, sleep due at 25. */
    jr_mood_reset(&s, 0);
    jr_mood_in_t still = { .now_ms = JR_MOOD_DREAM_MS };
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &still).mood);
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, JR_MOOD_DREAM_MS + JR_MOOD_SLEEP_MS - 1));
    TEST_ASSERT_TRUE(jr_mood_sleep_due(&s, JR_MOOD_DREAM_MS + JR_MOOD_SLEEP_MS));
    /* WHISPER is not DREAM: a device that has rested 5 min never sleeps. */
    jr_mood_reset(&s, 0);
    still.now_ms = JR_MOOD_WHISPER_MS;
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, jr_mood_step(&s, &still).mood);
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, JR_MOOD_WHISPER_MS + 2 * JR_MOOD_SLEEP_MS));
    TEST_ASSERT_FALSE(jr_mood_sleep_due(NULL, 0));
}

/* The saver ladder is the same ladder, four times faster, and it is an
 * INPUT: the moment the cell is charging again the normal waits apply. */
static void test_mood_saver_ladder_is_four_times_faster(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .saver = true };
    in.now_ms = JR_MOOD_AMBIENT_MS / JR_MOOD_SAVER_DIV - 1;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
    in.now_ms = JR_MOOD_AMBIENT_MS / JR_MOOD_SAVER_DIV;
    TEST_ASSERT_EQUAL(JR_MOOD_AMBIENT, jr_mood_step(&s, &in).mood);
    in.now_ms = JR_MOOD_WHISPER_MS / JR_MOOD_SAVER_DIV;
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, jr_mood_step(&s, &in).mood);
    in.now_ms = JR_MOOD_DREAM_MS / JR_MOOD_SAVER_DIV;
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &in).mood);
    const uint32_t dream_at = in.now_ms;
    TEST_ASSERT_FALSE(jr_mood_sleep_due(&s, dream_at + JR_MOOD_SLEEP_MS / JR_MOOD_SAVER_DIV - 1));
    TEST_ASSERT_TRUE(jr_mood_sleep_due(&s, dream_at + JR_MOOD_SLEEP_MS / JR_MOOD_SAVER_DIV));
    /* Plugged in (saver off) the same stillness is not yet DREAM. */
    jr_mood_reset(&s, 0);
    in.saver = false;
    in.now_ms = JR_MOOD_DREAM_MS / JR_MOOD_SAVER_DIV;
    TEST_ASSERT_EQUAL(JR_MOOD_AMBIENT, jr_mood_step(&s, &in).mood);
}

/* QUIET (privacy mute): five seconds still and the glass is a WHISPER watch,
 * never AMBIENT (a dimmed listener with nothing to listen for). A pickup, a
 * busy phase, and face-down keep their meanings, and DREAM keeps its own
 * clock — mute must not be a shortcut to a black glass. Mutation-checked in
 * both directions: a ladder that ignores quiet fails the 5 s check; one that
 * jumps straight to DREAM fails the 1 minute check. */
static void test_mood_quiet_rests_as_a_watch_in_five_seconds(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = JR_MOOD_QUIET_MS - 1U, .quiet = true };
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
    in.now_ms = JR_MOOD_QUIET_MS;
    jr_mood_out_t o = jr_mood_step(&s, &in);
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, o.mood);
    TEST_ASSERT_TRUE(o.clock_on);
    TEST_ASSERT_FALSE(o.voice_armed);
    TEST_ASSERT_EQUAL(22, o.brightness);
    in.now_ms = 60000U;
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, jr_mood_step(&s, &in).mood);
    in.now_ms = JR_MOOD_DREAM_MS;
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &in).mood);

    /* a pickup lights it, and it settles again in five seconds, not five minutes */
    in.now_ms = JR_MOOD_DREAM_MS + 1000U;
    in.moving = true;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
    in.moving = false;
    in.now_ms += JR_MOOD_QUIET_MS;
    TEST_ASSERT_EQUAL(JR_MOOD_WHISPER, jr_mood_step(&s, &in).mood);

    /* answering a question outranks quiet; face-down still outranks both */
    in.user_busy = true;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
    in.user_busy = false;
    in.face_down = true;
    TEST_ASSERT_EQUAL(JR_MOOD_DREAM, jr_mood_step(&s, &in).mood);

    /* not quiet: the same five seconds is still AWAKE */
    jr_mood_reset(&s, 0);
    jr_mood_in_t loud = { .now_ms = JR_MOOD_QUIET_MS };
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &loud).mood);
}

static void test_mood_poke_awake_resets_still(void)
{
    jr_mood_state_t s;
    jr_mood_reset(&s, 0);
    jr_mood_in_t in = { .now_ms = JR_MOOD_DREAM_MS };
    (void)jr_mood_step(&s, &in);
    jr_mood_poke_awake(&s, JR_MOOD_DREAM_MS + 10);
    in.now_ms = JR_MOOD_DREAM_MS + 20;
    TEST_ASSERT_EQUAL(JR_MOOD_AWAKE, jr_mood_step(&s, &in).mood);
}

void mood_tests_run(void)
{
    RUN_TEST(test_sleep_probation_refuses_before_audio_or_rtc);
    RUN_TEST(test_sleep_failed_audio_preserves_rtc_and_recovers_panel);
    RUN_TEST(test_sleep_quiesces_audio_then_records_and_arms_wake);
    RUN_TEST(test_dream_glass_requires_genuine_idle);
    RUN_TEST(test_weather_first_and_retries_respect_rest_and_network);
    RUN_TEST(test_weather_cadence_and_wraparound_are_preserved);
    RUN_TEST(test_mood_starts_awake);
    RUN_TEST(test_mood_still_climbs_ambient_whisper_dream);
    RUN_TEST(test_mood_motion_wakes);
    RUN_TEST(test_mood_busy_holds_awake);
    RUN_TEST(test_mood_face_down_is_dream);
    RUN_TEST(test_mood_quiet_rests_as_a_watch_in_five_seconds);
    RUN_TEST(test_mood_poke_awake_resets_still);
    RUN_TEST(test_mood_sleep_is_due_ten_minutes_into_dream);
    RUN_TEST(test_mood_saver_ladder_is_four_times_faster);
}
