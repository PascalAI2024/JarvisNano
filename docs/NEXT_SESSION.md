# Next Session Handoff

Last updated: **2026-05-21**.

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

## Do Not Repeat

- Do not start with AP access when the board is freshly flashed or confused.
- Do not use `screen` or pyserial defaults as the first diagnostic if reset
  state matters; they can toggle modem-control lines.
- Do not assume `hard-reset` means app boot on this board. It can be a very
  efficient way to boot exactly the wrong thing.

## Next Real Bug

The firmware boots and app services come up, but USB logs spam:

```text
E (...) TP: esp_lcd_touch_read_data(57): Touch controller must be initialized
E (...) TP: esp_lcd_touch_get_coordinates(71): Touch controller must be initialized
```

Next session should investigate the Waveshare touch init path:

1. Confirm whether `CST9217` is declared and initialized by board manager.
2. Check whether touch polling starts even when touch init fails.
3. Either fix the touch device init or gate the polling loop on a valid touch
   handle.
4. Keep USB monitoring as the primary proof while iterating.

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
