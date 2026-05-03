package com.ingeniousdigital.jarvisnano.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.withContext
import kotlinx.serialization.builtins.serializer
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import java.util.concurrent.TimeUnit

/**
 * OkHttp + WebSocket facade for the firmware's HTTP API.
 *
 * One instance lives in [com.ingeniousdigital.jarvisnano.App.deviceClient]; it's
 * stateless other than its connection pool. The base host is supplied per-call so
 * the same client can target different devices on the LAN without rebuilding.
 */
class DeviceClient(
    val http: OkHttpClient = defaultClient(),
    private val json: Json = defaultJson,
) {

    /** GET /api/status — short-poll telemetry. */
    suspend fun getStatus(host: String): DeviceStatus = withContext(Dispatchers.IO) {
        val body = http.newCall(get(host, "/api/status")).execute().use { it.requireBody() }
        json.decodeFromString(DeviceStatus.serializer(), body)
    }

    /** GET /api/config — full config blob. */
    suspend fun getConfig(host: String): DeviceConfig = withContext(Dispatchers.IO) {
        val body = http.newCall(get(host, "/api/config")).execute().use { it.requireBody() }
        DeviceConfig(raw = json.decodeFromString(JsonObject.serializer(), body))
    }

    /** POST /api/config — partial config patch; absent keys stay untouched on-device. */
    suspend fun putConfig(host: String, config: DeviceConfig): Unit = withContext(Dispatchers.IO) {
        val payload = json.encodeToString(JsonObject.serializer(), config.raw)
        val req = Request.Builder()
            .url(url(host, "/api/config"))
            .post(payload.toRequestBody(JSON))
            .build()
        http.newCall(req).execute().use { it.requireOk() }
    }

    suspend fun getCapabilities(host: String): CapabilityList = withContext(Dispatchers.IO) {
        val body = http.newCall(get(host, "/api/capabilities")).execute().use { it.requireBody() }
        json.decodeFromString(CapabilityList.serializer(), body)
    }

    suspend fun getLuaModules(host: String): LuaModuleList = withContext(Dispatchers.IO) {
        val body = http.newCall(get(host, "/api/lua-modules")).execute().use { it.requireBody() }
        json.decodeFromString(LuaModuleList.serializer(), body)
    }

    suspend fun getWebimStatus(host: String): WebimStatus = withContext(Dispatchers.IO) {
        val body = http.newCall(get(host, "/api/webim/status")).execute().use { it.requireBody() }
        json.decodeFromString(WebimStatus.serializer(), body)
    }

    /**
     * POST /api/webim/send.
     *
     * Sends as `text/plain;charset=UTF-8` for compatibility with older firmware.
     * Current bootstrap builds also support OPTIONS /api/* for JSON clients.
     */
    suspend fun sendWebim(host: String, chatId: String, text: String): Unit = withContext(Dispatchers.IO) {
        val stringSerializer = String.serializer()
        val payload = "{" +
            "\"chat_id\":" + json.encodeToString(stringSerializer, chatId) + "," +
            "\"text\":" + json.encodeToString(stringSerializer, text) +
            "}"
        val req = Request.Builder()
            .url(url(host, "/api/webim/send"))
            .post(payload.toRequestBody(TEXT_PLAIN))
            .build()
        http.newCall(req).execute().use { it.requireOk() }
    }

    /** POST /api/restart — fire and forget; the device will go away. */
    suspend fun restart(host: String): Unit = withContext(Dispatchers.IO) {
        val req = Request.Builder()
            .url(url(host, "/api/restart"))
            .post("".toRequestBody(JSON))
            .build()
        runCatching { http.newCall(req).execute().close() }
    }

    /**
     * Subscribes to /ws/webim and emits each parsed event.
     * The flow is cold; the socket opens on collect and closes on cancel.
     */
    fun openWebimSocket(host: String): Flow<WebimEvent> = callbackFlow {
        val req = Request.Builder().url(wsUrl(host, "/ws/webim")).build()
        val listener = object : WebSocketListener() {
            override fun onMessage(webSocket: WebSocket, text: String) {
                runCatching { json.decodeFromString(WebimEvent.serializer(), text) }
                    .onSuccess { trySend(it) }
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                close(t)
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                close()
            }
        }
        val socket = http.newWebSocket(req, listener)
        awaitClose { socket.close(1000, "client closed") }
    }

    /**
     * Fetch a single JPEG frame from /api/camera/snapshot.
     * Throws on non-2xx or empty body. Returned bytes are raw JPEG.
     */
    fun fetchSnapshot(host: String): ByteArray {
        val req = get(host, "/api/camera/snapshot")
        http.newCall(req).execute().use { resp ->
            if (!resp.isSuccessful) error("HTTP ${resp.code} from camera snapshot")
            val bytes = resp.body?.bytes() ?: error("empty body from camera snapshot")
            if (bytes.isEmpty()) error("zero-length JPEG from camera snapshot")
            return bytes
        }
    }

    // ---- helpers --------------------------------------------------------

    private fun get(host: String, path: String) = Request.Builder().url(url(host, path)).get().build()

    private fun url(host: String, path: String): String {
        val cleanHost = host.removePrefix("http://").removePrefix("https://").trimEnd('/')
        return "http://$cleanHost$path"
    }

    private fun wsUrl(host: String, path: String): String {
        val cleanHost = host.removePrefix("http://").removePrefix("https://").trimEnd('/')
        return "ws://$cleanHost$path"
    }

    private fun Response.requireBody(): String {
        if (!isSuccessful) error("HTTP $code from $request")
        return body?.string() ?: error("empty body from $request")
    }

    private fun Response.requireOk() {
        if (!isSuccessful) error("HTTP $code from $request")
    }

    companion object {
        private val JSON = "application/json; charset=utf-8".toMediaType()
        private val TEXT_PLAIN = "text/plain; charset=utf-8".toMediaType()

        private val defaultJson: Json = Json {
            ignoreUnknownKeys = true
            encodeDefaults = true
        }

        private fun defaultClient(): OkHttpClient = OkHttpClient.Builder()
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(15, TimeUnit.SECONDS)
            .writeTimeout(15, TimeUnit.SECONDS)
            .build()
    }
}
