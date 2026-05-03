# JarvisNano Master Wave Task List

This is the working execution board for moving JarvisNano from the current
stable Phase-2 baseline to a finished voice/BLE/battery build, then into the
Phase-3 display/privacy-mode work. Add to this file whenever a new blocker,
test gap, or follow-up appears.

## Current Hardware Baseline

- Board: Seeed XIAO ESP32-S3 Sense on USB as `/dev/cu.usbmodem1101`.
- Latest flashed build: boots Wi-Fi, FATFS, router rules, scheduler, Lua, MCP,
  Web IM, publishes `startup/boot_completed`, returns from `app_main()`, and
  starts the native GPIO21 heartbeat.
- Fixed boot blockers: App Claw serial REPL is disabled for the XIAO build to
  avoid internal heap exhaustion; the legacy Lua LED auto-start rule is disabled
  because the long-running Lua LED job exhausted Lua heap.
- Current open hardware blocker: Mac LAN curls to `192.168.50.80` still time
  out despite ARP resolution, including after the 2026-05-03 fresh flash, so
  final HTTP response/header verification is not complete.

## Status Legend

- `[x]` Done in repo and verified locally.
- `[~]` Partially done, blocked, or needs hardware validation.
- `[ ]` Not done yet.

## Active Agent Waves

| Agent wave | Scope | Status | Output expected |
| --- | --- | --- | --- |
| Firmware audit | runtime, patches, boot, HTTP, BLE, audio, battery, camera | complete | added reachability, pinning, flash/storage, audio, battery, camera, patch-debt tasks |
| Dashboard audit | admin, onboarding, browser BLE guard, polling, visual QA | complete | added browser guard, AP-to-STA onboarding, device tile, demo/QA tasks |
| Android audit | build, BLE client, HTTP client, screens, permissions | complete | added build tests, repository recovery, GATT queue, permissions, local LLM, docs tasks |
| Research finish pass | upstream ESP-IDF, Android, camera, CI efficiency | complete | added `docs/FINISH_PLAN.md`, CI, dashboard syntax check, patch check, and secret scan |

## Wave 0: Stabilized Baseline

- `[x]` Dashboard admin page guards Web Bluetooth inside the Codex in-app browser so the BLE button does not close Codex.
- `[x]` Dashboard admin page uses the canonical JarvisNano BLE service UUID.
- `[x]` Dashboard timer, status, camera preview URL, device-provided text handling, and alive preview are hardened.
- `[x]` Android app can scan, request BLE permissions, connect, disconnect, and diagnose a missing JarvisNano GATT service.
- `[x]` On-board GPIO21 user LED has a native boot flourish and soft alive heartbeat.
- `[x]` Firmware bootstrap applies `OPTIONS /api/*` CORS preflight support.
- `[x]` Firmware bootstrap applies `/api/battery` not-wired JSON stub.
- `[x]` Firmware bootstrap applies `/api/wifi/scan` for onboarding AP discovery.
- `[x]` Firmware bootstrap applies `/api/health` as a cheap AP/STA/mDNS reachability probe.
- `[x]` Firmware bootstrap gates the HTTP camera route when the Lua camera module is not built.
- `[x]` Firmware bootstrap copies JarvisNano Lua scripts and router rules into the FATFS image before each build.
- `[x]` Firmware bootstrap disables the heap-heavy serial CLI on the XIAO build; USB serial logs still work.
- `[x]` Firmware Docker build completes and produces `edge_agent.bin` with the 8 MB flash profile.
- `[x]` Latest flash reaches core startup without the REPL OOM or Lua LED OOM reboot.

## Wave 1: Build, Patch, And Release Hygiene

