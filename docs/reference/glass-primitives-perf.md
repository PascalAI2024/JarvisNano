# Glass row primitives: cost

**What it is** — `glass_ring_row`, `glass_arc_row`, `glass_glow_row` (and the text blitter) in `components/jr_display/src/glass_draw.c` and `glass_font.c`, the integer, anti-aliased row primitives the STATUS and WEATHER screens draw through. Pure C, no floats, no heap.

**How we use it here** — The shell composes the 466x466 panel in 12-row strips and calls each primitive once per row. A screen is therefore about 466 calls per primitive per frame, and the cost of one call (per row and per pixel) is the frame budget.

---

## Findings & gotchas

> This is the gold. Each entry is a hard-won lesson with a date. Future readers: trust these over intuition.

**[2026-11-10] Host numbers are a proxy, not device milliseconds**
Every figure below is nanoseconds per frame on an x86 host inside the `jarvisnano-hosttests` Docker image, `-O2`, CPU-time clock, reference and shipped frames alternated. The Docker host was shared with other builds and the medians moved by 20-40% between runs, so the table gives the median of one run and the minimum, which is steadier. The device will be measured by the coordinator; do not quote these as ESP32-S3 results. Source: `components/jr_display/tests/probe/glass_bench.c`, built with `cmake --build build --target glass_bench` in `components/jr_display/tests`.

**[2026-11-10] What it cost, measured first**
Counting calls on the STATUS 74 % frame, the old code spent its time in the integer square root: about 3,300 digit-by-digit roots for the arc, 3,900 for the glow and 1,300 for the track ring, against 2,300, 2,900 and 780 pixels that needed a coverage at all. `isqrt_u32` ran for each row's four chord widths and again for nearly every edge pixel, and the per-pixel arc path ran `shape_cov` over the whole chord outside the hole. An arc touching 12 % of a row paid for the other 88 % too, which is why WEATHER (twelve arcs, each called on all 260 ring rows) was the worst case.

