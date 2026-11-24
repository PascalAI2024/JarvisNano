# QUIET-REVIEW — independent frozen-source review

Result: **PASS, offline pre-deployment review.** The initial change had two recoverability defects. The display owner repaired both; unchanged production-function probes and compiled runtime mutations verify the repairs. No further blocking source defect was found in the bounded quiet-display paths. This report does not certify physical panel output, battery saving, deployment, or natural sleep acceptance.

Parent correlation: `t107/codex`, task register `.evidence/t107a/task-register.md`, row `QUIET-REVIEW`. Reviewer: `a1_ux`. Authority: read-only source, isolated focused tests, private evidence only. No device, network, MCP, commit, full gate, or shared-source write was performed by this reviewer. Repair source belongs to `w2_power`.

## Defects found and closed

### 1. A failed first decode could leave the one-shot permanently unprepared

Original severity: **P1** for the new one-shot path. An already prepared resident frame does not protect quiet entry. `gfx_anim_prepare_frame` resets/frees the previous resources at `managed_components/espressif2022__esp_emote_gfx/src/widget/gfx_anim.c:224` before allocating the new block offsets at line 246. The original timer callback ignored its return, advanced from frame 0 to frame 1, published completion/dirty pixels, and stopped on the following tick. Drawing then requires resources which the failed prepare freed (`gfx_anim.c:647`, `:662`). Later clock or snapshot invalidation redraws; it does not restart a stopped decoder.

Repair reviewed: the pinned managed patch in `scripts/patch-v5-managed.py:108` now returns from the callback before advance, completion, or invalidation when prepare fails. The current managed callback implements it at `gfx_anim.c:859`. It stays playing at the same frame and retries on the next timer tick. The patch transform rejects missing, duplicate, and partial anchors; the persistent vendor test compares the actual managed playback function bodies with the patched pinned licensed fixture.

Independent runtime proof: extracted **unchanged actual** `free_frame_buffers`, `gfx_anim_reset_frame`, `gfx_anim_prepare_frame`, and `gfx_anim_timer_callback`; fake EAF, allocator, geometry, and invalidation ports. Start with a valid resident frame, fail the first allocation during reprepare, verify that frame 0 remains playing with no invalidation, clear the fault, and verify that the next callback restores the resources and completes the visible one-shot. The prepared frame remains resident through 1,200 later refresh/timer ticks. Repeat-mode recovery and a successful one-shot control also pass. **3,613 checks.**

Compiled scratch mutation: remove only the new prepare-return guard. Compilation succeeds and the executable prints its runtime start marker, then exits 1 on `prepares==before+1 && anim.current_frame==0 && anim.is_playing`. This is runtime regression detection. The earlier const-fixture compile failure is not counted as mutation evidence.

### 2. Failed DMA could consume the only sparse fact refresh

Original severity: **P2** for a quiet face with no subsequent visible fact changes. Quiet fact keys were consumed when invalidation succeeded, before panel submission completed. Vendor render cleanup clears dirty rectangles (`managed_components/espressif2022__esp_emote_gfx/src/core/gfx_render.c:224`); a failed submission does not preserve them. With an inactive clock and unchanged facts, a failed refresh could therefore remain stale until another independent refresh.

Repair reviewed: `jr_display.c:5513` samples `flush_errors`; `:5543` includes it in the quiet refresh keys. Keys update only after a successful refresh request. A failed frame therefore requests another refresh on the following quiet poll. A graphics-lock refusal does not consume this retry.

Independent runtime proof: include the actual production renderer through the existing `test_shell.c` fake environment. Enter a one-shot HOME face; change a battery fact; force the actual `panel_flush` hardware port to return an error; restore it; verify a second refresh with identical visible facts and then no unnecessary refresh. A second failure plus graphics-lock refusal retains the retry until the lock recovers. The one-shot face is not reprogrammed. **10 checks.**

