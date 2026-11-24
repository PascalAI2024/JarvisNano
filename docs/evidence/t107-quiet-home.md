# t107 muted HOME follow-up — 2026-10-12

**BLOCKED on recharge.** The full 45-minute B control completed, but its
remaining 21% charge is below the firmware's unplugged OTA minimum of 30%.
The quiet repair is committed and built; it has not been uploaded. Live
redraw reduction, the repaired 45-minute drain window, and the untouched
natural ladder/default four-hour wake remain unproved.

CHARGE is closed: retain 200mA. No diagnostic or charge-setting PMIC write
was issued. The configured limit is not measured current or cell capacity.
The [owner checklist](../../.evidence/t107/OWNER-CHECKLIST.md) has seven steps,
targets 3 min 30 s, specifies cable/lift/touch/voice actions and the API
captures for each, and waits for `owner ready`. Privacy stays muted during
the autonomous work.

## Source and proof completed

C2's five source/test paths were committed explicitly at `c4681ad`. The
offline projection has 17 tests / 34,372 parser calls; the host bridge has
51 tests. C2 remains excluded from the target SRCS and ELF. Its live face,
three actual workers and needs-owner pulse remain behind C1 acceptance.

Quiet HOME repair is committed at `ff18294`. The repeating 8fps MUTED
asset dirtied each decoded frame; the WHISPER ceiling yielded approximately
5.9 actual redraws/s. B also has that cost, so this does not establish its
causal role in the earlier new-image drain increase.

A settled, muted HOME now retains a visible nonrepeating frame. Changed
clock, battery/privacy/environment, caption and brightness facts refresh it.
Foreground, WATCH, USB/Nightstand, speech, fades, overlays, OTA and snapshots
retain their active paths. The retained frame uses vendor segment [0,1];
[0,0] would stop before decoding. First-frame preparation failures now retry
before advancing, and failed DMA submissions request another sparse refresh.
The scheduler still wakes: this repair removes repeated decode/render/flush
work, not every task or timer wakeup.

All 15 host suites pass with positive counts; shell coverage increased from
2708 to 3563 and the vendor suite was added. No existing count fell.

| Suite | Positive count |
|---|---:|
| HUD | 11451 |
| Actual display shell | 3563 |
| Companion | 76940 |
| Production board transport | 72 |
| Pinned vendor playback / quiet entry | 5778 |
| Main input routing | 109 |
| Charge HTTP | 5120 |
| Display HTTP | 168 |
| Companion orchestration | 2896 |
| Companion integration | 1920 |
| Sleep coordinator | 16938 |
| OTA/sleep fence | 3278 |
| Core | 180 |
| Tools | 81 |
| Desk Python | 38 |

Scratch mutations: positive compiles/passes; **53/53 faults compile and fail
at runtime**. The first run had a masked caption-guard fixture (52/53); its
settled-caption correction passed the final gate and mutation run. Both
results are retained. Independent review adds actual vendor allocation-fault
recovery (3613 checks), actual panel-flush retry (10 checks), and two compiled
runtime faults; those are independent repetitions, not 55 unique mutations.
All 16 current EAF assets / 195 table entries satisfy the terminal-sentinel
contract. Permanent frame corruption is not covered by transient recovery.

Pinned ESP-IDF 5.5.4 build passed. App size **1,868,096 bytes** in a 4,194,304-byte
slot; **2,326,208 bytes / 55.461% free**. Frozen image SHA-256:
`60d51bdf6227fa52fc9388f16f3f86ac66915b1b1a3265f626f2c4933eb41682`.
All 41 recorded source hashes match the explicit source commit after LF
normalization. The container lacks the worktree Git metadata, so embedded
project version `1` is not a source identity; the frozen manifest binds the
image to source. C2 symbol exclusion was checked against the actual ELF.

Quiet production/managed-patch changes add 145 lines and remove 3. The repair
commit's 13 paths add 935 and remove 8 including tests, license and docs.
C2 separately adds 1474 lines across its five committed paths.

## Live result

The paired upload of frozen B `03864cc` passed probation to `ota_1 state=valid`
at 05:42:40Z. Its image SHA-256 is
`ac73185ce279c02e7d58e27e790463015eada3e6cd18951f4a3bd39e301a3e9b`.
The first settling attempt was abandoned before any measured sample to avoid
a HOME-maintenance/DREAM boundary race. The corrected run preserved every
sample and all four maintenance receipts.

**B control passed:** 91 samples over 2700.094 s, 05:49:04Z–06:34:04Z.
Brightness cap 65 / volume 95; settled WHISPER brightness 14, with identical
scheduled HOME maintenance as specified in the [protocol](t107-quiet-home/protocol.md).
Privacy held, capture off, USB/charging absent, fresh increasing battery
sequences, continuous uptime and zero flush errors throughout. SoC stayed
above the 20% saver boundary, falling **45% -> 21%**; voltage fell
**3696 -> 3624mV**.

