# Concept 2 — Open Cube: Architectural Plan

## Rationale

The Open Cube interprets J.A.R.V.I.S. through precision manufacturing aesthetics: the kind of enclosure you might find protecting a scientific instrument. Aluminum-look PLA (silver or space grey), exposed hex M2 cap-heads at the four corners of the lid, and a clear acrylic-style top panel revealing the PCB. The PCB tray is a removable insert that slides out from the top for service — no lid needed to access the XIAO for flashing.

Interior strategy: PCB tray system. The XIAO sits in a slide-out carrier tray that drops into top-section rails. Battery lies on the floor. Amp+speaker occupies the left mid-cavity. Speaker fires forward through a hex-pattern grille on the front face.

## Outer Dimensions

- Width: 76 mm
- Depth: 66 mm
- Height: 65 mm
- Footprint on desk: 76 x 66 mm

## Component Layout

```
Front view

          76 mm
  +---------------------------+
  | [M2] [lid visible] [M2]  | <- lid with 4x M2 hex caps visible
  |  - - - - - - - - - - -   |
  |                           |
  |  [hex grille] ( display ) | <- hex grille left, 39 mm cutout right-center
  |  [hex grille] (  39 mm  ) |
  |  [hex grille]             |
  |       [LED strip]         | <- 30 mm LED bar above display
  | [USB-C][mic]              | <- bottom-left, XIAO USB-C
  | [M2]              [M2]   |
  +---------------------------+
```

```
Top view (lid removed, PCB tray visible)

          76 mm
  +---------------------------+
  |  [rail]             [rail]| <- 2 mm rails on inner walls
  |     [ PCB TRAY 70x26 ]   | <- tray slides in from top
  |     [ XIAO on standoffs] |
  |                           |
  |  [ AMP + SPEAKER 50x30 ] | <- left-mid section
  |                           |
  |  [ BATTERY 52x35        ]| <- floor
  +---------------------------+
```

```
Cross-section (Y-Z)

  Z=65 +=========+  <- lid (acrylic-look top, window cutout)
       |rail  rail|
  Z=52 |[XIAO    ]|  <- PCB tray level
       |          |
  Z=34 |[AMP+SPKR]|  <- 18 mm module
  Z=16 |          |
  Z= 8 |[BATTERY  ]|  <- 6 mm cavity
  Z= 2 +----------+
  Z= 0
```

```
Exploded view

  [Lid — acrylic-look panel with window, 4x M2 clearance holes]
  [PCB tray — slides up out of body, XIAO stays attached]
  [AMP+SPEAKER module — sits in left mid-pocket]
  [BATTERY — floor]
  [Body — sharp corners, hex grille front, rails inside]
```

## Assembly Steps

1. Print body in silver or space grey aluminum-look PLA. 4 perimeters, 15% infill. Sharp corners — no support needed for any feature if oriented floor-down.
2. Print PCB tray separately. XIAO mounts on 4 corner standoffs using M1.6 screws or conductive foam pads.
3. Heat-set 4x M2 brass inserts into floor boss pillars.
4. Lay battery on floor between ribs. Route leads to XIAO.
5. Place amp+speaker in left cavity. Speaker dome faces front hex grille.
6. Slide PCB tray down into body rails (top-to-bottom). Tray self-indexes against floor stop.
7. Set lid (clear or printed acrylic-look). Thread 4x M2 x 8 mm cap-head screws.
8. For flashing: unscrew 4x M2, lift lid, pull tray up by 20 mm, connect USB-C without removing tray fully.

Total assembly time: approximately 25 minutes (PCB tray alignment adds time).

## Print Orientation and Supports

- Body: floor-down. Hex grille holes on front face are 2 mm diameter — FDM bridges these in one pass per layer. No supports.
- Lid: flat. The large rectangular window is a through-hole — no supports.
- PCB tray: print tray-floor-down. Standoffs are vertical features, no supports.

## Cooling and Airflow

The window cutout in the lid and the hex front grille create a passive chimney effect: warm air rising from the ESP32 exits through the window. The amp/speaker heat is minimal. The open design (window in lid) is the most thermally permissive of the four concepts.

## Phase 3 Upgrade Path

Phase 1: blank plate in 39 mm front cutout.
Phase 3: remove blank plate. GC9A01 display assembly snaps in. The +6 mm display height stacks above XIAO inside the tray — PCB tray height accommodates if XIAO is on 2 mm standoffs (tray designed for this). No body modification required.

## Notes on Amp Module Choice

Designed for Option A (PAM8002A, 50 x 30 x 18 mm). Speaker dome points toward front hex grille. Option B (MAX98357A + separate 28 mm speaker) would fit in the same left mid-cavity with room to spare.
