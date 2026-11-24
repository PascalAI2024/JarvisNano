# t107 Addendum A — muted UX and render-path work

Date: 2026-10-11. Branch: `t107/codex`, continued from `6f97afb`.
Status: **BLOCKED — gate `after-long` failed: `timer wake failed`.**
After the owner's reported manual wake at 00:12Z on 2026-10-12, paired reads
at 00:15:40Z succeed: uptime 231,756 ms, wake `touch`, `ota_0 state=valid`,
privacy muted, capture stopped and mic/ADC/DAC/PA off. The current manual
wake does not satisfy the failed timer gate; historical wake timing is unresolved.
The owner reported one hand-unmuted spoken turn and an audible answer
(reply `yes`, 2026-10-11). This is accepted as owner-observed voice evidence.
The tested image was not supplied, and the Addendum image had not been
uploaded when the report arrived. Paired readback verified privacy already
muted, capture stopped and mic/ADC/DAC/PA off. The parent never unmutes the
device and alone operates it and commits. Listening-state power is excluded.

## Evidence and selected work

The live sweep covered JARVIS, WATCH, WEATHER, STATUS, ACTIVITY, conditional
DESK, their production-reachable detail sheets, controls, all six watch styles,
three choice layouts, five transient card kinds and a remote canvas fixture.
Screenshots were opened as contact sheets. Failure/empty states are exercised
by shell fixtures rather than deliberately disrupting live services. JARVIS's
SESSION sheet has no production hit target; physical pairing, hardware test
and listening remain owner checks. No typography defect justified a
new layout change.

Ranked by daily exposure:

| Rank | Defect and change | Before/after proof |
|---|---|---|
| 1 | DOWN from DETAIL closed the sheet and advanced to another panel. It now closes on the same panel; the following DOWN resumes navigation. | WATCH [before](t107-addendum-a/live-before/watch-detail-down.png) / [after](t107-addendum-a/live-after/watch-detail-down.png); production-routing regression and restored-defect mutation. |
| 2 | Outside-sheet dismissal printed `CONTROLS - UP TO CLOSE` on an ordinary panel. It now restores that panel's caption, with WATCH's caption cleared. | WEATHER [before](t107-addendum-a/live-before/weather-detail-dismiss.png) / [after](t107-addendum-a/live-after/weather-detail-dismiss.png); all five detail panels in routing tests. |
| 3 | Finite fades consumed at most 100 ms per frame, stretching transitions at rest cadence. They now consume elapsed time, bounded by the longest fade; brightness keeps its existing slew cap. | Fake-clock production tests at 50/200/500 ms; endpoint [before](t107-addendum-a/live-before/slide-800.png) / [after](t107-addendum-a/live-after/slide-800.png). CLI latency prevents a subframe live timing claim. |
| 4 | Cycling baked dials retained compressed sources and exhausted snapshot headroom. Successfully decoded, unbound dial sources can be released; the stopped vendor parser's source remains alive until successful rebinding. | [Before HTTP 503](t107-addendum-a/cache-before.json) / [after DIVER](t107-addendum-a/live-after/style-2-diver.png); allocation/decode/rebind fault tests and three successful style cycles. |

Code subtraction removes preprocessor-excluded tilt arithmetic and a duplicate
RGB565 byte-order helper. Failed presenter locks now preserve DREAM keys,
snapshot requests and pattern acknowledgement for retry. No new task, mutex,
cross-main header, privacy path or cadence policy is introduced.
Production `.c`/`.h` changes remove 133 lines and add 160. The dead-path
subtraction alone removes 42 and adds six (net 36 removed); test and evidence
lines are counted separately from runtime source.

## Power method

The next measured cost is WATCH rendering. The stable muted FUTURE baseline
rendered 355 frames in 60.319479 seconds (5.885 frames/s), averaging 37.154 ms
of software rendering per frame. `gfx_render` used 31.079% of one core, the
largest non-idle task. Runtime and render counters are computational evidence,
not battery-current measurements.

