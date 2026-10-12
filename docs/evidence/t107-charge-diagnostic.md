# B: CHARGE — read-only diagnosis

The fixed AXP2101 charge-register cache and paired `GET /api/power/charge`
are implemented. Frozen image `03864cc` reached authenticated OTA valid after
the charged A/B. Three readbacks at 04:16:35–04:16:46Z advanced the diagnostic
sequence (11–13), completed without register errors and agreed on the limits:

| Register setting | Readback |
|---|---:|
| Constant-charge current | 200mA |
| Constant voltage | 4200mV |
| Input current limit | 1500mA |
| Input voltage regulation | 4360mV |
| Precharge current | 125mA |
| Termination current | 125mA, enabled |
| Charger | Enabled |
| Precharge / charge timeout | 60min / 12h, enabled |

The device was unplugged during these reads (63% SoC); these are retained
configuration limits, not current flowing into the cell. The fixed register
scan spans about 3.06 seconds and is explicitly non-atomic. Completion ages
were 3.805–4.438 seconds; individual successful-register ages reached roughly
7.5 seconds. Stable configuration across three scans is evidence for these
settings, not a simultaneous electrical snapshot.
No constant-charge, rail or current setting was changed. The sampler reads
only the documented fixed register set; HTTP copies the cache and never
accesses the bus. Failed, stale and reserved values decode to null, with
per-register errors and attempt/last-success ages retained.

The [preceding charging wait](t107-idle-ab-charged.md) measured a net gauge
rise of **39.27±2.18 percentage points/hour**: 16%→52% over 55 minutes with
USB/charging true. The bound permits ±2 percentage points of endpoint
quantization; gauge accuracy, relaxation, temperature and load remain
unbounded. Device consumption is already included in this net gauge result.
Cell capacity and charging rating are unverified, so milliamps cannot be
inferred. **Safe proposal: retain the existing 200mA constant-charge setting.**
A higher setting requires the actual cell/supply limits and the orchestrator's
ruling. The unusually large precharge/termination limits are disclosed above;
this diagnostic run provides no authority to alter them either.

Offline validation: 14 portable decoder tests, 186 actual backend checks,
5,120 actual-handler HTTP checks, five diagnostic and three HTTP scratch
mutants rejected. Display instrumentation adds 42 shell checks and rejects
12 scratch mutants. The combined seven suites pass: HUD 11,451; shell 2,242;
input 109; charge HTTP 5,120; core 180; tools 81; desk CLI 38. No count dropped.
The first gate failed because the new suite's output did not match the
positive-count parser; its summary format was corrected and the full gate
passed.

ESP-IDF 5.5.4 target build passes: 1,860,080 bytes in a 4,194,304-byte app
partition, 55.652% free. Two static 680-byte diagnostic caches consume
1,360 bytes of BSS; no new task or allocation is introduced. The sampler's
20 ms I2C API timeout is not a wall-clock bound: command-lock, event and bus
recovery waits can add a conservative approximately 310 ms fault wait budget
plus scheduling/logging overhead.

`/api/display` records initialization start/end and the first completed color
DMA. These are software transfer proxies: they exclude ROM time and do not
measure optical first-pixel latency or completion of a useful Glance face.

The source build was frozen before the idle A/B. B OTA `03864cc` followed
the completed pair and reached `ota_1 state=valid`; paired readback at
04:16:35Z confirmed privacy muted, mic/ADC/DAC/PA off and no flush errors.
The subsequent 300-second forced timer cycle passed: API readback at
04:21:55Z, 305.937 seconds after the command, wake reason `timer`, sleep
counter incremented, uptime 6,529ms. This upper bound includes the timer,
boot and API availability; it does not measure the electrical wake edge.
Software display markers were init start 230ms, end 1,853ms and first
completed transfer 1,899ms since boot, excluding ROM and optical latency.
Final authenticated health remained valid and muted. Address-redacted
[receipts, capture and harness](t107-charge/manifest.json) have verified
SHA-256 hashes, with private-original hashes retained in the manifest.
Image SHA-256:
`ac73185ce279c02e7d58e27e790463015eada3e6cd18951f4a3bd39e301a3e9b`.
The existing failed 1,800-second wake and possible awake-drain regression
remain unresolved by these offline checks.
