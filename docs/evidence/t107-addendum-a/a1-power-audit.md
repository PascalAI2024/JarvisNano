# Addendum A power audit and measurement handoff

Parent: t107/addendum-a, A/w1/power. Executor: /root/w2_power.
Baseline: t107/codex at 6f97afb. Read-only audit completed 2026-10-11.
This audit made no source changes, device calls, MCP calls or commits.

## Result

The AXP2101 exposes battery voltage and fuel-gauge percentage, but its pinned
public driver and datasheet do not expose measured battery-current magnitude
or a readable coulomb counter. A current endpoint cannot be honestly assembled
from its charger settings. Longer matched voltage windows remain necessary;
their error bars must separate sample scatter and quantization from unbounded
battery relaxation/SoC/temperature bias.

The first new 60.109631-second muted HOME task-runtime pair establishes
gfx_render as the largest measured CPU consumer: 24.310% of one core and
62.1% of summed non-IDLE task runtime. This is a CPU ranking, not a current or
electrical-power ranking. The start includes a short AWAKE transition and
240-to-160 MHz change, so a steady WATCH pair is required before choosing its
cadence predicate.

The parent's panel walk also found an actual PSRAM exhaustion defect: a screen
request needs a 434312-byte full-frame copy, while failure health reports only
419300 bytes free. Baked-dial compressed sources are retained after decoding
into a separate shared frame. A bounded dial-source lifetime fix is justified;
blind eviction of fast voice assets is not.

## Current-reading feasibility: local primary evidence

Read the pinned local AXP2101_SWcharge_V1.0.pdf under
docs/reference/vendor/datasheets and XPowersAXP2101.tpp plus
REG/AXP2101Constants.h under docs/reference/vendor/drivers/XPowersLib-src.
The vendor INDEX.md records their Waveshare provenance. No web fallback was
needed.

- Datasheet section 6.10, pages 23 and 32-33: SAR ADC channels are VBAT, VBUS,
  VSYS, TS and die temperature. VBAT has a 1 mV code step. The high byte is
  read before the low byte. No current channel is listed.
- Section 6.11, pages 23-25: fuel gauge exports percentage at 0xA4 and battery
  voltage at 0x34/0x35; battery-parameter learning/programming is described,
  without a documented current or coulomb-counter readout.
- Complete public register map, pages 24-25: 0x61/0x62/0x63 are precharge,
  constant charging current and termination settings. 0x16 is the input-current
  limit. They are configuration values, not observed device current.
- Status2 at 0x01, page 26, bits 6:5 reports standby/charging/discharging
  direction only. XPowers getters at lines 495-513 and 2386-2419 read limits
  and charge-current settings. Voltage and percentage getters are at 2232-2318.
- Electrical-characteristics page 10 gives PMIC IBAT typical 40 uA under its
  stated restricted rail conditions. It is not this board's deep-sleep draw.
  Charger-voltage regulation accuracy is not ADC measurement accuracy; no
  specified VBAT ADC calibration/accuracy bound was found in the public table.

Decision: do not add a fictional mA metric, undocumented register probing,
fuel-gauge parameter writes or rail changes. If electrical current is required,
it needs an owner-operated external meter; no such measurement exists here.

## Measured remaining CPU cost

Evidence: .evidence/t107a/live-before/tasks-start.json and tasks-end.json.
Subtract uint32 runtime counters modulo 2^32, matched by task name. The total
runtime counter is one wall-clock denominator; two IDLE tasks represent the
two cores. Do not divide individual task deltas by lifetime counters or call
the sum a one-core CPU percentage.

| Task | Delta runtime, us | Percent of one wall-clock core |
|---|---:|---:|
| gfx_render | 14612387 | 24.310 |
| jr_voice | 3866700 | 6.433 |
| wifi | 2476562 | 4.120 |
| jr_imu | 1990292 | 3.311 |
| Tmr Svc | 275014 | 0.458 |
| esp_timer | 130209 | 0.217 |
| jr_power | 79807 | 0.133 |
| jr_present | 54618 | 0.091 |
| tiT | 44478 | 0.074 |
| httpd | 8624 | 0.014 |
| jr_pb_feed | 3449 | 0.006 |

