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

---

## The wake-on-motion register sequence — sourced, not guessed

Resolved 2026-08-29 from [`lewisxhe/SensorLib`](https://github.com/lewisxhe/SensorLib)
(`src/SensorQMI8658.hpp`, `configWakeOnMotion()`), a mature and widely-deployed
QMI8658 driver. This is recorded here because the sequence is **not** in this
repo — `jr_imu.c` implements only `WHO_AM_I`, `CTRL1/2/7` and the axis burst —
and it must never be written from memory.

### Registers

```
CTRL1 0x02   CTRL2 0x03   CTRL3 0x04   CTRL5 0x06
CTRL7 0x08   CTRL8 0x09   CTRL9 0x0A
CAL1_L 0x0B  CAL1_H 0x0C  STATUS1 0x2F  RESET 0x60
```

### Sequence

1. Reset the device.
2. Clear `CTRL7` bit 0 (disable sensors).
3. `CTRL2`: set accel range (bits 4-6) and ODR (low bits) — low-power 128 Hz is the driver's default for WoM.
4. `CAL1_L` ← **WoM threshold**, resolution 1 mg (driver default 200 mg).
5. `CAL1_H` ← `(pin_sel << 6) | (blanking_time & 0x3F)`, where **`pin_sel = 0x02` selects INT1** with an initial value of 1 (`0x00` selects INT1 initial 0; `0x01`/`0x03` select INT2). Blanking time is a count of accelerometer samples ignored after enabling, to suppress false triggers — driver default `0x20`.
6. Issue CTRL9 command **`0x08`** (`CTRL_CMD_WRITE_WOM_SETTING`): write the command to `CTRL9` (0x0A), then poll `STATUS_INT` until the command-done bit sets.
7. Re-enable the accelerometer, then enable the chosen interrupt pin.

Motion then reports through `STATUS1` bit 9 (`WOM_MOTION`, mask `0x0200`).

**`pin_sel = 0x02` is the value this board needs**, because our routed interrupt
is INT1 (above).

### ⚠️ The constraint that changes the design

The driver documents it plainly:

> *"Configuring WoM will reset the sensor, set the function to WoM, and there
> will be no data output."*

**WoM mode and normal sampling are MUTUALLY EXCLUSIVE.** While WoM is armed the
part produces no readings — so `pitch/roll`, `orientation`, `motion_mg`,
`moving` and `shake` all stop. That means **flip-to-mute (GEST-02) and
shake-to-cancel (GEST-03) do not work while WoM is armed**, and neither does the
tilt feed to the HUD.

So WoM cannot simply be "turned on". It has to be a **mode switch tied to the
rest ladder**:

- **AWAKE / AMBIENT** — normal sampling. Flip, shake and tilt all live. This is
  where the user actually interacts, so nothing is lost.
- **WHISPER / DREAM** — stop the sampler (`jr_imu_stop()` already exists and has
  zero callers), arm WoM on INT1, and let the interrupt bring the device back.
  Flip and shake are unavailable here, which is acceptable *only because* the
  device is at rest and any motion at all is about to wake it anyway.

The transition back must reconfigure normal mode and restart the sampler before
anything reads a snapshot, or the first post-wake `jr_imu_read()` returns stale
data.

### Do not implement this blind

It is a sensor mode-switch that, if wrong, silently disables flip-to-mute and
shake-to-cancel — two shipped gestures, one of them a privacy control. It needs:

1. The GPIO21 probe (confirm the pin toggles when WoM fires) — the schematic
   evidence above is strong but unproven on hardware.
2. A physical test of the full cycle: rest → WoM armed → move → wake → flip and
   shake still work.

Neither can be done without hands on the device.
