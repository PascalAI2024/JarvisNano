package com.ingeniousdigital.jarvisnano

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Minimal local JVM test to prove the source set and runner are wired.
 *
 * Future unit tests can grow from this package without changing the CLI
 * command used by docs and CI.
 */
class UnitTestScaffold {
    private val sourceSet = "test"
    private val expectedCommand = ":app:testDebugUnitTest"

    @Test
    fun sourceSetIsWired() {
        assertEquals("test", sourceSet)
        assertEquals(":app:testDebugUnitTest", expectedCommand)
    }
}
