package com.ingeniousdigital.jarvisnano.data

import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.longOrNull

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

        /**
         * Render a JsonElement as a single-line string for the Settings text
         * field. Primitives use their content (so a number renders as "30000",
         * not "\"30000\"" with quotes); nulls and complex values fall back to
         * their canonical JSON form.
         */
        fun displayValue(value: JsonElement): String = when (value) {
            is JsonPrimitive -> value.contentOrNull ?: value.toString()
            JsonNull -> ""
            else -> value.toString()
        }

        /**
         * Build a `/api/config` PATCH body from a base config and the user's
         * edits. Only changed keys are included.
         *
         * Type preservation: if a key existed in [base] as a number, boolean,
         * or string, the patched value is coerced back to that type when the
         * user's input parses cleanly. If it does not parse, the patched value
         * falls back to a JSON string so the user's input is never silently
         * dropped, but the firmware will surface a validation error on save.
         *
         * New keys (not present in [base]) are emitted as JSON strings.
         */
        fun buildConfigPatch(
            base: DeviceConfig,
            edits: Map<String, String>,
        ): DeviceConfig {
            val patch = mutableMapOf<String, JsonElement>()
            for ((key, value) in edits) {
                val original = base.raw[key]
                if (original != null && value == displayValue(original)) continue
                patch[key] = coerceTo(original, value)
            }
            return DeviceConfig(raw = JsonObject(patch))
        }

        /** Merge a saved patch back over the loaded config so the UI reflects the saved state. */
        fun mergeConfig(base: DeviceConfig, patch: DeviceConfig): DeviceConfig {
            val merged = base.raw.toMutableMap()
            merged.putAll(patch.raw)
            return DeviceConfig(raw = JsonObject(merged))
        }

        private fun coerceTo(original: JsonElement?, edited: String): JsonElement {
            // No prior shape — emit as string. Firmware decides if it's valid.
            if (original !is JsonPrimitive) return JsonPrimitive(edited)

            // Quoted strings stay strings, even if the content parses as a number.
            if (original.isString) return JsonPrimitive(edited)

            // Original was an unquoted JSON primitive: bool or number.
            original.booleanOrNull?.let {
                edited.trim().lowercase().let { lower ->
                    if (lower == "true") return JsonPrimitive(true)
                    if (lower == "false") return JsonPrimitive(false)
                }
                return JsonPrimitive(edited)
            }
            original.longOrNull?.let {
                edited.trim().toLongOrNull()?.let { return JsonPrimitive(it) }
                edited.trim().toDoubleOrNull()?.let { return JsonPrimitive(it) }
                return JsonPrimitive(edited)
            }
            original.doubleOrNull?.let {
                edited.trim().toDoubleOrNull()?.let { return JsonPrimitive(it) }
                return JsonPrimitive(edited)
            }
            return JsonPrimitive(edited)
        }
    }
}

enum class ConfigGroup(val label: String) {
    WIFI("Wi-Fi"),
    LLM("LLM provider"),
    IM("Messaging"),
    MISC("Miscellaneous"),
}
