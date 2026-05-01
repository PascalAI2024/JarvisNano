package com.ingeniousdigital.jarvisnano.data

import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject

/**
 * The firmware's /api/config payload is loosely typed and grows often.
 * Rather than maintain a brittle data class, we keep the raw [JsonObject]
 * and expose typed accessors only for the fields the UI explicitly edits.
 *
 * [SettingsScreen] renders the config grouped by prefix:
 *   - "wifi_*"  → Wi-Fi
 *   - "llm_*"   → LLM provider
 *   - "im_*"    → Messaging
 *   - everything else → Misc
 *
 * Sensitive keys (rendered with an eye-toggle) are matched by suffix:
 *   *_password, *_pass, *_secret, *_token, *_api_key.
 */
data class DeviceConfig(
    val raw: JsonObject,
) {
    fun get(key: String): JsonElement? = raw[key]

    /** Returns a new copy with [key] set to [value]. */
    fun with(key: String, value: JsonElement): DeviceConfig {
        val next = raw.toMutableMap()
        next[key] = value
        return copy(raw = JsonObject(next))
    }

    companion object {
        val SENSITIVE_SUFFIXES = listOf("_password", "_pass", "_secret", "_token", "_api_key")

        fun isSensitive(key: String): Boolean =
            SENSITIVE_SUFFIXES.any { key.endsWith(it, ignoreCase = true) }

        fun groupOf(key: String): ConfigGroup = when {
            key.startsWith("wifi_", ignoreCase = true) -> ConfigGroup.WIFI
            key.startsWith("llm_", ignoreCase = true) -> ConfigGroup.LLM
            key.startsWith("im_", ignoreCase = true) -> ConfigGroup.IM
            else -> ConfigGroup.MISC
        }
    }
}

enum class ConfigGroup(val label: String) {
    WIFI("Wi-Fi"),
    LLM("LLM provider"),
    IM("Messaging"),
    MISC("Miscellaneous"),
}
