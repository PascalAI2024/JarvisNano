# JarvisNano — Android companion

The optional Android companion scaffold for the **JarvisNano** Waveshare
ESP32-S3 Touch AMOLED 1.75 device. The active product is standalone: Gemini and
the JarvisMCP client loop run from the ESP32 without this app; the gateway and
sandbox remain remote. Android is being retained as the future private
local-model brain.

> JarvisNano is the polished public companion. The personal "Zero Chat" build the
> author runs day-to-day stays separate, but speaks the same API surface so the
> two will stay compatible.

---

## What it does

| Phase | Status | Surface |
| ----- | ------ | ------- |
| 1 | Implemented | Wi-Fi cockpit against the v5 `/api/cockpit` contract |
| 2 | Scaffold only | BLE scan/connect types and canonical UUIDs; firmware GATT/audio is not enabled |
| 3 | Interface only | Private Android STT/LLM/TTS route; no local model is wired yet |

### Current verified slice

- **Cockpit** — typed v5 network, voice, display, touch, Brain Link, and
  on-device routing state from `/api/cockpit`.
- **Manual host connection** — required today because the v5 firmware does not
  yet bundle the ESP-IDF mDNS component.
- **Brain-route truth** — Cloud Gemini is current; Private Android and Codex
  Desk are shown as optional routes rather than silently implied as active.

The old ESP-Claw `/api/status`, `/api/config`, `/api/webim/send`, and
`/ws/webim` claims are not part of the v5 firmware contract.

---

## Tech stack

- Kotlin **2.1**
- Android Gradle Plugin **8.7+**, min SDK 28, target SDK 35
- Jetpack **Compose** with **Material 3**
- **OkHttp 4.12** — HTTP and WebSocket
- **kotlinx.serialization-json** — typed payloads
- **jmdns** — retained discovery scaffold; v5 currently requires manual IP
- Manual DI through `App.kt` (no Koin / Hilt — deliberately small)

---

## Open in Android Studio

1. Install **Android Studio Koala (2024.1)** or newer.
2. `File → Open…` and select this `android/` folder. Studio detects the
   version-catalog Gradle project and runs an initial sync.
3. The first sync needs internet access — Compose BOM, OkHttp, kotlinx-
   serialization, and jmdns are pulled from Maven Central.

---

## Build from the CLI

This repo commits the Gradle wrapper so CLI builds use the same pinned Gradle
distribution everywhere. Prefer `./gradlew` for project commands.

Required local tools:

- JDK 17
- Android SDK with API 35 installed
- Android SDK command-line tools or Android Studio SDK Manager

On macOS with Homebrew, the shortest clean setup is:

```bash
brew install openjdk@17
brew install --cask android-commandlinetools

export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"

sdkmanager "platform-tools" "platforms;android-35" "build-tools;34.0.0"
```

If you use Android Studio instead, install Android SDK Platform 35 and Android
SDK Build-Tools 34.0.0 from **Settings -> Languages & Frameworks -> Android SDK**,
then either export `ANDROID_HOME` or create an untracked `local.properties`
file containing `sdk.dir=/path/to/android/sdk`.

Preferred CLI path:

```bash
cd android
./gradlew :app:assembleDebug
```

Output APK: `app/build/outputs/apk/debug/app-debug.apk`.

### Tests

The JVM suite contains substantive serialization, API-compatibility,
repository, and brain-routing assertions. Android instrumentation sources also
compile as part of the normal verification command.

```bash
cd android
./gradlew :app:testDebugUnitTest
./gradlew :app:compileDebugAndroidTestKotlin
```

Run device tests only when a phone or emulator is connected:

```bash
./gradlew :app:connectedDebugAndroidTest
```

---

## Run on a phone

```bash
# Pair your phone via USB with developer mode + USB debug enabled.
./gradlew :app:installDebug
adb shell am start -n com.ingeniousdigital.jarvisnano.debug/com.ingeniousdigital.jarvisnano.MainActivity
```

The phone must be on the **same trusted Wi-Fi network** as the JarvisNano for
the current cockpit. Enter the device IP manually; mDNS is not active in v5.

### Emulator caveats

- BLE radios are not virtualized on the standard AVDs, so Phase 2 work needs a
  physical device.
- The emulator can hit the device over Wi-Fi if both are on a network the
  host machine can route to.

---

## Acceptance checklist

Use this checklist for Android build and smoke-test signoff.

### Build and tests

- [ ] From `android/`, `./gradlew :app:assembleDebug` completes.
- [ ] From `android/`, `./gradlew :app:testDebugUnitTest` completes.
- [ ] From `android/`, `./gradlew :app:compileDebugAndroidTestKotlin` completes.
- [ ] `gradlew`, `gradlew.bat`, and `gradle/wrapper/gradle-wrapper.jar` are
      present so the pinned Gradle distribution is reproducible.

### Install and launch

- [ ] `adb devices` lists the target phone as `device`.
- [ ] `./gradlew :app:installDebug` installs the debug APK.
- [ ] `adb shell am start -n com.ingeniousdigital.jarvisnano.debug/com.ingeniousdigital.jarvisnano.MainActivity`
      opens the companion app.
- [ ] App remains responsive after rotating the phone and backgrounding /
      foregrounding once.

### HTTP cockpit

- [ ] Phone and JarvisNano are on the same Wi-Fi network.
- [ ] Cockpit connects after setting a manual device IP.
- [ ] Cockpit telemetry updates from `/api/cockpit` for at least three polling
      intervals.

### BLE

- [ ] Test on a physical Android phone; standard emulators cannot validate BLE.
- [ ] Android permission prompts are shown and accepted when scanning.
- [ ] Treat scan/connect as scaffold validation only until the firmware GATT
      service and audio characteristics are enabled.

---

## Roadmap

- **Phase 1 (now):** Optional Wi-Fi cockpit for the standalone device.
- **Phase 2:** BLE GATT bridge — see [`app/src/main/kotlin/com/ingeniousdigital/jarvisnano/ble/README.md`](./app/src/main/kotlin/com/ingeniousdigital/jarvisnano/ble/README.md).
- **Phase 3:** On-device Gemma 4 E4B multimodal inference — see [`app/src/main/kotlin/com/ingeniousdigital/jarvisnano/llm/README.md`](./app/src/main/kotlin/com/ingeniousdigital/jarvisnano/llm/README.md).

---

## License

Apache-2.0, the same as the rest of the JarvisNano repo. See the root
[`LICENSE`](../LICENSE) file.
