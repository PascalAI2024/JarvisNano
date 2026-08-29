# Seeed XIAO ESP32-S3 Sense — experimental reference

This 8 MB XIAO board is retained for camera, compact-enclosure, PDM microphone,
and future private-companion experiments. It is **not** a JarvisNano release
build; `scripts/build-v5.sh` supports only the 32 MB Waveshare 1.75C.

## Preserved hardware definition

| Resource | Value |
|---|---|
| MCU | ESP32-S3, 8 MB flash, 8 MB octal PSRAM |
| Camera | OV2640/OV3660 batch-dependent Sense expansion |
| Microphone | On-board PDM MEMS |
| Display | Optional external round display |
| Product status | Experimental/reference only |

The board-manager source remains in this directory:

- `board_info.yaml` — identity and memory shape;
- `board_peripherals.yaml` — buses and pins;
- `board_devices.yaml` — declared devices;
- `setup_device.c` — device factories;
- `sdkconfig.defaults.board` — historical experiment defaults.

No current voice, camera, BLE, dashboard, OTA, or security claim closes on this
board without a new build and physical verification pass. Do not copy NVS,
credentials, calibration values, or flash images between this board and the
1.75C.

Use [`../../../README.md`](../../../README.md) and
[`../../../docs/HARDWARE.md`](../../../docs/HARDWARE.md) for the supported
product. Camera history remains in
[`../../../docs/CAMERA.md`](../../../docs/CAMERA.md).
