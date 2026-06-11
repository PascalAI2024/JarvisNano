idf_component_register(
    SRCS
        "src/jarvis_pmic.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public deps used in the header / by callers.
        esp_common       # esp_err_t
    PRIV_REQUIRES
        # Private deps used only inside the component source.
        log                          # ESP_LOGx
        freertos                     # pdMS_TO_TICKS
        esp_driver_i2c               # i2c_master_* (new IDF 5.x master driver)
        espressif__esp_board_manager # esp_board_manager_get_periph_handle (shared bus)
)
