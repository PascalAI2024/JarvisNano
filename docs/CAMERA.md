# Camera — current state + tomorrow plan

Last updated: **2026-05-03**.

## TL;DR

The on-board camera is **disabled in the current firmware build** because of two cascading upstream driver bugs. Everything else on the board works (chat, mic, dashboard, Wi-Fi, BLE skeleton, FATFS, MCP, mDNS).

To get the camera working tomorrow we need to **switch the firmware from `esp_cam_sensor` (V4L2-based) to the legacy `esp32-camera` driver** — that's the path Khangura's Feb-2026 write-up confirms works on this exact hardware.

## What the hardware actually is

| Spec | Value | Source |
|---|---|---|
| SoC | ESP32-S3R8 (8 MB octal PSRAM, 8 MB flash) | boot log `octal_psram` block |
| Camera sensor | **OmniVision OV3660** (3 MP) — *not* the OV2640 documented in the Seeed wiki | chip ID `0x3660` read from register `0x300A/0x300B` over SCCB |
| SCCB address | **0x3C** — *not* the 0x30 used by OV2640 | 7-bit SCCB scan returns ACK only at 0x3C |
| SCCB pinout | SDA=GPIO40, SCL=GPIO39, port 1 (dedicated) | `boards/seeed/xiao_esp32s3_sense/board_peripherals.yaml` |
| DVP pinout | VSYNC=38 HREF=47 PCLK=13 XCLK=10 D0..D7=15/17/18/16/14/12/11/48 | matches Seeed wiki — pin map is unchanged from the OV2640 era |
| Required XCLK | **20 MHz** (OV3660 reg tables are all named `20Minput_*`; the header sets `OV3660_XCLK_DEFAULT = 20*1000*1000`) | upstream `ov3660.c` |

> ⚠️ Seeed silently swapped the on-board camera from OV2640 → OV3660 sometime around mid-2025 without updating the wiki diagrams or the product page. Both units we tested from Amazon ASIN B0C69FFVHH ship with OV3660. Anyone following the OV2640-based docs will see "camera not detected" and assume hardware failure — exactly what happened to us on day 1, costing one RMA cycle. **Don't repeat that.** If `sccb_i2c: ... failed to i2c transmit` shows in the boot log, suspect sensor mismatch first, dead chip second.

## Why the camera is off in the current build

`CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT=n` in [`boards/seeed/xiao_esp32s3_sense/sdkconfig.defaults.board`](../boards/seeed/xiao_esp32s3_sense/sdkconfig.defaults.board) — gated off because two upstream `esp_cam_sensor` bugs cascade:

### Bug 1 — DVP driver rejects every JPEG frame as `NO-SOI` *(fixed locally)*

