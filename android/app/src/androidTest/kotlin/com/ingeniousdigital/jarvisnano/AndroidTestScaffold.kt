package com.ingeniousdigital.jarvisnano

/**
 * Compile-only placeholder for device and emulator tests.
 *
 * BLE acceptance still requires a physical Android device because standard
 * emulators do not expose a virtual BLE radio.
 */
internal object AndroidTestScaffold {
    const val sourceSet = "androidTest"
    const val compileCommand = ":app:compileDebugAndroidTestKotlin"
}
