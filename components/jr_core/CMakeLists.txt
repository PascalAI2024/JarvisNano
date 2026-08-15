# jr_core — L3 session state machine + L4 turn policy (the inner core).
#
# THE ONE HARD RULE: this component may depend ONLY on jr_ports (pure headers)
# and jr_dsp (pure math). It declares NO dependency on any driver, network, or
# model component. The host/ build compiles exactly these sources with no
# ESP-IDF in the include path (host/CMakeLists.txt) — a stray driver include
# fails there before a 15-20 min device build is ever attempted.
idf_component_register(
    SRCS
        "src/session.c"
        "src/turn_policy.c"
        "src/monitors.c"
        "src/snapshot.c"
        "src/orchestrator.c"
        "src/mood.c"
    INCLUDE_DIRS "include"
    REQUIRES jr_ports jr_dsp
)
