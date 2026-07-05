# Roadmap

JarvisNano is Waveshare-first for v1. The XIAO/camera/Android/BLE tracks remain
important, but they no longer block the USB desktop assistant release.

## v1 Target

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.75.
- Form factor: USB-powered desktop assistant.
- UX: voice + expressive round face + touch cockpit.
- Client: device firmware + browser dashboard first.
- Security: pairing token for writes/control; secrets only in NVS.
- Tools: JarvisMCP bridge through Gemini Live tool calls.
- Memory: on-device `claw_memory` without storing secrets.

## Phase 0 - Stabilize And Merge `gpt`

- [x] Merge the `gpt` display/runtime recovery work onto `main`.
- [x] Keep the direct `jarvis_board` CO5300 primitive for v1.
- [x] Keep full Waveshare BSP/LVGL migration out of the critical path.
- [x] Preserve existing NVS/storage by default during flash.
- [x] Document the display snapshot contract and software-mirror limitation.
- [ ] Re-run clean build, flash, 5-minute boot watch, display snapshot, and
      secret scan before tagging a release candidate.

## Phase 1 - Touch And Display Runtime

- [x] Expose `/api/touch` diagnostics.
- [x] Preserve `/api/display/face`, `/api/display/snapshot.*`, and `/api/ui/*`.
- [ ] Replace demo-dependent touch behavior with a dedicated CST9217 touch
      service.
- [ ] Short tap starts listening when idle and ends input while listening.
- [ ] Long press opens a cockpit/menu scene.
- [ ] Swipe/down or explicit control sleeps/dismisses UI.
- [ ] Emote resumes after UI dismissal.
- [ ] Add diagnostic-only touch injection behind the pairing token.

## Phase 2 - Voice And Face v1

- [ ] Make Gemini Live the primary interaction loop:
      idle -> listening -> thinking -> speaking -> listening/idle.
- [ ] Bind face states to real session state, not timers.
- [ ] Prove ES7210 mic frames on Waveshare during a physical voice session.
- [ ] Prove ES8311 speaker playback by audible output or loopback/tone route.
- [ ] Add tap or voice-activity interrupt during speaking.
- [ ] Ensure `/api/audio/level` pauses cleanly while Gemini owns the mic.
- [ ] Keep display flush timeouts from wedging the face.

## Phase 3 - Memory And JarvisMCP Tools

- [x] Add NVS-backed `jarvis_mcp_url`, `jarvis_mcp_key`, and pairing-token
      config fields.
- [x] Add `/api/tools/status` without exposing secrets.
- [ ] Configure Gemini and JarvisMCP only through NVS-backed setup.
- [ ] Return model-visible tool errors for unconfigured, timeout, and
      unreachable JarvisMCP states.
- [ ] Confirm successful Gemini tool call reaches JarvisMCP and returns a
      concise result.
- [ ] Keep `claw_memory` enabled while preventing secret extraction.

## Phase 4 - Secure Browser Setup

- [ ] Require `X-JarvisNano-Token` for writes/control in public builds.
- [ ] Generate or set the token on device storage, never source.
- [ ] Protect config writes, restart/control, Gemini control, touch injection,
      JarvisMCP config, and destructive file actions.
- [ ] Make the dashboard Waveshare-first:
      face preview, touch status, Gemini state, memory/tool status, masked
      config, and setup verification.
- [ ] Hide or mark camera/battery/Android/BLE as unavailable for USB desktop v1.
- [ ] Run desktop and mobile-width visual QA.

## Phase 5 - Release Candidate

- [ ] Clean checkout build.
- [ ] Preserve-config flash test.
- [ ] Wiped-storage flash test.
- [ ] 5-minute serial boot watch.
- [ ] HTTP matrix.
- [ ] Display screenshots for emote and UI.
- [ ] Physical touch tests.
- [ ] Gemini text and voice tests.
- [ ] JarvisMCP success, timeout, and unconfigured tests.
- [ ] Dashboard setup test from blank device.
- [ ] Public secret/identifier scan.
- [ ] Docs review for Waveshare build, first boot, pairing token,
      Gemini/JarvisMCP config, and troubleshooting.

## Post-v1

### Phase 6 - Better UI / Optional BSP-LVGL Migration

- Keep direct CO5300 path unless LVGL gives a clear measurable win.
- If migrating, start with a standalone BSP probe branch.
- Port cockpit scenes only after emote, voice, display, and touch acceptance
  remain green.

### Phase 7 - Android And BLE Privacy Mode

- Implement BLE state/control first, then audio.
- Make Android first-class after firmware service stability.
- Privacy mode with phone-side local model is post-v1.

### Phase 8 - Battery, Enclosure, Hardware Polish

- Add battery/PMIC reporting after USB desktop v1 ships.
- Fit-test enclosure, thermals, current draw, charging, and speaker loudness.
- Add low-battery UI only after readings are real.

### Phase 9 - Camera And XIAO Parity

- Keep camera deferred for Waveshare v1.
- Resume XIAO camera work separately with the `esp32-camera` path.
- Do not let XIAO parity block the Waveshare desktop release.
