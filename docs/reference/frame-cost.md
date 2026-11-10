# Frame cost: where a whole frame's time goes

**What it is** — A whole-frame profile of the software compositor in `components/jr_display/src/jr_display.c` (`panel_flush`: the HUD, shell and surface layers over 12-row strips), taken on the host with callgrind over the production overlay path, set against the device's own `render_us`. Tools: `components/jr_display/tests/probe/probe.c` (`probe frame SCENE clk=MS`), `components/jr_display/tests/frame_scenes.inc` (the scenes), `test_frame_identity.c` (the pixel pin).

**How we use it here** — The glass-primitive rewrite measured 1.7x to 8.6x on an isolated bench and changed nothing on the device (STATUS 73.6 ms before and after). This page is the corrective: it says which screens' time is instruction count, which is something only the device has, which cuts were made, and which timers the firmware now carries so the next round starts from device numbers.

---

## Findings & gotchas

> Host numbers are instructions, not milliseconds. Nothing on this page was measured on the device; the device column is the figures the owner measured with the operator lease held.

**[2026-11-10] How the profile was taken**
`probe frame SCENE clk=MS` runs one warm frame (the previous frame at clk-42 ms first, so caches and eases are in steady state) through `overlay_fade_tick`, then per 12-row strip `rest_eligible`, `apply_hud_overlay`, `apply_shell_overlay`, `apply_surface_overlay`, exactly the software layers of `panel_flush`. The dial blit and the canvas are not in it: they are PSRAM copies and the host has nothing to stand in for them. The face behind is a black disc with four cyan rings, not the decoded clip. Built `-O2 -g -fno-tree-vectorize -fno-tree-slp-vectorize -fno-unroll-loops` (the Xtensa compiler vectorises nothing; the flags moved totals by under 2 %). The firmware's own flags are `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (`-O2`), so no `-Os`/`-Og` finding. valgrind 3.24 callgrind in a Docker image derived from `jarvisnano-hosttests` (`apt install valgrind`; `scripts/host-tests.Dockerfile` untouched), `--toggle-collect=fc_frame_measured*` so only the measured frame counts. Cache columns: `--cache-sim=yes --I1=16384,8,32 --D1=32768,8,32`, the S3's instruction cache from `sdkconfig` (`CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB`, 8 ways); the data cache size is the S3 default and was not read from the build.

