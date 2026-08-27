# Waveshare ESP32-S3-Touch-AMOLED-1.75C

The C revision of the primary JarvisNano board: round 466x466 AMOLED,
capacitive touch, ES7210 microphones, ES8311 speaker output, AXP2101 PMIC,
**32 MB flash** (double the original), and 8 MB PSRAM, in a CNC aluminum case.

Deltas vs [`../esp32s3_touch_amoled_1_75/`](../esp32s3_touch_amoled_1_75/):
LCD RST moved 39→1, touch RST moved 40→2, I2S MCLK moved 42→16; the TCA9554
IO expander, PCF85063 RTC, microSD slot, and 8-pin expansion header were
removed. PWR-button state is read via the AXP2101 PKEY IRQ over I2C. Source:
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

## Runtime Config

Runtime secrets live in NVS only. Configure through `/api/config`; readback must
mask sensitive fields.

| Field | Purpose |
|---|---|
| `llm_api_key` / `gemini_api_key` | Gemini Live credential path used by the active build |
| `jarvis_mcp_url` | JarvisMCP `/act` endpoint |
| `jarvis_mcp_key` | JarvisMCP bearer token |
| `pairing_token` | Protected write/control token |

Do not commit keys, local endpoint URLs, Wi-Fi credentials, LAN addresses, MAC
addresses, or device-specific logs.

## Current Runtime Status

- Direct `jarvis_board` CO5300 primitive is the v1 display path.
- Display software snapshots are available through `/api/display/snapshot.json`,
  `/api/display/snapshot.ppm`, and `/api/ui/snapshot.ppm`.
- CST9217 touch diagnostics are exposed through `/api/touch`.
- Known face states are `idle`, `listen`, `think`, `speak`, `error`, and
  `sleep`.
- JarvisMCP config fields are NVS-backed; `/api/tools/status` reports
  configured state without exposing secrets.
- Gemini Live text and voice diagnostics exist; physical voice quality and
  speaker proof remain release-candidate QA items.

## Display Snapshot Contract

The snapshot API is a software mirror, not CO5300 panel readback.

| Route | Capture source |
|---|---|
| `/api/display/snapshot.json` | owner/freshness metadata |
| `/api/display/snapshot.ppm` | active display owner, usually emote |
| `/api/ui/snapshot.ppm` | UI-layer framebuffer |

If the physical panel shows the old reactor face, the emote snapshot should
also show that face. If the cockpit/menu owns the display, capture the UI route.

## Differences From XIAO

| Aspect | XIAO Sense | Waveshare AMOLED |
|---|---|---|
| Release priority | Secondary | Primary v1 |
| Flash | 8 MB | 16 MB |
| Mic | PDM MEMS | ES7210 codec mic path |
| Speaker | External analog amp path | ES8311 codec output |
| Display | Optional add-on | Built-in 466x466 AMOLED |
| Touch | None | CST9217 capacitive |
| Camera | Built-in camera track | None |
| Form factor | Tiny/camera experiments | USB desktop assistant |

## References

- Waveshare wiki: <https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75>
- Schematic PDF: <https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75/ESP32-S3-Touch-AMOLED-1.75.pdf>
- Official BSP: <https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/bsp/esp32_s3_touch_amoled_1_75>
