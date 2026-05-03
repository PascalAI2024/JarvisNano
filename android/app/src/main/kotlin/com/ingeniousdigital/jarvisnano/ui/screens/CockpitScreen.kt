package com.ingeniousdigital.jarvisnano.ui.screens

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import com.ingeniousdigital.jarvisnano.ble.BleConstants
import com.ingeniousdigital.jarvisnano.ble.BleClient
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
fun CockpitScreen(repository: DeviceRepository, bleClient: BleClient) {
    val connection by repository.connection.collectAsState()
    val bleState by bleClient.state.collectAsState()
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
        item { BleTile(bleClient, bleState) }
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
private fun BleTile(bleClient: BleClient, state: BleClient.State) {
    val context = LocalContext.current
    var permissionMessage by remember { mutableStateOf<String?>(null) }
    val permissions = remember { bleRuntimePermissions() }
    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { result ->
        val granted = permissions.all { permission ->
            result[permission] == true ||
                ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED
        }
        if (granted) {
            permissionMessage = null
            bleClient.startScan()
        } else {
            permissionMessage = "Bluetooth permission denied."
        }
    }

    fun startScanWithPermissions() {
        val missing = permissions.filter {
            ContextCompat.checkSelfPermission(context, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) {
            permissionMessage = null
            bleClient.startScan()
        } else {
            launcher.launch(missing.toTypedArray())
        }
    }

    Tile(title = "Bluetooth", accent = IgdPalette.Cyan) {
        when (state) {
            BleClient.State.Idle -> {
                TileRow("State", "idle")
                TileRow("Service", "waiting for scan")
            }
            BleClient.State.Scanning -> {
                TileRow("State", "scanning")
                TileRow("Name prefix", BleConstants.NAME_PREFIXES.joinToString(" / "))
            }
            is BleClient.State.Found -> {
                TileRow("State", "found")
                TileRow("Device", state.name)
                TileRow("Address", state.address, monospaceValue = true)
            }
            BleClient.State.Connecting -> {
                TileRow("State", "connecting")
                TileRow("Service", "discovering")
            }
            is BleClient.State.Connected -> {
                TileRow("State", "connected")
                TileRow("Device", state.name)
                TileRow(
                    "Jarvis GATT",
                    when (state.jarvisServicePresent) {
                        true -> "present"
                        false -> "missing"
                        null -> "discovering"
                    },
                    monospaceValue = state.jarvisServicePresent != true,
                )
            }
            is BleClient.State.Failed -> {
                TileRow("State", "failed")
                TileRow("Reason", state.reason)
            }
        }

        permissionMessage?.let {
            Spacer(Modifier.height(8.dp))
            Text(
                it,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
            )
        }

        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = { startScanWithPermissions() },
                enabled = state !is BleClient.State.Scanning && state !is BleClient.State.Connecting,
            ) { Text("Scan") }

            if (state is BleClient.State.Found) {
                OutlinedButton(onClick = { bleClient.connect(state.device) }) { Text("Connect") }
            }

            if (state is BleClient.State.Connected) {
                OutlinedButton(onClick = { bleClient.disconnect() }) { Text("Disconnect") }
            }
        }
    }
}

private fun bleRuntimePermissions(): List<String> =
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        listOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
        )
    } else {
        listOf(Manifest.permission.ACCESS_FINE_LOCATION)
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
