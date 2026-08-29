# Next Session Handoff

Last reconciled: **2026-08-28**.

The live target is the **Waveshare ESP32-S3-Touch-AMOLED-1.75C** with 32 MB
flash. The active image is plain ESP-IDF v5 rooted at `main/` and
`components/jr_*`.

## Start here

```bash
./scripts/build-v5.sh
./scripts/flash-v5.sh

export JARVIS_DEVICE_HOST='<device-ip>'
# Blank host/device only: hold BOOT 1.5–5 s, then run:
python3 scripts/jarvis-desk.py --host "$JARVIS_DEVICE_HOST" pair
python3 scripts/jarvisctl.py status
python3 scripts/jarvis-desk.py --host "$JARVIS_DEVICE_HOST" doctor
python3 scripts/jarvisctl.py gestures 80
python3 scripts/jarvisctl.py logs 131072
python3 scripts/jarvisctl.py screen
```

USB-Serial-JTAG is single-owner. Stop an active monitor before flashing. A
charge-only cable can power the board without creating `/dev/cu.usbmodem*`;
Wi-Fi OTA may still pass preflight when USB power is present.

## Current interaction grammar

| Input | Action |
|---|---|
| PWR short | Listen/wake only; never mute |
| PWR long | Battery and charging status |
| BOOT short after boot | Open/close controls |
| BOOT hold 1.5–5 s after boot | Open a visible 60-second pairing claim window |
| BOOT held during reset | Enter ROM downloader |
| Left-edge vertical | Volume +/− 5 globally |
| Right-edge vertical | Brightness +/− 5 globally |
| Horizontal swipe | Jarvis ↔ Desk ↔ Tools ↔ Settings |
| Top-edge down | Open controls |
| Centre up | Detail or controls close |
| Double tap | Jarvis Home |
| Glass hold | Physical privacy mute/unmute |
| Sustained face-down / face-up | Enter flip privacy / clear only a flip-origin mute |

The controls surface is the on-device legend: `L VOL`, `R LIGHT`,
`PWR LISTEN`, `BOOT CLOSE`, and centre MUTE/LISTEN. Do not reintroduce the
failed continuous circular-rotation recognizer; the CST9217’s reliable signal
is the classified edge-origin swipe.

## Current runtime truth

- Gemini Live is direct from the device over Wi-Fi.
- Capture/playback share a native 24 kHz I²S clock; uplink is AEC-cleaned and
  downsampled to 16 kHz.
- Server VAD owns turn boundaries. Local VAD is observability/pacing; local
  barge is not the primary path.
- Display uses one compositor, 12-row internal-DMA strips, baked EAF faces, and
  a sparse listening halo. Normal Jarvis/Watch cadence is roughly 15–16 FPS.
- PWR and USB-powered mood policy keep the desk assistant listening; deliberate
  glass/flip privacy still wins.
- OTA uses two 4 MB slots and validates voice/network/tools/HTTP/wake/display
  before marking a new image valid.
- JarvisMCP server policy is live; byte-budgeted device catalog projection and
  cursor semantics remain incomplete.

## Current blockers

1. **Long-session voice:** run N6.2/N6.4 playback-gap telemetry and a clean
   30-minute conversation soak.
2. **JarvisMCP:** replace fixed-count search output with a ≤3071-byte projection
   carrying `has_more` and a cursor; prove voice search + execution.
3. **Controls shade:** raise settled controls cadence to ≥14 FPS without losing
   behavior or internal memory.
4. **First face transitions:** prewarm assets so first Think/Speak applies under
   150 ms and stale requests never flash late.
5. **Release security:** trusted-LAN OTA works, but signed images, authenticated
   encrypted upload, at-rest credential protection, and exact third-party
   notices remain public-release gates.

The actionable order and acceptance criteria are in [`../PLAN.md`](../PLAN.md).

## Safety invariants

- Synthetic input cannot clear privacy, answer asks, approve consent, or escape
  operator ownership.
- Remote resume never clears physical hold/flip privacy.
- Screenshots are submitted software buffers, not panel readback.
- PCM taps prove codec-write data, not audible speaker output.
- Never print or commit keys, tokens, endpoints, SSIDs, addresses, NVS images,
  or device-specific logs.
- Secure Boot/eFuse work is physically attended and separate from ordinary OTA.

## Do not repeat

- Do not build `firmware/` or `esp-claw/` when validating v5.
- Do not add LVGL or a second renderer beside the live compositor.
- Do not put display DMA buffers in PSRAM.
- Do not treat `transport_poll_write(0)` as a socket death.
- Do not use raw tool result count as a byte-budget guarantee.
- Do not turn a normal tap or PWR press into an accidental privacy toggle.
- Do not claim physical proof from an HTTP response alone.