- `[x]` Restore Android CLI build verification by documenting the system Gradle path and installing Android command-line tools, SDK Platform 35, Build-Tools 34.0.0, and JDK 17 for local validation.
- `[x]` Verify Android CLI with `:app:assembleDebug`, `:app:testDebugUnitTest`, `:app:compileDebugAndroidTestKotlin`, and `:app:lintDebug` using `JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home` and `ANDROID_HOME=/opt/homebrew/share/android-commandlinetools`.
- `[ ]` Verify Android install and BLE permission flow on a physical phone.
- `[x]` Replace every prose-only patch artifact with a real patch or explicitly mark it as bootstrap-generated documentation.
- `[x]` Add a matching patch artifact for the native status LED mutation currently embedded in `scripts/bootstrap.sh`.
- `[x]` Add a local check that each patch artifact applies cleanly against the pinned `esp-claw` commit, or that the bootstrap mutation is intentionally generated; `scripts/check-patches.sh` now separates direct patch application from bootstrap-managed mutations.
- `[x]` Harden bootstrap patch failure behavior so missing managed components or upstream drift fails clearly before build.
- `[x]` Remove or replace any silent `reconfigure || true` path that can mask patch non-application.
- `[x]` Add a `scripts/flash.sh` or document the exact `esptool write_flash` command used after `./scripts/bootstrap.sh build`.
- `[x]` Add a smoke script that records firmware build metadata, binary size, storage image contents, and expected strings such as `Native status LED on gpio`.
- `[x]` Pin the `esp-claw` checkout to a fixed commit or tag; `scripts/bootstrap.sh` currently clones the default branch with `--depth 1`.
- `[x]` Record the resolved `esp-claw` commit in build output and release notes.
- `[ ]` Confirm a clean checkout can run `./scripts/bootstrap.sh`, `./scripts/bootstrap.sh build`, and the flash flow without relying on generated local files.
- `[x]` Add GitHub Actions CI for repository checks, dashboard JavaScript syntax, patch artifact checks, secret-pattern scanning, and Android CLI build/test tasks.
- `[x]` Add local open-source safety checks for obvious API tokens/private keys before publishing.
- `[~]` Verify the normal app flash path preserves Wi-Fi/LLM config while an explicit storage flash wipes and reprovisions cleanly.
- `[ ]` Decide whether generated `esp-claw/` edits should remain ignored-only or be reproducible solely from `scripts/bootstrap.sh` and `patches/`.
- `[x]` Add a short release checklist for firmware artifact generation, dashboard manifest update, Android APK build, and docs sync.
- `[x]` Add open-source repository metadata: contribution guide, security policy, support notes, changelog, issue templates, PR template, and binary/text attributes.

## Wave 2: Hardware Acceptance And LAN API Reliability

- `[x]` Build: `./scripts/bootstrap.sh build` completes in Docker for the 8 MB XIAO profile.
- `[x]` Flash: USB device enumerates as `/dev/cu.usbmodem1101`; `esptool write_flash` completes and hard-resets the board.
- `[x]` Boot: services reach Wi-Fi/FATFS/router/scheduler/Lua/MCP/Web IM, publish startup, and return from `app_main()`.
- `[x]` Physical LED: native GPIO21 task starts after `app_claw_start()` and logs `Native status LED on gpio 21 active_low=1`; heartbeat is non-fatal if allocation fails.
- `[x]` Dashboard: `http://127.0.0.1:8000/index.html` opens in the Codex in-app browser.
- `[~]` HTTP reachability: isolate why Mac curls to `192.168.50.80` time out despite ARP resolution; after the 2026-05-03 health-route flash, `/api/health`, `/api/status`, `/api/battery`, `/api/audio/level`, and `/api/wifi/scan` still timed out from macOS, while `esp-claw.local` resolution also timed out.
- `[ ]` Test HTTP from the device AP path at `192.168.4.1` to separate STA LAN issues from server issues.
- `[ ]` Test HTTP from another client on the same Wi-Fi network to rule out macOS routing/firewall behavior.
- `[~]` Capture serial logs during each failed curl and add the observed HTTPD/socket errors to this file; the post-flash boot log proves HTTPD starts, but macOS curl still timed out before a useful request log appeared.
- `[x]` Add a repeatable `scripts/http-matrix.sh` probe for AP, STA IP, and `esp-claw.local` covering `/api/health`, `/api/status`, battery, audio level, and Wi-Fi scan.
- `[~]` Probe `/api/health` first on AP, STA IP, and `esp-claw.local`; STA IP and mDNS failed from macOS after the 2026-05-03 flash, AP path and second-client path are still pending.
- `[ ]` Verify final response bodies for `/api/status`, `/api/config`, `/api/battery`, `/api/audio/level`, and `/api/wifi/scan`.
- `[ ]` Verify normal API responses include `Access-Control-Allow-Origin: *`, not only `OPTIONS /api/*`.
- `[~]` Confirm dashboard polling does not starve or wedge the HTTP server when camera auto-refresh is enabled; post-flash serial logs showed repeated `/api/camera/snapshot` 405/500 responses from an active browser session, while a fresh dashboard reload disables auto-refresh after gated/pending camera failures.
- `[ ]` Add an HTTP regression checklist using absolute IP, `esp-claw.local`, AP IP, and dashboard configured host.

