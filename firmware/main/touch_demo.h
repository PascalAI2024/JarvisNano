#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef bool (*touch_demo_gemini_configured_cb_t)(void);

esp_err_t touch_demo_start(touch_demo_gemini_configured_cb_t gemini_configured);
esp_err_t touch_demo_get_diagnostics_json(char *out, size_t out_size);
esp_err_t touch_demo_run_scene(const char *scene_name);

/* Hardware power moods driven by IMU orientation + stability + motion (face_down
 * for DREAM/sleep; lift+face_up or moving for AWAKE). Battery/charging influences
 * visuals indirectly via face amp and status. Shake dismisses scenes. */
typedef enum {
    POWER_MOOD_AWAKE = 0,
    POWER_MOOD_DREAM,
} power_mood_t;

power_mood_t touch_demo_get_power_mood(void);
const char *touch_demo_get_power_mood_name(void);
