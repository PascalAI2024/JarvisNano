# Next Session Handoff

Last updated: **2026-08-27** (the 1.75C elevation day).

v5 runs on the **Waveshare ESP32-S3-Touch-AMOLED-1.75C** — the 32 MB-flash
aluminum revision, the default build target. Milestones shipped today: board
port (`56a799b`), "Jarvis" wake word (`9dcfe99`), every deaf-device path
closed (`4c3a837`, `482c81d`), realtime playback feeder (`11e66a8`), remote
canvas + seconds hand (`05f4bc0`), operator tooling (`500b297`), and the
eased-glass experience wave with a teammate (`dd5d48b`, `a615a52`).

## Current Board + Build

- Board deltas, calibrations, and the provisioning-transplant recipe:
  [`reference/board-175c.md`](reference/board-175c.md). **On ANY new board
  revision, run [`reference/board-bringup-checklist.md`](reference/board-bringup-checklist.md)
  first — physics does not port.**
- `./scripts/build-v5.sh` targets the 1.75C by default; original 1.75 needs
  `BOARD_NAME=esp32s3_touch_amoled_1_75`. Flipping an EXISTING sdkconfig
  symbol needs `RECONFIGURE=1` (see build-toolchain.md — stale sdkconfig wins).
- Serial is single-owner: kill any usb-monitor before flashing.

## Talk to the device (no serial needed)

```bash
export JARVIS_DEVICE_HOST=<device-ip>
scripts/jarvisctl.py status     # one-line verdict; non-zero exit = deaf/muted
scripts/jarvisctl.py logs       # on-device 128 KB log ring — read AFTER the fact
scripts/jarvisctl.py screen     # capture the glass
scripts/jarvisctl.py say "..."  # speak a turn
scripts/jarvisctl.py canvas img.png   # pixels on the glass (TTL-bounded)
scripts/jarvisctl.py lease 300 / release   # operator claim; owner tap evicts
scripts/jarvisctl.py tune pbgain=250 speakmic=21   # live audio calibration
```

**Before debugging "no response" as firmware: run `status`.** Nearly every
"dead device" today was a privacy state (see BUGLOG.md B5/B6).

## Interaction model (post tap-redesign)

Tap = stop-talking / attention (NEVER mutes). Double-tap = bloom + "YES,
SIR?". Swipe-left = status glance; swipe-right = 10 s watch peek; swipe-down
from top = shade. Flip face-down = privacy mute / passive watch (the ONLY
casual mute); long-press = mute + shade. "Jarvis" wakes from rest and from
any non-deliberate silence — it never overrides a deliberate mute.

## Open threads (see PLAN.md + BUGLOG.md for the full boards)

- Owner's consolidated verdict pending — last rating 2/10 before the
  experience wave; re-rate after the full circuit.
- glass-ux teammate has an UNCOMMITTED in-flight slice in the working tree:
  choice-arc dismissal fades (tapped arc stays lit through exit). Commit it
  on their handover, not before.
- Long-session soak (30+ min unattended) never run. B9 (one unexplained
  reboot, coredump blank — suspect USB brownout) still WATCH.
- Perceptual dim curve + ring seam latch: glass-ux has fixes ready if the
  owner's eyes flag either.
- 128 MB upper flash still unpartitioned beyond `model` — dual-OTA and
  bigger asset space live there when wanted.

## Do Not Repeat

- Do not port calibrations between board revisions — measure (bringup checklist).
- Do not add a second mute path to casual gestures; that trap cost hours.
- Do not run builds while a teammate's domain has half-landed edits.
- Do not commit LAN addresses, MACs, SSIDs, keys, or NVS dumps — and delete
  local NVS dumps as soon as a transplant is done (they carry the keys).
- `/api/display/snapshot.ppm` is a software mirror, not panel readback.
- Negative-space rule stands: nothing new over the baked rwave art
  (JARVISNANO_OS_PLAN.md).
