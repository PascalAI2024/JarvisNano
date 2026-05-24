#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef bool (*touch_demo_gemini_configured_cb_t)(void);

esp_err_t touch_demo_start(touch_demo_gemini_configured_cb_t gemini_configured);
esp_err_t touch_demo_get_diagnostics_json(char *out, size_t out_size);
esp_err_t touch_demo_run_scene(const char *scene_name);
