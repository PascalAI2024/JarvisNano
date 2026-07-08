# jr_transport — L2 Transport adapter (Phase 0: empty-but-registered skeleton).
# Phase 1 fills this in: the Gemini Live WSS impl of the RealtimeVoiceClient
# port behind an owned esp_websocket_client framer that treats would-block as
# backpressure (never teardown), with independent uplink/downlink liveness and
# heartbeat-driven staleness. A host `mock` impl lives under host/ fakes.
idf_component_register(
    SRCS "src/jr_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES jr_ports
    PRIV_REQUIRES esp_websocket_client
)
