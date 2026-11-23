# JarvisNano v5 Protocol

> **Runtime authority:** `main/http_routes.c` on the 32 MB **Waveshare ESP32-S3-Touch-AMOLED-1.75C**.
> Last reconciled with compiled source: **2026-11-23**.

JarvisNano runs standalone on the ESP32-S3 by default — owning audio, display, touch, Gemini Live, 18 instant offline tools, and 6 JarvisMCP gateway tools. A browser **Command Deck**, **Codex** operator client, or paired **ZeroChat** phone can interact over the local HTTP control plane on port 80.

---

## Brain routes

| Route | Status | Owner of reasoning & audio |
|---|---|---|
| `cloud_gemini` | **Active (default)** | Direct ESP32 → Gemini Live WSS; 18 local tools (0 ms) + 6 persistent keep-alive JarvisMCP tools |
| `private_wifi` | **Active while leased** | Paired ZeroChat phone claims `mode=zerochat`, reads 16 kHz mic PCM, returns sequenced 24 kHz speaker PCM |
| `desk_codex` | **Optional surface** | Paired desktop/CLI claims `mode=codex`, presents rim-arc choice/notice/progress cards or 466×466 canvases |
| `private_android` | Planned | Future mutually exclusive BLE audio route (not wired in firmware) |

Entering a `zerochat` lease pauses Gemini with no automatic cloud fallback; lease release, expiry, or physical double-tap restores the prior privacy-safe state.

---

## Discovery, authentication & transport boundary

- **Discovery:** DHCP hostname `jarvisnano-<hex>` (no mDNS in v5). Read the IP from serial boot log or router DHCP table and open `http://<device-ip>/` or pass `--host <device-ip>` to host CLIs.
- **Pairing:** Hold **BOOT** for 1.5–5 s to show a 6-digit code and open a 60 s window. Claim once via `POST /api/pairing/claim` with `X-JarvisNano-Pair-Code` (3 wrong attempts close the window). Host CLIs (`jarvis-desk.py`, `jarvisctl.py`, `screens.py`, `send-canvas.py`, `board-probe.py`) store the token in **macOS Keychain** or **Windows per-user Registry** (`HKCU\Software\JarvisNano\Desk`) — never in source, logs, or CLI argv.
- **Auth headers:**
  - `X-JarvisNano-Token: <token>` — required on all paired reads and mutating controls.
  - `X-JarvisNano-Control: 1` — additionally required on all mutating `POST` routes.
  - `X-JarvisNano-Lease: <lease_id>` — additionally required on ZeroChat audio & lease renew/release routes.
- **Plaintext LAN boundary:** HTTP on port 80 is plaintext and suitable **only** on an isolated WPA2/WPA3 LAN controlled by the owner. `JR_DEV_OPEN_DIAGNOSTICS` relaxes pairing during development and **must be 0 before any release**.

---

## HTTP route catalog

### Command Deck, glass, timers, stopwatch & notes (Wave 1–3)

| Method | Path | Auth | Purpose |
|---|---|---|---|
| `GET` | `/` | Open (LAN) | Embedded **Wi-Fi Command Deck** (`main/diagnostics.html`): live 466×466 mirror, 8-watch switcher, timers/stopwatch, NVS notes, canvas pusher, board probe, crash forensics, logs |
| `POST` | `/api/glass` | Token + Control | Switch glass & display toggles via query params: `screen=jarvis\|watch\|weather\|status\|activity`, `style=jarvis\|diver\|dress\|pilot\|minimal\|future\|skeleton\|celestial`, `hbm=0\|1` (returns `409` if thermal/battery guard refuses), `ui_sounds=0\|1`. Never touches privacy or lease state. |
| `GET` | `/api/notes` | Token | Read NVS-persisted quick desk notes (`{count, max, notes:[{id, text, created_s}]}`) |
| `POST` | `/api/notes` | Token + Control | Add a note via JSON `{"text":"..."}` (or `?text=...`), delete one (`?delete=<id>`), or clear all (`?clear=1`) |
| `POST` | `/api/debug/timer` | Token + Control | Set/manage on-device timers: `?sec=N[&label=...]`, Pomodoro `?focus=M[&label=...]`, `?cancel=1[&label=...]`, or `?dismiss=1` |
| `POST` | `/api/debug/stopwatch` | Token + Control | Control the 4-lap chronograph: `?action=start\|stop\|lap\|reset` |
| `POST` | `/api/display/canvas` | Token + Control | Push a raw 466×466 RGB565 LE frame (`434,312` bytes) with `?ttl=ms` (default 30,000 ms), or clear via `?clear=1` |

