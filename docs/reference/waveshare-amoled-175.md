# Waveshare ESP32-S3-Touch-AMOLED-1.75

**What it is** — The primary target board for JarvisRobot. A round 1.75" AMOLED development board built on the ESP32-S3-WROOM-1, with integrated touch, audio codec chain, PMIC, and optional peripherals. Hardware V1.0.

**How we use it here** — The board provides the display, touch, microphones, speaker DAC, and PSRAM that all JarvisRobot subsystems rely on. Its full description string (from the generated board info) is: "Waveshare ESP32-S3-Touch-AMOLED-1.75 (16 MB flash, 8 MB octal PSRAM, 1.75in 466x466 AMOLED CO5300 QSPI + CST9217 touch, AXP2101 PMIC, TCA9554 IO expander, ES8311+ES7210 audio chain with AEC, dual MEMS mic, QMI8658 IMU, PCF85063 RTC, optional LC76G GNSS)."

Source: `esp-claw/application/edge_agent/components/gen_bmgr_codes/gen_board_info.c:18`.

---

## Hardware summary

| Component | Part | Notes |
|-----------|------|-------|
| MCU | ESP32-S3-WROOM-1 | Xtensa LX7 dual-core, Wi-Fi + BLE |
| Flash | 16 MB | |
| PSRAM | 8 MB octal | Used for audio frame queues, display buffers |
| Display | CO5300 AMOLED, 466x466 | QSPI (4-wire SPI) on SPI2_HOST, 16 bpp RGB565 |
| Touch | CST9217 | Capacitive, I2C `0x5A`, interrupt-driven |
| Speaker DAC | ES8311 | I2C `0x30`, I2S out, external PA (NS4150-class) |
| Mic ADC | ES7210 | I2C `0x80`, 4-channel, hardware AEC |
| PMIC | AXP2101 | I2C `0x34`, controls display/audio rails |
| IO expander | TCA9554 | I2C `0x40` (A0=A1=A2=GND) |
| IMU | QMI8658 | Accelerometer + gyroscope |
| RTC | PCF85063 | |
| GNSS | LC76G | Optional |
| SD card | SDMMC 1-bit | D0=GPIO3, CMD=GPIO1, CLK=GPIO2 |

Source: `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml` (all devices and GPIO assignments).

---

## Findings & gotchas

**[2026-05-21] AXP2101 PMIC init is skipped — factory defaults are correct**

`board_devices.yaml` sets `init_skip: true` for the `axp2101_power_manager` device. The factory AXP2101 power-on defaults already bring up the display and audio rails correctly on this board. Reinitializing it risks disrupting rails that are already live.

Source: `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:26`.

**[2026-05-21] CO5300 is QSPI — flash with `--flash-mode dio`**

The display uses quad SPI (`quad_mode: true` in `board_devices.yaml:110`). When flashing firmware that drives this display, use `--flash-mode dio`. Using the default `qio` mode has caused boot failures. See [gemini-live-api.md](./gemini-live-api.md).

**[2026-05-21] CST9217 touch is interrupt-driven — do not poll it on I2C**

The CST9217 sleeps when idle and will not ACK polled I2C reads. It raises its INT pin on a touch event. Reading it on I2C without a prior INT edge will appear to succeed (I2C ACK) but return stale or garbage data.

Source: `esp-claw/application/edge_agent/boards/waveshare/esp32s3_touch_amoled_1_75/setup_device.c:82` (comment confirming interrupt-driven requirement).

**[2026-05-21] Device-specific identifiers live in local notes only**

The board's MAC address, local IP, Wi-Fi SSID, and dashboard URL are development-machine-specific. They must NOT appear in this repository. They are recorded in local session memory only.

---

## Interrupt & wake routing (from schematic, 2026-06-11)

Read off `docs/reference/vendor/waveshare/ESP32-S3-Touch-AMOLED-1.75-schematic.pdf`
(net names verbatim). **This reshapes the sleep/wake design** — only the IMU INT2 line
is a real, wake-capable GPIO; the AXP IRQ and IMU INT1 are behind the TCA9554 IO expander.

