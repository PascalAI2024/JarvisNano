# Gemini Live Voice — Build Plan (AMOLED-1.75)

> Status: **planned, not started.** This doc exists so phase 1 starts clean in a fresh session.
> Target board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (see `boards/waveshare/esp32s3_touch_amoled_1_75/README.md`).

## Goal

A **toggle-on/off voice conversation** on the AMOLED board: tap to open a Gemini Live session (it listens + talks back), tap again to close it (session ends → billing stops, mic stops). On-screen state shows listening / thinking / speaking.

## Why toggle (not always-on, not per-turn push-to-talk)

- **Cost:** Gemini Live is metered per session-minute. A toggle keeps the WSS session open only while the user wants to converse, instead of all day.
- **Duplex constraint:** within a toggled-on session, use **half-duplex turn-taking** (listen → detect end-of-speech → play reply → listen again). This sidesteps the board's hardware limit (below) without resampling. True full-duplex barge-in is a later enhancement if wanted.

## The hardware constraint (critical — drove the design)

This board's audio is **I2S0 in full-duplex with a SHARED clock generator** (ESP32-S3 = `SOC_I2S_HW_VERSION_1`). Consequences, both confirmed on hardware this session:

1. **Mic (RX) only captures while the speaker (TX) channel is also enabled.** Mic-alone reads fail `i2s_channel_read: The channel is not enabled`. Verified: `audio.loopback()` (opens TX then RX) captures fine; mic-only does not.
2. **RX and TX must run at the same sample rate** (shared clock). Gemini Live wants **16 kHz in / 24 kHz out** — they cannot run simultaneously at different rates.

**Chosen strategy:** half-duplex turn-taking with I2S reconfigure between phases:
- **Listen phase:** I2S @ 16 kHz, capture ES7210 mic, stream PCM up. (TX must be co-enabled even if silent — the shared-clock rule.)
- **Speak phase:** tear down + reopen I2S @ 24 kHz, play Gemini's audio through ES8311.
- Switch cost ~few hundred ms, invisible against turn latency.

Alternative (deferred): resample 24→16 kHz (sub-5% CPU on S3) for true full-duplex. Only needed if barge-in becomes a requirement.

## Protocol (Gemini Live API, WebSocket)

- **Endpoint:** `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=<API_KEY>`
- **Auth:** API key as `?key=` query param (or `x-goog-api-key` header).
- **Setup message** (first frame after connect):
  ```json
  {"setup": {
     "model": "models/<live-model>",
     "generationConfig": {"responseModalities": ["AUDIO"]},
     "systemInstruction": {"parts": [{"text": "<J.A.R.V.I.S. persona>"}]}
  }}
  ```
  Server replies `{"setupComplete": {}}` when ready.
- **Send audio (realtime input):**
  ```json
  {"realtimeInput": {"audio": {"mimeType": "audio/pcm;rate=16000", "data": "<base64 PCM16 mono>"}}}
  ```
  (Older API revisions use `mediaChunks`; verify the current field name at build time.)
- **Receive audio:** `serverContent.modelTurn.parts[].inlineData` → `{ "mimeType": "audio/pcm;rate=24000", "data": "<base64 PCM16>" }`.
- **Turn signals:** `serverContent.turnComplete: true` (model done speaking), `serverContent.interrupted: true` (barge-in).
- **Target model:** user specified `gemini-3.1-flash-live-preview` — **verify the exact current Live model id at build time** (model names change; check ai.google.dev/gemini-api/docs/models).

## Architecture on the ESP32-S3

- **WebSocket:** `espressif__esp_websocket_client` (already in the managed-components tree) over TLS. Needs the Google CA cert + ~30-40 KB heap for the TLS session.
- **Audio in:** ES7210 ADC @ 16 kHz (TX co-enabled per the duplex rule). Reuse the `audio.*` Lua module patterns OR a native task.
- **Audio out:** ES8311 DAC @ 24 kHz.
- **Encoding:** base64 of raw PCM frames, wrapped in JSON. ~110 KB/s sustained both directions combined.
- **Toggle + on-screen state:** tap (CST9217) toggles the session; reuse the emote status overlay (`emote_set_status_detail`, already working — "Ready * <model>") for Listening / Thinking / Speaking, or a full text view via the `display` module.
- **Key storage:** NVS via `POST /api/config` (add a `gemini_api_key` field or reuse `llm_api_key`). **Never** commit the key — not even in patch docs.

## Phased plan (each phase = a clean commit / stopping point)

| Phase | Deliverable | Proves |
|---|---|---|
| **1** | WSS + TLS handshake to the endpoint, send `setup`, parse `setupComplete`. Expose as CLI `gemini-live test`. No audio. | Connectivity + auth + TLS heap budget |
| **2** | One text turn: `clientContent` text in → `modelTurn` text out, printed to console. | Round-trip + message parsing |
| **3** | Receive audio: a text prompt → model audio reply → decode base64 → ES8311 @ 24 kHz. | Outbound audio path (easier than capture) |
| **4** | Send audio on tap: ES7210 @ 16 kHz capture → `realtimeInput` frames → wait for reply → play. Half-duplex turn-taking with I2S reconfigure. | Full push-to-talk voice loop |
| **5** | Toggle UX + on-screen state: tap toggles session; screen shows Listening/Thinking/Speaking. | The product |

## Risks / open questions

- **Multi-session scope.** Phase 1 alone ~half a day (TLS-to-Google + esp_websocket_client wiring) before any audio.
- **Sustained throughput:** ~110 KB/s over WSS+TLS + base64/JSON overhead. Memory-budget check before phase 4 (PSRAM has headroom but TLS + audio buffers + JSON aren't free).
- **Cost:** metered per session-minute — the toggle is the cost control.
- **Vendored code:** changes to esp-claw need `patches/` files + a `bootstrap.sh` mutation (see `patches/0009` + `apply_emote_status_detail_patch` for the pattern). Pin `esp-bmgr-assist==0.5.0` and IDF `v5.5.4`.
- **API key:** user pasted one in chat — rotate it. Store in NVS, never in the repo.
- **I2S reconfigure robustness:** tearing down/reopening I2S between listen/speak phases must be clean (no channel-leak, no "channel not enabled"). The duplex enable rule (TX co-enabled for RX) applies in the listen phase.

## Confirmed-working foundation (from this session)

- Display (CO5300), touch (CST9217), Wi-Fi (STA), agent text round-trip (MiniMax via Anthropic-compat), emote status overlay — all working.
- Speaker (ES8311) + mic (ES7210) hardware confirmed via tone + loopback.
- Build: pinned `espressif/idf:v5.5.4` + `esp-bmgr-assist==0.5.0`; flash via esptool 6-partition layout.