`dvp_calculate_jpeg_size()` in [`esp_cam_ctlr_dvp_cam.c`](https://github.com/espressif/esp-video-components/blob/master/esp_cam_sensor/src/driver_dvp/esp_cam_ctlr_dvp_cam.c) requires the receive buffer's first two bytes to be the JPEG SOI marker (`0xFF 0xD8`). OV3660 prepends a few sync bytes before the actual JPEG, so every frame is rejected and the V4L2 dequeue loop logs `E:NO-SOI` ~37 times per second, starving the HTTP server.

**Fixed by [`patches/0003-dvp-cam-scan-for-jpeg-soi.patch`](../patches/0003-dvp-cam-scan-for-jpeg-soi.patch)** — scans the first 64 bytes of the buffer for the SOI marker and `memmove`s the buffer to start there. Verified: zero `NO-SOI` errors after the patch is applied. Worth submitting upstream as a PR.

### Bug 2 — `ov3660_set_format()` SCCB writes fail mid-sequence *(not fixed)*

When the V4L2 layer first opens `/dev/video2` and calls `ov3660_set_format()`, the driver writes ~50 SCCB registers in sequence. One write (register `0x88`, part of the OV3660 "common regs" block) NACKs:

```
E sccb_i2c: s_sccb_i2c_transmit_reg_a16v8(88): failed to i2c transmit
E ov3660: ov3660_set_format(563): Set common regs failed
E dvp_video: dvp_video_init(201): failed to set basic format
E esp_video: video->ops->init=103
E camera_service: Failed to open /dev/video2 (errno=16)
```

The driver then falls back to YUYV 640×480, but capture from that mode hangs on dequeue (`VIDIOC_DQBUF`). Net result: `/api/camera/snapshot` always returns HTTP 500.

Possible causes (untested): SCCB timing, sensor electrical issue at config time, or the upstream OV3660 reg table being subtly wrong for this silicon revision. Would need bench-level debugging — multimeter on SCCB lines, scope on PCLK during the format-set sequence — to confirm.

## Tomorrow's plan — switch to the legacy `esp32-camera` driver

Khangura's Feb-2026 write-up ([medium link](https://medium.com/@manjotkhangura/getting-esp32-s3-sense-ov3660-camera-working-a-weekend-deep-dive-941d9c1a05d8)) uses the **legacy `espressif/esp32-camera` component** (not `esp_cam_sensor` / `esp_video`) on this exact OV3660 batch and confirms it works. The two stacks are different code paths entirely — `esp_cam_sensor` is the newer V4L2-based replacement, but `esp32-camera` is more battle-tested and has working OV3660 init sequences.

### Concrete tomorrow checklist

1. **Add the legacy component** to `application/edge_agent/main/idf_component.yml` (or wherever the dependency is most natural):
   ```yaml
   espressif/esp32-camera: "^2.0.15"
   ```
2. **Disable `esp_cam_sensor` / `esp_video` for the camera path** — they conflict with `esp32-camera` because both want the LCD_CAM peripheral. Likely have to keep `esp_cam_sensor` linked because `lua_module_camera_service.c` uses its V4L2 ioctls; **the simplest path is rewriting `lua_module_camera_service.c` to use the legacy `esp_camera_*()` API instead of V4L2**. Khangura's write-up has a working init snippet to copy.
3. **Use Khangura's known-good config** in `board_devices.yaml` / `sdkconfig.defaults.board`:
   - `xclk_freq_hz = 20000000`
   - `CONFIG_SPIRAM_MODE_OCT=y` + `CONFIG_SPIRAM_SPEED_40M=y` (cap PSRAM speed when camera is back on — see commit `8fefc08` for the trade-off rationale)
   - `CONFIG_CAMERA_PSRAM_DMA_MODE=n`
   - `fb_count = 2`
   - `jpeg_quality = 12`
   - 30 ms `vTaskDelay` after returning each frame buffer (avoids JPEG encoder timeout)
4. **Re-flash with `STORAGE=1` ONCE** (the partition layout might shift slightly with a new component) — this wipes saved Wi-Fi credentials and LLM config. Re-provision via the dashboard's onboarding wizard at `192.168.4.1`, or by POSTing `/api/config` from the AP and restarting.
5. **Test**:
   - `curl -m 10 -o /tmp/snap.jpg http://192.168.50.80/api/camera/snapshot` → should produce a valid 1280×720 JPEG (`file /tmp/snap.jpg` confirms)
   - Dashboard camera card → "Capture frame" → image renders
   - Auto-2s checkbox → continuous live feed updates
6. **Once camera works end-to-end**, toggle the `# CONFIG_…=n` comments in `sdkconfig.defaults.board` back on, and rip out the gated-off `esp_cam_sensor`-based `lua_module_camera_service.c` if it's no longer needed.

### What's already in place to make tomorrow easy

- ✅ Wi-Fi credentials persist across reflashes ([commit `f395e88`](https://github.com/PascalAI2024/JarvisNano/commit/f395e88) — flash.sh skips storage by default)
- ✅ Wi-Fi modem-sleep is disabled so HTTP throughput is fast ([patch `0002-wifi-disable-modem-sleep.patch`](../patches/0002-wifi-disable-modem-sleep.patch))
- ✅ NO-SOI patch is in place if we ever come back to `esp_cam_sensor`
- ✅ Dashboard camera card is fully wired — `Capture frame` button, auto-2s checkbox, 4:3 viewport. Just needs `/api/camera/snapshot` to return JPEG bytes.
- ✅ Android Kotlin app has a Camera tab ([commit `abf8250`](https://github.com/PascalAI2024/JarvisNano/commit/abf8250)) — Snap once + Start live (500 ms refresh). Same — needs the endpoint.

## What we tried today that didn't work

| Approach | Result |
|---|---|
| Use `CONFIG_CAMERA_OV2640` driver | NACK at SCCB 0x30 — wrong sensor on this batch |
| Use `CONFIG_CAMERA_OV3660` + JPEG 1280×720 | Boot loop (PSRAM 80 MHz) → stable boot at PSRAM 40 MHz, but `NO-SOI` flood |
| Patch `NO-SOI` rejection (`0003-dvp-cam-scan-for-jpeg-soi.patch`) | NO-SOI errors gone, but `ov3660_set_format` SCCB writes fail mid-sequence |
| Use `CONFIG_CAMERA_OV3660` + RGB565 240×240 (small enough for internal RAM) | Stream settle still failed during open, V4L2 dequeue errno=1 |

## References

- Seeed forum thread confirming the OV2640 → OV3660 swap: <https://forum.seeedstudio.com/t/how-to-get-ov3660-for-esp32s3-sense/285171>
- Khangura's working write-up using the legacy esp32-camera driver: <https://medium.com/@manjotkhangura/getting-esp32-s3-sense-ov3660-camera-working-a-weekend-deep-dive-941d9c1a05d8>
- Upstream NO-SOI / format-set issue tracker: <https://github.com/espressif/esp-video-components/issues/52>
- xiaozhi-esp32 issue with the same symptom envelope: <https://github.com/78/xiaozhi-esp32/issues/1588>
