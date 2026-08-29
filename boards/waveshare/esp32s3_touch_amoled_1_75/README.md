# Waveshare ESP32-S3-Touch-AMOLED-1.75 — compatibility reference

This is the original 16 MB board: round 466×466 AMOLED, CST9217 touch,
ES7210/ES8311 audio, AXP2101 PMIC, TCA9554 expander, PCF85063 RTC, microSD,
and 8 MB PSRAM.

The 32 MB **1.75C** is JarvisNano’s only release-gated product target. The
original board definition and historical measurements remain for reference, but
`./scripts/build-v5.sh` intentionally refuses this revision until its 16 MB
partition, flash, OTA, audio, input, and security circuits are revalidated.
Never flash a 1.75C image onto this board: LCD/touch reset pins, MCLK, flash
geometry, and peripherals differ.

Canonical live product truth:
[`../../../docs/HARDWARE.md`](../../../docs/HARDWARE.md).

## Hardware delta

| Subsystem | Original 1.75 | 1.75C product |
|---|---|---|
| Flash | 16 MB | 32 MB |
| Display reset | GPIO39 | GPIO1 |
| Touch reset | GPIO40 | GPIO2 |
| I²S MCLK | GPIO42 | GPIO16 |
| TCA9554 | Present | Removed |
| PCF85063 | Present | Removed; SNTP time |
| microSD | Present, 1-bit SDMMC | Removed |
| Expansion header | Present | Removed/undocumented |

Shared hardware includes the ESP32-S3R8, 8 MB octal PSRAM, CO5300 display,
CST9217 touch, ES7210 ADC, ES8311 DAC, AXP2101 PMIC, QMI8658 IMU, and native
USB-Serial-JTAG.

## Original pin map

```text
I2C bus:           SDA = 15, SCL = 14
I2S0:              MCLK = 42, BCLK = 9, WS = 45, DOUT = 8, DSIN = 10
Power amp enable:  GPIO46
Display QSPI:      CS = 12, PCLK = 38, D0 = 4, D1 = 5, D2 = 6, D3 = 7, RST = 39
Touch:             RST = 40, INT = 11
SD card:           D0 = 3, CMD = 1, CLK = 2
USB-C:             ESP32-S3 native USB-Serial-JTAG
```

## Compatibility boundary

- `boards/waveshare/esp32s3_touch_amoled_1_75/` preserves the board-manager
  definition and pin data.
- `partitions_16MB.csv` preserves the historical single-slot layout; it is not a
  current release artifact.
- [`../../../docs/reference/waveshare-amoled-175.md`](../../../docs/reference/waveshare-amoled-175.md)
  and [`../../../docs/reference/sdmmc-storage.md`](../../../docs/reference/sdmmc-storage.md)
  preserve subsystem findings.
- No current voice, touch, display, tool, OTA, or security claim closes on this
  revision without a new physical verification pass.

Do not copy NVS or flash dumps between board revisions. They contain credentials,
pairing identity, and device-local state.

## References

- Product page: <https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm>
- Wiki: <https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75>
- Official BSP: <https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/bsp/esp32_s3_touch_amoled_1_75>
