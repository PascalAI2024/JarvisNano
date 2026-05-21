# Waveshare ESP32-S3-Touch-AMOLED-1.75 — JarvisNano Board Adaptation

Second JarvisNano host (alongside the Seeed XIAO ESP32-S3 Sense). Round 466×466 AMOLED + capacitive touch + dual-mic far-field audio + AXP2101 PMIC, in a 46 mm circular PCB.

> 🟠 The XIAO has a camera. This board has a screen. They are complementary, not competitive.

## Hardware

| Subsystem | Chip | Bus | JarvisNano use |
|---|---|---|---|
| SoC | ESP32-S3R8 | — | 16 MB flash, 8 MB octal PSRAM (vs XIAO's 8 MB flash) |
| Display | **CO5300** | QSPI on SPI2_HOST | 1.75" AMOLED 466×466, emote engine canvas |
| Touch | **CST9217** | I²C | Capacitive multi-touch, agent confirmation surface |
| Audio DAC | **ES8311** | I²S + I²C | Speaker output via MX1.25 SPK connector |
| Audio ADC | **ES7210** | I²S + I²C | 2 MEMS mics + on-chip AEC reference loopback |
| PMIC | AXP2101 | I²C | Factory defaults sufficient — no power_manager.c required |
| IO expander | TCA9554 | I²C | 8-bit expansion (currently unused by JarvisNano logic) |
| RTC | PCF85063 | I²C | Battery-backed clock |
| IMU | QMI8658 | I²C | 6-axis, currently `init_skip: true` |
| Storage | microSD | SDMMC 1-bit | YAML committed, full integration deferred (see issues) |
| Buttons | PWR + BOOT | side-press | PWR = AXP2101 soft-power, BOOT = GPIO0 strap |

## Pin map (verified against Waveshare schematic PDF)

```
I2C bus (shared):  SDA = 15,  SCL = 14
I2S0 (duplex):     MCLK = 42,  BCLK = 9,  WS = 45,  DOUT = 8,  DSIN = 10
Power amp enable:  GPIO46
Display QSPI:      CS = 12,  PCLK = 38,  D0 = 4,  D1 = 5,  D2 = 6,  D3 = 7,  RST = 39
Touch:             RST = 40,  INT = 11   (on shared I²C)
SD card (SDMMC):   D0 = 3,    CMD = 1,   CLK = 2
USB-C:             ESP32-S3 native USB-Serial-JTAG (no bridge chip)
```

## Build

From the JarvisNano repo root:

```bash
BOARD_VENDOR=waveshare \
BOARD_NAME=esp32s3_touch_amoled_1_75 \
./scripts/bootstrap.sh build
```

(Note: `bootstrap.sh` currently hardcodes the XIAO board. Edit lines 9-10 and 106 to point at this board until the script is parametrized.)

## Flash

```bash
esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 \
  --before default_reset --after hard_reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0      esp-claw/application/edge_agent/build/bootloader/bootloader.bin \
  0x8000   esp-claw/application/edge_agent/build/partition_table/partition-table.bin \
  0xf000   esp-claw/application/edge_agent/build/ota_data_initial.bin \
  0x20000  esp-claw/application/edge_agent/build/edge_agent.bin \
  0x820000 esp-claw/application/edge_agent/build/emote_assets.bin \
  0xb20000 esp-claw/application/edge_agent/build/storage.bin
```

## Onboarding

Wi-Fi can be provisioned without the captive portal — the `app_claw` CLI is enabled on this board and reachable over USB-CDC at 115200 baud:

```
app> wifi --set --ssid <YOUR_SSID> --password <YOUR_PASS> --apply
```

After STA connect, the dashboard is reachable at the LAN IP printed in the log:

```
I (xxxx) wifi_manager: STA connected; provisioning AP stopped for LAN reachability
I (xxxx) app: Wi-Fi STA ready: 192.168.x.x
```

## LLM configuration

POST to `/api/config` on the dashboard:

```jsonc
{
  "llm_backend_type": "anthropic",  // protocol family
  "llm_profile":      "anthropic",  // ENUM — must be one of: anthropic / openai / qwen / qwen_compatible
  "llm_model":        "claude-sonnet-4-5",
  "llm_base_url":     "https://api.minimax.io/anthropic/v1",
  "llm_auth_type":    "bearer",
  "llm_api_key":      "sk-..."
}
```

> ⚠️ `llm_profile` is the **protocol enum**, not the vendor name. Setting it to `"minimax"` will boot-loop on `app_claw_start` abort. Recovery is `esptool erase-region 0x9000 0x6000` to wipe NVS.

After POST, restart the device. `ask_once Hello` should round-trip in ~5 seconds.

## What's different from the XIAO build

| Aspect | XIAO Sense | AMOLED-1.75 |
|---|---|---|
| Flash | 8 MB | 16 MB |
| Mic | 1× PDM via I²S | 2× MEMS via ES7210, hardware AEC |
| Speaker | PAM8002A through I²S-PDM-TX + RC LPF | ES8311 codec → MX1.25 → external 28 mm speaker |
| Display | Optional GC9A01 round add-on (Phase 3) | Built-in 466×466 AMOLED, CO5300 QSPI |
| Touch | None | CST9217 capacitive |
| Camera | OV3660 (2026) / OV2640 (older) | None |
| GPS | No | LC76G module on some SKUs (B variant) |
| App CLI | Disabled (heap) | Enabled |

## Phases shipped

- ✅ **Phase 1** — display + touch + I²C bus + IO expander + PMIC declared (ff6c29f, d419232)
- ✅ **Phase 2** — audio chain (ES8311 + ES7210 + GPIO46 PA enable) (9384b97)
- ✅ **Phase 4** — Wi-Fi onboarding via USB-CDC CLI
- ✅ **Phase 5** — full chat round-trip verified end-to-end on hardware (MiniMax M2.7 via Anthropic-compat endpoint, ~5.5s round-trip)
- 🟠 **Phase 3** — SD card YAML committed; tooling-blocked on `esp-bmgr-assist<0.8` pin (see CHANGELOG)
- 🔜 **Phase 6** — display reactive to agent state (router rules → emote_set_state)
- 🔜 **Voice loop** — mic VAD → STT → LLM → TTS → speaker (multi-day, separate sprint)

See [`../../hardware/enclosure/amoled-1_75/`](../../../hardware/enclosure/amoled-1_75/) for the mascot-bust 3D enclosure concept.

## Memory map

The build emits 6 partitions in the flash layout:

```
0x000000  bootloader.bin              second-stage bootloader
0x008000  partition-table.bin         partition table
0x00f000  ota_data_initial.bin        OTA selector
0x020000  edge_agent.bin              ~2.51 MB application (43% headroom in 4 MB slot)
0x820000  emote_assets.bin            ~2.48 MB packed emote frames (currently the
                                      284×240 swim + offline pack — needs 466×466
                                      JarvisNano-mascot pack)
0xb20000  storage.bin                 4 MB FATFS — Lua scripts, router rules, memory
```

8 MB is unused — reserved for OTA slot B (which is currently empty) and future skill assets.

## References

- Waveshare wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75
- Schematic PDF: https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75/ESP32-S3-Touch-AMOLED-1.75.pdf
- 3D dimensions: https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75/ESP32-S3-Touch-AMOLED-1.75-3D.zip
- Official BSP: https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/bsp/esp32_s3_touch_amoled_1_75
