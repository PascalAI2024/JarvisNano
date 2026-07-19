# jr_http — production v5 same-origin cockpit and authenticated control API.
#
# The HTTP daemon owns protocol/auth/config parsing. Runtime mutations are
# handed to the composition root through a bounded action queue; the daemon
# never writes the voice/display/audio state directly.
idf_component_register(
    SRCS "src/jr_http.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_http_server
        esp_timer
        jr_net
        json
        nvs_flash
    EMBED_TXTFILES "assets/index.html"
)
