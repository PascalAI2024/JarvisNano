# jr_probe - bounded, read-only board measurements behind GET /api/diag/board.
# board_json.c is plain C and host-tested; gpio_probe.c touches two pins.
idf_component_register(
    SRCS
        "src/board_json.c"
        "src/gpio_probe.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_driver_gpio
        esp_timer
        freertos
        log
)
