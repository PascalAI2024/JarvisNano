# BSP Primitive Reboot

**What it is** — A 2026-07-04 reset of the Waveshare ESP32-S3-Touch-AMOLED-1.75 board strategy around the vendor BSP component `waveshare/esp32_s3_touch_amoled_1_75` v3.0.1, instead of expanding generated-tree patches inside `esp-claw`.

**How we use it here** — JarvisRobot treats board bring-up as a narrow primitive: display ownership, touch, audio, I2C, I2S, SD, and PMIC setup stay below the Jarvis application. Gemini Live, memory, diagnostics, and personality UI stay as Jarvis application code above that layer.

**Current integration result** — The recovery branch did not land the full BSP/LVGL migration yet. The verified path is deliberately smaller: `jarvis_board` owns the CO5300 QSPI display directly through `esp_lcd_co5300`, while `esp_board_manager` still owns touch/audio/SD/PMIC. This removes the display race without changing the voice stack, storage layout, or Gemini runtime.

---

## Findings & gotchas

**[2026-07-04] Exact-board BSP exists and is the right primitive**
The exact board now has a registry BSP: `waveshare/esp32_s3_touch_amoled_1_75` v3.0.1. It depends on IDF `>=5.5`, `esp_lcd_co5300`, `esp_lcd_touch_cst9217`, `esp_codec_dev`, `esp_lvgl_adapter`, and LVGL. That covers the board bring-up currently being recreated through `esp_board_manager` YAML and bootstrap patches.

**[2026-07-04] Do not jump to ESP-IDF 6 during display recovery**
ESP-IDF 6.x exists, but the BSP supports IDF 5.5 and this repository already builds on `espressif/idf:v5.5.4`. IDF 6, `esp_board_manager` 0.6.x, and `esp-claw` changes should be a separate migration. Combining those with display recovery makes the failure surface larger for no immediate hardware benefit.

**[2026-07-04] BSP first, then Jarvis adapter**
The clean architecture is:

1. `jarvis_board` owns board setup and returns typed handles: LVGL display, touch, speaker codec, microphone codec, shared I2C, and optional SD/PMIC helpers.
2. Display service uses BSP + `esp_lvgl_adapter` first. Runtime diagnostics should prefer LVGL snapshot primitives when running on LVGL.
3. Touch service uses `esp_lcd_touch_cst9217` through BSP. No raw CST9217 parser in app code unless it is a temporary diagnostic.
4. Audio service uses BSP-created `esp_codec_dev` handles for ES8311/ES7210. Jarvis-specific sample-rate, barge-in, AEC, and channel-demux logic remains above the board layer.
5. Gemini Live, Jarvis memory, HTTP tools, and the face/cockpit state machine do not know GPIO numbers, panel register commands, or codec register tables.

**[2026-07-04] Keep DIO flash until hardware proves QIO safe**
Waveshare examples commonly use QIO/OPI settings, but JarvisRobot's known-good flashing rule is still `idf.py flash --flash-mode dio` for the CO5300 board. Do not switch this while recovering display boot unless a real-device A/B test proves QIO stable.

**[2026-07-04] Probe before integration**
The first implementation artifact is `experiments/waveshare_bsp_probe/`: a standalone IDF app that depends directly on the Waveshare BSP and draws a visible LVGL status screen with touch feedback. If that app does not show a real screen on hardware, the problem is below Jarvis. If it does, the integration target is clear.

**[2026-07-04] BSP v3.0.1 does not compile with `CONFIG_BSP_ERROR_CHECK=n`**
The BSP's non-asserting error macro `BSP_ERROR_CHECK_RETURN_ERR()` returns an `esp_err_t`. `bsp_io_expander_init()` returns `esp_io_expander_handle_t`, so IDF 5.5.4 with `-Werror=all` fails with `-Wint-conversion` when `CONFIG_BSP_ERROR_CHECK` is disabled. Keep the probe on the BSP default `CONFIG_BSP_ERROR_CHECK=y` until upstream fixes that macro/function mismatch or we carry a tiny, explicit BSP patch.

