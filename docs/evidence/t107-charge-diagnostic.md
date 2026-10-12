# B: CHARGE diagnostic preparation

The fixed AXP2101 charge-register cache and paired `GET /api/power/charge`
are implemented. This is diagnostic preparation, not device acceptance.
No constant-charge, rail or current setting was changed. The sampler reads
only the documented fixed register set; HTTP copies the cache and never
accesses the bus. Failed, stale and reserved values decode to null, with
per-register errors and attempt/last-success ages retained.

The configured charge current is a limit, not a measured net current.
Net charging will be reported from battery gauge samples with integer
endpoint uncertainty. Cell capacity and rating are not verified for this
board, so the safe proposal remains to retain the current setting until
those limits and the orchestrator's ruling are available.

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

Next acceptance: freeze the committed source build, finish the frozen-image
idle A/B, then OTA this diagnostic image, confirm probation/paired health,
read actual charge settings and report a net gauge rate. Privacy stays muted.
The existing failed 1,800-second wake and possible awake-drain regression
remain unresolved by these offline checks.
