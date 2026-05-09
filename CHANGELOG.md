# Changelog

All notable user-facing changes should be recorded here.

## Unreleased

### Firmware

- Pinned the generated ESP-Claw checkout to a fixed commit in
  `scripts/bootstrap.sh`, and recorded the resolved commit in build output.
- Added `scripts/smoke-build.sh` for post-build firmware sanity checks
  (binary size, storage image contents, expected log strings).
- Added `scripts/check-patches.sh` to verify that each patch artifact applies
  cleanly to the pinned ESP-Claw commit, or is an explicit bootstrap-managed
  mutation.
- Hardened `scripts/check-secrets.sh`: tightened the source-tree regexes to
  reduce documentation false-positives, added Google API key / JWT /
  Telegram bot token / Anthropic / OpenAI project-key patterns, and added a
  second pass that runs `strings` over `dashboard/firmware/*.bin` so a
  baked-in credential in the published WebSerial blob can no longer slip
  through the scan.
- Added native GPIO21 status LED heartbeat as `patches/0007-native-status-led.patch`,
  starting after `app_claw_start()` so the event router and scheduler get first
  claim on heap; LED allocation failure is non-fatal.
- Disabled the heap-heavy App Claw serial REPL on the XIAO build to avoid
  internal heap exhaustion after Phase-2 services start; USB serial logs still
  work.
- Disabled the long-running Lua status_led auto-start router rule on XIAO so
  Lua heap is preserved at boot; the script remains in FATFS for future
  state-pattern work.
- Added `OPTIONS /api/*` CORS preflight handler
  (`patches/0004-http-phase2-preflight-battery.patch`).
- Added `/api/battery` not-wired JSON stub returning
  `{wired:false, state:"not_wired", source:"stub"}` until ADC wiring lands
  (same patch).
- Added `/api/wifi/scan` route used by the onboarding wizard
  (`patches/0006-http-wifi-scan.patch`).
- Added `/api/health` cheap reachability probe with uptime, free heap, request
  count, and Wi-Fi mode (`patches/0008-http-health-route.patch`).
- Gated the `/api/camera/snapshot` route when the Lua camera module is not
  built (`patches/0005-http-camera-gate-lua-module.patch`).
- Added DVP camera scan-for-JPEG-SOI patch
  (`patches/0003-dvp-cam-scan-for-jpeg-soi.patch`) for OV3660 frame capture
  reliability.
- Disabled Wi-Fi modem sleep
  (`patches/0002-wifi-disable-modem-sleep.patch`) so HTTP responses are not
  delayed by power-save wakeups during the dashboard's polling cadence.

### Dashboard

- Hardened BLE behavior in Codex/in-app browsers: shows Chrome/Edge guidance
  instead of opening the native chooser inside an embedded WebView.
- Switched to the canonical JarvisNano BLE service UUID.
- Camera tile distinguishes blocked vs pending vs failure and disables
  auto-refresh after a gated/pending response, instead of looping a broken
  image.
- Battery tile distinguishes endpoint failure from intentional
  `{wired:false, state:"not_wired"}`.
- Added `scripts/check-dashboard-js.mjs` for repeatable JS syntax checks
  (locally and in CI).
- Added onboarding AP-to-STA Wi-Fi handoff: continue on AP when reachable,
  otherwise verify the STA IP / `esp-claw.local` before advancing.

### Android companion

- BLE scan + connect flow is bounded (10 s scan, 15 s connect) with service
  UUID filtering where supported and name-prefix fallback.
- BLE GATT and scan status codes now surface in `BleClient.State.Failed` so the
  Cockpit BLE tile can report missing-service / missing-permission diagnostics
  instead of silently returning to idle.
- mDNS discovery verifies the resolved host with `GET /api/status` before
  marking it connected, instead of trusting the first
  `_http._tcp.local.` `esp-claw*` result.
- Repository status polling now recovers from a `200 → timeout → 200` sequence:
  the app surfaces failure and returns to connected on the next successful
  poll without an app restart.
- Added unit tests for `DeviceConfig` patch typing (booleans/numbers/strings
  preserved across save), `DeviceClient` HTTP behavior, and JSON
  serialization round-trips.
- Fixed config save type drift: editing a numeric or boolean config field no
  longer round-trips it as a JSON string; unchanged fields remain omitted from
  the patch.
- Restored Android Gradle wrapper for reproducible CLI builds; documented
  `JAVA_HOME` / `ANDROID_HOME` setup.
- Added Android JVM test harness with a real assertion so
  `:app:testDebugUnitTest` cannot pass as an empty source set.

### Docs

- Added `docs/FINISH_PLAN.md` — shortest practical path from current baseline
  to a finished Phase-2 release.
- Documented OV3660 sensor swap on 2026 XIAO Sense batches in
  `docs/CAMERA.md`; UI labels are kept hardware-neutral or `OV3660 / OV2640`.
- Reconciled BLE audio frame size to a single canonical value in
  `docs/PROTOCOL.md` §6.5: 20 ms / 320 samples / 644 bytes per frame
  (4-byte header + 640 PCM bytes), with explicit fragmentation and
  backpressure rules.
- Added explicit HTTP error / feature-gate schema to `docs/PROTOCOL.md` §3.6
  with `feature_gated` (405), `feature_unavailable` (503),
  `feature_not_wired` (200 with typed body), `upstream_timeout` (504), and
  related error codes.
- Reconciled Phase-3 runtime in `docs/PROTOCOL.md` §7 and
  `android/app/src/main/kotlin/.../llm/README.md` to a single canonical stack:
  `llama.cpp` Android NDK build (not `llama.rn`) loading
  `unsloth/gemma-4-E4B-it-GGUF` at `Q4_K_M`.

### Tooling and CI

- Added GitHub Actions CI for repository checks, dashboard JavaScript syntax,
  patch artifact checks, secret-pattern scanning, and Android CLI build/test
  tasks.
- Added open-source contribution, security, support, issue, and PR templates.
- Added Android acceptance checklist covering sync, build, install, BLE
  permission flow, scan, connect, missing-service diagnostic, HTTP
  status/config, chat, and camera failure messaging.

## 0.1.0

- Initial public JarvisNano board adaptation, dashboard, Android companion
  scaffold, documentation, firmware patches, and enclosure concepts.