**[2026-07-04] Hardware smoke passed with the standalone BSP probe**
`experiments/waveshare_bsp_probe/` built with `espressif/idf:v5.5.4`, flashed in DIO mode, and booted on the connected Waveshare unit. Serial logs show PSRAM OK, CO5300 panel creation, CST9217 touch detection at 466x466, LVGL touch registration, backlight enabled, and `probe UI started`. This proves the vendor BSP can bring up display + touch on hardware before Jarvis application integration.

**[2026-07-04] Landed architecture: direct display primitive, not generated display YAML**
`esp_board_manager` no longer registers the CO5300 display. The board YAML keeps shared I2C, I2S audio, PMIC, SD, and CST9217 touch, but leaves SPI2/QSPI display ownership to `jarvis_board`. This avoids board-manager pre-claiming the display bus and lets the Jarvis app expose a stable `esp_lcd_panel_handle_t` + `esp_lcd_panel_io_handle_t` to emote and UI code.

**[2026-07-04] Display proof now happens at boot and over HTTP**
Boot calls `ui_layer_init()` and posts a short cockpit frame after `app_claw_ui_start()`. Serial proof includes `ui_layer ready`, `cockpit frame presented`, and no `ESP_ERROR_CHECK` abort. Runtime proof uses `/api/display/snapshot.json`, `/api/display/snapshot.ppm`, and `/api/ui/snapshot.ppm`; the last endpoint streams the UI visible framebuffer row-by-row so it does not need another full-frame allocation.

**[2026-07-04] Touch demo is not boot-critical**
`touch_demo_start()` can return `ESP_ERR_NOT_SUPPORTED` on the direct-display build. Treating that as fatal caused the post-display boot loop. The generated `main.c` patch now logs `touch demo disabled: ESP_ERR_NOT_SUPPORTED` and continues, so a diagnostic/toggle feature cannot take down a working panel.

**[2026-07-04] PSRAM and framebuffer allocation are part of display correctness**
The working build enables octal PSRAM and lets Wi-Fi/LWIP prefer SPIRAM. `display_hal` uses a small chunked swap buffer for CO5300 byte-order submission, while snapshots are allocated lazily only when a consumer asks. This leaves enough memory for emote, HTTP diagnostics, and one active `ui_layer` scene.

**[2026-07-04] Current primitive versions checked**
The direct-display path uses `espressif/esp_lcd_co5300` `^2.1.0`, which the ESP Component Registry lists as latest and IDF `>=5.4` compatible. The full Waveshare BSP latest stable is `3.0.1` and remains IDF `>=5.5` compatible. ESP-IDF v6.x exists, but the recovered Jarvis build stays on the IDF 5.5 lane because the display failure was architecture/order, not lack of a newer major IDF.

---

## Primary sources

