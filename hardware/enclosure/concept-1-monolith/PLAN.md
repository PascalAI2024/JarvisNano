# Concept 1 — Monolith: Architectural Plan

## Rationale

The Monolith is the primary recommendation. It achieves the J.A.R.V.I.S. identity most directly: a matte charcoal rectangular slab that looks like it belongs next to a Mac Pro. The form vocabulary is borrowed from high-end studio monitor speakers and luxury desktop accessories. Nothing curves unnecessarily. The single horizontal LED slit reads as a status eye — intent without decoration.

Interior strategy: vertical layer stack. Battery on the floor. Amp+speaker module occupies the middle-right cavity. XIAO board mounts to a standoff shelf near the top, behind the front wall, with USB-C exiting through a slot in the front face. The round display cutout occupies the front face below the LED slit.

## Outer Dimensions

- Width: 78 mm
- Depth: 68 mm
- Height: 66 mm
- Footprint on desk: 78 x 68 mm

## Component Layout

```
Front view (not to scale — mm dimensions labeled)

          78 mm
  +---------------------------+
  |                           | <- top (lid, M2 screws in corners)
  |  [ LED SLIT  40 mm wide ] | <- 2.5 mm H slit, 10 mm from top
  |                           |
  |       ( display )         | <- 39 mm dia cutout, centered
  |       (  39 mm  )         |    center at Z = 25 mm from base
  |                           |
  | [USB-C]  [mic port o]     | <- USB-C slot left, mic 3 mm right of it
  |                           |
  +---------------------------+
         ^
         front face
```

```
Top view (X-Y cross-section, lid removed)

          78 mm
  +---------------------------+
  | WALL=2 |           | 2   |
  |        [ XIAO 21x17.5 ]  | <- XIAO centered X, front-biased Y
  |                           |
  |  [  AMP + SPEAKER  ]      | <- 50x30 mm module, right-rear
  |  [  50 mm W x 30 D ]      |
  |                           |
  |  [ BATTERY 52x35 mm ]     | <- centered X, floor of cavity
  |                           |
  +---------------------------+
```

```
Cross-section (Y-Z, center cut)

  Z=66 +---------+  <- lid top
       | LID 2.5 |
  Z=63 +=========+  <- lid / body join (M2 brass inserts in bosses)
       |         |
  Z=56 | [XIAO ] |  <- XIAO board, on standoffs, USB-C faces front
       |         |
  Z=40 +---------+
       | [AMP+  ]|  <- 18 mm tall speaker dome
       | [SPKR  ]|
  Z=22 +---------+
       | [BAT   ]|  <- 6 mm cavity
  Z= 2 +---------+  <- floor
  Z= 0            <- base
```

```
Exploded view (Y axis)

  [Phase3 bezel ring] -- snaps over display cutout
  [Blank plate / screen] -- snaps into 39 mm cutout
  [LID] -- 4x M2 cap screws to brass inserts in bosses
  [XIAO on standoffs] -- drops into top section
  [AMP+SPEAKER module] -- slides into mid-right pocket
  [BATTERY] -- lays on floor between ribs
  [BODY] -- single printed piece
```

## Assembly Steps

1. Print body and lid in matte charcoal PETG or ASA. Recommend 4 perimeters, 20% gyroid infill, no supports needed if oriented correctly (floor down).
2. Heat-set 4x M2 brass inserts into body boss pillars using soldering iron at 220 C. Press until flush.
3. Lay LiPo battery flat on floor between retention ribs. Route leads to XIAO BAT+/GND pads.
4. Slide amp+speaker module into middle-right pocket. Route speaker wires forward.
5. Place XIAO on standoffs, align USB-C with front slot. Tack XIAO with double-sided foam tape if desired.
6. Route all wires through cable channels. No connectors required at this assembly stage.
7. Snap blank front plate into 39 mm cutout until it clicks.
8. Set lid on body, align to lip, thread 4x M2 x 6 mm cap-head screws finger-tight, then quarter-turn with hex key.

Total assembly time: approximately 20 minutes including heat-set curing.

## Print Orientation and Supports

- Body: print floor-down. LED slit and USB-C slot are on the front wall — horizontal spans are under 10 mm, no support needed.
- Lid: print flat. No supports.
- Blank plate: print face-down on smooth PEI sheet for best surface finish.
- Speaker grille wall: the right side wall has through-holes — printed vertically, these are bridged spans of 2 mm diameter, well within FDM bridging capability.

## Cooling and Airflow

The PAM8002A generates minimal heat at the 1W operating level. The XIAO ESP32-S3 thermal dissipation is negligible. No active cooling is required. The acoustic mic port (1.8 mm, front wall) provides a small air exchange path. The LED slit provides a secondary convection path. The enclosure is not sealed — wire routing gaps at the lid joint provide adequate passive ventilation.

## Phase 3 Upgrade Path

Phase 1: blank plate snaps into front cutout. No modification to body.
Phase 3: remove blank plate, bond GC9A01 display (39 mm OD) to phase3_screen_bezel ring using UV adhesive. Snap bezel assembly into cutout. Connect 7-pin SPI header to XIAO. The +6 mm display height sits inside the top cavity which has 8 mm clearance above the XIAO board — fits without body modification.
