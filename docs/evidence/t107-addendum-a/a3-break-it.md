# A/w3/break-it — frozen display/main review

Parent: t107/addendum-a. Executor: a1_code. Branch: t107/codex, HEAD 6f97afb. Scope: current main input policy, jr_display changes, display fault tests, persistent main input test runner. Read-only except this report. No source edits, device calls, MCP, gate execution or commits in this audit. The separate proposed HUD battery-rim performance change is excluded and needs its own follow-up.

## Result

No actionable defect found that invalidates the frozen fixes. The new compressed-dial release is safe under the actual vendored EAF/animation ownership contract, including decode, lock and replacement failures. Snapshot/pattern/DREAM retry state is acknowledged only after refresh submission. The main changes preserve physical privacy controls and leave probation, lease, transport and sleep policy intact.

This is source and existing-evidence verification. It does not claim real-device wake timing, Wi-Fi-loss recovery or OTA acceptance. Parent owns those gates and post-change proof. Voice remains owner check pending with privacy muted.

## Evidence: compressed source lifetime

Actual vendor sources inspected, rather than inferred from stubs:

- `managed_components/espressif2022__esp_emote_gfx/src/lib/eaf/gfx_eaf_dec.c:705`: `eaf_init` allocates its parser/entries, but each entry's table and frame memory point into caller `data` (`:741-742`). It does not copy the compressed source.
- `gfx_eaf_dec.c:767`: `eaf_deinit` frees entries and parser only. `:793` returns the borrowed frame memory. The compressed allocation remains caller-owned throughout.
- `managed_components/espressif2022__esp_emote_gfx/src/widget/gfx_anim.c:230`: prepared animation frame data is borrowed from that parser. `gfx_anim_stop` at `:1063-1080` only changes `is_playing`; it does not release the parser or source. Hiding likewise cannot justify freeing a bound clip.
- `gfx_anim.c:944-994`: source replacement stops playback and clears old frame-data/header references. It parses the new source first. Parse failure returns while retaining the old `file_desc`; successful replacement deinitializes the old parser at `:981`, binds the new parser and returns success.
- `components/jr_display/src/jr_display.c:5300`: the only production call to `gfx_anim_set_src` is in `apply_face`. `ctx->bound` changes only after success, while the render mutex remains held; only then can the previous baked source be freed (`:5302-5305`). No other binding path bypasses this tracking.
- `jr_display.c:5158-5173`: release is restricted to four baked-dial entries and explicitly rejects `clip == ctx->bound`. Fast voice clips remain resident. Release zeroes the freed cache entry, preventing stale source reuse.
- `jr_display.c:5111-5152`: the temporary parser used by the independent RGB565 decode is deinitialized before `dial_show` can release compressed data. Decode is done into the separately allocated shared frame. Failed decode/lock does not call the release helper.
- `jr_display.c:5189-5217`: successful decode/hide frees only an unbound dial. The same decoded face subsequently skips compressed reload/decode; a bound dial stays allocated even with `ctx->active == NULL`.

Ownership case review:

| Case | Source lifetime | Retry behavior |
|---|---|---|
| Compressed load fails | `clip_load` frees only its unsuccessful new allocation; caller invokes it only with `data == NULL` (`:310-367`, `:5191`, `:5264`) | Existing asset/fallback policy remains |
| Decode lock fails | Bound source untouched; requested compressed data retained; existing decoded frame preserved | Falls back to animation or retries |
| Decode fails | Temporary parser deinitialized; compressed data retained | Animation fallback can bind retained source |
| Hide lock fails after decode | Both shared frame and compressed source retained | Next attempt can use the decoded frame |
| Source-rebind lock fails | Old parser and bound source untouched; new cached source retained | Apply error retains retry word |
| Vendor source parse fails | Old parser remains bound; caller leaves `ctx->bound` unchanged and frees neither source | Existing recovery branch/500 ms apply retry remains |
| Source replacement succeeds | Old vendor parser destroyed under lock before old unbound dial free | New source tracked before unlock |
| A hidden dial remains vendor-bound | Stop/hide retain parser, and `bound` prevents release | Later successful replacement releases it |

