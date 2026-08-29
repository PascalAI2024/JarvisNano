# QMI8658 interrupt routing on the 1.75C — resolved from the schematic

**Status:** resolved 2026-08-29 from the vendor schematic PDF. Supersedes every
"INT2 → GPIO21" claim in this repo, which came from the **1.75** board.

**Source:** `ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf`, page 1, part **U3**
(`QMI8658`, labelled "6/9-Axis", I²C address `0x6B`), from
[waveshareteam/ESP32-S3-Touch-AMOLED-1.75C](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/blob/main/Schematic).

## What the schematic shows

U3 pinout as drawn:

```
 1  SDO/SAO ── GND            2/3   SDx / SCx
14  SDA     ── ESP32_SDA      10/11 NC
13  SCL     ── ESP32_SCL      8/5   VDD / VDDIO ── VCC3V3
12  CS      ── VCC3V3         6/7   GND
 4  INT1    ── QMI_INT1   ← the routed one
 9  INT2    ── QMI_INT2   ← goes nowhere
```

Directly above the part, drawn as a net-alias pair:

```
QMI_INT1 ——— GPIO21
```

## The two facts that matter

| Fact | Evidence |
|---|---|
| **INT1 (pin 4) is the interrupt that reaches the MCU**, aliased to **GPIO21** | the net-label pair above U3, read from the rendered page |
| **INT2 (pin 9) is NOT connected to the ESP32** | the string `QMI_INT2` occurs **exactly once** in the entire 3-page document — at the part pin. A net that appears once has no other endpoint. |

So **any plan targeting INT2 is dead on this board.** Use **INT1**, and configure
the QMI8658 to map its wake/any-motion/no-motion/tap engines onto **INT1** rather
than INT2.

## What this corrects

`components/jr_imu/include/jr_imu/jr_imu.h` states Phase 5 "replaces the poll
entirely with the QMI8658's own No-Motion/Any-Motion/Tap engines on **INT2 →
GPIO21**". The pin is right; **the interrupt number is wrong**. That line, and
the archived plan rows repeating it, came from the 1.75 schematic.

## Residual uncertainty — read before writing wake code

The page also carries a GPIO pin-assignment table in which `QMI_INT1` renders on
a row that reads `GPIO38 / QSPI_SCL`, one row below an empty `GPIO21` row. I could
not resolve that table's column semantics at any zoom, and it disagrees with the
net label at the part.

**The net label is the stronger evidence** — it is drawn at the component, in the
form Altium uses for a net alias, and it is corroborated by `QMI_INT2` having no
second endpoint. But before enabling an interrupt, **confirm on hardware**:
configure the engine, then check GPIO21 actually toggles. That is one cheap probe
and it settles it; do not skip it on the strength of this document.

## Why this was blocking

Lift-to-wake, any-motion wake and every deep-sleep wake source depend on this pin.
It is also the prerequisite for retiring the continuous IMU poll. Carrying 1.75
routing onto the C is the same class of error that caused a wrong-variant flash
and a phantom "IMU inverted" bug in one session — see the board-variant notes.
