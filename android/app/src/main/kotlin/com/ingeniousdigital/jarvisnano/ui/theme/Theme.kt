package com.ingeniousdigital.jarvisnano.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

/**
 * Force-dark Material 3 theme tuned to the IGD palette. The dashboard is dark-only
 * and JarvisNano follows suit — system isSystemInDarkTheme() is intentionally ignored.
 */
@Composable
fun JarvisNanoTheme(content: @Composable () -> Unit) {
    val colors = darkColorScheme(
        primary = IgdPalette.Orange,
        onPrimary = IgdPalette.Background,
        primaryContainer = IgdPalette.SurfaceElevated,
        onPrimaryContainer = IgdPalette.Orange,

        secondary = IgdPalette.Amber,
        onSecondary = IgdPalette.Background,

        tertiary = IgdPalette.Cyan,
        onTertiary = IgdPalette.Background,

        background = IgdPalette.Background,
        onBackground = IgdPalette.Foreground,

        surface = IgdPalette.Surface,
        onSurface = IgdPalette.Foreground,
        surfaceVariant = IgdPalette.SurfaceElevated,
        onSurfaceVariant = IgdPalette.ForegroundDim,

        error = IgdPalette.Red,
        onError = IgdPalette.Foreground,

        outline = IgdPalette.ForegroundDim,
    )

    MaterialTheme(
        colorScheme = colors,
        typography = JarvisTypography,
        content = content,
    )
}
