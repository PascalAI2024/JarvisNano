idf_component_register(
    SRCS
        "src/jarvis_brain.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public deps used in the header / by callers.
        esp_common       # esp_err_t
    PRIV_REQUIRES
        # Private deps used only inside the component source.
        log              # ESP_LOGx
        esp_timer        # esp_timer_get_time() for monotonic memory.log timestamps
        fatfs            # FILE* reads/writes land on the FATFS-mounted SD card
)
