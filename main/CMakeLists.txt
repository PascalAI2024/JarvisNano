# main — L6 App Orchestration / Composition Root.
#
# The ONLY component that "new"s concretes and injects them into the core. It
# depends on the core (jr_core), the pure math (jr_dsp), the port vocabulary
# (jr_ports), and the L0 HAL (jr_hal). It does NOT link the transport/display
# adapters directly in Phase 0 — those are wired in as they land in later
# phases. nvs_flash + esp_timer are IDF services main owns.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        jr_ports
        jr_core
        jr_dsp
        jr_hal
        nvs_flash
        esp_timer
)
