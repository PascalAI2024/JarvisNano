# Concept 3 — Egg: Architectural Plan

## Rationale

The Egg is the cleanest industrial design of the four. Every edge is fillet-continuous. The form reads as a single cast object rather than an assembled box. It suits an audience who wants J.A.R.V.I.S. to feel like a designed product, not a hobbyist build. The tradeoff is print complexity: the egg shape requires careful orientation and potentially some support material at the equator joint.

Interior strategy: horizontal split at equator (Z=28 mm). Bottom half houses battery and amp+speaker. Top half is the dome — contains XIAO near the front wall, speaker fires upward through dome perforations. The two halves join with 4x M2 screws at the equator.

The egg's narrowing profile at top means the XIAO fits comfortably but the amp+speaker (50 x 30 mm footprint) requires the lower half, which at Z=28 mm has sufficient internal width (~64 mm clear) to accommodate it lengthwise.

## Outer Dimensions

- Width: 72 mm (maximum, at equator)
- Depth: 62 mm (maximum)
- Height: 68 mm
- Footprint on desk: 72 x 62 mm (smallest footprint of the four concepts)

## Component Layout

```
Front view

          72 mm (max at equator)
     _____________________
    /                     \    <- top dome
   /  [speaker grille top] \   <- perforations through dome
  |                         |
  |      ( display )        |  <- 39 mm cutout, front face
  |      (  39 mm  )        |     center at Z = 26 mm
  |                         |
  |  [USB-C]  [mic port]   |   <- front lower, near equator
   \                       /
    \_____________________ /   <- base (flat for stability)
             28 mm
```

```
Top view (equator cross-section, 72 x 62 mm)

    _____________________________
   /                             \
  | [XIAO 21x17.5] front-biased  |  <- near front wall
  |                               |
  |  [AMP + SPEAKER   50 x 30 ]  |  <- centered-rear lower half
  |                               |
  |  [BATTERY      52 x 35    ]  |  <- floor of bottom half
   \_____________________________ /
```

```
Cross-section (Y-Z, dome shape)

  Z=68    o       <- top of dome (narrow)
  Z=60  [====]   <- speaker grille perforations
  Z=50  [XIAO ]  <- XIAO board, dome interior
  Z=40  |      |
  Z=28  +======+  <- equator join (4x M2 screws)
  Z=20  |[AMP ]|  <- 18 mm module
  Z= 8  |[BAT ]|  <- 6 mm cavity
  Z= 2  |      |
  Z= 0  +------+  <- flat base
```

```
Exploded view

  [Top dome half — speaker grille through dome, XIAO inside]
  [4x M2 screws at equator]
  [Bottom half — battery floor, amp+speaker mid, M2 boss pillars]
  [Blank plate — snaps into 39 mm front cutout]
```

## Assembly Steps

1. Print bottom half floor-down. Print top half dome-up (equator face on bed). Both require no supports if oriented correctly.
2. Note: the equator face of each half is flat — this is the print bed face, ensuring a clean mating surface.
3. Heat-set 4x M2 brass inserts into bottom half boss pillars at equator level.
4. Lay battery on bottom half floor. Route leads up.
5. Place amp+speaker in bottom half mid-section. Speaker dome faces upward into space between it and the equator joint — airflow path to dome grille.
6. Set XIAO into top dome half, align USB-C with front slot near equator.
7. Mate two halves. Thread 4x M2 x 6 mm cap-head screws through top half clearance holes into bottom half inserts.
8. Snap blank front plate into 39 mm cutout.

Total assembly time: approximately 25 minutes.

## Print Orientation and Supports

- Bottom half: print equator face down (flat face on bed). The outer egg curve overhangs on the base are less than 45 degrees over most of the profile. Small support may be needed at the very base corners where the egg narrows. Recommended: tree supports, touching buildplate only.
- Top half: print equator face down. Speaker grille holes through the dome are vertical when printed this way — no bridging required.
- Blank plate: print face-down on smooth PEI.

## Cooling and Airflow

The speaker fires upward through dome perforations, which creates a natural chimney effect. Warm air from the XIAO in the upper dome exits the same perforations. The equator joint is not fully sealed — minor air exchange through the M2 boss gaps. Thermal performance is adequate for the low-power components involved.

## Constraint Note

The egg outer profile narrows below the equator on the base side. The amp+speaker module (50 x 30 mm) is 50 mm wide. The inner cavity at Z=10 mm (above the base) is approximately 60 mm wide — just enough margin (5 mm each side) for the module to sit with wiring clearance. This is the tightest fit of all four concepts. If a future larger module is selected, this concept would need to grow to OW=78 mm.

## Phase 3 Upgrade Path

Phase 1: blank plate in front cutout.
Phase 3: remove blank plate, insert GC9A01 display + bezel ring. The +6 mm display stack fits within the top dome cavity (approximately 20 mm of clearance above XIAO). No body modification required.
