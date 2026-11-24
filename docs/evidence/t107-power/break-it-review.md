# t107 independent break-it review

Correlation: `t107/w3/break-it`. Authority: read-only source/test review; no device operations, source edits, commits, Jarvis calls, or additional agents. The only artifact written by this reviewer is this file.

## Result

Wave 4 repaired source: **no unresolved actionable code finding in the prescribed review**. B1-B3 are repaired in the actual source and covered by passing executable sequence tests plus production wiring checks. Scratch audio mutation evidence was not yet available when the coordinator stopped the run. The full task remains **BLOCKED at the firmware build gate**, which failed before compilation because `/project/build` is an incomplete CMake build directory. No repair, cleanup, build retry, OTA, or commit is authorized for this stopped run.

The owner ruling is preserved: privacy is cleared only by hand. Listening power and spoken-turn proof are excluded from this run and remain owner check pending. This review did not unmute or operate the device.

## Findings

### B1: unsent microphone half-batch survives privacy and session changes

Evidence in the pre-repair candidate: `main/main.c:2015-2016`, `2634-2641`, `3663-3675`, `3713-3725`, `3807-3832`; `components/jr_core/src/session.c:646-653`.

The Gemini uplink accumulates two 512-sample frames in a static batch. Its only resets are after a send or inside the active capture branch when `send_uplink` is false. Disarm sets privacy and injects USER_STOP; the session moves to DRAINING and pauses capture. Because DRAINING fails the capture-phase gate, the reset branch is skipped.

Concrete trigger: capture one real frame, leaving 512 samples pending; physically mute or disarm; start a text-driven server-VAD session while still muted. The next source frame is correctly zeroed, but it completes the old half-batch. The send then includes the 512 real samples retained from before privacy. A transport-loss/reconnect sequence can similarly mix a prior session's half-frame into the next session.

Required repair: discard and erase the pending batch whenever privacy/flip gates it or the Gemini capture/session/transport lane is interrupted. Cover the post-read privacy edge too. Regression proof should prefill nonzero samples, apply each interruption, and prove that resumed muted output contains only zeros and that new sessions contain only their own samples. A scratch mutant removing reset must fail that executable sequence assertion.

Status: repaired in Wave 4; actual source and sequence proof reviewed below.

### B2: automatic privacy history clear rewinds the companion absolute cursor

Evidence: `components/jr_audio/src/jr_audio.c:1375-1395` and `1502-1522`; `main/http_routes.c:3767-3778`; `main/main.c:3114-3126`; protocol contract `docs/PROTOCOL.md:319-324`.

The new automatic privacy clear resets `tap->total` to zero along with clearing retained samples. Physical privacy hold leaves the ZeroChat lease alive. A client obeying the documented protocol resumes with its previous `X-Jarvis-Audio-End`; that cursor is now ahead of the restarted producer and receives 409 until the producer catches up or the client reinitializes. The delay scales with prior capture duration.

Concrete trigger: a live lease has cursor 160000; physically mute and unmute without ending the lease; capture 1024 samples; request `after=160000`. The new producer end is 1024, so the actual copy function rejects the cursor. This is a regression introduced by the automatic wipe.

Required repair: clear readable history while keeping its absolute sample clock monotonic. Existing skipped samples should remain accounted for, and an old cursor should follow the documented dropped-window semantics. Test an existing lease cursor across automatic privacy clearing and resumed capture. A scratch mutant rewinding the total must fail.

Status: repaired in Wave 4; actual source and sequence proof reviewed below.

### B3: companion input sends an already copied microphone window after privacy turns on

Evidence: `main/http_routes.c:3661-3667`, `3767-3769`, `3781-3805`; protocol contract `docs/PROTOCOL.md:323-324`.

The companion handler checks privacy before copying. After the PCM copy it rechecks lease liveness and identity, but not privacy or flip. Physical mute may run between those operations while the lease remains valid; the private response buffer is then sent. Clearing the source ring cannot erase this independent copy. The WAV export already rechecks privacy after its copy.

Concrete trigger: pause the handler after a nonempty PCM copy, apply physical privacy, resume the handler. The unchanged lease passes the response guard and PCM is sent despite the 423/no-microphone-bytes contract. This gap predates t107, but is exposed by the changed privacy/export integration and falls within the prescribed privacy review.

Required repair: recheck microphone privacy after copying and before each bounded PCM send. The regression should exercise copy -> privacy/flip -> response with a fake sender and assert that no PCM reaches it; also retain the existing lease-expiry rejection. A scratch mutant removing that response guard must fail.

Status: repaired in Wave 4; actual source and sequence proof reviewed below.

## Wave 4 actual repair verification

