# Concept 4 — Radio: Architectural Plan

## Rationale

The Radio takes J.A.R.V.I.S. into a different tonal register: warmth and authority rather than cold precision. The form references a vintage Braun table radio — landscape orientation, vertical grille slots, a circular display as the tuning dial, a cylindrical status knob on top. In walnut-look filament (or actual walnut veneer wrap) with a brass-colored PLA face panel, it reads as bespoke desk hardware with character. The cylindrical knob serves as a hollow LED light pipe — no functional rotating parts.

Interior strategy: landscape cavity. Battery on the left-floor. Amp+speaker occupies the right-mid section, speaker dome facing the front grille. XIAO mounts near the front-left, USB-C exiting through a slot in the lower front face adjacent to the display. Lid is the rear panel, accessed via 4x M2 screws on the back.

## Outer Dimensions

- Width: 78 mm
- Depth: 60 mm
- Height: 55 mm
- Footprint on desk: 78 x 60 mm

## Component Layout

```
Front view

              78 mm
  +------------------------------+
  | (knob)                       |  <- status knob top-right, LED pipe
  +---------+--------------------+
  |         |    [  grille  ]    |
  | ( disp) |    [  grille  ]    |  <- 39 mm dial left, vertical slots right
  | ( 39mm) |    [  grille  ]    |
  |         |    [  grille  ]    |
  | [USB-C] |                    |  <- USB-C bottom-left below display
  +---------+--------------------+
                55 mm tall
```

```
Top view (rear panel removed)

              78 mm
  +------------------------------+
  | [BATTERY 52x35]              |  <- battery left-floor
  |                              |
  | [XIAO 21x17.5]               |  <- XIAO front-left, USB-C faces front
  |                 [AMP+SPEAKER]|  <- 50x30 module right-mid
  |                 [  50 x 30  ]|  <- speaker dome faces front grille
  +------------------------------+
              60 mm deep
```

```
Cross-section (X-Z, side cut at Y=center)

  Z=55 +---------o-------+  <- top (knob protrudes 10 mm above)
       |         | knob  |
  Z=40 |[XIAO  ] |       |
       |         |       |
  Z=22 |    [AMP+SPEAKER ]|  <- 18 mm module height
  Z= 8 |[BATTERY          ]  <- 6 mm cavity
  Z= 2 +------------------+
  Z= 0
```

```
Exploded view

  [Status knob — presses onto top boss, no fastener]
  [Rear panel lid — 4x M2 screws into rear wall bosses]
  [XIAO — mounted front-left near display cutout]
  [AMP+SPEAKER — right-mid cavity, dome toward front grille]
  [BATTERY — floor, left side]
  [Body — landscape box, walnut-look or veneer wrap]
  [Blank front plate — snaps into 39 mm dial cutout]
```

## Assembly Steps

1. Print body in walnut-look PLA (Eryone Wood PLA or eSun ePLA-Wood). 4 perimeters. Print in landscape orientation (largest flat face on bed, front face up-vertical). Speaker grille slots are front-face features; no support needed for vertical slots.
2. Optionally wrap outer sides in walnut veneer (0.5 mm self-adhesive) for premium finish.
3. Heat-set 4x M2 brass inserts into rear wall boss pillars using iron at 220 C.
4. Lay battery on floor left section between ribs.
5. Slide amp+speaker module into right-mid cavity. Speaker dome faces front grille slots.
6. Mount XIAO front-left, USB-C aligned to front slot. Use double-sided foam tape.
7. Press status knob onto top-right boss (friction fit; interior LED wire routes through hollow core).
8. Snap blank front plate into 39 mm cutout.
9. Set rear panel lid against back. Thread 4x M2 x 6 mm cap-head screws.

Total assembly time: approximately 20 minutes.

## Print Orientation and Supports

- Body: print lying on side (largest flat face down). Front face is vertical during print. Vertical grille slots require no bridging — they are open at top and bottom. No supports required.
- Rear lid: print face-down. Flat part, no overhangs.
- Status knob: print standing up. Hollow core is a simple cylinder — bridge at top (2 mm span), no support needed.
- Blank plate: print face-down.

## Cooling and Airflow

The vertical grille slots provide direct airflow to the speaker and amp module. The knob hollow provides a small chimney path for the XIAO heat. Rear panel is the most sealed of the four concepts — minimal passive convection. For the power levels involved this is acceptable. If future ESP32 workloads increase, adding two 3 mm ventilation holes to the rear panel is trivial.

## Phase 3 Upgrade Path

Phase 1: blank plate in 39 mm dial cutout.
Phase 3: remove blank plate, snap in GC9A01 + bezel ring. The display sits left-front, XIAO is immediately behind it — the 7-pin SPI ribbon connection is short and direct. No body modification required.

## Material Notes

For the authentic Radio aesthetic:
- Body shell: wood-fill PLA (Eryone or Polymaker PolyWood), sanded to 400 grit, wax-finished.
- Front accent panel: print separately in gold or brass-look PLA, bond with M1.6 screws or snap-fit tabs.
- Status knob: print in chrome-look or metallic silver PLA.
- Rear lid: plain matte grey PLA (functional, not visible in use).
