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

The fixed templates (`components/jr_tools/src/jr_tools_templates.c`, pinned
by `components/jr_tools/host/test_templates.c`), as of 2026-09-02:

- `recall_memory` — five hits, 200 characters each
- `remember` — one append-only capture event, 200 characters
- `current_time` — the owner's zone unless one is given
- `search_tools` — eight compact matches with the three basics pinned
- `execute_tool` — one allowlisted `service.method` with JSON args; the
  device refuses `delete*/remove*/destroy*/purge*/wipe*/archive*` names
- `delegate_task` — one work item on the paired board; returns `{id, title,
  status}`
- `delegated_tasks` — six items, newest first, 60 of title, 120 of result
- device-owned, never declared to the model: `weather_glance`, `board_poll`

`remember` needs no tap since 2026-09-01 (`REMEMBER_NEEDS_TAP 0`): the
owner's asking is the approval. `physical_confirmed` is still an internal job
field, not a Gemini argument, for any future write that keeps the card.

## Findings

**[2026-09-02] `completeWorkItem` wants `result` as an object.** A top-level
`resultSummary` is refused with `bad_input - result must be an object`; the
accepted shape is `{ result: { summary, evidence?: string[] } }`, and the
summary then appears as `resultSummary` in the summary projection that
`listWorkItems` and the device's `board_poll` read. `tools/board-worker/worker.py`
was corrected the same day.

**[2026-09-02] The board, as seen from the device.** `coordination.*`
takes its arguments directly (never wrapped in `{input:…}` whatever a
generated schema says) and every write needs an identity tuple
`{runtime, agentId, hostId, sessionId}`; the templates use the device's own
(`pi/jarvisnano/jarvisnano/<date>`). `createWorkItem({projectId, title,
description, priority})` answers with the item (`id`, `status`);
`listWorkItems({projectId, detail:"summary"})` answers with an array whose
items carry `id, title, status, resultSummary, lastProgressAt` and lease
state, description bodies omitted. Raw items do not fit the 3 KB response
slot, so every board template projects: `board_poll` keeps six of
`{i:40, s:12, n:48, r:120}` (worst case under 1.6 KB). The project id is
spliced into the program as `const PJ="…";` and is therefore validated to
`[A-Za-z0-9._-]` at both the NVS field and the template setter, with the
host suite proving a quote or a space is refused. The `jarvisnano-desk`
project was created on the live board on 2026-09-02.

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
