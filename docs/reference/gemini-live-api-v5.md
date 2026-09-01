# Gemini Live API — v5 Transport Verification (2026-07-07)

**Status** — current v5 protocol reference. External API findings were
source-verified on 2026-07-07 and reconciled with the live firmware on
2026-08-28. The older [`gemini-live-api.md`](./gemini-live-api.md) is retained
only as a v4 history record.

**How we use it here** — `components/jr_transport` owns
`BidiGenerateContent` framing, setup, server VAD events, audio parsing, tool
calls, and bounded session state. `main/main.c` supplies policy and
`components/jr_audio` supplies a 16 kHz AEC-clean uplink; native downlink is
24 kHz. The primary model constant is
`models/gemini-3.1-flash-live-preview`, with
`models/gemini-2.5-flash-native-audio-preview-12-2025` as fallback.

Model availability is provider-controlled. Recheck the official catalog before
a release rather than treating this dated research pass as an availability
guarantee.

---

## VERDICT — the three things that matter most

1. **Model ID: retire `models/gemini-2.5-flash-native-audio-latest`.** It does
   not appear in the current model catalog, deprecations table, or any
   fetch-verified page — treat it as unconfirmed/unverifiable, not "known
   good." Also retire `gemini-2.5-flash-native-audio-preview-09-2025`
   (confirmed 404). **Use `models/gemini-3.1-flash-live-preview`** as the
   primary v5 model (current flagship Live/native-audio preview, release date
   March 11 2026 per Google's own deprecations table, used in the current
   official get-started code samples), with **`models/gemini-2.5-flash-native-audio-preview-12-2025`**
   as a fallback/alternate. See the open discrepancy flagged below re: this
   repo's earlier 404 on the 3.1 string.
2. **`thinkingLevel` moved (or was always) nested**: it lives at
   `generationConfig.thinkingConfig.thinkingLevel`, not flat under
   `generationConfig` and not top-level in `setup`. v4's setup code should be
   checked against this — if it's setting a flat field, the server is likely
   silently ignoring it.
3. **Session resilience has three hard edges v5 must handle**: the raw
   WebSocket connection caps at **~10 minutes regardless of session type**
   (separate from the 15-min audio-only / 2-min audio+video session caps);
   `goAway.timeLeft` is a **protobuf `Duration` type, not a raw number of
   seconds**; and a `sessionResumptionUpdate` handle is only usable when
   `resumable: true` — a handle captured mid-generation or mid-function-call
   comes back empty and unusable. Also: **never send `audioStreamEnd` in
   manual/push-to-talk mode** — it is documented as valid only under
   automatic (server-side) VAD, which explains v4's prior PTT bug where
   `audioStreamEnd` left the session stuck in `THINKING`.

Endpoint and core setup-field shapes are otherwise **unchanged** from the v4
baseline — this is a narrower delta than "gemini-3.x 404" made it sound.

---

## 1. Endpoint + auth (fetch-verified 2026-07-07)

