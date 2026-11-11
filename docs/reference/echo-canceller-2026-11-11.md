# Echo canceller, gain staging and voice interruption (2026-11-11)

**What it is** — The path that lets the owner interrupt a reply by voice: the ES7210 microphone PGA while the device speaks, the esp-sr AEC (`espressif__esp-sr` `esp_aec.h`, `AEC_MODE_FD_LOW_COST` today), the uplink make-up gain after it, the echo gate, and the "Jarvis" wake word (WakeNet9 `wn9_jarvis_tts`, `components/jr_wake`) fed while a reply plays.

**How we use it here** — `src_read` in `components/jr_audio/src/jr_audio.c` is the one capture caller: it reads the codec, runs the canceller while playback is pending, applies the make-up and hands the frame to the voice task in `main/main.c`. This page records what the device measured on 2026-11-10/11, what each run-time knob on `POST /api/debug/gain` changes, and what the device run must measure to pick the defaults.

---

## Findings & gotchas

**[2026-11-11] Microphone headroom decides everything.** Gate off, device speaking, "residual" is the canceller output before the 6x make-up (`mic-clean / 6`). The owner's speech reads 75 to 143 rms (p90) at the 24 dB listening gain.

| volume | speak mic gain | ref gain | raw mic rms / peak | residual rms per 2 s window | note |
| --- | --- | --- | --- | --- | --- |
| 100 | 21 dB (old default) | 12 | 6874-12091 / 32768, up to 2008 clipped | saturated | microphone rails; nothing to cancel |
| 100 | 9 | 12 | 3007-3665 / 30000-32767 | 335, 8, 525+ | still at the rail |
| 100 | 3 | 12 | 1166-2301 / 13000-15000 | 91, 2, 66, 2.5 | headroom; 22 to 58 dB |
| 100 | 6 | 24 | 846-2155 / 10500-20300 | 14, 3.5, 102, 18 | 40 to 49 dB typical, one 26 dB window |
| 100 | 9 | 24 | 2426-4124 / 23600-28400 | 155-183 | near the rail again, 24 to 26 dB |
| 80 | 21 | 12 | 1411-4129 / 19000-32768 | 112-138, saturated | clips at times |
| 80 | 9 | 12 | 386-1380 / 4600-15400 | 48, 90, 67, 21 | |
| 80 | 9 | 24 | 606-1049 / 6000-10800 | 6.5, 22, 28, 37 | |
| 70 | 21 | 12 | 817-899 / 11000 | 54-67 | one phantom interrupt |
| 70 | 9 | 12 | 550-970 / 5500-6300 | 7, 4, 25, 6 | 29 to 44 dB |
| 60 | 9 | 12 | 99-290 / 1500-2900 | 2 to 7 | |

Readings: the reference lane never clips; whenever raw peaks stay under about 16000 the canceller (FD_LOW_COST, filter 4, NLP aggressive) reaches 40 dB and more, and near the rail it collapses to 20 dB or nothing; a louder reference lane seems to help (thin evidence); even at its best the residual at volume 100 is about as loud as the owner at the microphone with bursts above, so server VAD cannot be trusted alone at full volume. Phantom server interrupts with the gate off: one each at volume 100/21 dB, 80/21 dB, 70/21 dB, none at the lower gains (one reply per point).

**[2026-11-11] What the build changes, and the knob for each.** All knobs are query parameters on `POST /api/debug/gain` (`gain_get_handler`, `main/http_routes.c:354`), run-time only, not persisted, all echoed in its JSON. Defaults are the values the image boots with.