The battery rim now searches the two monotone angular ranges that can reach
each render strip, retaining the original ascending writes and charging comet.
The fixed FUTURE/69% frame uses 191 polar visits instead of 6,903; search probes
remain additional work. There is no allocation, persistent cache, new public
API or frame-rate change. All 28,566 full-frame comparisons match the frozen
renderer. Restoring the old traversal preserves pixels but fails a conservative
400-visit host bound; both non-equivalent strip-edge mutants fail pixel tests.
The repeated endpoint omission is an equivalent mutant and is recorded honestly.

All 21 alternating host timing runs improve process CPU time. Median gains
are 3.93% in the initial `-Os` batch, 4.07% in its repeat and 2.68% at `-O2`.
The benchmark executes the production software overlay stages in one process,
with separately compiled HUD versions, matching fixtures and complete pixel
comparison. It excludes physical panel/DMA and vendor animation decoding.
These host results justify the bounded hot-path change, not a battery claim.

The pinned AXP2101 datasheet and resolved driver expose battery voltage,
fuel-gauge percentage and charge direction. They do not provide a measured
battery-current channel or a readable coulomb counter. Charge-current limits
are configuration values, not measured current.

Longer matched battery windows were prespecified before inspecting outcomes:
fresh muted timer boot; 120 s settling then 720 s each of HOME idle and FUTURE
WATCH; 1800 s timer sleep; genuine timer-wake DREAM with 120 s settling and
420 s retained. Awake samples use identical endpoints every 15 s. No lease,
USB, charging or screenshots occur inside retained windows. The owner privacy
verification made two health GETs and one cockpit GET at 22:46:42Z, coinciding
with the baseline DREAM first-sample timestamp. This extra boundary traffic
is retained and disclosed; no control write was needed. Brightness cap/volume
remain 65/95 and the cell must remain above 20%.

Awake fits use six two-minute block means (`t(4)=2.776`); DREAM uses seven
one-minute means (`t(5)=2.571`). Conditional scatter intervals assume independent
blocks. A conservative 1 mV bound per block mean is propagated through the
actual OLS weights; two-endpoint slopes use a 2 mV differential bound.
ADC calibration, correlated cell relaxation, temperature, SoC and chronological
image order remain unbounded. Sleep's two endpoints include shutdown, boot
and reconnect. A lower CPU count must not be converted into an invented
battery-life percentage.

## Acceptance record

### Measured power and runtime

Both images retain 49 samples across 720 seconds for idle and WATCH, at
15-second cadence, muted WHISPER, brightness 14, cap/volume 65/95, no USB,
charging or lease. The baseline also completed 1801.532 seconds of timer
sleep and 29 DREAM samples over 420 seconds at brightness 5. The after run
retains its sleep-start point only: no sleep-end or DREAM observations exist.
Missing data is not substituted or selectively rerun.

| State | Before drain mV/h | After drain mV/h | Conservative rounding bound mV/h |
|---|---:|---:|---:|
| Idle, 12 min | 29.42 ±34.84 | 100.20 ±25.77 | ±15.26 each fit |
| WATCH, 12 min | 65.83 ±10.52 | 126.56 ±14.52 | ±15.26 each fit |
| DREAM, 7 min | 40.70 ±101.37 | Missing | ±25.33 before fit |
| Sleep cycle, 30 min | −7.99 | Missing | ±4.00 before endpoint slope |

The ± values beside awake fits are conditional 95% residual-scatter
intervals, separate from rounding and the unbounded systematic effects
described above. Baseline idle endpoints rose 3784→3815 mV; after idle fell
3759→3738 mV. WATCH endpoints were 3805→3802 versus 3737→3712 mV.
SoC was 66→64% versus 57→54% in idle, and 64→62% versus 53→49% in WATCH.
These larger after voltage-drain fits do **not** demonstrate a saving or
isolate firmware effects from cell state. They are a **possible battery
regression**, not explained away by cell state. Battery acceptance is unproven.
The [plot](t107-addendum-a/battery-windows.png) retains every point and labels
the absent after observations.

