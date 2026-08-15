# Roadmap

JarvisNano is Waveshare-first for v1. Last reconciled: **2026-08-14** against
`origin/v5` (`3aff8e04`) plus `docs/evidence/` from 2026-07-18/19.

XIAO / camera / Android / BLE remain post-v1. They do not block the USB
desktop assistant.

## v1 Target

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.75.
- Form factor: USB-powered desktop assistant.
- UX: voice + expressive round face + touch cockpit.
- Client: device firmware + browser dashboard first.
- Security: pairing token for writes is still **open**; secrets only in NVS.
- Tools: JarvisMCP bridge through Gemini Live tool calls.
- Memory: on-device memory without storing secrets.

## Phase 0 — Land v5

- [x] Merge display/runtime recovery onto the live branch.
- [x] Keep the direct CO5300 primitive (no LVGL migration).
- [x] Soft-fail emote mount instead of bootlooping.
- [x] Document the display snapshot contract (software mirror, not panel readback).
- [x] v5 boots on hardware (`docs/evidence/20260718-v5-boot.log`).

## Phase 1 — Touch, sensors, display runtime

- [x] CST9217 touch path into the session machine.
- [x] QMI8658 IMU + AXP2101 battery in v5 components.
- [x] Overlay compositor in the baked art's negative space (no second HUD).
- [x] Short tap starts/stops listening; tap-to-interrupt while speaking.
- [ ] Long-press cockpit/menu — verify on current image (was UI-layer on the old stack).
- [ ] Diagnostic-only touch injection behind a pairing token.

## Phase 2 — Voice and face

- [x] Gemini Live as the primary loop (idle → listen → think → speak).
- [x] Face states bound to session (thinking spinner, listen/speak waveform).
- [x] ES7210 / ES8311 path in the v5 image; 16 kHz shared clock + resample.
- [x] API key on `x-goog-api-key`, not the WebSocket query string (`a88a1941`).
- [ ] `JR_CMD_PUBLISH_SNAPSHOT` is still a no-op in `main/main.c` — face still polls.
- [ ] Host-testable UI port / consent flow (no `jr_ports/ui.h` yet).
- [ ] Re-prove mic + speaker on the currently connected board (USB first).
- [ ] Pairing-token gate for remaining diag POSTs (`/api/demo`, HUD, say).

## Phase 3 — Cheap glass (shipped 2026-07-19)

- [x] Touch ripple, live captions, status captions (MUTED / CONNECTION LOST).
- [x] Ambient watch face.
- [x] Attract reel (`POST /api/demo`).
- [x] Time-aware courtesy.
- [x] Flip-to-mute, shake-to-cancel.
- [ ] Listen countdown rim — **do not build** until listening is windowed.
      `VOICE_ALWAYS_READY` keeps the deadline at 0.

## Phase 4 — Choice arcs (shipped 2026-07-19)

- [x] `ask_user` on glass; tap answers; `functionResponse` closes the loop.
- [x] Hardware photos in `docs/evidence/20260719-choice-arcs.png`.

## Phase 5 — Moods and leftover silicon (in progress)

- [x] Four-mood rest ladder (AWAKE / AMBIENT / WHISPER / DREAM) — dim + clock + lift-to-wake. No deep sleep, no rail gating.
- [x] CO5300 brightness follows mood.
- [x] PCF85063 RTC claimed for the watch face when SNTP is dark.
- [ ] WakeNet ("Hey Jarvis") — still the hands-free entry gate.
- [ ] Clean checkout `./scripts/build-v5.sh`.
- [ ] Preserve-config flash + wiped-storage flash.
- [ ] 5-minute serial boot watch on the connected board.
- [ ] Pairing token for writes/control in public builds.
- [ ] Dashboard Waveshare-first (hide XIAO WebSerial blob as current firmware).
- [ ] Public secret/identifier scan.
- [ ] Rotate any Gemini key that hit pre-fix serial/SD logs.

## Post-v1

- WakeNet ("Hey Jarvis") + AMBIENT / WHISPER / DREAM moods. Gated on esp-sr.
- BLE state/control, then Android privacy mode.
- Battery/PMIC polish beyond the current AXP read path.
- Camera / XIAO parity. Do not block Waveshare v1.

**Archive:** older finish lists live in [`ARCHIVE/`](ARCHIVE/README.md).