| knob | default | range | what it does |
| --- | --- | --- | --- |
| `speakmic` | 6 dB (was 21) | 0..36 | PGA while speaking (`GL_MIC_PGA_SPEAK_DB`, `jr_audio.c:86`). Applied on the speaking edge by `jr_audio_set_speaking`. |
| `speakgain` | 100 | 0..400 | Percent of the make-up compensation, below. 0 = none. |
| `ref` | 12 dB (unchanged) | existing | Reference lane PGA (`GL_REF_PGA_DB`). Knob already existed; the device run measures 24. |
| `aecmode` | 5 (FD_LOW_COST) | 0, 1, 3, 4, 5, 6 | Canceller mode, `aec_mode_t` in `esp_aec.h`. 1 = SR_HIGH_PERF, 4 = VOIP_HIGH_PERF, 6 = FD_HIGH_PERF. Others ignored. |
| `aecfilter` | 4 | 1..8 | Filter length. More is more CPU (`aec_us` in the reply). |
| `echogate` | 1 | 0/1 | Existing. 0 disables the gate entirely. |
| `echovol` | 0 | 0..100 | The gate applies only when the codec volume is above this. 0 = at every volume with sound, as before. |
| `wakebarge` | 1 | 0/1 | Feed "Jarvis" while speaking and cut the reply on a hit. |

Reply JSON adds `speakgain`, `echovol`, `wakebarge`, `aecmode`, `aecfilter`, `aec_us` (`jr_audio_last_aec_us()`), `aec_rebuilds`, `aec_rebuild_failures`. `/api/cockpit` `voice` gains `wake_cuts`.

**[2026-11-11] Make-up gain after the canceller.** `jr_uplink_makeup_q8` (`components/jr_core/src/echo_gate.c`) returns `6x * (1 + (r - 1) * speakgain/100)` with `r = 10^((listen_db - applied_db)/20)`, clamped to 64x (`JR_UPLINK_MAKEUP_MAX_Q8`); `soft_gain` keeps its 28000 knee. 24 dB listening against 6 dB speaking is 7.94x, so 47.7x in total at `speakgain=100`. `applied_db` is the PGA the codec last accepted (`s_applied_mic_db`, set in `apply_in_gains` only after a successful write), so the make-up changes on the first frame after the PGA write, not on the `s_capture_speaking` flag; the exact conversion frame inside the ES7210 is not knowable. The published `clean_rms` (`jr_audio_clean_rms`) stays pre-gain, so while speaking it reads about 18 dB lower than before and `vadclean` thresholds see a quieter signal.

**[2026-11-11] The dead-microphone watch does not judge while speaking.** At 6 dB a quiet moment inside a reply can read under `JR_MIC_DEAD_RMS` 3.0 on a healthy lane. `jr_mic_watch_meant_open(mic_enabled, sleeping, speaking)` (`components/jr_core/src/mic_watch.c`) is the "meant open" input, with `speaking` = `s_capture_speaking` (SPEAKING phase or playback pending). The 4 s window starts again when the reply ends, so a lane that is dead after the reply is still found.

**[2026-11-11] Canceller rebuild.** `jr_audio_request_aec` only stores the request in atomics (httpd). `aec_apply_pending` runs at the top of `src_read` under the ADC guard, between frames: destroy, `aec_build` (internal RAM first, PSRAM fallback, chunk size must equal 512), and on failure rebuild the previous configuration; if that fails too the capture runs un-cancelled (same pass-through as an init failure). The log line `aec now mode=.. filter=..` carries the rebuild time and the largest internal block before and after. The filter restarts from zero: ignore the first second after a change. The voice task is blocked for the rebuild, once per change.

**[2026-11-11] Echo gate by volume, and the wake pass.** `jr_echo_gate_applies(enabled, volume, threshold)` is `enabled && volume > threshold`; main.c passes `jr_audio_out_vol()` and `echovol`. `jr_echo_gate_pass_for` opens a 4 s window (`JR_ECHO_PASS_MS`) in which `jr_echo_gate_silence` returns false whatever is audible. It only turns zeros into the live frame the privacy gate already allowed; a muted or flipped device still zero-fills before the gate is consulted (`mic_gated`, `main/main.c`).

