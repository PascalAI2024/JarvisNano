# JarvisNano — AMOLED-1.75 Enclosure Concepts

Enclosure designs for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** variant of JarvisNano. Different form factor than the XIAO Sense (round PCB instead of rectangular), so this lives in its own subdir.

## Component Assumptions

All dimensions derived from the canonical Waveshare drawing
(`files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75/ESP32-S3-Touch-AMOLED-1.75-3D.zip`).

| Component | Dimension | Note |
|---|---|---|
| PCB diameter | ⌀46.0 mm | Round board |
| Cover-glass diameter | ⌀48.96 mm | AMOLED bezel sits proud of PCB |
| Display visible (VA) | ⌀44.16 mm | Active picture area |
| Display height above PCB | 2.05 mm | Adds to total Z height |
| PCB thickness | ~1.10 mm | |
| Total stack height (Z) | ~10 mm | PCB + display + bottom-side components |
| Mounting holes | 3× M2 on ⌀46 mm bolt circle, 120° apart | At radius 23.0 mm from center |
| USB-C | jacks out radially from edge | one edge of the board |
| microSD slot | projects radially, ~16.5 mm | opposite side from USB-C |
| SPK MX1.25 connector | on top edge | for external speaker |
| BAT MX1.25 connector | on top edge | for 3.7 V Li-Po |
| MIC1 + MIC2 | edge-mounted MEMS mics | far-field AEC pair |
| PWR + BOOT buttons | opposite side edges | side-press |

## Concept Overview

| Concept | Mood | Form factor | Status |
|---|---|---|---|
| 5 — Mascot Bust | Matte black + orange-neon chibi character, AMOLED is the face | Vertical bust ~100 mm tall | **Primary** |
| 6 — Disc on Angled Stand | Minimal puck on pedestal, screen tilted ~25° toward user | Compact ~60×60×60 mm | Alternative |

## Why two concepts

Concept 5 commits to the J.A.R.V.I.S. brand identity (mascot face on the front). Concept 6 is the practical/elegant fallback if you want something less character-driven on a desk where a chibi figurine would feel out of place (work meeting room, kitchen counter, etc.).

Both share the same component assumptions and mounting strategy — just different shells around the board.

## Rendering

Each concept directory contains an `enclosure.scad` file renderable with:

```bash
openscad -o enclosure.stl enclosure.scad
```

The OpenSCAD files are parametric — adjust the variables at the top of each file to tune wall thicknesses, button cutout sizes, screw type, etc.

## Bill of Materials (per build, both concepts)

| Item | Qty | Notes |
|---|---|---|
| M2 brass heat-set inserts (3.2 mm OD × 4 mm L) | 3 | Mount the board to the front shell |
| M2 × 6 mm cap-head screws | 3 | Through-shell fasteners |
| 28 mm speaker, 8 Ω 1 W | 1 | Wired to SPK MX1.25 connector |
| MX1.25 2-pin cable, ~50 mm | 1 | Speaker pigtail |
| 3.7 V LiPo 503450 (850 mAh) | 1 (optional) | Hot-swap not supported — solder leads |
| MX1.25 2-pin battery cable | 1 (optional) | Pre-wired to LiPo |
| 3M VHB / double-sided foam, 2 mm | small strip | Speaker dampening |

Heat-set inserts: press in at 220 °C soldering iron, slow.
