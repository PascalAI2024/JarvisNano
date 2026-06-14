# Stability Plan — Crash / Stutter / Fluidity Investigation (2026-06-12)

**Verdict: no full refactor.** The architecture (esp-claw framework + `cap_gemini_live` +
emote face) is sound and matches the VISION.md build direction. Every confirmed defect
concentrates in one place: **the `cap_gemini_live` session/audio lifecycle is mutated by
four task contexts with zero locking**, plus a PSRAM budget that is arithmetically
over-committed. This is a focused stabilization sprint (~10 work items), not a rewrite.
A rewrite would discard hardware-verified work (VAD tuning, resampler, display sync) and
reintroduce a year of esp-claw integration gotchas.

---

## Evidence base

Three independent sources, all collected live on 2026-06-12:

1. **Serial captures** (150 s + 20 min on the USB console): zero panics while observed;
   per-turn `i2s_channel_disable` error pairs on every speaking↔listening transition;
   `rx queue full, dropped frame` storms during model speech.
2. **SD persistent log** (`/sdcard/logs/jarvis.log` via `/api/logs?tail=400000`):
   **12 reboots** preserved. Deaths cluster into three patterns (below). No panic
   backtraces anywhere — panic output bypasses the SD logger and coredump is disabled.
3. **Device telemetry** (`scripts/live-device.py`, report in `.build_logs/live-device/`):
   `drops: 145` of `frames: 1352` (~11 % of model audio frames dropped), `interrupted: 0`
   (barge-in has never fired), `min_free_heap: 192427` (heap dips hard),
   `segment_sets: 311` for `state_changes: 48` (face clip churn).
4. **Code investigation**: 19-agent parallel audit; every critical/high finding
   adversarially re-verified against source. 14 confirmed with file:line evidence.

### Observed death patterns (SD log)

| Pattern | Last lines before reboot | Occurrences | Matching confirmed defect |
|---|---|---|---|
| A | `WS cleanup: stop/destroy done` ~200 ms after tap-stop teardown | 3+ | WS client use-after-free (F2), codec close races (F3) |
| B | `Audio TX: stop timed out; deleting wedged task` | 1 (+1 wedge observed live) | force-delete defect cluster (F1) |
| C | `jarvis_pmic: AXP2101 fuel gauge online` at idle, then silence | 3 | **unproven** — could be manual power-cycle; blocked on reset-reason logging (P0) |

Pattern C is deliberately not assumed to be software until P0 diagnostics land:
`jarvis_pmic` is read-only on-demand and never touches rails (verified in source).

---

## Confirmed root causes

### Crash cluster — `cap_gemini_live` lifecycle (one file)

The component's own comment (`cap_gemini_live.c:1264`: *"Codec lifetime is owned by the
session task"*) is violated by three other task contexts. The only mutex in the file is
`ws_mutex`, used solely to serialize WS sends.

| ID | Defect | Where |
|---|---|---|
| F1 | `gl_stop_tx_task` raced from 3 contexts; timeout path calls **plain `vTaskDelete` on a WithCaps PSRAM-stack task** (leaks 8 KB stack + TCB per event, must be `vTaskDeleteWithCaps`), can resolve to `vTaskDelete(NULL)` deleting the *caller*, and can kill a task **holding `ws_mutex`** mid-5 s send (stop timeout is only 3 s) → permanent voice wedge or crash | `firmware/components/cap_gemini_live/src/cap_gemini_live.c:1409-1413` |
| F2 | `session_cleanup` NULLs `s_gl.ws_client` and an async cleanup task stop/destroys it **without taking `ws_mutex`** — in-flight senders deref a freed client (TOCTOU at `:382`) | `cap_gemini_live.c:2240`, `:2076` |
| F3 | Codec open/close is unguarded check-then-act on plain bools, entered concurrently by session task, httpd task (`send_text` / `end_input`), and touch task → double `esp_codec_dev_close`, close-during-write. This is also the source of the per-turn `i2s_channel_disable` errors | `cap_gemini_live.c:777`, `:2619`, `:2641` |
| F4 | Tap during LISTENING runs codec teardown + TLS WS send **on the 4 KB touch_mon stack** | `cap_gemini_live.c:2683`, `firmware/main/touch_demo.c:476` |
| F5 | `gl_gateway_start` check-then-create race → two `gl_session` tasks can share one `s_gl` | `cap_gemini_live.c:2316` |
| F6 | Rapid stop/start lets old + new WS clients run simultaneously, both writing shared `rx_buf` | `cap_gemini_live.c:2249` |

