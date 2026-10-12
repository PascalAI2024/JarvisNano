# A/w3/HUD independent break-it audit

Parent/correlation: `t107/addendum-a`, `A/w3/HUD`, executor `a1_code`.
Authority: read-only source/evidence review; only this report may be written.
No source edits, device calls, MCP calls, combined gates, or commits.

## Result

PASS. No actionable issue found in the frozen monotone battery-rim traversal.
It preserves the legacy overlapping stamp indices and their write order,
including the repeated full-charge endpoint. The candidate is suitable for
the parent's final gates. This result does not claim electrical power savings.

Reviewed working-tree `components/jr_display/src/hud_render.c` SHA256:
`06AD986AAE14A5C7E4AAA2F8977BBD5CAAEAD94002EEFF3EE36D7E5DA0AC9D7F`.
The current source equals `perf/hud-rim.c` after newline normalization;
normalized SHA256 is
`e31cd767ed1ffa777aa44d68bf53ee8fb6da59a7b84925707f1cd887a5be7f3f`.
Different raw hashes of the two files are newline representation only.

## Evidence: production algorithm

| Property | Source evidence | Independent check |
| --- | --- | --- |
| Quantized monotonicity | `hud_render.c:30-54`, `:726-738` | Recomputed all 256 actual integer LUT entries and all 257 battery positions. Y is nondecreasing on indices 0..128, nonincreasing on 129..256. Plateaus satisfy the search predicates. No floating-point sine assumption is used. |
| Two-row stamp overlap | `hud_render.c:645-669`, `:765-776` | For a 2px stamp the exact overlap predicate is `y + 2 > y0 && y < y1`, equivalent to `y0 - 1 <= y <= y1 - 1`. Ascending bounds select that interval. Descending entry is first `y <= y1 - 1`; exit is first `y <= y0 - 2`, retaining the last overlapping row. |
| Inclusive arc length and endpoint | `hud_render.c:760`, `:765-776` | Ascending range ends at `min(steps + 1, 129)`; descending starts at 129 and ends at `steps + 1`. Thus indices 0..steps occur once in order. At 100%, index 256 survives and has the same angle/Y as index 0. At 0%, index 0 survives. |
| Offsets and clipping | `hud_render.c:645-669`, `:810-811` | Bounds use the same `cy = 232 + oy` as stamping. X and row clipping remain in the unchanged primitives. All `int8_t` offsets keep these calculations comfortably within `int`; the LUT product is at most `32767 * 218 = 7143206`. |
| Byte order and color | `hud_render.c:744-760`, `:807` | Palette initialization still precedes battery rendering. Low-battery/charging palette choice and pixel values are unchanged. Only invisible base-rim stamps are skipped. |
| Charging and absent battery | `hud_render.c:744-746`, `:778-786` | Values >100 still return before either arc or comet. The charging head, modulo `steps + 1`, 14-tail limit, 3px stamp, and draw order after the base rim are unchanged. `steps > 0` still protects its modulus/zero-charge behavior. |
| Privacy and resource/state contract | `hud_render.c:726-786`, `:813-822` | New helper uses stack integers and existing LUT only. No allocation, global state, timer, cadence, privacy write, or public API is added. The existing muted privacy ring still draws after the battery rim. |

An independent ephemeral Python check reconstructed the actual integer LUT
and the production binary-search predicates. For every arc length 0..256,
723 relative strip starts (-360..362), and strip heights 1, 2, 3, 12, 13,
and 466, it compared the selected ordered index list against the legacy
loop filtered by the exact 2-row overlap predicate. Relative starts cover
all valid panel starts for every `int8_t` Y offset. This adds direct
selection coverage beyond the integration fixtures; it is not a new test
file or a replacement for the C gates.

Receipt from `@' ... '@ | python -`, exit 0:

```text
PASS integer LUT monotonic: ascending indices0..128; descending129..256; endpoint y0=y256=-218
PASS all257 arc lengths,723 relative strip starts,6 strip heights: exact overlapping index/order selection 1114866 cases
PASS current hud_render.c equals perf/hud-rim.c after newline normalization
```

