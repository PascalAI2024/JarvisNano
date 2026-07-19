# jr_memory — bounded, power-loss-tolerant local fact storage.

idf_component_register(
    SRCS
        "src/jr_memory.c"
        "src/jr_memory_guard.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        freertos
)

