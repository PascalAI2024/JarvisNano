# Waveshare ESP32-S3-Touch-AMOLED-1.75C — revision delta

**Status:** verified on hardware 2026-08-27 (v5 `f117e6e` + 1.75C board variant; clean first-flash boot, all subsystems up, Gemini voice cycle passed).

The 1.75C is the upgraded revision of the primary board. v5 supports it via
`boards/waveshare/esp32s3_touch_amoled_1_75c/`:

```bash
./scripts/build-v5.sh          # 1.75C is the default board since 2026-08-27
NO_BUILD=1 ./scripts/flash-v5.sh
```

The original 1.75 remains a hardware/source reference. `build-v5.sh` refuses it
until its 16 MB partition/flash and physical acceptance circuits are revalidated;
a mismatched C image can black-screen or overrun that board.

## Delta vs original 1.75

| Subsystem | 1.75 (original) | 1.75C | v5 handling |
|---|---|---|---|
| Flash | 16 MB | **32 MB** (GD, quad) | 32 MB DIO with `partitions_32MB.csv`; both 4 MB app slots remain below the 16 MB executable-mapping boundary |
| PSRAM | 8 MB octal | 8 MB octal — unchanged | `CONFIG_SPIRAM_MODE_OCT` verified: full 8192K pool at boot. |
| LCD RST | GPIO39 | **GPIO1** | `#if CONFIG_ESP_BOARD_ESP32S3_TOUCH_AMOLED_1_75C` in `firmware/components/jarvis_board/src/jarvis_board.c`. |
| Touch RST | GPIO40 | **GPIO2** | `rst_gpio_num: 2` in C-board `board_devices.yaml`. |
| I2S MCLK | GPIO42 | **GPIO16** | `mclk: 16` in C-board `board_peripherals.yaml`. |
| TCA9554 expander | present @0x20 | **removed** | Device dropped from YAML; live PWR short/long handling reads AXP2101 PKEY latches directly |
| PCF85063 RTC | present @0x51 | **removed** | `jr_rtc` is deliberately not started on the C; SNTP is the wall-clock source |
| microSD | SDMMC 1-bit (CLK=2, CMD=1, D0=3) | **removed** — pins repurposed for LCD/touch reset | `fs_sdcard` dropped from YAML. Registering it on a C would fight the panel resets. |
| AXP2101 PMIC | present @0x34 | present @0x34 — unchanged | Fuel gauge online at boot (factory Brookesia demo never used it). |
| QMI8658 IMU | @0x6B, INT2→GPIO21 | @0x6B; INT routing UNVERIFIED (vendor BSP sets `BSP_CAPS_IMU 0`, examples poll); **Z axis mounted INVERTED** (measured on hardware 2026-08-27: face-up read gz>0, so flip-to-mute + mood ladder believed the device was face-down from boot) | `jr_imu` polls I2C — unaffected by INT. Z sign negated at the single read seam in `imu_read_axes()` under the C board symbol. Verify GPIO21 + X/Y orientation against the C schematic before Phase-5 deep-sleep wake. |
| Expansion header | 8-pin UART/GPIO | not documented (UNVERIFIED) | — |
| Case | bare / plastic | CNC aluminum standard | — |

All other pins (I2C SDA15/SCL14, QSPI CS12/PCLK38/D0-3=4/5/6/7, I2S BCLK9/WS45/DOUT8/DIN10,
PA-EN 46, BOOT GPIO0) are unchanged. Factory firmware is the esp-brookesia phone demo
(QIO, IDF v5.5.2); a full 32 MB backup of a factory unit exists locally (see session memory —
never commit device images).

## First-unit provisioning note

The first lab C unit was brought up with a raw NVS transplant from an original
1.75. **Do not repeat that shortcut.** It clones Wi-Fi and API credentials,
pairing identity, and assistant facts across physical devices, bypasses
field-level validation, and creates a secret-bearing flash dump. Provision
device-local values through the supported NVS/configuration workflow and keep
all dumps outside the repository.

## Sources

- Vendor repo: <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C> (examples, factory firmware, schematic PDF)
- BSP pin header: <https://github.com/waveshareteam/Waveshare-ESP32-components/blob/master/bsp/esp32_s3_touch_amoled_1_75c/include/bsp/esp32_s3_touch_amoled_1_75c.h>
- Product docs: <https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C> (not fetch-verified for the aluminum-case claim)
- Original-board reference: `docs/reference/vendor/` mirror + `boards/waveshare/esp32s3_touch_amoled_1_75/README.md`
