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
| `components/jr_dsp/src/dsp.c:101` + `host/test_jitter.c` | The audible-hole threshold and the logged session replayed. |
| `components/jr_audio/src/jr_audio.c:902` (`feeder_task`) | The new feeder counters. |
| `main/device_tools.c` (`device_try_local_tool`, `device_gateway_*`) | Local tools and gateway state. |
| `main/tests/test_board_gate.py` | Board poll gate and cadence, with mutations. |

---

## Open questions

- Does the server answer a 0.9 s utterance if `prefixPaddingMs`/`silenceDurationMs`/`startOfSpeechSensitivity` are set? One at a time, on the device.
- Is the 1500 ms refill hold after a hole worth its silence? Needs a soak with the new `refill_hold_*` counters against the stalls.
- Is `phantom_interrupts` non-zero with nobody speaking, and does it follow volume?
- Session resumption: string handle through the event types, setup field, and a device check.

---

## See also

- [playback-jitter.md](./playback-jitter.md) — the ring, the feeder and the walk this page changes.
- [gemini-live-api-v5.md](./gemini-live-api-v5.md) — setup fields, VAD config shape, resumption.
- [aec-barge-in.md](./aec-barge-in.md) — the AEC and barge design the echo finding leans on.
- [jarvismcp-bridge.md](./jarvismcp-bridge.md) — the gateway the tool policy gates.
