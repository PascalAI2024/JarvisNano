# jr_wake — L1 wake-word adapter (esp-sr WakeNet9, bare esp_wn_iface).
#
# The composition root pushes frames from the single-owner capture seam into a
# ring; the `jr_wake` worker task (core 0) runs detect() so the voice task never
# does, and the voice task polls for a hit. The wn9_jarvis_tts model rides the
# `model` flash partition (packed by esp-sr's build integration from
# CONFIG_SR_WN_*).
idf_component_register(
    SRCS "src/jr_wake.c"
    INCLUDE_DIRS "include"
    REQUIRES
        espressif__esp-sr
        esp_timer
        freertos
        heap
)
