# The voice loop: unanswered utterances, holes, echo, and an honest tool list

**What it is** — What one real two-and-a-half-minute session (device log, 301 lines, 2026-11-10) showed about the voice loop, the mechanism behind each symptom with `file:line`, what was changed, and what only a capture on the device can settle. Gemini Live (`models/gemini-3.1-flash-live-preview`) through `components/jr_transport`; policy in `components/jr_core`; playback in `components/jr_audio` and `components/jr_dsp`; wiring in `main/`.

**How we use it here** — Read this before touching the unanswered-utterance watch, the playback pre-roll walk, the setup message's tool list, or the system instruction's tool text. Line numbers are at the commit that introduced this page.

---

## Findings & gotchas

**[2026-11-10] "Two unanswered in a row" never meant in a row (item 2, fixed).**
The old watch kept `s_unanswered` and cleared it only when the reply deadline was still armed as the phase left Listening. A server reply clears the deadline first (`rich_cb` zeroed it on any frame), so the clear branch could not run after an answered utterance. In the log the first miss was followed by seven answered utterances and the next miss, 115 s later, was logged "2 in a row" and threw the session away (`StaleDeadline`, new `setup`, no context). Fix: `components/jr_core/src/utt_watch.c` (pure, host-tested). A reply-bearing server frame (`rich_cb`, `main/main.c:1295` onward) counts in `s_srv_answer_seq`; the voice task turns it into `jr_utt_answered()`, which clears the run. The reply to the device's own "Pardon" turn does not clear it (`pardon_turn`), otherwise a server deaf to audio and alive to text would be asked "Pardon" for ever.

**[2026-11-10] The recovery is now one short word, not a speech (item 2, changed).**
The first miss used to send a text turn telling the model to "answer it now, or ask one short question"; the log shows the model answering with "I heard nothing, Sir. What was your request?". It is now told to say only "Pardon, Sir?" (`main/main.c:3193`). The second consecutive miss still replaces the session.

**[2026-11-10] Session resumption is NOT built, so a reconnect loses the conversation (item 2, not built).**
`main/main.c`'s old comment said "reconnect with the resume handle". The setup message never asks for it: `jr_gemini_build_setup` (`components/jr_transport/src/gemini_live.c`) emits no `sessionResumption` object, so the server never sends a handle, and the core carries the token as a `uint32_t` (`components/jr_core/include/jr_core/session.h:143`) where the API's handle is a string. Building it needs a string handle in the event type, the setup field, and a device test of the server's behaviour; not built blind. `docs/reference/gemini-live-api-v5.md:146` has the field's shape.

**[2026-11-10] Why a 0.9 s utterance gets no answer: NOT proven (item 1).**
What the code proves about the uplink: with `vad_mode = JR_VAD_SERVER` (`main/main.c:4683`) every captured frame is batched two at a time (64 ms) and sent while `capturing` and the phase is Listening, Speaking, Asking or Thinking (`main/main.c:3993`, `:4152`); the local VAD gates nothing, only paces the loop and feeds the face. The speech-end flush sends the partial batch. The setup sets `automaticActivityDetection.disabled=false` and nothing else (`gemini_live.c:185`): no `prefixPaddingMs`, `silenceDurationMs` or sensitivities, so the server's defaults apply, and the repo has no published value for them. The two unanswered utterances were 0.94 s and 0.88 s; the shortest answered one was 1.07 s; `UTT_MIN_MS` (800 ms) is what let them count as misses at all. That fits a server that needs about a second, and equally fits a dropped frame or a missed onset. Nothing in the log separates them, so the setup is unchanged. New evidence on the device: the speech-end log line now reads `len=<ms> uplink=<frames> drops=<tx drops during the utterance>`; 0.9 s should be about 14 frames of 64 ms and no drops. If frames and drops are right, the server is not hearing a short utterance and the next experiment is `prefixPaddingMs`/`silenceDurationMs`/`startOfSpeechSensitivity` in the setup, one at a time, on the device.

