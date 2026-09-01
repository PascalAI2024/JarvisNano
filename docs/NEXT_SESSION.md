# Next Session Handoff

Last reconciled: **2026-09-01**.

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
| Centre vertical swipe | The ring: Jarvis ↔ Watch ↔ Power ↔ Desk ↔ Tools (wraps) |
| Horizontal swipe | Watch peek, 10 s |
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

## What changed on 2026-09-01

- **Voice is smooth.** The speaker now runs behind the network: 600 ms pre-roll
  before a reply's first word, a 1000 ms lead rebuilt after any hole, and a
  96-deep WebSocket queue. Cause, measured: Gemini paces native audio near real
  time with 0.8–1.34 s stalls mid-sentence. Counters live at
  `/api/device/health` (`playback`, `rx`); reset with
  `POST /api/debug/audio-stats?reset=1`; tune with
  `/api/debug/gain?preroll=&refill=`. Probe: reset → `/api/debug/say` → poll →
  read. See `docs/reference/gemini-live-api-v5.md` §7.
- **SETTINGS is gone.** The update ring draws on every screen; UPDATE/SLOT rows
  live on the POWER sheet; volume/brightness readouts are on the shade.
- **TOOLS shows all eight tools**, the DESK sheet heads with the task, the orbit
  rail stays in r185–194, one battery red, panic-home clears everything, the
  shade survives rapid volume taps, the peek caption leaves with the peek.
- **S21 refuted:** a lease never froze the glass; synthetic swipes were being
  refused under a lease. They now walk the ring; taps/holds stay physical-only.
- **Flashing a live device:** esptool could not sync over USB-JTAG while the
  firmware ran. POST `build/jarvisrobot_v5.bin` to `/api/ota/upload` with
  `X-JarvisNano-Control: 1` (no token while `JR_DEV_OPEN_DIAGNOSTICS` is 1);
  ~45 s, back in ~5 s. `jarvisctl ota` no longer refuses without a keychain
  token.

## Current blockers

1. **Long-session voice:** the counters exist; read them over a clean
   30-minute conversation soak (N6.4) and record the numbers in PLAN.md.
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
