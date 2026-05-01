package com.ingeniousdigital.jarvisnano.ui.theme

import androidx.compose.ui.graphics.Color

// Mirrors values/colors.xml — kept in Kotlin so Compose code doesn't need to round-trip
// through the resource system for every paint.
object IgdPalette {
    val Background = Color(0xFF0A0A0A)
    val Surface = Color(0xFF141414)
    val SurfaceElevated = Color(0xFF1D1D1D)

    val Orange = Color(0xFFFF5722)        // primary accent
    val Amber = Color(0xFFFFB400)         // secondary accent
    val Green = Color(0xFF3FCB6F)         // success / online
    val Red = Color(0xFFFF3B3B)           // error / offline
    val Cyan = Color(0xFF00B3FF)          // info / streaming

    val Foreground = Color(0xFFE8E8E8)
    val ForegroundDim = Color(0xFF888888)
}
