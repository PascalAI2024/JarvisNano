# Release Checklist

Use this before tagging or pushing a public JarvisNano v1 release. The primary
target is the Waveshare ESP32-S3-Touch-AMOLED-1.75C.

## 1. Source Hygiene

- [ ] Start from a clean checkout on the release branch.
- [ ] Confirm no unrelated worktree changes are included.
- [ ] Run `git diff --check`.
- [ ] Run `./scripts/check-secrets.sh`.
- [ ] Review image pixels and run a full-history credential scan; the working-tree
      scanner covers tracked/untracked text and printable binary metadata only.
- [ ] Confirm the diff contains no API keys, bearer tokens, Wi-Fi credentials,
      LAN addresses, MAC addresses, internal URLs, absolute developer paths, or
      device-specific logs.
- [ ] Generate the exact dependency/license inventory after the final build and
      attach all required third-party notice texts to the release.

## 2. Build

- [ ] Run a clean Waveshare build:

  ```bash
  ./scripts/build-v5.sh
  ```

- [ ] Run `./scripts/smoke-build.sh`; confirm the manifest and all six flash
      artifacts pass the 1.75C geometry check.
- [ ] Confirm `build/jarvisrobot_v5.bin` exists and the flash args are DIO.
- [ ] Confirm no generated-source edits under `build/` or `managed_components/`.
- [ ] Run every portable suite while N6.6 still tracks consolidation:

  ```bash
  cmake -S host -B build-host
  cmake --build build-host
  ctest --test-dir build-host --output-on-failure
  cmake -S components/jr_tools/host -B build-tools-host
  cmake --build build-tools-host
  ctest --test-dir build-tools-host --output-on-failure
  python3 -m unittest scripts/test_jarvis_desk.py
  node scripts/check-dashboard-js.mjs main/diagnostics.html
  bash -n scripts/*.sh
  ```

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
  - [ ] `/api/tools/config` (redacted; existing pairing token + control header)
  - [ ] `/api/audio/taps`

## 5. Display And Touch

- [ ] Emote face renders on the physical panel.
- [ ] `/api/display/snapshot.json` reports the active owner, capture source,
      freshness, and `panel_readback:false`.
- [ ] `/api/display/snapshot.ppm` captures the emote/software mirror without
      freezing later animation.
- [ ] Overlay HUD, choices, captions, Watch, and controls match the submitted
      software mirror and the physical round panel.
- [ ] Colour bars and grid appear on the physical panel, match the software
      mirror, and restore to the normal renderer; a mirror alone does not close
      this check.
- [ ] Start `/api/diag/panel-touch?action=start`, complete all three randomized
      physical sectors, and confirm the challenge reports physical success.
- [ ] PWR short listens without muting; PWR long shows battery; BOOT short
      toggles controls.
- [ ] Global left/right edge level gestures and horizontal navigation work.
- [ ] Glass hold privacy, flip-to-mute/face-up recovery, and shake-to-cancel
      work; reorientation does not clear a hold/controls mute.

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
- [ ] Audio diagnostics remain bounded and responsive while Gemini owns the mic.

## 7. Memory And JarvisMCP

- [ ] Configure Gemini and JarvisMCP through NVS or an already-paired
      `/api/tools/config` client; do not commit or bake secrets.
- [ ] Readback never exposes raw keys or the private JarvisMCP URL.
- [ ] `/api/tools/config` GET is redacted.
- [ ] A Gemini tool call reaches JarvisMCP and returns a concise result.
- [ ] Unconfigured, timeout, and unreachable JarvisMCP paths return a
      model-visible failure result without wedging the live session.
- [ ] On-device memory stores local assistant facts but does not retain secrets.

## 8. Security

- [ ] A token-protected write without `X-JarvisNano-Token` returns stable JSON
      with 401; a wrong token returns 403.
- [ ] A control-only diagnostic without `X-JarvisNano-Control: 1` returns 403.
- [ ] A route requiring both proofs rejects either missing header and succeeds
      only with the existing pairing token plus control header.
- [ ] Protected routes include voice control, paired input injection,
      JarvisMCP config, Brain/operator surfaces, device levels, and OTA.
- [ ] Agent Link POST rejects a missing/wrong token and succeeds with the token
      alone, matching its intentionally token-only auth row.
- [ ] On a blank device, a 1.5–5 second runtime BOOT hold visibly opens the
      60-second claim window; one `jarvis-desk.py pair` succeeds, stores a
      host-bound Keychain token, and a second claim fails closed.
- [ ] Unsigned and wrong-key images are rejected before boot selection; one
      correctly signed image passes.
- [ ] Release upload uses authenticated encrypted transport; a reusable bearer
      never crosses plaintext HTTP.
- [ ] Attended NVS/flash encryption is enabled and a physical flash read cannot
      recover Wi-Fi, Gemini, or JarvisMCP credentials.
- [ ] Secure Boot, anti-rollback, flash encryption, and recovery procedures are
      verified separately from ordinary OTA.

## 9. Trusted-LAN Cockpit

- [ ] `/` loads from a fresh browser with no cached application state.
- [ ] Root HTML plus coarse `/api/display`, `/api/touch`, and `/api/sensors`
      counters work without a token.
- [ ] Cockpit/session detail, Agent Link, logs, audio taps/WAV, VAD log, and
      display pixel routes reject missing/wrong tokens and succeed when paired.
- [ ] Protected controls accept a manually supplied existing token; the page
      does not display or persist it.
- [ ] Display mirror and physical-panel challenge preserve their evidence
      caveats.
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