Compiled scratch mutation: remove only `ctx->quiet_flush_error_key != flush_errors ||`. Compilation succeeds; the executable prints its runtime start marker and exits 1 with four failed recovery assertions. The mutant changes a private source copy only.

## Other paths reviewed

| Path | Evidence and conclusion |
|---|---|
| Retained face | Vendor callback tests `current_frame >= end_frame` before prepare (`gfx_anim.c:840`), so nonrepeating `[0,1]` decodes frame 0 once and stops while retaining visibility/resources. `[0,0]` stops before decoding. Stop does not hide or free. Quiet uses `[0,1]` at `jr_display.c:5527`. |
| Wrong or hidden asset | Quiet requires a loaded clip with at least one drawable frame, matching requested face, and an unblanked context (`jr_display.c:5498`). Binding/loading remains on the existing normal `apply_face` path. Blank clears the quiet latch (`:5334`). Dial display clears the active clip and cannot accidentally freeze an old emote. Missing-art retries remain on the normal binding path. |
| Exit to normal rendering | Loss of eligibility clears the latch, invalidates the remembered segment/request, and resets the apply retry gate (`jr_display.c:5500`). This restarts even the same asset. A failed entry falls back to the normal repeating segment. The graphics lock rechecks eligibility and the request before programming entry (`:5516`). |
| Privacy and busy work | Eligibility requires privacy muted and excludes charge animation, voice busy, companion modes, higher cadence, blank, foreground faces, choices, commit activity, canvases/surfaces, pinned/error captions, OTA ringing, and test patterns (`jr_display.c:4645`). All related shell guard cases pass. Quiet reads privacy; it never clears it. |
| Navigation and animation | HOME with settled navigation only. WATCH/WEATHER and other spaces, shell agent/shade, overlay transitions, fade/ease state, ripples and blooms all veto entry (`jr_display.c:4661`). Notifications cause the presenter to re-evaluate these guards. Existing render-loop lock/yield behavior remains in place. |
| Clock, rims, captions | Quiet compares the full clock word, environment word, caption revision, failed-flush count, and brightness pending state (`jr_display.c:5510`). Actual second/minute updates and battery/tilt facts refresh. Caption byte changes or clear advance revision; identical active text does not dirty the face. Pinned/foreground captions resume normal rendering. An enabled clock should cause approximately one dirty update per second; zero is not the expected result. |
| Brightness | Pending brightness prevents the long quiet presenter wait (`jr_display.c:5349`) and causes a refresh. This does not change the previously established boot or cap policy. |
| Snapshots and panel state | Pending snapshot refresh is handled before quiet selection (`jr_display.c:5929`); lock refusal requeues the refresh. Snapshot acceptance requires a new frame/flush after the request (`:6095`). Failed strips do not publish successful mirrors, and the new failure key forces another quiet refresh. A mirror proves submitted bytes, not optical output. |
| Assets and terminal frames | Independently parsed all **16 current EAF files / 195 table entries**. Every preceding drawable frame is `_S`; `_C` occurs only at the terminal excluded end. The retry guard therefore does not stall a valid current terminal sentinel. |

## Focused checks and binding

The final shell executable was configured and compiled with its real component CMake target in an isolated `/tmp` build directory. Cached host image only; `--network none`, `--read-only`, read-only worktree mount. Result: **all shell tests passed (3563 checks)**. No shared worktree build directory was used.

The shell suite includes the actual quiet presenter and renderer through external fakes: unchanged quiet polling, fact/clock/caption/brightness updates, snapshot/on/off refresh, guard matrix, late busy state, lock refusal, segment failure, asset mismatch, blank exit, normal re-entry, and failed-DMA retry. Its fake animation port alone does not establish vendor decode semantics; the independent actual-prepare proof above covers that gap.

The owner mutation pass exposed a masked error-caption test: its unsettled caption fade also vetoed quiet, so removing the foreground-caption predicate could survive that case. The owner settled `s_caption_prog` to 256 for `NETWORK ERROR` (`test_shell.c:4161`). This test-only repair isolates the intended guard. This reviewer reran the frozen focused shell and actual flush proof after that change; production source and counts are unchanged. The owner's broader mutation run belongs to its own report.

