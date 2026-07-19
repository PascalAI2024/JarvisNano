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
# src/gemini_device_ws.c is the esp_websocket_client impl of the lower
# ws_transport seam, plugging UNDER the (host-tested) framer in gemini_live.c.
# It IS built and device-verified: see docs/evidence/20260718-v5-boot.log, which
# shows TLS validation -> ws connected -> SEND_SETUP -> Handshaking -> Listening.
#
# The former src/jr_transport.c was a Phase-0 placeholder holding one empty
# function; nothing included its header. Removed 2026-07-18 — the real
# jr_realtime_voice_client_t impl is gemini_live.c + gemini_device_ws.c.
idf_component_register(
    SRCS
        "src/gemini_live.c"
        "src/gemini_device_ws.c"   # device WS byte transport (recv ring landed)
    INCLUDE_DIRS "include"
    REQUIRES jr_ports jr_core json
    PRIV_REQUIRES esp_websocket_client esp-tls esp_timer
)
