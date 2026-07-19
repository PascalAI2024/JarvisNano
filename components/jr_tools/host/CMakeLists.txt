cmake_minimum_required(VERSION 3.16)
project(jr_tools_template_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

add_executable(jr_tools_template_tests
    test_templates.c
    ../src/jr_tools_templates.c
    ${REPO_ROOT}/components/jr_transport/vendor/cJSON/cJSON.c
)
target_include_directories(jr_tools_template_tests PRIVATE
    ../include
    ${REPO_ROOT}/components/jr_transport/vendor/cJSON
    stubs
)
target_compile_options(jr_tools_template_tests PRIVATE -Wall -Wextra -Werror)
enable_testing()
add_test(NAME jr_tools_template_tests COMMAND jr_tools_template_tests)
