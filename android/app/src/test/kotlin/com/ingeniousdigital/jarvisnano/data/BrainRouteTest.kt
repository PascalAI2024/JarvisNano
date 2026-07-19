package com.ingeniousdigital.jarvisnano.data

import org.junit.Assert.assertEquals
import org.junit.Test

class BrainRouteTest {

    @Test
    fun defaultRoute_isCloudGemini() {
        val state = BrainRouteState()

        assertEquals(BrainRoute.CLOUD_GEMINI, state.selected)
        assertEquals(BrainRouteReadiness.WAITING, state.selectedStatus.readiness)
    }

    @Test
    fun currentCockpit_marksCloudAndActiveDeskReady() {
        val cockpit = DeviceCockpit(
            network = CockpitNetwork(connected = true, ip = "192.0.2.80", rssi = -48),
            voice = CockpitVoice(phase = "Listening", wsConnected = true),
            agent = CockpitAgent(
                active = true,
                state = "working",
                title = "Running Android tests",
            ),
            brain = CockpitBrain(
                deskConnected = true,
                surfaceActive = true,
                surfaceKind = "progress",
                surfaceTitle = "Running Android tests",
            ),
        )

        val state = BrainRouteState().withCockpit(cockpit)

        assertEquals(BrainRouteReadiness.READY, state.cloudGemini.readiness)
        assertEquals(BrainRouteReadiness.READY, state.deskCodex.readiness)
        assertEquals("Gemini Live · Listening", state.cloudGemini.detail)
    }

    @Test
    fun legacyTelemetry_neverPretendsCloudVoiceIsProven() {
        val cockpit = DeviceCockpit(
            network = CockpitNetwork(connected = true, ip = "192.0.2.80"),
            source = CockpitSource.LEGACY_STATUS,
        )

        val state = BrainRouteState().withCockpit(cockpit)

        assertEquals(BrainRouteReadiness.WAITING, state.cloudGemini.readiness)
        assertEquals("Device online · voice telemetry unavailable", state.cloudGemini.detail)
    }

    @Test
    fun privateRoute_neverFallsBackWhenBleDrops() {
        val ready = BrainRouteState()
            .select(BrainRoute.PRIVATE_ANDROID)
            .withPrivateAndroid(
                bleConnected = true,
                servicePresent = true,
                localBrainReady = true,
            )
        assertEquals(BrainRouteReadiness.READY, ready.privateAndroid.readiness)

        val dropped = ready.withPrivateAndroid(
            bleConnected = false,
            servicePresent = null,
            localBrainReady = false,
        )

        assertEquals(BrainRoute.PRIVATE_ANDROID, dropped.selected)
        assertEquals(BrainRouteReadiness.UNAVAILABLE, dropped.selectedStatus.readiness)
        assertEquals("Connect JarvisNano over Bluetooth", dropped.selectedStatus.detail)
    }

    @Test
    fun gattWithoutLoadedBrain_isWaitingNotReady() {
        val state = BrainRouteState().withPrivateAndroid(
            bleConnected = true,
            servicePresent = true,
            localBrainReady = false,
        )

        assertEquals(BrainRouteReadiness.WAITING, state.privateAndroid.readiness)
        assertEquals("BLE linked · private brain not loaded", state.privateAndroid.detail)
    }
}