IDLE0 is 67.854%; IDLE1 is 92.975%. Overall two-core CPU busy is 19.586%.
jr_tools and touch did no measured work in this pair. Start/end contiguous
internal free block stayed 30720 bytes. The parent reports HOME was entered
only about 1.2 seconds before the first snapshot; repeat steady state and WATCH
are needed to remove that initial transition. A renderer optimization targets
the largest demonstrated CPU burden. Panel electrical power, radio energy and
CPU-frequency savings remain hypotheses without paired battery evidence.

## Runtime and invalidation audit

| Consumer | Current source policy | Finding and boundary |
|---|---|---|
| Render/panel | app.h:85-88, live/ambient/WHISPER/DREAM 24/12/6/3 FPS; jr_display.c:5472 refreshes a blitted dial every presenter pass | Baked WATCH always schedules a full frame, even DRESS with no seconds. Preserve sweep hands, finite fades, rim changes and foreground lanes when coalescing. |
| DREAM glass | jr_display.c:4972 wait up to 1 second; 4998-5031 stops animation and invalidates only minute/rim/brightness changes | Already reduced to sparse panel updates. Empty engine handler cadence remains; do not mistake it for panel rendering. |
| FPS diagnostics | managed esp_emote_gfx/src/core/gfx_render.c:234-285 counts handler calls before skipping dirty_count==0 | Actual FPS is not panel-flush FPS. Compare flush_completions deltas as well as runtime. Full 466-row frames normally mean 39 twelve-row strips; partial frames differ. |
| CPU | power.c:48-65 pins min=max 240/160, automatic light sleep false; sdkconfig.defaults:178-184 enables PM/stats but disables automatic DFS and tickless idle | No measured basis for a config change in this wave. A lower gear must separately prove wake/audio/OTA behavior. |
| Voice owner | main.c:2030 loop, 3930 shell publish, 3976 muted unpaced delay 20 ms | About 50 wakes/s; shell link/task state is checked each loop but telemetry payload is 1 Hz. New slower waits need a wake-notification path and latency proof. |
| IMU | jr_imu.c:70, 350-407, 10 ms sampling plus five-sample motion statistics | About 100 wakes/s and floating-point tilt/math; not the largest measured CPU cost. Slowing it changes gesture windows and needs physical wake proof. |
| PMIC | jr_power.c:34,57,225-258, PKEY 500 ms and battery 1000 ms | 0.133% CPU in pair; lower priority than rendering. PMIC polling changes PKEY latency. |
| Playback/audio | jr_audio.c:892 idle feeder waits on notification up to 1 second; prepare_sleep bounded 200 ms | Privacy-muted codec/PA/mic already off in live health; retain unmuted WakeNet and sleep refusal ordering. |
| Wi-Fi | jr_net.c:1206-1228 applies MIN_MODEM when eligible; main uses NONE for armed realtime/busy/lease/OTA | Existing policy should stay. Network sampling itself perturbs the measured radio load. |
| Tools/keepalive | device_tools.c:316-317 board initial 30 s, then 90 s, skips DREAM; weather helper excludes WHISPER/DREAM; Gemini ping 20 s while open | No tool work in runtime pair. Avoid suppressing active task updates solely for a hypothetical saving. Muted idle live socket is closed. |
| Sleep | mood.h:44,48 natural DREAM at 15 min, sleep 10 min later; waits /4 at SoC<=20; main.c:2342 excludes USB/OTA/lease, power.c refuses probation first | Do not extend timers merely to make a benchmark fit. Timer wake restores natural DREAM without unmuting. |

Many setters already coalesce equal packed words: shell, brightness/FPS,
HUD environment, clock-minute DREAM notifications. Links-word RSSI/temperature
changes can still notify the presenter; a notification is not necessarily a
panel update. The measured bottleneck is rendering rather than presenter work.

## Actual PSRAM failure and bounded lifetime design

Evidence: .evidence/t107a/walk-failure-health.log; failed
live-before/watch-style-2.png.ppm has zero bytes. Parent's cockpit confirms the
failed style was DIVER; filename suffix is sweep order, not the source enum.
Original persisted style was FUTURE. No silent style remapping is inferred.

