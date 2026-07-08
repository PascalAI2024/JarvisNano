# jr_dsp — pure DSP math (inner core). Depends on the C standard library ONLY.
# NO REQUIRES on any driver/network/model/IDF component. The host build
# compiles this exact source with no IDF in the include path — a broken edge
# fails there immediately (architecture.md §Enforcement).
idf_component_register(
    SRCS "src/dsp.c"
    INCLUDE_DIRS "include"
)

# libm for sqrt() in the ESP-IDF build (host build links -lm in host/CMakeLists).
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