There is no early-free path for a compressed source still reachable by a vendor parser. Vendor failed replacement is not a fully transactional visual operation: it stops playback and clears frame/header state before failing. The host stub models retained ownership, not all those visual side effects. This limitation predates the patch and does not invalidate its lifetime proof.

## Evidence: retries, fades and wake

- `jr_display.c:4979-5015`: failed refresh returns false. An exchanged snapshot request is restored only when needed; concurrent snapshot stores of 1 coalesce safely. A request arriving after exchange survives on success because success does not clear the flag again. Pattern changes latch only on success; a concurrent newer pattern therefore remains different on the next pass.
- `jr_display.c:5018-5047`: failed DREAM refresh preserves both clock and rim keys. Entry still uses successful `apply_blank`, and DREAM exit resets the face latch and retry gate so the same previous face is restored.
- `jr_display.c:555`, `:569`, `:1058-1140`: elapsed UI time is capped at the longest finite fade, 400 ms, keeping the integer products bounded. Caption/sheet lanes use the same elapsed delta with their 250 ms denominator. Physical brightness independently retains its 100 ms slew cap. A 500 ms rest tick finishes finite fades; it no longer stretches them over several seconds.
- `jr_display.c:2665-2679`, `:5836-5843`, `:5937-5938`: DREAM changes and real navigation wake the presenter through task notification. The presenter waits on `ulTaskNotifyTake` (`:5435`), so its 1000 ms quiet-DREAM wait does not add a fixed second to a notified touch. Existing no-op navigation remains intentionally idempotent.
- `main/main.c:1596-1640`: helpers only select existing display navigation and captions. DOWN on DETAIL closes the sheet without walking the underlying panel. Outside-sheet DISMISS names that panel, with WATCH clearing the caption. Existing shade close and normal UP/ring behavior remain represented in persistent tests.

Existing display evidence read back:

- `.evidence/t107a/w2-display-focused.log`: actual combined shell suite passed **951 checks**.
- `.evidence/t107a/w2-display-mutations.log`: control exit 0; all ten mutations exit 1. They cover byte order, elapsed finite fade, brightness slew cap, DREAM/snapshot/pattern retry, retained dial allocation, bound-source free, unnecessary reload and untracked binding. The frozen host tests contain lock-hook snapshot interleaving (`test_shell.c:3126-3183`), rest/stall fade cases (`:3186-3250`) and three cycles across all baked styles plus decode/hide/rebind failure cases (`:3262-3349`).

## Evidence: privacy, probation and Wi-Fi

- `git diff -- main/main.c` changes only the three shell helpers and their existing navigation/caption/DISMISS dispatch sites. It adds no privacy clear, transport call, resource owner, app seam or lease mutation.
- `main/main.c:717` defaults privacy to muted. Synthetic hold is rejected at `:3481-3484`; physical privacy actions retain their separate guards. New shell helpers have no voice control or privacy write.
- `main/main.c:2661-2668`: privacy-safe RESUME still rejects a muted/face-down device. `:3731-3735` rechecks privacy after the blocking mic read, and `:3846-3856` rechecks immediately before the transport handoff. The new input helpers cannot bypass those gates.
- `main/http_routes.c:3239-3257`: OTA restore remains fail-closed (`was_private || current_private`) and restores only the prior still-live lease. No change in this patch.
- `main/main.c:2088-2098`, `main/http_routes.c:3275-3326`: probation starts checking at 45 s and still requires live voice task, Wi-Fi, HTTP/tool/wake readiness, display progress/FPS and a stable error window. Wi-Fi loss prevents confirmation and resets stability; deadline rollback remains.
- `main/power.c:84-104`: pending-verify images cannot enter deep sleep. `main/main.c:2325`, `:2399` keeps OTA/probation out of DREAM/deep-sleep paths. The reviewed fixes do not alter these guards or CPU/Wi-Fi policy.
- `main/main.c:1252-1293`: transport CLOSED/ERROR still produces the orchestrator's transport-closed event. Upload batching requires an open transport and the same privacy gates. No new routing path initiates or resumes a connection.

