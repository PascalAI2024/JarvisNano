# Addendum A power executor — frozen display fixes and render reduction

Parent work item: t107a wave 2 power/display. Executor: /root/w2_power.
Device, MCP, baseline, OTA, final gates and commit remain parent-owned.
Privacy remains muted; listening and spoken voice remain owner check pending.

## Result

The selected display correctness, memory and render fixes are implemented and frozen.
They advance finite UI fades using actual elapsed frame time up to the longest
finite fade, retain the brightness slew's existing 100 ms cap, consolidate
panel RGB565 byte ordering, and preserve refresh intent until the render lock
accepts DREAM, snapshot and diagnostic-pattern refreshes.

Baked dial sources no longer accumulate indefinitely. One shared decoded
434,312-byte frame remains. A same-dial frame does not reload or decode its
compressed source. A separate `bound` pointer tracks the vendor parser's
source independently of the visible `active` animation, which is NULL during
dial blitting. Only an unbound baked source is released; voice clips remain
resident. Decode failure, lock failure and failed source rebind preserve the
previous fallback source and permit retry.

The battery base rim now bounds its existing ascending arc writes to the rows
that reach each render strip. Binary searches operate on the two monotone arc
halves. The charging head/tails and original pixel order remain unchanged.
The code adds no persistent state, allocation, API or cadence change. The
selected measured cost is software rendering, which consumed 21.866% of the
60.319 s steady FUTURE WATCH window (37.154 ms/frame); this is CPU attribution,
not electrical attribution.

## Verified evidence

Focused Docker build `.evidence/t107a/build-display`, image
`jarvisnano-hosttests`, passes 2/2 CTest executables:

- HUD: 11,451 checks.
- Shell: 2,200 checks.

Raw output: `w2-display-rim-focused.log`. The earlier correctness-only
snapshot is retained in `w2-display-focused.log` (951 shell checks).

Production-used shell tests execute actual presenter refresh decisions,
all 65,536 RGB565 values under both byte orders, finite fades under 50/200/500
ms frame intervals, and tracked heap ownership across three cycles of all
four baked dial faces. They include initial binding, failed decode/header,
failed lock/rebind, fallback retention, later successful rebind, and zero
vendor-bound frees. Heap balances return to baseline; same-dial use does not
reload compressed memory.

The added rim regression compares whole-frame and 12-row strip output for
every battery percent, absent-battery values, charging states, both wire
orders and signed offset extremes. Semantic checks preserve the initial
2-pixel stamp at zero battery and no drawing for an absent battery.

Vendor ownership was checked in the pinned implementation, not inferred from
the stub: `managed_components/espressif2022__esp_emote_gfx/src/widget/gfx_anim.c`
944 onward validates/stops, initializes a replacement parser and retains the
old file descriptor on initialization failure. On success it deinitializes
the old parser before storing the new one. `gfx_anim_stop` (1063) retains the
parser. EAF entries point into the supplied data; deinitialization frees its
own parser/entry allocations, not the caller's source. The only display
`gfx_anim_set_src` site tracks `bound` immediately after success.

## Scratch mutation rejection

Scratch root `display-mutants` was resolved and checked inside this workspace's
`.evidence/t107a` before creation. No existing scratch run was overwritten.
The unchanged control compiles and passes all 951 checks. Each mutant compiles
against copied actual production sources and exits 1 with failed assertions:

| Mutant | Failed checks |
| --- | ---: |
| byte-order | 1 |
| finite-fade | 5 |
| brightness-cap | 1 |
| dream-retry | 8 |
| snapshot-retry | 7 |
| pattern-retry | 3 |
| cache-retain | 16 |
| cache-bound-free | 5 |
| cache-reload | 14 |
| cache-untracked-bind | 8 |

Raw combined log: `w2-display-mutations.log`. Status table:
`display-mutants/results.tsv`. Each directory retains build and assertion logs.
Preparation and runner are `prepare-display-mutants.py` and
`display-mutants/run.sh`. Rejected hand cull remains scratch-only.

## Battery rim exact pixels, work bound and timing

