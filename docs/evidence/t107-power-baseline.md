# t107 power baseline

Status: resumed pinned build passed with 55.786% app-partition headroom.
OTA readiness is blocked on a nonresponding device; owner wake is pending.
Candidate hardware acceptance and measured improvement remain pending.
Source baseline: `42060d18bbcba590b843c72e513c15bbc35c662f`.

## Method and acceptance boundary

The paired trusted-LAN client reads AXP2101 voltage, percentage, battery/USB/
charge flags, voice state, and display counters every 15 seconds. Each state
uses a 300-second window after 30 seconds settling. The required states after
the orchestrator's ruling are privacy-muted idle, privacy-muted WATCH, and
timer deep sleep. Listening and the spoken-turn proof are owner check pending.
Deep sleep has no HTTP polling; its endpoint readings include shutdown,
boot, and reconnect overhead and must be reported separately from awake slopes.
USB and charging must remain absent. The operator lease must remain inactive.

Local raw evidence is under `.evidence/t107/`: `before-samples.jsonl`,
`power-before.log`, `host-before.log`, and framebuffer mirror captures.
Credentials and network identity are excluded from these records. A software
panel mirror establishes rendered pixels, not physical emission or acoustics.

Use per-minute block means and linear voltage slopes in mV/h; report sampling
scatter, voltage quantization, and the short-window limitation. Percentage is
coarse corroboration, not a current sensor. Different state of charge, voltage
relaxation, temperature, and network work can confound sequential windows.
No estimated current or source-level saving substitutes for a measured result.

The omitted listening window requires physical unmute. The paired `armed=1` request is a
privacy-safe resume and cannot clear the owner's mute (`main/http_routes.c:612`,
`main/main.c:2604`). Synthetic holds/taps cannot clear privacy either
(`main/main.c:3445`, `main/main.c:3519`). Restore privacy after every test.

## Source inventory and estimated priority

The ordering below is an engineering hypothesis, not measured attribution.
There is no battery-current sensor in the current AXP interface.

| Priority | Consumer | Current behavior and source evidence |
|---|---|---|
| 1 | Codec, PA, shared I2S | Both ADC and DAC stay open after boot; PA GPIO46 is asserted at DAC open/unmute and never lowered by `jr_audio`. Capture is four lanes at 24 kHz. `components/jr_audio/src/jr_audio.c:37`, `:428`, `:449`, `:705`, `:904`, `:1047`. Privacy blocks streaming but an active muted text turn still reads and then zeros real samples, `main/main.c:3699`, `:3722`. Closing ADC alone is insufficient: pinned codec 1.5.11 defers paired S3 RX/TX disable until both close (`audio_codec_data_i2s.c:572-586`). Full gating must preserve paced text silence, WakeNet when unmuted, and playback drain. |
| 2 | AMOLED emission and renderer | Already uses 24/12/6/3 FPS plus mood brightness times user cap, `main/main.c:2295-2301`; unchanged FPS and brightness writes short-circuit, `components/jr_display/src/jr_display.c:1410-1459`. Presenter still wakes every 33 ms and invalidates baked WATCH on every pass, `:5170`, `:5251-5257`. N8.12's near-black minute clock remains a separate candidate; old claims of a universal 24 FPS rest policy are stale. |
| 3 | CPU and Wi-Fi | CPU already selects 240 MHz live / 160 MHz rest; PM enabled, automatic DFS and tickless idle disabled, `main/power.c:46-62`, `sdkconfig.defaults:175-184`. Wi-Fi MIN_MODEM already runs at rest, NONE during voice/user activity/lease/OTA, `main/main.c:2275-2288`, `components/jr_net/src/jr_net.c:687-719`. Shared active I2S holds the APB constraint; automatic light-sleep savings are unproven. |
| 4 | IMU and task polling | IMU reads at 100 Hz; gesture thresholds depend on sample timing, `components/jr_imu/src/jr_imu.c:66-79`, `:340-407`. PKEY polls at 500 ms and battery at 1 s, `components/jr_power/src/jr_power.c:193-258`. Main mood/environment tick is 100 ms, `main/main.c:2075-2091`. Playback feeder idle polling is roughly 10 ms, `components/jr_audio/src/jr_audio.c:730-842`. Touch normally waits on IRQ; tools block on their queue, `components/jr_hal/src/input_touch.c:320-328`, `components/jr_tools/src/jr_tools.c:587`. |
| 5 | Network upkeep | Gemini websocket has 20 s pings and TCP keepalive; stop/destroy drains it, `components/jr_transport/src/gemini_device_ws.c:254`, `:267`, `:356`. Passive HTTP server uses defaults, `main/http_routes.c:4382`. Weather's implemented guard skips only DREAM and first hydration bypasses it despite the WHISPER comment, `main/device_tools.c:280-309`. Board poll runs every 90 s outside DREAM, `:481-508`. |
| Existing | Deep sleep | Ten minutes in DREAM, minimum three-minute uptime, USB/OTA/lease exclusions, `main/main.c:2336-2354`, `main/app.h:224-233`. Probation refusal precedes shutdown; IMU/touch wake and timer fallback are armed in `main/power.c:80-155`. The current path does not explicitly close external audio before chip sleep. PKEY cannot wake the S3 from deep sleep on this board. |

