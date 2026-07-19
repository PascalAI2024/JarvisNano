package com.ingeniousdigital.jarvisnano.data

/**
 * The explicit owner of assistant inference.
 *
 * JarvisMCP is a capability used by a brain, not another route. Route changes
 * are user choices; readiness changes never rewrite [BrainRouteState.selected].
 */
enum class BrainRoute(val label: String) {
    CLOUD_GEMINI("Cloud · Gemini"),
    PRIVATE_ANDROID("Private · Android"),
    DESK_CODEX("Desk · Codex"),
}

enum class BrainRouteReadiness {
    READY,
    WAITING,
    UNAVAILABLE,
}

data class BrainRouteStatus(
    val readiness: BrainRouteReadiness,
    val detail: String,
)

data class BrainRouteState(
    val selected: BrainRoute = BrainRoute.CLOUD_GEMINI,
    val cloudGemini: BrainRouteStatus = BrainRouteStatus(
        BrainRouteReadiness.WAITING,
        "Waiting for device telemetry",
    ),
    val privateAndroid: BrainRouteStatus = BrainRouteStatus(
        BrainRouteReadiness.UNAVAILABLE,
        "Connect JarvisNano over Bluetooth",
    ),
    val deskCodex: BrainRouteStatus = BrainRouteStatus(
        BrainRouteReadiness.WAITING,
        "Awaiting Codex Agent Link",
    ),
) {
    val selectedStatus: BrainRouteStatus
        get() = statusFor(selected)

    fun statusFor(route: BrainRoute): BrainRouteStatus = when (route) {
        BrainRoute.CLOUD_GEMINI -> cloudGemini
        BrainRoute.PRIVATE_ANDROID -> privateAndroid
        BrainRoute.DESK_CODEX -> deskCodex
    }

    fun select(route: BrainRoute): BrainRouteState = copy(selected = route)

    /** Update device-backed routes without ever changing the user's selection. */
    fun withCockpit(cockpit: DeviceCockpit?): BrainRouteState {
        if (cockpit == null) {
            return copy(
                cloudGemini = BrainRouteStatus(
                    BrainRouteReadiness.WAITING,
                    "Waiting for device telemetry",
                ),
                deskCodex = BrainRouteStatus(
                    BrainRouteReadiness.WAITING,
                    "Awaiting Codex Agent Link",
                ),
            )
        }

        val cloud = when {
            !cockpit.network.connected -> BrainRouteStatus(
                BrainRouteReadiness.UNAVAILABLE,
                "Device Wi-Fi is offline",
            )
            cockpit.source == CockpitSource.LEGACY_STATUS -> BrainRouteStatus(
                BrainRouteReadiness.WAITING,
                "Device online · voice telemetry unavailable",
            )
            cockpit.voice.privacyPaused -> BrainRouteStatus(
                BrainRouteReadiness.WAITING,
                "Privacy paused on device",
            )
            cockpit.voice.wsConnected -> BrainRouteStatus(
                BrainRouteReadiness.READY,
                "Gemini Live · ${cockpit.voice.phase}",
            )
            else -> BrainRouteStatus(
                BrainRouteReadiness.WAITING,
                "Gemini session · ${cockpit.voice.phase}",
            )
        }
        val desk = if (cockpit.brain.deskConnected) {
            val task = cockpit.brain.surfaceTitle.takeIf(String::isNotBlank)
                ?: cockpit.agent.title?.takeIf(String::isNotBlank)
                ?: "Brain Link connected"
            BrainRouteStatus(
                BrainRouteReadiness.READY,
                if (cockpit.brain.surfaceActive) {
                    "$task · ${cockpit.brain.surfaceKind}"
                } else {
                    "$task · ready"
                },
            )
        } else {
            BrainRouteStatus(
                BrainRouteReadiness.WAITING,
                "Awaiting paired Codex Brain Link",
            )
        }
        return copy(cloudGemini = cloud, deskCodex = desk)
    }

    /**
     * Android private mode is ready only when both the canonical GATT service
     * and a local brain are ready. A dropped BLE link updates readiness but
     * deliberately leaves PRIVATE_ANDROID selected.
     */
    fun withPrivateAndroid(
        bleConnected: Boolean,
        servicePresent: Boolean?,
        localBrainReady: Boolean,
    ): BrainRouteState {
        val status = when {
            !bleConnected -> BrainRouteStatus(
                BrainRouteReadiness.UNAVAILABLE,
                "Connect JarvisNano over Bluetooth",
            )
            servicePresent == false -> BrainRouteStatus(
                BrainRouteReadiness.UNAVAILABLE,
                "Jarvis GATT service is missing",
            )
            servicePresent == null -> BrainRouteStatus(
                BrainRouteReadiness.WAITING,
                "Discovering Jarvis GATT service",
            )
            !localBrainReady -> BrainRouteStatus(
                BrainRouteReadiness.WAITING,
                "BLE linked · private brain not loaded",
            )
            else -> BrainRouteStatus(
                BrainRouteReadiness.READY,
                "Local Android brain ready",
            )
        }
        return copy(privateAndroid = status)
    }
}
