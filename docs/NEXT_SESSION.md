# Next Session Handoff

Last updated: **2026-07-05**.

This is the fast path back into the current Waveshare hardware state. Use USB
first. Wi-Fi checks are runtime confirmation after USB proves the app booted.

## Current Board

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.75.
- USB: ESP32-S3 native USB-Serial-JTAG. Use the detected `/dev/cu.usbmodem*`
  or configured `PORT`.
- Runtime AP: `esp-claw-XXXXXX` where the suffix is device-specific and must
  not be committed.
- Device host: use `JARVIS_DEVICE_HOST=<device-host>` locally; do not commit
  LAN addresses.

## What Was Proven

USB flashing works. The original trap was reset state: a host RTS hard reset
can leave the ESP32-S3 in ROM download mode. In that state the board accepts
flashes but does not boot the app.

The working path is the repo flasher plus the reset-safe monitor:

```bash
./scripts/flash.sh
./scripts/usb-monitor.py --seconds 5 --send status
```

Expected status after a good boot:

```text
JarvisNano USB diag ready. Type help.
boot stage=app_main
boot stage=runtime_state
boot stage=nvs
boot stage=config
boot stage=board_manager
boot stage=ui
boot stage=fatfs
boot stage=wifi_manager
boot stage=http_init
boot stage=wifi_start
boot stage=http_start
boot stage=app_claw_start
status uptime_ms=<increasing> heap=<free> wifi_ready=1 http_ready=1 app_claw_ready=1
```

## Current Firmware State

- Boot: current Waveshare lane boots with Wi-Fi and HTTP healthy when flashed
  correctly.
- Route slots: display diagnostics required a larger HTTP handler table.
- Display:
  - `/api/display/snapshot.json` reports `capture_source`,
    `display_owner`, `panel_readback:false`, and freshness.
  - `/api/display/snapshot.ppm` streams the active owner mirror.
  - `/api/ui/snapshot.ppm` streams the UI-layer framebuffer.
  - These are software mirrors, not CO5300 panel readback.
- Display transfer: CO5300 flush waits for SPI transfer completion before
  notifying the graphics runtime.
- Touch: `/api/touch` reports CST9217 status. Physical short tap routes to
  Gemini Live start/end in the current lane; long press remains reserved for
  local cockpit/menu behavior.
- Gemini Live text path: diagnostic text cycles should return audio parts and
  settle back to a stable state.
- Gemini Live voice path: push-to-talk uses manual activity boundaries.
- Mic: ES7210 codec path is the expected source.
- Speaker: firmware playback path can be inferred from Gemini audio parts;
  acoustic proof still needs hearing it or adding a tone/loopback route.

## Useful Commands

Build Waveshare firmware:

```bash
BOARD_VENDOR=waveshare \
BOARD_NAME=esp32s3_touch_amoled_1_75 \
./scripts/bootstrap.sh build
```

Flash and boot:

```bash
./scripts/flash.sh
```

Wipe saved config when needed:

```bash
ERASE_NVS=1 STORAGE=1 ./scripts/flash.sh
```

Monitor without reset-line side effects:

```bash
./scripts/usb-monitor.py --seconds 5 --send status
```

Live Wi-Fi diagnostics:

```bash
export JARVIS_DEVICE_HOST=<device-host>
scripts/live-device.py status --host "$JARVIS_DEVICE_HOST"
scripts/live-device.py screen --host "$JARVIS_DEVICE_HOST" --save-sd
scripts/live-device.py gemini-cycle --host "$JARVIS_DEVICE_HOST" --text "Say one short sentence." --report
scripts/live-device.py logs --host "$JARVIS_DEVICE_HOST" --grep touch,rwave,gemini,audio,ws
```

## Do Not Repeat

- Do not start with AP access when the board is freshly flashed or confused.
- Do not use a serial monitor that toggles modem-control lines when reset state
  matters.
- Do not assume a successful flash means the app booted.
- Do not treat `/api/display/snapshot.ppm` as panel readback.
- Do not commit keys, local URLs, LAN addresses, MACs, SSIDs, or device logs.

## Enclosure/STL Status

OpenSCAD sources can be exported with:

```bash
./scripts/export-stl.sh xiao
./scripts/export-stl.sh amoled
```

The AMOLED mascot-bust set is the relevant enclosure track for the connected
Waveshare screen board.
