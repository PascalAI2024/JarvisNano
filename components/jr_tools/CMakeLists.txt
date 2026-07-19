# jr_tools — bounded async JarvisMCP tool worker.
idf_component_register(
    SRCS
        "src/jr_tools.c"
        "src/jr_tools_templates.c"
    INCLUDE_DIRS "include"
    REQUIRES jr_net json
    PRIV_REQUIRES esp_http_client esp_hw_support mbedtls esp_timer heap
)
