# Next Session Handoff

Last updated: **2026-05-24**.

This is the fast path back into the current hardware state. Use USB first. AP
checks are only runtime confirmation after USB proves the firmware booted.

## Current Board

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.75
- USB port: `/dev/cu.usbmodem1101`
- Chip: ESP32-S3, native USB-Serial-JTAG
- MAC: device-specific (see local notes; not stored in this repo)
- Runtime AP observed after boot: `esp-claw-XXXXXX` (suffix = last 3 MAC bytes)
- AP IP observed after boot: `192.168.4.1` (ESP softAP default)

## What Was Proven

USB flashing works. The original confusion was reset state, not whether the Mac
had USB access.

Hard reset through RTS can leave the ESP32-S3 in ROM download mode. Evidence:

```text
0x60004038 = 0x00000023
boot:0x23 (DOWNLOAD(USB/UART0))
```

That means GPIO0 was sampled as a download strap during reset. In that state
the board accepts flashes but does not boot the app.

The working path is watchdog reset:

```bash
./scripts/flash.sh
./scripts/usb-monitor.py --port /dev/cu.usbmodem1101 --seconds 2 --send status
```

Expected USB status after a good boot:

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
boot stage=portal ap_ssid=esp-claw-XXXXXX ap_ip=192.168.4.1
boot stage=app_claw_paths
boot stage=app_claw_start
status uptime_ms=10043 heap=4542476 wifi_ready=1 http_ready=1 app_claw_ready=1
```

## Important Local State

`scripts/flash.sh` now writes firmware with `--after no-reset`, then performs a
separate watchdog reset. This avoids the host RTS hard-reset path that kept
landing the board in ROM download mode.

`scripts/usb-monitor.py` opens the USB serial device without pyserial modem
control toggles, so it does not disturb DTR/RTS while monitoring logs or sending
a command.

The current flashed firmware includes a USB diagnostic shell patched into the
generated `esp-claw/` checkout. That directory is ignored, so treat the shell as
local diagnostic state unless it is later promoted into tracked bootstrap
patches.

For Wi-Fi runtime diagnostics, use the live-device harness:

```bash
export JARVIS_DEVICE_HOST=<device-host-or-ip>
scripts/live-device.py status --host $JARVIS_DEVICE_HOST
scripts/live-device.py screen --host $JARVIS_DEVICE_HOST --save-sd
scripts/live-device.py gemini-cycle --host $JARVIS_DEVICE_HOST --text "Say one short sentence." --report
scripts/live-device.py logs --host $JARVIS_DEVICE_HOST --grep touch,rwave,gemini,audio,ws
```

See [`docs/LIVE_DEVICE_DEBUG.md`](./LIVE_DEVICE_DEBUG.md). It classifies the
current bad signatures instead of making the next agent rediscover them with
curl and optimism.

## Do Not Repeat

- Do not start with AP access when the board is freshly flashed or confused.
- Do not use `screen` or pyserial defaults as the first diagnostic if reset
  state matters; they can toggle modem-control lines.
- Do not assume `hard-reset` means app boot on this board. It can be a very
  efficient way to boot exactly the wrong thing.

## Current Firmware State (2026-05-24)

- **Boot**: current flashed build boots with STA Wi-Fi and HTTP healthy. Use
  `JARVIS_DEVICE_HOST` locally; do not commit LAN IPs.
- **Route-slot incident**: adding display diagnostics initially boot-looped with
  `httpd_register_uri_handler: no slots left`; fixed by raising HTTP
  `max_uri_handlers` to 40.
- **Display diagnostics**:
  - `GET /api/display/snapshot.json` gives lightweight animation heartbeat data.
  - `GET /api/display/snapshot.ppm?save=1` streams a PPM and saves a copy under
    `/sdcard/diagnostics/`.
  - `GET/POST /api/display/face` reads and forces reactive face state.
- **Animation/display transfer fix**: the emote flush path now waits for CO5300
  SPI transfer completion before notifying GFX. Render strips use internal DMA
  buffers, avoid `.buff_spiram`, and run 12-row chunks with QSPI at 20 MHz and
  queue depth 2. This keeps transfers under the board's `max_transfer_sz` and
  removed the physical horizontal green lines that did not appear in framebuffer
  screenshots. Verified: `driver_ticks` and `frame_id` continue increasing after
  25+ seconds and after a full screenshot capture.
- **Screenshot color fix**: capture conversion byte-swaps panel-order RGB565 and
  maps the old transparent/pink key to black. Verified local PNG:
  `.build_logs/live-device/20260524-004411-screen.png`.
- **Gemini Live text path**: verified start/text/stop over Wi-Fi. Text requests
  produce `audio_parts > 0`, `turn_complete=1`, `generation_complete=1`, and
  resume to `LISTENING`.
- **Gemini Live voice path**: push-to-talk uses manual activity boundaries:
  `activityStart`, codec-captured PCM frames, then `activityEnd`. Do not revert
  this to `audioStreamEnd`; that left the board in `THINKING` with no
  `serverContent`.
- **Mic**: voice capture now prefers the ES7210 codec path. Verified counters:
  `tx_codec_reads > 0`, `tx_raw_reads=0`, `tx_send_failures=0`,
  `tx_read_failures=0`.
- **Speaker**: verified indirectly through Gemini voice response:
  `audio_parts > 0`, `turn_complete=1`, `generation_complete=1`, `drops=0`.
  Acoustic proof still requires hearing it or adding a tone/loopback route.
- **Touch route**: short tap routes to Gemini Live. First tap starts listening;
  the next tap ends the input stream. Long press remains available for local
  hardware demo behavior.

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
./scripts/usb-monitor.py --port /dev/cu.usbmodem1101
./scripts/usb-monitor.py --port /dev/cu.usbmodem1101 --seconds 2 --send status
```

Read strap register if boot mode is suspect:

```bash
.build_tools/esptool/bin/esptool --chip esp32s3 \
  --port /dev/cu.usbmodem1101 \
  --before no-reset --after no-reset \
  read-mem 0x60004038
```

Known bad symptom:

```text
boot:0x23 (DOWNLOAD(USB/UART0))
```

## Enclosure/STL Status

The OpenSCAD sources have been exported to printable STL files under:

```text
hardware/enclosure/dist/
```

Use:

```bash
./scripts/export-stl.sh xiao
./scripts/export-stl.sh amoled
```

The AMOLED mascot-bust STL set is the relevant one for the connected Waveshare
screen board.