The actual production software overlay harness includes dial copying,
canvas, diagnostic pattern, HUD, shell and surface overlay stages. It excludes
physical panel/DMA and the vendor animation decoder. HUD code is a separate
translation unit, compiled without LTO. Each frame starts with fresh pixels;
comparisons consume every output pixel. Paired timing uses deterministic
matching input and hashes output outside the timed legs.

`perf/rim-exact-pixels.log` records 28,566 full-frame exact comparisons:
14,688 across all six styles, every percent, both byte orders, charging,
brightness/fade/shade/ask/DREAM/home modes; another 13,878 across all charging
head positions, selected boundary/absent percentages and nonzero offsets.
All byte comparisons match the frozen C1 baseline `perf/hud-before.c`.

Same-process alternating ABBA/BAAB pairs pin FUTURE, battery 69%, zero offset,
privacy muted, brightness 14 and state. Each run renders 10,000 frames per
version. All 21 paired runs reduce process CPU time and retain equal digests:

| Batch | Runs | Median CPU gain | Range |
| --- | ---: | ---: | ---: |
| Initial -Os | 7 | 3.93% | 2.13–6.67% |
| Repeat -Os | 7 | 4.07% | 1.25–5.48% |
| -O2 | 7 | 2.68% | 1.50–3.66% |

Raw logs are `perf/rim-paired-Os.log`, `perf/rim-paired-Os-repeat.log`, and
`perf/rim-paired-O2.log`. Reproduce the exact-pixel and paired protocol with
`perf/rim-paired-run.sh` inside Docker from the workspace root. Earlier noisy
whole-process timing remains preserved, but is not the adoption evidence.

`perf/check-rim-work.py` interposes a scratch-only counter into copied actual
source. It creates `rim-mutants` only after checking that its resolved path
is inside the workspace evidence directory and does not already exist.
`rim-mutants/run.sh` compiles the work check and the actual shell suite.
There are no production counters, exported runtime API or timing assertions.
The fixed FUTURE 69% frame's `ov_polar` calls fall from 6,903 to 191; binary
search probes remain extra work, so the 97.23% call reduction is not a CPU
or electrical reduction claim. The regression uses a conservative <=400
call bound, rather than requiring the implementation's exact count.

| Scratch variant | Work exit | Pixel exit | Interpretation |
| --- | ---: | ---: | --- |
| control | 0 | 0 | 2,200 shell checks pass |
| full-arc-restored | 1 | 0 | Restored source remains pixel-identical but violates work bound |
| ascending-stamp-edge | 0 | 1 | 985 failed pixel checks |
| descending-stamp-edge | 0 | 1 | 397 failed pixel checks |
| full-endpoint-equivalent | 0 | 0 | Omitting only i=256 repeats i=0 pixels; equivalent mutant |

All variants compile. The equivalent endpoint variant is recorded as such,
not forced to fail with an implementation-specific assertion. The production
code retains that original endpoint. Raw output: `w2-rim-mutations.log`;
status table: `rim-mutants/results.tsv`; individual build/work/pixel logs
remain under each scratch variant.

## Changed artifacts and boundary

- `components/jr_display/src/jr_display.c`
- `components/jr_display/src/hud_render.c` (battery traversal only; C1 retained)
- `components/jr_display/tests/test_shell.c`
- `components/jr_display/tests/probe/stubs/idf_stubs.inc`

No device operations, privacy change, cadence change, sdkconfig change, commit
or MCP call were performed by this executor. The parent owns full gates and
live validation. All four production/test paths are frozen for parent gates.

## Remaining risk and decision

Host stubs cannot establish live heap fragmentation, render lock scheduling,
TLS contiguous headroom or electrical battery gain. The parent must verify
the full sweep and matched runtime/battery windows on the selected image.
No executor blocker remains. The battery candidate satisfies exact pixels,
work-bound mutation rejection and stable full software overlay timing.
Parent combined host/firmware gates, device sweep, matched runtime windows
and voltage interpretation remain outstanding. No device power reduction
is claimed by the host benchmark. Listening and spoken voice remain owner
check pending until the owner manually unmutes.
