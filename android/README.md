# JarvisNano — Android companion

The open-source Android companion for the **JarvisNano** XIAO ESP32-S3 Sense
device. Built by [Ingenious Digital](https://ingeniousdigital.com), licensed
Apache-2.0.

> JarvisNano is the polished public companion. The personal "Zero Chat" build the
> author runs day-to-day stays separate, but speaks the same API surface so the
> two will stay compatible.

---

## What it does

| Phase | Status      | Surface                                                      |
| ----- | ----------- | ------------------------------------------------------------ |
| 1     | Implemented | Wi-Fi (HTTP + WebSocket) chat, telemetry, configuration       |
| 2     | In progress | BLE scan/connect UI + canonical GATT UUID readiness           |
| 3     | Marked TODO | On-device Gemma 4 E4B (multimodal) for offline private mode   |

### Phase 1 features (working)

- **Cockpit** — live telemetry orb, system / Wi-Fi / LLM / capability tiles,
  BLE scan/connect readiness, restart and new-session controls. Polls
  `/api/status` every 4 s.
- **Chat** — `/ws/webim` WebSocket subscription with user / agent / system
  bubbles. Sends through `/api/webim/send` with a `text/plain` body for
  compatibility with older firmware; current bootstrap builds also register
  `OPTIONS /api/*` for JSON clients.
- **Settings** — pulls `/api/config`, groups fields by Wi-Fi / LLM / IM /
  Misc, masks anything ending in `_password` / `_secret` / `_token` /
  `_api_key` with a show-hide toggle, then POSTs the full blob back. Save +
  Restart in one tap.
- **About** — mascot, version, links to the GitHub repo and Ingenious Digital.
- **mDNS** discovery for `esp-claw.local` on `_http._tcp.local.`. Manual host
  override available in Settings if your network blocks multicast.

---

## Tech stack

- Kotlin **2.1**
- Android Gradle Plugin **8.7+**, min SDK 28, target SDK 35
- Jetpack **Compose** with **Material 3**
- **OkHttp 4.12** — HTTP and WebSocket
- **kotlinx.serialization-json** — typed payloads
- **jmdns** — `esp-claw.local` discovery
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

This repo intentionally does **not** commit `gradlew`, `gradlew.bat`, or
`gradle/wrapper/gradle-wrapper.jar`. The checked-in wrapper properties pin the
expected Gradle distribution, but contributors should use a system Gradle first
and keep any locally generated wrapper files untracked.

Required local tools:

- JDK 17
- Android SDK with API 35 installed
- System Gradle compatible with the pinned wrapper distribution

On macOS with Homebrew, the shortest clean setup is:

```bash
brew install gradle
brew install --cask android-commandlinetools

export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"

sdkmanager "platform-tools" "platforms;android-35" "build-tools;35.0.0"
```

If you use Android Studio instead, install Android SDK Platform 35 and Android
SDK Build-Tools 35 from **Settings -> Languages & Frameworks -> Android SDK**,
then either export `ANDROID_HOME` or create an untracked `local.properties`
file containing `sdk.dir=/path/to/android/sdk`.

Preferred CLI path:

```bash
cd android
gradle :app:assembleDebug
```

If your machine does not already have Gradle, install it with your package
manager or SDKMAN. If you generate wrapper files locally for convenience, do not
commit the generated scripts or `gradle-wrapper.jar`:

```bash
cd android
gradle wrapper --gradle-version 8.10.2
./gradlew :app:assembleDebug
```

Output APK: `app/build/outputs/apk/debug/app-debug.apk`.

### Test scaffold

The project has dependency-free placeholder sources under `src/test` and
`src/androidTest` so the source sets compile once Gradle is available. They are
intentionally not full assertions yet; they reserve the package and commands for
future unit, integration, and device tests without forcing a test framework
choice in this slice.

```bash
cd android
gradle :app:testDebugUnitTest
gradle :app:compileDebugAndroidTestKotlin
```

Run device tests only when a phone or emulator is connected:

```bash
gradle :app:connectedDebugAndroidTest
```

---

## Run on a phone

```bash
# Pair your phone via USB with developer mode + USB debug enabled.
gradle :app:installDebug
adb shell am start -n com.ingeniousdigital.jarvisnano.debug/com.ingeniousdigital.jarvisnano.MainActivity
```

The phone must be on the **same Wi-Fi network** as the JarvisNano device for
Phase 1 connectivity. mDNS discovery times out after 8 s — if your network
blocks multicast (most enterprise / hotel networks do), open **Settings →
Device address** and paste the device's IP manually.

### Emulator caveats

- BLE radios are not virtualized on the standard AVDs, so Phase 2 work needs a
  physical device.
- The emulator can hit the device over Wi-Fi if both are on a network the
  host machine can route to.

---

## Acceptance checklist

Use this checklist for Android build and smoke-test signoff.

### Build and tests

- [ ] From `android/`, `gradle :app:assembleDebug` completes.
- [ ] From `android/`, `gradle :app:testDebugUnitTest` completes.
- [ ] From `android/`, `gradle :app:compileDebugAndroidTestKotlin` completes.
- [ ] No `gradlew`, `gradlew.bat`, or `gradle/wrapper/gradle-wrapper.jar` files
      are committed.

### Install and launch

- [ ] `adb devices` lists the target phone as `device`.
- [ ] `gradle :app:installDebug` installs the debug APK.
- [ ] `adb shell am start -n com.ingeniousdigital.jarvisnano.debug/com.ingeniousdigital.jarvisnano.MainActivity`
      opens the companion app.
- [ ] App remains responsive after rotating the phone and backgrounding /
      foregrounding once.

### HTTP and WebSocket

- [ ] Phone and JarvisNano are on the same Wi-Fi network.
- [ ] Cockpit discovers `esp-claw.local` or connects after setting a manual IP.
- [ ] Cockpit telemetry updates from `/api/status` for at least three polling
      intervals.
- [ ] Chat sends a message through `/api/webim/send` and receives WebSocket
      updates from `/ws/webim`.
- [ ] Settings loads `/api/config`, preserves masked secret fields, saves a
      non-secret change, and the device accepts restart from the app.

### BLE

- [ ] Test on a physical Android phone; standard emulators cannot validate BLE.
- [ ] Android permission prompts are shown and accepted when scanning.
- [ ] BLE scan shows the JarvisNano advertisement.
- [ ] Connect succeeds and the displayed service / characteristic UUIDs match
      the BLE README.
- [ ] Disconnect and reconnect work without force-closing the app.

---

## Roadmap

- **Phase 1 (now):** Wi-Fi cockpit + chat + settings.
- **Phase 2:** BLE GATT bridge — see [`app/src/main/kotlin/com/ingeniousdigital/jarvisnano/ble/README.md`](./app/src/main/kotlin/com/ingeniousdigital/jarvisnano/ble/README.md).
- **Phase 3:** On-device Gemma 4 E4B multimodal inference — see [`app/src/main/kotlin/com/ingeniousdigital/jarvisnano/llm/README.md`](./app/src/main/kotlin/com/ingeniousdigital/jarvisnano/llm/README.md).

---

## License

Apache-2.0, the same as the rest of the JarvisNano repo. See the root
[`LICENSE`](../LICENSE) file.
