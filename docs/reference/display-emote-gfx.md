# Display: `esp_emote_gfx` Engine

**What it is** — `espressif2022/esp_emote_gfx` is a lightweight emotion-driven graphics engine for embedded systems. It provides a widget set for animated and static baked images on the CO5300 AMOLED panel. Version is managed via the IDF component manager.

**How we use it here** — `esp_emote_gfx` drives the JarvisRobot emote face: idle animations, voice-state transitions (listening / thinking / speaking), and the reactive waveform display. Assets are pre-baked AAF/EAF frames stored in the `emote` flash partition at `0x820000`.

---

## Findings & gotchas

**[2026-08-14] Panel commands may ONLY be issued from the render task — brightness included**

The CO5300 frame flush and every other panel command (`set_brightness`, and any
future `esp_lcd_panel_*` call) are transactions on the **same QSPI device**. v5
deliberately has no display arbiter — `ARCHITECTURE.md` "Display Ownership" —
so nothing catches a second writer. Issuing a panel command from another task
races the flush and breaks the SPI bus acquire/release pairing:

```
E bus_lock: spi_bus_lock_acquire_end(755): Cannot release a lock that hasn't been acquired.
assert failed: spi_device_release_bus spi_master.c:1413 (ret == ESP_OK)
```

Proven on hardware 2026-08-14: a mood-driven brightness ramp called
`jr_display_set_brightness()` from the **voice task**, and the crash landed
inside `panel_co5300_draw_bitmap` on the render task, mid-flush.

**The pattern to follow** (see `jr_display.c`, `brightness_pump`): a public
setter must only publish an atomic *target*; the value is applied from
`panel_flush`, which runs on the gfx render task with the previous DMA already
completed and the bus idle. Two properties come free and both matter — the
caller may be any task at any rate, and the pump short-circuits unchanged
values. Without that short-circuit an unconditional per-tick write produced
**889 identical panel writes in 100 s (8.9/s)**, which both drowned the serial
console and multiplied the race window ~9× per second.

Note this rule is broader than the older `emote_lock` guidance in
`ARCHIVE/ANIMATION_OPTIMIZATION.md` §4.3: that mutex serializes `gfx_*` calls
and does **not** protect the QSPI bus from a raw `esp_lcd_*` command.

Evidence: `docs/evidence/20260814-mood-rtc-flash-report.md` (decoded backtrace,
before/after write counts).

**[2026-05-21] There is NO `gfx_canvas` widget — runtime CPU-drawn buffers do not work**

`esp_emote_gfx`'s `include/widget/` contains: `gfx_anim.h`, `gfx_img.h`, `gfx_label.h`, `gfx_button.h`, `gfx_qrcode.h`, `gfx_motion.h`, `gfx_motion_scene.h`, `gfx_mesh_img.h`. There is **no `gfx_canvas`**.

This was proven on hardware: feeding a runtime CPU-drawn RGB565A8 buffer to `gfx_img_set_src` renders as a skewed spiral / white garbage at all sizes, including a 64×64 solid-color test square (confirmed with `WAVEFORM_DIAG=1`). The cause is NOT PSRAM cache coherency — `gfx_blend.c::gfx_sw_blend_img_draw` is a CPU software blend, so cache is not involved. Every other lever (color-swap, stride, DSC flags) was also ruled out.

Root cause: `gfx_img.c` uses `src_stride = header.w` (never reads `header.stride`); `gfx_color_t` is exactly 2 bytes (`union{uint16_t full}`); the blend math is self-consistent for 466 px. No existing reference project (xiaozhi, EchoEar, any other) drives `gfx_img_set_src` with a runtime CPU buffer. All references use flash-baked images exclusively.

**Consequence: the runtime CPU-drawn waveform path is dead.** `WAVEFORM_ENABLED` is currently `0` in `waveform.c`. The fallback baked emote face shows instead.

Source: `managed_components/espressif2022__esp_emote_gfx/` (engine internals); memory file `project_waveform_solo_state.md`.

**[2026-05-21] Sanctioned reactive-face paths (no canvas)**

Two supported paths for a reactive face without runtime CPU drawing:

1. **AAF animation player (`gfx_anim`)** — Flash-baked `.aaf` asset sequences. Call `anim_player_set_segment(handle, start, end, fps, repeat)` to select and play a segment. Drive `fps` or `start/end` from audio RMS to create reactivity. Template: xiaozhi `main/boards/esp-hi/emoji_display.cc`.

2. **`gfx_motion` / `gfx_motion_scene`** — Affine transform (scale/rotate/translate) of a baked image at runtime. A single baked ring or bar image driven by `amp_cb` via `gfx_motion` is the cleanest reactive-waveform path and avoids the dead CPU-buffer route entirely.

No public project drives a face from live audio amplitude — the amplitude→display mapping is original work.

**[2026-05-21] AAF packer is NOT in `esp_emote_gfx`**

`esp_emote_gfx/scripts/image_converter.py` only converts a single PNG to RGB565/RGB565A8. It does NOT produce `.aaf` multi-frame animation files.

