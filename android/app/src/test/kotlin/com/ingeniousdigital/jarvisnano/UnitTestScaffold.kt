package com.ingeniousdigital.jarvisnano

/**
 * Compile-only placeholder for local JVM tests.
 *
 * Keep this dependency-free until the project adds a committed test framework
 * decision. This source set proves that `src/test` is wired and gives future
 * unit tests a stable package to extend.
 */
internal object UnitTestScaffold {
    const val sourceSet = "test"
    const val expectedCommand = ":app:testDebugUnitTest"
}
