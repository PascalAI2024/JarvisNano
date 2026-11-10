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

**[2026-11-10] After: the same 23 frames, byte-identical, fewer instructions**
Same probe and flags, last commit of the branch. `jr_display_frame_tests` pins 138 scene x clock hashes; all of them held through every cut. Ir is for the whole frame.

| Scene | Ir before | Ir after | Change |
|---|---|---|---|
| home | 304,843 | 307,529 | +0.9 % (the split timers) |
| WATCH jarvis style | 2,658,625 | 311,978 | -88 % |
| WATCH diver | 1,913,096 | 1,912,986 | 0 |
| WATCH dress | 973,673 | 973,489 | 0 |
| WATCH pilot | 2,811,418 | 2,812,697 | 0 |
| WATCH minimal | 6,372,451 | 2,900,371 | -54 % |
| WATCH future | 2,389,546 | 2,380,598 | 0 |
| WATCH skeleton | 3,718,782 | 3,717,530 | 0 |
| WATCH celestial | 1,532,344 | 1,532,160 | 0 |
| WATCH jarvis, fading in | 2,658,623 | 311,976 | -88 % |
| WATCH minimal, fading in | 12,693,531 | 9,078,668 | -28 % |
| WATCH dress, fading in | 1,047,590 | 1,046,972 | 0 |
| WEATHER | 3,048,820 | 2,886,792 | -5.3 % |
| WEATHER, mid-entry | 2,360,423 | 2,209,157 | -6.4 % |
| STATUS 5 % | 3,396,737 | 3,031,005 | -10.8 % |
| STATUS 74 % | 4,456,588 | 4,075,588 | -8.5 % |
| STATUS 100 % | 3,987,370 | 3,636,414 | -8.8 % |
| STATUS 74 % charging | 4,573,944 | 4,192,501 | -8.3 % |
| STATUS mid-entry | 4,128,271 | 3,772,013 | -8.6 % |
| DESK | 4,053,129 | 3,872,341 | -4.5 % |
| ACTIVITY | 3,225,731 | 2,332,168 | -27.7 % |
| control shade | 4,064,920 | 3,700,800 | -9.0 % |
| detail sheet | 3,431,384 | 2,884,818 | -15.9 % |

The baked watches, home and the six hand-driven frames do not move: nothing they spend was cut. Home and the baked watches pay a few thousand instructions for the timers.

**[2026-11-10] The cuts, and how far each carries to the device**
Instruction counts are host x86; device milliseconds below use 1 M Ir ~ 4 ms (one instruction a cycle at 240 MHz), which is generous to the cuts because it ignores stalls, so read them as an upper bound on what instruction removal alone buys.

| Cut | What | RAM | Expected device saving | Confidence it carries over |
|---|---|---|---|---|
| WATCH paints no shell it will clear (`watch_clear_follows`) | skips the veil, screen and orbit when the clock will clear the disc | none | JARVIS style about 1.3-2.3 M Ir, MINIMAL about 2.35 M Ir: of the order of 5-9 ms of a 78 ms MINIMAL frame | Moderate-high: it removes whole calls and their memory traffic, and the saving does not depend on the cache. The device frame time is dominated by something else (see the floor), so the fraction visible in fps is smaller than the fraction of Ir. |
| Text centred when a row first draws it (`sg_txt_t.pending`) | `glass_text_width` ran on every strip for every line; now once per line | 8 bytes per text slot, on the render task's stack | about 0.1-0.2 M Ir on text-heavy screens | Low: small and spread over screens whose time is not instruction-bound. |
| Glyph table (`s_glyph_lut`) | printable ASCII resolved by a table, not a walk then a binary search | 384 bytes `.bss` (4 fonts x 96) | about 0.2-0.4 M Ir on STATUS, ACTIVITY, DETAIL | Low-moderate: the table is data in internal RAM, so it also lowers instruction-cache pressure; it adds 384 bytes of data-cache footprint. Helps text, which ACTIVITY and the detail sheet are made of. |
| `aa_disc` writes only the two edge runs of each chord | the MINIMAL dial stored 143 k interior pixels one at a time, now only the runs that need a blend | none | MINIMAL only: of the order of 1 M Ir | Moderate: fewer stores to internal SRAM. MINIMAL is the one style that draws a disc procedurally. |
| Split timers | `?split=1` | 60 bytes `.bss` for 15 counters, 60 bytes rodata for the names | negative: 6 to 8 `esp_timer_get_time` reads per strip, 39 strips a frame, a few hundred microseconds a frame at most | High that it costs this much; it is the price of knowing. |

