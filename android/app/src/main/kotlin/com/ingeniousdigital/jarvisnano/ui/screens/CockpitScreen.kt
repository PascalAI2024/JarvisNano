package com.ingeniousdigital.jarvisnano.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.ingeniousdigital.jarvisnano.data.Capability
import com.ingeniousdigital.jarvisnano.data.ConnectionState
import com.ingeniousdigital.jarvisnano.data.DeviceRepository
import com.ingeniousdigital.jarvisnano.ui.components.StatusOrb
import com.ingeniousdigital.jarvisnano.ui.components.Tile
import com.ingeniousdigital.jarvisnano.ui.components.TileRow
import com.ingeniousdigital.jarvisnano.ui.theme.IgdPalette
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch

@Composable
fun CockpitScreen(repository: DeviceRepository) {
    val connection by repository.connection.collectAsState()
    val status by repository.status.collectAsState()
    var capabilities by remember { mutableStateOf<List<Capability>>(emptyList()) }
    val scope = rememberCoroutineScope()

    LaunchedEffect(Unit) {
        repository.startDiscovery()
    }

    LaunchedEffect(connection) {
        if (connection is ConnectionState.Connected) {
            runCatching { repository.loadCapabilities() }
                .onSuccess { capabilities = it.items }
            // Begin polling — runs until the composition leaves.
            repository.observeStatus().collectLatest { /* status flow updates StateFlow */ }
        }
    }

    LazyColumn(
        modifier = Modifier
            .fillMaxWidth()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item { ConnectionHeader(connection) }
        item { SystemTile(status) }
        item { WifiTile(status) }
        item { LlmTile(connection) }
        item { CapabilitiesTile(capabilities) }
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Button(
                    onClick = { scope.launch { runCatching { repository.restart() } } },
                    colors = ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.primary,
                        contentColor = MaterialTheme.colorScheme.onPrimary,
                    ),
                    modifier = Modifier.weight(1f),
                ) { Text("Restart device") }
                OutlinedButton(
                    onClick = { /* New session is just a chat-side concern; placeholder. */ },
                    modifier = Modifier.weight(1f),
                ) { Text("New session") }
            }
        }
    }
}

@Composable
private fun ConnectionHeader(state: ConnectionState) {
    val (color, label, host) = when (state) {
        is ConnectionState.Connected -> Triple(IgdPalette.Green, "Online", state.host)
        is ConnectionState.Searching -> Triple(IgdPalette.Amber, "Searching", "esp-claw.local")
        is ConnectionState.Failed -> Triple(IgdPalette.Red, "Offline", state.reason)
        ConnectionState.Disconnected -> Triple(IgdPalette.ForegroundDim, "Idle", "—")
    }
    Surface(
        modifier = Modifier.fillMaxWidth(),
        color = MaterialTheme.colorScheme.surface,
        shape = androidx.compose.foundation.shape.RoundedCornerShape(20.dp),
    ) {
        Row(
            modifier = Modifier.padding(20.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(modifier = Modifier.size(56.dp), contentAlignment = Alignment.Center) {
                Surface(
                    modifier = Modifier.size(56.dp),
                    shape = CircleShape,
                    color = color.copy(alpha = 0.15f),
                ) {}
                StatusOrb(size = 18, color = color)
            }
            Spacer(Modifier.size(16.dp))
            Column {
                Text(label, style = MaterialTheme.typography.headlineMedium)
                Spacer(Modifier.height(2.dp))
                Text(
                    host,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun SystemTile(status: com.ingeniousdigital.jarvisnano.data.DeviceStatus?) {
    Tile(title = "System", accent = IgdPalette.Cyan) {
        TileRow("Wi-Fi mode", status?.wifiMode ?: "—")
        TileRow("Storage", status?.storageBasePath ?: "—", monospaceValue = true)
        TileRow("Access point", if (status?.apActive == true) "active" else "off")
    }
}

@Composable
private fun WifiTile(status: com.ingeniousdigital.jarvisnano.data.DeviceStatus?) {
    Tile(
        title = "Wi-Fi",
        accent = if (status?.wifiConnected == true) IgdPalette.Green else IgdPalette.Red,
    ) {
        TileRow("Connected", if (status?.wifiConnected == true) "yes" else "no")
        TileRow("IP", status?.ip ?: "—", monospaceValue = true)
        TileRow("AP SSID", status?.apSsid ?: "—")
    }
}

@Composable
private fun LlmTile(state: ConnectionState) {
    Tile(title = "LLM", accent = IgdPalette.Orange) {
        when (state) {
            is ConnectionState.Connected -> {
                TileRow("Channel", "WebSocket /ws/webim", monospaceValue = true)
                TileRow("Provider", "as configured on device")
            }
            else -> {
                TileRow("Channel", "—")
                TileRow("Provider", "—")
            }
        }
    }
}

@Composable
private fun CapabilitiesTile(items: List<Capability>) {
    Tile(title = "Capabilities", accent = IgdPalette.Amber) {
        if (items.isEmpty()) {
            Text(
                "No capabilities reported.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                items.forEach { cap ->
                    TileRow(cap.displayName, cap.groupId, monospaceValue = true)
                }
            }
        }
    }
}
