# Finish Plan

Last updated: **2026-05-03**.

This is the shortest practical path from the current stable booting board to a
finished Phase-2 public release. It is based on the current task board plus
fresh checks against upstream ESP-IDF, Android, and ESP32 camera documentation.

## North Star

Ship Phase 2 as a reliable voice-ready robot base:

- Firmware builds, flashes, boots, and responds over HTTP from the dashboard
  network path.
- Browser dashboard and onboarding work without console errors or hidden
  hardware assumptions.
- Android companion builds from CLI and can show BLE scan/connect diagnostics.
- BLE firmware exposes the canonical JarvisNano service with `state` and
  `control` first.
- Battery and camera are either working or explicitly marked unavailable with
  clean UI/error payloads.
- Speaker/TTS path proves raw playback before cloud TTS is added.

## Priority Order

### 1. Make The Toolchain Reproducible

Do this before new firmware features. It makes every later wave cheaper.

- Install/configure Android SDK API 35 and prove:
  - `gradle :app:assembleDebug`
  - `gradle :app:testDebugUnitTest`
  - `gradle :app:compileDebugAndroidTestKotlin`
- Add GitHub Actions:
  - firmware script/static checks
  - dashboard JavaScript syntax check
  - Android build once SDK is available in CI
- Add `scripts/check-patches.sh` so patch artifacts either apply cleanly to the
  pinned ESP-Claw commit or are explicitly marked as generated bootstrap
  mutations.

Why: Android Gradle Plugin 8.7 supports compile SDK 35 and requires JDK 17+.
The current repo already uses AGP 8.7.3 and compile SDK 35, so the missing
piece is Android SDK installation, not code shape.

Progress on 2026-05-03: local command-line tools are installed, Android CLI
build/test/lint commands pass under JDK 17, and GitHub Actions now mirrors the
same checks.

## 2. Fix LAN HTTP Reachability Before Feature Work

The board boots cleanly, but macOS curls to `192.168.50.80` still time out.
Until that is explained, camera/BLE/TTS debugging will waste time.

Run this exact matrix:

- AP path: join `esp-claw-XXXXXX`, test `http://192.168.4.1/api/status`.
- STA path from macOS: test `http://192.168.50.80/api/status`.
- STA path from phone/another laptop: same endpoint.
- mDNS path: `http://esp-claw.local/api/status`.
- Serial capture during every request.

Add a tiny `/api/health` route if needed. It should avoid config/FATFS/LLM work
and return immediately with uptime, heap, Wi-Fi mode, and request count.

Efficiency rule: keep dashboard camera auto-refresh off by default until the
network path is stable. ESP-IDF HTTP server supports persistent connections and
per-session state; use that when adding long-lived or repeated transfers, but
keep health/status handlers tiny.

## 3. BLE: Ship `state/control` Before Audio

Do not start BLE with audio streaming. Start with the smallest service that
unblocks Android and Chrome/Edge diagnostics:

- Advertise service `1ec185cd-4bc7-5797-a8b1-0f5b66c59757`.
- Add `state` notify characteristic with JSON:
  - boot phase
  - Wi-Fi mode/IP
  - battery state
  - error string
  - feature gates: `camera`, `speaker`, `tts`
- Add `control` write characteristic:
  - `ping`
  - `restart`
  - `start_listen`
  - `stop`
- Defer `audio_in` and `audio_out` until MTU negotiation, queueing, and state
  notifications are reliable.

Use ESP-IDF NimBLE, not Bluedroid, for the first implementation. NimBLE has an
official ESP-IDF host port and the upstream `bleprph` peripheral example is the
right starting point. Keep write payloads small and framed even after MTU
negotiation; Android behavior varies by device and OS version.

## 4. Speaker/TTS: Prove Playback Before Cloud TTS

Finish the speaker in three steps:

1. Raw tone task to I2S0 PDM TX on GPIO4 through the RC filter.
2. Small PCM test clip from FATFS.
3. Cloud TTS streaming.