## Persistent test runner safety and coverage

`main/tests/test_shell_input.py:19-35` extracts current production static helper bodies, ignoring braces inside comments/literals. `:38-60` rejects input dispatch that bypasses those helpers; missing markers or signatures fail rather than silently skipping. `:66-86` applies mutations to generated helper text only. Compilation uses a subprocess argument array, without shell evaluation (`:96-109`). Default artifacts live in a `TemporaryDirectory` (`:123-124`); retained output is written only when explicitly requested. No device/credential/network access or production source mutation is present.

`main/tests/test_shell_input.c.in` includes the actual display test translation unit and its real display implementation, rather than a replacement navigation model. It tests 109 checks: every relevant detail panel, outside-sheet hit/dismiss, DESK active/inactive rings, previous UP behavior and both shade doors. Existing receipt `.evidence/t107a/a2-ux-result.md` and its referenced persistent output record a 109-check positive, 15/5 expected restored-defect failures, and five dispatch-guard checks. These are prior executor results read in this audit; no redundant run was made.

`scripts/host-tests.sh:134-150` hooks the new suite into the same native/Docker C-suite path. `finish_c` at `:87-104` requires both exit 0 and a positive count. A missing Python/compiler/helper/template causes failure. Main input test defaults create no lasting build directory. The existing platform-CMake cache cleanup is unchanged by this addition.

Coverage is deliberately narrower than the complete RTOS loop: actual helpers and dispatch shape are tested, while touch-driver ordering, vendor timer scheduling, codec latency and physical flush are not simulated. Display ownership tests do not execute the vendor EAF parser, so the source ownership proof above is required alongside the stub tests. The concurrent snapshot test explicitly injects a request during a failed lock; success-interleaving is proved by the atomic flag algorithm but has no separately injected success test. None of these gaps establishes a defect in this patch. Parent full gate, firmware build, muted post-OTA capture and wake proof remain the acceptance checks.

## Reviewed snapshot

SHA-256 of frozen paths at audit time:

| Path | SHA-256 |
|---|---|
| main/main.c | 4C73619864D5C228D3F0476E418C830D7623A2CF3BC4BB3A13C7BCB07D8836B5 |
| components/jr_display/src/jr_display.c | 9C67C8FCCF24E051D9AF1A305B28E6A47E8A85062C33174478ED4A0B13625691 |
| components/jr_display/tests/test_shell.c | FC6AB56387EE8306945F5A9D939CC4A1314847083376AAC26D36099E6CE5D423 |
| components/jr_display/tests/probe/stubs/idf_stubs.inc | 492EEC185533B4B92BCD9C03563F8EC13689BB65A3B0646E0B0E58006ED0798A |
| main/tests/test_shell_input.py | BD62075D09F5D25DB1505582BF735B90D22AA0A6BD86B334AF38E965FCB6FC7B |
| main/tests/test_shell_input.c.in | 47465AE7F921EFCE3C99E0ABA04EC6C36F86BDD6E6EDA5698F3648086A55BE78 |
| scripts/host-tests.sh | 9760AC7815E0C60D735832B2D5FAD8D4A800737FCF6D8A2D3FF3100A4A13A736 |

## Changed artifacts, remaining risk and decision

Changed artifact: this evidence report only. No source or register edits.

Remaining risk: final hardware acceptance has not run in this subtask, and the excluded HUD performance candidate needs separate review against its final diff. Prior shell fault/mutation evidence covers the reviewed source; parent must run the prescribed combined six-suite gate and firmware headroom/OTA/probation/capture/power proof after the final source freeze. Voice remains owner check pending.

Decision needed: none for this frozen fix set. Proceed to parent gates; do not treat this audit as OTA or voice completion.
