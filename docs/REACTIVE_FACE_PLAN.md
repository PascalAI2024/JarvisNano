# Reactive "Siri-style" Face — Technical Plan (baked-EAF, audio-reactive)

> Target: Waveshare ESP32-S3-Touch-AMOLED-1.75, 466x466 round CO5300 AMOLED.
> Engine: `espressif2022/esp_emote_gfx` (the version vendored in the build clone at
> `esp-claw/application/edge_agent/managed_components/espressif2022__esp_emote_gfx`).
> This plan is grounded in the **actual** headers/source in that clone, not training data.
> Status: **INTEGRATED (ready to build + flash).** Sections below are the original design; the
> box just under records the final measured decisions made during integration.

---

## INTEGRATION STATUS — final decisions (measured, updated 2026-05-23)

**Asset config chosen:** `canvas=466`, `frames=24` per state, **RLE-encoded** EAF.
- The current premium reactive face uses the full round AMOLED canvas: HUD ring identity,
  amber non-white visor core, cyan listening energy, amber speaking energy, idle breathing
  motion, and thinking orbit/scanner motion. **Measured packed sizes:**
  `rwave_idle.eaf` 141.8KB, `rwave_listen.eaf` 229.5KB, `rwave_think.eaf` 161.9KB,
  `rwave_speak.eaf` 228.6KB — **0.73 MB total for all four states.**
- **Local generator validation:** `python3 firmware/mascot/gen_reactive_face.py check`
  verifies non-blank frames, no white-ish fallback/eye pixels, and visible ramp energy for
  listen/speak. `python3 firmware/mascot/gen_reactive_face.py all` round-trips every EAF.
- **End-to-end verified:** ran the real asset packer (`build.py` → `spiffs_assets_gen.py`) against
  the live bundle. Produced `assets.bin` = **3.51 MB** (eye 3.3MB + reactive 0.27MB + manifest/index),
  vs the **6.00 MB** `emote` partition → **2.49 MB margin. FITS. No pruning needed**
  (`swim.eaf`/`offline.eaf` kept). All four `rwave_*.eaf` confirmed embedded in the packed bin and
  resolvable by name. This packer measurement predates the full-panel premium assets; rerun it before
  flashing if partition margin is tight.
- **RLE correctness verified:** decoded every block back to exactly `width×rows` pixels; confirmed the
  amplitude ramp survived (`check` confirms listen/speak gain visible energy) and visually checked
  the 24-frame preview sheets.

**Decode-buffer / PSRAM (was §8-Q2): RESOLVED, no concern.** `gfx_anim_prepare_frame`
(`src/widget/gfx_anim.c:254`) allocates the decode buffer **per block**, sized `width × block_height`
= 280×32 = **8960 bytes**, not per frame. Canvas size has negligible RAM cost; the engine decodes
block-by-block and blends to the framebuffer.

**Bundle resolution (was §8-Q1): RESOLVED.** The packer is **manifest-driven**
(`build.py:process_board_emoji_collection` only packs `.eaf` files listed in the manifest). Chain:
`CMakeLists build_speaker_assets_bin("emote","284_240",...)` → `assets_local/284_240/config.json`
(`"emoji_collection":"emoji_large"`) → packs EAF art from `assets_local/emoji_large/` using the
manifest `assets_local/284_240/emote.json`. So: reactive EAFs live in `assets_local/emoji_large/`,
and **must have an `emote.json` entry to be packed** (added: `rwave_idle/listen/think/speak`).
`emote_get_asset_data_by_name(h, "rwave_listen.eaf")` resolves on the packed **filename**
(`emote_load.c:196-197`), which equals the manifest `src` — confirmed.

**Primary-visual wiring:** the voice helpers in `emote.c`
(`emote_set_connecting/_listening/_thinking/_speaking/_voice_idle`) now call
`emote_face_set_state(...)` **after** `emote_apply(...)` (order matters — `emote_apply`→
`emote_set_anim_emoji` re-shows the eye via `gfx_obj_set_visible(true)`, `emote_op.c:289`; the face
call then hides it). The eye emoji is still set as a **graceful fallback**: if the reactive EAFs fail
to load, `emote_face_set_state` is a no-op and the eye shows. Voice-idle turns the face OFF, restoring
the eye + Wi-Fi/model status idle screen.

