# Concept Comparison

## Metrics Table

| Metric | Concept 1: Monolith | Concept 2: Open Cube | Concept 3: Egg | Concept 4: Radio |
|--------|---------------------|----------------------|----------------|------------------|
| Outer footprint (W x D, mm) | 78 x 68 | 76 x 66 | 72 x 62 | 78 x 60 |
| Height (mm) | 66 | 65 | 68 | 55 (+10 knob) |
| Desk area (cm2) | 53.0 | 50.2 | 44.6 | 46.8 |
| Print time @ 0.2 mm layer | 4.5 h | 4.0 h | 5.5 h (2 prints) | 3.5 h |
| Filament estimate | 55 g | 50 g | 60 g | 48 g |
| Screw count | 4 | 4 | 4 | 4 |
| Assembly difficulty (1=easy, 5=hard) | 2 | 3 | 3 | 2 |
| Phase 3 screen swap difficulty (1=easy) | 1 | 1 | 2 | 1 |
| Support material required | None | None | Tree supports at base | None |
| Acoustic performance | Good (side grille) | Good (front hex) | Best (top dome = omni) | Good (front slots) |
| Thermal performance | Adequate | Best (window in lid) | Adequate | Adequate |
| Phase 1 visual quality | Excellent | Excellent | Excellent | Excellent |
| Phase 3 visual quality | Excellent | Good | Excellent | Excellent |

## Qualitative Comparison

**Monolith** reads as the most universally "right" choice for a J.A.R.V.I.S. desk unit. The matte charcoal slab with a single LED slit and a circular display cutout is legible from across the room as intentional design. It occupies the middle ground of the four concepts — not the smallest, not the most complex, not the most unusual — which makes it the most likely to age well and appeal across different taste profiles.

**Open Cube** is the most accessible to modify. The clear lid window and visible M2 cap-heads signal that the internals are meant to be seen. The PCB tray slide-out mechanism is the most service-friendly design — XIAO can be reflashed without full disassembly. The tradeoff is visual transparency: the interior wiring and PCB are visible, which may read as "unfinished" to some users. Best for those who want to show the hardware inside.

**Egg** is the highest-ambition design. The continuous fillet surface has no parallel in consumer desktop accessories at this price point, which makes it the most likely to attract attention and comments. The downside is print time (two separate halves, each needing careful orientation) and the tightest interior fit of the four (the amp+speaker module leaves only 5 mm clearance on each side at the narrowest section of the lower half). The equator joint is also the most visually prominent seam, which requires careful print calibration to minimize.

**Radio** is the most distinctive concept — it occupies a category no other desk accessory currently inhabits. The landscape form, vertical grille slots, and status knob create strong visual personality. The tradeoff is that it does not obviously look like AI hardware; it looks like an audio device, which may or may not match the user's intent. The 55 mm height makes it the shortest of the four, which reads as less imposing on the desk — a legitimate advantage depending on setup.

## Winner: Concept 1 — Monolith

The Monolith is the primary recommendation for the following reasons:

1. Visual alignment with the J.A.R.V.I.S. identity. The horizontal LED slit above the round display cutout is the single most recognizable visual motif the project could adopt — it references both JARVIS's HUD strip and industrial control panels without being derivative of either. No other concept achieves this in the same clean read.

2. Interior geometry is the most forgiving. The vertical stack leaves generous clearance around all components. The amp+speaker module (50 x 30 mm) fits comfortably in the mid-right section with 12 mm lateral clearance. The battery sits flat on the floor. The XIAO has a clean path to the front wall for USB-C and the mic port.

3. Print is the second-easiest. Single-piece body, no supports, no equator seam calibration. The Egg is more complex; the Cube requires tray rail tuning; the Radio is comparable.

4. Phase 3 upgrade path is the cleanest. The front face is oriented for easy display integration, and the +6 mm display stack has 8 mm of clearance inside the top cavity, confirmed by the layer geometry in the SCAD file.

5. Material match. Matte charcoal PETG or ASA has the best surface aesthetics for an object that will be touched and seen daily. It hides fingerprints, takes post-processing well, and reads as premium at first contact.

The Open Cube is recommended as a secondary build for users who prioritize development access. The Egg is the right choice for a high-craft display piece. The Radio is the right choice for users who want J.A.R.V.I.S. to feel warm rather than cold.