### Diagnostics, hardware probe & crash forensics

| Method | Path | Auth | Purpose |
|---|---|---|---|
| `GET` | `/api/cockpit` | Token | Full network, voice, tool, display (`watch_style` across all 8 dials, `sun_rise_min`/`sun_set_min`, HBM, timers), touch, Agent Link, and Brain Link state |
| `GET` | `/api/device/health` | Token | Derived health verdict, memory headroom, playback jitter counters (`underruns`, `max_gap_ms`, `low_water_ms`, `dac_failures`, `replies`), OTA slot/probation state |
| `GET` | `/api/gemini/live` | Token | Detailed Gemini WebSocket, audio, tool counters, and transcript tail |
| `GET` | `/api/diag/board` | Token | Live 1.75C hardware probe (`?gyro=1`, `?watch=pwr\|touch`): GPIO13 TE frequency/period, GPIO3 PWR mirror, QMI8658C registers/temp/gyro, CST9217 15-byte touch report, AXP2101 ADC telemetry, QSPI clock |
| `GET` | `/api/diag/crash` | Token | Last reset reason, AXP2101 `pmic_on`/`pmic_off` boot record, flash ELF core dump status & parsed summary (`task`, `exc_pc`, `exc_cause`, `exc_vaddr`, `panic_reason`, `elf_sha`, `bt`) |
| `POST` | `/api/diag/crash?clear=1` | Token + Control | Erase the stored flash core dump |
| `GET` | `/api/diag/coredump.bin` | Token | Stream the raw stored flash core dump (`application/octet-stream`) |
| `GET` | `/api/diag/voiceperf` | Token | Voice-loop per-class FPS/latency (`speaking` vs `other`), tick skips, CPU MHz, `jr_wake` worker timing, and offline bench results |
| `POST` | `/api/diag/voiceperf?bench=1` | Token + Control | Run 64-frame offline AEC + WakeNet9 microbenchmark (read results via `GET`) |
| `GET` | `/api/diag/tasks` | Token | Contiguous internal SRAM, per-task stack high-water marks, and FreeRTOS runtime stats |
| `GET` | `/api/diag/vadlog` | Token | Bounded VAD/barge-in decision CSV |
| `GET` | `/api/logs?tail=N` | Token | 128 KB PSRAM ring log tail |
| `GET` | `/api/sensors` | Open (LAN) | QMI8658C motion and AXP2101 battery/USB telemetry |
| `GET` | `/api/power/charge` | Token | Read-only cached AXP2101 charge-register snapshot (`00..67`) with decoded phase/limits |
| `GET` | `/api/touch` | Open (LAN) | Touch counters and panel challenge state |

### Display mirrors, controls, voice, audio & OTA