**Files changed for integration:** see the report; durable changes are in `scripts/bootstrap.sh`
(patch functions), `firmware/emote/reactive_face.c`, `firmware/mascot/gen_reactive_face.py`
(added RLE), and the four vendored `firmware/mascot/reactive/*.eaf` (source-of-truth; bootstrap copies
them into the gitignored clone's `emoji_large/` bundle on every run).

---

## 0. The hard constraint, restated (why prior attempts spiraled)

The prior attempt (`firmware/emote/waveform.c`, `WAVEFORM_ENABLED 0`) drew a full-panel
RGB565A8 buffer on the CPU each frame and pushed it with `gfx_img_set_src`. On this panel that
renders as a **skewed spiral / white garbage** — proven empirically (even a 64x64 solid square
via `WAVEFORM_DIAG` spiraled). **There is no runtime canvas / pixel-draw widget in this engine.**
Do NOT revive any runtime CPU-buffer drawing path, not even for idle.

### `gfx_motion` is NOT shipped in this engine version (key finding)

`docs/RESEARCH_REFERENCES.md` and the task brief list `gfx_motion` / `gfx_motion_scene` as a
candidate (amplitude → affine transform of a baked sprite). **That widget is not present in the
vendored engine.** The shipped public widget headers are exactly:

```
managed_components/espressif2022__esp_emote_gfx/include/widget/
    gfx_anim.h   gfx_img.h   gfx_label.h   gfx_qrcode.h   gfx_font_lvgl.h
```

No `gfx_motion.h`. Therefore the affine-transform approach is **off the table** for this build.
The only sanctioned reactive lever is the **baked frame animation** widget, `gfx_anim`, driven
by per-frame segment selection. The whole design pins to `gfx_anim` alone. (If a future engine
bump adds `gfx_motion`, a scale/translate variant becomes possible; out of scope now.)

---

## 1. Chosen approach (2-3 sentences)

Each of the four face states (idle / listen / think / speak) is a **separately baked, looping
EAF animation** — one `.eaf` file per state, exactly mirroring how the existing emoji eye works
(`emote_set_anim_emoji` → `gfx_anim_set_src` → `gfx_anim_set_segment` → `gfx_anim_start`). The
face is a single custom `gfx_anim` object created on the emote display via
`emote_create_obj_by_type(handle, EMOTE_OBJ_TYPE_ANIM, "rwave")`. For the audio-reactive states
(listen/speak), the **live 0..1000 amplitude selects the looping segment's end frame** each tick
— a louder voice plays deeper into a pre-baked "amplitude ramp", so the waveform visibly grows
and shrinks with the audio — while idle/think just loop their full baked sequence.

---

## 2. Why segment-selection, not single-frame pinning (load-bearing detail)

It is tempting to bake one frame per amplitude bucket and "pin" the display to frame N with
`gfx_anim_set_segment(obj, N, N, fps, true)`. **That does not work** on this engine. From the
timer callback (`src/widget/gfx_anim.c:824`):

```c
static void gfx_anim_timer_callback(void *arg) {
    ...
    if (anim->current_frame >= anim->end_frame) {
        if (anim->repeat) { anim->current_frame = anim->start_frame; }  // resets, NO draw
        else { anim->is_playing = false; ... return; }                  // stops,  NO draw
    } else {
        gfx_anim_prepare_frame(obj);   // <-- the ONLY place a frame is decoded/drawn
        anim->current_frame++;
        ...
    }
    gfx_obj_invalidate(obj);
}
```

With `start == end == N`: `current_frame` starts at N, the first tick hits the `>=` branch,
resets to N, and **never enters the `else`** — so frame N is never prepared/drawn. Single-frame
pinning paints nothing.

**Therefore amplitude is mapped to the segment END, with a moving window that always traverses
intermediate frames:**

The draw window is **exclusive at `end`**: with `set_segment(0, end, fps, true)` the loop draws
frames `[0 .. end-1]` then resets at `cf == end` without drawing. So to display the highest *real*
frame at index `R-1` (where `R` = real frame count = `total - 1`, because `total` includes the
trailing `_C` sentinel), you must pass `end = R = total - 1`.

```c
// listen/speak: bake a monotonic "amplitude ramp" anim (frame 0 = quiet/flat,
// frame R-1 = loudest/widest). Each tick, set the loop to [0 .. end), end tracks
// level. amp 0 -> end 1 (flat line still loops + draws); amp 1.0 -> end R
// (= total-1) so the baked peak frame (index R-1) is displayed. The _C sentinel
// at index total-1 is never prepared (timer resets without drawing on cf>=end).
uint32_t last = clip->frames - 1;                  // = total-1 = max end
uint32_t end  = 1 + (uint32_t)(amp * (last - 1) + 0.5f);   // 1 .. last
gfx_anim_set_segment(obj, 0, end, fps, /*repeat=*/true);
```

Because `repeat=true` and `start=0 < end`, the loop always passes through the `else` branch and
draws. Idle/think use a fixed full-range loop (`set_segment(0, total-1, fps, true)`).

> Note on `0xFFFF`: the eye code calls `set_segment(0, 0xFFFF, fps, loop)`; `set_segment` clamps
> `end` to `total_frames-1` (`src/widget/gfx_anim.c:1005`). So passing a too-large end is safe and
> means "to the last frame (the _C sentinel index, which never draws)". We use explicit ends.

---

## 3. Engine API contract (verbatim signatures from the clone)

All confirmed by reading the headers/source in the vendored engine. Cited file:line.

### Object lifecycle (expression layer) — `managed_components/espressif2022__esp_emote_expression/include/expression_emote/emote_api.h`
```c
#define EMOTE_OBJ_TYPE_ANIM  "anim"      // emote_api.h:53
gfx_obj_t *emote_create_obj_by_type(emote_handle_t handle, const char *type_str, const char *name); // :~135
gfx_obj_t *emote_get_obj_by_name (emote_handle_t handle, const char *name);
esp_err_t  emote_set_obj_visible (emote_handle_t handle, const char *name, bool visible);
esp_err_t  emote_set_anim_visible(emote_handle_t handle, bool visible);   // hides/shows the eye
esp_err_t  emote_lock  (emote_handle_t handle);
esp_err_t  emote_unlock(emote_handle_t handle);
esp_err_t  emote_notify_all_refresh(emote_handle_t handle);
```
`emote_create_obj_by_type(h, "anim", name)` internally calls `gfx_anim_create(handle->gfx_disp)`
and registers the object under `name` (`src/emote_setup.c:245`, `:776`). The returned
`gfx_obj_t*` is a real `gfx_anim` object you drive with the `gfx_anim_*` API below.

### Anim widget — `managed_components/espressif2022__esp_emote_gfx/include/widget/gfx_anim.h`
```c
gfx_obj_t *gfx_anim_create   (gfx_disp_t *disp);                                         // :32
esp_err_t  gfx_anim_set_src  (gfx_obj_t *obj, const void *src_data, size_t src_len);     // :45
esp_err_t  gfx_anim_set_segment(gfx_obj_t *obj, uint32_t start, uint32_t end,
                                uint32_t fps, bool repeat);                              // :56
esp_err_t  gfx_anim_start    (gfx_obj_t *obj);                                           // :63
esp_err_t  gfx_anim_stop     (gfx_obj_t *obj);                                           // :70
```
`gfx_anim_set_src` parses the EAF and sets `end_frame = total_frames-1`
(`src/widget/gfx_anim.c:937,981`). The src buffer must **stay valid for the object's lifetime**
(the decoder keeps a pointer; it does not copy — see the eye's `emote_acquire_data` caching,
`src/emote_op.c:283`).

### Object positioning / visibility — `include/core/gfx_obj.h`
```c
void gfx_obj_align(gfx_obj_t *obj, gfx_align_t align, int16_t x_ofs, int16_t y_ofs);
// GFX_ALIGN_CENTER centers the anim on the 466x466 panel.
```

### The reference flow we mirror (eye animation) — `src/emote_op.c:286`
```c
gfx_anim_set_src(obj, src_data, emoji->size);
gfx_anim_set_segment(obj, 0, 0xFFFF, emoji->fps > 0 ? emoji->fps : EMOTE_DEF_ANIMATION_FPS, emoji->loop);
gfx_anim_start(obj);
gfx_obj_set_visible(obj, true);
```
Our `reactive_face.c` performs the same sequence, but re-issues `gfx_anim_set_segment` each tick
with an amplitude-derived `end` for the reactive states.

---

## 4. EAF asset format & the color-swap rule (verified vs decoder)

The local encoder `firmware/mascot/gen_mascot_eaf.py` (`emit_eaf`) is **byte-verified** against
the decoder `src/lib/eaf/gfx_eaf_dec.c` / `.h`. The format (per `gfx_eaf_dec.h:34`):

```
0   : 0x89
1-3 : "EAF"
4-7 : total frame count (int32 LE)   <-- includes the trailing "_C" sentinel frame
8-11: checksum (sum of table+data bytes, &0xFFFFFFFF)
12-15: length of (table+data)
16+ : frame table (count*8: uint32 size, uint32 offset) then frame data
```
Each frame blob = `0x5A5A` + a `_S` block: `_S\0 v1.0\0\0 bit_depth(8) w h blocks block_height
[block_len...] palette[256*BGRA] [blocks of (encoding_byte + raw indices)]`. We use
`EAF_ENCODING_RAW (5)` — uncompressed 8-bit palette indices. The final blob is `0x5A5A` + `_C`.

**Color swap (this is exactly why baked EAF works where the runtime buffer spiraled):** the engine
config sets `swap = true` for this QSPI panel (`emote.c:128`, `emote_should_swap_color`). The EAF
decoder applies the swap **inside the palette lookup** — `eaf_palette_get_color(header, idx,
swap_bytes, &result)` (`gfx_eaf_dec.h:197`) byte-swaps when `swap_bytes` is true. So the encoder
must store the palette as **UNswapped BGRA** (which `gen_mascot_eaf.py` already does:
`palette_bgra.append((b, g, r, 255))`), and the engine swaps at decode. The old runtime path had
to pre-swap pixels by hand and got it wrong — baked assets sidestep that entirely.

Palette index 0 is reserved fully-transparent (`(0,0,0,0)`), so off-circle and background pixels
are transparent → true black on the AMOLED (panel pixels OFF). Brand palette: amber `#F5870B`,
hot core `#FFE25E` on `#000000`.

---

## 5. Asset list (what the generator bakes)

One EAF per state, mounted from the **`emote` asset partition** alongside the eye assets (same
mechanism — `emote_get_asset_data_by_name`, `src/emote_load.c:185`). Centered, round-masked.

| File              | Canvas | Frames | Loop fps | Reactive? | Visual |
|-------------------|--------|--------|----------|-----------|--------|
| `rwave_idle.eaf`  | 466x466| 24     | 20       | no        | breathing Jarvis HUD shell with amber visor; no white-eye fallback |
| `rwave_listen.eaf`| 466x466| 24     | 24       | YES (mic) | cyan listening ramp: visor opens, bars/glow gain energy with mic level |
| `rwave_think.eaf` | 466x466| 24     | 24       | no        | narrowed visor, orbit particles, scanner arcs |
| `rwave_speak.eaf` | 466x466| 24     | 24       | YES (out) | amber speaking ramp: warmer bars/glow gain energy with output level |

**Canvas = 466x466.** The earlier 360x360 compromise was removed after confirming the engine decodes
block-by-block, not full-frame. RLE keeps the full-panel assets small enough for local iteration while
giving the display a deliberate state identity instead of a small center-strip equalizer.

> The "amplitude ramp" frames (listen/speak) are baked so that frame `i` draws the waveform at
> reach `i/(K-1)`. The reactive code plays `[0..end]` where `end` tracks the live level, so the
> displayed waveform expands/contracts with loudness while the loop animates the inter-bar
> shimmer for liveliness.

---

## 6. State → playback mapping (the reactive logic)

| `emote_face_state_t` | EAF            | per-tick segment call |
|----------------------|----------------|-----------------------|
| `EMOTE_FACE_IDLE`    | `rwave_idle`   | `set_segment(0, total-1, 20, true)` (full loop) |
| `EMOTE_FACE_LISTENING`| `rwave_listen`| `set_segment(0, 1 + lvl*(L-1)/1000, 24, true)` each tick (L = total-1 = max end) |
| `EMOTE_FACE_THINKING`| `rwave_think`  | `set_segment(0, total-1, 24, true)` (full loop) |
| `EMOTE_FACE_SPEAKING`| `rwave_speak`  | `set_segment(0, 1 + lvl*(L-1)/1000, 24, true)` each tick (L = total-1 = max end) |

> At `lvl=1000`: `end = 1 + (L-1) = L = total-1` — the full window, displaying the baked peak frame
> (index `total-2`). At `lvl=0`: `end = 1` — frame 0 (flat line) loops. This matches §2 and the
> `reactive_face.c` task: `end = 1 + round(amp*(clip->last - 1))`, `clip->last = total-1`.
| `EMOTE_FACE_OFF`     | —              | hide rwave obj, show eye (`emote_set_anim_visible(h,true)`) |

`lvl` (0..1000) comes from the bridge's registered callback `emote_face_amp_cb_t` (already wired:
`app_claw_face_bridge.c` → `cap_gemini_live_get_mic_level/_get_output_level`). A small driver task
(~20 Hz) reads the level, eases it (attack/decay lerp) and re-issues `set_segment` only when the
target frame changes (avoids needless engine churn). The existing public API in `emote.h`
(`emote_face_set_state`, `_set_synthetic_amplitude`, `_set_amplitude_source`) is kept **unchanged**
so the bridge and `face` CLI demo continue to work verbatim.

