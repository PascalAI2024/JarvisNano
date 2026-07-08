# jr_audio — L1 Audio Pipeline adapter (Phase 0: empty-but-registered skeleton).
# Phase 1 fills this in: TDM demux (mic lane0 / ref lane2), esp-sr AEC
# (FD_LOW_COST, filter_length=4), the mandatory 24->16 downsample, gain/knee,
# and the PCM ring feeder — the task bodies wrapping jr_dsp's pure math and the
# esp_codec_dev calls, exposed as AudioSource/AudioSink ports.
idf_component_register(
    SRCS "src/jr_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES jr_ports jr_dsp jr_hal
)
