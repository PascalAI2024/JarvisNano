# jr_rtc — PCF85063 civil-time access on the shared I2C bus.
# Hardware-backed; never in the host harness.
idf_component_register(
    SRCS "src/jr_rtc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES
        log
        esp_driver_i2c
        esp_board_manager
)
