# JarvisNano v5 protocol

> Runtime truth as of 2026-07-09. The active hardware is the Waveshare
> ESP32-S3 Touch AMOLED 1.75 (466 x 466). This replaces the retired ESP-Claw /
> Seeed protocol described by older revisions of this file.

JarvisNano is standalone today. The ESP32 owns the microphone, touch panel,
display, speaker, Wi-Fi, Gemini Live session, and JarvisMCP tool loop. A phone
or desktop is optional and is not in the active voice path.

```text
microphone -> ESP32 voice task -> Gemini Live
                                  |
                                  | function call
                                  v
                         ESP32 JarvisMCP worker
                                  |
                                  | HTTPS + device credential
                                  v
                         JarvisMCP device gateway
                                  |
                                  v
                     tool result -> Gemini -> speaker
```

## Brain routes

| Route | Status | What owns reasoning and audio |
| --- | --- | --- |
| `cloud_gemini` | Active | Gemini Live, with JarvisMCP calls executed directly by the ESP32 |
| `desk_codex` | Optional surface | Gemini still owns voice; a paired Mac may publish bounded cards and receive touch actions |
| `private_android` | Not implemented | Future mutually exclusive BLE audio route to a private Android STT/LLM/TTS stack |

There is no automatic cloud fallback from the future private route. Switching
to it must explicitly close Gemini/Wi-Fi voice before BLE audio starts.

## Discovery

The device uses a DHCP hostname shaped like `jarvisnano-a1b2c3`, but the v5
build does not yet ship the ESP-IDF mDNS component. Clients must currently use
the IP shown by the cockpit/network diagnostics or a manual host override.

HTTP listens on port 80. The built-in Orbit Console is `GET /`.

## HTTP surface

Read-only diagnostics are available on the trusted local network. Mutating
diagnostic routes are POST-only and require:

