# Gemini Live API

**What it is** — Google's low-latency bidirectional audio API, exposed as `BidiGenerateContent` over a WebSocket (WSS). The current model is `gemini-3.1-flash-live-preview`. It accepts 16 kHz PCM audio in and returns 24 kHz PCM audio out. Native audio mode — no STT/TTS pipeline needed.

**How we use it here** — `firmware/components/cap_gemini_live/cap_gemini_live.c` implements the full five-phase Gemini Live client: WSS+TLS handshake, setup frame, `setupComplete` wait, half-duplex I2S audio loop, and function-calling dispatch via the JarvisMCP bridge. Touch toggles the session on/off.

---

## Findings & gotchas

**[2026-05-21] Use `thinkingLevel`, NOT `thinkingBudget`**

Gemini 3.1 Flash Live uses `thinkingLevel` (values: `"minimal"`, `"low"`, `"medium"`, `"high"`). The previous `gemini-2.5` API used `thinkingBudget` (an integer). Setting both in the same `thinkingConfig` is a 400 error.

`"minimal"` is the default and gives lowest latency — appropriate for voice interaction.

Source: `cap_gemini_live.c:349-352` (comment and cJSON call confirming `thinkingLevel "minimal"`); [Gemini 3.1 Flash Live migration guide](https://ai.google.dev/gemini-api/docs/models/gemini-3.1-flash-live-preview).

**[2026-05-21] `setupComplete` handshake is mandatory — do not send audio before it**

After sending the `setup` frame, the client must wait for a `setupComplete` server message before sending any audio or text. Attempting to stream audio immediately results in the server dropping or rejecting the input.

`cap_gemini_live.c` handles this with an event bit (`GL_BIT_SETUP_OK`) polled via `xEventGroupWaitBits`, with a timeout after which an error is logged and the connection is closed.

Source: `cap_gemini_live.c:580, 927-944` (setup-poll drain loop and event wait).

**[2026-05-21] Tool call frame structure**

Incoming tool call:
```json
{
  "toolCall": {
    "functionCalls": [
      { "id": "...", "name": "function_name", "args": { ... } }
    ]
  }
}
```

Required response:
```json
{
  "toolResponse": {
    "functionResponses": [
      { "id": "...", "name": "function_name", "response": { ... } }
    ]
  }
}
```

Source: `cap_gemini_live.c:666-676` (handler entry and `toolCall`/`functionCalls` parse); [Live API WebSocket reference](https://ai.google.dev/api/live).

**[2026-05-21] `googleSearch` grounding and `functionDeclarations` coexist in the same `tools` array**

Both tool types can be declared together in the `setup.tools` array:
```json
{
  "tools": [
    { "googleSearch": {} },
    { "functionDeclarations": [ { "name": "...", ... } ] }
  ]
}
```

This is confirmed active in `cap_gemini_live.c:320-344` where both `googleSearch` and a `functionDeclarations` object are added to the same `tools` array. The model handles both without conflict.

Source: `cap_gemini_live.c:316-344`.

**[2026-05-21] `responseModalities: ["AUDIO"]` is required for native audio output**

Without this in `generationConfig`, the model defaults to text output. The `cap_gemini_live.c` setup always includes it.

Source: `cap_gemini_live.c:347-348`.

**[2026-05-21] No output volume API — normalize in-app**

Gemini Live has no API parameter to control the output audio level. The model returns PCM at whatever level it produces. Volume control must be done in application code (see [audio-es8311-es7210.md](./audio-es8311-es7210.md) for the soft-knee limiter approach).

**[2026-05-21] `speechConfig` / `voiceConfig` / `voiceName` — API capability, not currently configured**

The Gemini Live API supports `speechConfig.voiceConfig.prebuiltVoiceConfig.voiceName` (e.g. `"Kore"`) in the setup frame to select a specific voice. This is documented in the official API reference.

**This is not currently configured in `cap_gemini_live.c::gl_send_setup`.** The firmware does not send a `speechConfig` block. The model uses its default voice. If voice selection is needed, it must be added to `gl_send_setup`.

Source: [Live API reference](https://ai.google.dev/api/live) (API capability); `cap_gemini_live.c:310-365` (`gl_send_setup` does not include `speechConfig` — verified by inspection).

**[2026-05-21] Flash with `--flash-mode dio`**

When flashing firmware that includes `cap_gemini_live`, use `--flash-mode dio`. The QSPI display driver (`quad_mode: true`) and the flash mode interact; `qio` mode has caused boot failures with this firmware image.

Source: memory file `project_gemini_live_plan.md`.

**[2026-05-21] Model string: `gemini-3.1-flash-live-preview`**

Previous model string was `gemini-2.5-flash-native-audio-preview-12-2025`. This has been superseded. The new string is defined in `cap_gemini_live.c` as `GEMINI_LIVE_MODEL`. Do not hardcode the old string.

Source: [Gemini 3.1 Flash Live model page](https://ai.google.dev/gemini-api/docs/models/gemini-3.1-flash-live-preview); `cap_gemini_live.c:59` (comment confirming migration).

---

## Primary sources

| Source | Notes |
|--------|-------|
| `firmware/components/cap_gemini_live/src/cap_gemini_live.c` | Full implementation: setup frame, tool-call handler, audio loop, JarvisMCP bridge call. |
| [Gemini Live API reference](https://ai.google.dev/api/live) | `BidiGenerateContentSetup`, `BidiGenerateContentServerMessage`, tool call frames. |
| [Gemini 3.1 Flash Live model page](https://ai.google.dev/gemini-api/docs/models/gemini-3.1-flash-live-preview) | Model ID, token limits, migration notes from 2.5. |
| [Gemini Live capabilities guide](https://ai.google.dev/gemini-api/docs/live-api/capabilities) | `thinkingLevel` values, `responseModalities`, native audio config. |
| [`visorbarnis/voice-assistant`](https://github.com/visorbarnis/voice-assistant) | Only found ESP-IDF C project with Gemini Live + `voice_client.c`, `audio_buffer.c`, `aec_processor.c`. Best crib target. |

---

## Open questions

- Is there a way to request the model to signal turn end explicitly so the device knows when to switch from playback to listen mode without VAD?
- What are the rate limits on `gemini-3.1-flash-live-preview` sessions (max concurrent sessions, max session duration)?
- Does `googleSearch` grounding incur additional latency compared to a pure audio response?

---

## See also

- [jarvismcp-bridge.md](./jarvismcp-bridge.md) — function calling dispatch and the `/act` gateway pattern.
- [audio-es8311-es7210.md](./audio-es8311-es7210.md) — 16 kHz in / 24 kHz out codec configuration.
- [build-toolchain.md](./build-toolchain.md) — `--flash-mode dio` requirement.
- [llm-config.md](./llm-config.md) — other LLM protocol options when Gemini Live is not active.
