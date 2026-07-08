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
#     is deliberately NO path to components/jr_hal, jr_audio, or jr_display, and
#     NO ESP-IDF include dir — the pure core cannot need them.
set(CORE_SRCS
    ${REPO_ROOT}/components/jr_core/src/session.c
    ${REPO_ROOT}/components/jr_core/src/turn_policy.c
    ${REPO_ROOT}/components/jr_core/src/monitors.c
    ${REPO_ROOT}/components/jr_core/src/snapshot.c
    ${REPO_ROOT}/components/jr_dsp/src/dsp.c
)

# --- Run 3: the L2 Gemini transport (builders/framer/parser) compiles here too.
#     It depends ONLY on jr_ports/*, jr_core types, libc, and the vendored cJSON
#     — still NO ESP-IDF (only the device ws-transport adapter, excluded here,
#     references esp_websocket_client). The vendored cJSON is the SAME portable
#     source the device build uses (there via the IDF `json` component).
set(TRANSPORT_SRCS
    ${REPO_ROOT}/components/jr_transport/src/gemini_live.c
    ${REPO_ROOT}/components/jr_transport/vendor/cJSON/cJSON.c
)

add_executable(jr_host_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/test_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_clock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_ws_transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/unity/unity.c
    ${CORE_SRCS}
    ${TRANSPORT_SRCS}
)

target_include_directories(jr_host_tests PRIVATE
    ${REPO_ROOT}/components/jr_core/include
    ${REPO_ROOT}/components/jr_dsp/include
    ${REPO_ROOT}/components/jr_ports/include
    ${REPO_ROOT}/components/jr_transport/include
    ${REPO_ROOT}/components/jr_transport/vendor/cJSON
    ${CMAKE_CURRENT_SOURCE_DIR}/unity
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
)

# libm for sqrt() in jr_dsp; Unity needs nothing else.
target_link_libraries(jr_host_tests PRIVATE m)

# -Wall -Wextra keeps the seam honest on the host. The vendored cJSON is a
# third-party single-file lib — keep OUR code clean but do not fail on cJSON's
# own style, so build with warnings on but never as errors.
target_compile_options(jr_host_tests PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME jr_host_tests COMMAND jr_host_tests)
