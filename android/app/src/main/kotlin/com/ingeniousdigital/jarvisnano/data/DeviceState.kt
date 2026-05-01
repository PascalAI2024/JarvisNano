package com.ingeniousdigital.jarvisnano.data

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * Mirrors the firmware's GET /api/status payload.
 *
 * Fields are nullable / defaulted because the firmware may grow the schema
 * faster than this app gets updated.
 */
@Serializable
data class DeviceStatus(
    @SerialName("wifi_connected") val wifiConnected: Boolean = false,
    @SerialName("ip") val ip: String? = null,
    @SerialName("ap_ssid") val apSsid: String? = null,
    @SerialName("ap_active") val apActive: Boolean = false,
    @SerialName("wifi_mode") val wifiMode: String? = null,
    @SerialName("storage_base_path") val storageBasePath: String? = null,
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
