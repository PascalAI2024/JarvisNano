# Changelog

Notable user-facing changes to the supported JarvisNano 1.75C product are
recorded here. Historical ESP-Claw, XIAO, and original-1.75 development notes
are preserved in [`docs/ARCHIVE/CHANGELOG-v4.md`](docs/ARCHIVE/CHANGELOG-v4.md).

## Unreleased

### The glass (2026-09-01)

- Search works by voice. `execute_tool` runs on the device's legacy route
  behind a device-side read-only allowlist, and `search_tools` returns a
  compact projection with web search, weather and Wikipedia pinned. One tool
  call per question.
- A new ring: JARVIS, WATCH, **WEATHER** (Fort Lauderdale, live, with its age),
  **STATUS** (the device at a glance: LINK and TOOLS lamps, the battery arc
  with the percentage inside it, Wi-Fi bars with the dBm, and a sheet of nine
  facts — battery, power, Wi-Fi, IP, link, tools, desk, radio power mode,
  update), DESK only while an agent or companion is live, and
  **ACTIVITY** (the last three things Jarvis did). TOOLS is gone.
- The glass can talk about what it shows: tap an open sheet on WEATHER, WATCH
  or ACTIVITY and the assistant says it aloud.
- Lift to glance: picked up after a rest, the weather shows for eight seconds
  and the glass goes home by itself. A first fetch with rain in the day leaves
  one "RAIN TODAY" line.

- Removed the SETTINGS screen. The firmware-update ring now draws on every
  screen; UPDATE and SLOT rows moved to the POWER sheet; volume and brightness
  readouts live on the control shade.
- TOOLS shows all eight declared tools and lights the one that actually ran.
- The DESK sheet heads with the (marked, shortened) task title.
- One battery alarm colour on one rule: red below 20 %, never while charging,
  on both the rim and the POWER arc.
- The orbit rail stays inside its r185–194 band.
- Panic-home (BOOT held 5 s) also clears test patterns, pushed canvases, the
  touch challenge and the demo reel, and releases the brain surface correctly.
- Rapid taps on the shade's volume arc no longer fire double-tap-home; a single
  tap on an HTTP-opened shade no longer closes it.
- The watch-peek caption leaves with the peek.
- Synthetic swipes walk the ring under a companion lease (taps and holds stay
  physical-only); the "lease freezes the glass" report was a measurement
  artifact of that guard.
- The OTA "DO NOT UNPLUG" caption is pinned until the upload ends.
- `POST /api/agent/link` requires the control-intent header like every other
  mutating route; `/api/demo` answers honestly when the reel cannot start.

### Product target

- Made the 32 MB **Waveshare ESP32-S3-Touch-AMOLED-1.75C** the only
  release-gated target.
- Replaced the generated ESP-Claw composition with a plain ESP-IDF 5.5.4 image
  rooted at `main/` and `components/jr_*`.
- Added a 32 MB DIO partition layout with two 4 MB application slots, emote
  assets, WakeNet models, storage, and rollback metadata.
- Kept the original 16 MB board and XIAO material as explicitly labeled
  hardware/history references; the release build refuses mismatched targets.

### Voice and audio

- Fixed choppy replies. The speaker now runs behind the network with an
  adaptive jitter buffer (600 ms pre-roll, 1000 ms refill after a hole) and a
  96-deep WebSocket receive queue; measured cause was Gemini pacing native
  audio near real time with 0.8–1.34 s stalls. Zero underruns and zero dropped
  frames across the confirmation turns, from seven underruns and an 834 ms hole
  before. Counters at `/api/device/health` (`playback`, `rx`), reset via
  `POST /api/debug/audio-stats?reset=1`, knobs via
  `/api/debug/gain?preroll=&refill=`; `jarvisctl status` reports them.
- `/api/debug/gain` reports device state (mic, ref, vol, speakmic) instead of
  echoing the request.
- Moved Gemini Live framing and WebSocket state into `jr_transport` with bounded
  reconnect, backpressure, and event queues.
- Runs ES7210 capture and ES8311 playback on one native 24 kHz duplex clock;
  AEC-clean uplink is downsampled to 16 kHz and Gemini output remains 24 kHz.
- Made Gemini server VAD the normal interruption path. Local barge remains a
  measured diagnostic fallback after echo-triggered self-interruption caused
  reply hiccups.
- Added PSRAM-backed uplink/playback buffering, electrical audio taps, VAD
  decision logs, task watermarks, and explicit DAC/transport counters.

### Round-screen experience

- Shipped one CO5300 compositor with baked reactor faces, listening halo,
  captions, choices, Watch, controls, Desk/Tools/Settings spaces, remote canvas,
  and bounded operator surfaces.
- Consolidated physical input: global left-edge volume, right-edge brightness,
  horizontal space navigation, top-edge controls, glass-hold/flip privacy,
  PWR listen/battery, and BOOT controls/ROM recovery.
- Removed the unreliable continuous circular-rotation experiment and its dead
  event/state path.
- Added physical provenance so synthetic input cannot clear privacy, approve
  consent, answer asks, or escape operator ownership.

### Tools, diagnostics, and OTA

- Added typed JarvisMCP `/device/v1/invoke`, fixed safe tools, catalog discovery,
  policy-gated execution, local persisted level tools, and physical approval for
  the bounded `remember` write.
- Added the Orbit Console, paired Desk/operator clients, 128 KB incident log,
  display mirrors, panel-touch challenge, audio self-test, and evidence-first
  host tooling.
- Added trusted-LAN dual-slot OTA with power/network/memory preflight, inactive
  slot writes, 45-second health eligibility, a 120-second rollback deadline,
  and privacy-safe recovery.
- Kept signed application verification and authenticated update transport as
  explicit public-release blockers.

### Repository and documentation

- Rebuilt the README, documentation map, architecture, hardware, protocol,
  build, security, support, contribution, release, and debugging guides around
  the live 1.75C product.
- Archived completed/superseded plans and labeled dated subsystem research.
- Replaced dead public commands and original-board defaults with fail-closed,
  current paths.
- Added deterministic host suites, cockpit JavaScript validation, secret and
  identifier scanning, link validation, and clean generated-artifact ignores.

## Release gates still open

- One clean 30-minute natural conversation soak with playback-gap telemetry.
- Byte-budgeted, cursor-bearing JarvisMCP catalog projection under the Gemini
  result limit.
- Physical confirmation of final PWR, BOOT controls, and BOOT pairing semantics.
- Signed images, authenticated encrypted OTA, and attended at-rest credential
  protection.
- Exact third-party notice bundle and dependency inventory for binary releases.
