# jr_ports — L-seam: the C port interfaces (struct-of-fn-pointers) ONLY.
# Header-only, zero implementation, zero dependencies. Registering with no SRCS
# makes this an INTERFACE library that only exports include/. Any component that
# REQUIREs jr_ports links against pure headers and nothing else — this is the
# dependency-injection seam between the pure core and the replaceable adapters.
idf_component_register(
    INCLUDE_DIRS "include"
)
