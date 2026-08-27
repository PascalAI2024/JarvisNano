# Next Session Handoff

Last updated: **2026-08-27**.

v5 now runs on the **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (the upgraded
32 MB-flash revision in the aluminum case) — this is Pascal's live device.
USB first. Wi-Fi is confirmation after the app has actually booted.

## Current Board

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.75**C** (466×466 CO5300, CST9217,
  ES8311 + ES7210, AXP2101, 32 MB flash). Delta table + gotchas:
  [`reference/board-175c.md`](reference/board-175c.md).
- `./scripts/build-v5.sh` now **defaults to the 1.75C**. Building for the
  original 1.75 requires `BOARD_NAME=esp32s3_touch_amoled_1_75` — a mismatched
  image black-screens either board (LCD reset moved GPIO39→1 on the C).
- Verified on hardware 2026-08-27: clean boot, display + touch + gestures +
  AXP2101 fuel gauge + full Gemini Live voice cycle. No PCF85063 on the C —
  the RTC warning at boot is expected; NTP seeds the clock.
- USB: ESP32-S3 native USB-Serial-JTAG. Typical macOS path `/dev/cu.usbmodem*`.
- The original 1.75 unit still exists and remains supported via the default
  board dir.
- Live firmware: `components/jr_*` + `main/main.c` via `./scripts/build-v5.sh`.
- Legacy `firmware/` + `esp-claw/` + `scripts/bootstrap.sh` is the old overlay stack. The v5 CMake does **not** compile it.
- Device host: `JARVIS_DEVICE_HOST` locally. Never commit LAN addresses, SSIDs, MACs, or keys.

## Build and talk to the board

```bash
./scripts/build-v5.sh
NO_BUILD=1 PORT=/dev/cu.usbmodemXXXX ./scripts/flash-v5.sh
python3 scripts/usb-monitor.py --port /dev/cu.usbmodemXXXX --seconds 8 --send status
```

Do not use a serial monitor that toggles DTR/RTS. That can leave the S3 in ROM
download mode: flash succeeds, app never boots.

Host tests (no board required):

```bash
cmake -S host -B host/build && cmake --build host/build && (cd host/build && ctest --output-on-failure)
```

## What is already shipped on v5

July 18–19 hardware evidence lives in [`docs/evidence/`](evidence/README.md):
clean boot, thinking spinner, choice arcs, watch face, captions, attract reel.

Later commits on `v5` also landed flip-to-mute, shake-to-cancel, time-aware
courtesy, and the Gemini API key on `x-goog-api-key` instead of the query
string. Rotate any key that appeared in pre-fix serial/SD logs (owner: Pascal).

## Still open (do not re-plan)

- Phase 5 power moods need WakeNet. No `CONFIG_SR_WN*` in this image.
- Pairing token on writes is not required in current public builds.
- BLE, Android privacy mode, camera, XIAO parity are post-v1.
- Uncommitted local WIP when this note was written: extra `idf.py reconfigure`
  after `gen-bmgr-config` in `scripts/build-v5.sh`, plus
  `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` in `sdkconfig.defaults`.

## Do Not Repeat

- Do not start from `docs/ARCHIVE/` or root `plan.md` (archived). Use this file + [`ROADMAP.md`](ROADMAP.md).
- Do not edit `esp-claw/` copies. v5 does not consume them; bootstrap still overwrites that tree.
- Do not treat `/api/display/snapshot.ppm` as panel readback.
- Do not commit keys, LAN addresses, MACs, SSIDs, or device logs with those in them.
- Do not draw a second HUD on top of the baked `rwave_*.eaf` art. Negative-space rule is in `JARVISNANO_OS_PLAN.md`.

## Docs

[`DOCUMENTATION_MAP.md`](../DOCUMENTATION_MAP.md) is the index.
[`docs/reference/`](reference/README.md) is the gotcha knowledge base.