## Wave 3: Browser Dashboard And Onboarding

- `[x]` Keep Web Bluetooth guarded in Codex/in-app browser and show Chrome/Edge guidance instead of opening the native chooser.
- `[x]` Keep browser setup using `text/plain` POSTs for backward compatibility with older firmware.
- `[x]` Ship `/api/wifi/scan` as a firmware route returning `{aps:[{ssid,rssi,channel,auth}]}`.
- `[ ]` Re-test onboarding step 2 against real `/api/wifi/scan` once LAN/API reachability is stable.
- `[ ]` Re-test onboarding Wi-Fi save, restart, reconnect, and LLM test round-trip on a freshly erased storage image.
- `[x]` Define the AP-to-STA handoff after Wi-Fi save: continue on AP when it remains reachable, otherwise verify STA IP or `esp-claw.local` before advancing.
- `[~]` Verify onboarding Done succeeds after the handoff: `/api/config`, `/ws/webim`, and `/api/restart` now share the selected post-save host; physical-device retest pending.
- `[x]` Verify the dashboard displays battery endpoint failures differently from intentional `{wired:false,state:"not_wired"}`.
- `[x]` Verify camera unavailable/gated state renders as blocked/pending, not as a broken image loop.
- `[ ]` Add a Chrome/Edge BLE smoke test note for service discovery once firmware GATT advertising exists.
- `[ ]` Add visual QA screenshots for desktop and mobile widths after the current UI changes.
- `[x]` Run dashboard JavaScript syntax checks for `dashboard/index.html` and `dashboard/onboard.html`; `scripts/check-dashboard-js.mjs` now makes this repeatable locally and in CI.
- `[x]` Run in-app browser console QA and record any errors after a fresh reload; dashboard and onboarding reload without console errors in the Codex in-app browser, and the BLE button shows the Chrome/Edge guard without leaving the page.
- `[ ]` Confirm demo mode still masks SSIDs, IPs, and API keys after status/config polling changes.
- `[ ]` Capture repeatable screenshots for `index.html`, `index.html?demo=1`, `index.html?alive=1`, onboarding steps 1-5, Flash tab, and Settings tab.

## Wave 4: Firmware BLE GATT Bridge

- `[ ]` Enable the ESP32-S3 BLE stack configuration needed for a peripheral GATT service without regressing Wi-Fi, PSRAM, or heap.
- `[ ]` Implement firmware advertising for service `1ec185cd-4bc7-5797-a8b1-0f5b66c59757`.
- `[ ]` Use an advertised device name/prefix the Android client and dashboard docs can discover consistently.
- `[ ]` Implement `audio_in` notify characteristic `ca04b99f-5e74-5a35-8f4f-d1313f19b29b`.
- `[ ]` Implement `audio_out` write/write-no-response characteristic `872228b7-ccd8-55dd-b12b-5d0352903617`.
- `[ ]` Implement `state` notify characteristic `dab5c3d4-915d-5f25-acc9-9d511df742bf`.
- `[ ]` Implement `control` write characteristic `2e14c0f2-4b07-5802-a8f9-369752d7cf2a`.
- `[ ]` Negotiate MTU, document fallback packet sizes, and validate Android 12+ behavior.
- `[ ]` Add reconnect and disconnect state publishing.
- `[ ]` Add a firmware BLE smoke test: scan, connect, discover service, discover four characteristics, subscribe to state, write a harmless control command.
- `[ ]` Update `docs/PROTOCOL.md`, Android BLE README, and dashboard BLE copy only after firmware service behavior is verified.

## Wave 5: Android Companion

