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
import androidx.compose.foundation.shape.RoundedCornerShape
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
import com.ingeniousdigital.jarvisnano.data.BrainRoute
import com.ingeniousdigital.jarvisnano.data.BrainRouteReadiness
import com.ingeniousdigital.jarvisnano.data.BrainRouteState
import com.ingeniousdigital.jarvisnano.data.CockpitAgent
import com.ingeniousdigital.jarvisnano.data.CockpitSource
import com.ingeniousdigital.jarvisnano.data.ConnectionState
import com.ingeniousdigital.jarvisnano.data.DeviceCockpit
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
    val cockpit by repository.cockpit.collectAsState()
    val brainRoute by repository.brainRoute.collectAsState()
    val scope = rememberCoroutineScope()

    LaunchedEffect(Unit) {
        repository.startDiscovery()
        // Keep the poller alive through transient Failed states so its
        // three-failure rediscovery policy can actually run.
        repository.observeCockpit().collectLatest { /* StateFlow owns the snapshot. */ }
    }

    // The BLE skeleton can prove the transport and canonical service today.
    // LocalLlm is still interface-only, so private readiness remains honest.
    LaunchedEffect(bleState) {
        val connected = bleState as? BleClient.State.Connected
        repository.updatePrivateAndroidReadiness(
            bleConnected = connected != null,
            servicePresent = connected?.jarvisServicePresent,
            localBrainReady = false,
        )
    }

    LazyColumn(
        modifier = Modifier
            .fillMaxWidth()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item { ConnectionHeader(connection) }
        item {
            BrainRouteTile(
                state = brainRoute,
                onSelect = repository::selectBrainRoute,
            )
        }
        item { VoiceTile(cockpit) }
        item { NetworkTile(cockpit) }
        item { DisplayTile(cockpit) }
        item { AgentTile(cockpit?.agent) }
        item { TouchTile(cockpit) }
        item { BleTile(bleClient, bleState) }
        item { SystemTile(cockpit) }
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
                    onClick = { /* Session ownership is route-specific; no fake reset. */ },
                    modifier = Modifier.weight(1f),
                    enabled = false,
                ) { Text("New session") }
            }
        }
    }
}

