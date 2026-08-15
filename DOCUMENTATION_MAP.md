# Documentation Map

Last updated: 2026-08-14.

v5 on the Waveshare ESP32-S3-Touch-AMOLED-1.75 is the live product. If two
docs disagree, prefer this order: `docs/NEXT_SESSION.md` →
`docs/reference/` → `docs/ROADMAP.md` → `docs/JARVISNANO_OS_PLAN.md` (design
record) → `docs/ARCHIVE/` (history only).

## Quick Links

| Start here | Why |
|---|---|
| [README](./README.md) | What the device is |
| [Next session](./docs/NEXT_SESSION.md) | USB, build, do-not-repeat |
| [Build](./docs/BUILD.md) | `build-v5.sh` / `flash-v5.sh` |
| [Roadmap](./docs/ROADMAP.md) | What is shipped vs still open |
| [Reference](./docs/reference/README.md) | Gotchas with file:line |
| [Evidence](./docs/evidence/README.md) | Hardware proof |
| [Archive](./docs/ARCHIVE/README.md) | Superseded plans |

## Live vs leftover trees

| Tree | Role |
|---|---|
| `main/` + `components/jr_*` + `boards/` | **Live v5 firmware** |
| `docs/reference/` | Canonical subsystem notes |
| `firmware/` + `esp-claw/` + `scripts/bootstrap.sh` | Legacy overlay. Not compiled by v5 CMake |
| `dashboard/` | Browser cockpit. WebSerial blob is a **legacy XIAO** image |
| `android/` | Companion app, post-v1 |
| `docs/ARCHIVE/` | Finished or superseded plans |

## By category

### Product

| Document | Notes |
|---|---|
| [VISION.md](./docs/VISION.md) | Character / moods. LVGL item is obsolete. |
| [ROADMAP.md](./docs/ROADMAP.md) | Current checkbox truth |
| [JARVISNANO_OS_PLAN.md](./docs/JARVISNANO_OS_PLAN.md) | July 18 design record; Phases 0–4 shipped |
| [prototype](./docs/prototype/jarvisnano-os.html) | Clickable OS mock |

### Architecture and hardware

| Document | Notes |
|---|---|
| [ARCHITECTURE.md](./docs/ARCHITECTURE.md) | Roles. v5 names are `jr_*` |
| [HARDWARE.md](./docs/HARDWARE.md) | Board wiring |
| [PROTOCOL.md](./docs/PROTOCOL.md) | HTTP / BLE contract (BLE is post-v1) |
| [CAMERA.md](./docs/CAMERA.md) | XIAO camera track |
| [BRAIN_ARCHITECTURE.md](./docs/BRAIN_ARCHITECTURE.md) | Memory / tools sketch |

### Operations

| Document | Notes |
|---|---|
| [BUILD.md](./docs/BUILD.md) | Docker IDF 5.5.4 |
| [LIVE_DEVICE_DEBUG.md](./docs/LIVE_DEVICE_DEBUG.md) | HTTP diag after USB proves boot |
| [RELEASE_CHECKLIST.md](./docs/RELEASE_CHECKLIST.md) | Before a public tag |
| [reference/build-toolchain.md](./docs/reference/build-toolchain.md) | Pins, sdkconfig, flash mode |

### Do not start from these

Everything under [`docs/ARCHIVE/`](./docs/ARCHIVE/README.md), including the
NullClaw `plan.md`, the May XIAO finish list, and the "Gemini not started"
plan.

## Leftover binaries

`dashboard/firmware/jarvis-xiao-esp32s3-sense.bin` (~8 MB) is a published
XIAO WebSerial image, not the v5 AMOLED firmware. Flash Waveshare with
`scripts/flash-v5.sh`. Removing the blob is a product decision (breaks the
old in-browser XIAO installer).