**[2026-11-10] The pre-roll did not "ratchet and never come down" (item 3); it stepped up for gaps the speaker covered (fixed).**
The walk is `components/jr_dsp/src/dsp.c:44` (`jitter_adapt`): +300 ms per reply with a hole, ceiling 1500, -200 ms after three clean replies in a row, floor 600. It is capped and it comes down; the log has six replies and never three clean in a row, which is why it only went up (600 to 900 to 1200 to 1500). What was wrong is what counted as a hole. A hole is booked when the ring ran dry while the last write was still sounding (the 80 ms DAC tail) and data returned inside 2.5 s. A return inside the tail leaves no audible gap at all. Holes of 18 ms and 107 ms stepped the lead (two of the four steps in the log). `JR_JITTER_HOLE_MIN_MS` (130 ms = tail + 50 ms, `dsp.h`) now separates them; shorter ones are counted as `holes_ignored` and do not touch the walk (`dsp.c:101`). The threshold is a policy, not a measurement.

**[2026-11-10] Server pause or local starve (item 3): the log says server, the device can prove it.**
Evidence for a server pause: the first hole (607 ms) ends exactly when the next transcript event arrives (events at 36.6 s, then 39.2 s, hole booked at 39.2 s); the transcript and its audio arrive together, so nothing was waiting locally. That is one hole; it is not proof of the other three. What separates the cases on the device, all in `/api/device/health` (`playback`, `rx`):
- `rx.max_gap_ms` large and `rx.queue_wait_max_ms` small: the frames were late arriving (server or network).
- `rx.queue_wait_max_ms` large: frames arrived and sat before the parser took them (the ws task or parser).
- `playback.feeder_stall_max_ms` large (new): the feeder pass itself was late with audio waiting, i.e. it was kept off the CPU (a 1 MB face-clip read, `gfx_render` on core 0). Normal is about 32 ms, one chunk.
- `playback.refill_holds`, `refill_hold_max_ms`, `refill_hold_total_ms` (new): after a hole the feeder holds playback until a 1500 ms lead is rebuilt or 2 s pass (`jr_audio.c:1021`). That hold is silence added on top of the network's. It is the most likely larger cause of audible stutter and is not changed here: it was tuned against a 30-minute soak (see `playback-jitter.md`) and a change belongs with a new soak. The counters show how much it costs.

**[2026-11-10] The model hears itself (item 4): mechanism stated, not built.**
What the uplink carries while the model speaks: the AEC output (`jr_audio.c:708`, AEC runs only while playback is pending, which includes the 80 ms tail), then a fixed 6x soft-knee make-up gain (`jr_audio.c:111`, `:732`), sent as it is. Server VAD is on with `START_OF_ACTIVITY_INTERRUPTS` (`gemini_live.c:178`). The local barge gate is not in the path: `s_local_barge_enabled` is false by default (`main/main.c:809`), so barge is entirely the server's, and the comment at `gemini_live.c:173-177` ("the local barge gate carries interrupts") is stale. The AEC leaves residual echo (`aec-barge-in.md` puts it near 20-30 dB), the make-up gain multiplies it by 6, and it scales with speaker volume. In the log the model's last words ("Where are you?", `generationComplete`) were followed within 10 ms by `interrupted` with no local `vad: speech start` for 7 s: the server decided someone had spoken over a reply that had in fact finished. The smallest change that keeps real barge would be to send zeros instead of audio while the speaker is audible and the AEC-clean level is under the barge gate, which needs the local gate's scale to be right first (`turn_policy.h`: the gate was calibrated on post-gain RMS, and the VAD now judges pre-gain RMS, `main/main.c:4079`). That is a calibration on the device, not a host test, so it is not built. What was added: `srv_interrupts` and `phantom_interrupts` in `/api/cockpit` (`voice`), and a warning line when the server interrupts with no local speech start in 1.5 s, with the volume. Capture needed: with the owner silent, play ten replies at volume 100 and at 60; count `phantom_interrupts`; then pull `/api/audio/tap.wav` for the MIC_CLEAN tap during a reply to see the residual.

