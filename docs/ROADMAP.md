# Roadmap

## Phase 1 — Bare board chat + listen ✅

- [x] Custom board adaptation `seeed/xiao_esp32s3_sense`
- [x] On-board PDM mic captured at 16 kHz mono PCM
- [x] I²S TX pins reserved for future MAX98357A
- [x] I²C bus reserved for future touchscreen
- [x] Wi-Fi provisioning AP fallback
- [x] mDNS at `esp-claw.local`
- [x] MCP server live on `:18791`
- [x] Build & flash flow, Docker-only
- [x] Patch upstream codegen bug

## Phase 2 — Voice replies

- [ ] Wire MAX98357A I²S amp on `D1/D2/D3` per [docs/HARDWARE.md](HARDWARE.md)
- [ ] Flip `audio_dac` from `chip: none` placeholder to real codec entry
- [ ] Test TTS playback through esp-claw's audio pipeline (Bailian / OpenAI / ElevenLabs)
- [ ] Add a wake-word path (esp-sr porcupine / on-device VAD)
- [ ] Latency target: < 800 ms from end of utterance to first audio frame back

## Phase 3 — Touchscreen

- [ ] Add 2.8" SPI ILI9341 + XPT2046 touch
- [ ] New `spi_display` peripheral entry + `display_lcd` device entry
- [ ] Crib LVGL bringup pattern from `boards/m5stack/m5stack_cores3/`
- [ ] Build a chat-bubble UI matching the IM transports
- [ ] Animated emote face via `espressif2022/esp_emote_expression` (already pulled in build)

## Phase 4 — Vision + identity

- [ ] Enable on-board OV2640 DVP camera
- [ ] Add `camera` device entry + DVP peripheral
- [ ] Expose vision tools (describe scene, OCR, find object) as MCP tools
- [ ] Optional: 3D-printed enclosure with a camera window

## Phase 5 — Personality + integrations

- [ ] Custom Lua skills for Mac control via MCP
- [ ] FlowTrack / personal CRM hooks
- [ ] Daily briefing on first interaction of the morning
- [ ] Calendar + email summarization through MCP

## Open questions

- BLE audio (LE Audio / A2DP source) for cordless speaker pairing? S3 supports BLE but not classic A2DP — would need an external module.
- Battery? The XIAO Sense has a battery solder pad and on-board LiPo charger. Worth exposing once the desk-side use is comfortable.
- Multi-device mesh? esp-claw nodes can already discover each other via MCP — fun future direction once we have more than one.
