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
- device-owned, never declared to the model: `weather_glance`, `board_poll`.
  Since 2026-11-09 `weather_glance` bypasses the gateway: the worker GETs
  Open-Meteo directly (`jr_weather.h`, no key) and projects the body on the
  device; its gateway template in `jr_tools_templates.c` is now unused by the
  firmware and only pinned by the host tests.

`remember` needs no tap since 2026-09-01 (`REMEMBER_NEEDS_TAP 0`): the
owner's asking is the approval. `physical_confirmed` is still an internal job
field, not a Gemini argument, for any future write that keeps the card.

## Findings

**[2026-09-02] Open-Meteo can fail with a 200.** For about an hour the
gateway's `jarvis.weather` threw `Unexpected token 'U' … is not valid JSON`
or timed out at 15 s, while the same URL answered a laptop in 150 ms: the
body the gateway received was HTTP 200, `content-type: application/json`,
text `Unexpected error while streaming data: allEndpointsUnavailable` — a
regional edge failure on the provider's side. On the device it read as
`weather_glance status=http_error http_status=500` (the sandbox turns the
throw into a 500) and `execute_tool` for a spoken weather question spent
31 s (two 15 s timeouts, positional then named) and failed the same way.
`weather_glance` now retries every two minutes until it succeeds; the
gateway's `sandbox/sdk/weather.ts` could turn a non-JSON 200 into a named
upstream error, but that is the gateway's change to make.

**[2026-09-02] `completeWorkItem` wants `result` as an object.** A top-level
`resultSummary` is refused with `bad_input - result must be an object`; the
accepted shape is `{ result: { summary, evidence?: string[] } }`, and the
summary then appears as `resultSummary` in the summary projection that
`listWorkItems` and the device's `board_poll` read. `tools/board-worker/worker.py`
was corrected the same day.

**[2026-09-02] The managed sandbox worker is a coding agent, not a general
one.** `sandboxes.workerSubmit` with `kind:'pi'` refuses `delivery:'none'`
(`invalid_delivery`) and any repository off the gateway's allowlist
(`repo_not_allowed`, HTTP 403); it delivers a branch or a pull request on an
allowlisted repo, with model and GitHub credentials held server-side. A
`kind:'probe'` job ran the full lifecycle in 18 s (queued → started → exit 0
→ sandbox verified deleted). So the device's poll routes only goals that
name `owner/repo` there, and answers everything else with `research` (M3,
cited; measured 21 s and then 26 s for the whole poll including claim,
`memory.search` context, `memory.capture` and `completeWorkItem`, hence
`queries:2, maxSources:5` now) — inside the gateway's 30 s call budget,
which is why the poll settles one item at a time. `memory.capture` lands as
an append-only inbox event (`company-brain/inbox/events/<date>/…`), which
`memory.search` does not index — read it back with `memory.list` on the
inbox, not by searching. The research answer arrives as markdown with `[n]`
citations; the spoken 300 characters strip both. `listWorkItems` summary rows carry
`leaseState` (`active`/`expired`/…), which is what makes a poll killed at 30 s
recoverable on a later one via `recoverWorkItem`.

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

## A result larger than the device (2026-11-10)

**What happened.** The first voice session on the live gateway ended its first
tool call as `status=bad_response http_status=200` with no `mcp parse failed`
and no `mcp result over budget` line. Both of those live in
`response_from_mcp()`, so it never ran: the body never reached it. The only
silent routes to `bad_response` on a 200 were the two size checks in
`post_tool` (`announced > JR_TOOLS_HTTP_BODY_CAP` and the read loop's
`overflow`), so the gateway body was larger than 8192 bytes.

**What bounded an `execute_tool` result before.** Only the `T` projection at
the end of `JR_TOOLS_EXEC_JS` (`components/jr_tools/src/jr_tools_templates.c:196`
at 613b551): strings cut at 320 characters, arrays at six items, objects at 24
keys, and objects deeper than four levels returned **as they were**. Each limit
is per node and nothing bounds the total, so a list of work items (six items
times 24 keys times 320 characters is 46 KB), a deep object, or a wide one
all went past 8 KB. Which method the model named is not in the log. A body between 3 KB and
8 KB was already handled: `response_from_mcp` cuts it to a flagged text head
(`jr_tools.c:331-381`).

**The byte budget (gateway side).** The program's source is now
`components/jr_tools/js/exec_tool.js`; `scripts/gen-exec-js.py` strips it to one
line and rewrites the block between the `BEGIN/END GENERATED` markers
(`jr_tools_templates.c:194`). Never hand-edit the escaped line.

