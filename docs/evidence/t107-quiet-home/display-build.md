# QUIET-DISPLAY build receipt

Result: bounded repair complete and frozen for coordinator gates. No device,
API, privacy, main, power, SDK configuration, OTA or commit action was taken by
this executor. Listening and voice remain owner checks; this repair never
clears privacy mute.

## Cause and change

The coordinator retained B and C muted HOME/WHISPER windows with 4,235 frames
in 720 seconds (5.879 fps). The production presenter always started a repeating
MUTED/RESTING animation; its asset timer ran at 8 fps and invalidated the face
repeatedly. The WHISPER 6 fps render ceiling limited these dirty frames rather
than suppressing them. The pinned renderer already skips a cycle with no dirty
objects (`gfx_render.c` dirty-count gate), so changing a global ceiling or
stopping every animation was unnecessary.

Settled, privacy-muted HOME at the existing <=6 fps ceiling now uses a visible
one-shot frame. It retains the actual animation/parser and strip pipeline;
there is no additional framebuffer, mutex or task. Eligibility excludes busy
voice, foreground faces, WATCH and other spaces, charging, foreground/pinned
captions, choices, commit, live ripple/bloom, canvas/surface, pending navigation
and all finite fades, OTA activity, test patterns and disabled HUD. Existing
explicit Rest presentation retains its own path. Exiting quiet forces even the
same asset back through normal repeating presentation. Old fallback/error art,
missing or hidden assets and an invalid segment keep the existing bind/retry
path.

Quiet refreshes only when its displayed clock, packed HUD facts, caption bytes,
pending brightness or failed-transfer counter changes, plus the existing
explicit snapshot/panel requests. Normal HOME clock seconds remain truthful
when enabled; minute-only Rest retains its prior behavior. The standard HOME
clock is disabled by the composition root, and the existing disabled-parallax
publisher keeps packed offsets stable. Publishing unchanged values for 720
seconds causes no periodic quiet refresh. Repeated identical caption bytes
neither increment the new revision nor dirty the frame. A changed battery
percentage, clock second/minute, rim offset or caption still refreshes.

## Actual vendor semantics and failure recovery

Pinned `esp_emote_gfx` 3.0.2, repository commit
`c78aa9bec705ac67b494dffab0f8ae3c5e27a843`, prepares a frame only while
`current_frame < end_frame`. `[0,0]` stops before decoding. Stop followed by
`[0,1]`, nonrepeat, then start and visible=true decodes frame0 once, stops on the
next timer callback, and retains its visible frame/parser. These are source
and executable host proofs, not a physical panel observation.

Independent review found that the pinned callback ignored preparation errors
and advanced beyond the only quiet frame. The tracked managed patch now returns
before advancing, completion events or invalidation on preparation failure.
It retries the same frame on the next timer tick without periodic dirtying on
failure. This is generated through `scripts/patch-v5-managed.py`; the ignored
managed component is not a source artifact to commit. The patch rejects changed
or partial anchors and is idempotent.

A failed sparse panel submission is likewise not forgotten: quiet observes the
existing flush-error counter and schedules another refresh. A failed render-lock
attempt does not consume the dirty/error keys. Independent review exercised
actual preparation/reset/callback with a previously valid resident frame and an
allocation failure, and actual presenter/panel_flush submission failure.

## Verification

Isolated focused build `.evidence/t107/build-quiet-display`: **5/5 suites pass**.
Exact positive counts:

| Suite | Checks |
| --- | ---: |
| Production board transport | 72 |
| Companion | 76,940 |
| HUD renderer | 11,451 |
| Actual display shell/presenter | 3,563 |
| Extracted pinned vendor playback + actual quiet entry | 5,778 |

The vendor runner compiles the actual pinned callback/control functions and the
production quiet stop/segment/start/visibility block. A fresh checkout uses
licensed, unmodified pinned playback excerpts and the **actual tracked patch
function**, then compiles the result. Both managed-source and forced
fixture-only runs pass 5,778 checks. When managed source exists, all extracted
bodies must match the patched pinned fixture. There is no ESP-IDF requirement.
The coordinator owns adding this runner to the standard host-tests.sh gate;
CMake also registers it. This executor did not run the root gates.