- `[x]` BLE UUID constants match the protocol.
- `[x]` BLE scan/connect flow is bounded and reports service-present or missing-service diagnostics.
- `[x]` Restore reproducible Android build on CLI; command-line tools, SDK Platform 35, Build-Tools 34.0.0, and JDK 17 are installed locally, and CI mirrors the commands.
- `[x]` Add Android JVM test harness dependency and a real `src/test` assertion so `:app:testDebugUnitTest` cannot pass as an empty source set.
- `[ ]` Add unit tests for `DeviceClient`, config patch typing, discovery/repository state, and status polling recovery.
- `[ ]` Add instrumented smoke tests for permissions and navigation.
- `[x]` Add Android acceptance checklist: sync, build, install, grant BLE permissions, scan, connect, missing-service diagnostic, HTTP status/config, chat, camera failure message.
- `[x]` Harden mDNS discovery so it verifies the resolved host with `GET /api/status`, instead of trusting the first `_http._tcp.local.` `esp-claw*` result.
- `[x]` Fix repository polling recovery: with responses `200 -> timeout -> 200`, the app surfaces failure and then returns to connected on the next successful poll without an app restart.
- `[ ]` Preserve config JSON types when editing settings; boolean/number/string values must round-trip without type drift and unchanged fields should be omitted.
- `[ ]` Implement GATT characteristic discovery state in UI after firmware service lands.
- `[ ]` Implement a serialized GATT operation queue for MTU request, service validation, characteristic validation, reads, writes, and descriptor writes.
- `[ ]` Enable CCCD notifications for `audio_in` and `state`.
- `[ ]` Implement `state` JSON parsing and render mode, battery, and error diagnostics.
- `[ ]` Implement `control` writes for `start_listen`, `stop`, `restart`, and transport selection.
- `[ ]` Implement `audio_out` writes with 20 ms PCM16 mono frames for phone-to-device playback.
- `[ ]` Implement `audio_in` notify ingestion and either play/record/forward depending on final protocol direction.
- `[ ]` Add BLE reconnect behavior and clear user-facing failure messages for permission denied, scan timeout, GATT error, and service missing.
- `[ ]` Add permission matrix tests for API 30, 31, and 35.
- `[ ]` Request `RECORD_AUDIO` only when Phase-3/local audio mode is enabled; denial should disable local voice while leaving Wi-Fi chat usable.
- `[ ]` Replace placeholder "New session" behavior with a real session id rollover and local message clear.
- `[ ]` Add first-boot provisioning or clearly hand off to browser onboarding until mobile onboarding exists.
- `[ ]` Verify mDNS discovery, manual host override, `/api/status`, `/api/config`, `/ws/webim`, and `/api/camera/snapshot` failure handling on a physical phone.
- `[ ]` Keep cleartext HTTP limited to trusted LAN behavior and document the future auth/token plan.

## Wave 6: Voice Loop, TTS, And Speaker Output

- `[ ]` Wire PAM8002A combo module: GPIO4 -> 270 ohm -> 100 nF low-pass -> PAM8002A IN+, common ground, VCC from USB 5 V rail.
- `[ ]` Verify raw tone/PCM playback through I2S0 TX before adding cloud TTS.
- `[ ]` Decide firmware ownership for I2S0 TX so audio output does not conflict with mic level polling or future BLE audio.
- `[ ]` Add TTS config fields for provider, model/voice, sample rate, and output format.
- `[ ]` Add firmware TTS provider path from Lua or native capability for MiniMax, Bailian, OpenAI, ElevenLabs, or configured custom endpoint.
- `[ ]` Stream synthesized PCM to I2S0 TX without blocking the agent loop or starving HTTP.
- `[ ]` Add cancellation/interrupt behavior when the user speaks or presses stop during playback.
- `[ ]` Add wake-word or low-power VAD path.
- `[ ]` Measure end-of-utterance to first-audio-frame latency and tune toward the 800 ms target.
- `[ ]` Document speaker wiring, volume limits, current draw, and known distortion/noise tradeoffs.

## Wave 7: Battery And Power

- `[ ]` Wire the 503450 LiPo to the BAT+ pad and verify charge/power behavior.
- `[ ]` Choose and document the battery sense GPIO and resistor divider values.
- `[ ]` Add ADC one-shot read path for battery voltage.
- `[ ]` Replace `/api/battery` not-wired stub with calibrated voltage, percent, charging, source, and state readings.
- `[ ]` Cross-check firmware readings against a multimeter at full, nominal, and low voltages.
- `[ ]` Add low-battery state to GPIO21 LED pattern and BLE `state` payload.
- `[ ]` Add dashboard and Android low-battery warnings.
- `[ ]` Measure USB-powered, Wi-Fi idle, BLE connected, mic active, speaker active, and camera active current draw.
- `[ ]` Update enclosure/battery docs with heat, runtime, and charging notes.

## Wave 8: Camera

