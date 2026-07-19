package com.ingeniousdigital.jarvisnano.data

import com.ingeniousdigital.jarvisnano.discovery.Discovery
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/**
 * Single source of truth for "what's our device?" + cached telemetry.
 *
 * Responsibilities:
 *   - Track the active host (auto-discovered via mDNS or manually set in Settings).
 *   - Expose [connection] as a StateFlow the UI can render.
 *   - Provide an [observeCockpit] flow that polls /api/cockpit every [POLL_INTERVAL_MS]
 *     once a host is known.
 *   - Keep the selected [BrainRoute] explicit. A route becoming unavailable
 *     updates readiness but never silently selects another route.
 *   - Forward HTTP/WS calls to [DeviceClient] using the cached host.
 */
class DeviceRepository(
    private val client: DeviceClient,
    private val discovery: Discovery,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO),
) {
    private val hostLock = Mutex()

    private val _connection = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)
    val connection: StateFlow<ConnectionState> = _connection.asStateFlow()

    private val _manualOverride = MutableStateFlow<String?>(null)
    val manualOverride: StateFlow<String?> = _manualOverride.asStateFlow()

    /** Last successful cockpit snapshot. Null until the first probe succeeds. */
    private val _cockpit = MutableStateFlow<DeviceCockpit?>(null)
    val cockpit: StateFlow<DeviceCockpit?> = _cockpit.asStateFlow()

    private val _brainRoute = MutableStateFlow(BrainRouteState())
    val brainRoute: StateFlow<BrainRouteState> = _brainRoute.asStateFlow()

    /** Currently resolved host (hostname, IP, or null). */
    @Volatile private var host: String? = null
    @Volatile private var cockpitFailures: Int = 0

    fun selectBrainRoute(route: BrainRoute) {
        _brainRoute.update { it.select(route) }
    }

    fun updatePrivateAndroidReadiness(
        bleConnected: Boolean,
        servicePresent: Boolean?,
        localBrainReady: Boolean,
    ) {
        _brainRoute.update {
            it.withPrivateAndroid(bleConnected, servicePresent, localBrainReady)
        }
    }

    fun setManualHost(value: String?) {
        val manualHost = value?.let(::cleanHost)?.takeIf { it.isNotBlank() }
        _manualOverride.value = manualHost
        host = manualHost
        cockpitFailures = 0
        if (manualHost == null) {
            _connection.value = ConnectionState.Disconnected
            publishCockpit(null)
            return
        }

        _connection.value = ConnectionState.Searching
        scope.launch {
            runCatching { client.getCockpit(manualHost) }
                .onSuccess {
                    if (_manualOverride.value == manualHost) {
                        publishCockpit(it)
                        _connection.value = ConnectionState.Connected(manualHost)
                    }
                }
                .onFailure { t ->
                    if (_manualOverride.value == manualHost) {
                        _connection.value = ConnectionState.Failed(t.message ?: "manual host probe failed")
                    }
                }
        }
    }

    /**
     * Kick off mDNS discovery and update [connection] as results arrive.
     * Idempotent — safe to call from each screen's LaunchedEffect.
     */
    fun startDiscovery() {
        if (host != null) return
        if (_connection.value is ConnectionState.Searching) return
        _connection.value = ConnectionState.Searching
        scope.launch {
            runCatching {
                val resolved = discovery.findEspClaw()
                val snapshot = client.getCockpit(resolved)
                resolved.also { verified ->
                    hostLock.withLock {
                        if (_manualOverride.value == null) {
                            host = verified
                            cockpitFailures = 0
                            publishCockpit(snapshot)
                            _connection.value = ConnectionState.Connected(verified)
                        }
                    }
                }
            }.onFailure { t ->
                if (_manualOverride.value != null) return@onFailure
                _connection.value = ConnectionState.Failed(t.message ?: "discovery failed")
            }
        }
    }

    /**
     * Cold flow that emits the latest cockpit every [POLL_INTERVAL_MS] once a host
     * is known. Errors are swallowed but flip the connection state to Failed so the
     * UI can surface them.
     */
    fun observeCockpit(): Flow<DeviceCockpit> = flow {
        while (true) {
            val h = host
            if (h != null) {
                runCatching { client.getCockpit(h) }
                    .onSuccess {
                        cockpitFailures = 0
                        publishCockpit(it)
                        _connection.value = ConnectionState.Connected(h)
                        emit(it)
                    }
                    .onFailure {
                        cockpitFailures += 1
                        _connection.value = ConnectionState.Failed(it.message ?: "cockpit poll failed")
                        if (_manualOverride.value == null && cockpitFailures >= MAX_COCKPIT_FAILURES) {
                            host = null
                            publishCockpit(null)
                            cockpitFailures = 0
                            startDiscovery()
                        }
                    }
            }
            delay(POLL_INTERVAL_MS)
        }
    }

    suspend fun loadConfig(): DeviceConfig =
        client.getConfig(requireHost())

    suspend fun saveConfig(config: DeviceConfig) =
        client.putConfig(requireHost(), config)

    suspend fun loadCapabilities(): CapabilityList =
        client.getCapabilities(requireHost())

    suspend fun loadLuaModules(): LuaModuleList =
        client.getLuaModules(requireHost())

    suspend fun loadWebimStatus(): WebimStatus =
        client.getWebimStatus(requireHost())

    suspend fun sendChat(chatId: String, text: String) =
        client.sendWebim(requireHost(), chatId, text)

    suspend fun restart() = client.restart(requireHost())

    suspend fun loadSnapshot(): ByteArray = client.fetchSnapshot(requireHost())

    fun openChatSocket(): Flow<WebimEvent> =
        client.openWebimSocket(requireHost())

    private fun publishCockpit(snapshot: DeviceCockpit?) {
        _cockpit.value = snapshot
        _brainRoute.update { it.withCockpit(snapshot) }
    }

    private fun requireHost(): String =
        host ?: error("device host not set — discovery hasn't completed and no manual override configured")

    private fun cleanHost(value: String): String =
        value.trim()
            .removePrefix("http://")
            .removePrefix("https://")
            .substringBefore('/')
            .substringBefore('?')
            .substringBefore('#')
            .trimEnd('/')

    companion object {
        const val POLL_INTERVAL_MS: Long = 4_000L
        private const val MAX_COCKPIT_FAILURES: Int = 3
    }
}
