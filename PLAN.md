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
| N6.12 | P2 | Move stillness detection to the QMI8658 wake engine on **INT1** | PIN RESOLVED; PROBE PENDING | Pin was INT2 in this row and is wrong: the C schematic routes **INT1** (`QMI_INT2` appears once in the whole document, so it has no second endpoint) — see `docs/reference/imu-interrupt-routing.md`, which also carries the sourced WoM register sequence. Gate: scope-prove GPIO21 toggles; preserve flip/shake/lift **across the mode switch** (WoM outputs no data, so it disables both while armed); still task wakes <2/s |
| N6.13 | P0 release | Complete authenticated, signed OTA | BLOCKED ON RELEASE CREDENTIALS | Wrong-key/unsigned images fail before write; upload is authenticated; Secure Boot eFuse remains separately attended |
| N6.14 | P0 release | Protect Wi-Fi and cloud credentials at rest | BLOCKED ON ATTENDED KEY PROVISIONING | NVS/flash encryption is enabled through a separately attended procedure; a physical flash read cannot recover credentials; recovery is documented and tested |
| N6.15 | P0 release | Ship complete third-party notices and dependency inventory | PENDING | The exact resolved firmware dependencies, versions, licenses, and required notice texts are generated after build and attached to every binary release |

## Wave N7 — the glass: one interaction grammar, endless screens

Design of record: [`docs/GLASS_DESIGN.md`](docs/GLASS_DESIGN.md) (what surfaces
exist and why) and [`docs/INPUT_MAP.md`](docs/INPUT_MAP.md) (what every input
does, and why it is guessable). `scripts/gesture-doctor.py` lints four of these
invariants from the tree and must stay green.

### Shipped — on the device, evidence in the commit

| # | Deliverable | Commit | Evidence |
|---|---|---|---|
| N7.1 | Close the tap/swipe dead band | `232fe985` | Drift 31–41 px emitted NO event; tap slop now equals swipe min travel. Doctor asserts no gap |
| N7.2 | An open ask claims rim swipes, hit-tested at the landing point | `232fe985` | Arcs sit at r215–255 where a press always rolls; measured 44 swipes to 12 taps while answering |
| N7.3 | Un-trap the shade; every capturing surface names its exit | `80636399`, `956d194a` | Was: only UP escaped, DOWN did nothing, caption cleared. Doctor asserts ≤2 `caption_clear()` sites |
| N7.4 | Refusal ≠ acceptance ≠ unbound | `1e585570`, `617d37e3` | Contracting dim ring + falling 700→300 Hz; neutral ack for unbound; ADJUST no longer ripples |
| N7.5 | An open ask outranks double-tap-home | `20a921c5` | A fast retry after a missed arc was fired as `nav_home` over a live question. Doctor asserts the guard |
| N7.6 | Both level slabs increase on UP | `20a921c5` | Brightness rose on DOWN while volume rose on UP |
| N7.7 | Delete the side pages and the fake TOOLS feed | `6de40bd2`, `1a7485dd` | `{SEARCH, MEMORY, WEATHER, MORE}` was seeded once with `recent` pinned to 0 |
| N7.8 | HAL contact lifecycle (`PRESS_DOWN`/`PRESS_UP`) | `1d07f621` | Every prior event was terminal, so a filling ring had nothing to drive it. Queue 8→16; excluded from gesture telemetry |
| N7.9 | Hold-to-commit ring, abandonable | `1a081e7e`, `617d37e3` | Owner's long-press counter read 0 — the gesture was invisible. Previews at 400 ms, fills to 850 ms, silent abandon |
| N7.10 | Rim as a true annulus (r ≥ 168) | `3cbab992` | Slabs reached r≈93, over the reactor core. Doctor asserts both predicates agree |
| N7.11 | BOOT ≥5 s → panic-home | `77824301` | The band was unbound; a hold past 5 s did nothing |
| N7.12 | Endless mode ring, vertical slide | `d66db45a`, `5f8d12bd` | Wraps both ways; slide axis corrected from horizontal to vertical to match the finger |
| N7.13 | Dev mode: no pairing token on diagnostics | `2be528e8` | ⚠️ **Set `JR_DEV_OPEN_DIAGNOSTICS 0` before any release.** Boot logs warn every boot |