**[2026-11-11] "Jarvis" over a reply.** In the voice task, after the privacy zero-fill and before the uplink batch, `jr_wake_feed(mic_frame, n)` is called when the phase is SPEAKING, the frame is not privacy-gated, `wakebarge` is on and `jr_wake_ready()`. It sees the canceller output after make-up, the same frame the uplink would carry (before the echo gate zeroes the batch copy). On a hit: `speaking_stop_playback` (the stop and `JR_EV_BARGE_DETECTED` a tap uses), `jr_reply_cut_begin` (the physical-tap path, so the server's remaining audio is dropped), `jr_echo_gate_pass_for`, `wake_cuts++`. No new task or lock. Cost is about 3 to 4 ms per 32 ms frame on top of the AEC while a reply plays, inferred from the esp-sr benchmark figure quoted in `components/jr_wake/include/jr_wake/jr_wake.h`; it has not been measured on the device.

**[2026-11-11] Risks seen in the audio path, not fixed here.**
- `apply_in_gains_guarded` (speaking edge, httpd `speakmic`/`mic`) does blocking I2C with up to 3 retries per channel while holding the ADC guard; the capture caller then waits up to 100 ms on that guard. Not new, but the speaking edge now moves the PGA by 18 dB instead of 3.
- `mic_reopen_cycle` takes both guards and reopens the codec; with the watch not judging while speaking it can no longer run during playback, which removes one way to reopen under a reply.
- `aec_apply_pending` can stall the voice task for the length of two `aec_create_from_config` calls if the first fails; unmeasured.
- The wake model may hear the assistant say its own name inside a reply and cut it; at the canceller levels above this is unlikely but unmeasured.

---

## First device run of 4c33ab4: the voice loop falls behind (2026-11-11)

The sweep with the gate off looked worse than the knob-only runs at the same
gains. The raw microphone and reference levels match the earlier runs, so the
gain staging does what it should. The canceller gets fewer frames.

`/api/diag/vadlog` writes one row per frame the voice task reads. A frame is
512 samples at 16 kHz: 32 ms, 31.25 a second.

| image | listening | speaking | gaps over 48 ms while speaking |
|---|---|---|---|
| 8ca3258 (before the echo fix) | 31.3 fps | 26.5 fps | 16 % |
| 4c33ab4 (echo fix) | 29.1 fps | 18.4 fps | 50 %, 6.7 % over 96 ms |

Residual echo over a whole reply (canceller output before make-up, p90 of the
speaking rows), volume 100:

| speaking gain | knob-only on the old image | 4c33ab4 |
|---|---|---|
| 3 dB | 31, 48 | 262 |
| 6 dB | 31, 22 | 275 (reference 12 dB), 109 (reference 24 dB) |
| 9 dB | 178, 183 | 575 |

Within a reply the old image sits at a median of 1 per second; 4c33ab4 swings
between 1 and 170. A canceller that loses every third frame never settles.

Causes, both in `voice_task` (`main/main.c`):

- The only new work while speaking is `jr_wake_feed()` (WakeNet9), on the same
  task that already spends 12.5 to 15.3 ms of each 32 ms frame in `aec_process`
  (`aec_us` from `/api/debug/gain`).
- The loop ends with a one-tick sleep on the paced path. `CONFIG_FREERTOS_HZ`
  is 100, so that is up to 10 ms after every frame, also when the next frame is
  already waiting. This is why the old image already lost 15 % while speaking.

The same run logged playback holes of 219 to 798 ms mid-reply and one lost
websocket (no PONG for 20 s). Holes were logged on older images too, so they are
not proven to come from this.

Stopgap on the running device: `wakebarge=0` (run-time, lost on reboot). The
echo gate is on by default, so replies behave as before the fix.

### The fix (merged 4a6e98b, built, NOT yet on the device)

- `components/jr_wake`: a `jr_wake` task (core 0, priority 5, 12 KB stack in
  PSRAM, 8 KB ring in PSRAM) owns the model and is the only caller of `detect`.
  The voice task calls `jr_wake_push()` (copy, never blocks, drops and counts
  when full) and `jr_wake_take_hit(now, 500)`; a hit older than 500 ms is
  discarded. `jr_wake_feed()` is gone.
- Privacy: `jr_wake_flush()` runs at the top of every voice-loop pass while the
  microphone is gated. It empties the ring and voids a hit whose chunk was
  already inside the detector, so audio from before a mute is never analysed
  after it. A hit never touches privacy state.
- `components/jr_core/src/pace.c` (`jr_pace_skip_sleep`, host-tested): the loop
  skips its tick sleep when the codec read did not block (under 2 ms) or the
  iteration's own work took a frame, at most three times in a row, so IDLE1
  still gets a tick every fourth frame.
- The pacing uses the time the read was BLOCKED (`jr_audio_last_read_wait_us`),
  not the read's duration. The canceller runs inside the read and takes 12 to
  15 ms while speaking; judged on duration, the sleep would have stayed exactly
  in the speaking path.
- `GET /api/diag/voiceperf` (`jarvisctl.py voiceperf`): frames, fps, iteration
  mean and max, read mean for speaking and for the other capturing phases; tick
  skips and sleeps; the wake worker's detect time, drops, stale hits and stack
  headroom. `POST ?bench=1` (`voiceperf bench`) times 64 noise chunks through
  `detect` and 64 noise frames through the live canceller with no microphone,
  codec or session; it also runs muted. It feeds noise to the live canceller
  and holds the ADC guard for about 0.8 s: not during a measurement.
- Gates: 23 host suites, `jr_host_tests` 257 tests; image 2,478,768 bytes.

Not proven: any of it on the device. The flash did not happen because the
device stopped answering on the network (asleep, most likely). First checks
after flashing: `voiceperf bench` (detect and canceller cost, worker stack
headroom), then with the microphone open `fps` while speaking, which should be
near 31, and the sweep again.

How to read a capture: `p90` of the `rms` column over the speaking rows
(phase code 5) of a per-point vadlog. Two-second WAV windows are too noisy a
statistic for this; they were what misled the first reading.

## Primary sources

| Source | Notes |
|--------|-------|
| `components/jr_audio/src/jr_audio.c` (`src_read`, `aec_apply_pending`, `apply_in_gains`) | Capture, canceller, PGA, make-up. |
| `components/jr_core/src/echo_gate.c`, `mic_watch.c` | The pure decisions; host tests in `host/test_voice_fix.c`, `host/test_mic_watch.c`. |
| `main/main.c` (`speaking_stop_playback`, wake feed, gate call) | Wiring in the voice task. |
| `main/http_routes.c` (`gain_get_handler`, cockpit) | Knobs and reporting. |
| `managed_components/espressif__esp-sr/include/esp32s3/esp_aec.h` | `aec_mode_t`, `aec_config_t`, chunk size. |
| [voice-loop-2026-11-10.md](./voice-loop-2026-11-10.md) | First calibration and the echo gate. |

---

## Open questions

- Device run, one unmuted session, per volume (100, 80, 60) and per setting: raw peak and rms of the speaking lane (`mic-raw` tap), residual rms (`clean_rms` x make-up inverse), `aec_us`, phantom `srv_interrupts`, `echo_gated_frames`.
- Is 6 dB the best speaking gain with `ref` at 12, or `ref` 24 as in the table (40 to 49 dB at 6/24)? Try `speakmic` 3, 6, 9 against `ref` 12, 24.
- Does `aecmode` 6 or `aecfilter` 6..8 beat the 4/5 default, and what do they cost in `aec_us` and in the largest internal block (rebuild log line)?
- Which `echovol` keeps phantom interrupts at zero while the owner can still talk over a reply, and does `speakgain` 100 give the owner the same server-side level as while listening (compare `clean_rms` x make-up with the listening value)?
- Does "Jarvis" cut a reply at volume 100 and 60 (`wake_cuts`), how often does it fire with nobody speaking, and what do the voice task's timing and stack show in `/api/diag/tasks` with `wakebarge` 1 against 0?
- Does the image still restart by itself during a reply (open problem on the base image, read by another lane)?

---

## See also

- [voice-loop-2026-11-10.md](./voice-loop-2026-11-10.md) — the echo notes this page continues.
- [aec-barge-in.md](./aec-barge-in.md) — why the bare AEC and WakeNet engines are used instead of the AFE pipeline.
