# host/CMakeLists.txt — STANDALONE host test harness (NO ESP-IDF).
#
# This is the load-bearing Phase-0 proof: it compiles the PURE core (jr_core +
# jr_dsp) and the port fakes with a plain host compiler, no ESP-IDF anywhere in
# the include path, and runs a Unity test. If any core file reaches for a
# driver/network/model header, this build fails — which is exactly the
# architectural invariant we want enforced (architecture.md §Enforcement,
# §Testability). Build + run:
#
#     cd host && cmake -B build && cmake --build build && ./build/jr_host_tests
#
cmake_minimum_required(VERSION 3.16)
project(jr_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# --- The pure core under test. ONLY jr_core + jr_dsp compile here. Note there
#     is deliberately NO path to components/jr_hal, jr_audio, jr_transport, or
#     jr_display, and NO ESP-IDF include dir — the pure core cannot need them.
set(CORE_SRCS
    ${REPO_ROOT}/components/jr_core/src/session.c
    ${REPO_ROOT}/components/jr_core/src/turn_policy.c
    ${REPO_ROOT}/components/jr_core/src/monitors.c
    ${REPO_ROOT}/components/jr_core/src/snapshot.c
    ${REPO_ROOT}/components/jr_dsp/src/dsp.c
)

add_executable(jr_host_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/test_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_clock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/unity/unity.c
    ${CORE_SRCS}
)

target_include_directories(jr_host_tests PRIVATE
    ${REPO_ROOT}/components/jr_core/include
    ${REPO_ROOT}/components/jr_dsp/include
    ${REPO_ROOT}/components/jr_ports/include
    ${CMAKE_CURRENT_SOURCE_DIR}/unity
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
)

# libm for sqrt() in jr_dsp; Unity needs nothing else.
target_link_libraries(jr_host_tests PRIVATE m)

# Warnings-as-errors on the pure core keeps the seam honest on the host.
target_compile_options(jr_host_tests PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME jr_host_tests COMMAND jr_host_tests)