- http_routes.c:4271 allocates a 466*466*2 = 434312-byte immutable HTTP frame.
  With total free PSRAM 419300 this must fail. Its 503 conflates allocation and
  mirror readiness; it is not evidence of lost panel flushing.
- jr_display.c:369-416 lazily retains two snapshot frames = 868624 bytes.
- dial_decode at 5098 retains one decoded dial frame = 434312 bytes.
- clip_load at 342 retains compressed clips; successful loads are not freed.
  Current local reactive assets total 5337706 bytes, exceeding the old ~3.9 MB
  comment. DIVER=204059, DRESS=205999, PILOT=85369, FUTURE=143191 bytes.
  Those local sizes do not prove the live art image's total resident set.
- gfx_anim_stop at vendor widget/gfx_anim.c:1063 only stops playback; hidden
  animation retains its file_desc. EAF entries point into the source data
  (gfx_eaf_dec.c:737-742). Successful set_src deinitializes the previous parser
  at gfx_anim.c:980-985. ctx->active=NULL at dial_show:5174 does not detach it.

Selected narrow design: release compressed baked-dial data after successful
decode when it is not the vendor-bound source; skip clip loading entirely when
the shared decoded frame already holds that dial. Track the vendor-bound clip
independently of active/shown state. Release a formerly bound baked source only
after successful anim rebind. Preserve voice clip residency and failure/fallback
sources. DIVER+DRESS compressed data total 410058 bytes; releasing them would
cover the observed 15012-byte deficit, but the live after proof must establish
actual memory recovery and repeated-style capture success.

Exclusive implementation files accepted by parent: jr_display.c,
components/jr_display/tests/test_shell.c, tests/probe/stubs/idf_stubs.inc.
Production-used tests must exercise tracked heap ownership: repeated same dial
does not reload; all-style cycles do not accumulate compressed dial memory;
decode/header/lock failure retains anim fallback; failed rebind retains old
source; successful rebind frees only a no-longer-referenced baked source.
Scratch mutations must reject omitted release, repeated same-dial reload and
premature release. No broad voice-cache eviction in this change.

## Matched longer measurement and honest error bars

The t107 300-second voltage slopes cannot establish a saving: WATCH before and
both awake after windows rebounded, and sleep rose from 3892 to 3903 mV after
317.484 s. Cell relaxation, SoC and temperature swamp those short differences.
The earlier scatter interval assumed independent residuals and must not be
presented as a bound on total measurement error.

Prespecify the method before inspecting outcomes:

1. Use the same original FUTURE style, immutable image identity, privacy=true,
   no USB/charge, no lease, no active voice/tools/OTA/probation, matched brightness
   and renderer policy. Keep network/AP and sample endpoints/cadence identical.
   Record wall and uptime, voltage/percentage, USB/charge, mood, policy FPS,
   CPU MHz, actual brightness, audio power, radio-saving state and counters.
   Record initial SoC/temperature and every unexpected state transition.
2. HOME muted idle and WATCH each: 120 s settling then 720 s retained window.
   Total 14 min fits the healthy-cell 15 min natural WHISPER limit. Begin each
   after a meaningful attention reset and validate no DREAM transition during
   retained samples. At <=20% saver these durations do not fit; do not silently
   compare another state. Sample every 15 s, without screenshots/leases/tasks
   endpoints inside the slope window. Obtain runtime/flush pairs separately.
3. Sleep: matched 720 s timer or, preferably, 1800 s timer if run budget permits.
   Read battery immediately before sleep and after the same post-wake delay.
   Use actual elapsed duration, record timer wake and aborted/no-wake cases.
   Sleep's CPU/radio silence is useful proof, not an isolated PMIC current read.
4. DREAM: enter through validated muted timer wake, which restores natural
   DREAM; 120 s settling plus 420 s retained window fits the existing 10 min
   DREAM-to-sleep timeout. Validate unchanged eligibility. A 720 s DREAM window
   cannot fit without changing policy or introducing a disqualifying lease/USB.
   Do not use the transient three-second synthetic flip as a long DREAM fixture.
