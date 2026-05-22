# JarvisRobot — Project Orientation

JarvisRobot is ESP32-S3 firmware for a voice + display AI assistant. It runs on the `esp-claw` application framework (Espressif) with ESP-IDF v5.5.1. The primary target board is the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (round 466×466 CO5300 AMOLED, CST9217 touch, ES8311 DAC + ES7210 ADC, AXP2101 PMIC, 16 MB flash / 8 MB PSRAM). A second board (Seeed XIAO ESP32-S3) is also supported.

The firmware integrates **Google Gemini Live API** (`cap_gemini_live`) for real-time voice: 16 kHz PCM in / 24 kHz PCM out over a WebSocket. Display animations run on `esp_emote_gfx` with flash-baked AAF/EAF assets.

## Build + flash one-liner

```bash
ESP_CLAW_REF=$(cd esp-claw && git rev-parse HEAD) \
  BOARD_VENDOR=waveshare \
  BOARD_NAME=esp32s3_touch_amoled_1_75 \
  /abs/path/to/scripts/bootstrap.sh build
```

Flash: `idf.py flash --flash-mode dio` (DIO mode required for the CO5300 QSPI display).

Two version pins are required inside the build container before `idf.py build`:
- `pip install 'idf-component-manager==2.4.10'`
- `pip install 'esp-bmgr-assist==0.5.0'`

See `docs/reference/build-toolchain.md` for the full recipe and gotchas.

## Reference knowledge base

`docs/reference/` — organized, navigable pages for every subsystem. Start there before touching a new area.

Quick links to the most common gotchas:
- Board manager double-deref → `docs/reference/board-manager.md`
- Build pins and sdkconfig regeneration → `docs/reference/build-toolchain.md`
- `llm_profile` is a protocol enum (not a vendor name) → `docs/reference/llm-config.md`
- Gemini Live API (`thinkingLevel`, tool calls, flash mode) → `docs/reference/gemini-live-api.md`
- Display engine: no canvas, no runtime CPU buffers → `docs/reference/display-emote-gfx.md`
- NVS three-file registration pattern → `docs/reference/jarvismcp-bridge.md`

## Capture rule

After any research pass, record findings in `docs/reference/<topic>.md` using `docs/reference/_TEMPLATE.md`. Cite primary sources with file:line or a GitHub URL. Mark unverified links `(not fetch-verified)`.

**Never put in this repo:** secrets, API keys, bearer tokens, internal URLs (any `*.igddev.com` host or equivalent), Wi-Fi credentials, device MAC addresses, device IP addresses, or any other device-specific identifiers. These belong in local session memory or NVS on the device.
