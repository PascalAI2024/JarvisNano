# jr_imu — QMI8658 accelerometer sampler. Ported from firmware/components/jarvis_imu
# in Phase 1; the register driver is verbatim, the threading model is not
# (see include/jr_imu/jr_imu.h). Links hardware, so it is never in the host build.
idf_component_register(
    SRCS
        "src/jr_imu.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public deps used in the header / by callers.
        esp_common          # esp_err_t
    PRIV_REQUIRES
        log                 # ESP_LOGx
        freertos            # task, static mutex, pdMS_TO_TICKS
        esp_timer           # esp_timer_get_time for snapshot age
        esp_driver_i2c      # i2c_master_* (new IDF 5.x master driver)
        esp_board_manager   # esp_board_manager_get_periph_handle (shared bus)
)
