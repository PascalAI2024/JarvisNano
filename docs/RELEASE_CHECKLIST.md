# Release Checklist

Use this before tagging a public JarvisNano release.

## Firmware

- [ ] Start from a clean checkout.
- [ ] Run `./scripts/bootstrap.sh`.
- [ ] Run `./scripts/bootstrap.sh build`.
- [ ] Run `./scripts/smoke-build.sh`.
- [ ] Flash with `./scripts/flash.sh` and confirm storage should be preserved.
- [ ] Capture a fresh boot log through `startup/boot_completed`,
      `Native status LED on gpio`, and `Returned from app_main()`.
- [ ] Verify HTTP endpoints on the intended network path:
      `/api/status`, `/api/config`, `/api/battery`, `/api/audio/level`,
      `/api/wifi/scan`.

## Dashboard

- [ ] Serve `dashboard/` locally and reload `index.html`.
- [ ] Check the browser console for errors.
- [ ] Verify the BLE button is guarded in embedded/in-app browsers.
- [ ] Verify onboarding Wi-Fi scan, save, AP-to-STA handoff, LLM config, and
      Done flow on a freshly provisioned device.
- [ ] If publishing a browser-flash release, update
      `dashboard/firmware/jarvis-xiao-esp32s3-sense.bin`.
- [ ] Verify `dashboard/firmware/manifest.json` points to the intended binary.

## Android

- [ ] Run `gradle :app:assembleDebug`.
- [ ] Run `gradle :app:testDebugUnitTest`.
- [ ] Run `gradle :app:compileDebugAndroidTestKotlin`.
- [ ] Install on a physical phone.
- [ ] Grant Bluetooth permissions.
- [ ] Verify mDNS/manual host, HTTP status/config, chat, camera failure state,
      BLE scan, and missing-service diagnostics.

## Docs

- [ ] Update `CHANGELOG.md`.
- [ ] Update `docs/ROADMAP.md` and `docs/PHASE2_TASKS.md`.
- [ ] Confirm no private paths, credentials, serial logs, or API keys are in the
      diff.
- [ ] Confirm `README.md` current status matches the shipped firmware.
