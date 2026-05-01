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
| 2     | Scaffolded  | BLE GATT bridge for direct mic/speaker/control                |
| 3     | Marked TODO | On-device Gemma 4 E4B (multimodal) for offline private mode   |

### Phase 1 features (working)

- **Cockpit** — live telemetry orb, system / Wi-Fi / LLM / capability tiles,
  restart and new-session controls. Polls `/api/status` every 4 s.
- **Chat** — `/ws/webim` WebSocket subscription with user / agent / system
  bubbles. Sends through `/api/webim/send` (uses `text/plain` body to skirt the
  firmware's CORS preflight gap, which is also what the web dashboard does).
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

The Gradle wrapper jar is intentionally **not** committed. Generate it once:

```bash
cd android
gradle wrapper --gradle-version 8.10.2
```

(That requires a system Gradle. After that, all subsequent builds use the
wrapper.) Then:

```bash
./gradlew :app:assembleDebug
```

Output APK: `app/build/outputs/apk/debug/app-debug.apk`.

---

## Run on a phone

```bash
# Pair your phone via USB with developer mode + USB debug enabled.
./gradlew :app:installDebug
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

## Roadmap

- **Phase 1 (now):** Wi-Fi cockpit + chat + settings.
- **Phase 2:** BLE GATT bridge — see [`app/src/main/kotlin/com/ingeniousdigital/jarvisnano/ble/README.md`](./app/src/main/kotlin/com/ingeniousdigital/jarvisnano/ble/README.md).
- **Phase 3:** On-device Gemma 4 E4B multimodal inference — see [`app/src/main/kotlin/com/ingeniousdigital/jarvisnano/llm/README.md`](./app/src/main/kotlin/com/ingeniousdigital/jarvisnano/llm/README.md).

---

## License

Apache-2.0, the same as the rest of the JarvisNano repo. See the root
[`LICENSE`](../LICENSE) file.
