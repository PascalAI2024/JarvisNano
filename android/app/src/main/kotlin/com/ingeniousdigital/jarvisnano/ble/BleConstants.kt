package com.ingeniousdigital.jarvisnano.ble

import java.util.UUID

/**
 * Phase 2 — BLE GATT bridge.
 *
 * Canonical UUIDv5 values from docs/PROTOCOL.md. Keep these in lockstep with
 * the firmware GATT service; clients should never invent short-form aliases.
 */
object BleConstants {
    val SERVICE_UUID: UUID = UUID.fromString("1ec185cd-4bc7-5797-a8b1-0f5b66c59757")

    /** Notify-only. Carries PCM chunks captured by the device microphone. */
    val CHAR_AUDIO_IN: UUID = UUID.fromString("ca04b99f-5e74-5a35-8f4f-d1313f19b29b")

    /** Write. Phone streams PCM chunks (TTS playback) to the device. */
    val CHAR_AUDIO_OUT: UUID = UUID.fromString("872228b7-ccd8-55dd-b12b-5d0352903617")

    /** Notify-only. JSON state events (mode, battery, error). */
    val CHAR_STATE: UUID = UUID.fromString("dab5c3d4-915d-5f25-acc9-9d511df742bf")

    /** Write. Short JSON commands (start_listening, stop, restart, mode_switch). */
    val CHAR_CONTROL: UUID = UUID.fromString("2e14c0f2-4b07-5802-a8f9-369752d7cf2a")

    /** Standard Client Characteristic Configuration descriptor — needed to enable notifies. */
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")

    /** Device name prefix(es) we'll accept while scanning. */
    val NAME_PREFIXES: List<String> = listOf("esp-claw", "JarvisNano")
}
