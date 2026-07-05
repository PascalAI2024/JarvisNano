# JarvisNano Waveshare BSP Probe

Standalone hardware proof for the Waveshare ESP32-S3-Touch-AMOLED-1.75.

This deliberately bypasses `esp-claw`, `esp_board_manager`, Gemini, and the bootstrap patch layer. Its only job is to prove the official Waveshare BSP can initialize the CO5300 display, register CST9217 touch, and draw a visible LVGL screen.

## Build

```bash
scripts/bsp-probe.sh build
```

## Flash

Flash with DIO unless a real-device A/B test proves QIO safe:

```bash
scripts/bsp-probe.sh flash -p /dev/cu.usbmodemXXXX
```

The helper builds in Docker and uses a local temporary Python tool runner for
flash/monitor because macOS serial passthrough through Docker is unreliable.

## Monitor

```bash
BSP_PROBE_MONITOR_SECONDS=10 scripts/bsp-probe.sh monitor -p /dev/cu.usbmodemXXXX
```

## Expected screen

The probe should show a dark round-cockpit status screen with:

- `JarvisNano BSP Probe`
- display/touch primitive labels
- live heap counters
- touch coordinate updates when the screen is pressed

If this does not render, the display failure is below Jarvis application code. If it renders, the next step is a `jarvis_board` adapter that replaces board-manager display/touch/audio acquisition with BSP handles.