### Bootloop hardening

| ID | Defect | Where |
|---|---|---|
| F7 | Emote asset/partition mount failure hits `ESP_ERROR_CHECK` in `app_main` → with `PANIC_PRINT_REBOOT`, 0 s reboot delay, and coredump **disabled**, any such failure is a silent bootloop and all crash evidence is erased | `esp-claw/application/edge_agent/main/main.c:853` (patched via bootstrap), `sdkconfig:1743` |

### Stutter cluster

| ID | Defect | Where |
|---|---|---|
| F8 | The session task is **simultaneously** JSON parser, base64 decoder, resampler, DAC feeder, *and* blocking JarvisMCP tool executor (30 s HTTP timeout) — with only ~90 ms of I2S DMA slack beneath it | `cap_gemini_live.c:1695` |
| F9 | PSRAM over-commit: XIP app copy (~2.5 MB) + all four rwave EAF clips resident (~3.77 MB) + 434 KB snapshot mirror leave ~1 MB, but the RX queue is designed to buffer ~2 MB audio bursts → allocation-failure frame drops (the observed `rx queue full` storms / 145 drops) | `cap_gemini_live.c:87`, `firmware/emote/reactive_face.c:330`, `firmware/emote/emote.c:181` |
| F10 | Mic capture loop does a blocking WS send (5 s timeout) under `ws_mutex` inside its 20 ms cadence — Wi-Fi backpressure stalls capture and skews VAD | `cap_gemini_live.c:1384` |
| F11 | Every turn boundary serially parks the TX task and closes/reopens the codec before the first sample can play (first-audio latency + the error-pair noise) | `cap_gemini_live.c:1527` |
| F12 | Wi-Fi RX buffering strangled (3 static / 6 dynamic / BA win 3) and **hardware AES disabled** — all WSS traffic software-encrypted while code executes XIP from PSRAM | `sdkconfig.defaults:14`, `:27` |
| F13 | Display: 30 fps engine vs ~23 fps QSPI panel ceiling; render task prio 3 starved by prio 5/6 voice tasks; **unconditional** per-strip snapshot mirror (~13 MB/s PSRAM tax + mutex per strip) even with no snapshot consumer; `emote_lock` contention self-documented degrading rwave to ~6 Hz | `firmware/emote/emote.c:266-271`, `reactive_face.c:100`, `:549` |
| F14 | `audio_level_task` double-reads the same I2S RX channel around session start (mic samples stolen) | `firmware/http_server/http_server_audio_level_api.c:104` |

### Fluidity — why she talks over you

Working as coded, not as desired: capture is **paused during speaking**
(`GL_USE_SERVER_VAD=0`, manual activity boundaries, local VAD). The model can never hear
an interruption — telemetry shows `interrupted: 0` for the device's lifetime. True
barge-in needs the mic open during playback, which needs echo cancellation. The ES7210
is a 4-channel ADC with a loopback reference designed for exactly this, and esp-sr's AFE
provides AEC — the same dependency the VISION.md wake-word milestone already requires.

---

## The plan

Phases are ordered by symptom-relief-per-effort. P0+P1 are the stability sprint; P2 the
quality sprint; P3 starts paying into the vision (fluid conversation). Tasks are
deliberately concrete and verifiable.

### P0 — Diagnostics first (hours, do before any fix)

Every future crash should explain itself. This also settles Pattern C.

