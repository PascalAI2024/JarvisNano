package com.ingeniousdigital.jarvisnano.data

import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The firmware regularly grows its `/api/status` and `/ws/webim` schemas. The
 * Android side opts in to `ignoreUnknownKeys` so a new firmware field never
 * crashes the app — these tests pin that contract.
 */
class SerializationTest {

    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }

    @Test
    fun deviceStatus_acceptsUnknownKeys() {
        val payload = """
            {
              "wifi_connected": true,
              "ip": "192.168.50.80",
              "ap_active": false,
              "wifi_mode": "sta_ok",
              "future_field_added_in_firmware_v2": "shouldnt break old clients"
            }
        """.trimIndent()

        val parsed = json.decodeFromString(DeviceStatus.serializer(), payload)
        assertTrue(parsed.wifiConnected)
        assertEquals("192.168.50.80", parsed.ip)
        assertEquals("sta_ok", parsed.wifiMode)
    }

    @Test
    fun deviceStatus_defaultsAreUsedForMissingFields() {
        val parsed = json.decodeFromString(DeviceStatus.serializer(), "{}")
        assertEquals(false, parsed.wifiConnected)
        assertEquals(null, parsed.ip)
        assertEquals(false, parsed.apActive)
    }

    @Test
    fun webimEvent_parsesAssistantBroadcast() {
        val payload = """{"chat_id":"phone-1","text":"It's 2:14 PM.","source":"assistant"}"""
        val parsed = json.decodeFromString(WebimEvent.serializer(), payload)
        assertEquals("phone-1", parsed.chatId)
        assertEquals("It's 2:14 PM.", parsed.text)
        assertEquals("assistant", parsed.source)
    }

    @Test
    fun capabilityList_emptyDecodesCleanly() {
        val parsed = json.decodeFromString(CapabilityList.serializer(), """{"items":[]}""")
        assertTrue(parsed.items.isEmpty())
    }

    @Test
    fun capabilityList_defaultsLlmVisibleToTrue() {
        val payload = """
            {"items":[
              {"group_id":"weather","display_name":"Weather"},
              {"group_id":"hidden","display_name":"Hidden","default_llm_visible":false}
            ]}
        """.trimIndent()

        val parsed = json.decodeFromString(CapabilityList.serializer(), payload)
        assertEquals(2, parsed.items.size)
        assertTrue(parsed.items[0].defaultLlmVisible)
        assertEquals(false, parsed.items[1].defaultLlmVisible)
    }

    @Test
    fun webimStatus_unboundIsRecognized() {
        val parsed = json.decodeFromString(WebimStatus.serializer(), """{"ok":true,"bound":false}""")
        assertTrue(parsed.ok)
        assertEquals(false, parsed.bound)
    }
}