The per-cut instruction figures are approximate: the before and after tables above are the measured totals, and the split between the text cuts is my reading of the cut-by-cut runs, not a table. The MINIMAL saving of about 3.5 M Ir is the two WATCH cuts together.

No cut allocates more than 384 bytes of internal RAM, so none touches the contiguous-block budget.

**[2026-11-10] Tried and dropped: the `geo_get` hint order**
`geo_get` probes sixteen slots; moving the hinted slot first halved the cost of an empty call in an isolated bench (331 to 163 instructions) and moved the whole frame by under 1 %, and made WEATHER slightly worse. It was reverted and is not in the history. This is the same lesson as the primitive rewrite: a bench of one call is not a frame.

**[2026-11-10] Found and not cut**
- The orbit rail, 1.25 M Ir on every shell screen. A cached coverage row would remove most of it for about 1.3 KB of `.bss`. The pixels would stay identical, but the cache has to be keyed on the rail's parameters and invalidated with them; I did not take that on without a device number showing the rail matters.
- WEATHER's empty arc calls (3,355 of 3,559 draw nothing). A cheap reject before the call would not change pixels; it needs a per-arc bounding row range the primitive does not expose today.
- The hand fills on the PILOT, DIVER, FUTURE and SKELETON watches, which are the hands.
- Anything for the floor, until the split timers say where it is.

**[2026-11-10] Reading the timers on the device**
`GET /api/display?split=1` returns `render_frames`, `render_frame_us` and `split_us`, each a sum in microseconds since boot over every strip of every frame; divide by `render_frames` for a per-frame figure, or take two reads and subtract. Names: `hud_frame`, `hud_space`, `hud_clock`, `hud_ota`, `hud_caption`, `hud_ask`, `hud_bloom` partition the hud layer; `space_veil`, `space_screen`, `space_sheet`, `space_shade`, `space_orbit` are inside `hud_space`; `clock_clear`, `clock_face`, `clock_cells` are inside `hud_clock` (`clock_cells` includes the sun arc). The existing `layer_us` gives the dial blit (index 0) against the hud (index 2). If `render_us` per frame is much larger than the sum of the five `layer_us`, the render task is being pre-empted or waiting inside `panel_flush`, not computing.

**[2026-11-10] Second round: the shell, the rail, the timers, image 519b252, lease held**
Branch `feat/glass-speed`, cut from `feat/glass-2` at 519b252. No device was available, so everything in the Ir columns is measured (callgrind on the host probe, one warm frame at `clk=100000`, millions of instructions) and every ms figure is inferred. The device column is the owner's table from that image with the CODEX lease held, in ms per frame.

| Scene | Device total / hud / shell ms (measured, before) | Ir before | Ir after | Change |
|-------|----------------------------------------------|-----------|----------|--------|
| home (JARVIS) | 26.9 / 5.2 / 21.0 | 0.31 | 0.31 | none |
| home, agent rim 100 % | | 3.25 | 0.53 | -2.72 |
| home, agent rim 40 % | | 3.24 | 0.66 | -2.59 |
| WEATHER | 58.3 | 2.89 | 2.16 | -0.73 |
| STATUS | 84.6 | 3.03 to 4.19 | 2.34 to 3.49 | -0.70 |
| STATUS, agent rim 100 % | | 7.00 | 3.57 | -3.42 |
| STATUS, agent rim 60 % | | 6.99 | 3.70 | -3.29 |
| DESK | 67.3 | 3.87 | 3.18 | -0.70 |
| DESK, agent rim 13 % | | 6.82 | 3.54 | -3.28 |
| ACTIVITY | 50.8 | 2.33 | 1.59 | -0.74 |
| control shade | 75.2 | 3.70 | 3.60 | -0.10 |
| detail sheet | | 2.88 | 2.14 | -0.74 |
| WATCH styles | 30.3 to 66.8 | | unchanged | none (see below) |

What was cut, in the order it was done:

