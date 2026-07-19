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

enable_testing()
add_test(NAME jr_display_hud_tests COMMAND jr_display_hud_tests)
