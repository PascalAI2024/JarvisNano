# JarvisNano Roadmap

Last reconciled: **2026-11-23**. This is the product milestone arc; granular tasks and physical acceptance records live in [`../PLAN.md`](../PLAN.md) and [`../CURRENT_WORK.md`](../CURRENT_WORK.md).

---

## Shipped — Glass-2 & Waves 1–3 baseline

| Area | Shipped capabilities on the Waveshare 1.75C |
|---|---|
| **Voice & Wake** | Direct Gemini Live 24 kHz full-duplex audio, dedicated `jr_wake` WakeNet9 `"Jarvis"` task (wakes rest & cuts replies), 16 kHz AEC uplink, 600–1500 ms adaptive jitter buffer, DAC-synced speech cadence halo, warm 2nd-harmonic chimes, three session watchdogs, `/api/diag/voiceperf` |
| **24 Voice Tools** | **18 instant 0 ms offline tools** (deep-sleep surviving RTC timers, Pomodoro focus, 4-lap chronograph stopwatch, NVS desk notes, daily briefings, local weather/time, voice glass/watch/volume/brightness/sunlight/UI-sound self-control, rim-arc `ask_user`) + **6 JarvisMCP gateway tools** over persistent HTTPS keep-alive |
| **Glass-2 OLED UI** | 5-screen spatial ring (`JARVIS · WATCH · WEATHER · STATUS · ACTIVITY` + summoned `DESK`), **8 luxury watch dials** (`JARVIS · DIVER · DRESS · PILOT · MINIMAL · FUTURE · SKELETON · CELESTIAL`) with IMU tilt lighting/parallax & PSRAM dial cache, 60 Hz `LCD_TE` (`GPIO13`) flush sync, CO5300 HBM (`0x66`) sunlight boost with thermal/battery guard |
| **Command Deck & Companions** | Zero-dependency embedded Wi-Fi Command Deck (`GET /`), paired 466×466 RGB565 remote canvas (`send-canvas.py`), Codex rim-arc Desk mode, ZeroChat private-Wi-Fi audio lease, live board probe (`/api/diag/board`), crash forensics with ELF summary (`/api/diag/crash`) |
| **Memory, Power & Testing** | PSRAM-first SRAM budget (`ALWAYSINTERNAL=1024`, `cJSON` in PSRAM), adaptive CPU gears (`240 / 160 MHz`), 21 Hz low-power rest IMU, deep sleep with WoM lift-wake + touch-wake + RTC timer persistence, dual-slot OTA with probation/rollback, **23 automated host test suites** (`./scripts/host-tests.sh`) |

---

## Active — Physical hardware proof

Features compiled and host-tested still require physical 1.75C measurements before closing hardware acceptance:

- **CO5300 HBM & TE sync proof:** Capture live `/api/diag/board` 60 Hz TE phase stability and verify visual/thermal behavior of `0x66` sunlight boost on battery and USB.
- **Open hardware probes:** Validate `GPIO3` (`SYS_OUT`) PWR-key mirror polarity/strapping via `board-probe.py --watch pwr` (potential deep-sleep wake source) and check 2-finger CST9217 reporting via `board-probe.py --watch touch`.
- **Battery & unattended deep-sleep curves:** Record fresh unplugged drain curves across `AWAKE → WHISPER → DREAM → DEEP SLEEP` and close unattended 4-hour natural ladder acceptance.
- **Unanswered-utterance watchdog tuning:** Observe real-room false-positive rate and tune if ambient chatter triggers unnecessary reconnects.

---

## Public release gates (v1.0)

Mandatory before publishing prebuilt binaries or operating outside an isolated trusted LAN (see [`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md)):

1. **Authenticated, encrypted control plane:** `JR_DEV_OPEN_DIAGNOSTICS=0` and TLS/bound authentication on HTTP routes so pairing tokens never traverse plaintext Wi-Fi.
2. **Signed OTA:** Offline app-signing key verification before `esp_ota_set_boot_partition`, proven with both valid and wrong-key rejection tests on hardware.
3. **At-rest secret protection:** Attended NVS and flash encryption with negative flash-dump verification.
4. **Server-enforced tool boundary:** Provision the typed JarvisMCP `/device/v1/invoke` route as default so least-privilege capability policy lives on the server as well as the device.
5. **Exact third-party notices:** Generate and attach the exact component license bundle via `scripts/generate-third-party-notices.py`.

---

## Post-v1 — Spatial & private expansion

- **Private Android BLE route:** Mutually exclusive BLE audio transport to an on-phone STT/LLM/TTS stack (`android/` scaffold exists; GATT audio service not yet wired in firmware).
- **Seamless Gemini session resumption:** Preserve multi-turn conversational context cleanly across provider session boundaries and watchdog reconnects.
- **Dual-mic directionality:** Only after real enclosure captures classify left/right speech above 90%.
- **Unused FAT partition decision:** Either adopt for documented durable local storage or reclaim via an explicit partition-table migration.

---

## Non-negotiable product invariants

- **Physical authority wins:** Remote APIs and synthetic touch can never clear physical privacy or trap an operator lease.
- **One compositor, one input grammar:** No secondary GUI framework beside `jr_display` and no hidden gesture paths.
- **Evidence over theatre:** Host tests, HTTP 200s, and software framebuffer mirrors never substitute for physical hardware proof.
