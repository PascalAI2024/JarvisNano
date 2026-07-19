# JarvisMCP device bridge

The v5 bridge is an asynchronous ESP32 component in
`components/jr_tools`. Gemini declares a small function catalogue; the model
chooses a tool and supplies bounded JSON arguments; the worker performs HTTPS
on the physical device and returns a Gemini `toolResponse`.

No Android or desktop companion participates in this loop.

## Security boundary

- Store `jarvis_mcp_url` and `jarvis_mcp_key` only in the `app` NVS namespace.
- Use a dedicated, revocable JarvisNano credential.
- Prefer JarvisMCP `POST /device/v1/invoke`. It accepts typed tool names and
  arguments, never client-supplied JavaScript.
- The firmware still supports an already-provisioned `/act` endpoint for
  compatibility. Its locally-owned templates prevent model code injection,
  but a stolen legacy bearer retains the gateway authority assigned to that
  key. Move production devices to the typed route.
- Diagnostics expose only configured/ready/status counters. They never return
  endpoint or credential values.
- HTTPS is mandatory for JarvisMCP calls.

## Runtime flow

1. Gemini sends `toolCall` with its original string call ID.
2. The voice owner deep-copies the call into the bounded `jr_tools` queue.
3. One worker builds either a typed device request or a fixed legacy template.
   A typed `remember` also receives a stable top-level `request_id` derived for
   that Gemini call.
4. The worker POSTs over HTTPS with `Authorization: Bearer <device key>`.
5. A bounded `{ok,result}` response is copied to the result queue.
6. The voice owner rejects stale session generations and sends a Gemini
   `toolResponse` using the original call ID and tool name.

Tool HTTPS never runs on the voice task. Queue depth, response size, timeout,
cancellation, and session generations are bounded.

## Catalogue

Read tools:

- `current_time`
- `weather`
- `crypto_price`
- `recall_memory`
- `wikipedia`
- `country_info`

`remember` is mutating and is dispatched only after an ALLOW tap on the
physical consent card. Its note is limited to 47 panel-renderable characters.
The firmware rejects invalid or unrenderable notes before consent and displays
the exact accepted note on the DENY/ALLOW card. `physical_confirmed` is an
internal job field, not a Gemini argument; the typed request sends it as
confirmation together with the stable top-level `request_id` required for the
write.

The local consent card owns the panel and control-intent lane until ALLOW,
DENY, cancellation, or the 15-second timeout resolves it. Other control-intent
routes return `423 Locked`, and Brain inbox attempts return `409 Conflict`, so
remote surfaces cannot replace or dismiss the physical write prompt.

## Configuration truth

The NVS field names are each within ESP-IDF's 15-character key limit:

- `jarvis_mcp_url`
- `jarvis_mcp_key`

An empty field is a supported unconfigured state. The device still boots and
returns a structured `not_configured` result to Gemini. Do not interpret a
successful firmware build as proof that production gateway credentials have
been provisioned.

After a physical pairing claim, configuration is managed through
`GET/POST /api/tools/config`. Both methods require the pairing token and
`X-JarvisNano-Control: 1`. GET exposes only configured and route-kind booleans.
POST accepts exactly `{url,key}`; the URL must be HTTPS and end in
`/device/v1/invoke` or `/act`. Two empty strings clear both values. The worker
reloads the NVS values without returning or logging them.
