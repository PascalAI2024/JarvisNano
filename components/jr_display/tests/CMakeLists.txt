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
)
target_include_directories(jr_display_hud_tests PRIVATE ../include)
target_compile_options(jr_display_hud_tests PRIVATE -Wall -Wextra -Werror)

# The spatial shell lives in jr_display.c, which does touch IDF. It is pulled
# into the test's own translation unit (not linked) so the tests can assert on
# render-task state without widening the public API for testing's sake; the
# IDF surface comes from probe/stubs, the same set the visual probe uses.
add_executable(jr_display_shell_tests
    test_shell.c
    ../src/hud_render.c
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

enable_testing()
add_test(NAME jr_display_hud_tests COMMAND jr_display_hud_tests)
add_test(NAME jr_display_shell_tests COMMAND jr_display_shell_tests)
