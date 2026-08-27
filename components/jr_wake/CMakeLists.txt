# jr_wake — L1 wake-word adapter (esp-sr WakeNet9, bare esp_wn_iface).
#
# Fed by the composition root from the single-owner capture seam while the
# voice session is disarmed; a detection re-arms voice and pokes the mood
# ladder awake. The wn9_jarvis_tts model rides the `model` flash partition
# (packed by esp-sr's build integration from CONFIG_SR_WN_*).
idf_component_register(
    SRCS "src/jr_wake.c"
    INCLUDE_DIRS "include"
    REQUIRES
        espressif__esp-sr
        esp_timer
)