| Net | Routed to | Wake-capable? | Use |
|-----|-----------|---------------|-----|
| **`QMI_INT2`** | **`GPIO21`** (direct ESP32-S3, RTC-capable) | **Yes** — GPIO ISR + EXT0/EXT1 deep-sleep wake | Primary IMU interrupt / wake-on-motion line |
| `QMI_INT1` | `EXIO6` (TCA9554 pin 6) | No — behind I²C expander | Secondary IMU INT; only readable by polling the expander |
| `AXP_IRQ` | `EXIO5` (TCA9554 pin 5) | **No** — behind I²C expander | PMIC IRQ (power-button/low-batt) is NOT a direct deep-sleep wake source |
| `PWRON` | AXP2101 PWRON pin (not an ESP GPIO) | n/a | Physical power button handled internally by the AXP2101 (long-press off/on) |

Consequences for sleep/wake (historical write-up:
`docs/ARCHIVE/UPGRADE_RESEARCH.md` §2.2 / §2.4 / §7):
- **IMU wake-on-motion is viable** — route the QMI8658 motion engine to **INT2 → GPIO21**,
  and use GPIO21 as the EXT0/EXT1 deep-sleep wake source.
- **The AXP power-button cannot directly wake the ESP from deep sleep** (its IRQ is on
  `EXIO5`, behind the TCA9554, which is unpowered/unclocked in deep sleep). Power-button
  wake would rely on the AXP2101's own PWRON sequencing (it can re-enable rails / reset the
  SoC), not an ESP wake source. Plan deep-sleep wake around **motion (GPIO21) + RTC timer**.
- QMI8658 I²C address strap not legible in the text layer — confirm at runtime via WHO_AM_I
  (reg `0x00` → `0x05`); the driver should probe both `0x6B` (default) and `0x6A`.

## Primary sources

| Source | Notes |
|--------|-------|
| `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml` | All device declarations, pin assignments, I2C addresses. Ground truth. |
| `docs/reference/vendor/waveshare/ESP32-S3-Touch-AMOLED-1.75-schematic.pdf` | Full board schematic — interrupt/wake net routing (QMI_INT1/2, AXP_IRQ, PWRON), rail mapping. |
| `boards/waveshare/esp32s3_touch_amoled_1_75/board_info.yaml` | Board-level metadata. |
| `boards/waveshare/esp32s3_touch_amoled_1_75/setup_device.c` | Factory entry functions, CO5300 init commands, CST9217 interrupt setup. |
| [Waveshare BSP (official)](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/bsp/esp32_s3_touch_amoled_1_75/) | Official BSP; diff against our `board_devices.yaml` when debugging pin issues. |
| [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) | Datasheet links, demo sketches, hardware overview. |
| [Component registry](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75) | BSP version (`^1`), idf_component.yml pin. |

---

## Open questions

- What is the actual PA rail voltage on the NS4150-class amplifier? (Xiaozhi references 5.0 V; our board's PA supply may differ. Affects ES8311 `hw_gain.pa_voltage`.)
- QMI8658 IMU connectivity: **resolved** — present, INT2→GPIO21, INT1→EXIO6 (see Interrupt & wake routing above). Still not declared in `board_devices.yaml`; address strap to confirm at runtime via WHO_AM_I.
- AXP2101 rail→peripheral mapping (which DCDC/LDO feeds the AMOLED panel vs. the ES8311/ES7210 codecs) — needed before cutting rails for sleep (§2.3). Read from the schematic's power tree.

---

## See also

- [audio-es8311-es7210.md](./audio-es8311-es7210.md) — ES8311/ES7210 codec chain details.
- [display-emote-gfx.md](./display-emote-gfx.md) — CO5300 display and the `esp_emote_gfx` engine.
- [sdmmc-storage.md](./sdmmc-storage.md) — SD card wiring and mount procedure.
- [board-manager.md](./board-manager.md) — how `board_devices.yaml` becomes runtime handles.
- [build-toolchain.md](./build-toolchain.md) — `BOARD_VENDOR=waveshare BOARD_NAME=esp32s3_touch_amoled_1_75`.
