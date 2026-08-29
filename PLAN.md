# JarvisNano product plan

Current plan, reconciled **2026-08-28**. This file contains only active work and
release gates. Completed interaction and reliability waves are preserved in
[`docs/ARCHIVE/PLAN-2026-08-27-waves-1-5.md`](docs/ARCHIVE/PLAN-2026-08-27-waves-1-5.md).

The live product is the 32 MB **Waveshare ESP32-S3-Touch-AMOLED-1.75C** running
plain ESP-IDF v5 from `main/` and `components/jr_*`.

## Shipped baseline

- Native 24 kHz full-duplex codec path with 16 kHz AEC-cleaned Gemini uplink.
- Direct Gemini Live transport, server VAD, reconnect/re-arm policy, and
  privacy-safe recovery.
- One overlay compositor with baked EAF faces, listening halo, Watch, Desk,
  Tools, Settings, controls, captions, and remote canvas.
- One physical grammar: global left volume, global right brightness, PWR
  listen/battery, BOOT controls, glass hold privacy.
- Operator lease, bounded diagnostics, incident log, Wi-Fi OTA probation, and
  deterministic host suites.
- Typed JarvisMCP server policy and catalog search. Device-side result projection
  is still constrained by a temporary three-result limit.

## Active wave — cleanup, reliability, refinement

Every behavioral row closes on physical 1.75C evidence, not compilation alone.

| # | Priority | Deliverable | Status | Acceptance gate |
|---|---|---|---|---|
| N6.1 | P0 | Ship one interaction grammar; delete circular-rotation experiment | LIVE; PWR/BOOT hand proof pending | No rotate symbols; PWR short listens without muting; BOOT short toggles controls; BOOT hold opens one 60-second pairing claim; edge levels work from every surface |
| N6.2 | P0 | Add playback-gap and ring-low-watermark telemetry | PENDING | Live diagnostics expose underruns, maximum empty gap, low-watermark, and DAC failures; 60 s reply has no unexplained gap >120 ms |
| N6.3 | P0 | Byte-budget JarvisMCP results with cursor projection | TEMPORARY 3-result cap | Every normalized result is valid JSON ≤3071 bytes with `has_more` and cursor; voice search plus one returned read-only tool execute without `bad_response` |
| N6.4 | P0 | Run uninterrupted powered conversation soak | PENDING N6.2/N6.3 | 30 min with TX drops/deaths/flush errors 0, playback gaps ≤120 ms, largest internal block ≥16 KB, and final state Listening |
| N6.5 | P1 | Split `main/main.c` at existing ownership seams | PENDING | Input/buttons, HTTP diagnostics, and voice/power policy become three modules; `main.c` <4,000 lines; all builds/suites pass |
| N6.6 | P1 | Make one canonical host-test command | PENDING | One command runs core, transport, display, tool-template, desk CLI, and shell suites; CI calls it |
| N6.7 | P1 | Remove repository/document truth drift | IN PROGRESS | Root plan is current-only; historical plans are archived/labeled; all relative links resolve; no obsolete interaction claims remain |
| N6.8 | P1 | Consolidate visual interaction specification | DOCS ALIGNED; final capture set pending | Vision, hardware, and protocol agree; Jarvis, Watch, controls, privacy, and side-space captures match |
| N6.9 | P1 | Raise settled controls cadence | PENDING | Full controls remain present at ≥14 FPS, zero flush errors, largest internal block ≥24 KB |
| N6.10 | P1 | Prewarm face assets without blocking short turns | PENDING | First Think and Speak apply in <150 ms; stale requested phases never flash late |
| N6.11 | P2 | Add rolling frame/network-burst diagnostics | PENDING | Health exposes 1 s frame-gap max/p95 and rate-limited backpressure bursts; healthy Listening p95 ≤75 ms |
| N6.12 | P2 | Move stillness detection to proven QMI8658 INT2 | PENDING HARDWARE PROBE | Scope-prove pin first; preserve flip/shake/lift; still task wakes <2/s |
| N6.13 | P0 release | Complete authenticated, signed OTA | BLOCKED ON RELEASE CREDENTIALS | Wrong-key/unsigned images fail before write; upload is authenticated; Secure Boot eFuse remains separately attended |
| N6.14 | P0 release | Protect Wi-Fi and cloud credentials at rest | BLOCKED ON ATTENDED KEY PROVISIONING | NVS/flash encryption is enabled through a separately attended procedure; a physical flash read cannot recover credentials; recovery is documented and tested |
| N6.15 | P0 release | Ship complete third-party notices and dependency inventory | PENDING | The exact resolved firmware dependencies, versions, licenses, and required notice texts are generated after build and attached to every binary release |

## Execution order

1. **Correctness:** N6.1–N6.4.
2. **Maintainability:** N6.5–N6.8.
3. **Performance/hardware:** N6.9–N6.12.
4. **Public release:** N6.13–N6.15 after credentials and attended device access exist.

## Release gates

- One clean 30-minute voice soak with the counters in N6.4.
- Physical PWR, BOOT, both edge controls, glass privacy, flip privacy, and ROM
  downloader recovery witnessed on the target board.
- Host build/test command and release ESP-IDF build both green from a clean
  checkout.
- Relative-link, conflict-marker, secret, and generated-artifact checks clean.
- Signed image verification and authenticated OTA proven with both positive and
  negative cases.
- At-rest credential protection proven against a physical flash/NVS read.
- Exact third-party notice bundle and dependency inventory attached to the
  firmware release.

## Safety invariants

- Synthetic input cannot clear privacy, answer asks, approve consent, or escape
  operator ownership.
- Remote repair never clears physical hold/flip privacy.
- Brightness is a user ceiling; moods may dim below it, never exceed it.
- Low contiguous memory refuses canvas, snapshot, OTA, or self-test before it
  endangers TLS/voice.
- Screenshots are software submission mirrors, not panel readback. PCM taps prove
  codec-write data, not audible output.
- No keys, tokens, private/device-specific endpoints or addresses, SSIDs, NVS
  images, or device logs enter the repository.
