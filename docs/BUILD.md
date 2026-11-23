# Build, Flash, Test & Verify (v5)

> **Active target:** **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (ESP-IDF 5.5.4 in Docker).
> Last reconciled: **2026-11-23**.

Edit only canonical v5 sources under `main/`, `components/jr_*`, `firmware/components/jarvis_board/`, and `boards/waveshare/esp32s3_touch_amoled_1_75c/`. Do not patch generated `build/` artifacts.

---

## Requirements

- **Docker Desktop** (or Linux Docker engine) — firmware builds run inside `espressif/idf:v5.5.4`; host C test suites auto-fallback to `jarvisnano-hosttests` if no local C compiler is installed.
- **Python 3** (and `pillow` if using `scripts/send-canvas.py` or `scripts/screens.py`).
- **Node.js** (for `scripts/check-dashboard-js.mjs` when editing `main/diagnostics.html`).
- **USB-C data cable** for initial flashing and recovery.

> **Windows Git Bash note:** Git Bash rewrites POSIX container paths like `/w` into `C:/Program Files/Git/w`. Both `scripts/host-tests.sh` and `scripts/build-v5.sh` handle this automatically; when invoking `docker run` manually from Git Bash, always prefix with **`MSYS_NO_PATHCONV=1`** (see [`docs/reference/build-toolchain.md`](reference/build-toolchain.md)).

---

## 1. Host test suites (23 suites, one command)

Run every portable C and Python host suite before building or committing:

```bash
./scripts/host-tests.sh          # runs all 23 suites (auto-uses Docker gcc:14 if no local cc)
./scripts/host-tests.sh -q       # quiet mode: failures and summary only
```

What the 23 suites cover:
- **8 display suites (`components/jr_display/tests`):** procedural HUD, spatial shell, whole-frame pixel hashes, companion rest policy, AA glass primitives, icon ink/clip, board panel transport, quiet vendor retry.
- **9 app/HTTP/power/OTA suites (`main/tests`):** shell input routing, paired charge HTTP handler, display timing JSON, companion orchestration + integration, deep-sleep coordinator, OTA sleep fence, UI cues, gateway gate.
- **2 hardware-driver suites:** `jr_imu` Wake-on-Motion handshake (`components/jr_imu/tests`), `jr_probe` board JSON encoder (`components/jr_probe/tests`).
- **2 core & tools C suites:** `jr_core` + `jr_dsp` + `jr_transport` (`host/` — timers, notes, tool policy, DAC mute, mood, monitors, session), `jr_tools` JS template & allowlist (`components/jr_tools/host`).
- **2 Python CLI/budget suites:** `scripts/test_jarvis_desk.py`, `scripts/test_tools_budget.py`.

Additional fast static checks:

```bash
node scripts/check-dashboard-js.mjs main/diagnostics.html
git diff --check
./scripts/check-secrets.sh
```

---

## 2. Firmware build & smoke validation

```bash
./scripts/build-v5.sh            # pinned to espressif/idf:v5.5.4
./scripts/smoke-build.sh         # verifies 32 MB DIO/80 MHz manifest, offsets, and 4 MB slot limit
```

Output firmware image: `build/jarvisrobot_v5.bin`.
*(Note: after changing Wi-Fi IRAM or core `sdkconfig.defaults` memory options, rebuild with `RECONFIGURE=1 ./scripts/build-v5.sh`.)*

---

## 3. Initial USB flash & serial monitor

Flashing runs on the host (Docker cannot reliably pass through native ESP32-S3 USB-Serial-JTAG):

```bash
./scripts/flash-v5.sh                               # 1.75C DIO, verified, NVS preserved
NO_BUILD=1 ./scripts/flash-v5.sh                    # reuse existing build/jarvisrobot_v5.bin
PORT=/dev/cu.usbmodemXXXX NO_BUILD=1 ./scripts/flash-v5.sh   # macOS (use COMx on Windows)
```