**[2026-11-10] What changed (output is pixel-identical)**
1. *Intervals instead of a pixel walk.* On one row a circle and a half plane each select an interval of x. For a ring or arc the solid core (radial body where the wedge is also at full coverage, 127/255 of a pixel inside both lines) is found in closed form and filled as a run; the set of pixels that can be above zero (wedge above zero, plus the cap discs' reach) bounds the per-pixel work; everything else is skipped. The glow does the same for its full-strength span and its reach. Half-plane bounds use one 32-bit division each.
2. *Prepared geometry.* The trig, cap centres, vertical extent and edge windows are built once and kept in a 16-slot static cache keyed by the call's parameters (about 2 KB of `.bss`, plus one 1 KB glow table). A row outside the shape's vertical extent returns after a cache hit. WEATHER's twelve arcs mostly return here.
3. *Fewer roots, cheaper roots.* A pixel is at 0 or 255 outside the half-pixel ramp of an edge, so `n = 16 d^2` is compared with two precomputed squares and the root is taken only inside the ramp, by a tangent guess and two compares (`near_sqrt`) rather than the digit loop. `isqrt_u32` starts at its leading digit.
4. *Glow addend table.* A halo pixel's added light is a function of its distance alone, so it is tabulated once per (width, peak, colour, alpha) and looked up.
5. Small: the blend weights are worked out once per run, the dither columns once per row, and `glass_font_glyph` walks down from an index guess before it binary searches.

**[2026-11-10] How "same pixels" is proven**
`tests/glass_ref.c` is the pre-change `glass_draw.c` kept verbatim under `glassref_*` names. `test_glass.c` draws 220 random discs, rings, arcs and glows (random centre, thin and thick strokes, every sweep class from one unit to over a full turn, partial clips, both byte orders, alphas from 7 to 255), the STATUS ring at 0, 1, 5, 26, 50, 74, 99 and 100 percent (track, glow, arc, and a mid-fade arc), WEATHER's twelve arcs interleaved per row, and 40 shapes through the 16-slot cache in both orders, and compares every pixel with the reference. 47.8 million pixels in the random pass alone: **0 differ, largest channel step 0**. The tests hold it to zero, not to the 1/255 allowed. Two mutations (a loosened wedge threshold; an off-by-one in the root fix-up) make it fail with 43 and 8 differing pixels, so it does bite. The three `glass_specimen` PPMs built from the old and new sources are byte-identical.

**[2026-11-10] Where the time goes now, and what did not pay**
Per-frame, host, minima (ns, reference to shipped, the row-buffer copy removed):

| Scene | ring | glow | arc | all primitives + text |
|---|---|---|---|---|
| STATUS 100 % | 0 | 80,700 to 46,200 (1.75x) | 36,400 to 24,000 (1.51x) | 209,200 to 122,100 (1.71x) |
| STATUS 74 % | 60,800 to 16,600 (3.65x) | 80,900 to 49,200 (1.65x) | 83,100 to 35,900 (2.31x) | 372,100 to 165,900 (2.24x) |
| STATUS 5 % | 70,600 to 39,200 (1.80x) | 37,100 to 7,200 (5.2x) | 72,200 to 6,500 (11.1x) | 275,500 to 88,500 (3.11x) |
| WEATHER (12 arcs) | | | 1,092,600 to 101,000 (10.8x) | 1,134,900 to 132,600 (8.56x) |

Medians of the same run: STATUS 100 % 286,700 to 171,300 (1.67x), 74 % 439,200 to 201,600 (2.18x), 5 % 311,600 to 104,800 (2.97x), WEATHER 1,274,500 to 147,300 (8.65x). The "all" column includes about 20,000 ns of text that the reference column runs through the same code (text has no old version in the bench), so the true ratios on the shapes alone are a little higher. The row-buffer copy and clip that the bench itself pays (about 47,000 ns) is removed from every column.

- **STATUS 100 % is the weak case, and the 3x target is met only at 5 %.** At 100 % the ring is a plain full ring, which the old code already drew on its span fast path, so only the glow improves much. What is left is real work: the halo is about 3,700 pixels that each need a distance (9 ns each on the host), the 7,400-pixel core takes the glow's dithered add (12 us), and every row still takes about four digit-loop roots per primitive (an estimated 35 us of the frame at three primitives, not measured separately).
- **Text is about 20,000 ns, 15 % of the new STATUS frame.** The glyph lookup and a zero-byte skip in the nibble loop are exact (the `glass_specimen` charset PPM is byte-identical, and a test checks the lookup against a scan for 0x2200 code points in every atlas) but are lost in the noise on the host.
- **Tried and dropped:** a `glass_glow_under_row` that leaves the stroke's opaque core undrawn, since the arc overwrites it at alpha 255. It was pixel-identical but no faster on the host (the core is a quarter of the glow's cost and the extra root work ate the saving), so it is not in the tree.
- **Next lever, not done:** per-row roots. Rows are consecutive, so the four chord widths could be tracked from the previous row and corrected by a step or two instead of recomputed; estimated 15-20 % of the frame. Needs per-slot state beyond the geometry.

**[2026-11-10] The cache assumes one drawing task**
`geo_get` and the glow table are plain static state with no lock, on the assumption (CLAUDE.md, "one owner per realtime resource") that only the render task composes strips. They hold numbers only, never pointers, and every use is clamped to the row and clip, so a second caller could at worst draw one wrong frame; it could not fault. Do not call these primitives from the HTTP task.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `components/jr_display/src/glass_draw.c` | The shipped primitives; `ring_like_row` and `glass_glow_row` hold the interval logic, `geo_get` the cache. |
| `components/jr_display/tests/glass_ref.c` | The slow reference (test only). |
| `components/jr_display/tests/probe/glass_bench.c` | The bench (host numbers). |
| `components/jr_display/tests/test_glass.c` | `test_matches_reference_*`, `test_geometry_cache_thrash`, `test_glyph_lookup_matches_scan`. |

---

## Open questions

- Device milliseconds for STATUS and WEATHER after this change (coordinator to measure). The host ratios will not carry over exactly: the S3 has a hardware divider but no cache behaviour like x86, and flash-resident font bitmaps may dominate text there.
- Whether per-row root tracking is worth its per-slot state.

---

## See also

- [display-emote-gfx.md](./display-emote-gfx.md) — the render task that calls these.