| Cut | How | Pixels | Ir | Expected on the device (inferred) | How sure |
|-----|-----|--------|----|----------------------------------|----------|
| Agent rim | the rim tests two short runs a row (the band at the row's radius and its hole), not every pixel; the sector test is skipped once the rim is complete | identical, pinned by 25 frame hashes at 100, 40, 60 and 13 % | about -2.7 to -3.4 M per ring screen | the 21 ms shell cost is mostly this loop: the rim was about 2.9 M of the 3.25 M a rim frame cost against 0.31 M without it, so expect most of it to go, order of 15 ms or more | moderate: Ir to ms is not linear on this part (the primitive rewrite taught that), but this loop is pure ALU on a hot cache |
| Orbit rail | the invariant track ring is recorded once into PSRAM at `jr_display_start` and replayed as runs; the lit mark arc is asked about only on the rows it can reach | identical, pinned by the replay-equivalence test over 256 angles and by the hash table in both modes | about -0.7 M on every ring screen | 3 to 5 ms of the 12 to 14 ms | moderate |
| Split timers | `split_mark` is one branch unless `?split=1` armed the timers; the first `?split=1` request arms them for a few seconds, so read twice and subtract | n/a | not in the scenes | a few hundred microseconds a frame | high that it is not zero, low on how much |

RAM, per cut: the rail record is 14,066 bytes in PSRAM, allocated once; internal `.bss` is 4 bytes for its pointer. The split arming adds 5 bytes. The lease flag is 1 byte, the brightness cap rides in the existing `s_status_word`. Total new internal static: about 10 bytes against the 1.5 KB allowance.

The 4 KB loss in the largest internal block (26.6 KB to 22.5 KB) after the last lane: the statics that lane added are a 385-byte glyph lookup table (4 fonts x 96 + a ready flag) and 60 bytes of split counters, about 0.45 KB, and its other changes (the PMIC read, the `?split=1` JSON, new tests) allocate nothing that stays. Statics do not explain 4 KB. The most likely cause is a different heap state at the moment of the two readings (a TLS session, a socket or an OTA buffer held at one and not the other), which is how the block changes between boots with no code change. This is inferred; only `/api/diag/tasks` and the heap figures on a freshly booted device at the same uptime, on both images, can settle it.

Not cut:
- `space_screen` on STATUS, DESK and WEATHER (about 2 to 2.6 M Ir each now). A per-row record and replay of the focal ring track, glow and arc would repeat the orbit's method; it needs a cache keyed on the ring's parameters and I did not take it on without a device number for what remains after the rim and rail cuts.
- The control shade (`space_shade`, `space_veil`) and `clock_face`: no cut made. The WATCH styles are unchanged in this round.
- Tried and dropped: a flat-texel shortcut for the balance wheel raised the instruction count and was reverted.

Pixel-changing fixes in the same round, each with before and after images from the probe and regenerated hashes: the lease caption no longer prints over the shade or the detail sheet (`shade-lease`, `detail-lease` changed); the shade's brightness arc and number show the owner's cap (`shade-dimmed` changed; the cap travels in bits 8 to 15 of `s_status_word`, 0 meaning unknown); an ACTIVITY row ends on a whole sentence or on a word and three dots (`activity` changed; no font here has U+2026).

**[2026-11-10] Third round: the ring screens, the shade, the veil, the MINIMAL dial (branch `feat/glass-speed-2`, cut from `feat/glass-2` at 91dc040)**
No device was available. Everything in the Ir columns is measured (callgrind on the host probe, one steady-state frame at `clk=100000`, instructions per whole frame; the probe now runs two warm frames, see below). Every millisecond figure is inferred. The device column is the second-round table's `space_screen` / `space_shade` / `space_veil` / `clock_face` split numbers from image 519b252, in ms per frame.

| Scene | Ir before | Ir after | Change | Device ms before (measured, 519b252) |
|-------|-----------|----------|--------|--------------------------------------|
| STATUS 74 % | 3,376,035 | 1,861,501 | -44.9 % | `space_screen` 42.6 |
| STATUS 74 %, charging, muted | 3,492,948 | 1,978,857 | -43.3 % | |
| STATUS 5 % | 2,338,744 | 1,462,816 | -37.5 % | |
| STATUS 100 % | 2,946,056 | 1,940,848 | -34.1 % | |
| STATUS, agent rim 100 % | 3,573,398 | 2,057,911 | -42.4 % | |
| STATUS mid-entry (the ring still sweeps) | 3,075,481 | 3,065,632 | -0.3 % | |
| DESK | 3,175,287 | 1,650,634 | -48.0 % | `space_screen` 26.6 |
| WEATHER | 2,159,575 | 1,620,320 | -25.0 % | `space_screen` 16.6 |
| WEATHER mid-entry | 1,472,704 | 1,178,736 | -20.0 % | |
| ACTIVITY | 1,511,558 | 1,424,884 | -5.7 % | `space_screen` 7.7 |
| control shade | 3,599,979 | 1,911,832 | -46.9 % | `space_shade` 37.6, `space_veil` 7.7 |
| control shade, panel mood-dimmed | 3,694,463 | 1,925,177 | -47.9 % | |
| detail sheet | 2,142,912 | 2,074,596 | -3.2 % | |
| WATCH MINIMAL | 2,898,829 | 2,022,848 | -30.2 % | `clock_face` 25.6 |
| WATCH MINIMAL, clock fading in | 9,077,126 | 8,405,649 | -7.4 % | |
| WATCH PILOT | 2,810,568 | 2,655,292 | -5.5 % | `clock_face` 21.4 |
| WATCH FUTURE | 2,378,469 | 2,204,105 | -7.3 % | `clock_face` 12.8 |
| WATCH SKELETON, DIVER, DRESS, CELESTIAL, JARVIS, home | | | none (+-0.3 %) | |

The two WATCH MINIMAL rows measure the disc record and the word fill, which the knob round (below) removed after the device ran them 12 ms slower; the PILOT, FUTURE and every other row are unaffected.

The mid-entry rows barely move on purpose: while the ring sweeps or the shade opens, the value changes every frame and a record would be thrown away, so the long way is kept. The frame hashes agree on every row above except `home` and `home-agent-100`, and those two differ only because the profiler now runs two warm frames and these scenes are mid-ease at the clock it uses; the hash suite (one warm frame) pins both and passes.

What was cut, in the order it was done. Instructions are host figures; expected device time is inferred and uses no more than the 1 M Ir ~ 4 ms upper bound of the second round.

| Cut | How | Ir | Expected on the device (inferred) | How sure |
|-----|-----|----|-----------------------------------|----------|
| Battery / desk ring (STATUS, DESK) | the ring's track, glow and fill are each recorded once the entry has finished, the screen is still (`oy == 0`) and the sweep has held for a frame, then replayed: `glass_shape_record` / `glass_glow_record` keep what `glass_ring_row` / `glass_arc_row` / `glass_glow_row` would do (solid runs, edge coverage, the glow's distance per pixel); colour, alpha, clip and byte order stay free | STATUS 74 %: -1.42 M, DESK: -1.43 M | `space_screen` on STATUS 42.6 ms: perhaps 8 to 14 ms off by instruction count alone, more if the flash cache is the cost, because the replay touches a few hundred bytes of code where the ring primitives touched several kilobytes; DESK 26.6 ms: 8 to 12 ms off | moderate on the direction and on at least half the instruction figure, low on the ms |
| WEATHER's rain marks | `glass_arc_rows` gives the rows a mark can touch (the primitive's own early-out, computed once a strip); the other rows' calls are not made | WEATHER: -0.45 M | 1 to 2 ms of 16.6 | moderate |
| The shell's row clip | a 214-entry table of half widths replaces a shift-and-subtract square root per row | about -0.09 M on every shell screen | under 0.5 ms a screen | high that it is not zero, low on how much |
| Control shade | the two tracks recorded at start, the two fills once their value has held a frame; `glass_ring_replay_row` stands in for `glass_arc_row` | shade: -1.7 M with the clip table | `space_shade` 37.6 ms: 10 to 15 ms off | moderate |
| The veil | `hud_dim565` two pixels to a 32-bit word and no per-pixel test | none on the host (the stand-in face is mostly black, so the old per-pixel test rarely fired and the new loop does the same work) | `space_veil` 7.7 ms: the old loop cost a compare, a branch and the fold per lit pixel; the new one is about three instructions per pair. A guess of 2 to 4 ms off, by instruction count; the real face has far more lit pixels than the stand-in, so this is the one cut whose host figure says nothing | low |
| MINIMAL dial (reverted) | the disc's rows were kept the first time they were drawn (interior span plus the edge coverage, 9.2 KB) and replayed; opaque runs in `aa_disc` and `fill_run` stored two pixels to a word | MINIMAL: -0.88 M | predicted `clock_face` 25.6 ms: 3 to 4 ms off | measured the other way: 12 ms slower on the device. Both halves are gone; see "The knob round, 2026-11-10" |

