# t107 C1 companion — acceptance checkpoint

Companion comes before Deck. Main was merged at `3b5dbd7`; main has not moved.
The charged frozen-image A/B and B: CHARGE are complete. New source is built
around the existing Rest compositor, battery rim and authoritative privacy.
Privacy remains muted. Physical and energy acceptance are pending below.

## Implemented behavior

Quiet USB idle earns a sparse Nightstand clock with truthful battery/charge.
Brightness is 22% of the owner's cap by day, 8% from 22:00–07:00 only with
valid local time. Unknown time displays `--:--`. The USB presence cache must
have a sequence and age ≤2s. Foreground, speech, live/pinned caption, canvas,
choices, operator lease or OTA/probation restores normal content. The renderer
independently checks eligibility, including actual speech-busy state.

A physical deep-sleep lift/touch wake seeds a local eight-second Glance
before Wi-Fi/audio. Time and battery use retained local facts; agenda is
`NEXT UNAVAILABLE` because no agenda feed exists. Attention exits Glance
through the existing input path. Wake-word behavior retains the privacy gate.
No face clears privacy. Full interactive/network readiness is separate from
the early display seed.

Muted stillness still moves to WHISPER after five seconds; the unmuted ladder
retains 20s AMBIENT, 5min WHISPER, 15min DREAM and 10min-in-DREAM sleep.
No new timer/DFS/light-sleep policy is justified by the available energy
measurements. Measured battery life remains unavailable. No PMIC charge or
rail setting changed; [B retains the 200mA limit](t107-charge-diagnostic.md).

The panel owner now acknowledges off only after brightness zero, DISPOFF,
QSPI SLPIN and 120ms settling succeed. Cancellation, command failure and
failed transfer cannot acknowledge false success. Recovery requires matching
SLPOUT/120ms/DISPON, effective brightness and a completed transfer. Boot starts
brightness zero and applies the owner's cap before content DMA. RASET's
600ms wait was removed and SLPOUT's wait changed from 600 to 120ms: **1,080ms
removed from the source delay budget**, pending module/optical validation.

Sleep entry now checks fresh USB absence and owns a final Brain reservation
through audio/IMU/RTC/deep-sleep commitment. App and art OTA admission use
the same mutex before any flash mutation. This fixes an actual reproduced
late OTA/lease claim during the caption/panel wait that formerly slept after
accepting the claim. Refusals cancel the timer, recover the panel and preserve
RTC/privacy. Peripheral fault waits may hold the reservation for seconds;
new OTA/lease admission then refuses rather than being accepted before sleep.
A physical USB plug after the last cache observation remains a race.

## Hardware inventory and evidence boundary

| Hardware capability | Current use / proof |
|---|---|
| S3 active, modem sleep | 240MHz live/160MHz rest; MIN_MODEM at rest. Runtime policy/readback, no isolated electrical saving |
| S3 light sleep / DFS | Disabled; no measured saving/codec readiness proof |
| S3 deep sleep | GPIO/timer sources; 757 passed 3/3 forced 300s API wakes at 303.422/303.718/303.407s. B passed at 305.937s |
| S3 hibernation | No separate named S3 mode; RTC-domain deep-sleep choices unused |
| AXP2101 rail/sleep/off | Existing factory/off behavior; PMIC sleep unused, rail/current writes excluded |
| CO5300 normal / DISPOFF / SLPIN | Normal color retained. C implements error-checked QSPI sleep/recovery; transport success is not optical state |
| CO5300 IDMON / partial / deep standby | Unused: eight-MSB-color idle erases dim ink; partial must retain rims; standby needs reset. No current proof |
| QMI8658 normal / WoM | Awake 125Hz accel, gyro off; WoM 21Hz/100mg on GPIO21. Controlled lift pending; 32 blank samples imply ~1.52s arming exclusion |