**[2026-11-10] What was not done**
No per-row-band split (the brief's "if cheap"): the strip loop is uniform, so the band split would be the same call tree 39 times. The control shade and detail sheet are in the table below but have no device figure to rank against.

**[2026-11-10] Instructions per frame, before any cut**
Ir is instructions for the whole frame; I1 miss is simulated 16 KB instruction-cache line misses; device ms is the owner's `render_us` (panel flush excluded).

| Scene | Ir | I1 miss | Device ms |
|---|---|---|---|
| home | 304,843 | 20 | 27 (face clip's own rate) |
| WATCH jarvis style | 2,658,625 | 494 | not measured |
| WATCH diver | 1,913,096 | 376 | 55-64 |
| WATCH dress | 973,673 | 296 | 43-45 |
| WATCH pilot | 2,811,418 | 603 | 55-64 |
| WATCH minimal | 6,372,451 | 6,857 | 78 |
| WATCH future | 2,389,546 | 1,049 | 55-64 |
| WATCH skeleton | 3,718,782 | 387 | not measured |
| WATCH celestial | 1,532,344 | 343 | not measured |
| WATCH jarvis, clock fading in | 2,658,623 | 492 | |
| WATCH minimal, clock fading in | 12,693,531 | 7,328 | |
| WATCH dress, clock fading in | 1,047,590 | 294 | |
| WEATHER | 3,048,820 | 2,036 | 47 |
| WEATHER, mid-entry (180 ms) | 2,360,423 | 1,801 | |
| STATUS 5 % | 3,396,737 | 3,636 | 74 (74 %) |
| STATUS 74 % | 4,456,588 | 10,052 | 74 |
| STATUS 100 % | 3,987,370 | 7,673 | |
| STATUS 74 % charging | 4,573,944 | 11,016 | |
| STATUS mid-entry (180 ms) | 4,128,271 | 9,804 | |
| DESK | 4,053,129 | 4,656 | 35 |
| ACTIVITY | 3,225,731 | 964 | 34 |
| control shade | 4,064,920 | 1,870 | |
| detail sheet | 3,431,384 | 297 | |

**[2026-11-10] The host explains the shell screens' rank, not the watches', and not a floor of about 25 ms**
Ir and I1 misses rank the shell screens the way the device does: ACTIVITY (3.2 M, 1 k misses) 34 ms, DESK 35 ms, WEATHER (3.0 M, 2 k) 47 ms, STATUS (4.5 M, 10 k) 74 ms. They do not explain home or the baked dials. Home composes 0.3 M instructions (about 1.5 ms of work at one instruction a cycle) and measures 27 ms; WATCH dress composes 1.0 M and measures 43-45 ms. Something costs roughly 25 ms on every screen that no instruction in this probe is: the likeliest candidates are the PSRAM dial blit (`apply_dial`, 434 KB of PSRAM reads a frame, only on the baked dials), contention for PSRAM or the flash cache with the face decode on the other core, and the render task being pre-empted inside `panel_flush` (`render_us` is wall time from `esp_timer_get_time`, not CPU time). I cannot tell these apart without the device, and I am not sure any of them is right. The new split timers (below) separate the dial blit from the rest, and `render_us` against the sum of the layer timers shows pre-emption directly.

**[2026-11-10] STATUS 74 %: the primitives are 70 % of the instructions, so the earlier bench was not wrong about the host**
`sp_focal_status` is 69 % of the frame (glow 22 %, arc 18 %, text 10 %, ring 8 %, `glass_text_width` 4 %, `sg_txt_make` 4 %) and `sp_draw_orbit` 28 %. By self cost: `ring_like_row` 35 %, `glass_glow_row` 20 %, `glass_font_glyph` 8 %, `geo_get` 5.6 %, `glass_text_row` 5.5 %, `sp_clip` 2.6 %. The primitives did get faster on the host and STATUS did not move on the device, which says the device's STATUS time is not their instruction count. The I1 column is the candidate: STATUS misses the simulated instruction cache 10,000 times (11,000 charging) against 300 for the detail sheet, which executes more code in total but in long runs. On the device a miss is a line fetch from flash over DIO at 80 MHz (`CONFIG_ESPTOOLPY_FLASHMODE_DIO`, `FLASHFREQ_80M`: 20 MB/s, so a 32-byte line is of the order of 2 microseconds, 10,000 of them about 20 ms). That fits the size of the unexplained STATUS residual (74 ms measured against about 40 ms from instructions at the ACTIVITY ratio), but a host-simulated miss count on x86 code is not the Xtensa binary's, and the fit rests on one scene. Confidence it is the main device-only cost for STATUS and WEATHER: moderate, not high.

**[2026-11-10] Work that ends in no write**
Counted with the probe's row wrappers over the measured frame (primitive calls, and calls that changed no pixel):

| Scene | calls | no-pixel calls |
|---|---|---|
| STATUS 74 % | 1,957 | 660 (33 %) |
| STATUS 5 % | 1,929 | 996 (51 %) |
| WEATHER | 4,214 | 3,452 (81 %): 3,355 of 3,559 arc calls |
| DESK | 1,815 | 565 (31 %) |
| ACTIVITY | 1,061 | 470 (44 %) |
| control shade | 2,439 | 1,162 (47 %) |
| detail sheet | 1,255 | 534 (42 %) |
| WATCH jarvis / minimal (shell under the clear) | 899 | 433 (48 %) |

An empty call is not free: a ring call on a row the ring cannot reach cost 331 instructions, 300 of them `geo_get` probing fifteen slots before it found the hint's own (see the cut below). WEATHER spends about a third of its frame on arc calls that draw nothing.

**[2026-11-10] A third of every shell screen is the orbit rail**
`sp_draw_orbit` is 1.24-1.26 M instructions on every shell screen (28 % of STATUS, 41 % of WEATHER, 39 % of ACTIVITY, 31 % of DESK): a full-turn 4 px ring plus a 10-degree mark, called once per row over about 400 rows, 2,350 instructions per row that touches the ring. (Measured on a stand-alone call with the rail's parameters.) My estimate, not a measurement, is that a third of that is the four digit-by-digit square roots per row and most of the rest anti-aliased edge pixels going through `shape_cov`. It is identical every frame. A per-row cache of the rail's coverage would remove nearly all of it and costs memory: one quadrant of edge pixels is about 1.3 KB of `.bss`, which I did not spend without the device numbers to justify it. Listed, not cut.

**[2026-11-10] WATCH (JARVIS and MINIMAL styles) painted the whole shell and then erased it**
On the WATCH screen with a style that has no baked dial, `apply_space_overlay` ran the veil, the screens and the orbit rail into the shell disc and `apply_clock_overlay` then cleared that disc with a memset (`jr_display.c`, the clock's own comment: "the orbit indicator does not [survive]"). Both are bounded by the same r214 chord per row. That was 2.35 M instructions of a 6.37 M MINIMAL frame (37 %) and 1.3 M of 2.66 M for the JARVIS style (47 %); the veil's `if (row[x] != 0)` loop alone was 0.43 M. The baked dials do not clear and do not draw the shell (the watch art is the screen), so DIVER, DRESS, PILOT, FUTURE, SKELETON and CELESTIAL never paid it.

**[2026-11-10] Where the watches' own time goes**
MINIMAL: `aa_disc` 38 % self (the dial is a 214 px disc drawn procedurally every frame: 143 k interior pixels written one at a time and four chord square roots per row), `aa_fill` 15 %. PILOT, DIVER, FUTURE, SKELETON: `draw_hand` and `aa_fill` are 47-73 % of the frame (each hand is several anti-aliased polygon fills: shadow, body, light and dark flanks, lume), SKELETON's balance wheel blit another 46 %. None of these is work that draws nothing; they are the hands. Not cut (see below). A MINIMAL frame while the clock fades in costs 12.7 M, twice the settled frame, because every disc blends at partial alpha instead of storing: it lasts a fade, not a screen.

**[2026-11-10] No 64-bit divide inside a loop**
`esp_timer_get_time() / 1000` appears in `overlay_fade_tick`, `rest_eligible`, `apply_hud_overlay`, `panel_flush` and `display_task`: one per strip at worst (39 a frame, `rest_eligible` runs per strip), 39 divides of about 100 instructions. Not in `hud_overlay_watch`, `sub_needle`, `sp_draw_shade` or any per-row path. Not a cause.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `components/jr_display/src/jr_display.c` `panel_flush` | the layers and `layer_mark`. |
| `components/jr_display/tests/probe/probe.c` | `probe frame SCENE clk=MS [ppm=PATH] [calls]`, `probe frame list`. |
| `components/jr_display/tests/frame_scenes.inc` | the scenes and the whole-frame composer. |
| `components/jr_display/tests/test_frame_identity.c` | the pixel pin (scene x clock hash table), suite `jr_display_frame_tests`. |
| `docs/reference/glass-primitives-perf.md` | the bench that did not carry over. |

---

## Open questions

- What the roughly 25 ms floor is. The split timers and `render_us` against their sum answer it on the first flash.
- Whether STATUS and WEATHER are bound by instruction-cache misses on flash. If they are, the lever is code footprint and locality, or placing the hot primitives in IRAM, which costs internal RAM and needs an owner decision.

---

## See also

- [glass-primitives-perf.md](./glass-primitives-perf.md) — the primitive rewrite and why its bench did not predict the device.
- [display-emote-gfx.md](./display-emote-gfx.md) — the strip contract the compositor lives inside.