## Current display/backlog assessment

The requested N8 fixes are already implemented: segmented STATUS fill,
unknown gauge represented as `--` without a full arc, independent USB/charging
bits, explicit TIME/STATUS headings, and WATCH disc clearing bounded inside
the battery/privacy rim. See `components/jr_display/src/jr_display.c:1775-1781`,
`:2080`, `:2123`, `:3132-3149`, `:3900-3961`. Existing shell tests cover sheet
words and cadence; direct focal-pixel/rim-composition cases would strengthen
proof. Percentage roles are deliberate in the current design; no fresh
contact-sheet defect has been established.

N7.21 remains hardware blocked: a 500 ms PKEY read clears one latched short
press bit, so two presses can collapse before software sees either. Faster
polling after the first observed press cannot recover a collapsed pair.
Do not claim reliable double-tap without physical event evidence.

## Baseline gates

`scripts/host-tests.sh` passed all five suites before any source edits:
HUD 11,451 checks; shell 781 checks; core 137 tests; templates 81 checks;
desk Python 38 tests. No suite was skipped. The device authenticated on battery,
privacy-muted, with OTA state `valid`, no active operator lease, zero transport
drops/deaths, and 28,672 bytes in the largest internal block.

## Before measurements

| State | Window | Drain mV/h | Indicative 95% slope scatter | Rounding budget | Voltage endpoints | Gauge endpoints |
|---|---:|---:|---:|---:|---|---|
| Muted idle, WHISPER | 300 s, 21 samples | 296.2 | +/-84.7 mV/h | +/-29.1 mV/h | 4038 -> 4025 mV | 97 -> 95% |
| Muted WATCH, WHISPER | 300 s, 21 samples | 42.4 | +/-205.9 mV/h | +/-29.1 mV/h | 3978 -> 4004 mV | 94 -> 92% |
| Muted timer sleep cycle | 300 s timer, 313.844 s observed | 11.5 | Not identifiable from two readings | +/-22.9 mV/h | 3997 -> 3996 mV | 92 -> 91% |

The awake fit uses five minute-block means (four readings per block, five in
the last block), with a Student-t residual interval on three degrees of
freedom. This is indicative sampling scatter; it does not bound correlated
noise or battery state-of-charge/temperature drift. The rounding budget
allows a conservative 2 mV differential over the block span; sensor accuracy
and electrochemical relaxation remain additional, unmeasured uncertainty.
WATCH's early voltage rebound makes its fitted slope particularly uncertain.
Sleep is a shutdown/boot/reconnect cycle measurement, not sleep-only current.
Timer wake was observed, and privacy remained muted in every sample.

The live image's immutable revision is unavailable from existing telemetry.
Its WATCH identifies as PRISM, which is absent in this source baseline.
Accordingly, the later comparison is between the observed running image and
the candidate as a whole; attribution to one change is not established.
`before-dream.png` shows a transient, synthetic face-down DREAM on the real
device at brightness 5, with the emote still rendered. This is separate from
the battery windows and is the before proof for N8.12.

## Bounded implementation choice

N8.12 is the daily feature: a quiet, near-black minute clock during genuine
idle DREAM, with battery and privacy rims retained. Foreground choices,
captions, operator activity, USB and OTA/probation keep the normal glass.
Input restores the same emote through the existing display owner. Existing
N8 battery, heading, charging and WATCH rim fixes need no new behavior change.
N7.21 remains blocked on physical PKEY event resolution.

The main battery change closes the external ADC/DAC pair and PA while privacy
is muted and playback has drained. Muted text turns receive paced zero frames
at the capture boundary, before DSP or diagnostic taps. Unmuted WakeNet retains
its ADC and clock peer; rest never grants or removes microphone authority.
Sleep requires successful audio quiescence after the existing probation guard.
Weather's first fetch and retries now share the existing lit-state/network
gates, preserving their intervals. No DFS, light-sleep, IMU timing, ladder,
pairing, lease, brain or gateway settings are changed.

## Interrupted proof attempt — blocked before OTA

All five final host suites passed: HUD **11,451 checks**, shell **854 checks**,
core **166 tests**, templates **81 checks**, desk Python **38 tests**.
The baseline counts were 11,451 / 781 / 137 / 81 / 38; none decreased.
Scratch mutation proof rejected 7/7 power, 12/12 display, 21/21 initial audio
and 14/14 privacy-repair mutants. Repair scratch restoration passed 166 tests.
Audio proof combines executable production helpers and explicit source wiring
assertions; it does not establish device concurrency or codec timing.
The candidate includes audio power gating, the guarded N8.12 DREAM clock,
rest-aware weather scheduling, and three privacy repairs identified by the
independent review: retained half-batches, absolute diagnostic cursor continuity,
and companion microphone revalidation after copying PCM.