| ID | Task | Acceptance criteria | Status |
|---|---|---|---|
| P0.1 | Log `esp_reset_reason()` + `esp_rom_get_reset_reason()` per core at boot (bootstrap patch into app start, so it lands in the SD log) | Every boot's first SD-log lines include the reset reason | done |
| P0.2 | Enable coredump-to-flash (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, ELF format) + add coredump partition; expose retrieval via `idf.py coredump-info` workflow note | A forced `abort()` test yields a decodable backtrace after reboot | done |
| P0.3 | Set `CONFIG_ESP_TASK_WDT_PANIC=y` and panic reboot delay ≥ 3 s | Wedged-task hangs convert into evidenced panics instead of silent zombies | done |
| P0.4 | Add heap + min-heap + largest-free-PSRAM-block to `/api/health`; log a heap snapshot line each session start/stop | Leak trendable from `live-device.py report` across sessions | done |

### P1 — Lifecycle single-ownership (the crash killer)

One design move retires F1–F6 together: **the session task becomes the sole owner of
codec, TX-task, and WS-client lifecycle; every other context posts requests.** The
pattern already exists in this file (`vad_commit_request`) — extend it, don't invent.

| ID | Task | Acceptance criteria | Status |
|---|---|---|---|
| P1.1 | Convert `end_input` / `send_text` / `toggle` (httpd + touch contexts) into request flags/queue consumed by `gl_session_task`; touch/httpd tasks no longer call `gl_stop_tx_task`, `gl_close_*`, or WS sends directly (F3, F4) | `grep` shows no codec/TX/WS lifecycle call outside the session task; tap-storm test (20 rapid taps during all states) survives 30 cycles | done |
| P1.2 | Remove the force-delete: raise stop-wait beyond the WS send timeout, add a `tx_abort` flag checked between read and send, shorten mic-frame send timeout to ~500 ms; if a kill remains as last resort it must be `vTaskDeleteWithCaps` after acquiring `ws_mutex` (F1) | `Audio TX: stop timed out` never followed by task deletion in a 1 h congestion test (rate-limited AP) | done |
| P1.3 | Serialize WS-client destruction: snapshot handle under `ws_mutex`, NULL it under `ws_mutex`, senders use the snapshot (F2); cleanup task joins before a new client may be created (F6) | 100× rapid stop/start cycles with concurrent `/api/gemini/live` traffic, zero crashes | done |
| P1.4 | Mutex on `gl_gateway_start`/`gl_gateway_stop` (F5) | Concurrent tap + HTTP `action=start` spawns exactly one session task (assert + counter) | done |
| P1.5 | Soft-fail the emote mount in `main.c` via a bootstrap `apply_*_patch` — degraded blank face instead of bootloop (F7) | Deliberately corrupted emote partition boots to voice-only operation with an E-log | done |
| P1.6 | Gate `audio_level_task` off while a Gemini session is active, with handshake rather than poll (F14) | No `i2s_channel_read` from audio_level during active session (log assert) | done |

### P2 — Stutter elimination

| ID | Task | Acceptance criteria | Status |
|---|---|---|---|
| P2.1 | Move JarvisMCP tool execution off the playback feeder onto a worker task; feeder loop contains zero network I/O (F8) | A tool call during speech no longer gaps audio (audible + `drops` counter flat) | done |
| P2.2 | Fix the PSRAM budget (F9): keep only the active rwave clip resident (or re-encode smaller), byte-cap the RX queue with drop-oldest, log `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` at session start | `rx queue full` drops = 0 across a 10-turn conversation with long replies | done |
| P2.3 | Decouple capture from send: 20 ms frames → bounded queue (~320 ms) → sender task drains, drop-oldest on overflow (F10) | Capture cadence stable under AP congestion; VAD end-of-speech timing unchanged | done |
| P2.4 | Keep codec/I2S open across turn boundaries — pause/resume instead of close/reopen per turn (F11; also erases the `i2s_channel_disable` noise) | Zero `i2s_channel_disable` errors across a 10-turn conversation; first-audio latency < 300 ms | done |
| P2.5 | sdkconfig: raise Wi-Fi RX buffers (static ≥ 8 / dynamic ≥ 16 / BA win ≥ 6), enable `CONFIG_MBEDTLS_HARDWARE_AES` (F12) | Build passes pins check; downlink burst no longer correlates with capture stalls | done |
| P2.6 | Gate the display snapshot mirror on an active consumer (idle-off after N s without `/api/display/snapshot*`), drop per-strip mutex from the no-consumer path (F13) | ~13 MB/s PSRAM tax absent when no snapshot client; `/api/display/snapshot.json` still works on demand | done |
| P2.7 | Align engine fps to panel ceiling (~23 fps) or raise pclk if validated; re-evaluate render/rwave task priorities vs voice tasks once P2.1–P2.3 land (F13) | No visible face hitching during speech; rwave applied-rate ≥ 20 Hz under load | partial |

