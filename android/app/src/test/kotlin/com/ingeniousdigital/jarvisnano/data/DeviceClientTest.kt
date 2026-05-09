package com.ingeniousdigital.jarvisnano.data

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.util.concurrent.TimeUnit

/**
 * Wire-level tests for [DeviceClient]. They lock down request shape so the
 * older firmware compatibility rules in docs/PROTOCOL.md §3.1 cannot regress
 * silently — in particular, /api/webim/send MUST be sent as text/plain so the
 * browser CORS simple-request path works on un-patched firmware.
 */
class DeviceClientTest {

    private lateinit var server: MockWebServer
    private lateinit var client: DeviceClient

    @Before
    fun setUp() {
        server = MockWebServer().also { it.start() }
        val http = OkHttpClient.Builder()
            .connectTimeout(2, TimeUnit.SECONDS)
            .readTimeout(2, TimeUnit.SECONDS)
            .writeTimeout(2, TimeUnit.SECONDS)
            .build()
        client = DeviceClient(http = http)
    }

    @After
    fun tearDown() {
        server.shutdown()
    }

    private fun host(): String = "${server.hostName}:${server.port}"

    @Test
    fun getStatus_parsesPayload_andSendsProtocolHeader() = runBlocking {
        server.enqueue(
            MockResponse()
                .setResponseCode(200)
                .setHeader("Content-Type", "application/json")
                .setBody(
                    """
                    {"wifi_connected":true,"ip":"192.168.50.80","ap_active":false,"wifi_mode":"sta_ok"}
                    """.trimIndent(),
                ),
        )

        val status = client.getStatus(host())
        assertTrue(status.wifiConnected)
        assertEquals("192.168.50.80", status.ip)
        assertEquals("sta_ok", status.wifiMode)

        val recorded = server.takeRequest()
        assertEquals("GET", recorded.method)
        assertEquals("/api/status", recorded.path)
        assertEquals("1", recorded.getHeader("X-JarvisNano-Protocol"))
    }

    @Test
    fun getConfig_returnsRawJsonObject_preservingScalarTypes() = runBlocking {
        server.enqueue(
            MockResponse()
                .setResponseCode(200)
                .setHeader("Content-Type", "application/json")
                .setBody(
                    """
                    {
                      "agent_name":"jarvis",
                      "llm_timeout_ms":30000,
                      "llm_enable_thinking":true,
                      "wifi_ssid":"home",
                      "llm_temperature":0.7
                    }
                    """.trimIndent(),
                ),
        )

        val config = client.getConfig(host())
        // Numbers must arrive as numbers, not strings — the round-trip starts here.
        val timeout = config.raw["llm_timeout_ms"]!!.jsonPrimitive
        assertEquals(false, timeout.isString)
        assertEquals(30000L, timeout.content.toLong())

        val thinking = config.raw["llm_enable_thinking"]!!.jsonPrimitive
        assertEquals(false, thinking.isString)
        assertEquals("true", thinking.content)
    }

    @Test
    fun putConfig_sendsJsonBody_andHeader() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(200).setBody("""{"ok":true}"""))

        val patch = DeviceConfig(
            JsonObject(
                mapOf(
                    "agent_name" to JsonPrimitive("jarvis"),
                    "llm_timeout_ms" to JsonPrimitive(45000L),
                ),
            ),
        )
        client.putConfig(host(), patch)

        val recorded = server.takeRequest()
        assertEquals("POST", recorded.method)
        assertEquals("/api/config", recorded.path)
        val ct = recorded.getHeader("Content-Type") ?: ""
        assertTrue("expected application/json, was $ct", ct.startsWith("application/json"))

        // Decoded body keeps numeric types — the on-the-wire form is what the
        // firmware sees, so this is the canonical shape.
        val body = Json.parseToJsonElement(recorded.body.readUtf8()) as JsonObject
        val timeoutOnWire = body["llm_timeout_ms"]!!.jsonPrimitive
        assertEquals(false, timeoutOnWire.isString)
        assertEquals("45000", timeoutOnWire.content)
    }

    @Test
    fun sendWebim_usesTextPlain_forBrowserCompat() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(200).setBody("""{"ok":true}"""))

        client.sendWebim(host(), chatId = "phone-1", text = "hello")

        val recorded = server.takeRequest()
        assertEquals("POST", recorded.method)
        assertEquals("/api/webim/send", recorded.path)
        val ct = recorded.getHeader("Content-Type") ?: ""
        // PROTOCOL.md §3.1 — browser CORS simple-request requires text/plain.
        assertTrue("expected text/plain, was $ct", ct.startsWith("text/plain"))
        val body = Json.parseToJsonElement(recorded.body.readUtf8()) as JsonObject
        assertEquals("phone-1", body["chat_id"]!!.jsonPrimitive.content)
        assertEquals("hello", body["text"]!!.jsonPrimitive.content)
    }

    @Test
    fun sendWebim_escapesQuotesAndBackslashes() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(200).setBody("""{"ok":true}"""))

        val nasty = """She said "hi" \ then left"""
        client.sendWebim(host(), chatId = """ch"id\\""", text = nasty)

        val recorded = server.takeRequest()
        val body = Json.parseToJsonElement(recorded.body.readUtf8()) as JsonObject
        assertEquals("""ch"id\\""", body["chat_id"]!!.jsonPrimitive.content)
        assertEquals(nasty, body["text"]!!.jsonPrimitive.content)
    }

    @Test
    fun restart_postsEmptyBody_andSwallowsConnectionDrop() = runBlocking {
        // The device closes the socket as part of restart; the client must not
        // throw if that happens after the response (or instead of one).
        server.enqueue(MockResponse().setSocketPolicy(okhttp3.mockwebserver.SocketPolicy.DISCONNECT_AFTER_REQUEST))

        client.restart(host()) // must not throw

        val recorded = server.takeRequest()
        assertEquals("POST", recorded.method)
        assertEquals("/api/restart", recorded.path)
    }

    @Test
    fun fetchSnapshot_returnsBytes_onSuccess() {
        val payload = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0xFF.toByte(), 0xE0.toByte())
        server.enqueue(
            MockResponse()
                .setResponseCode(200)
                .setHeader("Content-Type", "image/jpeg")
                .setBody(okio.Buffer().write(payload)),
        )

        val bytes = client.fetchSnapshot(host())
        assertEquals(payload.size, bytes.size)
        assertEquals(0xFF.toByte(), bytes[0])
        assertEquals(0xD8.toByte(), bytes[1])
    }

    @Test(expected = IllegalStateException::class)
    fun fetchSnapshot_throws_onNon2xx() {
        server.enqueue(MockResponse().setResponseCode(503).setBody("""{"error":"feature_unavailable"}"""))
        client.fetchSnapshot(host())
    }
}
