# ui_layer — interactive, tappable UI drawn with the existing display_hal.
#
# A second display-arbiter owner. It draws via lua_module_display's display_hal
# (no LVGL, no baked emote-gfx) and grabs the board panel/io handles through the
# jarvis_board, exactly like firmware/emote.
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
        # board handle grab (Waveshare BSP display primitive)
        jarvis_board
        # display_hal_* drawing primitives (lua_module_display owns display_hal.c)
        lua_module_display
        # second-owner handoff (acquire/release/is_owner + owner-changed cb)
        display_arbiter
        # esp_lcd_panel_handle_t / esp_lcd_panel_io_handle_t types
        esp_lcd
        esp_timer
        freertos
        # Orbital Cockpit HUD (grok-hud): jarvis_imu_read() for live IMU
        # horizon/motion-radar tilt data. Added with the HUD rebase but the
        # REQUIRES entry was missing, so the #include "jarvis_imu.h" in
        # ui_layer.c failed to resolve on a clean build.
        jarvis_imu
        # Orbital Cockpit HUD: jarvis_pmic_read_battery() for the battery
        # energy arc. Same missing-REQUIRES gap as jarvis_imu above.
        jarvis_pmic
        # Orbital Cockpit HUD: #include "cap_gemini_live.h" is present for a
        # planned audio-level pulse ring (not yet called), but the header
        # still needs to resolve on the include path for the file to compile.
        cap_gemini_live
)
