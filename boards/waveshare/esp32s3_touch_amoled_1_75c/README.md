# Waveshare ESP32-S3-Touch-AMOLED-1.75C

The C revision of the primary JarvisNano board: round 466x466 AMOLED,
capacitive touch, ES7210 microphones, ES8311 speaker output, AXP2101 PMIC,
**32 MB flash** (double the original), and 8 MB PSRAM, in a CNC aluminum case.

Deltas vs [`../esp32s3_touch_amoled_1_75/`](../esp32s3_touch_amoled_1_75/):
LCD RST moved 39→1, touch RST moved 40→2, I2S MCLK moved 42→16; the TCA9554
IO expander, PCF85063 RTC, microSD slot, and 8-pin expansion header were
removed. PWR-button state is read from AXP2101 PKEY status latches over I2C. Source:
[waveshareteam/ESP32-S3-Touch-AMOLED-1.75C](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C)
BSP header + examples (see `docs/reference/`).

Live debug handoff: [`../../../docs/NEXT_SESSION.md`](../../../docs/NEXT_SESSION.md).

## Hardware

| Subsystem | Chip | Bus | JarvisNano use |
|---|---|---|---|
| SoC | ESP32-S3R8 | native | 32 MB flash, 8 MB octal PSRAM |
| Display | CO5300 | QSPI on SPI2_HOST | 466x466 AMOLED face and cockpit |
| Touch | CST9217 | I2C | Tap, long-press, and diagnostics |
| Audio ADC | ES7210 | I2S + I2C | Dual mic input and echo-reference lane |
| Audio DAC | ES8311 | I2S + I2C | Speaker output via board connector |
| PMIC | AXP2101 | I2C | Rails, fuel gauge, PKEY power button |
| IMU | QMI8658 | I2C | Gestures (flip-to-mute, shake, lift) |

No TCA9554, no PCF85063 RTC (AXP2101's internal RTC domain only), no microSD.

## Official-manual integration

| C-board capability | JarvisNano integration | Deliberate boundary |
|---|---|---|
| AXP2101 ADC, fuel gauge, VBUS and PKEY status latches | Battery %, millivolts, USB/charge state, 500 ms PKEY polling with short/long classification | Factory rail sequencing and charge-current registers remain untouched |
| QMI8658 | ±8 g/125 Hz accelerometer, orientation, lift/flip/shake, C-mount Z correction | Gyro disabled to save power; INT wake is not claimed until the C schematic/pin is proven |
| CO5300 AMOLED | QSPI presenter, brightness slew, round spatial shell, charging comet, snapshots | No panel-readback claim; snapshots are the software mirror |
| CST9217 | IRQ-driven tap/hold/four-way swipe with physical-vs-synthetic provenance | Synthetic QA events cannot approve consent, answer asks, control Codex, or clear privacy |
| ES7210 + ES8311 | 24 kHz shared-clock full duplex, 24→16 kHz uplink, AEC reference, native Gemini server VAD | Local barge remains diagnostic; Gemini owns native interruption |
| ESP32-S3R8 + 32 MB flash | 8 MB PSRAM, dual 4 MB OTA slots below the 16 MB executable mapping boundary | OTA signing/TLS remain release blockers; do not burn secure-boot eFuses from an ordinary update |
| Four Jarvis mood states | AWAKE/AMBIENT/WHISPER/DREAM drive brightness, voice session ownership, and safe Wi-Fi modem sleep | CPU/light-sleep and PMIC rail policies wait for measured current and wake-source proof |

## Pin Map

```text
I2C bus:           SDA = 15, SCL = 14
I2S0:              MCLK = 16, BCLK = 9, WS = 45, DOUT = 8, DSIN = 10
Power amp enable:  GPIO46
Display QSPI:      CS = 12, PCLK = 38, D0 = 4, D1 = 5, D2 = 6, D3 = 7, RST = 1
Touch:             RST = 2, INT = 11
USB-C:             ESP32-S3 native USB-Serial-JTAG
```

## Build

From the repo root:

```bash
BOARD_NAME=esp32s3_touch_amoled_1_75c ./scripts/build-v5.sh
```

## Flash

```bash
NO_BUILD=1 ./scripts/flash-v5.sh
```

The script preserves NVS by default. Use `ERASE_NVS=1` when saved config is
bad.

The board can land in ROM download mode if a host hard reset samples the boot
strap incorrectly. Prefer the repo flash path and monitor without reset-line
side effects:

```bash
./scripts/usb-monitor.py --seconds 5 --send status
```

## Runtime configuration

Runtime secrets live in NVS only. See
[`docs/BUILD.md`](../../../docs/BUILD.md#runtime-secrets) for the canonical field
and provisioning contract. Already-paired clients may provision JarvisMCP
through `/api/tools/config`; readback is redacted and new token claims remain
fail-closed until a dedicated physical claim surface ships.

Prefer the typed JarvisMCP `/device/v1/invoke` route. Legacy `/act` remains
fixed-template compatibility only.

Do not commit keys, local endpoint URLs, Wi-Fi credentials, LAN addresses, MAC
addresses, or device-specific logs.

## Current Runtime Status

- Direct `jarvis_board` CO5300 primitive plus one overlay compositor owns glass.
- Display software snapshots are available through `/api/display/snapshot.json`,
  `/api/display/snapshot.ppm`, and `/api/display/snapshot.rgb565`.
- CST9217 touch exposes tap/hold/swipe receipts through `/api/touch` and the
  128 KB `/api/logs` ring.
- QMI8658 gestures, AXP2101 battery/PKEY, WakeNet9, Gemini Live, JarvisMCP,
  operator lease, remote canvas, and dual-slot Wi-Fi OTA are active.
- USB/charging edges publish one caption/bloom; charging animates the battery
  rim and Settings carries battery/voltage truth.
- Known face states are `idle`, `listen`, `think`, `speak`, `error`, and
  `sleep`; privacy mute additionally owns the persistent gold outer ring.
- Runtime secrets are NVS-backed and redacted from diagnostics.
- Physical voice, native duplex, hold/swipe reliability, and long-session
  transport counters remain release-shaped hardware checks.

## Display Snapshot Contract

The snapshot API is a software mirror, not CO5300 panel readback.

| Route | Capture source |
|---|---|
| `/api/display/snapshot.json` | owner, source, freshness, and `panel_readback:false` metadata |
| `/api/display/snapshot.ppm` | submitted 466×466 software mirror |
| `/api/display/snapshot.rgb565` | the same submitted mirror as raw RGB565 |

Compare the mirror with the physical panel when claiming visual proof. Neither
pixel route reads the CO5300 panel back.

## Differences From XIAO

| Aspect | XIAO Sense | Waveshare AMOLED |
|---|---|---|
| Release priority | Compatibility | **Primary live target** |
| Flash | 8 MB | **32 MB** |
| Mic | PDM MEMS | ES7210 codec mic path |
| Speaker | External analog amp path | ES8311 codec output |
| Display | Optional add-on | Built-in 466×466 AMOLED |
| Touch | None | CST9217 capacitive |
| Camera | Built-in camera track | None |
| Form factor | Tiny/camera experiments | Aluminum desk companion |

## References

- Official C-board repository/manual: <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C>
- C-board product page: <https://www.waveshare.com/esp32-s3-touch-amoled-1.75c.htm>
- C-board schematic: <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/blob/main/Schematic/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf>
- Official ESP-IDF examples: <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/tree/main/examples/esp-idf>