Do not call a TTS provider from the main app path. Put TTS fetch/decode/playback
behind a queue:

- agent reply text -> playback queue
- TTS worker -> HTTP client -> PCM chunks
- audio worker -> I2S PDM TX
- stop/listen event -> cancel current playback

ESP32-S3 has I2S PDM support; the current board config already maps PDM TX to
GPIO4. The important efficiency choice is task ownership: one audio-output
owner, no competing direct writes from Lua, HTTP, BLE, and TTS code.

## 5. Battery: Use ADC Oneshot, Then Calibrate

Keep this small:

- Pick an ADC1-capable free GPIO that does not collide with display/camera.
- Use a high-value divider and document it.
- Use ESP-IDF ADC oneshot mode for periodic readings.
- Convert raw -> millivolts -> percentage with a simple LiPo curve.
- Add `source: "adc"` once real readings land.

Do not block Phase 2 on perfect fuel gauging. A calibrated voltage + low-battery
warning is enough for public release.

## 6. Camera: Switch Driver, But Defer It Behind HTTP/BLE

The fastest camera path is the legacy `espressif/esp32-camera` component:

- It supports ESP32-S3.
- It supports OV3660.
- Recent releases mention XIAO Sense OV3660 support.
- It documents the PSRAM/JPEG tradeoffs that match our failure mode.

Implementation order:

1. Add `espressif/esp32-camera`.
2. Create a new small camera service wrapper using `esp_camera_fb_get()`.
3. Return JPEG only.
4. Start with one frame buffer and low resolution.
5. Only add live preview after one-shot JPEG is stable.

Avoid RGB/YUV for Phase 2. The ESP32 camera driver docs explicitly call out
YUV/RGB pressure when Wi-Fi is enabled; JPEG is the efficient path.

## 7. Security Boundary For Public Release

Before calling it finished:

- Keep reads open on trusted LAN.
- Add a pairing/local token for writes:
  - `/api/config`
  - `/api/restart`
  - WebSocket send
  - BLE control writes that change state
- Document that Phase 2 is trusted-LAN unless token mode is enabled.

This can be simple: generated token stored in FATFS, displayed once in
onboarding, and accepted as `X-JarvisNano-Token` for write operations.

## What To Defer

Defer these until after Phase 2:

- BLE audio streaming
- camera live stream
- wake word
- phone-side local LLM
- round display
- full auth/pairing UI polish

Those are real features, but they are not blockers for a credible public Phase
2 base.

## Recommended Next Sprint

1. Install Android SDK API 35 and get Android CLI build green.
2. Add CI for static checks and Android build.
3. Add `/api/health` and run the AP/STA/mDNS HTTP matrix.
4. Implement BLE `state/control` only.
5. Wire speaker and prove raw PDM-TX tone.
6. Add ADC battery reading.
7. Revisit camera with `esp32-camera` only after HTTP and BLE are stable.

## Sources

- ESP32 camera driver: https://github.com/espressif/esp32-camera
- ESP32 camera driver releases: https://github.com/espressif/esp32-camera/releases
- ESP-IDF NimBLE overview: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/ble/overview.html
- ESP-IDF NimBLE peripheral example: https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/nimble/bleprph/tutorial/bleprph_walkthrough.md
- ESP-IDF HTTP server: https://docs.espressif.com/projects/esp-idf/en/latest/api-reference/protocols/esp_http_server.html
- ESP-IDF I2S/PDM: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html
- ESP-IDF ADC oneshot: https://docs.espressif.com/projects/esp-idf/en/v5.3.5/esp32s3/api-reference/peripherals/adc_oneshot.html
- Android Gradle Plugin 8.7: https://developer.android.com/build/releases/agp-8-7-0-release-notes
- Android SDK Manager: https://developer.android.com/tools/sdkmanager
- GitHub setup-java: https://github.com/actions/setup-java
- Gradle GitHub Actions: https://github.com/gradle/actions