@Composable
private fun BrainRouteTile(
    state: BrainRouteState,
    onSelect: (BrainRoute) -> Unit,
) {
    val selectedStatus = state.selectedStatus
    val accent = when (selectedStatus.readiness) {
        BrainRouteReadiness.READY -> IgdPalette.Green
        BrainRouteReadiness.WAITING -> IgdPalette.Amber
        BrainRouteReadiness.UNAVAILABLE -> IgdPalette.Red
    }
    Tile(title = "Brain route", accent = accent) {
        TileRow("Selected", state.selected.label)
        TileRow("Readiness", selectedStatus.readiness.name.lowercase())
        Text(
            selectedStatus.detail,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(10.dp))
        BrainRoute.entries.forEach { route ->
            val routeStatus = state.statusFor(route)
            val label = "${route.label} · ${routeStatus.readiness.name.lowercase()}"
            if (state.selected == route) {
                Button(
                    onClick = { onSelect(route) },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text(label) }
            } else {
                OutlinedButton(
                    onClick = { onSelect(route) },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text(label) }
            }
            Text(
                routeStatus.detail,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 4.dp, bottom = 8.dp),
            )
        }
        Text(
            "An unavailable private route stays selected. Cloud fallback requires your tap.",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun VoiceTile(cockpit: DeviceCockpit?) {
    val voice = cockpit?.voice
    val accent = when {
        voice?.privacyPaused == true -> IgdPalette.Amber
        voice?.wsConnected == true -> IgdPalette.Cyan
        else -> IgdPalette.Red
    }
    Tile(title = "Voice", accent = accent) {
        TileRow("Phase", voice?.phase ?: "—")
        TileRow("Microphone", when {
            voice == null -> "—"
            voice.privacyPaused -> "privacy paused"
            voice.capturing -> "capturing"
            else -> "idle"
        })
        TileRow("Gemini socket", when (voice?.wsConnected) {
            true -> "open"
            false -> "closed"
            null -> "—"
        })
        TileRow("Voice armed", if (voice?.voiceArmed == true) "yes" else "no")
        TileRow("Always ready", if (voice?.alwaysReady == true) "yes" else "no")
        TileRow("Mic RMS", voice?.let { "%.1f".format(it.micRms) } ?: "—", monospaceValue = true)
        TileRow("VAD starts", voice?.vadStarts?.toString() ?: "—", monospaceValue = true)
    }
}

@Composable
private fun NetworkTile(cockpit: DeviceCockpit?) {
    val network = cockpit?.network
    Tile(
        title = "Network",
        accent = if (network?.connected == true) IgdPalette.Green else IgdPalette.Red,
    ) {
        TileRow("Connected", when (network?.connected) {
            true -> "yes"
            false -> "no"
            null -> "—"
        })
        TileRow("IP", network?.ip?.takeIf(String::isNotBlank) ?: "—", monospaceValue = true)
        TileRow(
            "RSSI",
            network?.takeIf { it.connected }?.let { "${it.rssi} dBm" } ?: "—",
            monospaceValue = true,
        )
    }
}

@Composable
private fun DisplayTile(cockpit: DeviceCockpit?) {
    val display = cockpit?.display
    val accent = when {
        display?.flushErrors?.let { it > 0 } == true -> IgdPalette.Red
        display?.init == "ready" -> IgdPalette.Green
        else -> IgdPalette.Amber
    }
    Tile(title = "Round display", accent = accent) {
        TileRow("Presenter", display?.init ?: "—")
        TileRow("Frame rate", display?.let { "${it.actualFps} fps" } ?: "—", monospaceValue = true)
        TileRow("Flushes", display?.flushCompletions?.toString() ?: "—", monospaceValue = true)
        TileRow("Errors", display?.flushErrors?.toString() ?: "—", monospaceValue = true)
        TileRow(
            "Face",
            display?.let { "${it.requestedFace} → ${it.appliedFace}" } ?: "—",
            monospaceValue = true,
        )
    }
}

@Composable
private fun AgentTile(agent: CockpitAgent?) {
    Tile(
        title = "Codex Agent Link",
        accent = if (agent?.active == true) IgdPalette.Violet else IgdPalette.ForegroundDim,
    ) {
        if (agent?.active != true) {
            TileRow("State", "waiting")
            TileRow("Next revision", agent?.nextRevision?.toString() ?: "—", monospaceValue = true)
            Text(
                "No bounded Codex task is currently linked.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            TileRow("State", agent.state ?: "working")
            TileRow("Progress", "${agent.progress ?: 0}%")
            TileRow("Task", agent.title ?: agent.taskId ?: "active")
            agent.summary?.takeIf(String::isNotBlank)?.let {
                Text(
                    it,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(vertical = 6.dp),
                )
            }
            agent.evidence.forEach { evidence ->
                TileRow(evidence.label, evidence.state)
            }
        }
    }
}

@Composable
private fun TouchTile(cockpit: DeviceCockpit?) {
    val touch = cockpit?.touch
    val challenge = touch?.panelTouchChallenge
    Tile(title = "Touch + shade", accent = IgdPalette.Cyan) {
        TileRow("Events", touch?.events?.toString() ?: "—", monospaceValue = true)
        TileRow(
            "Last",
            touch?.last?.let { "${it.kind} · ${it.x},${it.y}" } ?: "—",
            monospaceValue = true,
        )
        TileRow("Shade", if (touch?.shadeOpen == true) "open" else "closed")
        TileRow("Physical proof", when {
            challenge == null -> "—"
            challenge.verified -> "verified"
            challenge.active -> "round ${challenge.correctRounds + 1}"
            challenge.pending -> "queued"
            else -> "not run"
        })
    }
}

@Composable
private fun SystemTile(cockpit: DeviceCockpit?) {
    Tile(title = "System truth", accent = IgdPalette.Cyan) {
        TileRow("Uptime", cockpit?.let { formatUptime(it.uptimeMs) } ?: "—", monospaceValue = true)
        TileRow("Telemetry", when (cockpit?.source) {
            CockpitSource.CURRENT -> "/api/cockpit"
            CockpitSource.LEGACY_STATUS -> "legacy fallback"
            null -> "—"
        }, monospaceValue = true)
        TileRow("Audio diagnostic", if (cockpit?.voice?.audioDiagRunning == true) "running" else "idle")
    }
}

private fun formatUptime(uptimeMs: Long): String {
    val totalSeconds = uptimeMs.coerceAtLeast(0) / 1_000
    val hours = totalSeconds / 3_600
    val minutes = (totalSeconds % 3_600) / 60
    val seconds = totalSeconds % 60
    return "%02d:%02d:%02d".format(hours, minutes, seconds)
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
        is ConnectionState.Searching -> Triple(IgdPalette.Amber, "Searching", "JarvisNano on LAN")
        is ConnectionState.Failed -> Triple(IgdPalette.Red, "Offline", state.reason)
        ConnectionState.Disconnected -> Triple(IgdPalette.ForegroundDim, "Idle", "—")
    }
    Surface(
        modifier = Modifier.fillMaxWidth(),
        color = MaterialTheme.colorScheme.surface,
        shape = RoundedCornerShape(20.dp),
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
