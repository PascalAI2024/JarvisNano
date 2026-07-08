/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * main.c — JarvisRobot v5 L6 App Orchestration / Composition Root.
 *
 * The ONLY site in the whole firmware that constructs concrete adapters and
 * injects them into the pure core. Nothing depends on this file; it depends on
 * everything (top of the dependency graph). Thin by design: build the object
 * graph, boot to idle, get out of the way.
 *
 * Phase 0 scope: init NVS, log a banner, init the board via the L0 HAL, obtain
 * the port structs from the HAL, wire them into a jr_app holding the core
 * state, set state = Idle, and present a blank/idle face. NO voice logic, NO
 * task spawning — that is Phase 1's composition work against this exact seam.
 */
#include <stdio.h>

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"

#include "jr_ports/ports.h"       /* pure port vocabulary                   */
#include "jr_core/session.h"      /* pure L3 state machine                  */
#include "jr_hal/hal.h"           /* L0 adapters (the concretes)            */

static const char *TAG = "jarvis_v5";

/*
 * The composition graph. In Phase 0 it holds the injected ports + the core's
 * current lifecycle state. Phase 1 grows this with the RealtimeVoiceClient, the
 * AudioSource/Sink, the owner task, and the resilience monitors — all injected
 * here, never reached for by the core.
 */
typedef struct {
    jr_clock_t   clock;    /* injected: esp_timer on device, fake on host   */
    jr_display_t display;  /* injected: L5 presenter port                    */
    jr_input_t   input;    /* injected: L0 CST9217 event port                */
    jr_state_t   state;    /* owned by the core; L6 seeds it to Idle         */
} jr_app_t;

static jr_app_t s_app;

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=====================================================");
    ESP_LOGI(TAG, " JarvisRobot v5  |  hexagonal core  |  Phase 0 boot   ");
    ESP_LOGI(TAG, "=====================================================");

    /* 1. Persistence up first (Wi-Fi/LLM config, calibration live here). */
    init_nvs();

    /* 2. L0: bring up the board (TCA9554 -> AXP2101 -> display -> touch via
     *    esp_board_manager). The direct-cast handle accessor + reset-before-
     *    probe live inside the HAL, never here (hardware.md #9, #12). */
    esp_err_t hal_err = jr_hal_init();
    if (hal_err != ESP_OK) {
        ESP_LOGE(TAG, "jr_hal_init failed: %s — booting core headless", esp_err_to_name(hal_err));
    }

    /* 3. Build the object graph: pull concrete ports from the HAL and inject
     *    them. This is the dependency-injection seam — the core never calls a
     *    HAL function, it only ever receives these structs. */
    s_app.clock   = jr_hal_clock();
    s_app.display = jr_hal_display();
    s_app.input   = jr_hal_input();

    /* 4. Seed the core lifecycle. Phase 0 boots to Idle and stays there. */
    s_app.state = JR_ST_IDLE;
    ESP_LOGI(TAG, "core state = %s", jr_state_name(s_app.state));

    /* 5. Present the idle face — the Phase-0 acceptance target (blank/idle
     *    face on real hardware). On device this drives the CO5300; in this
     *    skeleton the HAL display port logs the intent. */
    jr_display_present(&s_app.display, JR_FACE_IDLE, 0);

    ESP_LOGI(TAG, "boot complete — idle. (voice/session logic lands in Phase 1)");
    /* app_main returns; FreeRTOS idle task keeps the system alive. Phase 1
     * spawns the pinned session-owner task here instead of returning. */
}
