# JarvisMCP Bridge

**What it is** — The function-calling gateway between Gemini Live on-device and the JarvisMCP backend. When Gemini calls a declared function, the device dispatches it to a hosted `/act` endpoint over HTTPS with Bearer auth, receives a result, and sends a `toolResponse` back to the model.

**How we use it here** — `cap_gemini_live.c` declares function tools in the Gemini setup frame. When a `toolCall` frame arrives, `gl_run_tool_via_mcp()` POSTs `{"code": "..."}` to the configured endpoint, parses `{"ok": true, "result": "..."}`, and builds the `toolResponse`. The endpoint URL and Bearer key are stored in NVS only — never in source.

---

## Security rule

**No secrets in source or reference pages.** The `/act` endpoint URL and Bearer key must NEVER appear in source code, config files committed to this repo, or any reference page. They live in NVS, set via `POST /api/config` with fields `jarvis_mcp_url` and `jarvis_mcp_key` (group `jarvis`). If you see a URL or token in source, it is a security incident.

---

## Findings & gotchas

**[2026-05-21] A new config field must be registered in THREE places**

The NVS config system requires registration in exactly three files. Missing the third causes POST to return `applied: N` silently — the value appears accepted but is never saved to NVS, so readback is always empty.

The three required registrations:

1. **`app_config.h`** — add the field to the `app_config_t` struct:
   ```c
   // esp-claw/application/edge_agent/components/app_config/include/app_config.h:48-49
   char jarvis_mcp_key[APP_CONFIG_STR_LEN];
   char jarvis_mcp_url[APP_CONFIG_STR_LEN];
   ```

2. **`app_config.c` — `s_fields[]` table** — add an `APP_CONFIG_FIELD` macro entry (the NVS save/load table):
   ```c
   // app_config.c:77-78
   APP_CONFIG_FIELD(jarvis_mcp_key, "jarvis_mcp_key", APP_DEFAULT_JARVIS_MCP_KEY),
   APP_CONFIG_FIELD(jarvis_mcp_url, "jarvis_mcp_url", APP_DEFAULT_JARVIS_MCP_URL),
   ```

3. **`http_server_config_api.c` — `CONFIG_FIELDS[]` catalogue** — add a `CONFIG_FIELD` macro entry (the HTTP API exposure table):
   ```c
   // http_server_config_api.c:66-67
   CONFIG_FIELD("jarvis", jarvis_mcp_key),
   CONFIG_FIELD("jarvis", jarvis_mcp_url),
   ```

If you miss `s_fields[]` (step 2), the HTTP POST accepts the field name but `settings_store_set_string` is never called — NVS is unchanged. If you miss `CONFIG_FIELDS[]` (step 3), the field is not exposed via the HTTP API at all.

Source: `app_config.h:48-49`, `app_config.c:77-78`, `http_server_config_api.c:66-67` (all verified).

**[2026-05-21] NVS key names must be 15 characters or fewer**

ESP-IDF's NVS key name limit is `NVS_KEY_NAME_MAX_SIZE` = 15 characters (null-terminated). Keys `"jarvis_mcp_key"` and `"jarvis_mcp_url"` are each 14 characters — within the limit. If you add a new field with a longer key, `nvs_set_str` will return `ESP_ERR_NVS_KEY_TOO_LONG` and the write will silently fail in some IDF versions.

Source: `cap_scheduler_store.c:93` (`char key[NVS_KEY_NAME_MAX_SIZE]` usage pattern); ESP-IDF NVS docs.

**[2026-05-21] Gateway pattern: `executeInSandbox` call and response**

The dispatch flow:
1. Gemini sends `toolCall` with `{"toolCall": {"functionCalls": [{"id": "...", "name": "...", "args": {...}}]}}`.
2. Device POSTs `{"code": "<generated code string>"}` to the `/act` endpoint with `Authorization: Bearer <key>`.
3. Gateway runs `executeInSandbox({code, mode: "execute"})`.
4. Gateway returns `{"ok": true, "result": "..."}` (or `{"ok": false, ...}`).
5. Device builds `toolResponse` with the result and sends it back to Gemini.

Source: `cap_gemini_live.c:614-676` (gateway call and toolResponse assembly).

**[2026-05-21] `gl_run_tool_via_mcp` is blocking — model is paused during execution**

The gateway HTTP call in `gl_run_tool_via_mcp` is blocking. While it is in flight, the Gemini session is paused (the model waits for the tool response). This is acceptable for short tool calls but will cause perceived latency for slow network operations.

Source: `cap_gemini_live.c:614` (comment: "Blocking — only called from a toolCall (model is paused)").

**[2026-05-21] Gateway fields are logged at startup as unconfigured if NVS is empty**

If `jarvis_mcp_url` is empty in NVS, `gl_run_tool_via_mcp` logs a warning and returns a failure response without attempting the HTTP call. The session continues; the tool call returns an error result to the model.

Source: `cap_gemini_live.c:618` (warning log: "JarvisMCP not configured (set jarvis_mcp_url + jarvis_mcp_key via /api/config)").

---

## Primary sources

| Source | Notes |
|--------|-------|
| `firmware/components/cap_gemini_live/src/cap_gemini_live.c:614-676` | Gateway call, toolCall handler, toolResponse assembly. |
| `esp-claw/application/edge_agent/components/app_config/include/app_config.h:48-49` | `jarvis_mcp_key` and `jarvis_mcp_url` field declarations. |
| `esp-claw/application/edge_agent/components/app_config/app_config.c:77-78` | `s_fields[]` NVS save/load table entries. |
| `esp-claw/application/edge_agent/components/http_server/http_server_config_api.c:66-67` | `CONFIG_FIELDS[]` HTTP API catalogue entries. |

---

## Open questions

- Should `gl_run_tool_via_mcp` be made async (enqueue the HTTP call, respond to Gemini when it completes) to reduce session latency?
- Is there a timeout configured for the gateway HTTP call? What happens if the gateway is unreachable?
- Should the gateway URL and key be cached in RAM at session start to avoid repeated NVS reads during a live call?

---

## See also

- [gemini-live-api.md](./gemini-live-api.md) — tool call frame structure (`toolCall` / `toolResponse`).
- [llm-config.md](./llm-config.md) — NVS config system; the same three-file pattern applies to all new fields.
- [build-toolchain.md](./build-toolchain.md) — NVS namespace, `settings_store` component.
