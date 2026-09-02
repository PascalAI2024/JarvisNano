# Build, flash, and verify v5

The active target is the **Waveshare ESP32-S3-Touch-AMOLED-1.75C**. The old
ESP-Claw bootstrap build remains for history; it is not the firmware flashed
to the live 32 MB round device.

## Requirements

- Docker Desktop or a Linux Docker host
- Python 3
- CMake 3.16+ for portable host suites (or run them in the IDF container)
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

Then validate the generated flash manifest and every required artifact:

```bash
./scripts/smoke-build.sh
```

The smoke check enforces the 1.75C DIO/32 MB/80 MHz contract, exact offsets,
non-empty bootloader/app/OTA/model/emote artifacts, and the 4 MB app-slot limit.

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
the write, and performs a watchdog reset. Normal flashing preserves NVS.
`ERASE_NVS=1` permanently removes Wi-Fi credentials, the Gemini key, JarvisMCP
configuration, pairing-token hash, persisted levels, and device-local facts.
Use it only for deliberate blank-device recovery with a safe reprovisioning
plan—never during ordinary iteration.

## OTA release gate

The dual-slot updater and rollback path are operational, but a production OTA
must not ship on pairing-token/project-name checks alone. Before enabling remote
updates outside a trusted development LAN:

1. create an ESP-IDF app-signing key offline and keep it outside the repository;
2. enable signed-app verification for OTA and reject unsigned/wrong-key images
   before `esp_ota_set_boot_partition`;
3. protect the control plane with authenticated TLS or nonce/body-bound request
   authentication—never transmit a reusable bearer over plaintext;
4. run the preflight matrix (power, network, inactive slot, contiguous internal
   memory) and probation/rollback path—the first health confirmation is eligible
   at 45 seconds and the hard rollback deadline is 120 seconds;
5. verify a trusted update, wrong-key rejection, forced probation failure, and
   visible rollback evidence on the physical C-board;
6. enable and negatively verify attended NVS/flash encryption so a physical
   dump cannot recover Wi-Fi or cloud credentials;
7. generate the exact dependency/license bundle with
   `scripts/generate-third-party-notices.py` and attach it to the release.

Secure Boot v2, anti-rollback, and flash/NVS encryption involve keys or eFuses
and are intentionally separate, physically attended release procedures. Never
enable or burn them from ordinary OTA automation.

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

Current NVS is **not encrypted**. A physical flash read or shared NVS dump
exposes Wi-Fi, Gemini, and JarvisMCP credentials; the pairing token itself is
stored only as a SHA-256 hash. Use dedicated revocable cloud credentials.
Attended NVS/flash encryption provisioning and negative recovery tests are
public-release gates.

Paired `/api/cockpit` and `/api/gemini/live` never return raw credentials or
private endpoint values, but they can contain operational and transcript
content. Treat their responses as private diagnostics.

The preferred JarvisMCP URL must be HTTPS. Never provision a general
`MCP_API_KEYS` desktop credential onto the device.

Host tools share one pairing identity:

- `jarvis-desk.py` — first pairing plus Desk/operator surfaces;
- `jarvisctl.py` — concise paired status, input, level, display, OTA, and
  recovery commands;
- `live-device.py` — deeper reports, audio/display evidence, and voice cycles.

All load the same host-bound Keychain token without printing or placing it in
argv.

## Live verification

v5 does not currently advertise mDNS. Supply the real IP explicitly:

```bash
export JARVIS_DEVICE_HOST='<device-ip>'
# On a blank device, hold BOOT for 1.5–5 seconds until PAIRING OPEN appears.
python3 scripts/jarvis-desk.py --host "$JARVIS_DEVICE_HOST" pair
python3 scripts/live-device.py status
python3 scripts/live-device.py report
python3 scripts/jarvisctl.py gestures 40
python3 scripts/jarvisctl.py screen
python3 scripts/live-device.py gemini-cycle \
  --text 'Use the current_time tool and tell me the result.' \
  --report
# After the 32 MB dual-slot table is installed:
python3 scripts/jarvisctl.py ota
python3 scripts/jarvisctl.py art      # the faces: build/emote_assets.bin, ~150 s, after probation

# Codex display/interaction tool
python3 scripts/jarvisctl.py takeover 300
python3 scripts/jarvisctl.py desk present --id demo --kind choice \
  --title 'CODEX LINKED' --body 'DISPLAY TOOL ONLINE' \
  --action continue=CONTINUE --action normal=NORMAL --ttl 300
python3 scripts/jarvisctl.py desk events --after 0
python3 scripts/jarvisctl.py mode
# Double-tap the panel to exit, or:
python3 scripts/jarvisctl.py normal
```

Only coarse hardware counters are open. Content-bearing diagnostics and secured
controls use the host-bound Keychain token and never place it in argv or output.

The strongest automated checks are:

- `/api/cockpit`: Wi-Fi connected, Gemini WebSocket open, voice Listening,
  on-device tools ready/configured, display ready, nonzero FPS, zero flush
  errors;
- `/api/gemini/live`: bounded queues, voice-task heartbeat, tool call/result
  counters;
- `/api/display/snapshot.*`: software proof of pixels submitted to the panel;
- `/api/audio/taps` and `/api/audio/tap.wav`: electrical/software-path proof;
- `/api/diag/panel-touch`: control-path and human touch challenge.
- `/api/logs` plus `jarvisctl gestures`: 128 KB operational/error history and
  physical input-to-action receipts;
- `/api/diag/tasks`: contiguous internal-memory and task-stack budgets;
- `/api/ota/upload`: idle-slot update path; invalid uploads must not alter
  privacy or operator state.

A framebuffer mirror does not prove the physical AMOLED lit. A WAV buffer does
not prove the speaker was audible. Record those as external human/camera
observations instead of allowing a pleasant HTTP 200 to develop delusions of
grandeur.

## Local gates

```bash
cmake -S host -B build-host
cmake --build build-host --parallel
ctest --test-dir build-host --output-on-failure

cmake -S components/jr_tools/host -B build-tools-host
cmake --build build-tools-host --parallel
ctest --test-dir build-tools-host --output-on-failure

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
