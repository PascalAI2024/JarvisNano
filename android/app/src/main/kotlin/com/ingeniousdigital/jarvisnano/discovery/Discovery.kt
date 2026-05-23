package com.ingeniousdigital.jarvisnano.discovery

/**
 * Interface for device discovery.
 */
interface Discovery {
    /**
     * Returns the resolved host (IP literal or hostname) suitable for HTTP calls.
     * Throws on timeout. Callers should wrap with runCatching {} if they want a
     * recoverable failure path.
     */
    suspend fun findEspClaw(timeoutMs: Long = 8_000L): String
}