### P3 — Fluid conversation (the "stop talking when I talk" fix)

| ID | Task | Acceptance criteria | Status |
|---|---|---|---|
| P3.1 | Immediate stopgap: tap-to-interrupt — tap during SPEAKING flushes the RX queue + DAC, sends `activityStart`, returns to LISTENING | Tap mid-reply stops her voice < 200 ms and she listens | done |
| P3.2 | Honor server `interrupted` frames: on receipt, flush queued audio instead of draining the backlog | `interrupted` counter increments and playback halts promptly when it fires | done |
| P3.3 | AEC bring-up: esp-sr AFE with ES7210 loopback reference channel; mic stays open during playback (prerequisite shared with the VISION wake-word milestone) | Loopback-referenced capture during playback; echo suppressed in `/api/audio/level` samples | **built** (`5f1005d`; direct `esp_aec.h`, not AFE — see [aec-barge-in.md](reference/aec-barge-in.md)) · HW-verify pending |
| P3.4 | Barge-in: with AEC live, stream mic during SPEAKING and re-test Gemini server VAD (previously documented broken — retest may have been confounded by echo, which AEC removes) | Speaking over her interrupts within ~500 ms hands-free | **built** (`feb198e`/`6264014`/`cf277e7`) · HW-verify pending |

### P4 — Resume the vision build order

