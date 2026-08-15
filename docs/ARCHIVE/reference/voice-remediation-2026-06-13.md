# Voice Remediation — 2026-06-13 (device is mute)

> Status of the tree at authoring time: ALL patches below are already applied to the
> **working tree** (uncommitted). Braces balance 656/656. `HEAD` already carries some
> of the prior tuning (e.g. `GL_SPEAK_WATCHDOG_MS=20000`) but NOT the dominant cures
> (in-session WS resume, lWIP TX overrides, `activityHandling=NO_INTERRUPTION`, barge
> guard, PCM-decode retry). **Commit the working tree before any build that could reset
> it** — `git stash` / `git reset --hard` would destroy untracked + uncommitted work.

---

## 1. Failure summary — she is mute

The device produces no audio at all. The live `/api/gemini/live` snapshot (captured
2026-06-13) is the smoking gun:

| field | value | meaning |
|---|---|---|
| `state` | `IDLE` | no live session |
| `connected` | `false` | WebSocket is down |
| `audio_parts` | `0` | model never produced audio this session |
| `tx_send_failures` | `19` | more send failures than... |
| `tx_frames_sent` | `15` | ...frames actually sent — the uplink is failing |
| `watchdog_resumes` | `1` | the self-interrupt path fired once |
| `barge_rms_threshold` | `9000` | barge detector armed |
| `barge_hits` | `0` | no barges in the later snapshot |

SD-log census over the captured tail (7 boots, 262 KB, `/tmp/jarvis-ev/logs.txt`):

- **11x** `WS dropped, cleaning up session`
- **11x** `transport_poll_write` (write-0) events
- **29x** `Model interrupted`
- **1x** `no response for ... ms after activityEnd` (the input watchdog)
- **2x** `OOM for PCM decode buf`

The single most recent WS event before the snapshot was a teardown. After teardown,
`disable_auto_reconnect=true` and there was no in-session resume, so the device sat in
IDLE/mute. The "she is mute / not working at all" symptom matches the WS teardown
fingerprint, not the watchdog (which fired only once).

---

## 2. Ranked verified root causes

### RC1 (dominant, current mute) — transient WS write-0 tears the whole session down to IDLE

**Mechanism.** ESP-IDF `esp_websocket_client` treats a transport write that returns `0`
(a benign TCP would-block / poll-write timeout; `errno=0`, `transport_error=ESP_OK`) as
fatal: it calls `esp_websocket_client_abort_connection()` and fires
`WEBSOCKET_EVENT_DISCONNECTED`. The device handler clears `s_gl.ws_connected`, and the
session loop unconditionally broke to `session_cleanup`. With `disable_auto_reconnect=true`
and no in-session resume, **one transient would-block was a one-way trip to mute.**