---

## 7. Integration points (what the orchestrator must wire)

1. **Replace `firmware/emote/waveform.c` with `firmware/emote/reactive_face.c`** (draft provided).
   It keeps the exact public symbols from `emote.h` (`emote_face_init`, `emote_face_set_state`,
   `emote_face_set_synthetic_amplitude`, `emote_face_set_amplitude_source`) so `emote.c`'s call to
   `emote_face_init(s_emote_handle)` (`emote.c:281`) and the bridge need no changes. Update the
   component `CMakeLists.txt` to compile `reactive_face.c` instead of `waveform.c`.
2. **Bake the assets:** run `python3 firmware/mascot/gen_reactive_face.py all` to produce
   `firmware/mascot/reactive/rwave_{idle,listen,think,speak}.eaf`.
3. **Mount the EAFs in the `emote` partition.** They must land in the same asset bundle the eye
   uses so `emote_get_asset_data_by_name` finds them. Add the four `.eaf` to the assets manifest /
   `spiffs_assets` source dir the build packs into the `emote` partition (same place the eye
   `neutral/listen/thinking/happy/offline` EAFs live — find via the `assets_local/466_466` install
   step that bootstrap.sh performs). The names in the manifest must match the strings in
   `reactive_face.c` (`RWAVE_ASSET_*`). **Open question — see §8.**
