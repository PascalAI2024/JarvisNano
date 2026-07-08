# jr_audio — L1 Audio Pipeline adapter (device implementation).
#
# Capture: esp_codec_dev 4-lane TDM read -> 24->16 downsample -> demux -> esp-sr
# AEC (FD_LOW_COST) -> gain/knee, exposed as jr_audio_source_t. Playback: a
# drop-newest PSRAM ring drained by a feeder task to the ES8311, exposed as
# jr_audio_sink_t with the synchronous fast-kill mute_now. Codec handles come
# from esp_board_manager (double-cast dev_audio_codec_handles_t->codec_dev).
idf_component_register(
    SRCS "src/jr_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES
        jr_ports
        jr_dsp
        jr_hal
        esp_board_manager
        espressif__esp_codec_dev
        espressif__esp-sr
        esp_timer
)
