# Muted static HOME follow-up protocol

The owner closed CHARGE at 200mA and prohibited PMIC register writes. Physical
checks wait for `owner ready`; the coordinator never clears privacy remotely.
The owner checklist has seven numbered steps and targets 3 min 30 s.

## Maintained HOME comparison

Use baseline B `03864cc`, then repaired C1 `ff18294`. Verify the frozen image
digest before each OTA and authenticated valid/muted probation afterward.
The completed first B window exhausted OTA headroom. After recharge, use a
fresh back-to-back pair; do not match that old low-SoC window to a newly
charged after window. Keep the first control and its raw readings.
No USB/charging, operator lease, capture or powered audio rails. OTA requires
battery at least 30%; the measurement requires fresh battery readings above
20%, so the saver policy cannot change during the window.

Set brightness cap 65 and volume 95. Close the shade and send the existing
privacy-safe synthetic double HOME, then capture the software panel mirror.
Settle for 300 s. Measure 91 samples at 30 s spacing over 2700 s. Each sample
reads battery, cockpit and display. Require WHISPER, actual brightness 14,
privacy held, no capture, fresh increasing battery sequence, continuous
uptime and no flush errors.

An untouched HOME would enter DREAM at 15 min and sleep at 25 min. Therefore
both measurements use the same four brief HOME maintenance events at measured
540, 1140, 1740 and 2340 s. Save each event's before/after state and command
duration. Full-window voltage, gauge, frame and rendering totals include all
AWAKE/brightness excursions. This is **maintained HOME**, not continuously
untouched WHISPER-14. Only the separately labelled settled rendering rate
excludes the adjacent 30 s interval after each maintenance event.

Use nine 300 s block means for voltage OLS. Report a conditional 95% Student-t
scatter bound (7 degrees of freedom), plus the conservative influence of
1mV rounding. Endpoint voltage bounds use 2mV per window; endpoint gauge
bounds use 2 percentage points per window. These do not bound temperature,
relaxation, cell calibration, SoC/order effects or residual autocorrelation.
They establish neither measured current nor cell capacity. Do not project
battery life from these voltage fits or a configured charge-current limit.

Use `render_frames` and successful flush deltas divided by elapsed time for
actual redraw rate. Vendor `actual_fps` measures render-handler scheduling
before its no-dirty early return; it can remain near 6 on a static face.

## Separate untouched natural gate

On the repaired valid/muted image, fresh battery above 20%, unplugged and
still face-up, perform one HOME setup. That is the final control action.
Subsequent probes are GET only. No forced mood/sleep/timer, operator lease,
HOME maintenance, privacy change or owner contact during the run.

Observe the quiet fast path AWAKE -> WHISPER -> DREAM, then API disappearance
around the unchanged 25 min threshold. The muted path skips AMBIENT; do not
claim that stage was observed. The stage and disappearance brackets must
intersect their source thresholds and be at most 45 s wide.

The unmodified default timer is 14400 s. Poll every 300 s while offline,
then every 5 s near the due time. Require timer wake, exactly one RTC sleep
increment, low new uptime, valid OTA, live voice task and privacy held. API
loss is not an electrical sleep edge. Cause, counter and host timing together
support the sleep inference. Reset reason is not exposed by the current API.
Preserve failures without manual wake, USB flash or substituted short timer.

## Acceptance limits

Host and build proof does not establish live redraw reduction, battery saving,
optical wake-to-first-pixel, physical Nightstand/Glance or the new-image voice
turn. C2 offline sources are committed at `c4681ad` but excluded from the
target image and remain behind C1 acceptance. A real three-worker Deck face
and owner-attention pulse remain device acceptance checks.

Public text copies normalize line endings to LF; command logs trim trailing
horizontal whitespace and redact private checkout paths. Original captures
remain in private evidence. Export formatting does not change readings or
frozen production-source hashes. The public manifest hashes exported bytes.