The short live WATCH profiles averaged 37.154→46.078 ms/software frame
(24.02% slower), with `gfx_render` 31.079→36.191% of one core. The full
12-minute WATCH windows instead averaged 45.438→43.790 ms/frame (3.63%
lower), at 5.875→5.878 frames/s. The short profiles both use 160 MHz; all
windows use brightness 14. Clock content, scheduling, memory and SoC differ.
The before short profile did not
retain battery values; its nearby capture was 69%, versus 58% after.
Both runtime results are retained. Neither proves electrical improvement.

### Failed after gate

The 1800-second timer sleep began at 2026-10-11T23:30:28Z. The bounded
timer-wake retry ended with `RuntimeError: timer wake failed`; the cleanup
`armed=0` request also timed out. `after-long.exit` is 1. One final
authenticated health request timed out at 2026-10-12T00:02:41Z.
This establishes API unavailability, not whether the timer, boot or Wi-Fi
caused it. No USB flash, remote unmute or extra power-window run was attempted.
The last authenticated sleep boundary was `ota_0 state=valid`, privacy muted,
voice alive, mic/ADC/DAC/PA off, no lease, zero flush errors and 30,720 bytes
of contiguous internal memory. The later manual recovery supplies a fresh
muted/valid health readback, documented below; the original failed window
remains incomplete. The owner-observed voice acceptance remains recorded.

The combined host gate passes all six suites: HUD 11,451; shell 2,200;
production main routing 109; core 166; templates 81; desk 38. No prior check
count dropped. Ten display correctness mutants, two routing mutants and the
disabled-tilt mutant fail their assertions; rim work and strip-edge mutations
also fail, with the duplicate endpoint mutant identified as equivalent.
The pinned ESP-IDF 5.5.4 firmware build passes. Its 1,855,168-byte image
leaves 2,339,136 bytes (55.769%) free in each 4 MiB app partition.
Source commit `7578238` is bound to the unchanged image SHA-256
`7f2c42e3b9f3552912ce7188e859ddba2ff9d3725488adf36703ad746686e4b3`.
The full-length paired upload succeeded. Running/boot slot changed from
`ota_1` to `ota_0`, which reached `state=valid` at uptime 59,727 ms.
Authenticated health reports zero flush errors, privacy muted, audio off,
inactive operator lease and 31,744 bytes of contiguous internal memory.
The build's project version is `1`; the device API exposes no partition hash
or app version, so identity is the local source/image manifest plus upload,
slot change and authenticated reboot readback rather than a device hash.

Post-OTA capture proof includes ordinary panels, detail/DOWN endpoints and
three complete cycles of all six watch styles without resetting the cache.
All 32 captures succeeded; asset-load and flush errors remained zero.
Minimum contiguous internal memory was 31,744 bytes. Per-cycle minimum free
PSRAM was 3,881,584 / 3,878,000 / 3,881,124 bytes, avoiding the prior snapshot
exhaustion. Selected safe images were opened and reviewed. STATUS detail and
ACTIVITY owner-bearing captures remain private. Routing and captions are
verified in composed mirrors; fade duration is proved by fake-clock tests,
since CLI latency prevents physical subframe timing acceptance.

The source fixes, host/mutation/build gates, OTA probation and composed panel
captures are verified. Complete paired sleep/DREAM proof and measured battery
improvement remain unaccepted. The prior t107 measurements remain in
[their separate report](t107-power-baseline.md). The reviewed receipts and
reproduction commands are in [the evidence bundle](t107-addendum-a/README.md).

### Orchestrator-directed readback retry, 2026-10-12 UTC

Following the orchestrator's reported 2/2 ping replies at 00:09Z, the parent
retried paired, authenticated GETs only. Health at 00:10:37Z, cockpit at
00:10:45Z and sleep diagnostics at 00:10:53Z each timed out after about eight
seconds. [The retry receipt](t107-recovery-readback.json) retains the timestamps
and source comparisons. No device write, unmute, reboot, OTA or USB flash was
performed. Original power observations and failed-gate receipts are preserved.

At that retry no fresh uptime, boot/wake reason, sleep counter, OTA state or
privacy value could be obtained. The previous values were valid/muted/audio off.
Ping alone cannot distinguish a late timer wake, a timely wake with delayed
Wi-Fi/API availability, or a later reboot/manual wake. None of those causes
is established by these receipts.

