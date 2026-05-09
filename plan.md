# Open work that requires hardware or human decisions

This branch (`claude/evaluate-nullclaw-project-zSDEv`) closed the docs and
Android items that could be done from a clean Linux dev box. This file
records what is **still open** because it needs physical hardware, a paired
phone, or a product decision the maintainer should make.

It is the residual of `docs/FINISH_PLAN.md` and `docs/PHASE2_TASKS.md` after
this branch's work. Cross-reference against those files before starting any
item.

## Was NullClaw a better fit?

No. NullClaw (https://github.com/nullclaw/nullclaw) is a Zig static binary
that targets Linux SBCs and needs libc — it cannot run on the XIAO ESP32-S3
Sense (no OS, no libc). JarvisNano targets the bare microcontroller; the
two solve different problems and could complement each other (NullClaw on a
Pi, JarvisNano as the edge mic/speaker), but NullClaw is **not** a
replacement here. No migration is recommended.

## What this branch did finish locally

- Reconciled BLE audio frame size to a single canonical value across
  `docs/PROTOCOL.md`, `android/.../ble/README.md`, and `android/.../llm/README.md`:
  20 ms / 320 samples / 644 B per frame (4 B header + 640 B PCM).
- Added §3.6 HTTP error / feature-gate schema to `docs/PROTOCOL.md`.
- Added §6.5 BLE audio framing (sequence, fragmentation, backpressure).
- Reconciled Phase-3 runtime naming: `llama.cpp` Android NDK + Gemma 4 E4B
  `Q4_K_M` (was `q4_0` over `llama.rn` in PROTOCOL.md).
- Wrote a populated `CHANGELOG.md` "Unreleased" section covering the recent
  firmware patches (status LED, REPL disable, Wi-Fi scan, battery stub,
  camera gate, health route), dashboard, Android, docs, and tooling work.
- Fixed Android Settings type-drift bug: editing a numeric or boolean config
  field no longer round-trips it as a JSON string. Refactored
  `buildConfigPatch` / `displayValue` / `mergeConfig` into
  `DeviceConfig.kt` so they are unit-testable.
- Added 25 Android JVM unit tests across `DeviceConfigTest`,
  `DeviceClientTest` (MockWebServer-backed), and `SerializationTest`.
  Verified locally with `./gradlew :app:testDebugUnitTest` and
  `./gradlew :app:assembleDebug` (Android SDK Platform 35, Build-Tools 34,
  JDK 17). All 26 tests pass; APK builds clean.

## Cannot finish locally — needs the physical XIAO board

Most of the remaining Phase-2 work depends on flashing and observing the
device. Listed in roughly the order a maintainer with the board should
tackle them.

### LAN HTTP reachability matrix (PHASE2_TASKS.md Wave 2)

- Run `scripts/http-matrix.sh` for AP path (`192.168.4.1`), STA IP, and
  `esp-claw.local` against `/api/health`, `/api/status`, `/api/config`,
  `/api/battery`, `/api/audio/level`, `/api/wifi/scan`.
- Confirm normal API responses (not just `OPTIONS /api/*`) include
  `Access-Control-Allow-Origin: *`.
- Test from a second client on the same Wi-Fi to rule out macOS
  routing/firewall behavior.
- Capture serial logs during each failed curl so the HTTPD/socket failure
  mode is recorded, not just timed out.
- Confirm dashboard polling does not starve the HTTP server when camera
  auto-refresh is on.

### Onboarding (Wave 3)

- Re-test onboarding step 2 against real `/api/wifi/scan` once LAN is
  stable.
- Re-test onboarding Wi-Fi save → restart → reconnect → LLM round-trip on a
  freshly erased storage image.
- Verify onboarding "Done" succeeds after AP-to-STA handoff (`/api/config`,
  `/ws/webim`, `/api/restart` on the chosen post-save host).
- Capture demo-mode + alive-mode + onboarding screenshots for the dashboard.

### BLE GATT firmware (Wave 4) — the biggest open item

- Enable the ESP32-S3 BLE stack (NimBLE, per FINISH_PLAN.md) without
  regressing Wi-Fi, PSRAM, or heap.
- Advertise service `1ec185cd-4bc7-5797-a8b1-0f5b66c59757` with a stable
  device name prefix.
- Implement the four canonical characteristics: `audio_in`, `audio_out`,
  `state`, `control` (UUIDs in `docs/PROTOCOL.md` §6.1).
- Honor the `state/control` first / `audio_*` later order from
  `docs/FINISH_PLAN.md` §3.
- Negotiate MTU 247, document fallback, validate Android 12+ behavior.
- Add a smoke test: scan, connect, discover service, discover four
  characteristics, subscribe to `state`, write a harmless control command.
- Update `docs/PROTOCOL.md`, BLE README, and dashboard BLE copy only after
  the firmware service behavior is verified on hardware. The protocol doc
  and the Android client now declare the framing this work must implement
  (PROTOCOL.md §6.5).

### Speaker + TTS (Wave 6)

- Wire PAM8002A combo module: GPIO4 → 270 Ω → 100 nF low-pass → PAM8002A
  IN+, common ground, VCC from USB 5 V rail.
- Prove raw tone / FATFS PCM clip playback through I²S0 PDM TX.
- Decide firmware ownership of I²S0 TX so audio output cannot conflict with
  mic-level polling or future BLE audio.
- Add TTS provider config (provider, model/voice, sample rate, format).
- Stream synthesized PCM without blocking the agent loop or starving HTTP;
  add cancellation/interrupt when the user speaks or presses stop.
- Add a wake-word or low-power VAD path; tune end-of-utterance to
  first-audio-frame latency toward 800 ms.
- Document speaker wiring, volume limits, current draw, distortion notes.

### Battery + ADC (Wave 7)

- Solder 503450 LiPo to BAT+; verify charge/discharge/USB switchover.
- Pick a free ADC1 GPIO (no display/camera collision); document the
  resistor divider.
- Replace `/api/battery` not-wired stub with calibrated voltage / percent /
  charging / source / state.
- Cross-check against a multimeter at full / nominal / low.
- Add low-battery state to GPIO21 LED pattern and BLE `state` payload.
- Add dashboard + Android low-battery warning UI.
- Measure idle / Wi-Fi / BLE / mic / speaker / camera current draw.

### Camera (Wave 8)

- Decide whether camera ships in Phase 2 or is deferred to Phase 4. Current
  HTTP route is gated; the OV3660 `esp_cam_sensor` path is blocked by SCCB
  format-set failures.
- Prototype the legacy `espressif/esp32-camera` driver path
  (`docs/CAMERA.md`).
- Resolve dependency conflicts between `esp32-camera`, `esp_cam_sensor`,
  `esp_video`, and `lua_module_camera_service`.
- Validate `curl -m 10 -o /tmp/snap.jpg http://<device>/api/camera/snapshot`,
  the dashboard capture, and Android single-snap + live mode.
- Document final sensor detection, resolution, JPEG quality, memory use,
  and fallback. Upstream or document the local NO-SOI patch if the V4L2
  path remains relevant.

### Build hygiene (Wave 1)

- Confirm a clean checkout can run `./scripts/bootstrap.sh`,
  `./scripts/bootstrap.sh build`, and the flash flow without relying on
  generated local files. (Needs a fresh box and the board.)
- Decide whether generated `esp-claw/` edits should remain ignored-only or
  be reproducible solely from `scripts/bootstrap.sh` + `patches/`.
- Verify the normal app flash path preserves Wi-Fi/LLM config while an
  explicit storage flash wipes and reprovisions.

## Cannot finish locally — needs an Android phone

### Android BLE on real hardware (Wave 5)

- Verify install + BLE permission flow on a physical Android phone.
- Implement GATT characteristic discovery in the UI once the firmware
  service lands.
- Implement a serialized GATT operation queue (MTU request, service
  validation, characteristic validation, reads, writes, descriptor writes).
- Enable CCCD notifications for `audio_in` and `state`.
- Implement `state` JSON parsing and render mode/battery/error
  diagnostics.
- Implement `control` writes for `start_listen`, `stop`, `restart`, and
  transport selection.
- Implement `audio_out` writes (20 ms PCM16 frames per `docs/PROTOCOL.md`
  §6.5) and `audio_in` notify ingestion.
- Add reconnect behavior and clear failure messages for permission denied,
  scan timeout, GATT error, missing service.
- Add permission matrix tests for API 30, 31, and 35.
- Request `RECORD_AUDIO` only when Phase-3 / local audio mode is enabled.
- Verify mDNS discovery, manual host override, `/api/status`, `/api/config`,
  `/ws/webim`, and `/api/camera/snapshot` failure handling on a physical
  phone.

### Android session UX (Wave 5)

- Replace placeholder "New session" behavior with a real session id
  rollover and local message clear.
- Add first-boot provisioning, or clearly hand off to browser onboarding
  until mobile onboarding exists.

## Can be done locally but was deferred — no hardware needed, just time

- Add unit tests for `DeviceRepository` polling recovery and discovery
  state. Requires extracting a small `Discovery` interface so the
  repository is testable without the Android `Context`-bound `MdnsDiscovery`
  class. Not done here because it's a refactor that touches `App.kt` and
  every screen that constructs the repository.
- Add Android instrumented smoke tests for permissions and navigation
  (Wave 5). Compiles today via `:app:compileDebugAndroidTestKotlin`; needs
  device or emulator to actually run.
- Visual QA screenshots for the dashboard at desktop and mobile widths.

## Open product decisions (need maintainer input, not code)

- Camera in Phase 2 or deferred to Phase 4? (Wave 8 first item.)
- Does restart / config write need a local pairing token before public
  release? Short-LAN-token model is sketched in
  `docs/FINISH_PLAN.md` §7 but not committed.
- Reference Phase-3 LLM is now declared canonical (Gemma 4 E4B `Q4_K_M`
  via llama.cpp NDK). Confirm this is what should ship; if not, update
  `docs/PROTOCOL.md` §7 and `android/.../llm/README.md` together — they
  are now expected to stay in lock-step.

## Pre-merge cleanup audit

Run before this branch was put up for merge to `main`. Findings recorded so
the maintainer can review what was kept vs flagged.

### Confirmed clean

- No editor / OS junk (`.DS_Store`, `*.swp`, `*~`, `*.bak`, `*.orig`,
  `Thumbs.db`).
- No agent / IDE state directories (`.claude/`, `.cursor/`, `.idea/`,
  `.vscode/`, `.private/`, `node_modules/`).
- No env / credential files (`.env*`, `*.local`, `secrets.yaml`, `*.key`,
  `*.token`, `credentials*`, SSH keys).
- All four CI repo-checks pass locally:
  `bash -n scripts/*.sh`, `node scripts/check-dashboard-js.mjs`,
  `./scripts/check-patches.sh`, `./scripts/check-secrets.sh`.
- `./gradlew :app:assembleDebug :app:testDebugUnitTest
  :app:compileDebugAndroidTestKotlin :app:lintDebug` is green
  (JDK 17, Android SDK Platform 35, Build-Tools 34.0.0).

### Image / binary scrub

- All 41 tracked PNG / SVG files were inspected with `exiftool` for
  identifying metadata. No GPS, no Author, no Artist, no original file
  paths, no machine hostnames.
- Mascot / brand / hero / concept / wordmark PNGs (mascot.png, logo.png,
  wordmark.png, hero*.png, igd-rebrand/*, early-concepts/*) carry **C2PA
  Content Credentials** identifying them as AI-generated by Flux.1. This
  is honest provenance disclosure — recommended to keep, not strip.
- Dashboard screenshots (`images/dashboard/*.png`) and technical SVG
  drawings (`hardware/enclosure/*/technical-drawing.svg`) carry no
  metadata at all. Clean.
- `dashboard/firmware/jarvis-xiao-esp32s3-sense.bin` (8 MB ESP32-S3
  firmware) was scanned with `strings` against the secret-pattern set.
  Only false positives matched: mbedTLS PEM-format label strings
  (`-----BEGIN ... PRIVATE KEY-----` markers without a body) and config
  field labels (`wifi_password`, `llm_api_key`, etc., used in the
  firmware's own CLI help and JSON keys). No real credentials, no JWTs,
  no Telegram bot tokens, no Google / OpenAI / Anthropic / GitHub keys.
- Hardened `scripts/check-secrets.sh` so future builds run the same
  binary scan automatically — see the `firmware_patterns` list.

### Flagged for maintainer review (not changed)

- `dashboard/_seed3.html`, `dashboard/_seed4.html`,
  `dashboard/_setstep.html` — onboarding-step seeding helpers that
  preload `localStorage` keys and bounce to `onboard.html`. Look like
  intentional QA helpers from the rebrand commit. Leaving in place; if
  they are pure scratch and should not ship in the public repo, delete
  them in a follow-up. They contain no secrets — only the documented
  AP IP `192.168.4.1`.
- The ESP-Claw firmware embeds default API endpoints for several Chinese
  LLM/IM providers (Tavily, DashScope, Feishu, QQ Bot Gateway). These
  are upstream defaults, not project-specific config, so they should
  stay. Worth mentioning in the README's privacy/network section if the
  maintainer wants to be explicit about outbound endpoints the device
  may reach when the user configures those providers.

## Verification done on this branch

```
JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
ANDROID_HOME=/tmp/android-sdk      (Platform 35, Build-Tools 34.0.0)

cd android
./gradlew :app:testDebugUnitTest    # 26 tests, 0 failures
./gradlew :app:assembleDebug        # APK builds
```

Test breakdown:

| File | Tests |
| ---- | ----- |
| `DeviceConfigTest` | 11 (type preservation, sensitivity, group, patch + merge) |
| `DeviceClientTest` | 8 (MockWebServer; status/config/webim/restart/snapshot) |
| `SerializationTest` | 6 (ignore-unknown-keys, defaults) |
| `UnitTestScaffold` | 1 (existing) |
