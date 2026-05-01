/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Seeed XIAO ESP32-S3 Sense — board setup.
 *
 * Phase 1 leaves all devices in init_skip mode (PDM mic is driven
 * directly by the audio pipeline; LED strip and fake DAC are
 * placeholders). This file exists so the board manager codegen has a
 * compile target; add CUSTOM_DEVICE_IMPLEMENT(...) entries here when
 * real custom devices (camera, touch, etc.) are wired up.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_board_manager_includes.h"
#include "gen_board_device_custom.h"
#include "periph_rmt.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "led_strip_types.h"
