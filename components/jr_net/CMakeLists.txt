# jr_net — L0 bounded Wi-Fi/APSTA + typed NVS "app" config adapter.
#
# The device-side network/persistence bring-up the composition root uses before
# it can build the Gemini endpoint URL. Credentials are read from NVS at runtime
# (never hardcoded). It speaks ESP-IDF Wi-Fi/NVS on the inside and plain
# esp_err_t / char* on the outside, so main stays thin.
idf_component_register(
    SRCS "src/jr_net.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        mbedtls
)
