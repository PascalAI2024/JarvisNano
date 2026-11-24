# Waveshare ESP32-S3-Touch-AMOLED-1.75C: capability map vs firmware use

**What it is** — A cited inventory of everything the Waveshare 1.75C carries, set against what the compiled image (`main/` + `components/jr_*`) touches. Written 2026-11-10 from the vendor's C schematic (downloaded and read page by page), the vendor wiki and product page, the CO5300/QMI8658/AXP2101 datasheets, the vendor BSP, and a source search of the firmware. Pins are the C revision's; do not mix in the original 1.75 map.

**How we use it here** — A decision aid for picking showpiece features from unused hardware. Companion pages: [board-175c.md](./board-175c.md) (revision delta), [imu-interrupt-routing.md](./imu-interrupt-routing.md), [power-modes.md](./power-modes.md), [display-emote-gfx.md](./display-emote-gfx.md), [audio-es8311-es7210.md](./audio-es8311-es7210.md). Where this page disagrees with them, this page says why.

---

## What "C" means (resolved)

Source: product pages, fetched 2026-11-10. [1.75C product](https://www.waveshare.com/esp32-s3-touch-amoled-1.75c.htm), [1.75C docs](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C), [1.75 docs](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75).

| | plain 1.75 | 1.75-G | 1.75C |
|---|---|---|---|
| Flash | 16 MB | 16 MB | **32 MB** |
| RTC | PCF85063 (battery-backed via AXP2101) | same | **none** (spec table shows "-") |
| TF/microSD | yes | yes | **none** (spec table "-") |
| IO expander | TCA9554 | same | **none** |
| GNSS | no | **LC76G + IPEX antenna holder** | none (page silent; schematic has no GNSS part) |
| Expansion | 8-pin 2.54 mm header (3 GPIO + UART) | same | **bare GPIO/test pads** |
| Case | plastic/bare | plastic/bare | **one-piece anodised CNC aluminium**, lanyard holes, lanyard included |
| Battery | optional | optional | SKU 33691 includes one, SKU 33692 does not; MX1.25 header either way |
| Shared | ESP32-S3R8, 8 MB octal PSRAM, CO5300 466x466 QSPI, CST9217, QMI8658, AXP2101, ES7210 + ES8311, dual mics, PWR + BOOT | | |

"-G" is the GPS build of the original 1.75, not a revision of the C ([search snippet, not fetch-verified](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75)). The product page lists the panel at 700 cd/m², 100000:1 (typical, no HBM figure).

---

## Answers to the five doubts

1. **RTC on the 1.75C: no.** The C schematic text has zero hits for `PCF85063`; the pad table lists no 0x51 device on the shared I2C pair; the product page spec table shows RTC as "-"; the vendor C repo ships no RTC example and the C resources page lists no RTC datasheet. **`"rtc": false` is not evidence**: `jr_rtc_start()` is compiled out on the C (`main/main.c:4350-4357`), so `jr_rtc_present()` (`main/http_routes.c:2808`) is false whether or not a chip exists. The AXP2101 does expose `VBACKUP` (input) and `RTCLDO1/2` (30 mA output) pins (AXP2101 datasheet pin table); the schematic wires them to a `VBAT2`/`VCC-RTC` net, but no coin-cell pad was identified (see unverified). Timekeeping is SNTP (`main/main.c:4407`) plus retained OS time; deep-sleep drift rides the S3 RTC timer.
2. **Haptic driver or motor: none.** The C schematic has no DRV2605, haptic, motor or vibration string and no free GPIO assigned to one; the product page, wiki and vendor repo never mention haptics. The repo's `SensorDRV2605.hpp` is part of lewisxhe's generic SensorLib bundle (it sits beside BQ27220, BMA423 and others the board does not have), so it proves nothing about this board. A motor would have to hang off the exposed pads (GPIO17/18/39-42).
3. **QSPI clock: 40 MHz, set by a library macro, not by us.** `jarvis_board.c:123` builds the panel IO with `CO5300_PANEL_IO_QSPI_CONFIG(...)` and never overrides `pclk_hz`; the macro in `managed_components/espressif__esp_lcd_co5300/include/esp_lcd_co5300.h:136` (component 2.1.0) hardcodes `40 * 1000 * 1000`. The vendor C BSP uses the same macro unmodified (`esp32_s3_touch_amoled_1_75c.c:396`, [raw](https://raw.githubusercontent.com/waveshareteam/Waveshare-ESP32-components/master/bsp/esp32_s3_touch_amoled_1_75c/esp32_s3_touch_amoled_1_75c.c)), so the vendor demo is also 40 MHz. Derived: one full RGB565 frame is 434,312 bytes; at 4 bits x 40 MHz that is about 21.7 ms of pure wire time, so about 46 fps full-frame ceiling before overhead. The CO5300 datasheet text gives no maximum QSPI SCL (see unverified).
4. **Touch points: hardware reports more than one, nobody reads them.** The Waveshare ESP-IDF driver we ship hardcodes `CST9217_MAX_TOUCH_POINTS 1` (`managed_components/waveshare__esp_lcd_touch_cst9217/esp_lcd_touch_cst9217.c:32`), and our reader asks for one point (`components/jr_hal/src/input_touch.c:270`). lewisxhe's SensorLib driver for the same chip reads `CST92XX_MAX_FINGER_NUM = 2` (`docs/reference/vendor/drivers/SensorLib-src/REG/CST9xxConstants.h:61`) and also decodes a palm bit and a gesture nibble from byte 4 (`touch/TouchDrvCST92xx.cpp:149-160`, gesture left undecoded). No CST9217 datasheet could be found, so two is the only evidenced count and the true maximum is unverified.
5. **Gyroscope: off everywhere.** The only IMU code is `components/jr_imu/src/jr_imu.c`: CTRL7 = 0x01 (accel enable only, `:47-48`) and the comment at `:271` "Gyro stays off (power)"; `CTRL3` (0x04, gyro config) is never written, and no gyro string appears in `main/`, `jr_core` or `jr_ports`.

---

## Pin map (C schematic, page 1 GPIO table)

Source: [C schematic PDF](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/raw/main/hardware/schematic/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf), table at top right of page 1, read at 520 dpi; cross-checked against `boards/waveshare/esp32s3_touch_amoled_1_75c/*.yaml` and the vendor BSP header.

| GPIO | Net | Used by firmware |
|---|---|---|
| 0 | BOOT key | yes, `main/main.c:289` |
| 1 | LCD_RESET | yes, `jarvis_board.c` (`JARVIS_LCD_RST`) |
| 2 | TP_RESET | yes, `board_devices.yaml:88` |
| **3** | **SYS_OUT (PWR-key mirror, see below)** | **no** |
| 4/5/6/7 | QSPI SIO0..3 | yes |
| 8 / 10 | I2S DSDIN (to ES8311... see note) / I2S ASDOUT | yes (`board_peripherals.yaml:36-37`) |
| 9 / 45 / 16 | I2S SCLK / LRCK / MCLK | yes |
| 11 | TP_INT | yes (driver + ext1 wake, `main/power.c:243-250`) |
| 12 | LCD_CS | yes |
| **13** | **LCD_TE** | **no pin read** (panel TE is switched on by `0x35`, `jarvis_board.c:47`) |
| 14 / 15 | I2C SCL / SDA (touch, codecs, IMU, PMIC) | yes |
| **17, 18, 39, 40, 41, 42** | "OUT" column: bare pads | **no** |
| 19 / 20 | USB D- / D+ | yes (USB-Serial-JTAG) |
| 21 | QMI_INT1 (net alias at U3) | yes, ext0 wake (`main/power.c:30`) |
| 38 | QSPI SCL | yes |
| 43 / 44 | U0TXD / U0RXD pads | no |
| 46 | PA_CTRL | yes, `components/jr_audio/src/jr_audio.c:43` |

Note on GPIO8/10 labels: the table's ES8311 column reads `I2S_DSDIN` on 8 and `I2S_ASDOUT` on 10, which is the codec's point of view; our YAML names them `dout: 8, din: 10` from the S3's point of view. Same wires.

I2C devices on SDA15/SCL14: AXP2101 0x34, ES8311 0x30 (8-bit form), ES7210 0x80 (8-bit form), CST9217 0x5A, QMI8658 0x6B (`board_devices.yaml`, schematic annotation "0X6B").

---

## Capability matrix

"Wired" = reaches the S3 on the C (schematic). "Used" = compiled firmware touches it today.

| Peripheral | Capability | Wired on C? | Used by firmware? | Source |
|---|---|---|---|---|
| **Panel CO5300** | 466x466 RGB565 over QSPI, 40 MHz | yes | yes, 40 MHz | `jarvis_board.c:111-123`; `esp_lcd_co5300.h:136` |
| | Brightness DBV `0x51`, 8 bit | yes | yes, percent x 255/100 | `jarvis_board.c:81-89`; driver `esp_lcd_co5300_spi.c:379-385` |
| | High-brightness mode: `0x63` HBM level, `0x66` HBM_EN | yes (command set) | **half**: `0x63=0xFF` written at init (`jarvis_board.c:50`), `0x66` never sent, so HBM is off | CO5300 datasheet commands 63h/66h ([PDF](https://files.waveshare.com/wiki/common/Co5300_Datasheet.pdf)) |
| | Idle mode `0x38/0x39`: 16.7M, 4096 or 8 colours | yes | **no** (deliberately: 8-colour idle erases the dim palette, `docs/HARDWARE.md:101-103`) | datasheet features list; IDMON command |
| | Partial display `0x12` with window `0x30/0x31` | yes | **no** | datasheet commands PARON/PTLAR/PTLAR_H |
| | Sleep `0x10/0x11`, display on/off | yes | yes, `jarvis_board.c:61-70` | |
| | TE output, V-blank mode via `0x35`, scan-line mode via `0x44` | **yes: LCD_TE to GPIO13** | panel TE enabled, **GPIO13 never read** | schematic table; datasheet section 5.3; `jarvis_board.c:47` |
| | Native refresh | 60 Hz VFR in the datasheet timing table (454-line spec; the 466 panel is not separately specified) | measured fps is render-limited (`docs/reference/display-emote-gfx.md`) | datasheet timing table |
| **Touch CST9217** | I2C 0x5A, INT GPIO11, RST GPIO2 | yes | yes | `board_devices.yaml:66-91` |
| | Multi-point | hardware reports at least 2 (SensorLib) | **1 point read** | see doubt 4 |
| | Palm bit, gesture nibble in report byte 4 | in the report | **ignored** (our swipes are classified in software, `docs/HARDWARE.md:47-49`) | `TouchDrvCST92xx.cpp:149-160` |
| | Sleep command / INT wake | INT wired | INT used as ext1 deep-sleep wake | `main/power.c:243-250`; SensorLib `sleep()` |
| **IMU QMI8658C** | Accel +-2/4/8/16 g; gyro +-16 to +-2048 dps; ODR to 8 kHz; 1536-byte FIFO; AttitudeEngine; 16-bit temp sensor; Wake-on-Motion | yes | accel +-8 g at 125 Hz polled; WoM on INT1; **gyro, FIFO, AttitudeEngine, temperature unused** | QMI8658C datasheet ([PDF](https://files.waveshare.com/wiki/common/QMI8658C.pdf), local `docs/reference/vendor/datasheets/QMI8658C.pdf` sections 2.1, 3.8, 9); `jr_imu.c:44-58` |
| | Tap / any-motion / no-motion / pedometer engines | **in the QMI8658A datasheet only**; the C datasheet has none of these words | no | local `QMI8658A_datasheet_RevA.pdf` section 10 vs `QMI8658C.pdf` |
| | INT1 to GPIO21; INT2 not connected | INT1 only | yes | schematic U3 (net alias `QMI_INT1 -- GPIO21`); [imu-interrupt-routing.md](./imu-interrupt-routing.md) |
| **Mics (2x MEMS) + ES7210** | MIC1, MIC2 = mics; MIC3 = ES8311 echo-reference tap; MIC4 not connected | yes | yes: 3 channels, AEC on the MIC3 ref | `board_devices.yaml:40-60`; `docs/reference/aec-barge-in.md` |
| **ES8311 + NS4150B** | DAC to class-D amp, PA enable GPIO46, MX1.25 speaker connector | yes | yes | `board_peripherals.yaml:45-52`; `jr_audio.c:43`; NS4150B is a 3 W-class mono amp (2.8 W at 10% THD, 5 V, 4 ohm per [datasheet copy](https://micros.com.pl/mediaserver/ULNS4150b_NSIWAY_0001.pdf), search snippet, not fetch-verified) |
| | ES8311 on-chip mic ADC | unknown (the codec's mic inputs were not traced; mics go to the ES7210) | off | `board_devices.yaml:24` |
| **AXP2101 PMIC** | 14-bit ADC: VBAT, VBUS, VSYS, TS, die temperature | yes | **VBAT only** (`jr_power.c:176-178`) | AXP2101 datasheet Table 6-7 (local PDF) |
| | E-Gauge percentage reg `0xA4` | yes | yes (`jr_power.c:222`) | |
| | Charger: fast charge 0-1400 mA in 25 mA steps (reg `0x62`), precharge 0-200 mA, input limit 100 mA to 2 A class, TS thermistor suspend, safety timer | yes | **read-only**: `charge.c` samples `0x61-0x65` for diagnostics, never writes a setpoint | datasheet charge tables; `components/jr_power/src/charge.c:6-12` |
| | PWRON key short/long IRQ latches (`0x49`), soft power-off (`0x10` bit0), on-hold (`0x27`) | yes | yes (`jr_power.c:27-46`) | |
| | IRQ pin | **not wired to the S3** (the net has only the AXP pin and its pull-up) | polled over I2C every 500 ms | schematic netlist; `jr_power.c:35` |
| | Battery current | **no ADC channel exists** | none (correct: "no ammeter") | Table 6-7 |
| **RTC** | PCF85063 | **absent on C** | `jr_rtc` compiled out | doubt 1 |
| **Storage** | microSD | **absent on C** (GPIO1/2 became panel/touch resets) | no | `board-175c.md` |
| **Buttons** | BOOT to GPIO0 | yes | yes (controls shade, pairing, download mode) | `main/main.c:289` |
| | PWR key, two paths: AXP PWRON, and a MOSFET mirror **to GPIO3** | yes | PMIC path only; **GPIO3 mirror unused** | schematic KEYS block (below) |
| **USB** | native USB-Serial-JTAG on GPIO19/20, Type-C, VBUS into AXP2101 | yes | yes | |
| **Radios** | 2.4 GHz Wi-Fi, BLE 5, onboard antenna | yes | Wi-Fi yes; BLE off (coexistence starves audio, `audio-es8311-es7210.md`) | wiki |
| **GNSS / haptics / LEDs** | none on the C | n/a | n/a | doubt 2; the AXP2101 has a `CHGLED` pin, whether an LED hangs on it was not traced |
| **Expansion pads** | GPIO17, 18, 39, 40, 41, 42, U0TXD/U0RXD, plus the I2C pair and 3V3/VBUS/GND test pads | yes (pads only) | **no** | schematic table "OUT" column and page-1 test-point list |

---

## Unused hardware worth using (ranked by showpiece value against effort)

1. **TE-synchronised flush (LCD_TE on GPIO13).** Value: high for a watch face, since tear-free sweeps and a measured vsync are the difference between "smooth" and "smooth-looking". Effort: low, one input interrupt feeding the `gfx_render` strip scheduler (the only task allowed to touch the panel, `CLAUDE.md`). Proof it is real: GPIO13 row reads `LCD_TE` in the schematic table, and `0x35` is already sent at init. Risk: the 12-row DMA strip pipeline already overlaps strips (N14.x), so a TE gate can cost throughput if placed per strip rather than per frame; the panel's 60 Hz refresh vs our 24 fps target needs a phase policy.
2. **Wrist raise with the gyro (plus the 16-bit temperature sensor).** Value: high. Accel-only "lift" is crude; gyro adds a true rotation signal for raise-to-speak, wrist-flick dismiss, and a spirit-level or compass-free tilt face, and the AttitudeEngine can give quaternion orientation. Effort: low-medium: write CTRL3, set CTRL7 bit1, widen the burst read at `jr_imu.c:294`. Proof: `QMI8658C.pdf` gyro full-scale table and `3.8` temperature section; our CTRL7 enables accel only. Risk: gyro on removes the low-power accel modes (SensorLib comment, `SensorQMI8658.hpp:73-76`) and costs sampling current; WoM is mutually exclusive with sampling already ([imu-interrupt-routing.md](./imu-interrupt-routing.md)). Z axis is mounted inverted on the C (negated at `imu_read_axes()`), and the gyro Z needs the same flip.
3. **Second touch point (pinch / two-finger hold).** Value: medium-high: pinch-to-zoom on the Weather ring, two-finger hold as a privacy gesture that cannot happen by accident. Effort: low: one constant and a 15-byte read in the driver (SensorLib reads 2 fingers with the same layout), then `esp_lcd_touch_get_data(..., 2)`. Proof: `CST92XX_MAX_FINGER_NUM = 2` in SensorLib's `CST9xxConstants.h:61`. Risk: the true maximum and how the chip reports finger 2 on this panel are unverified; the driver ships single-point because Waveshare capped it, perhaps for a reason. Test on hardware before relying on it.
4. **PWR key on GPIO3 as a deep-sleep wake source.** Value: high for dependability. Today the only wakes are lift, touch and a timer, and `docs/HARDWARE.md:20` says "the PWR key cannot wake the S3 from deep sleep". The schematic says it can. Effort: low: ext1 on GPIO3 (RTC-capable, GPIO0-21 are) beside the existing touch ext1 line, polarity high-when-pressed (derived below). Proof: KEYS block, `PWRON -> R12 1k -> T1 BSS138 gate`, `T1 drain = SYS_OUT` pulled to 3V3 by R9 10k, and `SYS_OUT = GPIO3` in the table. Risk: GPIO3 is an S3 strapping pin (JTAG source select); the MOSFET holds it low at rest, which must be checked against the reset strapping rules on a real unit. Polarity is my reading (PWRON idles high through the AXP's internal 100k pull-up, so T1 conducts and SYS_OUT sits low; pressing the key pulls the gate low and SYS_OUT rises). It is not yet seen on a scope.
5. **HBM for outdoor / sunlight mode.** Value: medium: "tilt to sun" brightness boost for the clock face, the one thing an AMOLED watch is judged on outdoors. Effort: low: send `0x66` bit HBM_EN with the already-loaded `0x63` level, gated to a user setting and a thermal/battery guard. Proof: datasheet lists WRHBMDISBV 63h and HBM enable 66h; `0x63=0xFF` is already written. Risk: nits at HBM are not documented anywhere I could open; power draw and AMOLED ageing rise sharply; do it time-boxed, never on battery saver.
6. **Idle-mode always-on clock (IDMON + partial window).** Value: medium for a nightstand or watch-peek face with the CPU asleep. Effort: medium-high: needs a design that survives 8-colour or 4096-colour output (`HARDWARE.md:101-103` rejected it for Nightstand because the dim palette would vanish) and a partial window that keeps the pixel count low. Proof: datasheet feature list "Idle mode: 16.7M-colors, 4096-colors, 8-colors" and PARON/PTLAR commands. Risk: the datasheet pages I read do not state the current saved; measure before promising battery life.
7. **AXP2101 telemetry beyond VBAT: die temperature, VBUS, VSYS, TS thermistor, and a charge-current setpoint.** Value: medium: a visible "charging at N mA", a thermal throttle for HBM and Wi-Fi, charge-limit-80% as a user feature. Effort: low-medium: enable ADC channels in `0x30` and write `0x62`. Risk: charge current is changed on a battery of unknown capacity (SKU-dependent); never raise it above the cell's C/2 without knowing the pack; the TS pin only protects if the battery pack carries an NTC (unverified).
8. **Expansion pads (GPIO17/18/39-42, UART0, shared I2C).** Value: high for makers, low for the product story: a status LED strip, a vibration motor driver (there is none on board), or an I2C sensor bolted to the aluminium case's back. Effort: medium and needs hardware. Risk: pad-only, no connector; GPIO39-42 are the JTAG-capable group (only an issue if the JTAG fuses change); GPIO41/42 are free again because MCLK moved to 16.
9. **IMU FIFO + data-ready routed to INT1.** Value: low-medium: lets the poll task sleep, saving CPU wake-ups and enabling gesture recognition at 100+ Hz without the 10 ms timer. Effort: medium. Risk: INT1 is also the WoM line, so modes switch; the schematic's page-1 table puts `QMI_INT1` on the GPIO38 row, contradicting the net alias (see unverified).

Not worth chasing: tap/pedometer/any-motion engines (documented only for the QMI8658A, absent from the C datasheet), GNSS, SD, RTC, haptics (not on this board).

---

## Findings & gotchas

**[2026-11-10] The PWR key has a second path that reaches the S3.**
KEYS block on schematic page 1: the PWRON net (to AXP2101 pin 30) also drives the gate of a BSS138 through a 1k resistor; its drain, `SYS_OUT`, is pulled up by 10k and appears in the pin table as GPIO3. Our docs say the PWR key cannot wake the chip; that was true for the original 1.75, where the equivalent signal (`AXP_IRQ`) sat behind the TCA9554. On the C there is no expander and a direct wire. Unverified on hardware.

**[2026-11-10] `"rtc": false` was never a measurement.**
It is a compile-time branch (`main/main.c:4350-4357`). Read the schematic or the vendor spec table, not the status JSON, to decide what the board has.

**[2026-11-10] LCD_TE reaches GPIO13, and the panel is already emitting TE.**
Both the pin table and the net alias show it. The CO5300 init sequence (`jarvis_board.c:47`) sends `0x35 0x00`, V-blank-only TE mode. Nothing listens.

**[2026-11-10] Our docs cite SensorLib engines that belong to a different IMU part.**
`docs/reference/vendor/INDEX.md` and `power-modes.md` talk about tap / no-motion / any-motion engines "from the QMI8658". Those words appear 87, 73 and 55 times in the QMI8658A datasheet and essentially never in the QMI8658C one, and the board is a C (schematic table header). The one engine we use, Wake-on-Motion, is in both.

**[2026-11-10] The vendor C BSP header still says RTC and IMU in comments.**
`esp32_s3_touch_amoled_1_75c.h` carries `BSP_CAPS_IMU 0` and a copied comment that the I2C bus is "shared by the onboard touch, audio, IMU, RTC, and power-management devices". The comment is stale from the original board; the code sets `BSP_CAPS_SDCARD 0` and has no RTC code.

---

## Unverified

- **CO5300 maximum QSPI clock** and the idle-mode power saving: the datasheet text I extracted has no SCL frequency figure (it may be in an image) and no idle-mode current. An 80 MHz QSPI bus would also need the S3's GPIO-matrix limit checked (from memory, not fetch-verified: GPIO-matrix SPI is limited to about 40 MHz, and our pins 4-7, 12, 38 are not the FSPI IO-MUX group).
- **HBM luminance** and whether `0x66` works on this panel's init sequence. The product page's 700 cd/m² has no stated mode.
- **QMI_INT1 routing**: the net alias at U3 says GPIO21, but the page-1 table also prints `QMI_INT1` on the GPIO38 row (that row's first column is `QSPI_SCL`). Prior work chose GPIO21 and the table looks like a column slip; probing GPIO21 while forcing a WoM event settles it. `INT2` unconnected is inferred from the net appearing once ([imu-interrupt-routing.md](./imu-interrupt-routing.md)).
- **Which QMI8658 variant is fitted** (A or C): the schematic table says C; the `REVISION` register (`0x01`, defined at `jr_imu.c:36`) or a CTRL9 tap-config attempt would tell. `power-modes.md` already records that the CmdDone bit never sets on this part.
- **CST9217 specifics**: no datasheet found, so true maximum touch points, report rate, built-in gesture codes and wake-on-touch are not documented from a primary source. The INT line is wired and used as an ext1 wake.
- **PWR key GPIO3 polarity and strapping behaviour**: schematic reading only.
- **Mic spacing and gain range** (ES7210): schematic shows two MEMS mics; their separation was not measured and the ES7210 datasheet ([Waveshare copy](https://files.waveshare.com/wiki/common/ES7210_DS.pdf)) was not opened (search snippet only, not fetch-verified).
- **PA supply rail** feeding the NS4150B and thus real speaker power, and whether a speaker ships with the unit (product page says "speaker pads, no speaker included").
- **AXP2101 backup/RTC pins**: whether `VBACKUP` ends at a usable coin-cell pad, and what the charger's power-on default current is on this board (read `0x62` live).
- **TS pin**: whether the supplied battery carries a thermistor.
- **AXP2101 power-key edge bits and other IRQ sources** were taken from the firmware and XPowersLib, not re-read in the datasheet.
- **LC76G / any GNSS on the C**: none found in the schematic text or the vendor pages; the "-G" comparison came from a search snippet, not fetch-verified.

---

## Primary sources

| Source | Notes |
|--------|-------|
| [C schematic PDF](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/raw/main/hardware/schematic/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf) | Fetched and read 2026-11-10: GPIO table, KEYS block, U3 IMU, AXP2101 block, netlist text. Three pages. |
| [1.75C docs](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C) | Fetched. Chip list, SKUs, buttons, case. |
| [1.75C product page](https://www.waveshare.com/esp32-s3-touch-amoled-1.75c.htm) | Fetched (first 100,000 of 124,684 characters read). Spec table with RTC "-", TF "-", 700 cd/m². |
| [1.75 docs](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) | Fetched. Original-board features (PCF85063, TCA9554, TF, GPS variant). |
| [1.75C resources page](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C/Resources-And-Documents) | Fetched. Datasheet list has no RTC. |
| [Vendor C repo](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C) | Fetched. Examples tree via the GitHub API: 5 ESP-IDF, 7 Arduino, no RTC/SD/haptic example. |
| [Vendor C BSP](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/bsp/esp32_s3_touch_amoled_1_75c) | Fetched header and .c via raw URLs: pins, init commands, 40 MHz macro use. |
| [CO5300 datasheet V0.01](https://files.waveshare.com/wiki/common/Co5300_Datasheet.pdf) | Downloaded and text-searched: features, TE section 5.3, commands 12h/30h/31h/35h/38h/39h/44h/51h/53h/63h/66h, 60 Hz VFR. |
| `docs/reference/vendor/datasheets/QMI8658C.pdf`, `QMI8658A_datasheet_RevA.pdf`, `AXP2101_SWcharge_V1.0.pdf` | Local; ranges, ODRs, ADC table, charge registers. |
| `docs/reference/vendor/drivers/SensorLib-src/touch/TouchDrvCST92xx.cpp`, `REG/CST9xxConstants.h` | Two-finger read, gesture nibble, palm bit. |
| `boards/waveshare/esp32s3_touch_amoled_1_75c/board_devices.yaml`, `board_peripherals.yaml` | Our pin and device declarations. |
| `firmware/components/jarvis_board/src/jarvis_board.c` (built via `components/jr_display/CMakeLists.txt:7,17`) | Panel init commands, brightness, sleep, QSPI config use. |
| `components/jr_imu/src/jr_imu.c`, `components/jr_power/src/{jr_power,charge}.c`, `components/jr_hal/src/input_touch.c`, `main/power.c`, `main/main.c` | What the firmware touches. |
| [NS4150B datasheet copy](https://micros.com.pl/mediaserver/ULNS4150b_NSIWAY_0001.pdf) | Search snippet only, **(not fetch-verified)**. |

---

## Open questions

- Does GPIO13 show TE pulses at 60 Hz on a live unit, and what is the phase against our strip scheduler?
- Does the PWR key raise GPIO3 on press, and does the unit still boot with the key held?
- Can the CST9217 deliver two fingers on this panel with the Waveshare driver's offset layout?
- What does the AXP2101 default charge current read on a fresh C, and what cell capacity ships in SKU 33691?

---

## How to probe

`GET /api/diag/board` (code: `components/jr_probe`, handler `board_diag_handler` in `main/http_routes.c`) answers the open questions above on a live unit. It sits behind the same pairing gate as the other `GET` diagnostics. Run it through `scripts/board-probe.py`, which reuses `jarvisctl.py` for host and token and never prints either.

```bash
export JARVIS_DEVICE_HOST=<ip>
scripts/board-probe.py                  # about 1 s: TE, GPIO3 level, IMU registers + temperature, PMIC
scripts/board-probe.py --gyro           # adds a gyro pass, on for about 300 ms
scripts/board-probe.py --watch pwr      # holds 3 s: press and release the PWR key meanwhile
scripts/board-probe.py --watch touch    # holds 4 s: put two fingers on the glass meanwhile
scripts/board-probe.py --json           # the raw object
```

Every probe is bounded and reports `ok:false` plus a reason rather than failing the response. Only one watch window runs per request, so the worst case is just under 5 s. Nothing sends a panel command, drives GPIO3 or GPIO13, or writes a charge, voltage or rail register.

| Key | What a reading settles |
|---|---|
| `te` | `hz` near 60 with `period_us_min`/`max` near 16 600: GPIO13 carries the panel's TE and the TE-synchronised flush is real; the spread says how steady it is. `edges: 0`: the panel was off at that moment (re-run with the screen lit), TE is not reaching the pin, or `0x35` did not take. A different steady rate tells us the panel's actual refresh. |
| `pwr_key` | With `--watch pwr`, `changed:true` and `high_ms` roughly the press length means GPIO3 follows the key. `level_max:1` after a press with `level` 0 at rest confirms the polarity (high when pressed) and clears the wake-source idea in item 4 of the ranking. `changed:false` through a real press means the mirror does not reach the pin as drawn. A press shorter than the 10 ms poll can be missed: hold for half a second. |
| `imu` | `who_am_i` 5 and `revision`: the revision byte tells A from C (see Unverified). `ctrl1/2/3/7` as found; `ctrl3` 0 and `ctrl7` 1 is the shipped accel-only state. `temp_c` is the die temperature (expect a few degrees above ambient). With `--gyro`: `mean_dps` near 0 on a still desk and the spread in `min`/`max` give the zero-rate offset and noise; `restored:true` shows CTRL3/CTRL7 were put back. `gyro.why` explains a refusal (Wake-on-Motion armed, or sampler stopped). Gyro Z is flipped on the C like accel Z. |
| `touch` | With `--watch touch`: `max_fingers` 2 with `seen:true` and `ack` 171 (0xAB) proves the CST9217 reports a second finger on this panel with the SensorLib layout; the `raw` hex is the 15-byte report at that moment (bytes 0-4 finger 0, byte 5 low seven bits the count, byte 6 the 0xAB ack, bytes 7-11 finger 1). `max_fingers` stuck at 1 with two fingers down means the chip or its firmware reports one point and the pinch idea is dead. `reads: 0` means nobody touched the glass during the window. |
| `pmic` | `die_c`, `vbus_mv`, `vsys_mv`, `vbat_mv`, `ts_mv` are the telemetry the ranking item 7 wants. `ts_mv` near 0 or pinned at a rail suggests no pack thermistor. `charge_cc_ma` is the factory fast-charge setpoint (register 0x62, read only). `adc_before`/`adc_during`/`adc_after`: the ADC enable register as found, with the extra channels on, and after the write-back (`adc_restored:true`). |
| `spi` | The panel's QSPI clock as the firmware sets it: a constant, 40 MHz from the library macro (doubt 3). It measures nothing. |

Choices and guesses worth knowing:

- **ADC channels are switched back off.** The probe enables the TS, VBUS, VSYS and die-temperature channels for about 100 ms and writes `0x30` back to what it found, so the probe leaves the PMIC as it was. If the extra channels turn out to be worth keeping, that is a separate change.
- **TE count** is an interrupt counter on rising edges over 500 ms, not a PCNT unit: smaller and nothing to free afterwards. The pin is left an input with its interrupt off and no pull set.
- **GPIO3** is read with the input buffer enabled and nothing else, so its strapping pull is untouched.
- **Touch** is read by the touch worker itself, right after its own read, with the driver's own register sequence but 15 bytes instead of 10. The driver never writes the 0xAB ack back (SensorLib does), so whether the chip re-serves a report on a second read is a guess; if `reads` is high and `seen` is false, that is the first suspect.
- **Die temperature** uses the XPowersLib formula, 22 + (7274 - raw) / 20 degrees C (not fetch-verified against the AXP2101 datasheet). VBUS, VSYS and TS counts follow the same 14-bit layout as VBAT, TS at 0.5 mV per count.
- **IMU gyro range** is set to +-512 dps (64 LSB per dps) at 112 Hz for the probe only.
- The probe reads the IMU and PMIC from the httpd task, relying on the shared I2C master to serialise against the sampler tasks.

---

## See also

- [board-175c.md](./board-175c.md): the revision delta this page extends.
- [imu-interrupt-routing.md](./imu-interrupt-routing.md): INT1 vs INT2, WoM recipe.
- [power-modes.md](./power-modes.md): measured sleep behaviour.
- [display-emote-gfx.md](./display-emote-gfx.md): the strip compositor any TE work has to fit.