5. Run the identical sequence on before and after images. Prefer at least two
   valid cycles per image, with order/image alternation if the owner accepts the
   extra OTA/time; one chronological pair remains relaxation/SoC confounded.
   Keep all raw windows and rejection reasons; never repeat only an adverse run.

Report endpoint mV/h for each window and plot all voltages. Endpoint uncertainty
using the conservative existing 2 mV differential quantization bound is
+/-10 mV/h for 720 s, +/-4 mV/h for 1800 s, and +/-17.1 mV/h for 420 s. This
is quantization only; ADC calibration error and battery relaxation remain.

For awake 720 s, six nonoverlapping two-minute block means give a conditional
OLS slope and residual-scatter half-width t(4)=2.776*SE. Use actual block-time
centres. Propagate the conservative +/-1 mV rounding bound of each mean through
the actual regression weights: sum(abs(x-xbar))/Sxx, where x is in hours.
This is about +/-15 mV/h for equally spaced awake blocks. For 420 s DREAM,
seven one-minute means give t(5)=2.571*SE and about +/-26 mV/h under those
weights. The two-endpoint 2 mV/span rule does not bound a multi-point OLS slope.
Label these as
conditional sample-scatter intervals, not a 95% total error bound: blocks remain
correlated. An after-minus-before improvement needs the two conservative
half-widths/quantization bounds combined and still cannot erase drift bias.
Sleep has only two endpoint samples, so no fitted-scatter confidence interval.
Fuel-gauge percent is coarse with undocumented accuracy; do not turn it into
coulombs or a precise current estimate.

Acceptance: report measured CPU/flush reduction separately from battery drain.
Claim improved battery drain only when a positive matched difference exceeds
the stated measurement uncertainty and repeated matched windows support it.
If rebound persists, mark battery acceptance unresolved instead of converting
CPU counts into an invented battery-life percentage.

## Handoff and remaining decision

Await the parent's steady WATCH profile before specifying/coding dirty cadence.
Selected display fixes also include finite fades using actual elapsed time,
duplicate byte-order helper subtraction, and retrying DREAM/snapshot/pattern
refresh after failure; the separate UX/code audits specify their tests.
Listening/voice remains owner check pending. Preserve privacy, WakeNet,
probation/OTA, lease semantics, task-stack policy and contiguous TLS headroom.
Parent owns baseline, device recovery, gates, scratch-root validation, commit,
OTA and all live captures. This audit does not establish a battery improvement.

## Steady WATCH evidence and selected implementation

The parent's `.evidence/t107a/watch-profile.jsonl` captures matched FUTURE,
style 5, face 11, privacy=true, brightness 14, 160 MHz, target WHISPER 6 fps.
Across 60.319479 s, 355 frames were rendered (5.885 frames/s; integer diagnostic
5 is not a configured 5 fps target). Software rendering consumed 13,189,701 us,
21.866% of wall time, or 37.154 ms/frame. The gfx_render task used 18,746,977 us,
31.079% of one CPU core; voice 6.602%, wifi 4.454%, imu 3.230%, tools 0.879%.
This makes display rendering the next evidenced CPU cost. It does not establish
the ranking of electrical current, which the pinned PMIC cannot meter here.

WHISPER stays at 6 fps. DRESS-only coalescing would not affect this matched
FUTURE window. A scratch geometric hand cull retained pixels but did not show
a stable gain and was rejected. The bounded follow-on candidate restricts
battery-rim arc traversal to the two monotone angular ranges that can reach
each strip, retaining original ascending writes and all charging comet writes.
It uses no allocation, persistent cache, public API or cadence change.

The accepted display correctness and memory fixes are frozen separately:
finite shell fade elapsed time, one panel byte-order helper, refresh-failure
retry, and release of compressed baked dial sources only after vendor parser
ownership has ended. Fast voice clips remain resident. Focused verification
is 11,451 HUD checks plus 951 shell checks; scratch control passes and all ten
compiled mutants fail assertions. Detailed evidence and source ownership are
in `a1-power-w2-result.md`. Battery-rim adoption remains conditional on exact
full-path pixel comparisons, bounded operation counts and stable alternating
runtime measurements.