RAM, per record. Everything large is PSRAM, allocated once in `jr_display_start` next to the orbit rail's, and never freed (the screens that use them can come back at any time; 86 KB of 5 MB free). Internal RAM added: about 150 bytes in total (against the 1 KB allowance).

| Record | PSRAM | Built | Internal |
|--------|-------|-------|----------|
| Battery / desk ring: track, glow, fill | 3 x 12,288 B = 36,864 B (largest in use 10,043 B at 99 %; 3.1 + 7.7 + 7.1 KB at 74 %) | lazily, in the render task, the second frame a sweep holds; again on a change (one frame of the old long way first, then one frame that pays for the record, about the cost of the ring's own drawing once) | pointer 4 B, key 8 B, settle gate 12 B |
| Control shade: two tracks, two fills | 4 x 8,192 B = 32,768 B (largest 4,708 B) | tracks at start; a fill the second frame its value holds | pointer 4 B, 4 x 24 B keys |
| Frame counter and the 214-byte clip table | none | | 4 B `.bss`, 214 B rodata |

The hitch is the thing to watch on the device: a record costs about one frame's worth of the ring it replaces, once, when a value stops moving (a battery percentage ticking over, the volume slider released). At worst that is one 40 ms frame on STATUS. If it shows, the lever is to record rows as they are drawn.

Not cut, and why:
- SKELETON's balance wheel (`hud_balance_blit`, 1.73 M Ir, 46 % of its frame). It visits every pixel of a 143 px box and reads four alpha texels for each; the rotation makes any row bound a per-pixel test again, and the flat-texel shortcut the last lane tried raised the count. A bound that does not is a per-radius occupancy table of the sprite (rotation does not change a pixel's distance from the centre), which turns the visited disc into the rim and spokes. Not attempted in this lane; it is the one cut here with new geometry in it, and the existing SKELETON scenes would pin it.
- PILOT's sub-dials: the three rings are the day arc (`sp_annulus_row`, 0.43 M) and the three needles (`sub_needle`, 0.5 M). The rings are static and could be recorded the way the ring is; the needles are the hands. PILOT (-5.5 %) and FUTURE (-7.3 %) moved only by the clip table, which their ring and cell rows call.
- MINIMAL's sixty ticks and twelve dots (about 0.3 M of `aa_fill`), static and recordable the same way as the disc; the hands are the other 0.7 M.
- The shade's button disc (0.47 M) and ring: `glass_disc_row` has no recorder.
- WEATHER's twelve rain marks drawn on their own rows still cost 0.45 M together; they animate during the entry and are static after, a record each would be twelve 934-byte offset tables.
- The 25 ms floor is not touched by any of this; see the open questions.