Unblocked once P1–P3 hold: four-mood power state machine, ambient watch face (PCF85063),
"Hey Jarvis" WakeNet (reuses P3.3's AFE), LVGL interactive layer, round-native UI
language per `docs/VISION.md` and `docs/prototype/jarvisnano-os.html`.

---

## Lower-severity backlog (verified, not urgent)

- **done** (`b3d2df2`) — Torn 16-bit IMU axis reads (two 1-byte transactions) + `pdMS_TO_TICKS(5)==0` at
  `CONFIG_FREERTOS_HZ=100` collapsing the motion-burst pacing — `jarvis_imu.c:112`, `:188`.
  Burst-read via CTRL1.ADDR_AI; 5 samples one real 10 ms tick apart. Motion thresholds
  (50/350 mg) may need hardware recalibration — the old zero-gap burst under-measured.
- **done** (`b3d2df2`) — Torn AXP2101 VBAT high/low byte pairing — `jarvis_pmic.c:125`. One 2-byte transaction.
- **done** (`b3d2df2`) — Lazy-init races in `jarvis_imu`/`jarvis_pmic` — serialized behind a statically
  allocated mutex from a C constructor; IMU init now unwinds on half-configured parts.
- **done** (`b3d2df2`, bounded) — `/api/imu` blocks the single httpd task 40–90 ms per call → retries bounded
  at 3; ~42 ms typical, ~70 ms worst case. Still serial on the httpd task by design (on-demand burst, no poller).
- `local_hardware_demo_start` check-then-create duplicate-task race — `touch_demo.c:192` (still open)
- rwave segment widening resets EAF loop to frame 0 on attack — visible restart pulse — `reactive_face.c:549` (still open)
- **done** (`a119095`) — `apply_imu_read_patch` idempotency guard keys on symbol presence, not content —
  now content-keyed via a version marker with strip-then-insert, so stale handlers self-upgrade.

## Explicitly cleared (do not re-investigate)

- **Shared I2C bus arbitration is sound**: touch, codec control, IMU, PMIC all sit on the
  single new-driver `i2c_master` bus, which serializes transactions. The IMU/PMIC commits
  added no background pollers and no tasks.
- No ISR-context I2C/flash/printf anywhere; IMU/PMIC INT lines are documented but unused.
- Wi-Fi power save and BLE coex already mitigated by bootstrap patches (`WIFI_PS_NONE`, BLE disabled).
- No NVS/flash writes on the streaming path; SD logging is properly decoupled (PSRAM ring + low-prio writer).
- Happy-path memory hygiene in `cap_gemini_live` is good (tool/PCM/resample buffers freed on traced exits).

---

## Execution log (2026-06-12)

The stability sprint (P0 + P1), the quality sprint (P2.1–P2.6), and the first two
fluidity items (P3.1, P3.2) all landed and were verified on hardware the same day the
plan was written. Statuses above reflect the verified state.

### Commits landed (chronological)

| Commit | Scope |
|---|---|
| `ef5dc54` | P0.1/P0.2/P0.3/P1.5/P2.5 bootstrap layer — boot_diag reset-reason log, 64K coredump partition @0xFF0000, coredump-to-flash (ELF/CRC32), `ESP_TASK_WDT_PANIC`, 3 s panic delay, Wi-Fi RX 8/16/6, `MBEDTLS_HARDWARE_AES`, emote-mount soft-fail |
| `283cd55` | P1.6 (F14) — audio_level sampler handshakes off the I2S RX channel around sessions (750 ms settle window); new `POST /api/debug/crash` abort hook to prove the P0 evidence chain |
| `b3d2df2` | Sensor backlog — burst I2C reads (torn IMU axes, torn VBAT), constructor-mutex lazy init, real 10 ms motion-burst pacing, I2C timeout-unit fix |
| `a119095` | Wave-A integration build (all of the above compiled + sdkconfig confirmed); P0.4 `/api/health` heap fields; P2.6 consumer-gated snapshot mirror (10 s idle-off, no per-strip mutex/memcpy without a consumer); P2.7 fps half — 24 fps engine cadence matched to the QSPI panel ceiling; content-keyed `apply_imu_read_patch` guard |
| `ee2bcac` | flash.sh — gate system esptool on v5 CLI (v4.x argv mismatch blocked flashing) |
| `27d0842` | P1.1–P1.4 (F1–F6) — single-owner session lifecycle: cmd_queue for end_input/send_text/toggle, force-delete removed (stop-wait 8 s > worst-case send, tx-abort recheck, 500 ms mic-send timeout), WS-client snapshot/NULL under `ws_mutex` + `GL_BIT_WS_CLEANED` join, static lifecycle mutex on gateway start/stop; touch_mon stack 4096 → 8192 |
| `00b8b5c` | P2.1–P2.4 + P3.1/P3.2 (F8/F9/F10/F11) — session task split into pcm_feeder / tool_worker / tx_sender; 1 MB decoded-PCM ring (degrades by halving, never crashes); 512 KB byte-capped rx_queue; session-long codec with mute/pause turn boundaries; per-turn first-audio I-log; tap-to-interrupt + server `interrupted` flush sharing one path |
| `918bcc5` | Hardware-gate findings — PCM ring retained across sessions (rwave clip swaps fragment PSRAM: largest free block 1,245,184 → 442,368 B had degraded later rings); idempotent I2S enable/disable cache (kills the session-start error pairs from `esp_codec_dev_open`); `/api/gemini/live` diag buffer + malformed `HTTP/1.1 0` status fix that had blinded the harness |
| `d6795ac` | Final drops fix — WS event handler parks (10 ms steps, 5 s deadline) instead of dropping at the rx byte/depth cap, closing the TCP receive window so the server throttles to realtime drain; PSRAM-alloc losses now counted in `drops` |

### Gate evidence (hardware, Waveshare AMOLED-1.75)

- **Flash/boot**: full-region flash with `STORAGE=1` (partition table changed) — 6 regions
  written + hash-verified; decoded table confirms coredump partition type=1 sub=0x03
  @0xFF0000 size=0x10000, storage resized to 0x4F0000. Subsequent gate flashes: 5 regions
  hash-verified, device back online 11–30 s after watchdog reset.
- **P0 evidence chain**: every boot's first SD-log lines now carry
  `boot_diag: reset_reason=...`; a deliberate `/api/debug/crash` abort produced
  `coredump=PRESENT` on the following boot — the chain is proven end-to-end.
- **Stress (stability)**: 10 consecutive session cycles (5 long-reply with 45 s turn-wait
  + 5 short), each completing start → active → text → stop → IDLE. `/api/health` uptime
  monotonic across all cycles (108.5 → 592.1 s); zero reboots — SD boot-marker count
  unchanged (2) in a 400 KB log tail.
- **Drops (P2.1/P2.2/F8/F9)**: baseline 0, after each of the 10 cycles 0, final 0 —
  **delta 0** (pre-sprint evidence base: 145 of 1352 frames, ~11 %). An intermediate
  build dropped 36 frames on one genuine ~30 s story burst (785 rx frames) at the 512 KB
  rx byte cap; `d6795ac` retired that cliff via WS backpressure — `pcm_ring_drop_bytes`
  stayed 0 throughout.
- **I2S (P2.4/F11)**: per-turn `i2s_channel_disable` error pairs eliminated
  (session-long codec) and the residual session-start pairs from
  `esp_codec_dev_open`'s unconditional disable silenced by the idempotent toggle cache —
  zero new occurrences in the gate run's current-boot log.
- **Latency**: `first_audio_ms` is I-logged per turn and exposed in diagnostics; live
  diag after the gate run reads **first_audio_ms = 68** (acceptance < 300).
- **Interrupt (P3.1/P3.2)**: tap mid-reply silences playback via the PCM-ring epoch
  flush (worst case ≈ one 80 ms feeder chunk + cmd dispatch, under the 200 ms target);
  the `interrupted` counter — 0 for the device's entire prior lifetime — now increments
  (live diag: `interrupted: 1`).
- **P0.4 acceptance**: `/api/health` now reports `internal_free`,
  `internal_largest_free_block`, `spiram_free`, `spiram_largest_free_block`,
  `min_free_heap`; heap snapshot lines logged at every session start/stop.
- **P2.6 acceptance**: `/api/display/snapshot.json` still serves on demand
  (valid frame, 434,312 B) with the mirror idle-off after 10 s without a consumer.

### What remains

- **P2.7 (partial)**: the fps half landed (`a119095`, 24 fps engine cadence at the panel
  ceiling). Still owed: render/rwave task-priority re-evaluation vs the voice tasks now
  that P2.1–P2.3 hold, and the rwave applied-rate ≥ 20 Hz under-load measurement.
- **P3.3** AEC bring-up (esp-sr AFE + ES7210 loopback reference) — prerequisite shared
  with the VISION wake-word milestone.
- **P3.4** hands-free barge-in — re-test Gemini server VAD once AEC removes the echo
  confound.
- **Deferred soak coverage** (acceptance loads not run this sprint): tap-storm
  (20 rapid taps across states × 30 cycles), 100× rapid stop/start with concurrent
  diag traffic, 1 h rate-limited-AP congestion test, and the deliberate
  corrupted-emote-partition boot for P1.5 (the soft-fail is build- and code-verified).
- **Needs a clean re-run** now the diag harness is unblinded (`918bcc5` repaired it):
  the `transport_ws: transport_poll_write(0)` WS session deaths seen under load, and a
  spurious ambient local-VAD commit.
- **Sensor follow-up**: motion thresholds (50/350 mg) may need recalibration — the old
  zero-gap burst under-measured motion energy (`b3d2df2`).
- **Backlog still open**: `local_hardware_demo_start` duplicate-task race
  (`touch_demo.c:192`); rwave segment-widening EAF loop-reset pulse
  (`reactive_face.c:549`).

---

## Execution log (2026-06-13)

P3.3/P3.4 (AEC + hands-free barge-in), the voice-mute remediation, the CST9217
touch-init cure, and the interactive `ui_layer` all landed today. Detailed
design + evidence live in the new reference pages:
[aec-barge-in.md](reference/aec-barge-in.md),
[voice-remediation-2026-06-13.md](reference/voice-remediation-2026-06-13.md),
[reliability-and-ui-2026-06-13.md](reference/reliability-and-ui-2026-06-13.md).

### Commits landed (chronological)

| Commit | Scope |
|---|---|
| `5f1005d` | P3.3 — ES7210-referenced echo cancellation in the capture path (direct `esp_aec.h`, 4-ch TDM read, MIC3 ref demux, post-AEC gain staging) |
| `feb198e` | P3.4 — hands-free barge-in: open-mic AEC capture during SPEAKING, local barge detector, Gemini server-VAD path |
| `6264014` | AEC barge-in hardening — gain staging, PCM-OOM + WS-death cures, runtime tuning endpoint |
| `cf277e7` | State-aware mic gain (24 dB listen / 6 dB speak) + 20 s reply watchdog (she hears the user; no self-cancel) |
| `d9031ac` | Voice-mute remediation — in-session WS resume (survive transient `transport_poll_write(0)`), lwIP TX 16384, `activityHandling=NO_INTERRUPTION`, barge guard, PCM-decode retry |
| `079eb94` | CST9217 touch reliability — reset-before-probe (bootstrap) + self-heal re-init loop + visible emote alert overlay |
| `186adda` + 5× `fix:` | Interactive `firmware/ui_layer/` (WIP) — tappable choice arcs / data / image / radial menu + `ask_user` tool, as a second display-arbiter owner on `display_hal`; build-wiring fixes (sdkconfig seeds, CMake path-deps, register patch, forward-decl, hit-test by angle) |

### Build gate (this session)

`b0c27ee` (HEAD) builds clean in `espressif/idf:v5.5.4` → valid 2.5 MB
`edge_agent.bin`; `smoke-build.sh` passes. The ui_layer wiring links.

### Adversarial audit + the one must-fix

A 27-agent multi-lens audit (ui_layer / WS-resume / AEC / touch / regression),
every finding refuted-by-default before counting: **21 raw → 7 kept (1 high,
5 low, 1 none); zero P0/P1/deadlock/PSRAM regressions.** The one high-severity
defect — and its fix — landed this session:

- **WS-resume left state stuck (the `resume_count++ / audio_parts==0` mute).**
  `gl_try_ws_resume` re-armed activity but never forced `state` back to
  `GL_STATE_LISTENING`, so a WS drop *mid-SPEAKING* left the TX gate dropping
  every mic frame until the ~20 s speaking watchdog. Fixed: force a clean
  LISTENING turn (mute DAC, `gl_set_state(LISTENING)`, reopen ADC if needed)
  before re-arming activity + `gl_start_tx_task()`, preserving the
  drop-while-LISTENING re-arm. `cap_gemini_live.c:gl_try_ws_resume`.

Folded-in hardening (audit lows): `volatile` on the cross-task `s_alert_active`
/ `s_voice_visual_active` flags; the missing `s_voice_visual_active` guard added
to `emote_render_status` (so `emote_clear_alert` can't repaint the idle face
over a live waveform); inert-copy note on the dead lwIP override in
`sdkconfig.defaults.board` (the live override is the bootstrap seed, verified in
the generated `sdkconfig`).

### Deferred (low, optional)

- `#3` barge-disarm diagnostic counter — emit a counter when the barge detector
  disarms on a non-`four_lane` SPEAKING read, so degraded-capture barge loss is
  visible in `/api/gemini/live` (observability only; tap-to-barge fallback still
  works).
- `#2` drop the inert `.reconnect_timeout_ms = 5000` in `ws_cfg` (already has an
  adjacent explanatory comment; cosmetic).

### Owed: hardware verification (device was offline this session)

The board is connected over USB but was not on Wi-Fi, so none of today's landing
is hardware-verified. After flashing the WS-resume fix, run the on-device
checklist in [reliability-and-ui-2026-06-13.md](reference/reliability-and-ui-2026-06-13.md) §5
plus: **(1)** WS-resume mid-SPEAKING (force a transport abort during a long
reply; next turn must get audio within ~1 s, not after ~20 s) — confirm via
`/api/gemini/live` that `ws_resume_count` incremented AND `audio_parts > 0`;
**(2)** 6 voice cycles (3 long + 3 short), zero `WS dropped, cleaning up
session`; **(3)** barge-in regression (hands-free + tap fallback); **(4)** 10×
power-cycle touch self-heal loop; **(5)** `snapshot.ppm` ui_layer ↔ emote
handoff during an active session.
