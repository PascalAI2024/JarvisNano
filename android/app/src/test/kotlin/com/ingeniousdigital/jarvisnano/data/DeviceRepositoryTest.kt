package com.ingeniousdigital.jarvisnano.data

import com.ingeniousdigital.jarvisnano.discovery.Discovery
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runTest
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.util.concurrent.TimeUnit

@OptIn(ExperimentalCoroutinesApi::class)
class DeviceRepositoryTest {

    private lateinit var server: MockWebServer
    private lateinit var client: DeviceClient
    private lateinit var discovery: FakeDiscovery
    private lateinit var repository: DeviceRepository

    private val testDispatcher = StandardTestDispatcher()
    private val testScope = TestScope(testDispatcher)

    class FakeDiscovery : Discovery {
        var result: Result<String> = Result.success("127.0.0.1")
        var callCount = 0

        override suspend fun findEspClaw(timeoutMs: Long): String {
            callCount++
            return result.getOrThrow()
        }
    }

    @Before
    fun setUp() {
        server = MockWebServer().also { it.start() }
        val http = OkHttpClient.Builder()
            .connectTimeout(500, TimeUnit.MILLISECONDS)
            .readTimeout(500, TimeUnit.MILLISECONDS)
            .build()
        client = DeviceClient(http = http)
        discovery = FakeDiscovery().apply {
            result = Result.success(host())
        }
        repository = DeviceRepository(
            client = client,
            discovery = discovery,
            scope = testScope
        )
    }

    @After
    fun tearDown() {
        server.shutdown()
        testScope.cancel()
    }

    private fun host(): String = "${server.hostName}:${server.port}"

    private suspend fun TestScope.awaitBackgroundCompletion(condition: () -> Boolean) {
        val start = System.currentTimeMillis()
        while (System.currentTimeMillis() - start < 3000L) {
            testScheduler.runCurrent()
            if (condition()) return
            java.lang.Thread.sleep(10)
        }
        testScheduler.advanceUntilIdle()
        if (!condition()) {
            println("Timeout reached. Current connection state: " + repository.connection.value + ", Request count: " + server.requestCount)
        }
    }

    @Test
    fun testSuccessfulDiscovery() = testScope.runTest {
        server.enqueue(
            MockResponse()
                .setResponseCode(200)
                .setHeader("Content-Type", "application/json")
                .setBody("""{"wifi_connected":true,"ip":"192.0.2.80","ap_active":false,"wifi_mode":"sta_ok"}""")
        )

        assertEquals(ConnectionState.Disconnected, repository.connection.value)

        repository.startDiscovery()
        awaitBackgroundCompletion { server.requestCount == 1 && repository.connection.value is ConnectionState.Connected }

        assertEquals(1, discovery.callCount)
        assertTrue(repository.connection.value is ConnectionState.Connected)
        val connectedState = repository.connection.value as ConnectionState.Connected
        assertEquals(host(), connectedState.host)

        // status is expected to be null right after discovery, as it is only updated via polling or manual host setting
        assertNull(repository.status.value)
    }

    @Test
    fun testPollingStateTransitions() = testScope.runTest {
        server.enqueue(
            MockResponse()
                .setResponseCode(200)
                .setHeader("Content-Type", "application/json")
                .setBody("""{"wifi_connected":true,"ip":"192.0.2.80","ap_active":false,"wifi_mode":"sta_ok"}""")
        )
        repository.startDiscovery()
        awaitBackgroundCompletion { server.requestCount == 1 && repository.connection.value is ConnectionState.Connected }
        assertTrue(repository.connection.value is ConnectionState.Connected)

        server.enqueue(
            MockResponse()
                .setResponseCode(200)
                .setHeader("Content-Type", "application/json")
                .setBody("""{"wifi_connected":true,"ip":"192.0.2.80","ap_active":false,"wifi_mode":"sta_ok"}""")
        )
        server.enqueue(
            MockResponse()
                .setResponseCode(500)
                .setBody("Internal Server Error")
        )

        val collectedStatuses = mutableListOf<DeviceStatus>()
        val job = launch {
            repository.observeStatus().take(2).collect {
                collectedStatuses.add(it)
            }
        }

        awaitBackgroundCompletion { server.requestCount == 2 }
        assertEquals(1, collectedStatuses.size)
        assertTrue(repository.connection.value is ConnectionState.Connected)

        testScheduler.advanceTimeBy(DeviceRepository.POLL_INTERVAL_MS)
        awaitBackgroundCompletion { server.requestCount == 3 && repository.connection.value is ConnectionState.Failed }

        assertTrue(repository.connection.value is ConnectionState.Failed)

        job.cancel()
    }

    @Test
    fun testRecoveryBehavior() = testScope.runTest {
        // 1. Initial successful discovery (verification probe needs 200 OK)
        server.enqueue(
            MockResponse()
                .setResponseCode(200)
                .setHeader("Content-Type", "application/json")
                .setBody("""{"wifi_connected":true,"ip":"192.0.2.80","ap_active":false,"wifi_mode":"sta_ok"}""")
        )
        repository.startDiscovery()
        awaitBackgroundCompletion { server.requestCount == 1 && repository.connection.value is ConnectionState.Connected }
        assertTrue(repository.connection.value is ConnectionState.Connected)
        assertEquals(1, discovery.callCount)

        // 2. Poll 1 fails
        server.enqueue(
            MockResponse()
                .setResponseCode(500)
                .setBody("Timeout or Connection Failure")
        )
        // 3. Poll 2 fails
        server.enqueue(
            MockResponse()
                .setResponseCode(500)
                .setBody("Timeout or Connection Failure")
        )
        // 4. Poll 3 fails (Triggers Rediscovery)
        server.enqueue(
            MockResponse()
                .setResponseCode(500)
                .setBody("Timeout or Connection Failure")
        )
        // 5. Rediscovery verification probe succeeds (200 OK)
        server.enqueue(
            MockResponse()
                .setResponseCode(200)
                .setHeader("Content-Type", "application/json")
                .setBody("""{"wifi_connected":true,"ip":"192.0.2.80","ap_active":false,"wifi_mode":"sta_ok"}""")
        )

        // Start observing status
        val job = launch {
            repository.observeStatus().collect {}
        }

        // Poll 1 (Immediate)
        awaitBackgroundCompletion { server.requestCount == 2 && repository.connection.value is ConnectionState.Failed }
        assertTrue(repository.connection.value is ConnectionState.Failed)

        // Advance to Poll 2
        testScheduler.advanceTimeBy(DeviceRepository.POLL_INTERVAL_MS)
        awaitBackgroundCompletion { server.requestCount == 3 }
        assertTrue(repository.connection.value is ConnectionState.Failed)

        // Advance to Poll 3. This fails and triggers startDiscovery()
        testScheduler.advanceTimeBy(DeviceRepository.POLL_INTERVAL_MS)
        
        // Wait for discovery and its verification probe to complete (Request count should reach 5)
        awaitBackgroundCompletion { server.requestCount == 5 && repository.connection.value is ConnectionState.Connected }

        // Discovery call count should now be 2
        assertEquals(2, discovery.callCount)
        // State should have recovered back to Connected
        assertTrue(repository.connection.value is ConnectionState.Connected)
        val connectedState = repository.connection.value as ConnectionState.Connected
        assertEquals(host(), connectedState.host)

        job.cancel()
    }
}