The real AAF packer is the `esp_mmap_assets` component in `espressif/esp-iot-solution` (`py_tool/spiffs_assets_gen.py`). See [asset-pipeline.md](./asset-pipeline.md).

**[2026-05-21] `image_converter.py` usage**

```bash
python3 image_converter.py img.png              # RGB565A8 (alpha), C array
python3 image_converter.py img.png --format rgb565 # smaller, no alpha
python3 image_converter.py img.png --bin         # raw binary vs C array
python3 image_converter.py img.png --swap16      # byte-swap for QSPI panels (likely needed for CO5300)
python3 image_converter.py img.png --output DIR  # output directory
```

Use for static baked sprites. Pair with `esp_mmap_assets` for animated `.aaf` sequences.

Source: `esp_emote_gfx/scripts/` (local managed component copy).

**[2026-05-21] Widget API summary**

| Widget | Header | Use |
|--------|--------|-----|
| `gfx_anim` | `include/widget/gfx_anim.h` | AAF animation player — the reactive-face workhorse |
| `gfx_img` | `include/widget/gfx_img.h` | Static baked image (flash-baked only) |
| `gfx_label` | `include/widget/gfx_label.h` | Text |
| `gfx_motion` | `include/widget/gfx_motion.h` | Affine transform/tween of a baked object |
| `gfx_motion_scene` | `include/widget/gfx_motion_scene.h` | Scene-level motion composition |
| `gfx_button` | `include/widget/gfx_button.h` | Touch button |
| `gfx_qrcode` | `include/widget/gfx_qrcode.h` | QR code display |
| `gfx_mesh_img` | `include/widget/gfx_mesh_img.h` | Mesh-distorted image |

---

## Primary sources