- **Endpoint unchanged**: `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key={API_KEY}`.
  `v1beta` is still correct for the standard API-key flow — not superseded.
  [Get started with Live API (WebSockets)](https://ai.google.dev/gemini-api/docs/live-api/get-started-websocket) ·
  [Live API reference](https://ai.google.dev/api/live) — HIGH confidence, fetch-verified.
- **`?key=API_KEY` remains valid and is shown as a first-class auth method** in current
  official code samples — not deprecated. No OAuth-mandatory language found. MEDIUM
  confidence on permanence (absence of a deprecation notice isn't proof), fetch-verified.
- **Ephemeral tokens exist as an optional, RECOMMENDED-not-required hardening path**
  for client-side/device connections — a *different* endpoint (`v1alpha` +
  `BidiGenerateContentConstrained`), auth via `access_token` query param or
  `Authorization: Token <token>` header. Requires a backend `AuthTokenService.CreateToken`
  minting call — a device can't self-mint, so this is an architecture change, not a
  drop-in swap. Worth considering for v5 if hardening device-side key exposure matters,
  but not required for baseline functionality.
  [Ephemeral tokens](https://ai.google.dev/gemini-api/docs/live-api/ephemeral-tokens) — HIGH, fetch-verified.

## 2. Model IDs (fetch-verified 2026-07-07)

| Model string | Status | Notes |
|---|---|---|
| `gemini-3.1-flash-live-preview` | **Current flagship, Preview** | Release date March 11 2026 per official deprecations table; used in current official get-started-websocket sample; audio-to-audio, barge-in support. **RECOMMENDED for v5.** |
| `gemini-2.5-flash-native-audio-preview-12-2025` | Valid, Preview | Released Dec 12 2025, no shutdown date announced. Successor to the `-09-2025` string. **Recommended fallback.** A Google AI Developers Forum thread (WebSearch snippet only, not independently fetched — LOW confidence, single source) reports this model returning WS close code `1011` mid-turn at ~80% rate starting 2026-05-27 — worth a smoke test before committing to it as primary fallback. |
| `gemini-2.5-flash-native-audio-preview-09-2025` | **Confirmed dead — 404** | Direct fetch of its model page returns HTTP 404; absent from the deprecations table (likely pruned post-shutdown). |
| `gemini-2.5-flash-native-audio-latest` | **Unconfirmed / unverifiable** | This is the repo's CURRENT `GEMINI_LIVE_MODEL` constant (`cap_gemini_live.c:117`). Not present in the models catalog, deprecations table, or any direct hit. Google does use `-latest` aliases elsewhere but nothing ties this specific alias to a live model right now. Do not treat as safe. |
| `gemini-2.0-flash-live-001` | Shutdown 2025-12-09 | Confirmed, matches prior baseline. |
| `gemini-live-2.5-flash-preview` | Shutdown 2025-12-09 | Released June 17 2025 — a different string from anything in the v4 baseline docs. |
| `gemini-live-2.5-flash-preview-native-audio-09-2025` / `gemini-live-2.5-flash-native-audio` | **Do not use** | Referenced only in a low-confidence WebSearch snippet claiming a March 19 2026 deprecation/migration; neither string is present in the official deprecations table on direct fetch, and the `gemini-live-2.5-flash-native-audio` model page 404s. Likely stale/paraphrased third-party content. |

**Open discrepancy — flagged, not resolved**: the v4 baseline's 2026-05-23 pass
got a live 404 testing `gemini-3.1-flash-live-preview`, yet the model's own
official page states a release date of **March 11, 2026** — over two months
before that 404 was observed. Possible explanations: a staged/allowlisted
rollout ahead of full public doc publication, or the exact string tested
differed subtly from the current canonical one. State this as unresolved; if
v5 hits a 404 on this string, that history is why — retest carefully rather
than assuming the string is simply wrong.

No `gemini-3.0` or other `3.x` live/native-audio variant besides `3.1-flash-live-preview`
was found in the catalog (MEDIUM confidence — negative result).

## 3. Setup message schema (`BidiGenerateContentSetup`, fetch-verified 2026-07-07)

Full current field list (confirmed via two independent fetch channels):
`model`, `generationConfig`, `systemInstruction`, `tools[]`, `realtimeInputConfig`,
`sessionResumption`, `contextWindowCompression`, `inputAudioTranscription`,
`outputAudioTranscription`, **`proactivity`** (new), **`historyConfig`** (new).

No fields were found renamed or removed relative to the v4 baseline's known set.

- **`responseModalities`** — unchanged: `generationConfig.responseModalities: [string]`,
  e.g. `["AUDIO"]`. Confirmed still restricted to exactly one modality per session
  (`AUDIO` or `TEXT`, never both); native-audio models are audio-only. No new modality
  option. HIGH confidence (shape), MEDIUM (no-new-modality claim — cross-referenced via
  WebSearch, not a single Google-primary quote).

- **`realtimeInputConfig.automaticActivityDetection`** — field names unchanged, verified
  verbatim against the official field table:
  ```json
  "realtimeInputConfig": {
    "automaticActivityDetection": {
      "disabled": false,
      "startOfSpeechSensitivity": "START_SENSITIVITY_LOW",
      "prefixPaddingMs": 20,
      "endOfSpeechSensitivity": "END_SENSITIVITY_LOW",
      "silenceDurationMs": 100
    },
    "activityHandling": "START_OF_ACTIVITY_INTERRUPTS",
    "turnCoverage": "TURN_INCLUDES_ONLY_ACTIVITY"
  }
  ```
  `StartSensitivity` (`START_SENSITIVITY_UNSPECIFIED` defaults HIGH / `_HIGH` / `_LOW`),
  `EndSensitivity` (same pattern), and `ActivityHandling`
  (`ACTIVITY_HANDLING_UNSPECIFIED` defaults to `START_OF_ACTIVITY_INTERRUPTS` /
  `START_OF_ACTIVITY_INTERRUPTS` / `NO_INTERRUPTION`) all match the v4 baseline exactly.
  **New sibling field not in baseline: `turnCoverage`** (enum
  `TURN_COVERAGE_UNSPECIFIED` / `TURN_INCLUDES_ONLY_ACTIVITY` / `TURN_INCLUDES_ALL_INPUT` /
  `TURN_INCLUDES_AUDIO_ACTIVITY_AND_ALL_VIDEO`) — controls what counts as part of the
  user's turn. HIGH confidence, fetch-verified.

- **Manual VAD / push-to-talk** — unchanged: `automaticActivityDetection.disabled: true`
  in setup, client sends `{"realtimeInput":{"activityStart":{}}}` /
  `{"realtimeInput":{"activityEnd":{}}}` around the turn. **`audioStreamEnd` confirmed
  valid ONLY in automatic-VAD mode** (sent when the audio stream pauses >1s, e.g. mic
  switched off, to flush cached audio — stream can resume anytime). Official quote: *"An
  `audioStreamEnd` isn't sent in this configuration [manual/disabled VAD]. Instead, any
  interruption of the stream is marked by an `activityEnd` message."* This directly
  explains v4's earlier finding that `audioStreamEnd` broke PTT (stuck in THINKING, no
  response) — it was documented off-label usage. HIGH confidence, fetch-verified.

- **`sessionResumption`** — unchanged shape:
  ```json
  "sessionResumption": { "handle": "previous-session-handle-or-omit-for-new" }
  ```
  Server response (`sessionResumptionUpdate`) confirmed to have exactly two fields:
  `newHandle` (string, empty if not resumable) and `resumable` (bool). **No
  `lastConsumedClientMessageIndex` field exists.** Resumption tokens valid for **2 hours**
  after session termination. HIGH confidence, fetch-verified.

- **`contextWindowCompression`** — unchanged mechanism:
  ```json
  "contextWindowCompression": { "slidingWindow": {} }
  ```
  Union field `compressionMechanism`: `slidingWindow` (object) or `triggerTokens`
  (int64). If not set, default is 80% of the model's context window limit. HIGH
  confidence, fetch-verified.

- **`inputAudioTranscription` / `outputAudioTranscription`** — confirmed top-level
  siblings of `generationConfig` (not nested inside it), type `AudioTranscriptionConfig`,
  typically sent as `{}` to enable. HIGH confidence, fetch-verified.

- **`thinkingLevel` vs `thinkingBudget`** — both live under
  `generationConfig.thinkingConfig` (a nested `ThinkingConfig` object), **not** flat
  under `generationConfig` and **not** top-level in `setup`:
  ```json
  "generationConfig": {
    "responseModalities": ["AUDIO"],
    "thinkingConfig": { "thinkingLevel": "low", "includeThoughts": true }
  }
  ```
  `thinkingLevel` enum: `minimal`, `low`, `medium`, `high` — this is the **Gemini 3.x**
  parameter (default `medium` for 3.5 Flash, `high` for 3.1 Pro / 3 Flash, `minimal` for
  3.1 Flash-Lite). `thinkingBudget` (integer, model-specific range, `-1` = dynamic) is
  the **Gemini 2.5-series** parameter; Google's guidance is to use `thinkingLevel` for
  Gemini 3 models, with `thinkingBudget` "accepted for backwards compatibility [on 3.x]
  but may cause unexpected performance." **Not confirmed**: whether
  `gemini-2.5-flash-native-audio-preview-12-2025` specifically validates/uses
  `thinkingLevel` — the model page marks "Thinking: Supported" without specifying which
  param. v4's report that `thinkingLevel: "minimal"` was "accepted" (setupComplete
  returned) on the 2.5 model is plausible but may mean the field was silently ignored
  rather than actively used — worth a live behavioral test, not just a setupComplete
  check. HIGH confidence (nesting/enum), LOW confidence (2.5-native-audio exact
  acceptance behavior).

- **`systemInstruction`** — confirmed type is a `Content` object (role/parts shape,
  text-only parts), per the formal field table. Note: the same reference page's
  simplified human-readable example shows `"systemInstruction": string` — this is a
  documentation shorthand, not the authoritative type; the formal field table is
  correct. HIGH confidence, fetch-verified (with a flagged internal doc inconsistency).

- **New fields not in the v4 baseline**:
  - `proactivity: { proactiveAudio: bool }` — if enabled, the model can reject
    responding to the last prompt, ignore out-of-context speech, or stay silent if the
    user hasn't made a request yet.
  - `historyConfig: { initialHistoryInClientContent: bool }` — if true, after
    `setupComplete` the server waits and processes `clientContent` messages (without
    triggering a model call) until `turnComplete`, letting the client seed conversation
    history before going live via `realtimeInput`.

## 4. Session management & turn signals (fetch-verified 2026-07-07)

- **Time limits**: audio-only sessions cap at **15 minutes** without
  `contextWindowCompression`; audio+video caps at **2 minutes**. Separately, the
  **underlying WebSocket connection itself caps at ~10 minutes regardless of modality**
  — the session can outlive this only via resumption. Enabling
  `contextWindowCompression` removes the session-duration cap ("extend sessions to an
  unlimited amount of time"). Context window: **128k tokens** — stated on Firebase's
  (Google-owned) Live API limits page and corroborated by forum threads, but not found
  as a bare number on the primary `ai.google.dev` pages fetched — MEDIUM-HIGH confidence.
  [Session lifetime](https://ai.google.dev/gemini-api/docs/live-session) ·
  [Firebase Live API limits](https://firebase.google.com/docs/ai-logic/live-api/limits-and-specs) —
  HIGH confidence (time caps), fetch-verified.

- **`goAway`** — single field `timeLeft`, type `google.protobuf.Duration` (**not** a raw
  int/seconds field — must be decoded per protobuf Duration semantics). Official text:
  *"The remaining time before the connection will be terminated as ABORTED. This
  duration will never be less than a model-specific minimum."* Firebase's page cites
  ~60 seconds as typical, but the canonical spec only guarantees a "model-specific
  minimum" — treat 60s as a common default, not a contract. Recommended client behavior:
  reconnect proactively on receipt, using the `timeLeft` budget, rather than waiting for
  a hard ABORTED close. HIGH confidence, fetch-verified.

- **`sessionResumptionUpdate`** — fields `newHandle` (string, empty if not resumable)
  and `resumable` (bool). Valid for 2 hours after last session termination.
  *"Resumption is not possible at some points in the session... when the model is
  executing function calls or generating. Resuming... in such a state will result in
  some data loss. In these cases, `newHandle` will be empty and `resumable` will be
  false."* → v5 must track the `resumable` flag per update, not just the latest handle
  string, and only persist/use handles where `resumable=true`. No official
  backoff/retry-timing algorithm was found — genuine documentation gap. HIGH confidence,
  fetch-verified.

- **`turnComplete` vs `generationComplete`** — both current, both live inside
  `BidiGenerateContentServerContent`, and distinct (no renaming/deprecation found):
  - `generationComplete`: model is done generating content. *"When model is interrupted
    while generating there will be no `generation_complete` message in interrupted turn,
    it will go through `interrupted > turn_complete`. When model assumes realtime
    playback there will be delay between generation_complete and turn_complete caused by
    model waiting for playback to finish."*
  - `turnComplete`: the turn is fully closed; model stays silent until new client input.
  - On an **interrupted** turn, `generationComplete` is skipped entirely — straight from
    `interrupted` to `turnComplete`. HIGH confidence, fetch-verified.

- **Interruption / barge-in** — `interrupted` field: *"If true, indicates that a client
  message has interrupted current model generation. If the client is playing out the
  content in real time, this is a good signal to stop and empty the current playback
  queue."* Governed by `ActivityHandling` (`START_OF_ACTIVITY_INTERRUPTS` is default).
  Mode-dependent origin, confirmed by cross-referencing field docs:
  - **Automatic (server-side) VAD**: server runs VAD continuously and can originate an
    interruption independently — no client message required.
  - **Manual/disabled VAD (this firmware's PTT mode)**: `activityStart` *"can only be
    sent if automatic... activity detection is disabled"* — the client's `activityStart`
    is itself the trigger the server acts on; `interrupted: true` is the server's
    downstream acknowledgment, not an independent detection. This sharpens (doesn't
    contradict) the v4 baseline's framing. HIGH confidence (field text), MEDIUM-HIGH
    (auto-vs-manual origination distinction — logically forced by the gating text, but
    the supporting prose is a paraphrase, not a verbatim quote).

- **`audioStreamEnd`** — official definition: *"Indicates that the audio stream has
  ended, e.g. because the microphone was turned off. This should only be sent when
  automatic activity detection is enabled (which is the default). The client can reopen
  the stream by sending an audio message."* Confirms: **never send in manual/PTT mode**;
  `activityEnd` is the correct manual-mode boundary signal. HIGH confidence,
  fetch-verified, cross-confirmed by both the session-management and setup-schema
  research passes independently.

## 5. Audio formats (fetch-verified 2026-07-07)

- **Input**: 16-bit little-endian PCM, mono, natively 16kHz, `mimeType: "audio/pcm;rate=16000"`.
  The capabilities guide notes the Live API will resample if needed — *"so any sample
  rate can be sent"* — meaning 16kHz is the recommended/native rate, not a hard
  client-side requirement. v4 baseline confirmed current. HIGH confidence, fetch-verified.
- **Output**: fixed 24kHz, 16-bit LE PCM, mono. Quote: *"Audio output always uses a
  sample rate of 24kHz."* Not configurable/selectable — remains fixed, unlike input. No
  output-side `mimeType` required from the caller. HIGH confidence, fetch-verified.
- **Framing**: `realtimeInput.audio` (a `Blob` with `data` + `mimeType`):
  ```json
  {"realtimeInput": {"audio": {"data": "base64-encoded-audio", "mimeType": "audio/pcm;rate=16000"}}}
  ```
  `realtimeInput.mediaChunks[]` still exists in the reference but is explicitly marked
  **"DEPRECATED: Use one of `audio`, `video`, or `text` instead."** No max/recommended
  chunk size is documented anywhere — a genuine doc gap. HIGH confidence on field
  names/deprecation, N/A on chunk-size limits (not found).
- **Voice options**: native audio output models support the same voice roster as the
  standard TTS models via `speechConfig.voiceConfig.prebuiltVoiceConfig.voiceName` (30
  names: Zephyr, Puck, Charon, Kore, Fenrir, Leda, Orus, Aoede, Callirrhoe, Autonoe,
  Enceladus, Iapetus, Umbriel, Algieba, Despina, Erinome, Algenib, Rasalgethi,
  Laomedeia, Achernar, Alnilam, Schedar, Gacrux, Pulcherrima, Achird, Zubenelgenubi,
  Vindemiatrix, Sadachbia, Sadaltager, Sulafat), with the docs noting the exact set
  "is slightly different" for `generateContent` vs Live without spelling out the delta.
  Google's May 2026 I/O added **multi-speaker native audio output to the standard TTS
  `generateContent` API** (2 speakers, 24 languages) — **no official documentation found
  confirming multi-speaker or voice-cloning support inside the Live/WebSocket API
  specifically**; treat as unconfirmed/likely-not-yet-shipped there. LOW-MEDIUM
  confidence on the multi-speaker-in-Live-API question (absence-of-evidence inference).

## 6. OpenAI Realtime API — fallback-port sanity check (brief)

- Current GA model: **`gpt-realtime`**, latest snapshot **`gpt-realtime-2.1`** (released
  2026-07-06 — one day before this research pass), plus a `gpt-realtime-2.1-mini`
  distilled variant. `gpt-4o-realtime-preview*` and the old Realtime Beta API are fully
  deprecated/removed (`gpt-4o-realtime-preview` shutdown 2026-05-07; Beta API removed
  2026-05-12). HIGH confidence, fetch-verified.
- Transport: WebRTC (recommended browser/client), WebSocket (recommended server-side),
  native SIP (phone systems).
- Audio: PCM16, 24kHz, mono, LE for both input **and** output — **symmetric**, unlike
  Gemini's asymmetric 16kHz-in/24kHz-out. Real gotcha if v5 shares a resampler code path
  across both backends. Current GA session schema uses an object-style format field
  (e.g. `session.audio.input.format = {"type": "audio/pcm", "rate": 24000}`) rather than
  the old flat `"pcm16"` string enum. MEDIUM confidence on the exact schema (partial
  fetch + community corroboration, not one fully verbatim official page).

---

## CHANGED SINCE v4 (delta summary)

| Area | v4 baseline | Verified 2026-07-07 |
|---|---|---|
| Model | `gemini-2.5-flash-native-audio-latest` | **Unverifiable — retire.** Use `gemini-3.1-flash-live-preview` (primary) or `gemini-2.5-flash-native-audio-preview-12-2025` (fallback). |
| `gemini-3.1-flash-live-preview` | Believed non-existent (404 on 2026-05-23) | **Real, current flagship preview**, per official model page + deprecations table (release March 11 2026). Open discrepancy on the earlier 404 — unresolved, retest before relying on it. |
| `thinkingLevel` location | Ambiguous ("generationConfig? top-level?") | **Resolved: `generationConfig.thinkingConfig.thinkingLevel`** — nested, not flat. |
| `thinkingBudget` vs `thinkingLevel` | Not distinguished | `thinkingBudget` = Gemini 2.5-series param; `thinkingLevel` = Gemini 3.x param. 2.5-native-audio's actual handling of `thinkingLevel` unconfirmed by docs — test, don't assume. |
| `realtimeInputConfig` | `automaticActivityDetection` + `activityHandling` only | **New sibling field `turnCoverage`** added. |
| Setup top-level fields | 9 known fields | **Two new fields: `proactivity`, `historyConfig`.** |
| `audioStreamEnd` | Empirically broke PTT, cause unknown | **Root cause confirmed**: valid only under automatic VAD; manual/PTT mode must use `activityEnd` only. |
| `sessionResumptionUpdate` | Assumed heartbeat, handle usability unclear | **Confirmed fields are only `newHandle` + `resumable`** (no `lastConsumedClientMessageIndex`); handle unusable unless `resumable=true`; 2-hour validity window. |
| `goAway.timeLeft` | Assumed simple duration | **Confirmed protobuf `Duration` type** — decode accordingly, don't treat as a plain number. |
| WS connection lifetime | Not explicitly tracked | **New hard limit found: ~10 min WS connection cap, independent of the 15-min/2-min session caps** — a resilient client must race this clock too. |
| Auth | `?key=` only | `?key=` still valid; **ephemeral tokens now documented as an optional hardening path** (different endpoint/version, requires backend minting). |

---

## Primary sources

| Source | Notes | Fetch-verified |
|--------|-------|---|
| `firmware/components/cap_gemini_live/src/cap_gemini_live.c` | v4's working implementation — baseline for this delta pass. | n/a (local source) |
| [gemini-live-api.md](./gemini-live-api.md) | v4's own verified-knowledge page (2026-05-21 to 2026-05-24 passes). | n/a (local doc) |
| [Live API WebSockets reference](https://ai.google.dev/api/live) | `BidiGenerateContentSetup`, `RealtimeInputConfig`, `SessionResumptionUpdate`, `ContextWindowCompressionConfig`, `GoAway`, `ServerContent` field tables. | yes |
| [Get started with Live API (WebSockets)](https://ai.google.dev/gemini-api/docs/live-api/get-started-websocket) | Current code samples, current default model reference. | yes |
| [Live API capabilities guide](https://ai.google.dev/gemini-api/docs/live-guide) | VAD modes, `audioStreamEnd` semantics, native-audio behavior. | yes |
| [Session management with Live API](https://ai.google.dev/gemini-api/docs/live-session) | Time limits, resumption flow, `goAway` guidance. | yes |
| [Gemini Live API overview](https://ai.google.dev/gemini-api/docs/live-api) | Audio format (24kHz output fixed), general capabilities. | yes |
| [Live API best practices](https://ai.google.dev/gemini-api/docs/live-api/best-practices) | Reconnect-before-`goAway`-deadline guidance. | yes |
| [Ephemeral tokens](https://ai.google.dev/gemini-api/docs/live-api/ephemeral-tokens) | Optional client-side auth hardening path. | yes |
| [Gemini API deprecations table](https://ai.google.dev/gemini-api/docs/deprecations) | Model shutdown/release dates. | yes |
| [gemini-3.1-flash-live-preview model page](https://ai.google.dev/gemini-api/docs/models/gemini-3.1-flash-live-preview) | Confirms model is real, current, release date. | yes |
| [gemini-2.5-flash-native-audio-preview-12-2025 model page](https://ai.google.dev/gemini-api/docs/models/gemini-2.5-flash-native-audio-preview-12-2025) | Fallback model details. | yes |
| [GenerationConfig / ThinkingConfig reference](https://ai.google.dev/api/generate-content) | `thinkingConfig` nesting, field exclusions for Live API. | yes |
| [Gemini thinking docs](https://ai.google.dev/gemini-api/docs/generate-content/thinking) | `thinkingLevel` enum values, 2.5 vs 3.x guidance. | yes |
| [ai.google.dev/pricing](https://ai.google.dev/pricing) | Per-token/per-minute pricing for `3.1-flash-live-preview` and `2.5-flash-native-audio-preview-12-2025`. | yes |
| [Firebase AI Logic — Live API limits](https://firebase.google.com/docs/ai-logic/live-api/limits-and-specs) | Secondary Google-owned cross-check for context window (128k) and `goAway` timing; concurrency figure is Firebase-scoped, not confirmed for raw API keys. | yes |
| [Google AI Developers Forum — WS 1011 close thread](https://discuss.ai.google.dev/t/gemini-live-api-gemini-2-5-flash-native-audio-preview-12-2025-returns-code-1011-mid-turn-at-80-rate-started-2026-05-27/167186) | Reports `-12-2025` model closing mid-turn at ~80% rate since 2026-05-27. | **no** — WebSearch snippet only, single source, LOW confidence |
| OpenAI: [Realtime guide](https://developers.openai.com/api/docs/guides/realtime), [Realtime conversations guide](https://developers.openai.com/api/docs/guides/realtime-conversations), [gpt-realtime-2.1 model page](https://developers.openai.com/api/docs/models/gpt-realtime-2.1), [Deprecations](https://developers.openai.com/api/docs/deprecations) | Fallback-port sanity check. | yes (model/transport), partial (exact audio-format schema) |

---

## 7. Server pacing, measured on the device (2026-09-01)

Native-audio replies are **not** delivered faster than real time. Over nine
spoken turns (`/api/debug/say`, `gemini-3.1-flash-live-preview`, RSSI −32,
power save off), the device's receive counters (`/api/device/health` →
`rx`) showed:

| Quantity | Measured |
|---|---|
| Longest gap between WS frames inside one reply | 0.81–1.34 s |
| Frames queued at once after a stall (TCP backlog) | 9–24 |
| Parser lag (queue wait) | ≤ 142 ms |
| Frames per 3–4 sentence reply | 55–121 |

The output transcription events arrive on the same cadence as the audio, and
a playback hole sat exactly inside a 1.75 s silence between two consecutive
transcript words — so the stalls are the server's generation, not the radio
or the device. Consequence for any client: the speaker must run **behind**
the network. JarvisRobot v5 pre-rolls 600 ms before a reply's first word and
rebuilds a 1000 ms lead after any hole (`jr_audio.c`, `PB_PREROLL_MS_DEFAULT`
/ `PB_REFILL_MS_DEFAULT`, tunable via `/api/debug/gain?preroll=&refill=`);
below that, one hole of 81–407 ms per reply survived. A WebSocket receive
queue of 24 frames overflowed on the post-stall burst and dropped speech
before parsing; 96 does not.

## Open questions

- Why did `gemini-3.1-flash-live-preview` 404 on 2026-05-23 when its official release
  date is March 11 2026? Retest directly before committing v5 to this model.
- Does `gemini-2.5-flash-native-audio-preview-12-2025` actually have the WS-close-1011
  instability reported in the single forum thread found? Worth a direct fetch of that
  thread and/or a live smoke test before relying on it as the fallback model.
- Does `gemini-2.5-flash-native-audio-preview-12-2025` actually use `thinkingLevel` or
  does it require `thinkingBudget` — the model page doesn't say, and v4's "acceptance"
  evidence (setupComplete returned) doesn't rule out the field being silently ignored.
- Is multi-speaker native audio output (added to standard TTS `generateContent` API,
  May 2026 I/O) available in the Live API specifically? No confirming or denying
  official doc found either way.
- What is the actual current per-key concurrent-session limit for the raw Gemini API
  (not Firebase AI Logic)? The official `ai.google.dev/gemini-api/docs/rate-limits` page
  defers to the account-specific AI Studio dashboard rather than publishing a number.
- No official backoff/retry-timing guidance exists for reconnects after a session drop
  — v5 will need to pick its own strategy (exponential backoff is the sane default, but
  it's not Google-prescribed).

---

## See also

- [gemini-live-api.md](./gemini-live-api.md) — v4's baseline findings this page is a delta against.
- [llm-config.md](./llm-config.md) — other LLM protocol options.
