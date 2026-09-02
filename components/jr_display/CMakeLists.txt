# jr_display — direct CO5300 presenter + baked EAF asset partition.
#
# The proven board primitive still lives under firmware/components so v4 can
# consume it. The plain v5 project intentionally compiles that source into this
# adapter rather than adding the entire legacy firmware component tree.
set(JARVIS_BOARD_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/jarvis_board")

idf_component_register(
    SRCS
        "src/jr_display.c"
        "src/hud_render.c"
        "${JARVIS_BOARD_ROOT}/src/jarvis_board.c"
    INCLUDE_DIRS
        "include"
        "${JARVIS_BOARD_ROOT}/include"
    REQUIRES
        jr_ports
        esp_lcd
        esp_driver_spi
        esp_timer
        spiffs
    PRIV_REQUIRES
        freertos
        heap
        espressif2022__esp_emote_gfx
        espressif__esp_lcd_co5300
)

# Produce the emote_assets SPIFFS image exclusively from tracked canonical EAF
# sources. Staging in CMAKE_BINARY_DIR keeps generated copies out of the repo,
# while configure_file makes source changes participate in CMake regeneration.
set(JR_DISPLAY_ASSET_STAGE "${CMAKE_BINARY_DIR}/jr_display_assets")
file(REMOVE_RECURSE "${JR_DISPLAY_ASSET_STAGE}")
file(MAKE_DIRECTORY "${JR_DISPLAY_ASSET_STAGE}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware/mascot/reactive/rwave_idle.eaf"
    "${JR_DISPLAY_ASSET_STAGE}/rwave_idle.eaf" COPYONLY)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware/mascot/reactive/rwave_listen.eaf"
    "${JR_DISPLAY_ASSET_STAGE}/rwave_listen.eaf" COPYONLY)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware/mascot/reactive/rwave_think.eaf"
    "${JR_DISPLAY_ASSET_STAGE}/rwave_think.eaf" COPYONLY)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware/mascot/reactive/rwave_speak.eaf"
    "${JR_DISPLAY_ASSET_STAGE}/rwave_speak.eaf" COPYONLY)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware/mascot/eaf/offline.eaf"
    "${JR_DISPLAY_ASSET_STAGE}/error.eaf" COPYONLY)
# 2026-09-02: the quiet faces (rest / muted / link), 0.93 MB together.
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware/mascot/reactive/rwave_rest.eaf"
    "${JR_DISPLAY_ASSET_STAGE}/rwave_rest.eaf" COPYONLY)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware/mascot/reactive/rwave_muted.eaf"
    "${JR_DISPLAY_ASSET_STAGE}/rwave_muted.eaf" COPYONLY)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware/mascot/reactive/rwave_link.eaf"
    "${JR_DISPLAY_ASSET_STAGE}/rwave_link.eaf" COPYONLY)

spiffs_create_partition_image(emote_assets "${JR_DISPLAY_ASSET_STAGE}" FLASH_IN_PROJECT)
