idf_component_register(
    SRCS
        "src/jarvis_logger.c"
        "src/jarvis_logger_http.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public deps used in the header / by callers.
        esp_common
    PRIV_REQUIRES
        # Private deps used only inside the component sources.
        log              # esp_log_set_vprintf / vprintf_like_t
        freertos         # task, spinlock, task notifications
        esp_timer        # esp_timer_get_time() for periodic flush bookkeeping
        fatfs            # FILE* writes land on the FATFS-mounted SD card
        heap             # heap_caps_malloc(MALLOC_CAP_SPIRAM)
        esp_http_server  # GET /api/logs handler (jarvis_logger_http.c)
)
