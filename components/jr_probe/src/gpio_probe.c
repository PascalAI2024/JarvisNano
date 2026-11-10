/* SPDX-License-Identifier: Apache-2.0 */
#include "jr_probe/gpio_probe.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_LCD_TE   GPIO_NUM_13   /* CO5300 TE output (schematic LCD_TE)      */
#define PIN_PWR_KEY  GPIO_NUM_3    /* SYS_OUT, the PWR-key mirror (strapping)  */
#define TE_WINDOW_MAX_MS   1000u
#define PWR_WATCH_MAX_MS   5000u

static volatile uint32_t s_te_edges;
static volatile int64_t  s_te_last_us;
static volatile uint32_t s_te_min_us, s_te_max_us;

static void IRAM_ATTR te_isr(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
    const int64_t last = s_te_last_us;
    if (last != 0) {
        const uint32_t d = (uint32_t)(now - last);
        if (s_te_min_us == 0 || d < s_te_min_us) { s_te_min_us = d; }
        if (d > s_te_max_us) { s_te_max_us = d; }
    }
    s_te_last_us = now;
    s_te_edges++;
}

void jr_probe_te(uint32_t window_ms, jr_probe_te_t *out)
{
    memset(out, 0, sizeof(*out));
    if (window_ms == 0 || window_ms > TE_WINDOW_MAX_MS) { window_ms = 500; }
    out->window_ms = window_ms;

    /* Input, no pulls, rising-edge interrupt. Nothing else owns this pin. */
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_TE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        out->why = "gpio_config failed";
        return;
    }
    /* The touch factory normally installed the service already (that is the
     * INVALID_STATE case); either result leaves it usable. */
    const esp_err_t svc = gpio_install_isr_service(0);
    if (svc != ESP_OK && svc != ESP_ERR_INVALID_STATE) {
        out->why = "isr service unavailable";
        return;
    }
    s_te_edges = 0; s_te_last_us = 0; s_te_min_us = 0; s_te_max_us = 0;
    if (gpio_set_intr_type(PIN_LCD_TE, GPIO_INTR_POSEDGE) != ESP_OK ||
        gpio_isr_handler_add(PIN_LCD_TE, te_isr, NULL) != ESP_OK) {
        gpio_set_intr_type(PIN_LCD_TE, GPIO_INTR_DISABLE);
        out->why = "isr add failed";
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(window_ms));
    gpio_set_intr_type(PIN_LCD_TE, GPIO_INTR_DISABLE);
    gpio_isr_handler_remove(PIN_LCD_TE);

    out->ok = true;
    out->edges = s_te_edges;
    out->hz = (uint32_t)(((uint64_t)out->edges * 1000u) / window_ms);
    out->period_us_min = s_te_min_us;
    out->period_us_max = s_te_max_us;
    out->level_now = gpio_get_level(PIN_LCD_TE);
}

void jr_probe_pwr_key(uint32_t watch_ms, jr_probe_pwr_t *out)
{
    memset(out, 0, sizeof(*out));
    if (watch_ms > PWR_WATCH_MAX_MS) { watch_ms = PWR_WATCH_MAX_MS; }

    /* Enable the input buffer only. gpio_config() would also write the pull
     * bits; GPIO3 is a strapping pin and keeps whatever it booted with. */
    if (gpio_set_direction(PIN_PWR_KEY, GPIO_MODE_INPUT) != ESP_OK) {
        out->why = "gpio_set_direction failed";
        return;
    }
    out->ok = true;
    out->level = gpio_get_level(PIN_PWR_KEY);
    out->level_min = out->level;
    out->level_max = out->level;
    if (watch_ms == 0) { return; }

    out->watched = true;
    out->watch_ms = watch_ms;
    int prev = out->level;
    const int64_t start = esp_timer_get_time();
    int64_t last = start;
    const int64_t end = start + (int64_t)watch_ms * 1000;
    int64_t high_us = 0;
    while (last < end) {
        vTaskDelay(1);
        const int64_t now = esp_timer_get_time();
        const int lv = gpio_get_level(PIN_PWR_KEY);
        if (prev) { high_us += now - last; }
        if (lv != prev) { out->transitions++; }
        if (lv < out->level_min) { out->level_min = lv; }
        if (lv > out->level_max) { out->level_max = lv; }
        prev = lv;
        last = now;
    }
    out->high_ms = (uint32_t)(high_us / 1000);
}
