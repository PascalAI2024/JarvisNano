# main — L6 App Orchestration / Composition Root.
#
# The ONLY component that "new"s concretes and injects them into the pure core.
# It depends on the core (jr_core), the port vocabulary (jr_ports), the pure math
# (jr_dsp), the L0 HAL (jr_hal), the network/config adapter (jr_net), the L1
# audio pipeline (jr_audio), and the L2 Gemini transport (jr_transport). It owns
# NVS, esp_timer, and the diag esp_http_server. This is the composition root —
# it sits at the top of the dependency graph; nothing depends on it.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        jr_ports
        jr_core
        jr_dsp
        jr_hal
        jr_net
        jr_audio
        jr_tools
        jr_transport
        jr_display
        jr_imu
        jr_power
        jr_rtc
        jr_wake
        nvs_flash
        esp_timer
        esp_http_server
        json
    EMBED_TXTFILES
        "diagnostics.html"
)
