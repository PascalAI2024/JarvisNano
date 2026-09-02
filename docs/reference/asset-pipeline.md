# Asset Pipeline (EAF / AAF / `emote_assets.bin`)

**What it is** — The toolchain that converts PNG/APNG frames into `.eaf` (single-emote animation format) or `.aaf` (multi-asset mmap pack) binaries, packs them into `emote_assets.bin`, and flashes that binary to the `emote` partition at `0x820000`.

**How we use it here** — JarvisRobot's emote face animations are stored in `emote_assets.bin`. The build system packs them automatically during `idf.py build` from sources in `assets_local/284_240/` (emote.json) and `assets_local/emoji_large/` (`.eaf` files). A local Python encoder (`firmware/mascot/gen_mascot_eaf.py`) generates custom `.eaf` files when no upstream source exists.

---

## Findings & gotchas

**[2026-09-02] Baked watch dials: the encoding byte is per block, RAW beats RLE on a sunburst, and SPIFFS holds ~5.5 MB of the 6 MB partition**

`firmware/mascot/gen_watch_dials.py all` cuts the four owner-chosen dials
out of the concept sheets in `docs/evidence/20260902-dial-concepts-*.png`
(2×2 grids, one quadrant each), fits each disc to 466 px, fills the
photographed centre hole along the radius, blanks the diver's printed date,
quantises to 255 colours without dithering and emits one-frame EAFs in the
byte-verified container, choosing RAW or RLE **per 32-row block** — the
decoder reads `encoding_type = block_data[0]` per block
(`gfx_eaf_dec.c:372`), so a file may mix them. Measurements: a sunburst's
radial grain makes whole-frame RLE LARGER than raw (diver 263–285 KB RLE vs
217 KB raw; dress 284 KB RLE); a matte or near-black dial collapses under RLE
once a black floor is applied (pilot 296 KB → 82 KB, future 256 KB → 141 KB
at floor 28); per-block mixing then takes the black corners off the raw
dials too (diver 204 KB, dress 206 KB). Budget: with
`spiffsgen.py 6160384 … --page-size 256 --block-size 4096 --obj-name-len 32
--meta-len 4 --use-magic --use-magic-len` (the sdkconfig values), the
6,016 KB partition holds about 5.5 MB of files; the eight face clips take
4,728 KB, four raw dials (5,592 KB total) overflow, the shipped mix
(5,356 KB) builds and leaves 128–144 KB (padding-file probe in 16 KB
steps). Overhead arithmetic: 1,504 blocks of 16 pages, 2 lookup pages per
block, ~5 header bytes per data page, index pages per file, 2 blocks kept
free ≈ 86–88 % usable. Run spiffsgen on a staging directory before trusting
arithmetic — and write its output OUTSIDE the staging directory, or the
image counts itself on the next run. The `.eaf` byte sizes: diver 204,059,
dress 205,999, pilot 85,369, future 143,191.

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

**[2026-09-02] Three quiet faces baked procedurally; the v5 asset budget**

`rwave_rest.eaf` (24 frames, 8 fps, 223 KB), `rwave_muted.eaf` (16, 8 fps,
236 KB) and `rwave_link.eaf` (24, 12 fps, 474 KB) come from the same field
renderer as the four live faces — `firmware/mascot/gen_reactive_face.py`
(`render_rest_frame`, `render_muted_frame`, `render_link_frame`; RLE, 466×466,
96-colour palette). No image model was used: everything on these faces is a
line or a glow, and the procedural route is deterministic and byte-reproducible
(`python3 gen_reactive_face.py one rwave_rest`). Dark coils RLE to roughly a
third of an idle frame, which is why three clips cost 0.93 MB against
3.9 MB for the original five.

Budget on the 1.75C (`partitions_32MB.csv`): `emote_assets` is 0x5E0000 =
6,160,384 B. Used after this change: 4,829,828 B of clip data
(`build/emote_assets.bin` is always the full partition size; SPIFFS keeps
~1.3 MB of headroom). Every clip stays resident in PSRAM after first load
(`jr_display.c` clip cache), so the same 0.93 MB is also the PSRAM cost.

The v5 staging list is `components/jr_display/CMakeLists.txt` (`configure_file`
per clip into `build/jr_display_assets`, then `spiffs_create_partition_image`);
`face_asset()` / `face_fps()` / `hud_face_of()` in `jr_display.c` are the
runtime table, and `test_every_face_has_a_clip_and_a_hud_face`
(`components/jr_display/tests/test_shell.c`) pins basename, fps and stand-in
per `jr_face_t` so a face added without a clip fails on the host instead of
rendering blank on glass.

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