> **Preserving NVS:** Normal flashing keeps Wi-Fi, Gemini key, JarvisMCP config, pairing hash, persisted levels, watch style, and NVS desk notes intact. `ERASE_NVS=1 ./scripts/flash-v5.sh` permanently wipes all device secrets and state — use only for blank-device recovery.

Read serial boot logs without toggling DTR/RTS into ROM download mode:

```bash
python3 scripts/usb-monitor.py --seconds 10
```

---

## 4. Pairing, Command Deck & Wi-Fi OTA

A running device cannot be re-flashed over USB-JTAG while the firmware owns the port. After initial USB setup, use Wi-Fi OTA and the embedded **Command Deck**:

1. Open **`http://<device-ip>/`** in any browser on your LAN for the zero-dependency Command Deck (live 466×466 AMOLED mirror, 8-watch switcher, Pomodoro/Stopwatch studio, NVS Notes, drag-and-drop Canvas push, board probe, crash forensics, live logs).
2. Hold **BOOT** for **1.5–5 s** until the 6-digit pairing code appears, then pair once from the host CLI:

```bash
export JARVIS_DEVICE_HOST='<device-ip>'
python3 scripts/jarvis-desk.py --host "$JARVIS_DEVICE_HOST" pair
```

### Shared OS token store
All host scripts (`jarvis-desk.py`, `jarvisctl.py`, `screens.py`, `send-canvas.py`, `board-probe.py`, `live-device.py`, `gesture-doctor.py`) share one pairing token per device host — never printed or passed in CLI arguments:
- **macOS:** Keychain service `com.ingeniousdigital.jarvisnano.desk`
- **Windows:** Per-user Registry key `HKCU\Software\JarvisNano\Desk`

### Daily Wi-Fi OTA & operator commands

```bash
# Firmware & baked art OTA (dual-slot with probation & automatic rollback)
python3 scripts/jarvisctl.py ota build/jarvisrobot_v5.bin    # ~30 s upload, back in ~5 s
python3 scripts/jarvisctl.py art                             # emote_assets.bin (~150 s, after probation)

# Health, voice performance, crash forensics & board probe
python3 scripts/jarvisctl.py status                          # exits non-zero if deaf or muted
python3 scripts/jarvisctl.py voiceperf                       # voice loop FPS & WakeNet9 timing
python3 scripts/jarvisctl.py crash                           # reset reason + ELF core dump summary
python3 scripts/jarvisctl.py gestures 40                     # physical input receipts from log ring
python3 scripts/board-probe.py                               # live GPIO13 TE, GPIO3, IMU & PMIC probe

# Live display capture & remote 466x466 OLED canvas
python3 scripts/screens.py --out ring.png                    # capture submitted RGB565 frames
python3 scripts/send-canvas.py photo.png --ttl 60            # push image to 466x466 OLED
python3 scripts/send-canvas.py --banner "DEEP FOCUS" --sub "Back at 3:00 PM" --accent cyan

# Codex Desk operator mode
python3 scripts/jarvisctl.py takeover 300
python3 scripts/jarvisctl.py desk present --id demo --kind choice \
  --title 'BUILD GREEN' --body 'SHIP TO DEVICE?' \
  --action ship=SHIP --action later=LATER --ttl 300
python3 scripts/jarvisctl.py normal                          # or double-tap glass to exit
```

---

## 5. Runtime secrets & release gates

The `app` NVS namespace holds `wifi_ssid`, `wifi_password`, `llm_api_key`, `jarvis_mcp_url`, `jarvis_mcp_key`, and the SHA-256 pairing-token hash. **Current NVS is unencrypted** — use dedicated, revocable keys on a private LAN.

Before any public/non-LAN release, close every gate in [`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md):
1. Set `JR_DEV_OPEN_DIAGNOSTICS=0` and require authenticated TLS on the control plane.
2. Enable offline-key signed OTA verification with positive and wrong-key rejection proof.
3. Complete attended NVS/flash encryption with negative physical-dump verification.
4. Generate the exact dependency license bundle via `scripts/generate-third-party-notices.py`.
