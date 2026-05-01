# J.A.R.V.I.S. Enclosure — Hardware Design

This directory contains four distinct enclosure concepts for the JarvisNano desktop AI agent. All four are designed around the same internal component set and share the same Phase 1/Phase 3 upgrade strategy.

## Component Assumptions

All dimensions are derived from the canonical component table below. No sizes were invented.

| Component | Dimensions (W x D x H, mm) | Design note |
|-----------|----------------------------|-------------|
| Seeed XIAO ESP32-S3 Sense | 21 x 17.5 x 3.5 | USB-C on short edge; externally accessible. PDM mic port: 1.8 mm dia hole, 3 mm from USB-C edge. |
| PAM8002A amp + 28 mm speaker (Option A — adopted) | 50 x 30 x 18 | Larger option chosen to guarantee fit. Option B (MAX98357A 16x20x3 + separate 28 mm speaker) fits trivially in the same cavity. Speaker dome needs 2 mm dia grille holes. |
| LiPo battery cavity | 52 x 35 x 6 | Fits both 503040 (600 mAh) and 503450 (850 mAh). Use foam shim for the thinner cell. Soldered leads, no swap door. |
| Round Display for XIAO (Phase 3) | 39 mm OD, 32 mm glass, +6 mm height | Front face has 39 mm circular cutout from day one. Phase 1: blank plate. Phase 3: display + bezel ring. |

### Why Option A

Option A (PAM8002A with built-in speaker dome, 50 x 30 x 18 mm) is used as the design basis because it is the larger of the two amp options. If Option B (separate MAX98357A + speaker) is selected, it fits with room to spare in the same cavity, requiring no enclosure modification.

## Concept Overview

| Concept | Directory | Mood | Interior Strategy | Lid Type |
|---------|-----------|------|-------------------|----------|
| 1 — Monolith | concept-1-monolith/ | Matte charcoal slab, horizontal LED slit | Vertical layer stack: battery floor, speaker mid, XIAO top | Top panel, 4x M2 cap-head |
| 2 — Open Cube | concept-2-cube/ | Aluminum-look, visible hex M2, clear top | PCB slide-out tray + rails, speaker fires forward | Top panel with window, 4x M2 cap-head |
| 3 — Egg | concept-3-egg/ | Smooth curves, pebble silhouette | Horizontal equator split, speaker fires upward through dome | Top half, 4x M2 at equator |
| 4 — Radio | concept-4-radio/ | Walnut + brass, landscape, status knob | Landscape cavity, speaker fires forward through vertical slots | Rear panel, 4x M2 |

## Concept Comparison Matrix

| Attribute | Monolith | Open Cube | Egg | Radio |
|-----------|----------|-----------|-----|-------|
| Footprint (W x D, mm) | 78 x 68 | 76 x 66 | 72 x 62 | 78 x 60 |
| Height (mm) | 66 | 65 | 68 | 55 (+10 knob) |
| Print time (est. @ 0.2 mm layer) | 4.5 h | 4.0 h | 5.5 h (two halves) | 3.5 h |
| Filament estimate (PLA/PETG) | ~55 g | ~50 g | ~60 g | ~48 g |
| Screw count | 4 | 4 | 4 | 4 |
| Assembly time (est.) | 20 min | 25 min | 25 min | 20 min |
| Visual mood | Cold precision | Industrial transparency | Organic premium | Warm authority |
| Phase 3 screen swap difficulty | 1 | 1 | 2 | 1 |
| Recommended for Phase 1 | Yes (primary) | Yes | Yes | Yes |
| Recommended for Phase 3 | Yes (primary) | Yes | Yes | Yes |
| Overall recommendation | PRIMARY | Secondary | Premium alt. | Niche/unique |

## Materials

- Monolith: PETG or ASA in matte charcoal (recommended: Prusament PETG Jet Black or Polymaker PolyMax PETG)
- Open Cube: Silver or space grey aluminum-look PLA (Polymaker Metallic Silver or Eryone Matte Silver PLA)
- Egg: Matte white or warm grey PETG (Prusament PETG Pearl Mouse or Bambu PLA Matte Beige)
- Radio: Wood-fill PLA body (Eryone Matte Wood Brown or PolyWood Bambu), separate brass-look PLA face panel

## Bill of Materials (per build, all concepts)

| Item | Qty | Source |
|------|-----|--------|
| M2 x 6 mm cap-head screws | 4 | McMaster-Carr or local hardware |
| M2 brass heat-set inserts (3.2 mm OD x 4 mm L) | 4 | Amazon or AliExpress |
| 3M double-sided foam tape 1 mm | 1 strip | For XIAO and module retention |
| Foam shim 52 x 35 mm x 1-2 mm | 1 | For battery if using thinner 503040 cell |
| (Optional) 5 mm LED for light pipe (Concept 4 knob) | 1 | Any electronics supplier |

## Assembly Notes

- Heat-set inserts: use a soldering iron at 220 C. Press slowly with a flat tip until the insert is flush with the plastic. Let cool 60 seconds before threading.
- Battery wiring: solder leads before inserting battery into cavity. Verify polarity (BAT+ and GND pads on XIAO underside) before sealing.
- Speaker wiring: the PAM8002A takes a 3.3V PWM or analog audio signal. Wire speaker +/- to the module output terminals.
- Acoustic mic port: the 1.8 mm hole must be unobstructed and not covered by foam tape when placing the XIAO.
- Display upgrade (Phase 3): the GC9A01 module plugs into the XIAO via 7-pin SPI. Ribbon length with the display seated in the front cutout is approximately 5 mm — use the shortest ribbon that fits.

## OpenSCAD Rendering

Each concept directory contains an `enclosure.scad` file renderable with:

```
openscad -o enclosure.stl enclosure.scad
```

To export individual parts, comment/uncomment the render calls at the bottom of each file. The files use standard OpenSCAD built-ins (cube, cylinder, sphere, hull, minkowski, difference, intersection, translate, rotate, for) — no external libraries required.
