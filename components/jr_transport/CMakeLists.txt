# jr_transport — L2 Transport adapter.
#
# Run 3 fills in the Gemini Live impl of the RealtimeVoiceClient port. The
# builders / would-block-as-backpressure framer / inbound parser live in
# src/gemini_live.c — portable C (jr_ports + jr_core + cJSON + libc, NO ESP-IDF)
# so the SAME source is exercised by the host Unity suite (host/CMakeLists.txt)
# and compiled here (cJSON via the IDF `json` component instead of the vendored
# copy). would-block => JR_ERR_WOULD_BLOCK (drop-newest), never a teardown;
# every server frame (incl. heartbeat) is a liveness tick; reconnect is an L3
# transition, not a flag here.
#
# src/gemini_device_ws.c is the DEVICE-PHASE stub: the esp_websocket_client impl
# of the lower ws_transport seam that plugs UNDER the (already host-tested)
# framer. It is intentionally NOT built yet — its recv (WEBSOCKET_EVENT_DATA ->
# ring) wiring is a Phase-1 DEVICE acceptance step. Add "src/gemini_device_ws.c"
# to SRCS (it is the only file that needs PRIV_REQUIRES esp_websocket_client)
# once that lands; do NOT enable it before it is device-verified.
idf_component_register(
    SRCS
        "src/jr_transport.c"
        "src/gemini_live.c"
        # "src/gemini_device_ws.c"   # device-phase (see note above)
    INCLUDE_DIRS "include"
    REQUIRES jr_ports jr_core json
    PRIV_REQUIRES esp_websocket_client
)