The actual display task runs 721 test passes under the existing fake scheduler;
the quiet one-shot remains visible, with no periodic fact refresh beyond the
existing startup test-pattern refresh. The extracted callback then survives
5,760 timer calls across the 720-second equivalent without another successful
decode or periodic invalidation. Dynamic guards, normal resume, loader/fallback,
late-entry changes, brightness, snapshots, panel-off/on, lock failures, clock
seconds/minutes, caption revisions and actual panel submission faults are
covered. No timing threshold is asserted in a normal host suite.

Scratch proof: positive shell compiles and passes; **53/53 faults compile and
fail at runtime with assertion failures**. This includes restoring the original
periodic task path, repeated/empty/hidden one-shot playback, every dynamic
eligibility family, lost fact refresh, consuming keys after a failed lock,
losing DMA retry, caption revision regression, and restoring the vendor's ignored
prepare error. Compile failures are not counted as rejection evidence. All eight
frozen production/test hashes are verified before and after the run.

The first attempt rejected 52/53 faults: the caption guard test also had an
unfinished fade, masking removal of the caption predicate. Its NETWORK ERROR
fixture now uses a settled caption, independently proving the foreground
guard. The first result and freeze are retained; production was unchanged.

Evidence:

- `quiet-display/focused.log`, `fixture.log`, `patch-check.log`
- `quiet-display/vendor/*-managed/{test.c,compile.log,run.log,receipt.json}`
- `quiet-display/vendor/*-fixture/{test.c,compile.log,run.log,receipt.json}`
- `quiet-display/check-mutants.py`, `mutation.log`, `mutants.tsv`, `results.json`
- `quiet-display/mutants/*/{jr_display.c,compile.log,run.log}`; vendor faults use
  `gfx_anim.c` and the maintained extracted-source runner
- `quiet-display/results-first-masked.json`, `mutants-first-masked.tsv`,
  `freeze-first-masked-sha256.json`
- Independent reviewer: 3,613 actual prepare/reset/callback checks and 10 actual
  panel-flush retry checks, with two compiled runtime restoration faults. Its
  separately owned report supplies those receipts.

## Frozen artifacts

Exact byte SHA256 manifest: `quiet-display/freeze-sha256.json`.

| Owned path | SHA256 |
| --- | --- |
| components/jr_display/src/jr_display.c | 5b9d54bd530dd3ac91a59bc038076f8f104ef72ffb2199c599511c9833de4010 |
| components/jr_display/tests/probe/stubs/idf_stubs.inc | 5dad7d7de648f6e232e12d112c2eff8be870b1732c4ed801e60a2d24341f97aa |
| components/jr_display/tests/test_shell.c | c8f5e3fc2777691041b45c6b2770991d06462eddce79c412e8825ef7c5ee1738 |
| components/jr_display/tests/CMakeLists.txt | 5ed5ad959a04bfc3148c640a2faf5688ebd9545c409bba4159a1449fa3f1afe8 |
| components/jr_display/tests/test_quiet_vendor.py | 077c5305f688fec47db3ea36b30cc4524ebd2403f326f52ad7cc3eba42d0cd09 |
| components/jr_display/tests/fixtures/gfx_anim_playback_3_0_2.c | 82227e7d8e65bf7f50e0ae23b86e6dc0cf6d322bfb65626cf8e360e6979bbb5c |
| components/jr_display/tests/fixtures/LICENSE.esp_emote_gfx | cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30 |
| scripts/patch-v5-managed.py | 9c9e7d9fb55b6e695aff2494c5e7a9041c7ba7f1a0d1fa7330d4085f6fb7a7b4 |

Generated managed gfx_anim.c SHA256:
`3d021df92a439379e1b3421fe3db6d809a367a361ebbbf0174d3974d1876f034`.

## Remaining risk and next decision

Physical before/after redraw, optical appearance and battery/CPU savings remain
the coordinator's device acceptance. Host zero-periodic invalidation is not a
measured mA or battery-life claim. The engine and animation timers still check
their state; this patch reduces decode/render/DMA work, not every periodic CPU
wake. Changing honest values or leaving quiet intentionally produces frames.

Permanent corrupt-frame preparation can continue retrying; block-level decoding
errors remain an inherited vendor limitation. Independent inspection found all
16 current EAF assets place their `_C` control sentinel only at the excluded
final frame, with `_S` preceding frames, so the new callback guard does not stall
those valid assets. No scope expansion for hypothetical corrupt assets is
required by this bounded quiet repair.

Decision needed: coordinator verifies the standard fifteen-suite host gate and
target build against the final freeze, then performs the retained muted matched
device comparison. No new main API or hook is required.