The write-0 is provoked by the small default lwIP TX buffer (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`
default = 5760 B = 4×MSS, no override on this board) plus the previously-short 500 ms mic
send timeout: a Wi-Fi RTT spike fills the buffer, the poll-write times out, the send
returns 0. The 10 s automatic keepalive PING is also a writer that can hit the same abort
during pure silence.

**Evidence in source (working tree):**
- Teardown loop: `firmware/components/cap_gemini_live/src/cap_gemini_live.c:4029-4047`
  (`if (!s_gl.ws_connected) { ... "WS dropped, cleaning up session"; break; }`)
- `disable_auto_reconnect = true`: `cap_gemini_live.c:3900`
- Mic send timeout: `GL_WS_MIC_TIMEOUT_MS` at `cap_gemini_live.c:230`
- Board sdkconfig had no lwIP TX override (now appended):
  `boards/waveshare/esp32s3_touch_amoled_1_75/sdkconfig.defaults.board:53-54`

**Verified, not assumed.** Live `gemini.json` shows IDLE/disconnected/audio_parts=0
right now; the log census shows 11x teardown + 11x `transport_poll_write`; the most
recent WS line before the snapshot is a teardown. The ESP-IDF abort-on-write-0 behavior
is corroborated by upstream sources (see §4, Topic A).

### RC2 (real, but RARE in this evidence) — input watchdog `activityStart` self-cancels the model's reply

**Mechanism.** In manual VAD mode (`GL_USE_SERVER_VAD=0`, `cap_gemini_live.c:285`), the
`gl_send_setup` manual branch wrote `automaticActivityDetection.disabled=true` but, **at
`HEAD`, never set `activityHandling`** — so it defaulted to
`START_OF_ACTIVITY_INTERRUPTS`. When the THINKING-state watchdog fired, it called
`gl_resume_listening("input watchdog")` → `gl_begin_audio_activity` →
`gl_send_activity_start` which sends `{"realtimeInput":{"activityStart":{}}}`. The Gemini
server reads that as the **user** barging in and cancels the model's pending reply before
any audio → `serverContent.interrupted`, `audio_parts=0`, stuck LISTENING.

**Evidence in source:**
- Manual branch (now fixed): `cap_gemini_live.c:1506-1520` (`else { disabled:true;
  activityHandling=NO_INTERRUPTION }`)
- `HEAD` baseline had the bug: `git show HEAD:...` line ~1500 shows the `else` branch was
  only `cJSON_AddBoolToObject(aad, "disabled", true);` with no `activityHandling`.
- `GL_SPEAK_WATCHDOG_MS` = 20000 (was 4500): `cap_gemini_live.c:254`

**Frequency check.** The log has exactly **1** `no response ... after activityEnd` and
`watchdog_resumes=1` across the whole capture. So this is a genuine self-interrupt path
but NOT the dominant mute — the EVIDENCE DUMP's leading hypothesis overstated its
frequency. It is ranked #2 because (a) `GL_SPEAK_WATCHDOG_MS` was already widened to 20 s
at `HEAD`, mostly disarming it, and (b) the new `NO_INTERRUPTION` setting neutralizes the
remaining self-interrupt for ALL client-activity paths, not just the watchdog.

### RC3 (intermittent polish) — PCM decode-buffer OOM drops one model audio chunk

**Mechanism.** `gl_play_audio_b64` computes `pcm_max = (b64_len/4)*3+4` and
`heap_caps_malloc(pcm_max, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)` per chunk; on NULL it
logged `OOM for PCM decode buf` and returned, dropping that chunk. A 24 kHz native-audio
chunk's base64 is larger than a 16 kHz one, so a transient PSRAM low-water moment (e.g.
just after a ~1 MB rwave-clip alloc, or AEC/feeder churn) can fail the alloc.

**Evidence in source:** alloc + retry at `cap_gemini_live.c:1779-1782`. Logs: 2 OOMs,
each on the FIRST chunk of a turn at `model_rate=24000`, each immediately followed by a
successful `first-audio latency` line — proving it drops one chunk, not a whole turn.

**Refutation of a phantom claim.** This is NOT a PSRAM fragmentation ceiling.
`health.json` shows `spiram_largest_free_block=737280` (720 KB contiguous) and
`spiram_free=901616` — far above any single chunk's `pcm_max`. The decode buffer is
allocated from SPIRAM, so the 31744 B internal-RAM largest-free-block is irrelevant here.
Real but minor; ranked below the two mutes.

### RC4 (stutter risk, not mute) — 24 dB LISTENING mic gain can leak echo into the barge detector

**Mechanism.** The working tree raises `GL_MIC_PGA_DB` 6→24 (`cap_gemini_live.c:194`) so
the local VAD can hear the user, with a state-aware drop to `GL_MIC_PGA_SPEAK_DB=6` on the
SPEAKING edge (`cap_gemini_live.c:195`, applied in `gl_set_state`). Risk: on the
LISTENING→SPEAKING edge the gain drop is an **async** codec control write that does not
land on the very next captured frame, and the AEC is still re-converging — so the opening
SPEAKING frames can carry a loud, not-yet-cancelled echo transient at the higher gain.
If `mic_rms` crosses `barge_thr` (9000) for `GL_BARGE_LATCH_FRAMES`, it latches a phantom
barge and flushes the model's own reply.

**Evidence in source:** barge gate at `cap_gemini_live.c:2468-2489`; AEC p95 cost already
~12 ms over its 10 ms gate (logs) → thin suppression headroom. Logs show 7x
`Interrupt (barge-in)`. This causes a stutter/cut-off, not total silence — hence rank #4.

---

## 3. Patches (apply order)

All are already applied to the working tree. Listed in the order their cures matter.

### P1 — Survive transient WS write-0: in-session resume + prevent the write-0 at the source (cures RC1)

**Files:**
- `firmware/components/cap_gemini_live/src/cap_gemini_live.c`
- `boards/waveshare/esp32s3_touch_amoled_1_75/sdkconfig.defaults.board`

**Changes:**
1. New resume constants `GL_WS_RESUME_ATTEMPTS=3`, `GL_WS_RESUME_CONNECT_MS=8000`,
   `GL_WS_RESUME_SETUP_MS=8000`, `GL_WS_RESUME_BACKOFF_MS=300` (`cap_gemini_live.c:250-253`).
2. `GL_WS_MIC_TIMEOUT_MS` 500 → 1500 (`cap_gemini_live.c:230`) — rides out an RTT spike
   instead of returning a write-0; stays well under `GL_TX_STOP_WAIT_MS=8000`.
3. New diag field `ws_resume_count` (`cap_gemini_live.c:530`, reset at `:630`, emitted at
   `:4478`).
4. New helper `gl_try_ws_resume()` (`cap_gemini_live.c:3770-3836`): reuses
   `s_gl.ws_client` (NO destroy/re-init) — `esp_websocket_client_stop()` → clear
   `GL_BIT_SETUP_OK`+`ws_connected` → `esp_websocket_client_start()` on the SAME handle →
   poll `ws_connected` (8 s) → `gl_send_setup()` → poll `GL_BIT_SETUP_OK` (8 s) →
   re-arm activity → `ws_resume_count++`. Runs in the session task, never the WS event
   handler.
5. Teardown loop replaced with a bounded resume loop (`cap_gemini_live.c:4029-4047`): on
   `!ws_connected`, try `gl_try_ws_resume()` up to 3 times with backoff; only break to
   `session_cleanup` if every attempt fails.
6. `ws_cfg` keepalive PING (`cap_gemini_live.c:3909-3911`): `ping_interval_sec=20`,
   `pingpong_timeout_sec=20`, `keep_alive_enable=true`, keeping
   `disable_auto_reconnect=true` (the explicit resume owns reconnection).
7. Board sdkconfig appended (`sdkconfig.defaults.board:53-54`):
   `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384`, `CONFIG_LWIP_TCP_WND_DEFAULT=16384`. This is
   the CANONICAL source — `bootstrap.sh` regenerates the esp-claw sdkconfig from it, so a
   clean/regenerated build is required for the new lwIP values to take effect.

**Restart-after-stop is safe:** `stop()` leaves the client in `WEBSOCKET_STATE_UNKNOW(0)`
and `start()` only rejects `state >= WEBSOCKET_STATE_INIT(1)`, so `UNKNOW < INIT` ⇒
restart allowed.

**Acceptance:** see §5 step P1.

### P2 — Cure the activityStart self-interrupt: `activityHandling=NO_INTERRUPTION` in manual mode (cures RC2)

**File:** `firmware/components/cap_gemini_live/src/cap_gemini_live.c`

**Change:** the manual-mode `else` branch of `gl_send_setup` now emits
`cJSON_AddStringToObject(ric, "activityHandling", "NO_INTERRUPTION")`
(`cap_gemini_live.c:1519`). Plus build-identity diag fields `speak_watchdog_ms` and
`activity_handling` (`cap_gemini_live.c:4505-4507`) so the orchestrator can confirm the
fix from `/api/gemini/live` without a voice cycle.

**Why NO_INTERRUPTION (not a "passive watchdog"):** the TX sender gate only sends mic
frames when `activity_open` is true, and in manual mode the ONLY path that sets
`activity_open` is `gl_begin_audio_activity` (which sends activityStart). A "passive
resume" that leaves `activity_open=false` would MUTE the user's next turn — strictly
worse. `NO_INTERRUPTION` keeps the full activity lifecycle intact while preventing the
server from cancelling on ANY client activityStart. Local barge-in still works because the
local detector flushes playback + mutes the DAC independently of the server's interrupted
frame.

**Acceptance:** see §5 step P2 (build-identity check is voice-free).

### P3 — Retry PCM decode-buffer alloc once on transient OOM (cures RC3)

**File:** `firmware/components/cap_gemini_live/src/cap_gemini_live.c`

**Change:** new `GL_PCM_DECODE_RETRY_MS=8` (`cap_gemini_live.c:258`); on a NULL alloc,
`vTaskDelay(8 ms)` then retry once before giving up (`cap_gemini_live.c:1779-1782`), with
the failure log now including the requested size: `OOM for PCM decode buf (%u B, retry
failed)`. Memory-neutral (no standing allocation), so it does not refight the AEC PSRAM
budget that forced `GL_PCM_RING_BYTES` to 512 KB. The 8 ms wait runs on the session task,
not the feeder task, so buffered audio keeps flowing.

**Acceptance:** see §5 step P3.

### P4 — 300 ms post-SPEAKING guard window on the local barge detector (mitigates RC4)

**File:** `firmware/components/cap_gemini_live/src/cap_gemini_live.c`

**Change:** new `GL_BARGE_GUARD_MS=300` (`cap_gemini_live.c:348`); the barge gate now
holds the detector OFF until `GL_BARGE_GUARD_MS` past `s_gl.speak_enter_us`
(`cap_gemini_live.c:2475-2478`, `barge_guarded` term added to the gate). Covers the
async gain-drop settle + initial AEC convergence. Leaves the 24 dB listen gain and the
9000 threshold untouched (steady-state user sensitivity unchanged). Tap-to-interrupt is
NOT gated — only the RMS auto-barge — so a deliberate tap still kills playback instantly.

**Acceptance:** see §5 step P4.

---

## 4. Research-backed enhancements (with citations)

### Topic A — ESP-IDF treats `transport_poll_write() == 0` as fatal (RC1 corroboration)

A `0` from the transport poll-write is a **write timeout = TCP send buffer full**, NOT a
socket error (`errno` untouched ⇒ `errno=0`). `base_poll_write()` does a `select()` on
the write set; on a full TX window it returns `0`. `ssl_write()`/`base_write()` then
return that `0` without ever calling `send`. The websocket client treats `ret <= 0` from
the raw send as a hard error and calls
`esp_websocket_client_abort_connection(WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT)` — i.e. tears
the socket down on a transient timeout.

- ESP-IDF issue (IDFGH-2161): `esp_transport_write() returned 0, errno=0` is transmission
  backpressure, not a connection fault — https://github.com/espressif/esp-idf/issues/4316 (fetch-verified)
- `transport_ssl.c` `base_poll_write()` returns 0 on write timeout —
  https://raw.githubusercontent.com/espressif/esp-idf/master/components/tcp_transport/transport_ssl.c (fetch-verified)
- `esp_websocket_client.c` aborts on `ret<=0` —
  https://raw.githubusercontent.com/espressif/esp-protocols/master/components/esp_websocket_client/esp_websocket_client.c (fetch-verified)
- lwIP guide: TCP `SND_BUF` default 4×MSS; per-socket `TCP_SNDBUF`; `TCP_NODELAY` for
  latency; TCPIP task fixed prio 18 —
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/lwip.html (fetch-verified)
- WS client config fields (`ping_interval_sec` def 10, `pingpong_timeout_sec`,
  `keep_alive_*`, `reconnect_timeout_ms` def 10 s) —
  https://docs.espressif.com/projects/esp-protocols/esp_websocket_client/docs/latest/index.html (fetch-verified)

**Optional further hardening (not yet applied):** raise WS `buffer_size` 4096→8192, bump
WS `task_prio` 5→6/7 (below the prio-18 TCPIP task), pin the WS task off the AEC core, and
enable `LWIP_IRAM_OPTIMIZATION`. These reduce scheduling-induced send stalls but are not
required to clear the current mute.

### Topic B — Gemini Live protocol corrections (RC2 corroboration)

- `activityStart`/`activityEnd` are `realtimeInput` sub-fields, valid only when
  `automaticActivityDetection.disabled=true`; both are empty messages —
  https://ai.google.dev/api/live (fetch-verified)
- `ActivityHandling` default = `START_OF_ACTIVITY_INTERRUPTS`: "start of activity will
  interrupt the model's response (also called 'barge in'). The model's current response
  will be cut-off." So a mid-response client `activityStart` IS an interruption —
  https://ai.google.dev/api/live (fetch-verified)
- `serverContent.interrupted`: "a client message has interrupted current model
  generation ... stop and empty the current playback queue"; an interrupted turn goes
  `interrupted → turnComplete` with no `generationComplete` —
  https://ai.google.dev/api/live (fetch-verified)
- Correct text-turn shape (already correct in firmware): `clientContent` with `turns` +
  `turnComplete:true`, `generationConfig.responseModalities=[AUDIO]`. Do NOT wrap a
  text-only turn in `activityStart`/`activityEnd` —
  https://ai.google.dev/gemini-api/docs/live-api/capabilities (fetch-verified)
- Native-audio first-audio latency routinely spikes past 4–5 s, especially with
  `googleSearch` grounding + `functionDeclarations` enabled (both on in this firmware) —
  hence a fixed 4.5 s watchdog that takes a turn-cancelling action is unsafe. Drive turn
  completion off server terminal frames (`generationComplete`/`turnComplete`), and emit
  `activityStart` ONLY from the local barge detector on real speech onset —
  https://discuss.ai.google.dev/t/significant-delay-with-gemini-live-2-5-flash-native-audio/122650 (not fetch-verified)
- Quota exhaustion surfaces as a WS close (often 1011 / RESOURCE_EXHAUSTED) or a `goAway`
  message — NOT as a silent stall. The observed failure is the self-interrupt + WS
  teardown, not quota. Recommend logging the WS close code+reason to rule quota in/out,
  and prefer ONE long-lived session over per-cycle reconnects to reduce RPM pressure —
  https://ai.google.dev/api/live (GoAway; fetch-verified)

### Topic C — AEC suppression beyond ~18 dB (RC4 headroom)

- esp-sr AEC: keep `AEC_MODE_FD_LOW_COST` (linear + nonlinear; the cheaper SR modes are
  linear-only and would LOSE loudspeaker-echo suppression). Cheapest win with near-zero
  added MIPS: raise `nlp_level` `AEC_NLP_LEVEL_AGGR` → `AEC_NLP_LEVEL_VERYAGGR` (effective
  only in FD mode). `filter_length` is the expensive linear-tail lever; `sample_rate` must
  be 16000 —
  https://docs.espressif.com/projects/esp-sr/en/latest/esp32/acoustic_echo_cancellation/README.html (fetch-verified),
  https://raw.githubusercontent.com/espressif/esp-sr/master/include/esp32s3/esp_aec.h (fetch-verified)

### Topic D — 24 kHz native DAC vs 16 kHz shared-clock resample

Keep 16 kHz shared-clock + resample. The ES8311 DAC and ES7210 ADC share one I2S port in
STD duplex (`SOC_I2S_HW_VERSION_1`), so TX and RX cannot hold different clocks — native
24 kHz playback was already tried and produced "deep, garbled, cut-off" audio
(`cap_gemini_live.c:841-849`). esp-sr AEC also locks `sample_rate=16000`. No change.

---

## 5. Ordered execution checklist (orchestrator)

> Pre-step (BLOCKING): `git add -A && git commit` the working tree first — uncommitted
> work is at risk from any clean build that resets the tree. Then build/flash from the
> committed state. Edit only canonical sources; `bootstrap.sh` overwrites esp-claw copies.

**Step 0 — clean build + flash (all patches land together; lwIP needs regen):**
1. `ESP_CLAW_REF=$(cd esp-claw && git rev-parse HEAD) BOARD_VENDOR=waveshare BOARD_NAME=esp32s3_touch_amoled_1_75 scripts/bootstrap.sh build`
2. Before flashing, grep the GENERATED sdkconfig under
   `esp-claw/application/edge_agent/` and confirm
   `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384` and `CONFIG_LWIP_TCP_WND_DEFAULT=16384` are
   present (proves the regen picked up the board override).
3. `idf.py flash --flash-mode dio` (DIO required for the CO5300 QSPI display).
4. Confirm boot.

**Step P2 (build-identity, voice-free — do this first, it's the cheapest gate):**
- `GET /api/gemini/live`. PASS = `"server_vad": false`, `"activity_handling":
  "NO_INTERRUPTION"`, `"speak_watchdog_ms": 20000`, and the new `"ws_resume_count"` field
  is present (=0 at session start). If any differs, the binary predates the fix — reflash.

**Step P1 (the dominant cure — WS survival):**
- Run **6 voice cycles**: 3 long (>20 s replies) + 3 short. After each turn,
  `GET /api/gemini/live`.
- PASS = `state != IDLE` and `connected=true` and `audio_parts>0` after each turn; AND
  `tx_send_failures` stops climbing relative to `tx_frames_sent` (ratio improves vs the
  19>15 baseline).
- Scan the SD log for the whole run. PASS = ZERO `WS dropped, cleaning up session`. If a
  `WS dropped; in-session resume attempt` appears, it MUST be followed by `WS resume OK
  (#N)` and the session must NOT have gone IDLE — that is the success path.
- STRESS: during a long reply, induce Wi-Fi RTT (walk far from AP / microwave near the
  radio). PASS = session keeps streaming OR logs a resume-attempt → `WS resume OK` and
  resumes audio in ~<1–2 s; `ws_resume_count` increments by the number of survived drops.
- FAIL signals: any `WS dropped, cleaning up session`; state stuck IDLE after a turn;
  `ws_resume_count` increments but `audio_parts` stays 0 on the next turn (then revisit
  `gl_send_setup` re-send / activity re-arm in `gl_try_ws_resume`).

**Step P2 functional (self-interrupt cure under load):**
- In the same 6 cycles, include (a) a JarvisMCP tool-call turn that legitimately takes
  >5 s and (b) a grounded/web-search turn with slow first token.
- PASS = every turn reaches SPEAKING with `audio_parts>0`; ZERO `no response for ... ms
  after activityEnd`; ZERO `Model interrupted` that is NOT preceded by a real local
  barge (`Barge-in: speech over playback`); `watchdog_resumes` stays 0.

**Step P4 (barge regression — local barge still works, no self-barge):**
- TEST A (no self-barge): long reply, user SILENT through it and 1–2 s after. PASS = ZERO
  `Barge-in: speech over playback` and `barge_hits` does not increment, especially in the
  first ~300 ms of each turn (the guarded window). Repeat ≥10 turns at out_vol=100.
- TEST B (real barge, fast): user speaks over the reply AFTER the first ~0.5 s. PASS =
  exactly ONE barge, playback flushed, `barge_latency < 250 ms`.
- TEST C (AEC unchanged): compare `aec_atten` before/after — suppression must be
  statistically unchanged (the guard touches only the barge latch, not the AEC).

**Step P3 (PCM OOM polish):**
- Ask several open-ended questions for long 24 kHz replies. PASS = ZERO `OOM for PCM
  decode buf` across the run (a single benign `... retry failed` is the only tolerable
  variant, and should also be absent); no audible clip at the START of any utterance;
  `spiram_largest_free_block` stays well above a single chunk's `pcm_max` (~45 KB for a
  ~60 KB b64 chunk; 720 KB free observed). If `... retry failed` ever appears, bump
  `GL_PCM_DECODE_RETRY_MS` to 16.

**Final census cross-check:** on the new log tail, `tx_send_failures`, `Model
interrupted`, and `Interrupt (barge-in)` counts must all be lower than the pre-patch tail
(11x teardown, 29x `Model interrupted`, 7x `Interrupt (barge-in)`). The previously-observed
pattern (`activityEnd → watchdog → activityStart → Model interrupted → audio_parts=0 →
stuck LISTENING`) must NOT recur on any of the 6 cycles.
