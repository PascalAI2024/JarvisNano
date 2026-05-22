# Asset Pipeline (EAF / AAF / `emote_assets.bin`)

**What it is** — The toolchain that converts PNG/APNG frames into `.eaf` (single-emote animation format) or `.aaf` (multi-asset mmap pack) binaries, packs them into `emote_assets.bin`, and flashes that binary to the `emote` partition at `0x820000`.

**How we use it here** — JarvisRobot's emote face animations are stored in `emote_assets.bin`. The build system packs them automatically during `idf.py build` from sources in `assets_local/284_240/` (emote.json) and `assets_local/emoji_large/` (`.eaf` files). A local Python encoder (`firmware/mascot/gen_mascot_eaf.py`) generates custom `.eaf` files when no upstream source exists.

---

## Findings & gotchas

**[2026-05-21] Missing EAF file → silent `"lack":true` blank face — build does NOT fail**

If an `.eaf` file named in `emote.json` is absent from the emoji collection directory (`assets_local/emoji_large/`), `build.py` does NOT abort. It logs `Warning: EAF file not found` and marks the entry `"lack": true`. The face renders blank/fallback at runtime with no error message on device.

Trap: you can have a fully successful build + flash and a silent blank face if your `.eaf` file is misnamed or in the wrong directory.

Debug checklist: verify the name in `emote.json` matches the file on disk exactly (case-sensitive); run `build.py` manually and grep its output for `lack`.

Source: memory file `project_emote_asset_pipeline.md` (build.py behavior documented from source inspection).

**[2026-05-21] Local `.eaf` encoder now exists at `firmware/mascot/gen_mascot_eaf.py`**

There is no upstream EAF encoder in `esp_emote_gfx` or its tools. The format was reverse-engineered from the binary and a local `emit_eaf()` function was written and byte-verified against `gfx_eaf_dec.c`.

The EAF format (uncompressed RAW encoding, `encoding_type = 5`):
- File header: `0x89 "EAF"` + `i32 total_frames` + `u32 checksum` (32-bit sum of all bytes from offset 16) + `u32 length`
- Frame table: `total_frames × {u32 size, u32 offset}`
- Frames: each `u16 0x5A5A` then `_S` body (`"_S" pad ver[6] u8 bi…[1 enc byte=5][raw 8-bit indices]`)
- Trailing `_C` block

Source: `firmware/mascot/gen_mascot_eaf.py` (local encoder); memory file `project_mascot_pivot_minimalist.md`.

**[2026-05-21] `esp_mmap_assets` is the canonical AAF packer — not `esp_emote_gfx`**

The `.aaf` multi-frame animation packer lives in `espressif/esp-iot-solution` at `components/display/tools/esp_mmap_assets/py_tool/spiffs_assets_gen.py`. It produces the mmap'd asset blob plus a `mmap_generate_<name>.h` header containing the `mmap_assets_table` struct (asset name, size, offset, width, height) that `EmoteDisplay::GetAssetData()` uses.

Xiaozhi's `main/scripts/spiffs_assets/spiffs_assets_gen.py` and `scripts/build_default_assets.py` are project-local copies of the same tool.

Source: `docs/RESEARCH_REFERENCES.md` (verified against `esp-iot-solution` repo structure).

**[2026-05-21] `emote_assets.bin` build flow**

The pack is triggered by `build_speaker_assets_bin("emote" "284_240" ...)` in `esp-claw/components/common/emote/CMakeLists.txt`. It calls `managed_components/espressif2022__esp_emote_assets/scripts/spiffs_assets/build_all.py` → `build.py`. The resolution directory is `assets_local/284_240/`; the art directory is `assets_local/emoji_large/`. Both paths are resolved relative to the build root.

**[2026-05-21] Reactive waveform direction: `gfx_motion` on baked art (not EAF)**

As of 2026-05-21, the primary reactive face direction is a Siri-style animated waveform driven by audio RMS at runtime, using `gfx_motion` transforms on baked art — not pre-baked EAF frames. The EAF pipeline (`firmware/mascot/`) is retained as a fallback.

States: idle breathing line / listen = mic RMS / think = scan pulse / speak = output RMS bars. Palette: `#F5870B` / `#FFE25E` on black.

Source: memory file `project_mascot_pivot_minimalist.md`.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `firmware/mascot/gen_mascot_eaf.py` | Local EAF encoder. Byte-verified against `gfx_eaf_dec.c`. Use this when generating custom `.eaf` files. |
| `firmware/mascot/gen_reactive_face.py` | Reactive face generator (waveform states). |
| `assets_local/284_240/emote.json` | Maps emote names to `.eaf` filenames. Names must exactly match files in `assets_local/emoji_large/`. |
| `esp-claw/components/common/emote/CMakeLists.txt` | Build trigger for `emote_assets.bin`. |
| [`esp_mmap_assets` packer](https://github.com/espressif/esp-iot-solution/tree/master/components/display/tools/esp_mmap_assets) | The `.aaf` packer. `py_tool/spiffs_assets_gen.py`. |
| [`xiaozhi-esp32 emoji_display.cc`](https://github.com/78/xiaozhi-esp32/blob/main/main/boards/esp-hi/emoji_display.cc) | Reference state→`.aaf` mapping. Template for our idle/listen/think/speak set. |

---

## Open questions

- Is there a published specification for the AAF format, or is it also reverse-engineered?
- The `"lack": true` silent failure: is there a build-time flag to make `build.py` error out instead of silently marking entries missing?
- Can `gen_mascot_eaf.py` produce multi-frame animations efficiently enough for 24 fps at 284×240?

---

## See also

- [display-emote-gfx.md](./display-emote-gfx.md) — how assets are consumed at runtime.
- [waveshare-amoled-175.md](./waveshare-amoled-175.md) — `emote` partition at `0x820000`.
- [build-toolchain.md](./build-toolchain.md) — overall build flow context.
