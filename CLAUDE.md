# JarvisRobot — Project Orientation

JarvisRobot is plain ESP32-S3 firmware for a voice + round-display AI
assistant, built with ESP-IDF 5.5.4. The primary target is the **Waveshare
ESP32-S3-Touch-AMOLED-1.75C**: 466×466 CO5300 AMOLED, CST9217 touch,
ES8311/ES7210 audio, AXP2101 PMIC, 32 MB flash, and 8 MB PSRAM. The original
16 MB 1.75 is a hardware/source reference; Seeed XIAO remains experimental.

The live image is rooted at `main/` + `components/jr_*`. Gemini Live runs
through `jr_transport` with 16 kHz uplink and native 24 kHz playback; display
animations use `esp_emote_gfx` EAF assets plus one procedural compositor.

## Build + flash one-liner

Live v5 image (plain ESP-IDF, `main/` + `components/jr_*`):

```bash
./scripts/build-v5.sh
./scripts/flash-v5.sh
```

The leftover esp-claw overlay is `scripts/bootstrap.sh` and is **not** the
v5 image. If you must build that tree:

```bash
ESP_CLAW_REF=$(cd esp-claw && git rev-parse HEAD) \
  BOARD_VENDOR=waveshare \
  BOARD_NAME=esp32s3_touch_amoled_1_75 \
  /abs/path/to/scripts/bootstrap.sh build
```

Flash: DIO mode required for the CO5300 QSPI display (`flash-v5.sh` already
does this).

**⚠️ Edit canonical sources, never the esp-claw copies.** `bootstrap.sh` overwrites
parts of the `esp-claw/` tree on EVERY build: `firmware/components/cap_gemini_live/`
→ `esp-claw/components/claw_capabilities/cap_gemini_live/`, `firmware/emote/` →
the emote runtime, `boards/` → board YAMLs, and it rewrites
`application/edge_agent/partitions_16MB.csv` from a heredoc inside
`apply_emote_partition_resize_patch`. Edits made directly to those esp-claw copies
are silently reverted at the next build. Files that exist ONLY in esp-claw (e.g.
`application/edge_agent/main/main.c`) are modified via idempotent `apply_*_patch`
functions in `bootstrap.sh` — add a patch function there, not a bare edit.

Two version pins are required inside the build container before `idf.py build`:
- `pip install 'idf-component-manager==2.4.10'`
- `pip install 'esp-bmgr-assist==0.5.0'`

See `docs/reference/build-toolchain.md` for the full recipe and gotchas.

## Operator tooling

With the device on LAN (`export JARVIS_DEVICE_HOST=<ip>`), drive it as a tool:
`scripts/jarvisctl.py status|listen|mute|say|screen|canvas|tune|taps|vadlog|reboot`.
`status` exits non-zero when the device is deaf/muted — gate on it before
debugging "no response" as a firmware bug (it is usually a privacy state).
`canvas` pushes any image to the glass (TTL-bounded). Serial is single-owner:
kill any monitor before flashing.

## Reference knowledge base

`docs/reference/` — organized, navigable pages for every subsystem. Start there before touching a new area.

Quick links to the most common gotchas:
- Board manager double-deref → `docs/reference/board-manager.md`
- Build pins and sdkconfig regeneration → `docs/reference/build-toolchain.md`
- `llm_profile` is a protocol enum (not a vendor name) → `docs/reference/llm-config.md`
- Gemini Live API (`thinkingLevel`, tools, current models) → `docs/reference/gemini-live-api-v5.md`
- Display engine: no canvas, no runtime CPU buffers → `docs/reference/display-emote-gfx.md`
- NVS three-file registration pattern → `docs/reference/jarvismcp-bridge.md`

## Capture rule

After any research pass, record findings in `docs/reference/<topic>.md` using `docs/reference/_TEMPLATE.md`. Cite primary sources with file:line or a GitHub URL. Mark unverified links `(not fetch-verified)`.

**Never put in this repo:** secrets, API keys, bearer tokens, internal URLs (any `*.igddev.com` host or equivalent), Wi-Fi credentials, device MAC addresses, device IP addresses, or any other device-specific identifiers. These belong in local session memory or NVS on the device.