All eight owner freeze hashes matched **before and after** the final focused run:

| Frozen path | SHA-256 |
|---|---|
| `components/jr_display/src/jr_display.c` | `5b9d54bd530dd3ac91a59bc038076f8f104ef72ffb2199c599511c9833de4010` |
| `components/jr_display/tests/probe/stubs/idf_stubs.inc` | `5dad7d7de648f6e232e12d112c2eff8be870b1732c4ed801e60a2d24341f97aa` |
| `components/jr_display/tests/test_shell.c` | `c8f5e3fc2777691041b45c6b2770991d06462eddce79c412e8825ef7c5ee1738` |
| `components/jr_display/tests/CMakeLists.txt` | `5ed5ad959a04bfc3148c640a2faf5688ebd9545c409bba4159a1449fa3f1afe8` |
| `components/jr_display/tests/test_quiet_vendor.py` | `077c5305f688fec47db3ea36b30cc4524ebd2403f326f52ad7cc3eba42d0cd09` |
| `components/jr_display/tests/fixtures/gfx_anim_playback_3_0_2.c` | `82227e7d8e65bf7f50e0ae23b86e6dc0cf6d322bfb65626cf8e360e6979bbb5c` |
| `components/jr_display/tests/fixtures/LICENSE.esp_emote_gfx` | `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` |
| `scripts/patch-v5-managed.py` | `9c9e7d9fb55b6e695aff2494c5e7a9041c7ba7f1a0d1fa7330d4085f6fb7a7b4` |

Actual managed animation source SHA-256: `3d021df92a439379e1b3421fe3db6d809a367a361ebbbf0174d3974d1876f034`. The vendor receipt also records each extracted unchanged function hash. Remaining vendor source hashes and each current EAF hash are in the final-check receipt.

## Remaining risk and measurement limits

1. A permanent corrupt drawable frame or sustained allocation failure now keeps retrying rather than advancing. Current assets' frame table/header compatibility passes; these probes establish transient recovery, not a permanent-corruption fallback or full pixel decoder validation.
2. Quiet stops repeated decode/dirty work; it does not remove the vendor animation timer or render scheduler wakeups. Vendor `actual_fps` is sampled on **every render-handler invocation before** the no-dirty early return (`gfx_render.c:244` versus `:267`). `jr_display.c:5991` publishes that scheduler cadence. A healthy quiet device can still report roughly 6 there while actual rendered/flush deltas approach zero. Use `render_frames` and successful flush deltas over elapsed time for live rendered cadence, and retain energy measurement as a separate acceptance check.
3. Host fake ports cannot establish physical panel visibility, optical stale pixels, battery saving, wake latency, natural ladder timing, or owner voice behavior. Parent full gates, reproducible firmware build, OTA validity/readback, maintained-HOME crossover and separate untouched natural ladder remain required by their own tasks. Privacy remains muted; voice remains an owner check.

## Changed artifacts and decision

Reviewer-created artifacts only, under `.evidence/t107d/`:

- `quiet-review.md` — this report.
- `quiet-vendor-failure-review.py`, generated positive/mutant C files, and `quiet-vendor-failure.receipt.json` — actual prepare/reset/callback extraction and runtime fault proof.
- `quiet-flush-failure-review.py`, generated C, private `quiet-flush-source-mutant/jr_display.c`, and `quiet-flush-failure.receipt.json` — actual quiet presenter/panel-flush retry proof.
- `quiet-review-final-check.py` and `quiet-review-final-check.receipt.json` — frozen source verification, isolated shell command/result, and current asset compatibility/hash audit.

Decision needed: parent may close `QUIET-REVIEW` after opening this report and proceed through its existing combined-gate/build/live acceptance sequence. No additional source repair or user decision is requested by this review. This conclusion binds only to the eight hashes above and the actual vendor hash; later source changes require review of the affected paths.
