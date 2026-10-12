# Charged idle A/B — 2026-10-12

The requested pair completed. The old image has a lower voltage-drain fit;
causal battery consumption remains unresolved. This is a possible regression
in `7578238`, not a demonstrated saving or a proven cell-state explanation.

| Source image | HOME interval (UTC) | Start/end SoC | Voltage fit, mV/h | Conditional interval, mV/h |
|---|---|---|---:|---:|
| `7578238` | 03:44:23–03:56:24 | 59% → 62% | 207.44 | 158.91–255.97 |
| rebuilt `0b4108f` | 04:01:03–04:13:03 | 63% → 63% | 87.67 | 27.79–147.55 |

Both windows used 120 seconds of settling, 720 seconds of measurement,
49 samples at 15-second cadence, HOME, WHISPER, brightness cap65/actual14,
volume95, privacy muted, USB disconnected and charging false. Battery samples
advanced and remained fresh, uptime advanced, and display flush errors stayed
zero. Gold privacy rings and matching brightness were visually checked in
the [new](t107-idle-ab-charged/7578238-idle.png) and
[old](t107-idle-ab-charged/0b4108f-idle.png) captures. The new image retains a
JARVIS caption absent from the old capture; the rendered screens are not
pixel-identical.

The fit uses six two-minute block means and OLS. Its interval adds t(4)=2.776
residual scatter and a conservative 1mV-per-block rounding bound. These
intervals are conditional on independent residuals. Relaxation, charge history,
temperature, correlated samples and fixed chronological order are unbounded.
The measured difference is +119.77mV/h (new minus old), conditional half-width
108.41mV/h. The two voltage intervals favor the old image.

SoC-derived drain is −14.98±9.99 percentage points/hour for the new image
and 0±9.99 for the old image, using conservative ±2-point endpoint differences.
The new gauge rose while unplugged. That directly demonstrates gauge settling
in this run; it does not establish why the older after-image fits were higher.
SoC is not a current meter. Neither result supports a projected battery life.
Rendering consumed 3.872% versus 4.025% of elapsed wall time; that is CPU
render timing, not electrical consumption.

The prespecified selection requires both lower conservative voltage intervals
and lower SoC loss beyond endpoint rounding to retain the old image. Only
voltage satisfies it, so the harness selects `7578238` for restoration.
The [analysis and final receipts](t107-idle-ab-charged/summary.json) record that
rule and its limitations. Later B/C OTA acceptance is a separate image stage.
The final authenticated readback at 04:14:30Z confirmed `ota_0 state=valid`,
uptime 58,019ms, privacy muted, capture false and mic/ADC/DAC/PA off.
Battery was 63%, USB/charging false. See the
[final receipt](t107-idle-ab-charged/7578238-final.json).

Charging wait: twelve read-only five-minute polls ran from 02:41:42 to
03:36:42, with USB/charging true and SoC16%→52%. At 03:41:42, SoC58%, fresh
telemetry, USB false and charging false satisfied the owner's release rule.
The connected interval gives a net gauge rise of 39.27±2.18 percentage
points/hour (±2 points over 55 minutes), including device consumption. Cell
capacity/rating is unknown; this cannot be converted to milliamps.

The original baseline binary was overwritten before this comparison. The old
image is an isolated rebuild of the exact `0b4108f` source: 134 tracked source
files verified and resolved sdkconfig matched to the candidate. Compile-date
metadata changes its hash. [Image receipts](t107-idle-ab-charged/images.json)
retain original and rebuilt hashes. This is source-equivalent, not a comparison
against the lost original bytes.

The first baseline OTA invocation was rejected locally because its timeout
argument exceeded the CLI limit; no upload began. Its attempted-start receipt
is retained. Timeout was corrected, the baseline reached valid, and the
candidate window was not repeated or discarded.

All samples, boundaries, captures and exact measurement/analysis harnesses are
retained with [SHA256 hashes](t107-idle-ab-charged/manifest.json). Credentials,
network addresses and device MACs are excluded. The prior three successful
300-second timer cycles do not explain the failed historical 1,800-second
window; no saved log for it exists.
