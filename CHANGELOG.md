# Changelog

All notable user-facing changes should be recorded here.

## Unreleased

- **Second supported board: Waveshare ESP32-S3-Touch-AMOLED-1.75.** Adds a
  full board adaptation at `boards/waveshare/esp32s3_touch_amoled_1_75/` —
  1.75" CO5300 QSPI AMOLED (466×466), CST9217 capacitive touch, ES8311 DAC,
  ES7210 4-channel ADC with on-chip AEC, TCA9554 IO expander, AXP2101 PMIC,
  16 MB flash, 8 MB octal PSRAM. End-to-end chat round-trip verified on
  hardware (MiniMax M2.7 via Anthropic-compatible endpoint, ~5.5 s).
- **Concept-5 mascot-bust enclosure** for the AMOLED-1.75 board, under
  `hardware/enclosure/amoled-1_75/concept-5-mascot-bust/` — parametric
  OpenSCAD model, dimensions derived from Waveshare's authoritative 3D
  drawing.
- **`bootstrap.sh` reproducibility:** pinned the IDF Docker tag to
  `espressif/idf:v5.5.4` (was the rolling `release-v5.5`) and pinned
  `esp-bmgr-assist==0.5.0`. Newer minor versions of `esp-bmgr-assist`
  dropped customer-board discovery (`boards/<vendor>/<name>/`) and
  changed CLI surface; pinning prevents the next person to bootstrap
  from hitting the same wall.
- Pinned the generated ESP-Claw checkout to a fixed commit in
  `scripts/bootstrap.sh`.
- Added `scripts/smoke-build.sh` for post-build firmware sanity checks.
- Added native GPIO21 status LED patch documentation.
- Hardened dashboard BLE behavior in embedded/in-app browsers.
- Added dashboard camera blocked/pending handling and battery endpoint state
  distinction.
- Added onboarding AP-to-STA Wi-Fi handoff handling.
- Added Android test source-set scaffolds and acceptance checklist.
- Added open-source contribution, security, support, issue, and PR templates.

## 0.1.0

- Initial public JarvisNano board adaptation, dashboard, Android companion
  scaffold, documentation, firmware patches, and enclosure concepts.
