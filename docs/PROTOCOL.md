# JarvisNano v5 protocol

> Runtime truth as of 2026-09-01. The active hardware is the 32 MB Waveshare
> ESP32-S3 Touch AMOLED 1.75C (466×466). The original 1.75 and retired
> ESP-Claw/Seeed protocol remain compatibility tracks, not route authorities.

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

The device uses a DHCP hostname shaped like `jarvisnano-a1b2c3`, but v5 does
not ship mDNS. Read the assigned IP from the USB serial boot log or router DHCP
lease, then pass it explicitly to the host tools.

HTTP listens on port 80. The built-in Orbit Console is `GET /`.

## HTTP surface

The root page and coarse hardware counters are available on the trusted local
network. Content-bearing reads require pairing. Mutating diagnostics are
POST-only and require:

```http
X-JarvisNano-Control: 1
```

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/` | Orbit Console |
| GET | `/api/cockpit` | Paired network, voice, tool, display, touch, Agent Link, and Brain Link truth |
| GET | `/api/gemini/live` | Paired detailed Gemini/audio/tool counters and transcript tail |
| POST | `/api/debug/say?text=...` | Queue a text turn through the live device voice session |
| GET/POST | `/api/debug/gain?...` | Audio tuning with readbacks; `preroll=` / `refill=` pin the playback jitter buffer (`preroll=0` returns it to adaptive); `cpu=80|160|240` forces a CPU gear for a bench, `cpu=0` returns to auto |
| POST | `/api/debug/audio-stats?reset=1` | Reset the playback and receive-queue counters that `/api/device/health` reports |
| GET/POST | `/api/debug/sleep` | How the device last woke and what was armed; `?now=1&wake_s=N` forces a deep sleep with an N-second timer (refused with 409 while the image is on OTA probation) |
| POST | `/api/voice/control?armed=0|1` or paired `?resume=1` | Explicit privacy mute/unmute or privacy-safe operational resume |
| POST | `/api/audio/self-test` | Run the speaker-to-microphone diagnostic capture |
| GET | `/api/audio/taps` | Paired audio tap metadata |
| GET | `/api/audio/tap.wav?source=...` | Paired bounded WAV capture (`mic-clean`, `mic-raw`, `reference`, or `playback`) |
| GET | `/api/touch` | Touch counters and panel challenge state |
| POST | `/api/diag/panel-touch?action=start|cancel` | Physical panel/touch proof |
| GET | `/api/diag/tasks` | Internal-memory and per-task stack high-water state; per-task `run` counters and `total_runtime` (esp_timer µs) give each core's idle share between two snapshots |
| GET | `/api/diag/vadlog` | Paired bounded VAD/barge decision CSV |
| GET | `/api/logs?tail=N` | Paired 128 KB in-memory device log ring |
| GET | `/api/sensors` | QMI8658 motion and AXP2101 battery telemetry |
| POST | `/api/display/hud?on=0|1` | HUD frame-cost diagnostic |
| POST | `/api/display/choices?n=0..3` | Static choice-arc rendering diagnostic |
| GET | `/api/display/choices/hit?x=...&y=...` | Read-only choice geometry check |
| POST | `/api/input/tap?...` | Paired legacy synthetic input diagnostic |
| POST | `/api/demo` | Bounded on-device attract reel |
| POST | `/api/ui/shade?action=open|close|toggle` | Pull-down shade control |
| GET/POST | `/api/agent/link` | Paired bounded coding-status surface |
| GET | `/api/brain/outbox?after=N` | Brain Link state and physical action events |
| POST | `/api/brain/inbox` | Present, update, or dismiss a bounded panel surface |
| GET/POST | `/api/tools/config` | Redacted JarvisMCP status / paired provisioning |
| GET | `/api/device/health` | Paired derived diagnosis and repairability verdict |
| GET/POST | `/api/device/levels` | Paired persistent volume and mood-capped brightness |
| POST | `/api/pairing/claim?rotate=1` | One-time token claim during the physical BOOT-hold window |
| GET | `/api/display` | Display health |
| GET | `/api/display/snapshot.json` | Submission-mirror metadata |
| GET | `/api/display/snapshot.ppm` | Paired PPM display mirror |
| GET | `/api/display/snapshot.rgb565` | Paired raw 466×466 RGB565 display mirror |
| POST | `/api/display/test?pattern=...` | Deterministic display diagnostic |
| POST | `/api/display/canvas?ttl=ms` | TTL-bounded RGB565 remote canvas |
| POST | `/api/debug/input?kind=...` | Synthetic tap/hold/swipe through the real input queue |
| GET/POST | `/api/operator/lease` | Read/enter/exit bounded Codex glass ownership |
| POST | `/api/ota/upload` | Stream an app image into the idle OTA slot and reboot |

Authentication is route-specific:

| Surface | Required proof |
| --- | --- |
| Control-only diagnostics (say/gain/audio self-test/panel challenge/HUD/choices/demo/shade/display test/canvas) | control header |
| Paired synthetic input (`/api/input/tap`, `/api/debug/input`) | pairing token plus control header |
| Agent Link GET/POST | pairing token |
| Brain outbox | pairing token |
| Brain inbox | pairing token plus control header |
| Tools config GET/POST | pairing token plus control header |
| Pairing claim | control header plus the 60-second window opened by a 1.5–5 s runtime BOOT hold |
| Operator takeover/release | pairing token plus control header |
| Operator mode GET | none |
| OTA upload | pairing token plus control header |
| Device health GET | pairing token |
| Device levels GET/POST | pairing token; POST also requires control header |
| Voice control (mute, unmute, or privacy-safe resume) | pairing token plus control header |
| Content diagnostics (cockpit, Gemini detail, audio taps/WAV, VAD log, logs, display pixels) | pairing token |

While a local `remember` consent card is active, it owns the physical panel
and the control-intent lane. Other control-intent routes return `423 Locked`;
Brain inbox requests return `409 Conflict` so a paired remote client cannot
replace, update, or dismiss the local approval prompt.

The HTTP control plane is plaintext and therefore trusted-LAN-only. Pairing
tokens authorise protected routes; they do not encrypt traffic. BLE, USB, or
device HTTPS is required before the same bearer can be considered safe on an
untrusted network.

## Direct JarvisMCP tools

The Gemini setup advertises seven fixed gateway templates, two local controls,
and two server-policy meta-tools. Gemini never supplies JavaScript. The ESP32
worker owns one asynchronous HTTPS lane and returns a Gemini `toolResponse`
without blocking microphone/display work.

Local device tools:
- `set_volume(level)` — local/persistent, 10..100
- `set_brightness(level)` — local/persistent mood ceiling, 10..100

Fixed JarvisMCP gateway tools:
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

Server-policy meta-tools:
- `search_tools(query)` — bounded catalog discovery; discovery never grants
  execution
- `execute_tool(tool,args_json)` — resolves a catalog method on the server;
  only explicitly classified read/compute methods run directly, write/proposal
  methods require physical confirmation and a stable request ID, and
  destructive, site-scoped, credential-management, executable-code, unknown,
  and unclassified methods fail closed

The ESP32 translates `args_json` into a JSON object only for the typed device
endpoint. The legacy `/act` path returns a fixed error for `execute_tool`; it
never receives or evaluates model-provided code.

Firmware never treats a model-authored summary as physical authority.
`execute_tool` is therefore read/compute-only from Gemini today: the server
accepts explicitly classified safe methods and rejects writes/proposals because
the device supplies no confirmation. The fixed `remember(note)` flow remains
the only write because its complete canonical payload fits and is rendered
exactly. Generic writes stay blocked until the server can issue a canonical,
bounded confirmation challenge that the panel displays byte-for-byte.

The endpoint and credential are persisted only in the `app` NVS namespace,
then copied into bounded worker/request RAM only while needed:

- `jarvis_mcp_url`
- `jarvis_mcp_key`

The preferred endpoint is `POST /device/v1/invoke` on JarvisMCP. It accepts
only `{tool,args,confirmation?,request_id?}`, resolves methods against the same
merged capability manifest used by MCP search, rate limits each device
identity, caps results, and never accepts arbitrary code. The server supports
confirmed mutating calls with stable request IDs, but JarvisNano does not emit
that authority for generic `execute_tool` calls yet.
The legacy `/act` form remains a compatibility path for the seven fixed
templates only; it does not provide server-enforced device scope.

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

The controls surface exposes volume, brightness, physical **MUTE/LISTEN**, and
the PWR/BOOT role legend. Holding BOOT for 1.5–5 seconds after startup opens a
visible 60-second pairing claim window; `jarvis-desk.py pair` claims exactly one
token and stores it in the host Keychain. BOOT held during reset remains the ROM
downloader path. Glass hold remains exclusively privacy mute/unmute.

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

## Physical-state signals

- Breathing cyan ring at r186–193: microphone is actively listening.
- Persistent gold ring at r221–222: privacy mute/deactivated voice.
- Red inner rim: recoverable error state.
- Battery uses r215–220; choice arcs use r223–231, so all three outer states
  remain disjoint.
- `scripts/jarvisctl.py gestures [lines]` extracts physical input receipts and
  their resolved actions from the device log ring.

## Codex operator mode

`POST /api/operator/lease?ttl=seconds` enters a bounded tool mode. Gemini voice
and normal gestures pause; the violet Agent rim identifies the external glass
owner. A paired Desk client may present one bounded notice/progress/result/
choice/consent surface through `/api/brain/inbox` and receive touch actions from
`/api/brain/outbox`.

- Single taps belong to the active Desk surface and return `surface.action`.
- Inputs without a surface are retained by Codex mode, not routed to voice.
- **Double-tap always exits** and dismisses the remote surface. If privacy was
  already held, gold mute remains; otherwise always-ready Jarvis resumes.
- TTL expiry performs the same privacy-safe recovery without a client.
- `GET /api/operator/lease` returns `{active,mode,ttl_ms}`.
- `jarvisctl takeover`, `mode`, `normal`, and `desk ...` are the operator CLI.

This is external tool ownership, not autonomous JarvisMCP push. JarvisMCP
remains the on-device bounded tool gateway used by Gemini; Desk surfaces are
the paired display/action channel used by Codex.

## Self-diagnosis and paired levels

`GET /api/device/health` derives one bounded verdict from voice liveness,
privacy/operator ownership, memory, transport drops, display health, DAC mute,
and the latest JarvisMCP status. Its `ota` object reports active upload bytes,
running/boot slots, image state (`pending-verify` versus `valid`), and the last
ESP-IDF error without exposing credentials. `jarvisctl doctor` adds
task/audio/sensor reads and collapses the 16 KB log tail into incident
categories. `doctor --repair` may issue only paired `resume=1`, which refuses
to clear hold/flip privacy.

`POST /api/device/levels?volume=40&brightness=60` persists either optional
10..100 field without touching voice state. Brightness is a ceiling multiplied
by the current mood, so rest may dim further. The app task applies both values
and puts a receipt on glass.

Global physical edge swipes, paired level commands, and the two local Gemini
level tools converge on the same persisted app-task state and on-glass receipt.
Left-edge UP/DOWN controls volume; right-edge DOWN/UP controls brightness.
PWR short is listen-only recovery, PWR long is battery status, BOOT short
opens/closes controls, and a 1.5–5 second runtime BOOT hold opens pairing.
Sustained face-down enters flip privacy; sustained face-up clears it only when
the flip created the mute. A hold/controls mute survives reorientation.

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
