package com.ingeniousdigital.jarvisnano.data

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.Transient

/**
 * Current firmware telemetry from GET /api/cockpit.
 *
 * Every nested object is defaulted so a firmware adding fields, or an older
 * build omitting a field, cannot take down the companion. [source] is local
 * provenance: it tells the UI when the compatibility fallback supplied only
 * the much smaller retired /api/status shape.
 */
@Serializable
data class DeviceCockpit(
    @SerialName("uptime_ms") val uptimeMs: Long = 0,
    val network: CockpitNetwork = CockpitNetwork(),
    val voice: CockpitVoice = CockpitVoice(),
    val display: CockpitDisplay = CockpitDisplay(),
    val touch: CockpitTouch = CockpitTouch(),
    val agent: CockpitAgent = CockpitAgent(),
    val brain: CockpitBrain = CockpitBrain(),
    @Transient val source: CockpitSource = CockpitSource.CURRENT,
)

enum class CockpitSource {
    CURRENT,
    LEGACY_STATUS,
}

@Serializable
data class CockpitNetwork(
    val connected: Boolean = false,
    val ip: String = "",
    val rssi: Int = 0,
)

@Serializable
data class CockpitVoice(
    val phase: String = "Unknown",
    @SerialName("voice_armed") val voiceArmed: Boolean = false,
    @SerialName("always_ready") val alwaysReady: Boolean = false,
    @SerialName("privacy_paused") val privacyPaused: Boolean = false,
    val capturing: Boolean = false,
    @SerialName("ws_connected") val wsConnected: Boolean = false,
    @SerialName("auto_idle_ms") val autoIdleMs: Long = 0,
    @SerialName("mic_rms") val micRms: Double = 0.0,
    @SerialName("vad_starts") val vadStarts: Long = 0,
    @SerialName("audio_diag_running") val audioDiagRunning: Boolean = false,
)

@Serializable
data class CockpitDisplay(
    val init: String = "unknown",
    @SerialName("actual_fps") val actualFps: Int = 0,
    @SerialName("flush_completions") val flushCompletions: Long = 0,
    @SerialName("flush_errors") val flushErrors: Long = 0,
    @SerialName("requested_face") val requestedFace: Int = 0,
    @SerialName("applied_face") val appliedFace: Int = 0,
)

@Serializable
data class CockpitTouch(
    val events: Long = 0,
    val last: CockpitTouchEvent = CockpitTouchEvent(),
    @SerialName("shade_open") val shadeOpen: Boolean = false,
    @SerialName("panel_touch_challenge")
    val panelTouchChallenge: CockpitTouchChallenge = CockpitTouchChallenge(),
)

@Serializable
data class CockpitTouchEvent(
    val kind: String = "none",
    val x: Int = 0,
    val y: Int = 0,
)

@Serializable
data class CockpitTouchChallenge(
    val pending: Boolean = false,
    val active: Boolean = false,
    val verified: Boolean = false,
    @SerialName("correct_rounds") val correctRounds: Int = 0,
    val wrong: Int = 0,
    @SerialName("expected_sector") val expectedSector: Int = 0,
    @SerialName("last_latency_ms") val lastLatencyMs: Long = 0,
)

@Serializable
data class CockpitAgent(
    val active: Boolean = false,
    @SerialName("revision_hwm") val revisionHighWaterMark: Long = 0,
    @SerialName("next_revision") val nextRevision: Long = 0,
    @SerialName("task_id") val taskId: String? = null,
    val revision: Long? = null,
    val state: String? = null,
    val progress: Int? = null,
    val title: String? = null,
    val summary: String? = null,
    @SerialName("ttl_ms") val ttlMs: Long? = null,
    val evidence: List<CockpitEvidence> = emptyList(),
)

@Serializable
data class CockpitEvidence(
    val label: String = "",
    val state: String = "wait",
)

@Serializable
data class CockpitBrain(
    @SerialName("voice_route") val voiceRoute: String = "cloud_gemini",
    @SerialName("desk_connected") val deskConnected: Boolean = false,
    @SerialName("private_android_ready") val privateAndroidReady: Boolean = false,
    @SerialName("private_android_reason") val privateAndroidReason: String = "BLE firmware not enabled",
    @SerialName("next_inbox_seq") val nextInboxSeq: Long = 1,
    @SerialName("event_cursor") val eventCursor: Long = 0,
    @SerialName("surface_active") val surfaceActive: Boolean = false,
    @SerialName("surface_kind") val surfaceKind: String = "none",
    @SerialName("surface_title") val surfaceTitle: String = "",
    @SerialName("surface_ttl_ms") val surfaceTtlMs: Long = 0,
)

/** Retired firmware payload retained only as a targeted 404 fallback. */
@Serializable
internal data class LegacyDeviceStatus(
    @SerialName("wifi_connected") val wifiConnected: Boolean = false,
    @SerialName("ip") val ip: String? = null,
    @SerialName("ap_ssid") val apSsid: String? = null,
    @SerialName("ap_active") val apActive: Boolean = false,
    @SerialName("wifi_mode") val wifiMode: String? = null,
    @SerialName("storage_base_path") val storageBasePath: String? = null,
)

internal fun LegacyDeviceStatus.toCockpit(): DeviceCockpit = DeviceCockpit(
    network = CockpitNetwork(
        connected = wifiConnected,
        ip = ip.orEmpty(),
    ),
    source = CockpitSource.LEGACY_STATUS,
)

/** WS event broadcast by the firmware on /ws/webim. */
@Serializable
data class WebimEvent(
    @SerialName("chat_id") val chatId: String? = null,
    @SerialName("text") val text: String = "",
    @SerialName("source") val source: String? = null,
    @SerialName("type") val type: String? = null,
)

/** GET /api/capabilities item. */
@Serializable
data class Capability(
    @SerialName("group_id") val groupId: String,
    @SerialName("display_name") val displayName: String,
    @SerialName("default_llm_visible") val defaultLlmVisible: Boolean = true,
)

@Serializable
data class CapabilityList(@SerialName("items") val items: List<Capability> = emptyList())

@Serializable
data class LuaModule(
    @SerialName("module_id") val moduleId: String,
    @SerialName("display_name") val displayName: String,
)

@Serializable
data class LuaModuleList(@SerialName("items") val items: List<LuaModule> = emptyList())

@Serializable
data class WebimStatus(
    @SerialName("ok") val ok: Boolean = false,
    @SerialName("bound") val bound: Boolean = false,
)

/** Connection state flowing through DeviceRepository. */
sealed interface ConnectionState {
    data object Disconnected : ConnectionState
    data object Searching : ConnectionState
    data class Connected(val host: String) : ConnectionState
    data class Failed(val reason: String) : ConnectionState
}

/** Chat bubble used by ChatScreen. */
data class ChatMessage(
    val id: Long,
    val author: Author,
    val text: String,
    val timestampMs: Long = System.currentTimeMillis(),
) {
    enum class Author { USER, AGENT, SYSTEM }
}