- **B1:** `components/jr_dsp/src/audio_power.c:64-77` erases PCM and clears fill on lane interruption or microphone-permission change. `main/main.c:2609-2611` resets on control requests. The guard at `3721-3726` runs even when no source read occurs and excludes diagnostic/companion lanes, closed transport, paused capture and closed manual activity. `3828-3842` resamples permission after the read and fills muted input with zeros. `3849-3861` checks again immediately before sending; read failure clears the batch at `3885-3887`. This closes the retained half-batch and old-session carryover triggers while preserving the two-frame/speech-end cadence.
- **B2:** automatic clearing at `components/jr_audio/src/jr_audio.c:1386-1388` accounts skipped samples and calls the production history helper with `reset_cursor=false`. `components/jr_dsp/src/audio_power.c:80-85` clears head/count and preserves the absolute total. The separate explicit diagnostic reset retains `reset_cursor=true` at `components/jr_audio/src/jr_audio.c:1367`.
- **B3:** `main/http_routes.c:3717-3730` samples app privacy, flip and source microphone permission through the production export helper. Its checks follow the PCM copy at `3787-3789` and immediately precede the bounded binary send at `3825-3830`; the empty response is also checked at `3820`. Existing lease liveness/identity rejection remains at `3804-3811`.
- Executable tests in `host/test_audio_power.c:164-279` cover real half-batch -> disarm -> muted text, privacy during capture/pre-send, capture/transport gaps, preserved cadence/speech-end flush, a cursor larger than 32 bits across privacy clearing, and fake sends rejected on privacy/flip/source-close after copying or privacy immediately before sending. The executing production helpers are supplemented by source ordering/argument assertions at `365-406`. These are useful host regressions; they do not claim device-level concurrency or codec-latency proof.
- `.evidence/t107/audio-repair-host.log`: **166 tests, 0 failures, 0 ignored**. `.evidence/t107/host-after.log`: all **5 suites** pass on the repaired candidate, with **11451 HUD checks, 854 shell checks, 166 core tests, 81 tools checks, 38 Python tests**. No count dropped.
- Audio scratch mutation results: **pending/uninspected at stop**; no mutation acceptance is claimed here. Existing power/display scratch results below were inspected.

## Other paths reviewed

- OTA/deep sleep: `main/power.c:100-108` rejects probation before audio shutdown or RTC/peripheral writes; failed shutdown refuses sleep. `main/main.c:2275-2278` excludes probation and active OTA from DREAM; the existing power-off probation guard is retained at `2439-2444`. Actual production sleep code is included in the host harness (`host/test_mood.c:11`, `87-120`).
- Codec recovery: `components/jr_dsp/src/audio_power.c:6-13` keeps a TX clock peer while ADC is live, turns PA on only for pending playback, and closes the pair for sleep. Failed open/close preserves tracked truth for retry. `components/jr_audio/src/jr_audio.c:536-577` serializes transition ownership; ordinary RX/TX remain independently guarded. Hardware reopen parameters are reapplied.
- Playback close race: `components/jr_audio/src/jr_audio.c:927` claims feeder ownership before ring dequeue. The audible-tail timestamp is published before clearing feeder ownership at `999`; `1234-1245` includes queued, in-flight and tail activity. PA changes use the common critical section and are checked against mute/sleep.
- Muted capture: the pure capture boundary produces paced zeros without hardware reads and rechecks permission after a blocking read. Main repeats the app privacy check before publishing levels (`main/main.c:3717-3722`). Diagnostic ring reads check permission both before and under their lock. B1 and B3 are downstream buffers that those checks did not cover.
- DREAM/wake: the presenter requires idle/rest, no USB/charging, no foreground content or OTA, and only exact quiet captions. It stops face animation, retains battery/privacy rims, redraws on minute/rim/brightness changes, waits up to 1000 ms while settled, and restores the same face after foreground wake. Touch/navigation/foreground publication notifies the render owner. Production presenter host seams test the cadence, same-face restoration and immediate wake.
- Wi-Fi loss: weather scheduling uses connection and mood eligibility for first fetch, retry and ordinary refresh (`main/device_tools.c:284`; `host/test_mood.c:153-178`). The uplink half-batch carryover during interruption is included in B1. No new reconnect/lease behavior was added by the power candidate.

## Evidence inspected

- `.evidence/t107/host-after.log`: all 5 host suites green on Wave 4; display HUD 11451 checks, display shell 854 checks, core 166 tests, tools template 81 checks, Python 38 tests.
- `.evidence/t107/audio-host.log`: 159 tests, 0 failures, including executable pure controller/capture tests and production-source wiring assertions. Source assertions alone do not prove the downstream buffer races above.
- `.evidence/t107/power-focused.log`: 15 tests, 0 failures; actual production sleep inclusion.
- `.evidence/t107/power-mutations.log`: 7/7 scratch mutants rejected by assertions.
- `.evidence/t107/display-focused.log`: HUD 11451 checks and shell 854 checks pass.
- `.evidence/t107/display-mutations.log`: 12 scratch mutants rejected; `invalid_or_surviving=0`.
- `.evidence/t107/build-wave3-stopped.log`: initial pinned build was stopped for these repairs.
- `.evidence/t107/build.log`, `.evidence/t107/build.exit`: repaired-candidate build fails with exit 2 before compilation: `Directory '/project/build' doesn't seem to be a CMake build directory. Refusing to automatically delete files in this directory.` The coordinator applied the brief's stop rule for a gate requiring a non-code repair. Firmware/headroom/OTA acceptance is not established.

## Changed artifacts, remaining risk, decision

Changed source artifacts: **none**. Review artifact: `.evidence/t107/break-it-review.md`.

Remaining risk: audio scratch mutation evidence was pending when the run stopped; firmware compilation/headroom and OTA acceptance remain unverified. Hardware codec reopen latency and physical panel/OTA/power behavior require the coordinator's device evidence. Listening and voice remain owner check pending under the explicit ruling. B1-B3 have no unresolved source correctness objection from this review.

Decision needed: coordinator must report the build blocker and stop with the brief's report. A later authorized run can resolve the incomplete build directory and resume the outstanding gates. This reviewer starts no further work.

## Coordinator evidence addendum at stop

The parent subsequently read `audio-repair-mutants-run.log`,
`audio-repair-mutations-v1/results.json` and `restored-test.log`.
All 14 Wave 4 mutants built successfully and were rejected by test assertions;
the restored scratch source passed 166 tests with zero failures/ignored.
Seven mutants exercise executable batch/cursor/export helpers; seven pin
production wiring through source assertions. This distinction does not claim
integrated device concurrency proof. Mutation evidence is now complete locally;
the pinned firmware build blocker and remaining device gates are unchanged.