### Open

| # | Priority | Deliverable | Status | Acceptance gate |
|---|---|---|---|---|
| N7.14 | P1 | Give TOOLS real content, or drop it from the ring | OPEN | TOOLS renders a tool that actually ran (name + outcome) within 2 s of a real tool call, or the screen is removed from the ring. **No hardcoded lists** — an empty screen telling the truth beats a full one that lies |
| N7.15 | P1 | Add WATCH, POWER and MOTION to the ring | OPEN | Each shows only live data: WATCH via `hud_overlay_clock` (exists, strip-tested); POWER via `jr_power` (%, charging, mV); MOTION via `jr_imu` (live tilt). Each passes strip invariance and holds ≥14 FPS |
| N7.16 | P1 | Position indicator that survives a wrap | OPEN | Wrapping must not jump the indicator the width of the dial. Draw position as a **rotating** mark; the caption naming each screen is the interim answer |
| N7.17 | P1 | Input layer stack (`CONSUMED`/`PASS`) | OPEN — sequenced after deletions | Dispatch is ~430 lines with 20+ `continue`s. Precedence becomes a declared table; every affordance in `INTERACTION_MODEL.md` §5 still reachable; no binding changes |
| N7.18 | P2 | Enforce the coverage invariant in a host test | OPEN | A table lists every binding and its non-gesture (voice) equivalent; a binding with no equivalent fails the build. Pairing is the one allowed exception (physical-presence proof) |
| N7.19 | P2 | Re-home the OTA ring, then delete the SETTINGS renderer | OPEN | OTA progress stays visible without `nav_set(SETTINGS)`; only then may `sp_focal_settings` go. **Do not delete first** — it would blind a firmware update |
| N7.20 | P2 | Remove the orphaned shell setters | OPEN | `jr_display_desk_set_task` / `tools_set` / `space_set_label` have no production callers but are exercised by `test_shell.c`; remove implementations and their tests in one change |
| N7.21 | P2 | PWR double-tap for privacy | BLOCKED BY MEASUREMENT | The AXP2101 PKEY latch polls at 500 ms (`jr_power.c:33`), so a ~400 ms window cannot be resolved; `iot_button` cannot help (GPIO/ADC only). Needs a faster PKEY poll (costs shared-bus traffic) or an AXP multi-press feature. Until then privacy stays on the glass hold |
| N7.22 | P2 | Auto-upright the procedural overlay | OPEN | 4-way quadrant snap with hysteresis + debounce, procedural layer only. **The choice-arc hit test must take the same offset** or taps target pre-rotation positions. Baked faces stay fixed; `hud_tilt_offset()` is the wrong hook (translation, wrong axes) |

## Execution order

1. **Correctness:** N6.1–N6.4.
2. **The glass, content first:** N7.14–N7.16. The ring is live but two of its
   four screens have nothing worth reading; that is the same failure the side
   pages died of, so it outranks refactoring.
3. **Maintainability:** N7.17 (layer stack) then N6.5–N6.8. Deletion before
   refactor: N7.19/N7.20 remove code that N7.17 would otherwise re-home.
4. **Performance/hardware:** N6.9–N6.12, N7.22.
5. **Before any release:** N7.13 — set `JR_DEV_OPEN_DIAGNOSTICS 0`.
4. **Public release:** N6.13–N6.15 after credentials and attended device access exist.

## Release gates

- One clean 30-minute voice soak with the counters in N6.4.
- Physical PWR, BOOT, both edge controls, glass privacy, flip privacy, and ROM
  downloader recovery witnessed on the target board.
- Host build/test command and release ESP-IDF build both green from a clean
  checkout.
- **`JR_DEV_OPEN_DIAGNOSTICS` is 0** (N7.13). While it is 1 the pairing token is
  not required on `/api/logs`, `/api/cockpit`, the audio taps or
  `/api/debug/input`, so anything on the LAN can read logs, hear the mic and
  inject input. `grep -rn JR_DEV_OPEN_DIAGNOSTICS main/` before tagging.
- `scripts/gesture-doctor.py` reports **healthy** against the release build.
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
