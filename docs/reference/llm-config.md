# LLM Configuration

**What it is** — The NVS-backed runtime configuration that controls which LLM backend the `esp-claw` agent connects to. Fields are set via `POST /api/config` on the on-device HTTP server and persist across reboots in the `app` NVS namespace.

**How we use it here** — When Gemini Live (`cap_gemini_live`) is active, these fields select the fallback text-mode LLM for non-voice interactions. The `llm_profile` field selects the wire protocol; the other fields configure the backend URL, model name, and auth.

---

## Findings & gotchas

**[2026-05-21] `llm_profile` is a PROTOCOL enum — not a vendor name**

`llm_profile` selects the wire protocol, not the vendor. Valid values:
- `"anthropic"` — Anthropic Messages API
- `"openai"` — OpenAI Chat Completions API
- `"qwen"` — Qwen-native API
- `"qwen_compatible"` — Qwen-compatible mode

Setting `llm_profile` to a vendor name (e.g. `"minimax"`, `"gemini"`, `"mistral"`) causes `claw_llm_profile_find()` to return NULL. `app_claw_start()` calls `abort()` and the device enters a 15-second boot loop — dashboard comes up for ~1 second per cycle, too short to POST a fix reliably.

Source: `esp-claw/application/edge_agent/components/app_config/app_config.c:142-154` (profile lookup); `claw_llm_runtime.c` (registered profiles). Valid values verified from memory file `feedback_llm_profile_enum.md`.

**[2026-05-21] Vendor identity goes in `llm_base_url` and `llm_model`**

To use a vendor that exposes an Anthropic-compatible API (e.g. MiniMax):
```
llm_profile:       "anthropic"              (the protocol)
llm_backend_type:  "anthropic"              (matching the protocol family)
llm_base_url:      https://<vendor>/anthropic/v1   (the compat host + /v1 — without /v1 gives 404)
llm_model:         "<vendor model name>"    (the model the vendor exposes through the compat layer)
llm_auth_type:     "bearer"
```

The `/v1` suffix on `llm_base_url` is required — without it the vendor returns 404.

Source: memory file `feedback_llm_profile_enum.md`.

**[2026-05-21] Boot-loop recovery: erase the NVS partition**

If the board is in a boot loop caused by a bad `llm_profile` value (or any other bad NVS config):

```bash
esptool.py erase_region 0x9000 0x6000
```

This erases the NVS partition (default address `0x9000`, size `0x6000` = 24 KB). The board will boot clean with factory defaults. You will need to re-provision Wi-Fi via the USB CDC CLI (`wifi --set --ssid X --password Y --apply`) and then re-POST your config.

The dashboard comes up for ~1 second per cycle during the boot loop. If you can catch the window, POST the corrected config. If you cannot, erase NVS.

Source: memory file `feedback_llm_profile_enum.md`.

**[2026-05-21] Legacy `llm_provider` field is auto-migrated on load**

`app_config.c::app_config_apply_legacy_llm_profile()` reads the old NVS key `"llm_provider"` and maps it to the correct `llm_profile` value on first load:
- `"qwen"` → `"qwen_compatible"`
- `"deepseek"` → `"custom_openai_compatible"`
- `"openai"` → `"openai"`

Source: `app_config.c:103-132` (migration function).

**[2026-05-21] NVS key length limit: 15 characters**

All NVS key names must be 15 characters or fewer (`NVS_KEY_NAME_MAX_SIZE`). The existing keys are all within limit (see [jarvismcp-bridge.md](./jarvismcp-bridge.md) for length table). When adding new config fields, verify the key string length before registering.

---

## Config field reference

| Field | NVS key | Notes |
|-------|---------|-------|
| `llm_api_key` | `"llm_api_key"` | API key for the text LLM backend |
| `llm_backend_type` | `"llm_backend"` | Protocol family (match `llm_profile`) |
| `llm_profile` | `"llm_profile"` | Protocol enum: `anthropic`/`openai`/`qwen`/`qwen_compatible` |
| `llm_model` | `"llm_model"` | Model name string |
| `llm_base_url` | `"llm_base_url"` | Backend URL (include `/v1` suffix) |
| `llm_auth_type` | `"llm_auth_type"` | Auth type (e.g. `"bearer"`) |
| `gemini_api_key` | `"gemini_key"` | Gemini API key (NVS key is `"gemini_key"`, 10 chars) |
| `jarvis_mcp_key` | `"jarvis_mcp_key"` | JarvisMCP Bearer token |
| `jarvis_mcp_url` | `"jarvis_mcp_url"` | JarvisMCP `/act` endpoint URL |

Source: `app_config.c:51-78` (full `s_fields[]` table, verified).

---

## Primary sources

| Source | Notes |
|--------|-------|
| `esp-claw/application/edge_agent/components/app_config/include/app_config.h` | `app_config_t` struct — all field declarations. |
| `esp-claw/application/edge_agent/components/app_config/app_config.c` | `s_fields[]` NVS table, legacy migration, load/save logic. |
| `esp-claw/application/edge_agent/components/http_server/http_server_config_api.c` | `CONFIG_FIELDS[]` — HTTP API exposure. |

---

## Open questions

- Are there additional valid `llm_profile` values beyond the four listed? The `claw_llm_runtime.c` registered-profiles list should be checked if adding a new backend.
- Is `"custom_openai_compatible"` a valid `llm_profile` value (referenced in the legacy migration for `"deepseek"`) or is it a legacy artifact?

---

## See also

- [jarvismcp-bridge.md](./jarvismcp-bridge.md) — three-file registration pattern for new config fields; JarvisMCP-specific fields.
- [build-toolchain.md](./build-toolchain.md) — NVS recovery, `settings_store` component.
- [gemini-live-api.md](./gemini-live-api.md) — `gemini_api_key` usage.
