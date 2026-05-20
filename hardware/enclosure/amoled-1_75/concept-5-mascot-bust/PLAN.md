# Concept 5 — Mascot Bust

## Vision

Chibi J.A.R.V.I.S. character bust, ~100 mm tall, AMOLED IS the face. Matte black PETG (head + body), orange accent ring around the AMOLED bezel (the "visor" glow).

Sets on a desk. Looks at you. Speaks. Listens. Tap the chest to acknowledge. The face animates with the agent's state (idle pulse, listening waveform, thinking spinner, speaking lipsync).

## Form

```
        ╭─────────╮          ← head crown (printable separately for fit)
       │  ┌───────┐  │         orange accent ring sits between
       │  │ AMOLED │  │        the head shell and the crown
       │  └───────┘  │
        ╲   ▒▒▒    ╱           ← grille slots = chin / "voice"
         ╲ ──── ╱
          ┃    ┃                ← neck
        ┌─┴────┴─┐
        │  ◉      │              ← chest button (PWR pass-through)
        │  ┌──┐   │              ← optional decorative core badge
        │  └──┘   │
        │         │              ← body shell, holds speaker + battery
        └─────────┘
           ╲   ╱                ← base pedestal, USB-C cable out the back
            ╲ ╱
            ▔▔▔
```

Dimensions:

| Part | W × D × H | Notes |
|---|---|---|
| Head shell (outer) | ⌀60 × 60 mm | Sphere-ish capsule, AMOLED set 1 mm proud of front face |
| AMOLED window cutout | ⌀48 mm (slight clearance over cover-glass) | Board mounts to inner ring at the 3× M2 holes |
| Neck | ⌀22 × 12 mm | Hides the USB-C ribbon down to base, allows ~5° head tilt down |
| Body shell | 50 × 30 × 50 mm rounded rect | Holds speaker chamber + LiPo cavity |
| Base pedestal | 70 × 50 × 15 mm | Counterweight for stability, USB-C grommet out the back |
| **Total assembled** | ~70 × 50 × 105 mm | Approx |

## Internal layout

- **Inside the head**: AMOLED board mounted to a 3-spoke inner ring matching the ⌀46 mm × 3-hole pattern (M2 heat-set inserts at radius 23 mm, 120° apart). Mic1/Mic2 ports are 1.8 mm dia through-holes at the appropriate edge positions, drilled at 12° upward angle so they catch your voice from above. PWR + BOOT buttons get printed-in flex tabs from the head shell — a finger-press on the outside transfers force to the on-board button.
- **Through the neck**: short flat ribbon (or just the through-hole 8-pin pigtail) from the AMOLED-1.75's 8-pin connector down to a speaker-amp board in the body, and through to USB-C in the base.
- **Inside the body**: a 28 mm dia × 18 mm tall speaker chamber fires forward through angled chest grille slots (the "voice"). A 52 × 35 × 6 mm battery cavity sits behind the speaker, soldered leads (no swap door — matches XIAO enclosure pattern).
- **Inside the base**: USB-C right-angle pigtail routed up through the body to the AMOLED board's USB connector. The base also holds a small (optional) tactile chest button that the user can wire to one of the 8-pin header GPIOs as a confirmation input.

## Assembly order

1. Heat-set 3 × M2 inserts into the head's inner ring (radius 23 mm, 120° apart).
2. Solder the speaker pigtail (MX1.25 2-pin) and battery pigtail to their respective leads.
3. Drop the speaker into its chamber, foam-pad it, route the pigtail up through the neck.
4. Slot the AMOLED board into the inner ring of the head, USB-C edge pointing down toward the neck.
5. Connect the SPK pigtail to the board's MX1.25, and the BAT pigtail similarly.
6. Mount the AMOLED board with 3 × M2 cap screws through the back of the head into the heat-set inserts.
7. Snap the head's back panel on (friction-fit + 2 small M1.6 hidden screws on the underside of the head).
8. Slide the head onto the neck stub (friction-fit, with a single M2 grub screw from the back of the neck to keep it from rotating).
9. Press the base onto the body's bottom, USB-C grommet aligned at the back.

## Print parameters

- **Layer height**: 0.16 mm (FDM) — the face shell wants smooth curves
- **Infill**: 25 % gyroid for the head, 35 % for the body (more rigidity around speaker), 60 % for the base (mass for stability)
- **Walls**: 4 perimeters
- **Material**: Matte black PETG (Prusament Jet Black or Polymaker PolyMax PETG Black)
- **Accent ring**: separate part, printed in orange or post-painted (Bambu PLA Matte Orange or Eryone Matte Orange)
- **Supports**: tree supports under the chin overhang, the neck-to-base transition, and the speaker chamber roof
- **Estimated print time**: 6 h total for all parts at 0.16 mm
- **Estimated filament**: ~95 g black + 8 g orange

## What's parametric in enclosure.scad

The OpenSCAD model exposes these variables at the top so this design can be tuned without re-modeling:

| Variable | Default | What it changes |
|---|---|---|
| `pcb_diameter` | 46.0 | Inner ring diameter — increase clearance if needed |
| `pcb_thickness` | 1.1 | Inner ring offset from face |
| `cover_glass_diameter` | 48.96 | Front face cutout for AMOLED bezel |
| `head_outer_diameter` | 60.0 | Head shell outside diameter |
| `head_wall` | 2.4 | Head wall thickness |
| `mount_hole_radius` | 23.0 | Bolt circle for board |
| `mount_hole_count` | 3 | Number of mounting screws |
| `mount_hole_dia` | 2.2 | Hole for M2 self-tapping or heat-set |
| `neck_diameter` | 22 | Visual neck width |
| `neck_height` | 12 | Head-to-body separation |
| `body_w / body_d / body_h` | 50 / 30 / 50 | Body shell dimensions |
| `speaker_diameter` | 28 | Voice chamber |
| `battery_w / battery_d / battery_h` | 52 / 35 / 6 | Cavity for 503450 LiPo |
| `base_w / base_d / base_h` | 70 / 50 / 15 | Pedestal |
| `mic_port_dia` | 1.8 | Acoustic port through the head shell |
| `mic_port_angle_up` | 12 | Tilted upward so mics catch voice from above |
| `usb_grommet_dia` | 12 | USB-C cable exit on back of base |

## Open questions

- Does the AMOLED's cover-glass sit flush with the front of the head, or recessed? Default plan: 0.5 mm recessed so the head edge "protects" the screen from desk drops.
- Is the chin a fixed grille or a swappable mesh insert? Default plan: integrated diagonal slots (fewer parts).
- Does the chest button do anything in firmware yet? Default plan: wired to GPIO16 from the 8-pin header; firmware adds a Lua skill in phase 6.

## Status

Plan only — `enclosure.scad` is the next deliverable in this directory.