| Method | Path | Auth | Purpose |
|---|---|---|---|
| `GET` | `/api/display` | Open (LAN) | Display health, `render_us`/`render_frame_us`, TE wait/timeout stats, and `boot_timing` |
| `GET` | `/api/display/snapshot.json` | Open (LAN) | Submission-mirror metadata and freshness |
| `GET` | `/api/display/snapshot.ppm` | Token | Exact submitted 466×466 frame as PPM (software mirror, not panel readback) |
| `GET` | `/api/display/snapshot.rgb565` | Token | Exact submitted 466×466 frame as raw RGB565 LE |
| `POST` | `/api/display/test?pattern=...` | Token + Control | Deterministic display diagnostic pattern |
| `POST` | `/api/display/hud?on=0\|1` | Token + Control | HUD frame-cost toggle |
| `POST` | `/api/display/choices?n=0..3` | Token + Control | Static choice-arc rendering diagnostic (`GET /api/display/choices/hit?x=&y=` checks geometry) |
| `POST` | `/api/debug/display?knob=N` | Token + Control | Dev renderer perf knob (`0` = shipped path) |
| `GET/POST` | `/api/device/levels` | Token (+ Control on POST) | Read or persist `volume` (`10..100`) and `brightness` ceiling (`10..100`) |
| `POST` | `/api/voice/control` | Token + Control | `?armed=0` mutes; `?armed=1` or `?resume=1` resumes **only** if physical privacy is not held |
| `POST` | `/api/debug/say?text=...` | Token + Control | Inject a text turn into the live Gemini session (works even when mic is muted) |
| `POST` | `/api/debug/briefing` | Token + Control | Deliver the daily briefing immediately without modifying the once-a-day latch |
| `POST` | `/api/debug/gain?...` | Token + Control | Audio gain, jitter buffer (`preroll=`, `refill=`), AEC mode/filter, and bench CPU gear (`cpu=80\|160\|240\|0`) |
| `POST` | `/api/debug/audio-stats?reset=1` | Token + Control | Zero playback and RX diagnostic counters |
| `GET/POST` | `/api/debug/sleep` | Token (+ Control on POST) | Inspect last wake/armed sources; `?now=1&wake_s=N` forces deep sleep (`409` during OTA probation); `?off=1` soft powers off via PMIC |
| `POST` | `/api/audio/self-test` | Token + Control | Run speaker-to-microphone sonar diagnostic capture |
| `GET` | `/api/audio/taps` · `/api/audio/tap.wav` | Token | Audio tap metadata & bounded WAV (`source=mic-clean\|mic-raw\|reference\|playback`) |
| `POST` | `/api/debug/input?kind=...` · `/api/input/tap` | Token + Control | Inject synthetic touch/swipe/hold (never clears physical privacy or approves consent) |
| `POST` | `/api/ui/shade?action=open\|close\|toggle` | Token + Control | Toggle control shade |
| `POST` | `/api/demo` | Token + Control | Bounded on-device attract reel |
| `POST` | `/api/diag/panel-touch?action=start\|cancel` | Token + Control | Physical panel/touch proof challenge |
| `GET/POST` | `/api/tools/config` | Token + Control | Redacted JarvisMCP status (`GET`) or write-only provisioning (`POST` `{"url","key","project_id"}`) |
| `POST` | `/api/pairing/claim?rotate=1` | Control + Pair Code | Claim pairing token during 60 s BOOT-hold window |
| `POST` | `/api/ota/upload` | Token + Control | Stream `jarvisrobot_v5.bin` into inactive 4 MB OTA slot and reboot into probation |
| `POST` | `/api/ota/assets` | Token + Control | Stream `emote_assets.bin` into art partition and reboot (`409` during app probation) |

### Operator leases, Codex Desk & ZeroChat audio

| Method | Path | Auth | Purpose |
|---|---|---|---|
| `GET/POST` | `/api/operator/lease` | Open (`GET`) / Token + Control (`POST`) | Read `{active, mode, ttl_ms}` or enter/renew/release `?ttl=S&mode=codex\|zerochat` / `?release=1` |
| `GET/POST` | `/api/agent/link` | Token | Bounded coding-status companion rim (`working\|verifying\|waiting\|succeeded\|failed`) |
| `GET` | `/api/brain/outbox?after=N` | Token | Poll physical `surface.action` touch events (8-event monotonic ring) |
| `POST` | `/api/brain/inbox` | Token + Control | Present, update, or dismiss a bounded Codex card (`surface.present\|update\|dismiss`) |
| `GET` | `/api/companion/audio/in` | Token + Lease | ZeroChat 16 kHz mono PCM16 LE mic stream (`?after=latest\|N&max_samples=512..8192`); returns `423 Locked` while muted |
| `POST` | `/api/companion/audio/out` | Token + Control + Lease | Sequenced 24 kHz mono PCM16 LE speaker upload (`?seq=N&end=0\|1`, monotonic `seq`, idempotent retry of immediate previous `seq`) |

