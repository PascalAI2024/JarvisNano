# jr_hal — L0 HAL / Board Support. The ONLY component that includes ESP-IDF
# driver + board headers. It adapts esp_board_manager / esp_timer / (later) the
# CO5300 + CST9217 drivers to the pure jr_ports interfaces. Because it links
# hardware, it is NEVER compiled by the host build — only jr_core + jr_dsp are.
idf_component_register(
    SRCS
        "src/hal.c"
        "src/input_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES
        jr_ports
        esp_board_manager
        esp_lcd_touch
        esp_timer
)
