# Standalone host tests for the procedural HUD renderer.
#
# Deliberately NOT folded into host/CMakeLists.txt: that harness enforces an
# architectural invariant — "no path to components/jr_hal, jr_audio, or
# jr_display" — and adding this there would break it. hud_render.c is pure C
# (stdint/stdbool/stddef/string.h only) so it stands alone, matching the
# pattern already used by components/jr_memory/tests and components/jr_tools/host.
#
#     cd components/jr_display/tests && cmake -B build && cmake --build build \
#       && ./build/jr_display_hud_tests
cmake_minimum_required(VERSION 3.16)
project(jr_display_hud_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(jr_display_hud_tests
    test_hud_render.c
    ../src/hud_render.c
    ../src/balance_wheel_data.c
)
target_include_directories(jr_display_hud_tests PRIVATE ../include ../src)
target_link_libraries(jr_display_hud_tests PRIVATE m)
target_compile_options(jr_display_hud_tests PRIVATE -Wall -Wextra -Werror)

# The spatial shell lives in jr_display.c, which does touch IDF. It is pulled
# into the test's own translation unit (not linked) so the tests can assert on
# render-task state without widening the public API for testing's sake; the
# IDF surface comes from probe/stubs, the same set the visual probe uses.
add_executable(jr_display_shell_tests
    test_shell.c
    ../src/hud_render.c
    ../src/balance_wheel_data.c
    ../src/companion.c
    ../src/glass_font.c
    ../src/glass_draw.c
    ../src/glass_font_data.c
    ../src/glass_icon.c
    ../src/glass_icon_data.c
)
target_include_directories(jr_display_shell_tests PRIVATE
    ../include
    ../src
    ../../jr_ports/include
    probe/stubs
)
target_compile_options(jr_display_shell_tests PRIVATE
    -Wall -Wextra -Werror -Wno-unused-function
)
# The two embedded dials: the stub reads the real baked EAFs from here.
target_compile_definitions(jr_display_shell_tests PRIVATE
    JR_HOST_DIAL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/mascot/reactive"
)

# The whole frame, byte for byte, over the same production path: the proof that a
# performance change moved no pixel. See test_frame_identity.c.
add_executable(jr_display_frame_tests
    test_frame_identity.c
    ../src/hud_render.c
    ../src/balance_wheel_data.c
    ../src/companion.c
    ../src/glass_font.c
    ../src/glass_draw.c
    ../src/glass_font_data.c
    ../src/glass_icon.c
    ../src/glass_icon_data.c
)
target_include_directories(jr_display_frame_tests PRIVATE
    ../include
    ../src
    ../../jr_ports/include
    probe/stubs
)
target_compile_options(jr_display_frame_tests PRIVATE
    -Wall -Wextra -Werror -Wno-unused-function
)
target_compile_definitions(jr_display_frame_tests PRIVATE
    JR_HOST_DIAL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../firmware/mascot/reactive"
)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME jr_display_board_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_board.py)
add_test(NAME jr_display_quiet_vendor_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_quiet_vendor.py)
add_executable(jr_display_companion_tests test_companion.c ../src/companion.c)
target_include_directories(jr_display_companion_tests PRIVATE ../include)
target_compile_options(jr_display_companion_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME jr_display_companion_tests COMMAND jr_display_companion_tests)
# The glass primitives (anti-aliased type, discs, rings, arcs, glows). Pure C,
# so like hud_render.c they stand alone; the test links libm for its analytic
# references only - the code under test uses none.
set(GLASS_SRCS ../src/glass_font.c ../src/glass_draw.c ../src/glass_font_data.c
    ../src/glass_icon.c ../src/glass_icon_data.c)
add_executable(jr_display_glass_tests test_glass.c glass_ref.c ${GLASS_SRCS})
target_include_directories(jr_display_glass_tests PRIVATE ../include)
target_compile_options(jr_display_glass_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(jr_display_glass_tests PRIVATE m)
add_test(NAME jr_display_glass_tests COMMAND jr_display_glass_tests)
add_executable(jr_display_icon_tests test_glass_icon.c ../src/glass_icon.c ../src/glass_icon_data.c ../src/glass_draw.c ../src/glass_font.c ../src/glass_font_data.c)
target_include_directories(jr_display_icon_tests PRIVATE ../include)
target_compile_options(jr_display_icon_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME jr_display_icon_tests COMMAND jr_display_icon_tests)
# Eyes-on-the-glass specimen: renders two scenes through the row API to PPMs.
# Not a suite; build it with `cmake --build build --target glass_specimen`.
add_executable(glass_specimen EXCLUDE_FROM_ALL probe/glass_specimen.c ${GLASS_SRCS})
target_include_directories(glass_specimen PRIVATE ../include)
target_compile_options(glass_specimen PRIVATE -Wall -Wextra -Werror)
target_link_libraries(glass_specimen PRIVATE m)
# Cost bench for the same primitives next to the slow reference. Host numbers
# only (x86, -O2); not a suite. Build with --target glass_bench.
add_executable(glass_bench EXCLUDE_FROM_ALL probe/glass_bench.c glass_ref.c ${GLASS_SRCS})
target_include_directories(glass_bench PRIVATE ../include)
target_compile_options(glass_bench PRIVATE -O2 -Wall -Wextra -Werror)
add_test(NAME jr_display_hud_tests COMMAND jr_display_hud_tests)
add_test(NAME jr_display_shell_tests COMMAND jr_display_shell_tests)
add_test(NAME jr_display_frame_tests COMMAND jr_display_frame_tests)