Method notes. The probe is built as the page's first round describes, with `-DFC_NOCOUNT` and the six `--wrap` flags. `probe frame` now runs two warm frames (`fc_extra_warm`) so the measured frame is the steady state, not the one that records; the hash suite keeps one, which also proves the frame that records draws the same pixels as the frame that replays. Four scenes were added and hashed on the code before any cut of this branch: `status-1`, `status-99`, `desk-ent` and `shade-ent`. `jr_display_frame_tests` runs the whole hash table four times (everything on; no orbit record; no ring record; no shade record) and compares every record's replay with the primitive it replaces over sweeps, alphas, clips and both byte orders. One deliberate mutation per cut was seen failing: the glow replay's lookup index, `glass_arc_rows`' last row, the clip table's first entry, the shade replay's alpha, the veil's mask, the word fill's tail.

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

- What the roughly 25 ms floor is. The split timers and `render_us` against their sum answer it on the first flash. The third round cut instructions on every screen except the baked watches and home, so a flash that still shows 25 ms on DRESS and 27 ms on home settles it.
- Whether STATUS and WEATHER are bound by instruction-cache misses on flash. If they are, the lever is code footprint and locality, or placing the hot primitives in IRAM, which costs internal RAM and needs an owner decision.

**[2026-11-10] Transition frames (feat/glass-motion)**
Worst frame of each preview script, callgrind instructions per frame, same preview harness before (feat/glass-2 at 175d912) and after, `-O2`, no vectorising, 24 fps scripts (method in glass-preview.md):

