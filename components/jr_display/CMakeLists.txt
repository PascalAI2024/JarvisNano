# jr_display — L5 Display / Presenter adapter (Phase 0: empty-but-registered
# skeleton). Phase 3 fills this in: the Presenter mapping StateSnapshot -> baked
# AAF segment + 8-bucket amplitude on emote_gfx, on its OWN task + failure
# domain so the display survives touch/audio death. NO runtime CPU framebuffer
# (hardware.md #5) — reactivity is flash-baked AAF/EAF.
idf_component_register(
    SRCS "src/jr_display.c"
    INCLUDE_DIRS "include"
    REQUIRES jr_ports
    PRIV_REQUIRES esp_emote_gfx
)