**Regression assessment:** the live acceptance result regressed relative to
baseline `0b4108f`, which completed the same 1800-second timer cycle in
1801.532 seconds. A firmware-caused timer or Wi-Fi regression in `7578238`
is **possible, not proved**. It must remain open until boot/wake diagnostics
are available; unchanged source does not establish unchanged runtime behavior.

The baseline and candidate have byte-identical `main/power.c`,
`main/http_routes.c`, `components/jr_audio/src/jr_audio.c`,
`components/jr_dsp/src/audio_power.c`, `components/jr_net/src/jr_net.c` and
`sdkconfig.defaults`. Muted audio gating and sleep preparation therefore were
already exercised by the successful baseline. `enter_deep_sleep` still shuts
down audio before arming the RTC timer (`main/power.c:107-165`). The Addendum's
main changes are shell routing/captions; network startup (`main/main.c:4236`)
and timer-wake/privacy restoration (`main/main.c:4427`) are unchanged.

The quiet DREAM clock text and drawing functions are unchanged. The Addendum
**does** change DREAM presenter acknowledgement: at
`components/jr_display/src/jr_display.c:5037`, minute/rim keys advance only
after successful refresh submission, so failed render locks retry. It retains
the existing lock and introduces no timer, Wi-Fi or wake-state write. Display
startup creates its task and returns (`jr_display.c:5526`), rather than waiting
for DREAM rendering before network startup. This bounded source review finds
no direct wake/Wi-Fi bring-up defect, but cannot exclude an indirect display,
scheduling or memory fault without diagnostics from the affected boot.

**Drain assessment:** the idle fit is 3.41 times baseline and WATCH is 1.92
times baseline; both are adverse observations and a **possible battery
regression**. Different SoC is measured (idle 66→64% before versus 57→54%
after; WATCH 64→62% versus 53→49%). Baseline idle voltage rises 31 mV despite
falling reported SoC, showing that voltage is not a direct consumption meter
in this run. This supports relaxation/SoC as plausible confounds, not a proven
explanation for the worse fits. There is no matched-SoC crossover, temperature
record, current channel or energy measurement to assign causality. The after
SoC also falls faster. Conditional scatter and rounding bounds do not remove
these uncontrolled effects. The disagreeing short/long render profiles do not
resolve electrical consumption. No battery improvement is accepted.

### Owner manual recovery, 2026-10-12 UTC

The owner reported the Nano back on at 00:12Z and instructed that recovery
be treated as manual. Authenticated health, cockpit and sleep GETs at
00:15:40Z all succeed. Cockpit uptime is **231,756 ms** (3 min 51.756 s);
subtracting it from the request interval estimates boot around **00:11:49Z**.
That is about 11 seconds before the minute-granular owner update and agrees
with a manual recovery around then. The hardware sleep wake cause is
**`touch`**, not `timer`. The current boot is owner/manual recovery regardless
of that small timestamp difference. Running and boot partitions are both
`ota_0`, OTA is inactive and **`state=valid`**, with `ESP_OK` and no invalid
image recorded. Privacy is **muted** in health and cockpit, capture is false,
all four audio power paths are off, voice is alive, flush errors are zero and
the largest internal block is 31,744 bytes. No device write was required.
[The manual recovery receipt](t107-manual-recovery-readback.json) retains
request timestamps, the safe state and private raw-response hashes.

The current app log starts at uptime 877 ms. Wi-Fi initializes at 1,047 ms,
gets an IP at 5,037 ms and boot completes at 5,617 ms with `starting MUTED
(privacy default), woke from sleep`; the later glance record says `touch`.
This verifies normal Wi-Fi bring-up on the **manual boot only**. It says
nothing about the preceding timer boot. Wall-clock seeding reports SNTP,
`rtc_seed=0`; the board has no PCF85063 clock. This wall clock is not a
persisted wake timestamp.