**[2026-11-10, second pass] The model hears itself (item 4): mechanism proved from the code, gate built (feat/echo-guard).**
*Which signal the ring logs.* `vadlog` `rms` is `td.rms`, the value the turn policy was fed (`main/main.c:4111`): with `s_vad_use_clean` true (the default, `main/main.c:889`) that is `jr_audio_clean_rms()`, the AEC output measured BEFORE the make-up gain (`jr_audio.c:728`). It is not the raw microphone and not the uplink. The uplink is the same AEC output times a fixed 6x with a soft knee at 28000 (`jr_audio.c:111`, `:653`, `:732`) and goes up as it is. So the owner's voice (p90 75-143, max 186-291) went up at roughly 450-860 and 1100-1750, and the model's own voice (p90 1310-3457, max 5459-30013) went up at 8000 to full scale: ten to a hundred times the owner's level, with the knee compressing only above 28000. `peak_play` is not "the speaker is on": it is a 4-frame (128 ms) peak-hold (`turn_policy.h:47`) of `playback_level`, which `main.c:4107` feeds only while the phase is Speaking, so it reads 0 once a `turnComplete` has moved the phase to Listening while the ring still drains (the ring holds up to ~10 s, `jr_audio.c:120`). The log therefore cannot give the tail length from `peak_play`; the gate uses `jr_audio_playback_pending()` (`jr_audio.c:1294`: ring not empty, feeder writing, or the 80 ms DAC tail of `:1048`), which is also exactly when the AEC runs (`:708`).
*The chain while it plays.* PGA: 24 dB listening, 9 dB while `jr_audio_set_speaking(phase == Speaking || playback_pending)` (`main.c:2937`, `jr_audio.c:83-85`, `:446`); AEC: `aec_process(mic lane, ref lane)` with `AEC_MODE_FD_LOW_COST`, filter length 4 (`jr_audio.c:113-115`, `:712`), the reference being the codec's own MIC3 loopback lane at 12 dB (`:87`, `:699`); then the 6x. Nothing gates or attenuates the uplink while speaking: the only existing defences are the 400 ms `VAD_POST_SPEECH_REFRACTORY_MS` (`main.c:77`, `:4133`), which suppresses only the LOCAL speech-start event, and the local barge gate, which is off (`main.c:809`). Neither touches what the server receives. `volume` (`jr_audio_set_gains`, `jr_audio.c:1374`) is the DAC's hardware level, so it scales the acoustic echo and the loopback reference together; the PGA and the 6x do not move with it, so the residual rises with it.
*Can the canceller be working?* Not at this residual, as far as the ring can say. With the model silent the AEC-clean level is 1-9 (the room is 8-9): the canceller squashes everything when the reference is silent. As soon as the reference carries speech it gives 1000-6000, 20-60 times the room and the owner. A canceller that worked would leave something near the room level (the ring's 8-9) plus a little. Two spikes of 30013 and 20547 at 43 and 79 ms after the last reply (window 313.3 s) coincide with the AEC switching off and the PGA stepping from 9 to 24 dB (+15 dB) while the room still rings: a transition artefact that the tail below covers. The ring cannot give the raw microphone level, so "AEC makes it worse than no AEC" is not proven; what proves it either way is `/api/audio/tap.wav`: pull MIC_RAW, REFERENCE and MIC_CLEAN together during one reply at volume 100 and at 60. (a) `rms(MIC_CLEAN)/6` against `rms(MIC_RAW)` gives the cancellation in dB (ERLE): a working canceller is 15 dB or more; near 0 or negative means it is not cancelling or is diverging. (b) A REFERENCE lane that is silent or uncorrelated with the PLAYBACK tap (cross-correlate; the delay should be a fixed few ms) means the loopback wiring or lane order is wrong and no filter can work. (c) `gate_off` capture (`/api/debug/gain?echogate=0`) with `phantom_interrupts` counted over ten replies at each volume.
*What was built.* `components/jr_core/src/echo_gate.c` (pure, tested in `host/test_voice_fix.c`): `jr_echo_gate_silence(now, audible, enabled)`. In `main/main.c:4213`, inside the branch where the microphone is already permitted, `audible = playback_pending || a chunk in the last 250 ms` and the frame is replaced by zeros of the same length, so frame count and cadence are unchanged (silence frames, never nothing). The tail is `JR_ECHO_TAIL_MS` = 150 ms on top of the pending state (which already includes the 80 ms DAC tail): after the last loud frame at the end of a reply the AEC-clean level was 385, 88, 39 at 59, 114, 150 ms (window 270 s) and 30013, 198, 90, 26 at 43, 99, 122, 160 ms (window 313 s), i.e. under about twice the room by 150 ms. A sentence begun the instant the reply's audio ends loses at most its first 150 ms; one begun 200 ms later loses nothing (test `test_echo_gate_owner_speaking_200ms_after_the_reply_is_not_lost`). The number is a reading of three windows, not a calibration. Counters in `/api/cockpit` `voice`: `echo_gate`, `echo_gated_frames`, `echo_passed_frames`, `reply_cuts`, `reply_cut_dropped_chunks`. Switch: `/api/debug/gain?echogate=0|1` (`http_routes.c:388`, echoed as `"echogate"`), on at boot, not persisted. THE GATE IS NOT THE PRIVACY GATE: privacy is decided first and elsewhere (`jr_audio_uplink_batch_guard`, the paused/flip latches, the `else` branch that zero-fills when `upload_microphone_enabled` is false); the gate only turns a live frame into zeros, and `test_production_echo_gate_sits_inside_the_privacy_branch_and_keeps_cadence` pins its position inside the permitted branch.
*Cost.* With the gate the owner cannot barge in by voice while the speaker is audible: that is the point, and the next item is what replaces it.
*Tap to stop (item 3).* A tap on the glass during Speaking already muted the DAC and flushed the ring (`main.c:3965-3970`, session row `session.c:765`), but it did NOT stop the reply: the server keeps sending, `rich_cb` staged every chunk into the ring regardless of phase (`main.c:1337`, unconditional before this change), and in Listening a chunk moves the phase back to Speaking and unmutes the DAC (`session.c:597`), so the rest of the reply played on. Now a PHYSICAL tap also begins a reply cut (`main.c:3974`): `rich_cb` discards audio chunks (no staging, no event to the core, `main.c:1324`) until the server's boundary (`interrupted`, `generationComplete`, `turnComplete`, a new `setupComplete`), or until 3 s pass with no chunk (`JR_REPLY_CUT_GAP_MS`; the boundary was lost and the next reply must not be swallowed). The server is not told: no transport message ends a turn without starting another (`audioStreamEnd` is a stream marker; `activityStart/End` are manual-VAD only and the transport returns OK without sending in auto-VAD, `gemini_live.c:1135-1144`; a text turn starts a new reply; `docs/reference/gemini-live-api-v5.md:249-270`), and none was invented, so the server spends the rest of that generation. A synthetic tap keeps the old behaviour (local stop) and does not start the cut, the same rule as for answering `ask_user` (`main.c:3599`).
*Wake word during playback.* Not built and not proven. `jr_wake_feed` runs only in Idle/Backoff (`main.c:4296`). WakeNet would hear "Jarvis" through the same AEC-clean signal that is 1000-6000 RMS while the model speaks at volume 100, against 75-143 for the owner; whether it still fires is a measurement: feed a MIC_CLEAN tap recorded during a reply, with the owner saying "Jarvis" over it, through the engine offline.
*Answered count (item 5).* `answered` counted one per reply-bearing server frame (62 after four utterances); `jr_utt_answered` (`utt_watch.c:36`) now counts once per turn (`turn_counted`, reset by the local speech start) and still clears the run of misses on every frame.
*What the owner should do to check it.* Flash, then with the glass quiet: `jarvisctl status`, `/api/cockpit` baseline. Talk for two minutes at volume 100 asking questions with long answers. Read `/api/cockpit`: `phantom_interrupts` should stay 0 and `srv_interrupts` should equal only the replies the owner really talked over (none, by the gate); `echo_gated_frames` grows by about 31 frames per second of reply (32 ms frames) and `echo_passed_frames` by about 31 per second of listening; `answered` should equal the number of questions asked. The log must not print "server interrupted with no local speech start". Tap the glass during a long reply: the audio stops, `reply_cuts` is 1, `reply_cut_dropped_chunks` grows by the rest of the reply, the line `gesture: tap stopped playback and cut the reply` appears, and the next answer plays. Then `/api/debug/gain?echogate=0`, repeat at volume 100 and 60 and compare `phantom_interrupts`: that is the calibration run, and the tap capture above gives the ERLE.

**[2026-11-10] The echo gate is a stopgap, by the owner's ruling.** Told that the gate ends interrupting by voice, the owner answered: "this is built in gemini feature that is necessary". Interruption is the server's own activity detection listening while the model speaks; a client that sends silence during playback has switched it off. The gate stays in the image only so the device does not cut its own replies in the meantime. The work that replaces it is the canceller: PLAN N16.9, the capture described under the second-pass echo notes above, with `echogate=0`. When cancellation holds at the owner's volume the gate defaults to off.

**[2026-11-10] First canceller calibration (PLAN N16.9), gate off, owner present.** Device speaking a counted list; `mic-raw`, `reference` and `mic-clean` pulled together from the two-second taps; `mic-clean` is after the 6x make-up, so the canceller output is `mic-clean / 6`.

| volume | speak gain | reference peak | raw mic rms | raw mic peak | clipped in 2 s | canceller output rms | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 100 | 21 dB | 3644 to 3779 | 6874 to 12091 | 32768 | 132 to 2008 | saturated (6x output at 32768) | the microphone rails; a linear filter has nothing to work with |
| 80 | 21 dB | 1115 | 1411 | 18957 | 0 | 138 | about 20 dB down, and still the size of the owner's voice (75 to 143) |
| 60 | 21 dB | 335 | not measured | | | | the microphone had failed by then (N16.10) |

Readings: the reference lane is healthy and never clips (peak 11 % of full scale at 100), so the reference is not the fault. At 100 the fault is the capture: `GL_MIC_PGA_SPEAK_DB` is 21 on this board (`jr_audio.c:83`), chosen so a talk-over stays audible, and at 21 dB the speaker rails the microphone. At 80 the canceller works to about 20 dB and that is not enough: the residual equals the owner's speech level, so the server cannot tell them apart by level. Not measured: speak gain 9 and 15 at volume 100, and anything at 60. One server interrupt with no local speech start was logged at volume 100 with the gate off. Captures are not in the repo.

What follows from it: (1) lower the speak gain until 100 does not clip and give the owner's level back as digital gain after the canceller, where it does not cost headroom; (2) measure `AEC_MODE_*_HIGH_PERF` and a longer filter against the 20 dB of `FD_LOW_COST`; (3) if the residual still matches speech at 100, cap the volume where it does not. The second calibration needs N16.10 first: a gain change is what silenced the microphone.

**[2026-11-11] Second calibration (PLAN N16.9), gate off, 31 counted replies.** Residual is the canceller output before the 6x make-up (`mic-clean / 6`), one value per two-second window. The owner reads 75 to 143 rms at the 24 dB listening gain; at 9 dB that is 13 to 25, at 6 dB 9 to 18.

| volume | speaking gain | reference gain | raw mic rms | raw mic peak | residual rms | cancellation |
| --- | --- | --- | --- | --- | --- | --- |
| 100 | 21 dB | 12 | 6874 to 12091 | 32768, clipped | saturated | none |
| 100 | 9 | 12 | 3007 to 3665 | 30000 to 32767 | 335, 8, 525+ | 18 to 21 dB, one window 54 |
| 100 | 3 | 12 | 1166 to 2301 | 13000 to 15000 | 91, 2, 66, 2.5 | 22 to 58 dB |
| 100 | 6 | 24 | 846 to 2155 | 10500 to 20300 | 14, 3.5, 102, 18 | 40 to 49 dB, one window 26 |
| 100 | 9 | 24 | 2426 to 4124 | 23600 to 28400 | 155 to 183 | 24 to 26 dB |
| 80 | 21 | 12 | 2825 to 4129 | 30409 to 32768 | 112, saturated | 11 to 28 dB |
| 80 | 9 | 12 | 386 to 1380 | 4600 to 15400 | 48, 90, 67, 21 | 13 to 25 dB |
| 80 | 9 | 24 | 606 to 1049 | 6000 to 10800 | 6.5, 22, 28, 37 | 25 to 45 dB |
| 70 | 21 | 12 | 817 to 899 | 11000 | 54 to 67 | 21 to 23 dB |
| 70 | 9 | 12 | 550 to 970 | 5500 to 6300 | 7, 4, 25, 6 | 29 to 44 dB |
| 60 | 9 | 12 | 99 to 290 | 1500 to 2900 | 2 to 7 | not computed (quiet windows) |

Readings. Headroom decides: with raw peaks under about 16000 the existing canceller (`AEC_MODE_FD_LOW_COST`, filter 4, aggressive NLP) reaches 40 dB and more; near the rail it gives 20 dB or nothing, and 21 dB of speaking gain (`jr_audio.c:83`) puts volume 80 and 100 at the rail. The speaker is far louder than a person at this microphone: about 45 dB at volume 100, 34 at 80, 20 at 60, so even 45 dB of cancellation leaves a residual the size of the owner's voice at full volume, with bursts above it. A louder reference lane (24 dB) looks better at 80 and 100 but each point is one reply. Phantom server interrupts with the gate off: one each at 100/21, 80/21 and 70/21, none at the lower gains. Volume is steep: 100 to 80 is about 10 dB at the microphone, 80 to 60 another 12. Windows vary because counting has pauses and the taps are two seconds long; captures are not in the repo.

What follows: speaking gain 6 dB with the difference made up after the canceller; the echo gate only above a volume threshold to be set on the device from the phantom count; the wake word fed during replies so the owner can cut a reply by saying it at any volume; `FD_HIGH_PERF` measured against today's mode. All four are in `feat/echo-fix` behind `/api/debug/gain` knobs.

**[2026-11-10] An honest tool list (item 5, fixed).**
The setup declared all ten tools whether or not the gateway could be used, and the system text called them the model's hands; the model offered to "delegate a task to an agent" and every call returned an error. The gateway's reachability was also never tracked: a stored host that does not resolve failed the board poll every 90 s all boot. Now `components/jr_core/src/tool_policy.c` decides:
- `jr_tool_needs_gateway(name)`: memory, search, execute, delegate and the board are gateway-only; `current_time`, `weather_here`, `set_volume`, `set_brightness`, `ask_user` run on the device. An unclassified name counts as gateway-only.
- `jr_gateway_*`: three consecutive calls that never reached a server make the gateway unreachable until one does. `device_gateway_usable()` (`main/device_tools.c`) is configured AND not unreachable.
- At every `SEND_SETUP` (`main/main.c:1590`) `cfg.no_gateway` is set from it; `jr_gemini_build_setup` skips the gateway-only declarations (`gemini_live.c`, tested with and without), and `compose_system_instruction` uses `jr_tool_instruction()` (`main/main.c:4411`), which says plainly that the gateway is not connected, what the device has, and that the model must never offer the rest.
- `board_maybe_poll` already did not run without a key (`main/tests/test_board_gate.py`); it now also probes an unreachable gateway once per 15 minutes instead of every 90 s.
- `/api/cockpit` `tools.configured` now means usable, `tools.declared` is the count the next setup declares, and `tools.why` is one of `ok`, `worker_not_ready`, `gateway_not_configured`, `gateway_unreachable`. The STATUS glass uses the same truth.
- Open: the brief says no key is stored while `tools.configured` read true; the device may hold a key the owner thought it did not, or a placeholder. `tools.why` will say which (`gateway_unreachable` means a key is stored and the host does not answer).

**[2026-11-10] It asked where Sir is with the weather on the glass (item 6, fixed).**
The only weather the model could reach was `execute_tool` with `weather {latitude, longitude}`, a gateway call; the place was a hint inside that tool's description. The glass's own weather (Open-Meteo, no gateway) was never offered to the model. Now `weather_here` (no arguments) is declared always and answered from the weather the device holds (`device_try_local_tool`, `main/device_tools.c`, run on the voice task, the only writer of `s_weather`); with no weather yet it returns `weather_unavailable`. With the gateway down, `current_time` is answered from the device clock too. Both system texts tell the model to use `weather_here` and never to ask where Sir is.

**[2026-11-10] Also in the log (not fixed).**
- `i2s_channel_disable ... has not been enabled yet`, twice at unmute (log lines 16 and 21): the codec open path disables an I2S channel it has not enabled yet; harmless noise from the open sequence, not investigated further.
- `playback underrun` is logged at WARN for what is usually the server pausing; the wording invites misreading. Left.
- `pre-roll` steps are logged only when they change; the ceiling is silent. Left.
- `gesture: hold on the rim ignored` while the owner was holding the rim: by design.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `components/jr_core/src/utt_watch.c` + `host/test_voice_fix.c` | The watch and its tests, including the logged defect replayed. |
| `components/jr_core/src/tool_policy.c` | Gateway-only classification, health, and the two instruction texts. |
| `components/jr_core/src/echo_gate.c` + `host/test_voice_fix.c` | The echo gate and the reply cut, with tests. |
| `components/jr_dsp/src/dsp.c:101` + `host/test_jitter.c` | The audible-hole threshold and the logged session replayed. |
| `components/jr_audio/src/jr_audio.c:902` (`feeder_task`) | The new feeder counters. |
| `main/device_tools.c` (`device_try_local_tool`, `device_gateway_*`) | Local tools and gateway state. |
| `main/tests/test_board_gate.py` | Board poll gate and cadence, with mutations. |

---

## Codec I2C failures and the silent microphone (N16.10)

**Mechanism, verified in source.** `es7210_update_reg_bit`
(`managed_components/espressif__esp_codec_dev/device/es7210/es7210.c:124-130`)
ignored the result of `es7210_read_reg`, which zeroes its output first (`:118-122`).
A failed read made `regv` 0, so the write cleared every bit outside the update mask,
including the PGA-enable bit 0x10 that `es7210_mic_select` sets (`:201`, `:208`, `:215`).
`_es7210_set_channel_gain` (`:347`) uses it for every gain change, and
`apply_in_gains` (`components/jr_audio/src/jr_audio.c`) calls it twice on every SPEAKING
edge, on `/api/debug/gain` and on ADC open. A second gain set cannot repair it: the update
touches 0x0f only. Same pattern in `es8311_set_mute` (`es8311.c`, `DAC_REG31`).

**What is built.** `scripts/patch-v5-managed.py` (checked by `--check` in the build): the
ES7210 update retries the read three times and returns the error without writing; the
ES8311 mute does the same for unmute, and still writes the mute bits on a failed read
(silence is the safe direction); the I2C transaction timeout goes 100 to 250 ms; the
failure log now carries the `esp_err_t` name. `apply_in_gains` retries each set three
times and counts a final failure. `jr_core/mic_watch` is the net under it: raw
microphone lane under rms 3.0 for 4 s while the microphone is enabled, awake and the ADC
open means dead; the capture caller then runs the ADC through
`jr_audio_power_reconcile` closed and open (backoff 10 s, doubling to 60 s, reset by a
live frame). It is never judged while muted, asleep or closed, and the second reconcile
re-reads the request, so a mute landing between the two leaves the ADC closed.
`/api/cockpit` `voice.mic_dead`, `voice.mic_recoveries`; `/api/device/health`
`audio_power.gain_failures`.

**Who is on the bus (verified in source).** One I2C master bus, port 0, 400 kHz
(`boards/waveshare/esp32s3_touch_amoled_1_75c/board_peripherals.yaml`,
`board_devices.yaml`), shared by: ES8311 and ES7210 (esp_codec_dev), the CST9217 touch
panel (board manager), the QMI8658 IMU (`jr_imu`, task prio 3, own 100 ms timeout) and
the AXP2101 PMIC (`jr_power`, task prio 2, 100 ms timeout, 1 s sampler plus a 500 ms
key poll). Codec callers: `httpd` (`/api/debug/gain`, `/api/device/levels`) through
`jr_audio_set_gains`, the voice task on SPEAKING edges (`jr_audio_set_speaking`), the
capture caller on open and reopen, and the feeder (mute and unmute, `:885`, `:1316`,
`:1341`). Gain calls hold `s_adc_guard`, volume and mute `s_dac_guard`; those two guards
do not exclude each other, only the IDF bus lock does.

**The timeout (verified in the IDF 5.5.4 source, `esp_driver_i2c/i2c_master.c`).**
`DEFAULT_I2C_TRANS_TIMEOUT` was 100 ms (`audio_codec_ctrl_i2c.c:16`). The driver uses the
caller's timeout twice: to wait for the bus lock (`s_i2c_synchronous_transaction`) and
for the transaction. The lock is `xSemaphoreCreateBinaryWithCaps`
(no priority inheritance). A transaction takes well under 1 ms at 400 kHz, so a 100 ms
failure means the lock holder (the IMU or PMIC task at priority 2 to 3) was not
scheduled for 100 ms (the feeder runs at 19, Wi-Fi at 23), or the bus was stuck, or a
NACK (reported as `ESP_ERR_INVALID_STATE`).

**Proved and not proved.** Proved: the read-ignored write is real and is the only path to
the silent lane; the bus has five devices and at least five task contexts; the lock
wait shares the 100 ms budget. Not proved: which of lock starvation, a stuck bus or a
NACK produced the logged failures; the old log line printed no code. Raising the timeout
is a mitigation against the first, chosen because it is the only one the source shows is
possible without a hardware fault. Ruled out by reading: two codec calls racing on the
same device (the ADC calls all hold `s_adc_guard`, the DAC calls `s_dac_guard`). The next
failure now prints `Fail to read from dev 80: ESP_ERR_TIMEOUT` (lock or transaction
timeout) or `ESP_ERR_INVALID_STATE` (NACK or stuck FSM).

---

## Open questions

- Does the server answer a 0.9 s utterance if `prefixPaddingMs`/`silenceDurationMs`/`startOfSpeechSensitivity` are set? One at a time, on the device.
- Is the 1500 ms refill hold after a hole worth its silence? Needs a soak with the new `refill_hold_*` counters against the stalls.
- Is `phantom_interrupts` non-zero with nobody speaking, and does it follow volume? (The gate is on by default; run with `echogate=0` to measure, and the MIC_RAW/REFERENCE/MIC_CLEAN capture gives the canceller's ERLE.)
- Session resumption: string handle through the event types, setup field, and a device check.

---

## See also

- [playback-jitter.md](./playback-jitter.md) — the ring, the feeder and the walk this page changes.
- [gemini-live-api-v5.md](./gemini-live-api-v5.md) — setup fields, VAD config shape, resumption.
- [aec-barge-in.md](./aec-barge-in.md) — the AEC and barge design the echo finding leans on.
- [jarvismcp-bridge.md](./jarvismcp-bridge.md) — the gateway the tool policy gates.
