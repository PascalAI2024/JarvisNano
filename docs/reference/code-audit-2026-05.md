# Code Audit — 2026-05-28

Full-subsystem audit of the firmware (voice/audio, display/face/touch, brain/logger/http, build/CI/docs). Four parallel read-only passes, then every flagged item verified against the actual source before any edit.

## Headline

The firmware is already defensively coded. The large majority of auto-flagged "CRITICAL/HIGH" findings were **false positives** — the guard the auditor asked for was already present. Three genuine, low-risk improvements were applied; nothing else warranted touching a hardware-verified codebase.

## Verified false positives (no change needed)

- `http_server_touch_api.c:111` — `snprintf(buf, sizeof(buf), "%s", scene)` is bounds-safe; not an overflow.
- `jarvis_logger_http.c:25,31` — `qlen < 128` against a 128-byte buffer is correct; negative `tail` already rejected by `if (v > 0)`.
- `http_server_display_api.c:107` — `path_len <= 0 || (size_t)path_len >= sizeof(...)` already guards the snprintf-return overflow.
- `cap_gemini_live.c:1452,1462` — oversized WS frames rejected on first fragment; BINARY frames are null-terminated before `cJSON_Parse`. No buffer overflow, no UAF.
- `cap_gemini_live.c` gain math — `int16 * 6` fits int32 with ~3 orders of magnitude of headroom; soft-knee + clamp catch peaks. Not an integer overflow.
- `reactive_face.c:506` — `end == clip->last` is permitted by the author's own clamp at line 426; intentional, not out-of-bounds.
- `app_claw_face_bridge.c` `FACE_MIC_GAIN 12→3` — intentional and consistent with the TX path now applying 6× digital gain. Left as-is.

## Changes applied

1. **`reactive_face.c` `rwave_target_amp()`** — snapshot `amp_cb` into a local before the check-then-call. Closes a (theoretical) TOCTOU where `emote_face_set_amplitude_source()` could clear the pointer between the null-check and the call. Zero behavioural change in normal operation.

2. **`scripts/check-patches.sh`** — recognise payload-free, bootstrap-managed patches (0020/0021/0022/0031/0032) as informational instead of failing them. The script previously exited 1 on a legitimate tree because the "bootstrap-managed" check sat *after* the "missing diff payload" early-exit.

3. **`.github/workflows/ci.yml`** — added two verified-green jobs alongside the existing leak check: `patch-check` (runs the fixed `check-patches.sh`) and `dashboard-js` (lints inline `<script>` blocks via the existing `check-dashboard-js.mjs`). A shellcheck job was prototyped but dropped — couldn't be confirmed green in this environment, and a CI gate that might be red on legitimate state is worse than no gate.

## Open recommendations (not actioned — your call)

- **CI firmware build job.** The biggest real gap: nothing in CI catches version-pin drift (`idf-component-manager==2.4.10`, `esp-bmgr-assist==0.5.0`) or patch-application failures against a new upstream. A gated Docker `bootstrap.sh build` + `smoke-build.sh` on `main` would catch these before a developer does. Heavy/slow, hence left as a decision.
- **`audio_level` / brain HTTP handlers** use `xSemaphoreTake(..., portMAX_DELAY)`. Safe today (the writer only holds the lock for a handful of scalar assignments, never across I/O), so not a real deadlock — but a bounded timeout would be cheap insurance if those critical sections ever grow.
- **`waveform.c`** is disabled dead code (superseded by `reactive_face.c`); consider archiving it out of the build to cut confusion.
- **3 unpushed commits** on `main` are all hardware-verified with good messages — safe to push.

## What was clean

No secrets, keys, internal URLs, IPs, or MACs anywhere in the tree. `.gitignore` is comprehensive. Docs/reference is current and well-linked. No double-free / UAF / leaks found in the brain, logger, or HTTP layers.