| Maintained HOME measure | B control | Quiet repair |
|---|---:|---|
| Voltage drain, nine 300s block means | 97.22 ± 8.17mV/h | Pending |
| Endpoint gauge decline | 32.00 ± 2.67 percentage points/h | Pending |
| Full-window redraw rate | 5.898 frames/s | Pending |
| Settled-interval redraw rate | 5.882 frames/s | Pending |
| Render work / wall time | 3.623% | Pending |

The voltage half-width is conditional 95% block scatter plus worst-case
1mV rounding; the gauge bound is endpoint rounding only. Temperature,
relaxation, gauge calibration, SoC/order effects and autocorrelation are not
bounded. Neither number establishes current, cell capacity or battery life.
All drain samples include the maintenance bursts; only the separately labelled
settled rendering rate excludes the four adjacent intervals. The firmware
`actual_fps` field read 5 in 87 samples and 6 in four: it measures scheduling,
not just completed redraws. The repeated muted clip is a shared B/C1 cost;
its removal has not yet established the cause of the earlier larger drain fits.

At **06:34:40Z**, final authenticated GETs confirmed B still running/booting
`ota_1`, `state=valid`, OTA inactive, last error `ESP_OK`, voice task alive,
privacy muted, capture false, mic/ADC/DAC/amp off and no operator lease.
Uptime was **3,178,096ms**; wake cause `power`, RTC sleep counter **0**, no
forced sleep. Battery was **21%, 3617mV**, USB/charging false, sequence 3159,
age 84ms. These are the final readback facts, not a natural-wake result.

Production preflight at `main/main.c:1792` refuses an unplugged OTA below
30%. No quiet-image upload was attempted and the guard was not bypassed.
**Device image at the end: B `03864cc`, last authenticated valid/muted.**
The frozen quiet image remains ready for the resumed comparison; no repaired
probation, after capture or live near-zero-redraw claim is made.
After recharge, run a fresh back-to-back B/repair pair with both windows at
least 45 minutes. Retain this completed B control as a standalone result;
pairing it with a newly charged repair window would introduce another large
SoC/charge-history confound.

[B raw samples](t107-quiet-home/B-idle.jsonl),
[B result and uncertainty](t107-quiet-home/B-result.json),
[B trace](t107-quiet-home/B-trace.png),
[B probation](t107-quiet-home/B-ota-valid.json),
[final readback](t107-quiet-home/final-readback.json).

## Natural gate and remaining acceptance

The source/read-route audit found no inactivity reset from the selected
authenticated GETs. The quiet path is AWAKE -> WHISPER at 5 s -> DREAM at
15 min -> deep-sleep eligibility at 25 min after the last activity. It skips
AMBIENT while muted. The default timer remains 14400 s.

The separate natural harness performs one HOME setup, then GETs only. It
requires ordered stage/timing brackets, fresh face-up/still IMU, no USB or
lease, battery above 20%, valid OTA, timer cause, exactly one RTC increment
and fresh low uptime with the voice task alive. Independent offline audit
accepted two positive fixtures and rejected 13 adverse fixtures. This is
not a device result. The current API does not expose reset reason.

The untouched natural run was **not started**: the repaired image could not
be installed at the remaining charge. Testing B instead, accepting saver
timings at or below 20%, or replacing the default timer with a short forced
cycle would not satisfy this gate. No natural sleep edge or four-hour timer
return is claimed. Resume with sufficient unplugged charge for the fresh pair
and the complete unattended run. The new after window must be at least
45 minutes; no short-window substitution is accepted.

Physical Nightstand, lift/touch Glance, optical wake-to-first-pixel and the
new-image spoken turn remain owner checks. Historical voice acceptance on
2026-10-11 is owner-observed, with the tested image unspecified; it does not
accept this new image. Software panel mirrors do not establish optical
appearance or wake latency. Voltage fits do not support a battery-life projection.

## Waves and artifacts

Three follow-up waves, three distinct executors: read/audit by `w2_power`
(display cost), `a1_code` (ladder/read routes), `a1_ux` (C2 review); build by
`w2_power` on the sole required display/managed-patch part; proof by `a1_ux`
(independent vendor/flush review), `a1_code` (natural harness), and parent
(full gates, commits, OTA and measurements). Parent accepted the frozen
returns before closing their tasks. The brief's board override was followed:
no Jarvis/MCP calls; the private coordinator register remains the task record.

[Protocol](t107-quiet-home/protocol.md), [build/source binding](t107-quiet-home/build.json),
[host gate](t107-quiet-home/host-tests.log), [build tail](t107-quiet-home/build-tail.log),
[mutations](t107-quiet-home/mutations.json), [independent review](t107-quiet-home/display-review.md),
[natural code audit](t107-quiet-home/natural-code-read.md),
[natural harness audit](t107-quiet-home/natural-harness-review.md),
[baseline panel mirror](t107-quiet-home/before-home.png),
[ELF exclusion receipt](t107-quiet-home/elf-exclusion.json),
[artifact hashes](t107-quiet-home/manifest.json).

**BLOCKED: Can the owner fully charge the Nano, unplug it, and send
`owner ready`?**

This is the one required owner action. It resumes the battery-gated OTA and
autonomous measurements; the seven-step physical session remains pending
that signal. C1 is not accepted and C2 has not been deployed.
