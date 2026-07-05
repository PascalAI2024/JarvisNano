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
  BOARD_VENDOR=waveshare \
  BOARD_NAME=esp32s3_touch_amoled_1_75 \
  ./scripts/bootstrap.sh build
  ```

- [ ] Run `./scripts/smoke-build.sh`.
- [ ] Confirm generated firmware uses DIO flash mode for the CO5300/QSPI display
      path.
- [ ] Confirm no generated-source edits are required outside `bootstrap.sh`.

## 3. Flash And Boot

- [ ] Flash in preserve mode with `./scripts/flash.sh`.
- [ ] Boot-watch for 5 minutes with no reboot loop.
- [ ] Repeat once with wiped storage/NVS when intentionally validating a blank
      device:

  ```bash
  ERASE_NVS=1 STORAGE=1 ./scripts/flash.sh
  ```

- [ ] Capture serial boot through Wi-Fi, HTTP start, UI/display init, and
      `app_claw_start`.
- [ ] Confirm no panic, watchdog reset, or download-mode loop.

## 4. HTTP And Diagnostics

- [ ] Set `JARVIS_DEVICE_HOST=<device-host>` locally.
- [ ] Run:

  ```bash
  scripts/live-device.py status --host "$JARVIS_DEVICE_HOST"
  scripts/live-device.py report --host "$JARVIS_DEVICE_HOST"
  ```

- [ ] Verify these routes:
  - [ ] `/api/status`
  - [ ] `/api/config` with sensitive fields masked/read-protected
  - [ ] `/api/tools/status`
  - [ ] `/api/touch`
  - [ ] `/api/audio/level`
  - [ ] `/api/gemini/live`
  - [ ] `/api/display/face`
  - [ ] `/api/display/snapshot.json`
  - [ ] `/api/display/snapshot.ppm`
  - [ ] `/api/ui/snapshot.ppm`

## 5. Display And Touch

- [ ] Emote face renders on the physical panel.
- [ ] `/api/display/snapshot.json` reports the active owner, capture source,
      freshness, and `panel_readback:false`.
- [ ] `/api/display/snapshot.ppm` captures the emote/software mirror without
      freezing later animation.
- [ ] `/api/ui/snapshot.ppm` captures a cockpit/menu UI scene.
- [ ] Physical short tap starts listening when idle.
- [ ] Physical short tap ends input while listening.
- [ ] Long press opens the cockpit/menu without crashing voice state.
- [ ] Dismissal returns ownership to emote.

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

- [ ] Configure Gemini and JarvisMCP through NVS-backed `/api/config`; do not
      commit or bake secrets.
- [ ] Readback never exposes raw keys or the private JarvisMCP URL.
- [ ] `/api/tools/status` reports configured/unconfigured state without secrets.
- [ ] A Gemini tool call reaches JarvisMCP and returns a concise result.
- [ ] Unconfigured, timeout, and unreachable JarvisMCP paths return a
      model-visible failure result without wedging the live session.
- [ ] `claw_memory` stores local assistant facts but does not retain secrets.

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