| Source | Notes |
|--------|-------|
| [Waveshare BSP component](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75) | Exact-board ESP Component Registry package. |
| [Waveshare BSP v3.0.1 dependencies](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75/versions/3.0.1/dependencies?language=en) | Dependency set: IDF `>=5.5`, CO5300, CST9217, LVGL adapter, codec, IO expander. |
| [Waveshare BSP source](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/bsp/esp32_s3_touch_amoled_1_75) | Source for `bsp_display_start`, `bsp_touch_new`, and codec helpers. |
| Waveshare BSP header | BSP declares display, touch, speaker, mic, SD, shared bus capabilities, and board pin constants. |
| Waveshare BSP implementation | Source for the CO5300 panel gap and `bsp_display_start()` LVGL bring-up entrypoint. |
| `experiments/waveshare_bsp_probe/build/log/idf_py_stderr_output_397` | First probe build failure showing `CONFIG_BSP_ERROR_CHECK=n` pointer-return compile bug. |
| `experiments/waveshare_bsp_probe/README.md` | Reproducible build/flash expectations for the standalone BSP smoke test. |
| `firmware/components/jarvis_board/src/jarvis_board.c:18` | Direct CO5300 geometry, QSPI host, and pin definitions. |
| `firmware/components/jarvis_board/src/jarvis_board.c:80` | `jarvis_board_display_get()` initializes the QSPI bus and CO5300 panel. |
| `firmware/components/jarvis_board/src/jarvis_board.c:135` | Runtime log marker for successful direct display bring-up. |
| `boards/waveshare/esp32s3_touch_amoled_1_75/board_peripherals.yaml:50` | Display intentionally removed from board-manager peripheral ownership. |
| `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:95` | CST9217 touch remains board-manager-owned on shared I2C. |
| `firmware/ui_layer/ui_layer.c:380` | Boot cockpit render and `cockpit frame presented` marker. |
| `firmware/ui_layer/ui_layer.c:397` | Active data-panel scene used for UI screenshot proof. |
| `scripts/bootstrap.sh:3794` | Idempotent display HAL capture patch for `/api/ui/snapshot.ppm`. |
| `scripts/bootstrap.sh:4392` | Idempotent generated-main boot cockpit patch. |
| `scripts/bootstrap.sh:4464` | Idempotent touch-demo soft-fail patch. |
| `scripts/bootstrap.sh:4696` | Build patch order: memory seed, chunked swap, capture, cockpit, touch soft-fail. |
| [ESP Component Registry: esp_lcd_co5300 2.1.0](https://components.espressif.com/components/espressif/esp_lcd_co5300/versions/2.1.0/dependencies?language=en) | CO5300 driver latest, IDF `>=5.4`. |
| [ESP Component Registry: Waveshare BSP versions](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75/versions/3.0.0/versions?language=en) | Board BSP latest stable `3.0.1`. |
| [ESP-IDF v5.5.4 release](https://github.com/espressif/esp-idf/releases/tag/v5.5.4) | Current build lane used by this firmware. |
| [ESP-IDF releases](https://github.com/espressif/esp-idf/releases) | Confirms newer IDF releases exist; migration intentionally deferred. |
| [LVGL snapshot docs](https://docs.lvgl.io/master/details/auxiliary-modules/snapshot.html) | Runtime screen/widget snapshot primitive for the LVGL path. |
| `docs/reference/vendor/board-examples/esp-idf/02_lvgl_demo_v9/main/main.c:14` | Vendor example starts display with `bsp_display_start()` and then runs LVGL. |
| `docs/reference/vendor/board-examples/esp-idf/05_Spec_Analyzer/components/bsp_extra/src/bsp_board_extra.c:145` | Vendor audio example creates speaker and microphone codec handles through BSP. |

---

## Open questions

- Should the next branch replace the direct CO5300 primitive with the full Waveshare BSP/LVGL adapter, or keep the smaller direct-display layer now that hardware is stable?
- The CST9217 hardware probes, but `/api/touch` still reports touch disabled when the local touch demo is not supported. Decide whether touch should be restored through a dedicated touch service instead of demo glue.
- Does BSP audio expose enough control for Jarvis's 16 kHz mic input, 24 kHz playback, barge-in, and AEC reference path without raw I2S fallback?
- Can `esp_emote_gfx` coexist cleanly with the BSP/LVGL adapter, or should the first recovered UI be LVGL-only?

---

## See also

- [waveshare-amoled-175.md](./waveshare-amoled-175.md) — exact board hardware notes.
- [display-emote-gfx.md](./display-emote-gfx.md) — current emote engine constraints.
- [audio-es8311-es7210.md](./audio-es8311-es7210.md) — current Gemini audio path.
- [build-toolchain.md](./build-toolchain.md) — IDF image and build tool pins.
