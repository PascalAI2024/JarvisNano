package com.ingeniousdigital.jarvisnano.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import com.ingeniousdigital.jarvisnano.data.ConfigGroup
import com.ingeniousdigital.jarvisnano.data.ConnectionState
import com.ingeniousdigital.jarvisnano.data.DeviceConfig
import com.ingeniousdigital.jarvisnano.data.DeviceRepository
import com.ingeniousdigital.jarvisnano.ui.components.Tile
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull

@Composable
fun SettingsScreen(repository: DeviceRepository) {
    val connection by repository.connection.collectAsState()
    val manualOverride by repository.manualOverride.collectAsState()

    var loaded by remember { mutableStateOf<DeviceConfig?>(null) }
    val edits = remember { mutableStateMapOf<String, String>() }
    val unmasked = remember { mutableStateMapOf<String, Boolean>() }
    val scope = rememberCoroutineScope()
    var status by remember { mutableStateOf<String?>(null) }
    var manualHostInput by remember { mutableStateOf(manualOverride.orEmpty()) }

    LaunchedEffect(connection) {
        if (connection is ConnectionState.Connected && loaded == null) {
            runCatching { repository.loadConfig() }
                .onSuccess { config ->
                    loaded = config
                    edits.clear()
                    config.raw.forEach { (k, v) -> edits[k] = displayValue(v) }
                }
                .onFailure { status = "Could not load config: ${it.message}" }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {

        Tile(title = "Device address") {
            Text(
                "Override the auto-discovered host. Leave blank to use mDNS.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.size(8.dp))
            OutlinedTextField(
                value = manualHostInput,
                onValueChange = { manualHostInput = it },
                label = { Text("hostname or IP") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.size(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { repository.setManualHost(manualHostInput) }) { Text("Apply") }
                OutlinedButton(onClick = {
                    manualHostInput = ""
                    repository.setManualHost(null)
                    repository.startDiscovery()
                }) { Text("Auto-discover") }
            }
        }

        val config = loaded
        if (config == null) {
            Tile(title = "Configuration") {
                Text(
                    if (connection is ConnectionState.Connected) "Loading…" else "Connect to the device first.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        } else {
            ConfigGroup.values().forEach { group ->
                val keys = config.raw.keys.filter { DeviceConfig.groupOf(it) == group }
                if (keys.isEmpty()) return@forEach
                Tile(title = group.label) {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        keys.sorted().forEach { key ->
                            ConfigField(
                                key = key,
                                value = edits[key].orEmpty(),
                                sensitive = DeviceConfig.isSensitive(key),
                                visible = unmasked[key] == true,
                                onValueChange = { edits[key] = it },
                                onToggleVisibility = { unmasked[key] = unmasked[key] != true },
                            )
                        }
                    }
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = {
                        scope.launch {
                            val patch = buildConfigPatch(config, edits)
                            if (patch.raw.isEmpty()) {
                                status = "No changes to save."
                                return@launch
                            }
                            runCatching { repository.saveConfig(patch) }
                                .onSuccess {
                                    loaded = mergeConfig(config, patch)
                                    status = "Saved."
                                }
                                .onFailure { status = "Save failed: ${it.message}" }
                        }
                    },
                ) { Text("Save") }
                OutlinedButton(
                    onClick = {
                        scope.launch {
                            val patch = buildConfigPatch(config, edits)
                            if (patch.raw.isEmpty()) {
                                status = "No changes to save."
                                return@launch
                            }
                            runCatching {
                                repository.saveConfig(patch)
                                repository.restart()
                            }.onSuccess {
                                loaded = mergeConfig(config, patch)
                                status = "Saved + restart requested."
                            }
                                .onFailure { status = "Save failed: ${it.message}" }
                        }
                    },
                ) { Text("Save and restart") }
            }
        }

        status?.let {
            Text(
                text = it,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.primary,
            )
        }
    }
}

@Composable
private fun ConfigField(
    key: String,
    value: String,
    sensitive: Boolean,
    visible: Boolean,
    onValueChange: (String) -> Unit,
    onToggleVisibility: () -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        label = { Text(key) },
        modifier = Modifier.fillMaxWidth(),
        singleLine = true,
        visualTransformation = if (sensitive && !visible) PasswordVisualTransformation() else VisualTransformation.None,
        trailingIcon = if (sensitive) {
            {
                IconButton(onClick = onToggleVisibility) {
                    Icon(
                        imageVector = if (visible) Icons.Filled.VisibilityOff else Icons.Filled.Visibility,
                        contentDescription = if (visible) "Hide" else "Show",
                    )
                }
            }
        } else null,
    )
}

private fun buildConfigPatch(
    base: DeviceConfig,
    edits: Map<String, String>,
): DeviceConfig {
    val patch = mutableMapOf<String, JsonElement>()
    edits.forEach { (key, value) ->
        val original = base.raw[key]
        if (original != null && value == displayValue(original)) return@forEach
        patch[key] = JsonPrimitive(value)
    }
    return DeviceConfig(raw = JsonObject(patch))
}

private fun displayValue(value: JsonElement): String =
    (value as? JsonPrimitive)?.contentOrNull ?: value.toString()

private fun mergeConfig(base: DeviceConfig, patch: DeviceConfig): DeviceConfig {
    val merged = base.raw.toMutableMap()
    merged.putAll(patch.raw)
    return DeviceConfig(raw = JsonObject(merged))
}
