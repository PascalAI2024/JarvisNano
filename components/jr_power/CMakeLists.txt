# jr_power — AXP2101 battery telemetry (READ-ONLY; never touches power rails).
# Ported from firmware/components/jarvis_pmic in Phase 1; register access is
# verbatim, threading is a slow sampler + non-blocking read (see the header).
idf_component_register(
    SRCS
        "src/jr_power.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common          # esp_err_t
    PRIV_REQUIRES
        log                 # ESP_LOGx
        freertos            # task, static mutex, pdMS_TO_TICKS
        esp_timer           # esp_timer_get_time for snapshot age
        esp_driver_i2c      # i2c_master_* (new IDF 5.x master driver)
        esp_board_manager   # esp_board_manager_get_periph_handle (shared bus)
)
