# Build, flash, and verify v5

The active target is the Waveshare ESP32-S3 Touch AMOLED 1.75. The old
ESP-Claw bootstrap build remains in the repository for history; it is not the
source of the v5 firmware flashed to the round panel.

## Requirements

- Docker Desktop or a Linux Docker host
- Python 3
- USB-C data cable
- the Waveshare board

The firmware build is pinned to ESP-IDF 5.5.4 in Docker. Flashing runs on the
host because macOS Docker cannot reliably pass through native
ESP32-S3 USB-Serial-JTAG.

## Build

```bash
./scripts/build-v5.sh
```

The script:

1. runs the pinned `espressif/idf:v5.5.4` image;
2. generates the Waveshare board-manager configuration;
3. synchronises the v5 sdkconfig contract;
4. builds the plain ESP-IDF composition rooted at `main/main.c`;
5. verifies managed patches and sdkconfig did not drift.

Expected application output:

```text
build/jarvisrobot_v5.bin
```

Edit the canonical v5 sources in `main/`, `components/`, and `boards/`.
`firmware/` is the leftover esp-claw overlay and is not part of the v5
image. Do not patch generated build output.

## Flash

```bash
./scripts/flash-v5.sh
```

Useful forms:

```bash
# Reuse an existing build and preserve NVS.
NO_BUILD=1 ./scripts/flash-v5.sh

# Select a specific native USB port.
PORT=/dev/cu.usbmodemXXXX NO_BUILD=1 ./scripts/flash-v5.sh

# Deliberately erase only NVS before flashing.
ERASE_NVS=1 ./scripts/flash-v5.sh
```

The script refuses non-DIO images, uses the generated flash manifest, verifies
the write, and performs a watchdog reset. Normal flashing preserves NVS,
including Wi-Fi, Gemini, JarvisMCP, and pairing state. Do not set `ERASE_NVS=1`
during ordinary iteration.

## Serial diagnostics

The monitor avoids DTR/RTS toggles that can strand the S3 in ROM download
mode:

```bash
python3 scripts/usb-monitor.py --seconds 10
```

Or specify the port:

```bash
python3 scripts/usb-monitor.py --port /dev/cu.usbmodemXXXX --seconds 10
```

A good boot includes display readiness, Wi-Fi connection, Gemini endpoint
configuration, on-device tool worker readiness, HTTP startup, and a stable
Listening phase. Logs may report whether a secret is configured; they must
never print its value.

## Runtime secrets

The `app` NVS namespace contains:

| Field | Purpose |
| --- | --- |
| `wifi_ssid` / `wifi_password` | Wi-Fi station credentials |
| `llm_api_key` | Gemini Live key |
| `jarvis_mcp_url` | Prefer JarvisMCP `/device/v1/invoke`; legacy `/act` is compatibility-only |
| `jarvis_mcp_key` | Dedicated, revocable JarvisNano device credential |
| pairing-token hash | Authenticates Agent Link writes, Brain Link in/out, and tools configuration |

`GET /api/cockpit` and `GET /api/gemini/live` expose only configured/readiness
booleans and counters. They never return secret or endpoint values.

The preferred JarvisMCP URL must be HTTPS. Never provision a general
`MCP_API_KEYS` desktop credential onto the device.

## Live verification

v5 does not currently advertise mDNS. Supply the real IP explicitly:

```bash
export JARVIS_DEVICE_HOST='<device-ip>'
python3 scripts/live-device.py status
python3 scripts/live-device.py report
python3 scripts/live-device.py screen
# For secured control tests: long-press the panel for 1.2 seconds, then pair
# once. The token goes to macOS Keychain and is loaded automatically below.
python3 scripts/jarvis-desk.py --host "$JARVIS_DEVICE_HOST" pair
python3 scripts/live-device.py gemini-cycle \
  --text 'Use the current_time tool and tell me the result.' \
  --report
```

Read-only diagnostics work without pairing. Secured write/control commands
send the host-bound Keychain token and never place it in argv or output.

The strongest automated checks are:

- `/api/cockpit`: Wi-Fi connected, Gemini WebSocket open, voice Listening,
  on-device tools ready/configured, display ready, nonzero FPS, zero flush
  errors;
- `/api/gemini/live`: bounded queues, voice-task heartbeat, tool call/result
  counters;
- `/api/display/snapshot.*`: software proof of pixels submitted to the panel;
- `/api/audio/taps` and `/api/audio/tap.wav`: electrical/software-path proof;
- `/api/diag/panel-touch`: control-path and human touch challenge.

A framebuffer mirror does not prove the physical AMOLED lit. A WAV buffer does
not prove the speaker was audible. Record those as external human/camera
observations instead of allowing a pleasant HTTP 200 to develop delusions of
grandeur.

## Local gates

```bash
cmake -S components/jr_tools/host -B /tmp/jarvisnano-jr-tools
cmake --build /tmp/jarvisnano-jr-tools --parallel
ctest --test-dir /tmp/jarvisnano-jr-tools --output-on-failure

python3 -m unittest scripts/test_jarvis_desk.py
node scripts/check-dashboard-js.mjs main/diagnostics.html
git diff --check
./scripts/check-secrets.sh
```

For Android compatibility checks:

```bash
cd android
./gradlew :app:testDebugUnitTest :app:assembleDebug \
  :app:compileDebugAndroidTestKotlin
```