| Source | Notes |
|--------|-------|
| [`espressif2022/esp_emote_gfx`](https://github.com/espressif2022/esp_emote_gfx) | Engine source. Widget APIs in `include/widget/`. |
| [`managed_components/espressif2022__esp_emote_gfx/`](../../managed_components/espressif2022__esp_emote_gfx/) | Local managed component. Check `gfx_img.c` and `gfx_blend.c` for blend internals. |
| [`xiaozhi-esp32 emoji_display.cc`](https://github.com/78/xiaozhi-esp32/blob/main/main/boards/esp-hi/emoji_display.cc) | Template for state→animation mapping and `anim_player_set_segment` usage. |
| [`xiaozhi-esp32 assets.cc`](https://github.com/78/xiaozhi-esp32/blob/main/main/assets.cc) | AAF asset mounting from a flash partition labelled `"assets"`. |
| [`esp-brookesia`](https://github.com/espressif/esp-brookesia) | Checked and ruled out — no `gfx_canvas`, no `anim_player` symbols. Skip for the runtime-buffer question. |

---

## Animation Performance: Known Issues & Fixes (2026-05-23)

> Full analysis: **[docs/ARCHIVE/ANIMATION_OPTIMIZATION.md](../ARCHIVE/ANIMATION_OPTIMIZATION.md)**

### 🔴 Root causes of choppy/noisy animation (in priority order)

**1. Compiler is in DEBUG mode (`-O0`) — single biggest issue**
- Config: `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` in `sdkconfig`
- Impact: 3–5× slower pixel math, EAF decode, and lerp calculations
- Fix: `idf.py menuconfig` → Compiler options → Optimization Level → **Performance (-O2)**

**2. GFX engine FPS capped at 10** (not 20/24 as intended)
- Source: `esp-claw/components/common/emote/emote.c` line 155: `.fps = 10`
- The `gfx_render_loop_task` (gfx_core.c:185–210) only renders 10× per second regardless
  of what `gfx_anim_set_segment(…, fps, …)` requests
- Fix: change to `.fps = 30` in `emote_get_default_config()`

**3. Partial render buffer too small (1.6% of screen = 29 flush strips per frame)**
- Source: `emote.c` line 158: `.buf_pixels = (size_t)s_lcd_width * 16` = 7,456 pixels
- At 30 FPS this would require 870 SPI transactions/second
- Fix: change to `.buf_pixels = (size_t)s_lcd_width * (s_lcd_height / 4)` ≈ quarter-screen strips

### ✅ What IS correctly configured
- Double buffering ON: `.double_buffer = true`
- DMA enabled: `.buff_dma = true`
- CPU at 240 MHz, PSRAM Octal at 80 MHz
- GDMA handlers in IRAM (`CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y`)
- PSRAM is DMA-capable (`CONFIG_SOC_PSRAM_DMA_CAPABLE=y`)

### [2026-06-10] Status update: fixes 1–2 applied; fix 3 had regressed, now resolved via PSRAM buffers

- Fix 1 ✅ — `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (-O2) is live in `application/edge_agent/sdkconfig:684`.
- Fix 2 ✅ — `.fps = 30` in `emote_get_default_config()` (`esp-claw/components/common/emote/emote.c`).
- Fix 3 ❌→✅ — the strip buffer had shrunk further (16 → 48 → **12 rows**) instead of growing. Cause: buffers were allocated with `MALLOC_CAP_DMA` only → **internal SRAM**, which was being fought over by SD-card DMA (see esp-claw commits `d6429e3`, `12af66b`). Only ~5.4% of a frame per strip ⇒ ~39 SPI flushes/frame ⇒ visible tearing/banding at 30 fps.
  - Resolution: set `.buff_spiram = true` alongside `.buff_dma = true` (`gfx_disp.c` allows the combo when `SOC_PSRAM_DMA_CAPABLE`, true on ESP32-S3) and raise `EMOTE_FLUSH_STRIP_ROWS` to `s_lcd_height / 4` (116 rows ⇒ 4 flushes/frame). Buffers (2 × ~108 KB) move to PSRAM, freeing ~22 KB internal SRAM for SD DMA.
  - This mirrors Waveshare's own `05_LVGL_WITH_RAM` ESP-IDF demo for this exact board: dual buffering + DMA from PSRAM is how they reach their advertised 200–300 fps LVGL benchmark (wiki: ESP32-S3-Touch-AMOLED-1.75, demo table).
  - Source: `managed_components/espressif2022__esp_emote_gfx/src/core/gfx_disp.c:57–67` (cap flag logic), `esp_emote_expression/include/expression_emote/emote_init.h` (`buff_spiram` flag).

### [2026-06-10] Smoothness round 2: emote partition 6 MB → 6.875 MB, reactive frame counts raised

- With the render pipeline fixed (30 fps engine, -O2, PSRAM quarter-strips), the remaining
  smoothness lever is baked frame count. Raised `STATE_FRAMES` in
  `firmware/mascot/gen_reactive_face.py`: idle 24→**30**, listen 16→**22**, think 24→**32**,
  speak 16→**22** (rwave total 3.71 MB; loops stay seamless because renderers parameterize on
  `t = i/n`). The runtime needs no change — `reactive_face.c` reads the real frame count from
  each EAF header and the 8 amplitude buckets scale to it.
- Budget: grew the `emote` partition `0x600000` → `0x6E0000` (6.875 MB) by folding in the 896 KB
  free top gap; `storage` moved `0xA20000` → `0xB00000` (5 MB, ends exactly at `0x1000000`).
- ⚠️ The partition CSV's source of truth is `scripts/bootstrap.sh` (`apply_emote_partition_patch`,
  ~line 2186) — it REWRITES `partitions_16MB.csv` on every build, guarded by a grep for the
  current emote size. Editing the CSV alone gets silently reverted at the next build; change the
  bootstrap heredoc (and its guard grep) instead.
- Moving `storage` requires reflashing with `STORAGE=1` (FAT re-laid-out). Wi-Fi + LLM config
  live in NVS (`0x9000`) and survive; runtime `storage_base_path` is the SD card anyway.
- Per-state playback rates are authoritative in `firmware/emote/reactive_face.c`
  (`RWAVE_FPS_IDLE` 24, `RWAVE_FPS_REACTIVE`/`RWAVE_FPS_THINK` 30) — the fps fields in
  `emote.json` are documentation-only for the rwave object.

### [2026-06-10] Fluidity round 3: segment churn + listen visibility

- **Every `gfx_anim_set_segment()` resets `current_frame` to 0.** With the loop window
  re-programmed each amplitude-bucket step, ramping speech restarted the animation every
  ~160 ms — a pulse train, not motion. `reactive_face.c` now does fast-attack/slow-decay:
  the window widens immediately on louder input, but holds `RWAVE_DECAY_HOLD_MS` (600 ms)
  before shrinking, so the loop plays through speech dips instead of restarting.
- **Listen vs idle was indistinguishable in a quiet room** (mic amp ~0 → both show a dim
  breathing core). The listen ramp in `gen_reactive_face.py` now has a baked baseline floor
  (`reach = 0.12 + 0.88·t`): frame 0 shows lit intake spokes + inner ring. Floor size cost is
  real — 0.18 cost +76 KB of RLE and left 25 KB partition headroom; 0.12 leaves ~54 KB.
  Anything visual added to ramp frames multiplies across all 22 of them.

## Open questions

- Can `gfx_motion` / `gfx_motion_scene` transform params be updated each frame from an audio RMS value? If yes, this is the cleaner reactive path than per-state AAF segment selection.
- What is the maximum `fps` argument accepted by `anim_player_set_segment`? Is it bounded by the display refresh rate or the AAF frame count?
- Is there a way to confirm `gfx_anim` uses double-buffering / DMA such that changing `fps` mid-play does not drop a frame?

---

## See also

- [asset-pipeline.md](./asset-pipeline.md) — AAF packer, EAF encoder, `emote_assets.bin` build.
- [waveshare-amoled-175.md](./waveshare-amoled-175.md) — CO5300 panel specs (466×466, QSPI).
- [audio-es8311-es7210.md](./audio-es8311-es7210.md) — Audio RMS source for amplitude-reactive face.
- [ANIMATION_OPTIMIZATION.md](../ARCHIVE/ANIMATION_OPTIMIZATION.md) — Historical performance audit and change sequence.
