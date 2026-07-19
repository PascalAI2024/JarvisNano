package com.ingeniousdigital.jarvisnano.data

import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The firmware regularly grows its `/api/cockpit` and `/ws/webim` schemas. The
 * Android side opts in to `ignoreUnknownKeys` so a new firmware field never
 * crashes the app — these tests pin that contract.
 */
class SerializationTest {

    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }

    @Test
    fun deviceCockpit_acceptsUnknownKeysAndParsesNestedTruth() {
        val payload = """
            {
              "uptime_ms": 42000,
              "network": {"connected":true,"ip":"192.0.2.80","rssi":-48},
              "voice": {"phase":"Speaking","ws_connected":true,"capturing":false},
              "display": {"init":"ready","actual_fps":18,"flush_errors":0},
              "agent": {"active":false,"revision_hwm":2,"next_revision":3},
              "future_field_added_in_firmware_v2": "shouldnt break old clients"
            }
        """.trimIndent()

        val parsed = json.decodeFromString(DeviceCockpit.serializer(), payload)
        assertTrue(parsed.network.connected)
        assertEquals("192.0.2.80", parsed.network.ip)
        assertEquals("Speaking", parsed.voice.phase)
        assertTrue(parsed.voice.wsConnected)
        assertEquals(18, parsed.display.actualFps)
        assertEquals(3L, parsed.agent.nextRevision)
    }

    @Test
    fun deviceCockpit_defaultsAreUsedForMissingFields() {
        val parsed = json.decodeFromString(DeviceCockpit.serializer(), "{}")
        assertEquals(false, parsed.network.connected)
        assertEquals("", parsed.network.ip)
        assertEquals("Unknown", parsed.voice.phase)
        assertEquals(false, parsed.agent.active)
        assertEquals(CockpitSource.CURRENT, parsed.source)
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
