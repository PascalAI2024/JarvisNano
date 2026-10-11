# t107 Addendum A — muted UX and render-path work

Date: 2026-10-11. Branch: `t107/codex`, continued from `6f97afb`.
The owner will perform the listening/voice check by hand later. Privacy stays
muted throughout this run. The parent alone operates the device and commits.

## Evidence and selected work

The live sweep covered JARVIS, WATCH, WEATHER, STATUS, ACTIVITY, conditional
DESK, their production-reachable detail sheets, controls, all six watch styles,
three choice layouts, five transient card kinds and a remote canvas fixture.
Screenshots were opened as contact sheets. Failure/empty states are exercised
by shell fixtures rather than deliberately disrupting live services. JARVIS's
SESSION sheet has no production hit target; physical pairing, hardware test,
listening and speech remain owner checks. No typography defect justified a
new layout change.

Ranked by daily exposure:

| Rank | Defect and change | Before/after proof |
|---|---|---|
| 1 | DOWN from DETAIL closed the sheet and advanced to another panel. It now closes on the same panel; the following DOWN resumes navigation. | Live WATCH detail/DOWN captures; production-routing regression and restored-defect mutation. |
| 2 | Outside-sheet dismissal printed `CONTROLS - UP TO CLOSE` on an ordinary panel. It now restores that panel's caption, with WATCH's caption cleared. | Live WEATHER dismissal captures; all five reachable detail panels in production-routing tests. |
| 3 | Finite fades consumed at most 100 ms per frame, stretching transitions at rest cadence. They now consume elapsed time, bounded by the longest fade; brightness keeps its existing slew cap. | Fake-clock production fade tests at 50/200/500 ms, endpoint captures. CLI latency prevents a subframe live timing claim. |
| 4 | Cycling baked dials retained compressed sources and exhausted snapshot headroom. Successfully decoded, unbound dial sources can be released; the stopped vendor parser's source remains alive until successful rebinding. | Before sweep snapshot returned HTTP 503; tracked allocation, decode/rebind fault tests and post-change style sweep. |

Code subtraction removes preprocessor-excluded tilt arithmetic and a duplicate
RGB565 byte-order helper. Failed presenter locks now preserve DREAM keys,
snapshot requests and pattern acknowledgement for retry. No new task, mutex,
cross-main header, privacy path or cadence policy is introduced.

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
USB, charging, screenshots or extra operator requests occur inside retained
windows. Brightness cap/volume remain 65/95 and the cell must remain above 20%.

Awake fits use six two-minute block means (`t(4)=2.776`); DREAM uses seven
one-minute means (`t(5)=2.571`). Conditional scatter intervals assume independent
blocks. A conservative 1 mV bound per block mean is propagated through the
actual OLS weights; two-endpoint slopes use a 2 mV differential bound.
ADC calibration, correlated cell relaxation, temperature, SoC and chronological
image order remain unbounded. Sleep's two endpoints include shutdown, boot
and reconnect. A lower CPU count must not be converted into an invented
battery-life percentage.

## Acceptance record

The combined host gate passes all six suites: HUD 11,451; shell 2,200;
production main routing 109; core 166; templates 81; desk 38. No prior check
count dropped. Ten display correctness mutants, two routing mutants and the
disabled-tilt mutant fail their assertions; rim work and strip-edge mutations
also fail, with the duplicate endpoint mutant identified as equivalent.
The pinned ESP-IDF 5.5.4 firmware build passes. Its 1,855,168-byte image
leaves 2,339,136 bytes (55.769%) free in each 4 MiB app partition.
Source commit, OTA probation/readback, matched after windows and final captures
remain pending at this checkpoint. The prior t107
measurements remain in [their separate report](t107-power-baseline.md).

## Waves

| Wave | Executors | Outcome |
|---|---|---|
| 1 — read | `a1_ux`, `a1_code`, `w2_power` | Live/test navigation map, bounded code audit, PMIC/method and measured remaining-cost audit. |
| 2 — build | `a1_ux`, `a1_code`, `w2_power` | Separate main routing, HUD subtraction, display fade/cache/retry and measured hot-path work. HUD ownership transfers only after subtraction is frozen. |
| 3 — prove | Parent plus reused `a1_code` break-it executor | Combined gates and independent immutable-patch review; parent alone owns OTA and live measurement. |

Three waves use three unique executors. The orchestrator owns the Jarvis board;
the brief prohibits Jarvis/MCP calls, so this run uses a parent-owned local
task register without claiming a shared board.

Frame mirrors prove composed pixels, not physical panel illumination, finger
recognition, audible speech or listening latency. Listening and voice remain
owner check pending.