Primary references: [Waveshare 1.75C](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C),
[IDF 5.5.4 sleep modes](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/sleep_modes.html),
[AXP2101](https://files.waveshare.com/wiki/common/X-power-AXP2101_SWcharge_V1.0.pdf),
[CO5300](https://dl.espressif.com/AE/esp-iot-solution/CO5300_Datasheet_V0.00.pdf),
[QMI8658C](https://files.waveshare.com/wiki/common/QMI8658C.pdf).
Detailed source/page inventory is retained privately in `C-AUDIT.md`.

The 1,800-second after-image API wake failed; subsequent owner recovery was
manual. No saved log exists. Short timer successes do not close that incident.
Natural DREAM-to-sleep, controlled lift/touch and four-hour wake remain
unproved. Hardware documentation now labels those as policy/capability.

## Offline proof

All **14 host suites pass**, with positive counts and no unexplained drop:

| Suite | Count |
|---|---:|
| HUD | 11,451 |
| Shell | 2,708 |
| Portable companion | 76,940 |
| Actual board seams | 72 |
| Main input | 109 |
| Charge HTTP | 5,120 |
| Display HTTP | 168 |
| Actual main orchestration | 2,896 |
| Main → actual renderer | 1,920 |
| Actual sleep coordinator | 16,938 / 144 scenarios |
| Actual OTA admission/restoration | 3,278 / 132 scenarios; 16 separate route checks |
| Core | 180 |
| Tools | 81 |
| Desk CLI | 38 |

Compiled runtime faults rejected: display/board/helper 62/11/47 (120), main
13, main→renderer 12, sleep 17, OTA fence 13, display HTTP 4 (179 total).
Compile failures, crashes and two observationally equivalent display faults
are excluded. The final integration mutation proof was rerun against the
combined renderer. Output-only changes made companion/integration summaries
compatible with the positive-count parser. An old source-wiring assertion
was updated to check the new Rest selector and DREAM predicate; the original
180 core tests remain. The initial combined failures are retained privately.

The public candidate scan passed for all 39 staged paths; shell parsing and
the staged diff check passed. The whole-worktree identifier scan finds only
the uncommitted brief and private device receipts under `.evidence/`; those
are excluded from publication. No scan finding is waived for a committed file.

ESP-IDF 5.5.4 target build passes: 1,867,168 bytes in a 4,194,304-byte app
partition, **55.483% free**. Frozen image SHA-256:
`c49898975b55b6feabf2778639bd604d9e7f40b41bee46eaf76f6adc4a5a41fa`.
Combined source/test hashes and compact proof are in
[the proof receipt](t107-companion-proof.json). OTA remains pending the
completed matched baseline at this checkpoint. Boot-relative
first DMA and valid-clock Glance-strip completion are explicitly `optical=false`
and exclude ROM. They cannot establish wake-to-first-pixel or a complete face.

## Device measurements and remaining acceptance

The matched baseline is B `03864cc`, derived from the measured 757 source
with read-only charge/timing diagnostics. Stage windows use a fresh 300s
timer boot, 120s settling, 420s DREAM, HOME/120s settling, then 720s WHISPER,
15s samples and the same cap65/actual5 or14, mute and unplugged state.
The first baseline capture returned 503 before any window sample; that
attempt is retained. A separate capture retry succeeded; the complete rerun
uses a new timer cycle and up to three logged capture attempts. No window
or failed attempt is silently discarded.

SoC endpoint quantization bounds are ±2pp/window (±17.14pp/h over420s,
±10pp/h over720s). Voltage fits use block regressions plus 1mV quantization;
their uncertainty is conditional, with relaxation/temperature/order unbounded.
AWAKE/AMBIENT steady idle is unavailable in this muted quiet path. No voltage
fit is converted into current or projected life. The preceding charged A/B
favored old voltage drain but showed unplugged gauge rebound on new; a
possible battery regression remains unresolved, not explained away by SoC.

Nightstand/Glance physical USB/lift captures, optical wake timing and the new
image's owner voice turn remain pending. The earlier owner `yes` on
2026-10-11 records audible voice acceptance for an unspecified earlier image.
It is not acceptance of this C image. Privacy stays muted.

C2 offline bridge/projection exists but is excluded from C1 deployment. Live
preview had zero actual workers; three live workers cannot be invented. C2's
server-side publish fence and device capture wait for C1 acceptance.
