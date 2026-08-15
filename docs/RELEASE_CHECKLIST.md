# Release Checklist

Use this before tagging or pushing a public JarvisNano v1 release. The primary
target is the Waveshare ESP32-S3-Touch-AMOLED-1.75.

## 1. Source Hygiene

- [ ] Start from a clean checkout on the release branch.
- [ ] Confirm no unrelated worktree changes are included.
- [ ] Run `git diff --check`.
- [ ] Run `./scripts/check-secrets.sh`.
- [ ] Confirm the diff contains no API keys, bearer tokens, Wi-Fi credentials,
      LAN addresses, MAC addresses, internal URLs, absolute developer paths, or
      device-specific logs.

## 2. Build

- [ ] Run a clean Waveshare build:

  ```bash
  ./scripts/build-v5.sh
  ```

- [ ] Confirm `build/jarvisrobot_v5.bin` exists and the flash args are DIO.
- [ ] Confirm no generated-source edits under `build/` or `managed_components/`.

## 3. Flash And Boot

- [ ] Flash in preserve mode with `./scripts/flash-v5.sh`.
- [ ] Boot-watch for 5 minutes with no reboot loop.
- [ ] Repeat once with wiped storage/NVS when intentionally validating a blank
      device:

  ```bash
  ERASE_NVS=1 ./scripts/flash-v5.sh
  ```

- [ ] Capture serial boot through Wi-Fi, HTTP start, display init, and
      Gemini listen. There is no `app_claw_start` on v5.
- [ ] Confirm no panic, watchdog reset, or download-mode loop.

## 4. HTTP And Diagnostics

- [ ] Set `JARVIS_DEVICE_HOST=<device-host>` locally.
- [ ] Run:

  ```bash
  scripts/live-device.py status --host "$JARVIS_DEVICE_HOST"
  scripts/live-device.py report --host "$JARVIS_DEVICE_HOST"
  ```

- [ ] Verify these routes (see `docs/PROTOCOL.md`):
  - [ ] `/api/cockpit`
  - [ ] `/api/gemini/live`
  - [ ] `/api/touch`
  - [ ] `/api/display` and `/api/display/snapshot.json`
  - [ ] `/api/display/snapshot.ppm`
  - [ ] `/api/tools/config` (redacted; pairing token as required)
  - [ ] `/api/audio/taps`

## 5. Display And Touch

- [ ] Emote face renders on the physical panel.
- [ ] `/api/display/snapshot.json` reports the active owner, capture source,
      freshness, and `panel_readback:false`.
- [ ] `/api/display/snapshot.ppm` captures the emote/software mirror without
      freezing later animation.
- [ ] Overlay HUD (thinking comet, choice arcs, captions) is in the
      snapshot when those states are active.
- [ ] Physical short tap starts or interrupts listening.
- [ ] Flip-to-mute and shake-to-cancel still work after the flash.

## 6. Voice

- [ ] Gemini text cycle succeeds:

  ```bash
  scripts/live-device.py gemini-cycle --host "$JARVIS_DEVICE_HOST" --text "Say one short sentence." --report
  ```

- [ ] Physical voice session sends ES7210 codec frames.
- [ ] Gemini returns `audio_parts > 0`.
- [ ] ES8311 playback is proven by audible output or a documented loopback/tone
      route.
- [ ] Face state follows session state within 250 ms.
- [ ] Tap or voice activity during speaking cancels playback and returns to
      listening.
- [ ] `/api/audio/level` pauses cleanly while Gemini owns the mic.

## 7. Memory And JarvisMCP

- [ ] Configure Gemini and JarvisMCP through NVS / pairing-gated
      `/api/tools/config`; do not commit or bake secrets.
- [ ] Readback never exposes raw keys or the private JarvisMCP URL.
- [ ] `/api/tools/config` GET is redacted.
- [ ] A Gemini tool call reaches JarvisMCP and returns a concise result.
- [ ] Unconfigured, timeout, and unreachable JarvisMCP paths return a
      model-visible failure result without wedging the live session.
- [ ] On-device memory stores local assistant facts but does not retain secrets.

## 8. Security

- [ ] Protected write without `X-JarvisNano-Token` returns stable JSON with
      401/403.
- [ ] Protected write with the pairing token succeeds.
- [ ] Protected routes include config writes, restart/control, Gemini control,
      touch injection, JarvisMCP config, and destructive file actions.
- [ ] Token is generated or set on device storage, never committed.

## 9. Dashboard

- [ ] Dashboard can complete setup from a blank device.
- [ ] Dashboard stores the pairing token only in browser-local storage.
- [ ] Dashboard never displays raw secrets after save.
- [ ] Waveshare tiles are first-class: live face preview, touch, Gemini,
      memory/tool status.
- [ ] Camera, battery, Android, and BLE are hidden or clearly unavailable for
      USB desktop v1.
- [ ] Desktop and mobile-width screenshots pass visual QA.

## 10. Docs

- [ ] `README.md` matches the shipped firmware.
- [ ] `docs/BUILD.md` covers Waveshare build/flash first.
- [ ] `docs/ARCHITECTURE.md` diagrams match the v1 architecture.
- [ ] `docs/ROADMAP.md` keeps post-v1 tracks separate from release blockers.
- [ ] `docs/NEXT_SESSION.md` contains no device-specific identifiers.
- [ ] Troubleshooting covers boot loop, white dot/old face confusion, display
      owner state, and snapshot limitations.

Do not publish firmware binaries that contain user config, NVS, keys, SSIDs,
LAN addresses, MAC addresses, or device-specific logs.
