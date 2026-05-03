package com.ingeniousdigital.jarvisnano.ui.nav

import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Chat
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.PhotoCamera
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.ingeniousdigital.jarvisnano.R
import com.ingeniousdigital.jarvisnano.ble.BleClient
import com.ingeniousdigital.jarvisnano.data.DeviceRepository
import com.ingeniousdigital.jarvisnano.ui.screens.AboutScreen
import com.ingeniousdigital.jarvisnano.ui.screens.CameraScreen
import com.ingeniousdigital.jarvisnano.ui.screens.ChatScreen
import com.ingeniousdigital.jarvisnano.ui.screens.CockpitScreen
import com.ingeniousdigital.jarvisnano.ui.screens.SettingsScreen

private enum class Destination(val route: String, val labelRes: Int, val icon: ImageVector) {
    Cockpit("cockpit", R.string.nav_cockpit, Icons.Filled.Speed),
    Chat("chat", R.string.nav_chat, Icons.Filled.Chat),
    Camera("camera", R.string.nav_camera, Icons.Filled.PhotoCamera),
    Settings("settings", R.string.nav_settings, Icons.Filled.Settings),
    About("about", R.string.nav_about, Icons.Filled.Info),
}

@Composable
fun JarvisNanoNavHost(
    repository: DeviceRepository,
    bleClient: BleClient,
    sessionId: String,
) {
    val nav = rememberNavController()
    val backStack by nav.currentBackStackEntryAsState()
    val current = backStack?.destination

    Scaffold(
        bottomBar = {
            NavigationBar {
                Destination.values().forEach { dest ->
                    val selected = current?.hierarchy?.any { it.route == dest.route } == true
                    NavigationBarItem(
                        selected = selected,
                        onClick = {
                            nav.navigate(dest.route) {
                                popUpTo(nav.graph.findStartDestination().id) { saveState = true }
                                launchSingleTop = true
                                restoreState = true
                            }
                        },
                        icon = { Icon(dest.icon, contentDescription = null) },
                        label = { Text(stringResource(dest.labelRes)) },
                        colors = NavigationBarItemDefaults.colors(
                            selectedIconColor = androidx.compose.material3.MaterialTheme.colorScheme.primary,
                            selectedTextColor = androidx.compose.material3.MaterialTheme.colorScheme.primary,
                            indicatorColor = androidx.compose.material3.MaterialTheme.colorScheme.surfaceVariant,
                        ),
                    )
                }
            }
        }
    ) { padding ->
        NavHost(
            navController = nav,
            startDestination = Destination.Cockpit.route,
            modifier = Modifier.padding(padding),
        ) {
            composable(Destination.Cockpit.route) { CockpitScreen(repository, bleClient) }
            composable(Destination.Chat.route) { ChatScreen(repository, sessionId) }
            composable(Destination.Camera.route) { CameraScreen(repository) }
            composable(Destination.Settings.route) { SettingsScreen(repository) }
            composable(Destination.About.route) { AboutScreen() }
        }
    }
}