- The budget is `JR_TOOLS_RESPONSE_CAP` (3072) minus the envelope the device
  adds (`{"result":` + `}` + NUL = 12) minus 64 bytes of slack (cJSON asks for
  26 bytes of room before it prints a number): **2996 bytes of UTF-8**. The
  generator derives it from the header, so it follows the cap.
- A result that fits is trimmed as before and not flagged. One that does not is
  projected through six tiers (items, string length, keys, depth) from
  `[6,320,24,4]` down to `[1,24,4,2]`; the first tier that serialises under
  budget wins. Arrays keep their first N items and carry `total`; a cut array
  inside an object gets a `<key>_total` sibling; objects keep their first K
  keys; long strings are cut with an ellipsis; branches past the depth become
  `"{…}"` or `"[…]"`. A projection forced by the budget carries `truncated:true`
  and a `hint` ("ask for fewer items or a narrower question").
- The last resort is a string head (`{text,total_chars,truncated,hint}`). It is
  reachable: three-byte keys and values overflow even the tightest tier.
- Sizes are counted in UTF-8 bytes, not characters.
- The program plus its largest path (100 B) and largest args (768 B) must fit
  `JR_TOOLS_CODE_CAP` (4096): the generator refuses to write otherwise (program
  3137 B), and `test_templates.c` builds the 47-byte-target, 767-byte-args call.

**The device side.** An oversize body no longer ends as `bad_response`.
`post_tool` (`jr_tools.c:532` announced, `:573` overflow) calls `note_oversize`
(`jr_tools.c:244`): one warning line `result over body cap: tool=<name>
target=<method> announced=<n> read=<n> cap=8192` (the method name only, never
its arguments; for `execute_tool` the `tool` member, validated against
`[A-Za-z0-9._-]`, else `?`), the counter, and a normal bounded result
`{"result":{"truncated":true,"too_large":true,"bytes":N,"note":"…ask for fewer
items or a narrower question"}}`. The pure parts (`jr_tools_target_name`,
`jr_tools_describe_oversize`, `jr_tools_oversize_result`) are in
`jr_tools_templates.c:523-580` and host-tested. Every other route to
`bad_response` now logs once: a result copy that ran out of memory
(`jr_tools.c:320`), an over-budget payload that could not be cut (`:383`), and
the weather fetch's two size checks (`:628`, `:650`); `mcp parse failed` and the
weather projection failure already did.

`/api/cockpit` `tools` gained `oversize` (bodies past the cap since boot) and
`last_target` (the method the last job addressed), `http_routes.c:2866`. The
snapshot buffer grew from 4096 to 4608 bytes (`COCKPIT_BODY_CAP`, `:2780`) to
make room.

**Wording.** The model reached for `execute_tool` to read the board because its
description still advertised `coordination.portfolio` and
`coordination.createWorkItem` "for the work board". That sentence is gone
(`main.c:131-144`): `execute_tool` now says never for the board, the tasks or
what is queued or done, that is `delegated_tasks`; `delegated_tasks` names the
phrases it answers (`main.c:202-210`); and the persona says `delegated_tasks`
alone answers anything about the board and `execute_tool` is for a method
`search_tools` found (`components/jr_core/src/tool_policy.c:82-86`). The
device-side allowlist is unchanged: `coordination` stays reachable by name.

**Tests.** `components/jr_tools/host/test_exec_budget.mjs` runs the shipped
program (read back out of the C macro) against mock services: fat lists, long
strings, deep and wide objects, multi-byte text, cycles, BigInt, and 1500 fuzzed
shapes, asserting the serialised size never exceeds the budget and that every
over-budget result is flagged. Node is not in the host-tests container, so it
runs in CI (`ci.yml`) and by hand. `scripts/test_tools_budget.py` (in
`host-tests.sh`) pins the generator against drift, the code-cap arithmetic, the
budget derivation, the wording, and the probe. The C suite
`jr_tools_template_tests` pins the oversize helpers and the largest call.
Not covered by any host test: `post_tool`'s HTTP loop itself (it needs
`esp_http_client`), the counters, and the cockpit fields; those compile in the
image build and need a live gateway to see.

**The probe.** `scripts/tools-template-probe.py TOOL [ARGS_JSON]` builds the
exact request the firmware sends (the program text is read from
`jr_tools_templates.c` itself, allowlist included), posts it to
`JARVIS_PROBE_URL` with the bearer in `JARVIS_PROBE_KEY`, and prints only the
HTTP status, the body size, whether it parses, whether it fits the device caps
(8192-byte body, 3072-byte response slot) and the top-level keys. It never
prints the URL, the key or the body. `--dry-run` builds without a network.

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
