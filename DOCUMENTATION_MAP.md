# Documentation Map

Last reconciled with the live 1.75C firmware: **2026-08-28**.

JarvisNano has one active product target and one canonical documentation set.
Historical plans remain useful evidence, but they do not define current behavior.

## Source-of-truth order

When two files disagree, use this order:

1. Compiled source in `main/`, `components/jr_*`, and the selected board definition.
2. [`README.md`](README.md) and the canonical live documents below.
3. [`PLAN.md`](PLAN.md) for incomplete work and explicit blockers.
4. `docs/reference/` for dated subsystem evidence and implementation gotchas.
5. `docs/ARCHIVE/` for superseded plans and historical decisions.

## Start here

| Document | Owns |
|---|---|
| [README](README.md) | Product story, capabilities, quick start, truth boundaries |
| [Vision](docs/VISION.md) | Experience contract and refinement principles |
| [Architecture](docs/ARCHITECTURE.md) | Live v5 components, ownership, and data flow |
| [Hardware](docs/HARDWARE.md) | 1.75C hardware, constraints, pins, and physical controls |
| [Protocol](docs/PROTOCOL.md) | HTTP routes, authentication, paired operations, state contracts |
| [Build](docs/BUILD.md) | Reproducible build, USB flash, Wi-Fi OTA, verification |
| [Release checklist](docs/RELEASE_CHECKLIST.md) | Public release gates and evidence |
| [Plan](PLAN.md) | Current actionable roadmap and blockers |

## Canonical live set

### Product and design

- [`README.md`](README.md) — public showcase and entry point.
- [`docs/VISION.md`](docs/VISION.md) — why the product behaves this way.
- [`PLAN.md`](PLAN.md) — what remains, with measurable acceptance gates.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — release phases; must agree with PLAN.

### System

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — active ESP-IDF v5 architecture.
- [`docs/HARDWARE.md`](docs/HARDWARE.md) — primary 1.75C capability map.
- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — live route and authority contract.
- [`docs/BRAIN_ARCHITECTURE.md`](docs/BRAIN_ARCHITECTURE.md) — Gemini, local tools,
  JarvisMCP, Desk, and future private routes.

### Operations

- [`docs/BUILD.md`](docs/BUILD.md) — build/flash/OTA workflow.
- [`docs/LIVE_DEVICE_DEBUG.md`](docs/LIVE_DEVICE_DEBUG.md) — evidence-first runtime debugging.
- [`docs/RELEASE_CHECKLIST.md`](docs/RELEASE_CHECKLIST.md) — tag/publish gate.
- [`SECURITY.md`](SECURITY.md) — secret handling and vulnerability reporting.
- [`SUPPORT.md`](SUPPORT.md) — user support boundary.
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) — binary redistribution and exact notice-bundle policy.

## Evidence and reference

- [`docs/evidence/README.md`](docs/evidence/README.md) — captured hardware evidence.
- [`docs/reference/README.md`](docs/reference/README.md) — dated engineering references.
- [`docs/reference/board-175c.md`](docs/reference/board-175c.md) — C-board deltas.
- [`docs/reference/board-bringup-checklist.md`](docs/reference/board-bringup-checklist.md)
  — mandatory checklist for any new board revision.
- [`boards/waveshare/esp32s3_touch_amoled_1_75c/README.md`](boards/waveshare/esp32s3_touch_amoled_1_75c/README.md)
  — board-local integration details.

Reference files may preserve failed experiments and old measurements. Each
reference should state its date and whether a newer canonical document supersedes
its conclusions.

## Compatibility and future tracks

| Path | Status |
|---|---|
| `boards/waveshare/esp32s3_touch_amoled_1_75/` | Original 16 MB hardware/source reference; not release-built |
| `boards/seeed/xiao_esp32s3_sense/` | Camera/compact experimental track |
| `android/` | Post-v1 private companion scaffold |
| `hardware/enclosure/` | Physical enclosure concepts and fabrication |
| `dashboard/` | Legacy browser/XIAO track, not the v5 route authority |

## Historical trees

- `docs/ARCHIVE/` — completed or superseded plans.
- `docs/ARCHIVE/JARVISNANO_OS_PLAN.md` — historical OS design record.
- `firmware/` and `esp-claw/` — earlier architecture/experiments, not built by
  `scripts/build-v5.sh`.

## Documentation rule

A behavior is documented as **live** only when the physical 1.75C or a bounded
runtime counter proves it. Otherwise mark it planned, experimental, blocked, or
historical. Never promote an HTTP 200, software mirror, PCM buffer, or synthetic
touch into physical proof.