**Reset and retained-log limits:** the firmware does not read or expose
`esp_reset_reason`; sleep wake cause is a different value. The ROM reset
banner precedes the app's log hook, so it is absent from the fetched log.
The supposedly "persistent" application log is a 128 KiB PSRAM ring, explicitly
lost on reboot (`main/main.c:488-495`) and freshly allocated in `app_main`.
No historical boot log survived this touch wake. A panic coredump partition
is configured, but there is no authenticated HTTP reader in this image; it
was not read or erased, and USB was not used. RTC activity receipts contain
owner/tool events, with no sleep/wake producer; they are not a boot journal.

RTC sleep diagnostics report **3 sleep entries**, lift/touch armed, no lift
arming failure, and the automatic DREAM sleep delay of 600,000 ms. The counter
increments **before** wake-source arming and `esp_deep_sleep_start`
(`main/power.c:116-169`), so it counts entry attempts, not successful timer
wakes. The harness records two forced sleeps: its initial 10-second boot and
the 1800-second measurement. Count 3 is consistent with an intervening wake
and another automatic sleep, including a timer wake followed by ten minutes
of DREAM. That could also fit ping replies reported at 00:09Z and API timeouts
around 00:10Z. This is an **inference, not proof**: no counter snapshot at the
1800-second boundary, timestamped wake history, or previous boot's network
log was retained. The counter alone cannot date or identify the extra entry.

The nominal timer deadline was **00:00:28Z**, excluding shutdown/boot overhead.
The available evidence cannot distinguish a late timer, an on-time timer with
late Wi-Fi/API, or another intervening wake. The observed acceptance regression
against the successful baseline remains; a firmware-caused timer/Wi-Fi
regression is possible but unproved. The current manual recovery is neither
timer acceptance nor a replacement sleep endpoint/DREAM sample. The bounded
source audit above found no direct Addendum wake/Wi-Fi defect. The worse awake
drain remains a possible battery regression, with no proven cell-state cause.
Original measurements, failed-gate receipts and the 91-artifact manifest are
preserved. No unmute, USB flash, OTA, reboot, or repeated power window was run.

## Waves

| Wave | Executors | Outcome |
|---|---|---|
| 1 — read | `a1_ux`, `a1_code`, `w2_power` | Live/test navigation map, bounded code audit, PMIC/method and measured remaining-cost audit. |
| 2 — build | `a1_ux`, `a1_code`, `w2_power` | Separate main routing, HUD subtraction, display fade/cache/retry and measured hot-path work. HUD ownership transfers only after subtraction is frozen. |
| 3 — prove | Parent plus reused `a1_code` break-it executor | Host/build/mutations and independent immutable-patch review pass; parent OTA valid and captures pass; after timer-wake power gate fails. |

Three waves use three unique executors. The orchestrator owns the Jarvis board;
the brief prohibits Jarvis/MCP calls, so this run uses a parent-owned local
task register without claiming a shared board.

Frame mirrors prove composed pixels, not physical panel illumination, finger
recognition, audible speech or listening latency. Voice acceptance is the
owner observation above; listening-state power remains excluded.

The orchestrator subsequently ruled that no saved log exists for that window;
historical log searching stopped. The requested new reproduction is recorded in
[t107-reproduction-300.md](t107-reproduction-300.md): candidate `7578238` passed
all three muted 300-second timer gates (API back in 303.422, 303.718 and
303.407 seconds). This does not resolve the original 1800-second failure.
The new idle A/B is incomplete: at 19% starting SoC, battery saver changed
HOME from WHISPER/14 brightness to DREAM/5 after 105 seconds; the device then
had 18% charge, below the 30% battery-only OTA preflight threshold. No OTA was
attempted. The previous worse drain remains a possible regression.

Report: **BLOCKED: Can the owner charge the Nano to at least 50%, unplug USB,
and leave privacy muted for the idle A/B?** Device: OTA `7578238`, authenticated
readback `ota_0 state=valid`, privacy muted; tests: host 11451/2200/109/166/81/38,
build 55.769% free; waves: 3 waves, 3 subagents; ux: detail navigation/captions,
finite fades and dial-cache headroom; code: 133 production lines removed,
160 added. This follow-up changes evidence and documentation only.
