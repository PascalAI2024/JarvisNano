# Vendor Reference — ESP32-S3-Touch-AMOLED-1.75

Datasheets, the board schematic, and the **relevant** driver source + examples for our board
([Amazon B0F99V6FGF](https://www.amazon.com/dp/B0F99V6FGF) ·
[Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75)). Kept in-repo and
trimmed to what applies to JarvisNano — the upstream BSP's bundled LVGL/GFX libraries and
unrelated-board demos were dropped (they come via the IDF component manager).

```
vendor/
├── datasheets/          every chip on the board
├── schematic/           the board schematic (resolves the §7 INT-pin questions)
├── drivers/             chip driver SOURCE for QMI8658 / AXP2101 / PCF85063 / CST touch / DRV2605
├── board-examples/      runnable reference sketches for our exact chips (ESP-IDF + Arduino)
└── espressif-docs/      offline HTML snapshots of the cited Espressif docs
```

---

## `datasheets/`
| File | Chip | Used by |
|---|---|---|
| `QMI8658C.pdf` · `QMI8658A_datasheet_RevA.pdf` | QMI8658 IMU | §1 — tap / wake-on-motion / motion-engine registers |
| `AXP2101_SWcharge_V1.0.pdf` | AXP2101 PMIC | §2 — ADC enable (`0x30`), batt-detect (`0x68`), fuel gauge, rails, PWRON |
| `PCF85063A.pdf` | PCF85063 RTC | §4 watch face / alarms / Pomodoro |
| `ES8311_datasheet.pdf` · `ES8311_user_guide.pdf` | ES8311 speaker codec | audio out |
| `ESP32-S3_datasheet.pdf` | ESP32-S3 SoC | sleep currents, GPIO, RTC IO |
| `ESP32-S3_technical_reference_manual.pdf` | ESP32-S3 TRM | §2 sleep/wake, ULP, I²S `HW_VERSION_1` |

> **Not local:** ES7210 (2-mic AEC ADC, §3 wake word) — absent from Waveshare's mirror. Fetch from
> Everest Semiconductor or esp-adf when needed.

## `schematic/`
`ESP32-S3-Touch-AMOLED-1.75-schematic.pdf` — **the** schematic. Confirms the §7 open hardware
questions: **QMI8658 INT2 → `GPIO21`** (RTC-capable, the deep-sleep wake line), AXP IRQ, the shared
I²C nets (SDA `GPIO15` / SCL `GPIO14`).

## `drivers/` — chip driver source (port the register sequences; ours is ESP-IDF, not Arduino)
| Driver | Chip | Maps to |
|---|---|---|
| `SensorLib-src/SensorQMI8658.hpp` | QMI8658 IMU | §1 — tap, wake-on-motion, no-motion config + INT routing |
| `SensorLib-src/SensorPCF85063.hpp` | PCF85063 RTC | §4 — clock + alarm |
| `SensorLib-src/SensorDRV2605.hpp` | DRV2605 haptics | §2.4 — the optional haptic-driver path |
| `SensorLib-src/TouchDrvCSTXXX.hpp` | CST9217 touch family | §4 — gesture decode from the single touch owner |
| `XPowersLib-src/XPowersAXP2101.tpp` | AXP2101 PMIC | §2 — the canonical AXP register map (the §0 addendum verified against this) |

## `board-examples/` — runnable reference for our exact chips
| Example | Shows | Build step it unblocks |
|---|---|---|
| `esp-idf/01_AXP2101/` | **AXP2101 ADC enable + battery read** in ESP-IDF | **Step 1** (the live `mV:16381` ADC-off fix) |
| `arduino/05_LVGL_AXP2101_ADC_Data/` | AXP2101 ADC → UI | Step 1 (Arduino cross-ref) |
| `arduino/04_LVGL_QMI8658_ui/` + SensorLib `QMI8658_WakeOnMotion*` examples | IMU motion → UI / wake-on-motion | **Step 3** (gestures, lift-to-wake) |
| `arduino/03_LVGL_PCF85063_simpleTime/` | RTC clock | Step 4 (watch face) |
| `esp-idf/02_lvgl_demo_v9/` · `04_Immersive_block/` · `arduino/06_LVGL_Widgets/` | LVGL on this panel (200–300 fps) | Step 6 (interactive UI) |
| `esp-idf/05_Spec_Analyzer/` | audio spectrum viz | §4 vibe-mode |
| `arduino/ESP32-S3-LC76G-I2C/` | LC76G GNSS | optional GNSS |

`WAVESHARE_BSP_README.md` / `_LICENSE` — provenance for the extracted code
(from [waveshareteam/ESP32-S3-Touch-AMOLED-1.75](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75)).
The SensorLib drivers are lewisxhe's (also published standalone).

## `espressif-docs/`
| File | Source |
|---|---|
| `esp32s3_sleep_modes.html` | ESP-IDF Sleep Modes |
| `esp-sr_wakenet.html` · `esp-sr_afe.html` | esp-sr WakeNet + Audio Front-End |

Full repos (link only, available via component manager): esp-sr · esp-idf · esp_emote_gfx · LVGL.
