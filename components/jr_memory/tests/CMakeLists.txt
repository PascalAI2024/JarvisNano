cmake_minimum_required(VERSION 3.16)
project(jr_memory_guard_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(jr_memory_guard_tests
    test_guard.c
    ../src/jr_memory_guard.c
)
target_include_directories(jr_memory_guard_tests PRIVATE ../include)
target_compile_options(jr_memory_guard_tests PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME jr_memory_guard_tests COMMAND jr_memory_guard_tests)