The pinned `scripts/build-v5.sh` gate failed before firmware compilation:

> Directory '/project/build' doesn't seem to be a CMake build directory.
> Refusing to automatically delete files in this directory.

The preceding dependency/configuration run was deliberately stopped before
compilation to repair the privacy findings. That interruption left an incomplete
generated build directory without `CMakeCache.txt` or `sdkconfig`; the next
build's `set-target`/`fullclean` refuses that directory. This is a generated
workspace repair, not a firmware code fix. The brief explicitly requires
stopping for this kind of gate failure. No cleanup or retry was performed.
See [the build log](t107-power/build.log) and
[the final host log](t107-power/host-after.log).

No candidate image SHA or app-partition headroom is available. No OTA,
after measurements, after panel captures, or commit occurred. There is no
measured improvement claim. N8.12 remains locally implemented and unaccepted
on hardware; N7.21 remains hardware blocked.

Final authenticated readback of the unchanged running device confirmed
privacy muted, voice task alive, inactive operator lease, OTA state `valid`,
zero display flush errors, and 28,672 bytes in the largest internal block.
This readback validates the existing device, not the candidate. Listening and
voice remain **owner check pending**. The OTA stage was not reached.

Four waves used seven unique subagents: wave 1 `w1_power`, `w1_display`,
`w1_audio`; wave 2 `w2_power`, `w2_display`, `w2_audio`; wave 3 parent gates
and `w3_break_it`; wave 4 reused `w2_audio` for the three privacy repairs.
The parent alone performed device measurements and final readback.

## Resumption ruling — cleanup rejected

The orchestrator authorized deleting only this worktree's gitignored `build/`
and rerunning the pinned build with `RECONFIGURE=1`. Before deletion, the parent
verified that the resolved absolute path is exactly this worktree's `build/`,
that its attributes show a regular directory without a link/reparse target,
and that `git check-ignore -- build/` succeeds.

The execution tool rejected both the guarded cleanup and the separate native
PowerShell `Remove-Item -LiteralPath <exact-build-path> -Recurse -Force` command.
Its only stated reason was **blocked by policy**. No cleanup, rebuild, commit,
OTA or further device measurement occurred during this resumed attempt.
The earlier host and mutation results remain the verified local candidate
evidence. Privacy remains under the physical owner's authority; the one-turn
voice test has not reached its owner-unmute stage.

## Resumed acceptance

The orchestrator subsequently removed this worktree's `build/` directly.
The parent confirmed its absence and restarted the prescribed pinned build
with `RECONFIGURE=1`, as explicitly authorized despite unchanged SDK defaults.
Firmware source remains frozen at the tested repair; all six recorded audio
repair hashes still match. The host counts and 54 scratch mutant rejections
above remain current. The requested commit precedes OTA. Listening-state
measurement remains excluded; the one voice turn now requires a pause for
the owner's physical unmute after muted device verification.

### Successful build and pre-OTA stop

`RECONFIGURE=1 ./scripts/build-v5.sh` completed with exit 0 using ESP-IDF 5.5.4.
Both managed-patch and board-symbol checks passed. The image is **1,854,464
bytes**; the smallest app partition is **4,194,304 bytes**, leaving **2,339,840
bytes (55.786%) free**. Its SHA-256 is
`6292894e5662c47b0276152aa7aa0ac0cd179310d5bfa69049c5a64171633a7b`.
See [image metadata](t107-power/image.json),
[successful build tail](t107-power/build-resumed-tail.log), and
[exit code](t107-power/build-resumed.exit). Firmware source is unchanged from
the green host/mutation freeze. Public candidate files pass the repository's
secret rules; the private brief and scratch evidence are excluded from that
public scan. Shell syntax and whitespace checks pass.

The pre-OTA readiness request to the previously verified paired device timed
out before receiving health. No candidate upload was attempted. The last
successful pre-change readback was muted and in DREAM; automatic deep sleep is
a plausible explanation, not a diagnosis. The owner wake request is pending.
This stop does not require unmuting: the owner must wake the device while
leaving privacy muted, then the parent can continue OTA and the same 300-second
idle/WATCH/sleep windows and after captures. The one spoken turn comes after
those muted checks, with a separate pause for physical unmute.

The local candidate and its proof are committed before OTA under the explicit
orchestrator order. No hardware closure, after voltage result, new panel proof,
or measured improvement is claimed. Listening-state measurement is excluded;
voice remains **owner check pending**. Four waves used seven unique subagents.
