# Support

JarvisNano is an open-source hardware project. Useful reports identify the exact
board, source revision, failing surface, and evidence boundary.

## Before asking

- Start with [`README.md`](README.md) and
  [`DOCUMENTATION_MAP.md`](DOCUMENTATION_MAP.md).
- Follow [`docs/BUILD.md`](docs/BUILD.md) for the primary 1.75C build and flash.
- Use [`docs/LIVE_DEVICE_DEBUG.md`](docs/LIVE_DEVICE_DEBUG.md) for bounded logs,
  counters, audio taps, and display mirrors.
- Check [`PLAN.md`](PLAN.md) for known blockers before opening a duplicate.
- Read [`docs/HARDWARE.md`](docs/HARDWARE.md) before applying board-family pin,
  power, battery, or speaker advice. The 1.75C and original 1.75 differ.
- Camera and Android reports belong to compatibility/future tracks; identify
  them explicitly rather than treating them as the primary product.

## A useful bug report includes

- Exact board model/revision and purchase batch, if known.
- Source commit plus whether the working tree had local changes.
- Host OS, build command, flash/OTA path, and reset cause.
- Expected behavior, actual behavior, and the shortest reproduction.
- A sanitized counter summary from `scripts/live-device.py report`; never paste
  the raw cockpit/session response.
- For touch/buttons: physical input receipt and resolved action.
- For display: submission mirror plus a physical photo when the claim concerns
  glass output.
- For audio: electrical tap evidence plus an acoustic observation when the claim
  concerns audible output.

Do not include keys, pairing tokens/hashes, Wi-Fi credentials, SSIDs, device or
host addresses, MACs, private endpoints, OAuth/browser state, NVS/flash dumps,
local databases, personal assistant content, or unredacted device logs. Report
security-sensitive issues through [`SECURITY.md`](SECURITY.md), not a public
issue.