The interval proof also applies to any positive strip height, including
heights not individually enumerated. Both loops retain ascending index
order. Removing stamps that cannot touch the strip cannot change overlap
overwrites or their later composition with privacy/choice layers.

## Evidence: exact pixels, timing, and mutations

Reviewed `perf/overlay-bench.c:25-104`, `:126-174`, `:178-210` and
`perf/rim-paired-run.sh`. The harness calls the actual production software
overlay stages, restores deterministic state/input pixels, and selects the
baseline/current HUD function through the same function-pointer wrapper.
HUD is compiled as a separate translation unit without LTO. Each paired
baseline uses the matching `-Os` or `-O2` setting. ABBA/BAAB alternation and
100 warm-up frames per version reduce ordering bias. Output hashes run
outside timed legs; exact comparison uses full-frame `memcmp`, not hashes.

`perf/rim-exact-pixels.log` contains **28,566 full-frame comparisons**,
not merely 28,566 individual pixels:

- 14,688 full software overlay frames: six styles, both byte orders,
  percentages 0..101, charging on/off, and six composition modes.
- 13,878 direct HUD frames: boundary/absent percentages, both byte orders,
  three offsets, and all 257 charging head inputs with privacy muted.

All compare successfully with the frozen C1 baseline. The persistent
`test_shell.c:3346-3386` additionally compares whole-frame vs production
strip rendering across percentages 0..101 and sentinel 255, both byte
orders, charging on/off, and extreme offsets -128/0/127. It asserts the
initial zero-charge 4 pixels and absent-battery zero pixels. The reviewed
`w2-display-rim-focused.log` records 11,451 HUD checks and 2,200 shell
checks passing. These C receipts were reviewed, not rerun by this auditor.

Independent log parsing confirmed all 21 timing runs positive and all
paired digests equal:

| Receipt | Runs | Minimum CPU gain | Median CPU gain | Maximum CPU gain |
| --- | ---: | ---: | ---: | ---: |
| `perf/rim-paired-Os.log` | 7 | 2.12847% | 3.93166% | 6.67122% |
| `perf/rim-paired-Os-repeat.log` | 7 | 1.25310% | 4.07087% | 5.48353% |
| `perf/rim-paired-O2.log` | 7 | 1.49705% | 2.68056% | 3.66095% |

Reviewed `perf/check-rim-work.py`, `rim-mutants/run.sh`,
`rim-mutants/results.tsv`, and `w2-rim-mutations.log`. Copies are made only
under the checked evidence directory. The instrumented counter is added
to copied source, never production. Compiler failures stop the runner;
work and pixel exit statuses are recorded separately.

| Variant | Work exit | Pixel exit | Verified interpretation |
| --- | ---: | ---: | --- |
| control | 0 | 0 | Work bound and all 2,200 shell checks pass. |
| full-arc-restored | 1 | 0 | Pixel-equivalent old traversal is rejected by the conservative <=400 polar-call bound. |
| ascending-stamp-edge | 0 | 1 | Omitting the stamp's overlap row fails 985 pixel checks. |
| descending-stamp-edge | 0 | 1 | Wrong descending exit boundary fails 397 pixel checks. |
| full-endpoint-equivalent | 0 | 0 | Properly labelled equivalent: i=256 repeats i=0 with the same base pixel and charging is drawn afterward. Production nevertheless retains it. |

The FUTURE69 polar-call receipts show 6,903 before and 191 after with
the same full-frame hash. Binary-search probes are extra work, so this
call reduction is neither a CPU reduction percentage nor an electrical
reduction percentage. Timing supplies the separate CPU evidence.

## Changed artifacts

Only `.evidence/t107a/a3-hud-break-it.md` written for this follow-up.
Source and test paths, scratch programs, logs, and task register read only.

## Remaining risk

The software benchmark excludes physical panel/DMA, vendor decode,
target scheduling, and electrical measurement. Host gains therefore do
not establish device energy savings or the exact target CPU gain. Parent
owns the full host/build gates, image headroom, commit, OTA/valid health,
muted matched power windows, and panel captures. Listening and voice
remain owner check pending. No source defect or executor blocker remains.

## Decision needed

None from this audit. Accept the frozen HUD candidate and proceed with
the parent's required gates and live checks.