```http
X-JarvisNano-Control: 1
```

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/` | Orbit Console |
| GET | `/api/cockpit` | Combined network, voice, on-device tool, display, touch, Agent Link, and Brain Link truth |
| GET | `/api/gemini/live` | Detailed Gemini/audio/tool counters |
| POST | `/api/debug/say?text=...` | Queue a text turn through the live device voice session |
| POST | `/api/debug/gain?...` | Bench-only audio tuning |
| POST | `/api/voice/control?armed=0|1` | Explicitly pause or resume always-ready voice |
| POST | `/api/audio/self-test` | Run the speaker-to-microphone diagnostic capture |
| GET | `/api/audio/taps` | Audio tap metadata |
| GET | `/api/audio/tap.wav?source=...` | Bounded diagnostic WAV capture (`mic-clean`, `mic-raw`, `reference`, or `playback`) |
| GET | `/api/touch` | Touch counters and panel challenge state |
| POST | `/api/diag/panel-touch?action=start|cancel` | Physical panel/touch proof |
| POST | `/api/ui/shade?action=open|close|toggle` | Pull-down shade control |
| GET/POST | `/api/agent/link` | Bounded coding-status surface |
| GET | `/api/brain/outbox?after=N` | Brain Link state and physical action events |
| POST | `/api/brain/inbox` | Present, update, or dismiss a bounded panel surface |
| GET/POST | `/api/tools/config` | Redacted JarvisMCP status / paired provisioning |
| POST | `/api/pairing/claim?rotate=1` | One-time pairing-token claim after a physical long press |
| GET | `/api/display` | Display health |
| GET | `/api/display/snapshot.json` | Submission-mirror metadata |
| GET | `/api/display/snapshot.ppm` | PPM display mirror |
| GET | `/api/display/snapshot.rgb565` | Raw 466 x 466 RGB565 display mirror |
| POST | `/api/display/test?pattern=...` | Deterministic display diagnostic |

Authentication is route-specific:

| Surface | Required proof |
| --- | --- |
| Diagnostic POSTs | `X-JarvisNano-Control: 1` |
| Agent Link GET | none |
| Agent Link POST | pairing token |
| Brain outbox | pairing token |
| Brain inbox | pairing token plus control header |
| Tools config GET/POST | pairing token plus control header |
| Pairing claim | control header plus the physical 60-second claim window |

While a local `remember` consent card is active, it owns the physical panel
and the control-intent lane. Other control-intent routes return `423 Locked`;
Brain inbox requests return `409 Conflict` so a paired remote client cannot
replace, update, or dismiss the local approval prompt.

The HTTP control plane is plaintext and therefore trusted-LAN-only. Pairing
tokens authorise protected routes; they do not encrypt traffic. BLE, USB, or
device HTTPS is required before the same bearer can be considered safe on an
untrusted network.

## Direct JarvisMCP tools

The Gemini setup advertises a fixed, bounded catalogue. Gemini never supplies
JavaScript. The ESP32 worker owns one asynchronous HTTPS lane and returns a
Gemini `toolResponse` without blocking microphone/display work.

Current read tools:

- `current_time()`
- `weather(location)`
- `crypto_price(symbol)`
- `recall_memory(query)`
- `wikipedia(topic)`
- `country_info(country)`

`remember(note)` is a write tool and requires an explicit approval tap on the
physical panel. A note is limited to 47 characters and every character must be
renderable by the panel's 5x7 glyph set. The firmware rejects an invalid or
unrenderable note before opening consent, then shows the exact accepted note on
the DENY/ALLOW card. The approval bit is carried outside model-provided
arguments; only the physical ALLOW tap sets it.

The endpoint and credential are persisted only in the `app` NVS namespace,
then copied into bounded worker/request RAM only while needed:

- `jarvis_mcp_url`
- `jarvis_mcp_key`

The preferred endpoint is `POST /device/v1/invoke` on JarvisMCP. It accepts
only `{tool,args,confirmation?,request_id?}`, constructs server-owned SDK
calls, rate limits each device identity, caps results, and never accepts
arbitrary code. A typed `remember` requires both the physical confirmation and
a stable top-level `request_id` generated by the firmware for that Gemini tool
call. The legacy `/act` form remains a compatibility path for an
already-provisioned device, but it does not provide server-enforced device
scope.

Provision a dedicated, revocable JarvisNano credential. Never provision a
general MCP bearer or put any credential in source, logs, diagnostics, or a
browser-visible response.

Provision or clear the device after pairing with `POST /api/tools/config`:

```json
{"url":"https://gateway.example/device/v1/invoke","key":"<device-key>"}
```

The body must contain exactly `url` and `key`. The URL must be HTTPS and end in
`/device/v1/invoke` or legacy `/act`. Sending two empty strings clears both.
`GET /api/tools/config` returns booleans and route kind only, never values.

## Pairing and Brain Link surfaces

Long-press the panel for 1.2 seconds to open a 60-second physical claim window,
then call `POST /api/pairing/claim?rotate=1` with the control-intent header.
The returned token is shown once; only its SHA-256 hash remains in NVS.

Authenticated Brain Link calls additionally send:

```http
X-JarvisNano-Token: <paired token>
```

Inbox envelope:

```json
{
  "v": 1,
  "type": "surface.present",
  "seq": 1,
  "session": "codex-desk",
  "id": "build-42",
  "ttl_ms": 30000,
  "payload": {
    "kind": "choice",
    "title": "BUILD PASSED",
    "body": "RUN DEVICE TESTS?",
    "actions": [
      {"id": "later", "label": "LATER"},
      {"id": "run", "label": "RUN"}
    ]
  }
}
```

Supported types are `surface.present`, `surface.update`, and
`surface.dismiss`. Updates and dismissals must match the active `(session,id)`.
Sequence numbers are strictly monotonic for the current boot. Text is bounded
to the panel's 5x7 glyph set; up to three actions are allowed, with stricter
cardinality for `choice`, `consent`, and `progress` kinds.

Physical button presses are returned as `surface.action` envelopes from
`GET /api/brain/outbox?after=N`. Consumers must advance their cursor per event
and fail loudly if the eight-event ring has overrun.

The host utility is:

```bash
python3 scripts/jarvis-desk.py --host '<device-ip>' status
```

It stores the pairing token in macOS Keychain. Its HTTP mode is also
trusted-LAN-only.

## Private Android route

The Android project currently provides cockpit compatibility and a BLE/client
scaffold. The firmware does not yet expose the GATT service or audio
characteristics, and no local model is wired. Those are future implementation
steps, not current runtime claims.

The intended private route is:

```text
ESP32 microphone/display/touch <-> BLE <-> Android STT + local LLM + TTS
```

JarvisMCP may still be called directly by the ESP32 using the same device
credential; Android is not required to proxy it.