- `[~]` Keep camera HTTP route gated while the current OV3660 `esp_cam_sensor` path is blocked by SCCB format-set failures.
- `[x]` Document that 2026 XIAO Sense batches use OV3660, not only OV2640.
- `[x]` Keep UI labels hardware-neutral or `OV3660 / OV2640`.
- `[ ]` Decide whether camera is required for Phase 2 or deferred to Phase 4.
- `[ ]` Prototype the legacy `espressif/esp32-camera` driver path documented in `docs/CAMERA.md`.
- `[ ]` Resolve dependency conflicts between `esp32-camera`, `esp_cam_sensor`, `esp_video`, and `lua_module_camera_service`.
- `[ ]` Re-enable camera board/device config only after boot remains stable with PSRAM settings.
- `[ ]` Validate `curl -m 10 -o /tmp/snap.jpg http://<device>/api/camera/snapshot` returns a valid JPEG.
- `[ ]` Validate dashboard capture and live preview.
- `[ ]` Validate Android snap once and live mode.
- `[ ]` Document final sensor detection, resolution, JPEG quality, memory use, and fallback behavior.
- `[ ]` Upstream or document the local NO-SOI patch if the V4L2 path remains relevant.

## Wave 9: Protocol, Security, And Client Contract

- `[x]` Canonical BLE UUIDs are frozen in `docs/PROTOCOL.md`.
- `[x]` HTTP endpoint matrix includes Phase-2 `/api/health`, `/api/audio/level`, `/api/battery`, `/api/wifi/scan`, camera snapshot, and preflight.
- `[ ]` Add explicit HTTP error schema for unavailable hardware, timeouts, and gated features.
- `[ ]` Add BLE packet framing details: sequence number, chunk duration, content type, backpressure, and error recovery.
- `[ ]` Add lifecycle diagrams for Wi-Fi mode and BLE Privacy Mode.
- `[ ]` Define trusted-LAN auth boundary for Phase 1 and the future auth/token plan.
- `[ ]` Decide whether restart/config writes need a local pairing token before public release.
- `[ ]` Add compatibility notes for older firmware that lacks preflight, battery, Wi-Fi scan, or BLE.
- `[ ]` Reconcile BLE frame sizing: Android BLE README says 20 ms frames while `docs/PROTOCOL.md` needs one canonical packet/frame size.
- `[ ]` Reconcile Phase-3 local runtime docs so Android README, `llm/README.md`, and protocol name the same reference runtime/model or clearly explain alternatives.
- `[ ]` Add a changelog entry for the native LED, serial REPL disable, Wi-Fi scan, battery stub, camera gate, and Android BLE diagnostics.

## Wave 10: Phase 3 Display And Privacy Mode

- `[ ]` Add the Seeed 1.28 in Round Display for XIAO to the hardware plan and confirm pin sharing with current I2C/audio/camera choices.
- `[ ]` Add `spi_display` peripheral and display device config.
- `[ ]` Bring up LVGL using the closest existing esp-claw board pattern.
- `[ ]` Build a compact chat/status UI that mirrors the dashboard without oversized marketing layout.
- `[ ]` Add passkey or pairing display flow for stronger BLE pairing.
- `[ ]` Implement animated state face using `espressif2022/esp_emote_expression` or a lighter fallback if memory is tight.
- `[ ]` Define phone-side Gemma 4 E4B local inference package plan, model download flow, storage budget, and thermal behavior.
- `[ ]` Wire BLE Privacy Mode: device mic -> phone model -> phone TTS -> device speaker.
- `[ ]` Add offline/privacy indicators to dashboard, Android, and on-device display.

## Done Criteria For Phase 2

- Firmware builds from a clean checkout with `./scripts/bootstrap.sh build`.
- Firmware flashes with a documented command and boots without OOM/reboot loops.
- Dashboard opens at `http://127.0.0.1:8000/index.html` without console syntax errors.
- HTTP `/api/health`, `/api/status`, `/api/config`, `/api/battery`, `/api/audio/level`, `/api/wifi/scan`, and browser preflight behavior are verified on the same client network path the dashboard will use.
- Android build command is documented and verified.
- Phone can discover the robot over BLE and either connect to the Phase-2 service or show a clear missing-service diagnostic.
- Firmware BLE GATT exposes the canonical service and four characteristics.
- The speaker path can play generated or test PCM through the PAM8002A module.
- Battery endpoint is either ADC-backed and calibrated or explicitly marked not wired in UI/docs.
- Any Phase-2 feature that remains hardware-blocked is explicitly marked with the blocker and next physical test.
