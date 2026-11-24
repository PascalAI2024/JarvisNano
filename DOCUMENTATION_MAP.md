# Documentation Map

Last reconciled with the live 1.75C firmware: **2026-11-23**.

JarvisNano has one active product target (**Waveshare ESP32-S3-Touch-AMOLED-1.75C**) and one canonical documentation set. Historical plans remain useful context, but they do not define current behavior.

## Source-of-truth order

When two files disagree, trust them in this order:

1. Compiled source in `main/`, `components/jr_*`, and `boards/waveshare/esp32s3_touch_amoled_1_75c/`.
2. [`README.md`](README.md) and the canonical live documents below.
3. [`PLAN.md`](PLAN.md) and [`CURRENT_WORK.md`](CURRENT_WORK.md) for active work, acceptance status, and explicit blockers.
4. `docs/reference/` for dated subsystem probes, benchmarks, and implementation gotchas.
5. `docs/ARCHIVE/` for superseded plans and historical decisions.

## Start here

| Document | Owns |
|---|---|
| [`README.md`](README.md) | Product showcase, live display gallery, capabilities, workflows, quick start |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Active ESP-IDF 5.5.4 runtime: tasks, PSRAM-first SRAM budget, voice, Glass-2, tools, power |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | 1.75C silicon, pin map, verified vs unverified hardware capabilities, deep-sleep wake lines |
| [`docs/INPUT_MAP.md`](docs/INPUT_MAP.md) | Complete physical control map (buttons, glass annulus, body IMU) and precedence rules |
| [`docs/PROTOCOL.md`](docs/PROTOCOL.md) | Embedded Command Deck, HTTP routes, auth gates, 24 voice tools, Codex & ZeroChat leases |
| [`docs/BUILD.md`](docs/BUILD.md) | Docker build, USB flash, Wi-Fi OTA, 23 host test suites, Windows & macOS CLI workflows |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Shipped milestones, active verification, and public release gates |
| [`docs/RELEASE_CHECKLIST.md`](docs/RELEASE_CHECKLIST.md) | Tag and public publish gates |

## Canonical live set

### Product & design
- [`README.md`](README.md) — open-source entry point and visual overview.
- [`docs/VISION.md`](docs/VISION.md) — experience contract and refinement principles.
- [`docs/GLASS_DESIGN.md`](docs/GLASS_DESIGN.md) — Glass-2 spatial ring, 8 watch dials, and surface design.
- [`docs/INPUT_MAP.md`](docs/INPUT_MAP.md) — physical input grammar, feedback classes, and coverage invariant.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — product arc and release milestones.
- [`PLAN.md`](PLAN.md) & [`CURRENT_WORK.md`](CURRENT_WORK.md) — actionable tracker and physical acceptance log.

### System & internals
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — single-owner runtime architecture.
- [`docs/HARDWARE.md`](docs/HARDWARE.md) — 1.75C capability map and physical constraints.
- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — HTTP control plane, auth matrix, tool schemas, companion leases.
- [`docs/BRAIN_ARCHITECTURE.md`](docs/BRAIN_ARCHITECTURE.md) — Gemini Live, local tools, JarvisMCP, Codex Desk, and ZeroChat routes.

### Operations & governance
- [`docs/BUILD.md`](docs/BUILD.md) — reproducible Docker builds, flashing, OTA, and host test gates.
- [`docs/LIVE_DEVICE_DEBUG.md`](docs/LIVE_DEVICE_DEBUG.md) — evidence-first runtime debugging.
- [`docs/RELEASE_CHECKLIST.md`](docs/RELEASE_CHECKLIST.md) — public release verification gates.
- [`SECURITY.md`](SECURITY.md) — secret handling, threat boundary, and vulnerability reporting.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) & [`SUPPORT.md`](SUPPORT.md) — contribution rules and support scope.
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) — binary redistribution and exact notice-bundle policy.

## Evidence & dated engineering references

| Path | Purpose |
|---|---|
| [`docs/evidence/README.md`](docs/evidence/README.md) | Tracked hardware captures, logs, and provenance |
| [`docs/reference/README.md`](docs/reference/README.md) | Index of dated subsystem deep-dives |
| [`docs/reference/board-capabilities.md`](docs/reference/board-capabilities.md) | Schematic-level 1.75C capability audit and `/api/diag/board` probe guide |
| [`docs/reference/board-175c.md`](docs/reference/board-175c.md) | 1.75C vs original 1.75 deltas |
| [`docs/reference/power-modes.md`](docs/reference/power-modes.md) | Sleep states, wake lines, and power measurements |
| [`docs/reference/crash-forensics.md`](docs/reference/crash-forensics.md) | Reset reason, PMIC boot record, and flash ELF coredump inspection |
| [`docs/reference/playback-jitter.md`](docs/reference/playback-jitter.md) | Adaptive 24 kHz jitter buffer design and measurements |
| [`docs/reference/tilt-parallax.md`](docs/reference/tilt-parallax.md) | Watch dial specular lighting and CELESTIAL star parallax |
| [`docs/reference/interface-cues.md`](docs/reference/interface-cues.md) | Harmonic UI earcons and feedback tone policy |
| [`docs/reference/build-toolchain.md`](docs/reference/build-toolchain.md) | Docker IDF 5.5.4 + Windows Git Bash `MSYS_NO_PATHCONV=1` notes |
| [`boards/waveshare/esp32s3_touch_amoled_1_75c/README.md`](boards/waveshare/esp32s3_touch_amoled_1_75c/README.md) | Board-local pin and peripheral declarations |

## Compatibility tracks & archived trees

| Path | Status |
|---|---|
| `boards/waveshare/esp32s3_touch_amoled_1_75/` | **Reference only** — original 16 MB board (has RTC/microSD/TCA9554); not release-built |
| `boards/seeed/xiao_esp32s3_sense/` | **Reference only** — compact camera experiment; not release-built |
| `android/` | **Scaffold** — post-v1 private companion app |
| `hardware/enclosure/` | **Fabrication** — physical enclosure concepts and STL export |
| `docs/ARCHIVE/` | **Archived** — superseded plans and historical OS records |
| `dashboard/` | **Archived** — legacy browser/XIAO UI; superseded by embedded `main/diagnostics.html` |
| `firmware/` & `esp-claw/` | **Archived** — earlier architecture/overlays (only `firmware/components/jarvis_board` is linked into v5) |

## Documentation rule

A behavior is documented as **live** only when compiled firmware and physical 1.75C evidence or bounded runtime counters prove it. Otherwise mark it **wired / unverified**, **planned**, or **archived**. Never promote an HTTP 200, software framebuffer mirror, PCM buffer write, or synthetic touch into physical proof.