4. **Verify partition size.** 4 states × ~24 frames × RAW 360x360 8-bit indices ≈
   4 × 24 × 130 KB ≈ 12.5 MB *uncompressed in the EAF*. That likely **overflows** a typical assets
   partition — so either (a) bake fewer frames (12), (b) switch the encoder to RLE/heatshrink (the
   decoder supports `EAF_ENCODING_RLE=0` and `HEATSHRINK=4`), or (c) shrink the canvas. The draft
   generator defaults to 16 frames RAW; the orchestrator should measure the packed size against the
   `emote` partition in `partitions.csv` and pick the compression/frame-count tradeoff. **This is
   the single most likely blocker — size it first.**
5. **State transitions already exist:** the voice helpers in `emote.c`
   (`emote_set_listening/_thinking/_speaking/_voice_idle`) currently swap the eye emoji. To make
   the reactive face the primary visual, the orchestrator calls `emote_face_set_state(...)` from
   those same helpers (or from `cap_gemini_live`'s state machine). The bridge
   (`app_claw_face_bridge_register()`) is already called after `emote_start()`.

---

## 7b. How the build wires these files (verified in bootstrap.sh + CMakeLists)

The firmware sources are **vendored under `firmware/emote/` and copied into the gitignored clone by
`scripts/bootstrap.sh`**, not edited in the clone directly:

- `scripts/bootstrap.sh:apply_reactive_waveform_face_patch()` (line ~1396) copies
  `firmware/emote/waveform.c` → `esp-claw/components/common/emote/waveform.c`, patches `emote.h`
  (adds the face API), patches `emote.c` (forward-decl + `emote_face_init()` call after assets
  load), and patches `components/common/emote/CMakeLists.txt` SRCS to add the source file.
- **To switch to the baked-EAF face, the orchestrator updates that patch function** to vendor
  `firmware/emote/reactive_face.c` (+ `.h`) instead of `waveform.c`: change `wf_src`, the `cp -f`
  target, and the CMakeLists SRC name from `"waveform.c"` to `"reactive_face.c"`. The emote.h/.c
  patches stay identical (same public symbols + same `emote_face_init` call site). No change to
  `app_claw_face_bridge.c`.

The EAF assets are packed into the `emote` partition by the CMake macro in that same CMakeLists:
```cmake
build_speaker_assets_bin("emote" "284_240" ${EMOTE_ASSETS_FILE} ... "${.../assets_local}")
esptool_py_flash_to_partition(flash "emote" "${EMOTE_ASSETS_FILE}")
```
**Finding:** the bundle id is currently `"284_240"` (a smaller-panel asset set), even though the
panel is 466x466 — the emoji eye assets live in `components/common/emote/assets_local/`. The four
`rwave_*.eaf` must be dropped into the matching `assets_local/<bundle>/` dir so
`build_speaker_assets_bin` packs them and `emote_get_asset_data_by_name` finds them by filename.
Confirm the active bundle dir and whether it should move to `466_466` (cross-ref the
`assets_local/466_466` install the README/memory mentions). **Sizing reality (measured):** a single
360x360, 16-frame RAW EAF is ~2.0 MB; four states ≈ 8 MB. Verify the `emote` partition size in
`partitions.csv` and pick frame-count / RLE-compression / canvas accordingly (see §7.4).

## 8. Open questions for the orchestrator

1. **Asset partition packing (BLOCKER).** Where exactly does bootstrap.sh install the eye EAFs
   into the clone (the `assets_local/466_466` dir + manifest), and what is the `emote` partition
   size in `partitions.csv`? The four reactive EAFs must be added there and must fit. Decide
   RAW vs RLE/heatshrink and frame count based on the measured packed size. (The generator emits
   RAW today; an RLE path is a ~30-line addition if needed — RLE opcode layout is in
   `gfx_eaf_dec.c:eaf_decode_rle`.)
2. **Does the engine decode buffer get sized to the largest anim, or is it shared/fixed?** If a
   single decode buffer is sized from the eye assets (which may be 466x466), a 360x360 reactive
   frame is free; if it's per-object, confirm the 360x360 choice fits PSRAM budget. (Look at
   `gfx_anim_prepare_frame` allocation in `src/widget/gfx_anim.c:210+` during integration.)
3. **Should idle/think replace the eye, or coexist?** Current `emote_face_set_state` hides the eye
   (`emote_set_anim_visible(h,false)`) when any active state is set and restores it on OFF. If the
   product wants the reactive face to also be the *idle* screen (not the eye), the orchestrator
   should call `emote_face_set_state(EMOTE_FACE_IDLE)` from the idle path instead of the eye emoji.
4. **fps from audio?** An alternative reactive lever is modulating `fps` (the eye's per-emoji fps
   arg) instead of the segment end — louder = faster shimmer. The chosen design uses segment-end
   (reach) because it reads more like a waveform; fps modulation can be layered on later if wanted.

---

## 9. Files delivered with this plan

- `docs/REACTIVE_FACE_PLAN.md` — this document.
- `firmware/mascot/gen_reactive_face.py` — stdlib+PIL asset generator (bakes the 4 EAFs; reuses
  the byte-verified `emit_eaf`/`assert_roundtrip` from `gen_mascot_eaf.py`).
- `firmware/emote/reactive_face.c` + `reactive_face.h` — draft runtime face using ONLY
  `gfx_anim_*` + the `emote_*` object API (no runtime pixel buffer). Drop-in replacement for
  `waveform.c`, same public symbols.
