package com.ingeniousdigital.jarvisnano.ble

import java.util.UUID

/**
 * Phase 2 — BLE GATT bridge.
 *
 * The firmware will expose a custom service with the four characteristics below.
 * UUIDs are placeholders and will be locked down once the firmware-side service
 * is registered. Don't ship these in a release until they match the firmware.
 */
object BleConstants {
    // Replace with the firmware's actual base UUID when published.
    val SERVICE_UUID: UUID = UUID.fromString("0000A100-0000-1000-8000-00805F9B34FB")

    /** Notify-only. Carries PCM chunks captured by the device microphone. */
    val CHAR_AUDIO_IN: UUID = UUID.fromString("0000A101-0000-1000-8000-00805F9B34FB")

    /** Write. Phone streams PCM chunks (TTS playback) to the device. */
    val CHAR_AUDIO_OUT: UUID = UUID.fromString("0000A102-0000-1000-8000-00805F9B34FB")

    /** Notify-only. JSON state events (mode, battery, error). */
    val CHAR_STATE: UUID = UUID.fromString("0000A103-0000-1000-8000-00805F9B34FB")

    /** Write. Short JSON commands (start_listening, stop, restart, mode_switch). */
    val CHAR_CONTROL: UUID = UUID.fromString("0000A104-0000-1000-8000-00805F9B34FB")

    /** Standard Client Characteristic Configuration descriptor — needed to enable notifies. */
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")

    /** Device name prefix(es) we'll accept while scanning. */
    val NAME_PREFIXES: List<String> = listOf("esp-claw", "JarvisNano")
}