| script | frames | worst before | worst after | where after |
|---|---|---|---|---|
| watch-enter (WATCH in and out, dial to home, dial to WEATHER) | 192 | 5.82 M | 6.51 M | frame 99, a baked dial dimming out to home |
| slide (WEATHER, STATUS, home) | 148 | 4.95 M | 4.52 M | frame 47 |
| shade (open and close over home and over WATCH) | 228 | 5.31 M | 5.41 M | frame 30 |
| style (dial to dial, to MINIMAL, back) | 252 | 3.75 M | 9.61 M | frames 91 to 94, MINIMAL fading in |

The style dip into MINIMAL is the one over budget: MINIMAL has no art, so its dial is drawn by the compositor, and at a strength below full it is blended per pixel (about 6 M instructions more than the opaque settled path) for the four frames it takes to come up. A dial record for MINIMAL was measured slower on the device and is off, so there is no cheap path to hand it; the options are a shorter dip in for the art-less styles or a record that carries the strength. Not measured on the device.

**[2026-11-10] The knob round (feat/glass-keep)**
Seven cost candidates sat behind the dev perf knob (`perf_knob.h`), each pixel-identical to knob 0. The device measured them on image 5af9fc4 (240 MHz, lease held, 3 s a sample, ms per frame; "face" is the `clock_face` split part, "dial" is the dial layer). Run-to-run spread on one knob was 1.5 to 2.5 ms.

| Dial | knob 0 | candidate | result |
|---|---|---|---|
| MINIMAL face | 38.5 / 38.4 | bit 0, record the disc: 33.5 | helps only the slow path |
| MINIMAL face | 38.5 | bit 1, old hud fill: 26.5 | wins, -12 ms |
| MINIMAL face | 38.5 | bit 3, old `aa_disc`: 25.8 | wins, -12.7 ms |
| MINIMAL face | 38.5 | bits 1+3: 25.0; bits 0+1+3: 25.1 | the record adds nothing once the old paths are back |
| SHADE total | 45.3 / 45.5 | bit 2, old glass fill: 43.7 | neutral to slightly better |
| STATUS total | 36.5 / 37.6 | bit 2: 36.4 | neutral |
| SKELETON face | 31.3 / 33.5 | bit 4, wheel class map: 32.8 | no gain |
| dial layer, six baked dials | 8.3 to 9.1 | bit 5, span copy: 7.5 to 7.8 | wins, about -1.1 ms on every dial |
| PILOT face | 20.4 / 22.0 | bit 6, `aa_fill` once per row: 21.1 | neutral |
| FUTURE face | 13.4 / 15.9 | bit 6: 16.6 | worse |
| DIVER face | 13.8 / 15.9 | bit 6: 17.0 | worse |

What was kept and what was deleted:

| Bit | Decision |
|---|---|
| 0 disc record | Deleted: `hud_dial_cache_set` / `hud_dial_cache_used`, the record branches in the disc code, the PSRAM arena (`dial_rec_alloc`, `SG_DIAL_REC_WORDS`), and the identity suite's "no dial record" pass. |
| 1 hud fill | The per-pixel store loop is the only path in `hud_render.c`; `fill16` is gone. |
| 2 glass fill | The same in `glass_draw.c`. |
| 3 disc rows | The `aa_disc` from before 91b44ec is the only one. |
| 4 wheel classes | Deleted with its 5.3 KB PSRAM map and its test; `hud_balance_blit` is as it was. |
| 5 dial span | The only path in `apply_dial`. The 233-byte span table stays, and `dial_span_check` in the identity suite compares the copy with a full-row reference built inside the test. |
| 6 `aa_fill` interior | Deleted; `aa_fill` is as it was. |

The knob mechanism stays with no bits defined (`JR_KNOB_KNOWN_MASK` is 0): the word, `POST /api/debug/display?knob=N`, the `"knob"` field of `GET /api/display`, the probe's `knob=N` and the identity suite's per-bit loop.

The lesson is about the word fill. `fill16` stored two pixels to a 32-bit word. On the host it counted fewer instructions (MINIMAL 2.90 M to 2.02 M with the disc record), and on the ESP32-S3 it ran 12 ms slower than the 16-bit store loop it replaced. Host instruction counts have now predicted the device wrong in both directions on this chip, so a change that is justified only by a callgrind figure ships behind the knob and waits for a device sample. The disc record, also justified by instructions, bought nothing once the plain loops were back.

---

## See also

- [glass-primitives-perf.md](./glass-primitives-perf.md) — the primitive rewrite and why its bench did not predict the device.
- [display-emote-gfx.md](./display-emote-gfx.md) — the strip contract the compositor lives inside.