---

## Built-in voice tools (24 declared)

Defined in `main/main.c` (`DEVICE_TOOL_DECL_COUNT == 24U`) and routed by `components/jr_core/src/tool_policy.c`:

### 18 instant offline tools (0 ms, no gateway required)

| Tool | Arguments | Behavior |
|---|---|---|
| `current_time` | *(none)* | Current SNTP-backed local time |
| `weather_here` | *(none)* | On-device cached forecast (temp, feels-like, hi/lo, humidity, wind) |
| `set_timer` | `seconds?` (`5..43200`), `label?` | Local countdown timer (deep-sleep surviving via RTC snapshot; `'focus'` defaults 25 m, `'break'` 5 m) |
| `cancel_timer` | `label?` | Cancel by label, `'all'`, or nearest timer |
| `list_timers` | *(none)* | List active & ringing timers with remaining seconds |
| `start_focus` | `minutes?` (`1..720`, def 25), `label?` | Start Pomodoro focus timer and switch glass to `WATCH` |
| `stopwatch` | `action` (`start\|stop\|lap\|reset\|status`) | On-device chronograph with up to 4 laps |
| `add_note` | `text` (≤ 64 chars) | Pin a durable NVS desk note (max 6) |
| `list_notes` | *(none)* | Read all pinned NVS desk notes |
| `clear_notes` | `target?` (`'all'`, ID, or keyword) | Remove pinned NVS desk notes |
| `daily_briefing` | *(none)* | Live summary: date, weather, 12 h rain outlook, notes, timers, tasks, battery |
| `set_glass` | `screen?`, `watch_style?` | Switch ring screen and/or persist any of the 8 watch dials |
| `device_status` | *(none)* | Live battery, USB/charge, RSSI, die temp, levels, watch style, mute, uptime, timers |
| `set_volume` | `level` (`10..100`) | Persist speaker volume with on-glass receipt |
| `set_brightness` | `level` (`10..100`) | Persist mood-capped brightness ceiling with on-glass receipt |
| `set_ui_sounds` | `enabled` (`bool`) | Toggle and persist subtle touch feedback cues |
| `set_sunlight` | `enabled` (`bool`) | Toggle CO5300 HBM (`0x66`; 5 min auto-revert; refused if chip > 65 °C or battery < 25% off USB) |
| `ask_user` | `question`, `options[]` | Render interactive rim-arc choice card on glass; returns user's physical tap |

### 6 JarvisMCP gateway tools (persistent HTTPS keep-alive)

| Tool | Behavior & policy |
|---|---|
| `recall_memory(query)` | Search Jarvis memory |
| `remember(note)` | Save a bounded plain-text note into Jarvis memory |
| `search_tools(query)` | Discover server capabilities with canonical method & parameter order |
| `execute_tool(tool, args_json)` | Run a read/compute method through the device-enforced allowlist (and server policy on `/device/v1/invoke`), projected to ≤ 3 KB |
| `delegate_task(goal)` | Queue a multi-step job (≤ 600 chars) on the coordination board (`project_id` configurable via `/api/tools/config`) |
| `delegated_tasks()` | List queued, running, done, or blocked delegated jobs |

Provision the gateway via `POST /api/tools/config` with `{"url":"https://gateway.example/device/v1/invoke","key":"<dedicated-device-key>","project_id":"jarvisnano-desk"}` (HTTPS `/device/v1/invoke` preferred; legacy `/act` supported with device-side allowlist). Send empty strings to clear. `GET /api/tools/config` never returns secret values.

---

## Physical-state visual invariants

- **Listening halo:** breathing / DAC-synced cyan ring (`r186–193`)
- **Battery rim:** `r215–220`
- **Physical privacy mute:** persistent gold ring (`r221–222`) — microphone frames are zeroed before VAD and uplink
- **Choice / Ask arcs:** `r223–231` (disjoint from battery and privacy rings)
- **Codex / ZeroChat lease:** violet companion rim; physical double-tap exits unconditionally
- **OTA probation:** violet update ring until health confirmation passes (45 s eligible, 120 s hard rollback deadline)
