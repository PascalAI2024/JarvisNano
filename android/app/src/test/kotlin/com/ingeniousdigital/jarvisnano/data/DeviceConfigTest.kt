package com.ingeniousdigital.jarvisnano.data

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.double
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Covers the bug fix where editing a numeric or boolean config field was
 * round-tripped as a JSON string. With the type-preserving patch builder,
 * numbers stay numbers, booleans stay booleans, and strings stay strings.
 */
class DeviceConfigTest {

    private fun base(map: Map<String, Any?>): DeviceConfig {
        val raw = buildMap<String, kotlinx.serialization.json.JsonElement> {
            for ((k, v) in map) {
                put(
                    k,
                    when (v) {
                        is Boolean -> JsonPrimitive(v)
                        is Int -> JsonPrimitive(v.toLong())
                        is Long -> JsonPrimitive(v)
                        is Double -> JsonPrimitive(v)
                        is String -> JsonPrimitive(v)
                        null -> JsonPrimitive(null as String?)
                        else -> JsonPrimitive(v.toString())
                    },
                )
            }
        }
        return DeviceConfig(JsonObject(raw))
    }

    @Test
    fun displayValue_unwrapsPrimitives() {
        assertEquals("hello", DeviceConfig.displayValue(JsonPrimitive("hello")))
        assertEquals("42", DeviceConfig.displayValue(JsonPrimitive(42L)))
        assertEquals("true", DeviceConfig.displayValue(JsonPrimitive(true)))
    }

    @Test
    fun groupOf_classifiesPrefixes() {
        assertEquals(ConfigGroup.WIFI, DeviceConfig.groupOf("wifi_ssid"))
        assertEquals(ConfigGroup.LLM, DeviceConfig.groupOf("llm_model"))
        assertEquals(ConfigGroup.IM, DeviceConfig.groupOf("im_telegram_token"))
        assertEquals(ConfigGroup.MISC, DeviceConfig.groupOf("agent_name"))
        assertEquals(ConfigGroup.WIFI, DeviceConfig.groupOf("WIFI_SSID"))
    }

    @Test
    fun isSensitive_matchesSuffixesCaseInsensitively() {
        assertTrue(DeviceConfig.isSensitive("wifi_password"))
        assertTrue(DeviceConfig.isSensitive("llm_api_key"))
        assertTrue(DeviceConfig.isSensitive("im_telegram_token"))
        assertTrue(DeviceConfig.isSensitive("LLM_SECRET"))
        assertFalse(DeviceConfig.isSensitive("wifi_ssid"))
        assertFalse(DeviceConfig.isSensitive("agent_name"))
    }

    @Test
    fun buildConfigPatch_omitsUnchangedFields() {
        val cfg = base(
            mapOf(
                "agent_name" to "jarvis",
                "llm_timeout_ms" to 30000L,
                "wifi_ssid" to "home",
            ),
        )
        val edits = mapOf(
            "agent_name" to "jarvis",
            "llm_timeout_ms" to "30000",
            "wifi_ssid" to "home",
        )

        val patch = DeviceConfig.buildConfigPatch(cfg, edits)
        assertTrue("expected no changes, got ${'$'}{patch.raw}", patch.raw.isEmpty())
    }

    @Test
    fun buildConfigPatch_preservesNumberType() {
        val cfg = base(mapOf("llm_timeout_ms" to 30000L))
        val edits = mapOf("llm_timeout_ms" to "45000")

        val patch = DeviceConfig.buildConfigPatch(cfg, edits)
        val patched = patch.raw["llm_timeout_ms"]!!.jsonPrimitive
        assertFalse("number must not be quoted as a string", patched.isString)
        assertEquals(45000L, patched.long)
    }

    @Test
    fun buildConfigPatch_preservesBooleanType() {
        val cfg = base(mapOf("llm_enable_thinking" to true))
        val edits = mapOf("llm_enable_thinking" to "false")

        val patch = DeviceConfig.buildConfigPatch(cfg, edits)
        val patched = patch.raw["llm_enable_thinking"]!!.jsonPrimitive
        assertFalse("boolean must not be quoted as a string", patched.isString)
        assertFalse(patched.boolean)
    }

    @Test
    fun buildConfigPatch_preservesDoubleType() {
        val cfg = base(mapOf("llm_temperature" to 0.7))
        val edits = mapOf("llm_temperature" to "0.5")

        val patch = DeviceConfig.buildConfigPatch(cfg, edits)
        val patched = patch.raw["llm_temperature"]!!.jsonPrimitive
        assertFalse(patched.isString)
        assertEquals(0.5, patched.double, 0.0001)
    }

    @Test
    fun buildConfigPatch_keepsStringsAsStrings() {
        val cfg = base(mapOf("agent_name" to "jarvis"))
        val edits = mapOf("agent_name" to "ultron")

        val patch = DeviceConfig.buildConfigPatch(cfg, edits)
        val patched = patch.raw["agent_name"]!!.jsonPrimitive
        assertTrue(patched.isString)
        assertEquals("ultron", patched.content)
    }

    @Test
    fun buildConfigPatch_invalidNumericInputFallsBackToString() {
        // Firmware will reject this, but we MUST surface the user's input
        // rather than silently coercing it to 0.
        val cfg = base(mapOf("llm_timeout_ms" to 30000L))
        val edits = mapOf("llm_timeout_ms" to "not-a-number")

        val patch = DeviceConfig.buildConfigPatch(cfg, edits)
        val patched = patch.raw["llm_timeout_ms"]!!.jsonPrimitive
        assertTrue(patched.isString)
        assertEquals("not-a-number", patched.content)
    }

    @Test
    fun buildConfigPatch_newKeyEmittedAsString() {
        val cfg = base(mapOf("agent_name" to "jarvis"))
        val edits = mapOf(
            "agent_name" to "jarvis",
            "newly_added_field" to "hello",
        )

        val patch = DeviceConfig.buildConfigPatch(cfg, edits)
        assertEquals(setOf("newly_added_field"), patch.raw.keys)
        val patched = patch.raw["newly_added_field"]!!.jsonPrimitive
        assertTrue(patched.isString)
        assertEquals("hello", patched.content)
    }

    @Test
    fun mergeConfig_overlaysPatchOnBase() {
        val cfg = base(
            mapOf(
                "agent_name" to "jarvis",
                "llm_timeout_ms" to 30000L,
            ),
        )
        val patch = DeviceConfig.buildConfigPatch(
            cfg,
            mapOf(
                "agent_name" to "jarvis",
                "llm_timeout_ms" to "45000",
            ),
        )

        val merged = DeviceConfig.mergeConfig(cfg, patch)
        assertEquals("jarvis", merged.raw["agent_name"]!!.jsonPrimitive.content)
        assertEquals(45000L, merged.raw["llm_timeout_ms"]!!.jsonPrimitive.long)
        assertNull(merged.raw["unknown_field"])
    }
}
