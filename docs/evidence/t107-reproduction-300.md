# T107: muted 300-second timer reproduction and idle A/B

**BLOCKED: Can the owner charge the Nano to at least 50%, unplug USB, and leave privacy muted for the idle A/B?**

The three requested timer cycles on `7578238` passed. The idle comparison is incomplete: the candidate entered battery-saver DREAM after 105 seconds of the retained window, and its 18% charge is below the firmware's 30% battery-only OTA threshold. No OTA was attempted. The device remains on `7578238`, `ota_0 state=valid`, privacy muted. No unmute or USB flash occurred.

## Timer reproduction

The orchestrator ruled that no saved boot/network log exists for the original 1800-second window. Historical log searching stopped. These are new observations, recorded on 2026-10-12 UTC, using the existing `device_lib.timer_sleep(300)` gate. Instrumentation records authenticated API timestamps without changing its polling cadence. Privacy remained muted, capture inactive, and mic/ADC/DAC/PA power off at every boundary.

| Run | API detected wake | Seconds to API back | Wake reason | Reset reason | Fresh uptime | Sleep-entry counter | SoC before/after |
|---|---|---:|---|---|---:|---|---|
| 1 | yes | 303.422 | timer | unavailable | 7,625 ms | 3 → 4 | 21% → 20% |
| 2 | yes | 303.718 | timer | unavailable | 7,911 ms | 4 → 5 | 20% → 19% |
| 3 | yes | 303.407 | timer | unavailable | 7,508 ms | 5 → 6 | 19% → 19% |

All first post-sleep authenticated polls succeeded. Each cycle has a new sleep-entry counter and a fresh uptime, so the wake reason is not merely stale telemetry from a previous boot. The image exposes sleep wake cause, but does not expose `esp_reset_reason`; reset reason is therefore unavailable, not inferred.

Seconds to API back are measured from dispatch of the sleep POST to the first successful authenticated GET. The unchanged harness waits 300 seconds after the POST response before polling. These times include shutdown, boot, Wi-Fi and polling delay; they are upper bounds on API availability, not measurements of timer accuracy. No owner/manual wake was used in these cycles. Because none failed, the conditional three-cycle baseline fallback was not triggered.

Three 300-second passes do **not** turn the original failed 1800-second acceptance window into a pass. The baseline completed that original gate; the candidate did not produce the required timely API evidence. The historical cause remains unknown. This reproduction found no repeatable failure at 300 seconds.

## Idle A/B: incomplete

The requested controls were HOME idle, 120-second settling, 720-second retained window, 15-second sampling, brightness cap 65 / actual 14, volume 95, no USB, no charging and no operator lease. The candidate window began at `2026-10-12T00:48:13.454300Z`, **19% SoC, 3591 mV**. Eight samples were retained through 105.391 seconds. At `00:49:58.852932Z`, the mood changed from WHISPER to DREAM, actual brightness fell from 14 to 5, and SoC was 18% (3623 mV). The harness rejected the changed state. No 12-minute voltage fit or baseline window exists.

I should have enforced the prior window's greater-than-20% charge precondition before starting. Low charge is the demonstrated cause of this aborted window; it is not evidence explaining away the earlier drain difference.

The source explains both blockers:

- `main/app.h:233` sets battery saver at 20%; `main/main.c:2301-2302` activates it on battery at or below that charge.
- `components/jr_core/include/jr_core/mood.h:44,52` and `components/jr_core/src/mood.c:19-24` reduce the 900-second DREAM threshold by four to 225 seconds. The 120-second settling period plus 105 seconds of sampling reached that threshold.
- `main/main.c:1675-1678` refuses battery-only OTA below 30%. `main/http_routes.c:3355-3371` applies this preflight before writing upload data to flash. The device was at 18%; an upload was not sent just to demonstrate rejection.

These saver and OTA checks are unchanged between baseline `0b4108f` and candidate `7578238`. Charging to 50% is an operating margin for the paired experiment, not the firmware threshold. USB must be disconnected for both retained battery windows, and charge must stay above 20% throughout.

The earlier awake voltage-drain fits remain **a possible regression**, not a proven cell-state effect: idle 29.42 ± 34.84 → 100.20 ± 25.77 mV/h; WATCH 65.83 ± 10.52 → 126.56 ± 14.52 mV/h. Those conditional fit intervals do not bound SoC/order/relaxation effects. This partial window cannot settle the comparison. Voltage slope is not a measured current or energy channel.

## Images, evidence and remaining work

Candidate source is `7578238fb766f4774f2fe772c6310e93512e5b41`; its frozen binary is 1,855,168 bytes, SHA-256 `7f2c42e3b9f3552912ce7188e859ddba2ff9d3725488adf36703ad746686e4b3`, with 55.769% app-partition headroom. No runtime source changed in this reproduction.

The pre-Addendum source is `0b4108fc9c25b7dc2ac87a06f853c3728108713f`. Its original measured binary was overwritten by the Addendum build. The isolated rebuild completed successfully with pinned ESP-IDF 5.5.4: 134 tracked source/build/config files match the committed archive, and all resolved SDK configuration values match the candidate. The rebuilt baseline is 1,854,464 bytes, 55.786% free, SHA-256 `1ae73d543d3c479dbb4ea5c80702670e2b4d80b8a957fa706dfb55869405da86`. Full identities are recorded in [images.json](t107-reproduction-300/images.json). This is not the original measured binary; the enabled compile date changes the hash. It has not been OTA-installed or live-tested in this reproduction.

The final authenticated receipt records `ota_0` running and boot slots, state `valid`, upload inactive, `ESP_OK`, privacy true, capture false, mic/ADC/DAC/PA off, timer wake and sleep-entry count 6. Battery is 18%, with no USB or charging. Source/binary binding follows the previously verified candidate deployment plus no intervening upload; the health route does not report a Git SHA.

Credential-free per-cycle receipts, partial idle samples, the HOME frame mirror, final authenticated readback, image identities, summary and exact harness/analysis copies are indexed by [manifest.json](t107-reproduction-300/manifest.json). The code copies are audit artifacts preserving the original private harness layout, not standalone launchers. The original Addendum report and its failed 1800-second evidence remain in [t107-addendum-a.md](t107-addendum-a.md).

Remaining authorized work after owner charging: restart the full candidate HOME window, OTA the validated baseline, run the matched baseline window, then select the final image under the orchestrator's rule and record authenticated valid/muted readback. Neither image has been demonstrated better by this incomplete A/B. Stop BLOCKED; do not claim Addendum power acceptance complete.
