# ui_layer — interactive, tappable UI drawn with the existing display_hal.
#
# A second display-arbiter owner. It draws via lua_module_display's display_hal
# (no LVGL, no baked emote-gfx) and grabs the board panel/io handles through the
# board manager, exactly like firmware/emote.
#
# Wiring: bootstrap.sh mirrors firmware/ui_layer/ -> esp-claw/components/common/
# ui_layer (see copy_ui_layer_runtime, parallel to copy_emote_runtime). Edit
# THIS canonical copy, never the generated esp-claw copy.
idf_component_register(
    SRCS
        "ui_layer.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        # board handle grab (esp_board_manager_get_device_handle + display_lcd cfg)
        espressif__esp_board_manager
        # display_hal_* drawing primitives (lua_module_display owns display_hal.c)
        lua_module_display
        # second-owner handoff (acquire/release/is_owner + owner-changed cb)
        display_arbiter
        # esp_lcd_panel_handle_t / esp_lcd_panel_io_handle_t types
        esp_lcd
        esp_timer
        freertos
)
