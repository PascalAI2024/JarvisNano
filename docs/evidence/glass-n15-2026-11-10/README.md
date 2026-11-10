# Wave N15 on the device, 2026-11-10

Image: `feat/glass-2` at 74b506f (2,418,048 bytes), flashed over Wi-Fi,
valid after 59 s of probation. Captures are the framebuffer mirror
(`/api/display/snapshot.ppm`): they prove what was composed, not that the
panel lit. Taken under a codex operator lease, so the rim is violet and DESK
is in the ring. The device was on battery (`ON CELL`, 86 %) and privacy-muted.

- `n15-screens.png`: home, the control shade, WEATHER, STATUS, DESK, ACTIVITY.
- `n15-dials.png`: the eight watch dials. The caption band is the style name
  shown for a moment after a style change.

## The walk

`GLASS QA PASS`: ring order WATCH > WEATHER > STATUS > DESK > ACTIVITY > home,
the face came back animated after a parked screen (2,300 lit pixels in the
centre box, 204 changing between two frames), parked WEATHER > up > WATCH
worked, levels read 90 / 85 before and after, `flush_errors 0`, largest
internal block 26,624 bytes (27,648 before the flash).

SKELETON's balance wheel moves: 7,950 pixels changed inside the wheel box
between two captures 0.35 s apart. CELESTIAL changed 1,342 pixels over the
whole dial in the same interval (stars and the seconds hand together).

## Where a frame's time goes

Milliseconds per frame from `render_us` and `layer_us` deltas over 3 s,
240 MHz, lease held. Flush time is not included.

| Screen | ms | dial | hud | shell |
|---|---|---|---|---|
| Home | 26.2 | 0.6 | 4.6 | 20.6 |
| Control shade | 78.6 | 1.3 | 56.0 | 20.7 |
| WATCH DRESS | 41.8 | 9.4 | 12.1 | 20.0 |
| WATCH CELESTIAL | 47.6 | 9.1 | 18.5 | 19.7 |
| WATCH DIVER | 49.5 | 9.3 | 19.7 | 20.1 |
| WATCH JARVIS | 53.7 | 1.2 | 32.4 | 19.6 |
| WATCH FUTURE | 55.2 | 9.3 | 25.5 | 20.0 |
| WATCH PILOT | 57.2 | 9.1 | 28.1 | 19.5 |
| WATCH SKELETON | 63.2 | 8.9 | 34.1 | 19.6 |
| WATCH MINIMAL | 88.7 | 1.4 | 66.7 | 20.0 |
| WEATHER | 53.4 | 0.9 | 31.9 | 20.0 |
| STATUS | 80.0 | 1.1 | 58.0 | 20.3 |
| DESK | 64.6 | 1.0 | 42.6 | 20.4 |
| ACTIVITY | 50.2 | 1.0 | 28.5 | 20.2 |

Canvas and surface are under 0.5 ms everywhere.

**The shell's 20 ms is the lease's rim ring, not the screen.** With the lease
released, over 5 s (the lease also holds the 240 MHz gear, which is the likely
reason WEATHER's hud reads slower here; the gear was not read back):

| Screen | ms | dial | hud | shell |
|---|---|---|---|---|
| WATCH DRESS | 23.2 | 10.7 | 12.0 | 0.1 |
| WEATHER | 44.8 | 1.0 | 42.9 | 0.1 |

So every frame-cost figure taken under a lease before this page carries about
20 ms that an owner without a lease never pays, and the rim ring itself is the
single most expensive thing the compositor draws. What is left is the hud
layer: the data screens' own content and the procedural MINIMAL dial.

## The board probe

`scripts/board-probe.py` and `--gyro`, device at rest on a desk:

| Probe | Answer |
|---|---|
| TE (GPIO13) | 60 Hz, period 16.70-16.72 ms |
| PWR key (GPIO3) | reads 0 at rest; a press was not observed |
| IMU | WHO_AM_I 0x05, die 36 C; gyro enabled for 20 samples and restored: mean -2.0 / -2.5 / 0.2 dps at rest |
| Touch | the driver reads 1 finger, the chip reports 2 |
| QSPI | 40 MHz (library default) |
| PMIC | vbat 3,980 mV. `die 385.7 C`, `vsys 0 mV` and `ts 8185 mV` are not believable: the conversion or the settle time after enabling the ADC channels is wrong. vbus 0 mV agrees with ON CELL |

## Seen in the captures, not yet fixed

- Under a lease the `CODEX MODE - DOUBLE TAP TO EXIT` caption overprints the
  shade's `PWR LISTEN · BOOT CLOSE` hint.
- The shade's brightness reads 19 % while the cap is 85: it shows the level
  the mood ladder has dimmed to, not the level the owner set.
- ACTIVITY rows cut a sentence to `FORGIVE ME, SIR. I.` and `I'M NOT QUITE.`
- The new DESK and ACTIVITY screens cost more than the ones they replaced
  (35 > 65 ms and 34 > 50 ms under the same lease).

## Second flash the same day: the frame-cost cuts, measured

Image 519b252 (2,422,592 bytes, valid after 60 s). Same method, lease held,
with `GET /api/display?split=1` naming the parts of the hud layer.

| Screen | total | hud before | hud after | the hud's largest parts |
|---|---|---|---|---|
| Home | 26.9 | 4.6 | 5.2 | caption 2.2 |
| Control shade | 75.2 | 56.0 | 53.8 | space_shade 37.6, space_veil 7.7 |
| WATCH DRESS | 43.2 | 12.1 | 13.0 | clock_face 7.2 |
| WATCH CELESTIAL | 50.1 | 18.5 | 19.9 | clock_face 13.3 |
| WATCH DIVER | 51.8 | 19.7 | 21.6 | clock_face 14.4 |
| WATCH FUTURE | 59.9 | 25.5 | 28.7 | clock_face 12.8, clock_cells 9.5 |
| WATCH PILOT | 63.6 | 28.1 | 32.7 | clock_face 21.4 |
| WATCH SKELETON | 66.8 | 34.1 | 36.8 | clock_face 30.2 |
| WATCH MINIMAL | 58.3 | 66.7 | **36.3** | clock_face 25.6 |
| WATCH JARVIS | 30.3 | 32.4 | **9.1** | clock_face 2.6 |
| WEATHER | 58.3 | 31.9 | 35.5 | space_screen 16.6, space_orbit 12.6 |
| STATUS | 84.6 | 58.0 | 62.3 | space_screen 42.6, space_orbit 12.1 |
| DESK | 67.3 | 42.6 | 45.4 | space_screen 26.6, space_orbit 11.7 |
| ACTIVITY | 50.8 | 28.5 | 28.2 | space_screen 7.7, space_orbit 13.7 |

Two cuts carried to the device (MINIMAL -30 ms, the JARVIS watch style
-23 ms). The rest did not, and most screens read 3 to 4 ms slower than on the
first image; the split timers are the only thing that image added on those
paths, which makes them the suspect, not the proven cause. The shell layer is
still 20 to 21 ms on every screen (the lease rim), the dial blit 9 ms on the
six baked dials, and the orbit rail 12 to 14 ms on every ring screen.

Internal RAM after this flash: 33 KB free, largest block 22,528 bytes, against
26,624 on the first image at the same uptime. Not yet explained.

The PMIC fix did what it could: VSYS and the die temperature still do not
convert within 500 ms of enabling their channels on battery, and are now
reported as absent (`die -`, `vsys -`) instead of as 0 mV and 385.7 C.

A later fresh wake of the same image read a largest block of 20,480 bytes, so
the figure moves by a few KB with heap state and the "drop" above is not
evidence against the code.

## Third flash the same day: two more rounds of cuts, measured

Image 7350dc1 (2,431,536 bytes, valid after 68 s). Same method, lease held.
"Before" is the second flash (519b252).

| Screen | ms before | ms after | fps before | fps after |
|---|---|---|---|---|
| Home | 26.9 | **7.5** | 8.7 | 8.7 (the face clip's own rate) |
| WATCH DRESS | 43.2 | **24.8-26.8** | 19.0 | 23.3-25.0 |
| WATCH CELESTIAL | 50.1 | 32.5 | 16.7 | 22.0 |
| WATCH DIVER | 51.8 | 37.2 | 16.0 | 20.3 |
| WATCH FUTURE | 59.9 | 40.3 | 14.0 | 19.3 |
| WATCH PILOT | 63.6 | 46.3 | 13.3 | 17.3 |
| WATCH SKELETON | 66.8 | 48.1 | 13.0 | 16.7 |
| WATCH JARVIS | 30.3 | 11.1 | 8.7 | 8.7 |
| WATCH MINIMAL | 58.3 | 49.6 | 8.7 | 8.3 |
| WEATHER | 58.3 | 36.3 | 14.0 | 20.3 |
| STATUS | 84.6 | **39.8** | 10.3 | 18.7 |
| DESK | 67.3 | 33.5 | 12.7 | 21.7 |
| ACTIVITY | 50.8 | **23.7** | 16.0 | 21.3 |
| Control shade over ACTIVITY | - | 39.0 | - | 19.3 |
| Control shade over home | 75.2 | 75.1 | 9.0 | 8.7 |

What carried, by part:

- The agent rim (shell layer): 21 ms to 2.0-3.4 ms on every screen.
- The orbit rail: 12-14 ms to 5.9-7.1 ms.
- `space_screen`: STATUS 42.6 to 21.0, DESK 26.6 to 15.1. WEATHER did not
  move (16.6 to 17.4), ACTIVITY 7.7 to 6.7.
- `space_shade`: 37.6 to 25.3.

Two cuts made the device slower although the host counted fewer or equal
instructions, both for the same reason (they write more memory):

- **The veil by the word**: `space_veil` 7.7 to 33.0 ms over the home face. It
  dims every pixel of the strip; the loop it replaced skipped black ones.
  Reverted (2be6e21).
- **The MINIMAL disc record**: `clock_face` 25.6 to 35.1 ms. Switched off on
  the device pending a measurement that separates it from the word fill that
  landed in the same commit.

Fourth flash (175d912, 2,431,280 bytes), veil reverted and the disc record
off: the shade over home is 52.9 ms (`space_veil` 12.3, `space_shade` 26.6),
down from 75.1. MINIMAL did **not** recover: `clock_face` 39.5 ms with the
record off, 35.1 with it on, 25.6 before the commit that added it. Run-to-run
spread on the watch was about 3 ms across these walks, so the record itself is
not shown to matter either way; what that commit changed in the dial's
ordinary drawing path is what costs the 10 to 14 ms, and it is still in. The
other screens repeated within that spread (STATUS 40.7, DESK 33.5, WEATHER
35.2, ACTIVITY 24.7, DRESS 26.9-28.9).

What is left on the watch: the dial copy from PSRAM is 8.4-8.9 ms on every
baked dial, and the hands are 8.5 (DRESS) to 29.6 ms (SKELETON, of which the
balance wheel is most).

## Fifth flash the same day: the transition grammar and the knob round

Image 5af9fc4 (2,440,560 bytes, valid after 60 s, largest internal block
20,480 bytes, `flush_errors 0`). It carries the transitions that pass through
the dark, the drawn dials at the full frame cadence, and seven cost candidates
behind `POST /api/debug/display?knob=N`, each pixel-identical to knob 0 on the
host. Lease held, 3 s per sample.

**The drawn dials' cadence carried.** The JARVIS watch style went from 8.3 to
23 fps (10.8 ms a frame). MINIMAL went from 8.7 to 12.3 fps, held there by its
cost, not by a cap.

**The knob round.** "face" is the `clock_face` part, "dial" the dial layer.
The first and last sample of each dial were both knob 0; they differed by 1.5
to 2.5 ms, which is the spread to read the rest against.

| Where | knob 0 | candidate | verdict |
|---|---|---|---|
| MINIMAL face | 38.5 / 38.4 | old store loop in `hud_render.c`: 26.5 | kept, -12 ms |
| MINIMAL face | 38.5 | `aa_disc` as before 91b44ec: 25.8 | kept, -12.7 ms |
| MINIMAL face | 38.5 | both: 25.0; both and the disc record: 25.1 | the record is deleted |
| MINIMAL face | 38.5 | the disc record alone: 33.5 | helps only the slow path |
| Shade over home, total | 45.3 / 45.5 | old store loop in `glass_draw.c`: 43.7 | kept (simpler, not slower) |
| STATUS, total | 36.5 / 37.6 | the same: 36.4 | neutral |
| SKELETON face | 31.3 / 33.5 | balance-wheel class map: 32.8 | deleted |
| Dial layer, six baked dials | 8.3-9.1 | copy only inside the glass: 7.5-7.8 | kept, about -1.1 ms |
| PILOT face | 20.4 / 22.0 | hands written once per row: 21.1 | deleted |
| FUTURE face | 13.4 / 15.9 | the same: 16.6 | deleted |
| DIVER face | 13.8 / 15.9 | the same: 17.0 | deleted |

So the MINIMAL regression was the word fill (`fill16`, two pixels to one
32-bit store): fewer instructions on the host, 12 ms slower on the ESP32-S3.
With the old paths back MINIMAL is about 40 ms a frame (54.6 with them off).

**Transitions on the device cost two to three times a settled frame.** Polling
the render counters for 1.5 s after each trigger (so "worst" is the worst
average over one poll interval of 1 to 5 frames, and the polling itself loads
the device):

| Transition | frames in the window | mean ms | worst ms | where the worst frame went |
|---|---|---|---|---|
| Shade open over home | 13 | 57.8 | 96.9-108 | `space_shade` 37, `space_veil` 25, orbit 17 |
| Shade close over home | 11 | 33.8 | 54-71 | `space_shade` 32 |
| Home > WATCH | 24 | 28.5 | 30-57 | dial 15, hud 37 |
| Style DRESS > PILOT | 17 | 53.2 | 76-83 | `clock_face` 23, dial 19 |
| Style PILOT > MINIMAL | 17 | 65.8 | 84-127 | `clock_face` 53 |
| Style MINIMAL > FUTURE | 14 | 59.6 | 92-113 | `clock_face` 47 |
| Shade open over WATCH | 17 | 77.9 | 157-180 | `space_shade` 72, `clock_face` 32, `clock_cells` 27 |
| Shade close over WATCH | 25 | 44.5 | 115-150 | `space_shade` 31, dial 22 |
| WATCH > WEATHER | 25 | 45.1 | 60-89 | `clock_cells` 17, `clock_face` 17 |
| WEATHER > STATUS | 21 | 59.2 | 90-108 | `space_screen` 63 |
| STATUS > DESK | 21 | 56.2 | 81-97 | `space_screen` 43 |
| DESK > ACTIVITY | 31 | 31.5 | 47-83 | `space_screen` 20, orbit 11 |
| ACTIVITY > home | 16 | 22.2 | 49-70 | `space_veil` 11 |

A screen that costs 20 ms settled (`space_screen` on STATUS) costs 63 ms while
it fades, and PILOT's hands go from 20 to 53: a fading screen blends every
primitive it draws. The grammar is right in the preview and reaches the panel
as three to five steps, not nine. The fix is to draw the screen at full
strength and dim the finished strip once.

## Sixth flash the same day: the subtraction confirmed, the one-pass fade lost

Image cecd2f7 (2,435,008 bytes, valid after 60 s, largest internal block
22,528 bytes, `flush_errors 0`). The knob round's winners are the only paths
and its losers are deleted; one new candidate rides behind the knob.

Settled, knob 0, lease held, 3 s per screen:

| Screen | ms | fps | note |
|---|---|---|---|
| Home | 6.8 | 8.7 | the face clip's own rate |
| Shade over home | 48.1 | 8.7 | `space_shade` 25.9, `space_veil` 7.5 |
| WATCH JARVIS style | 10.7 | 32 | was 8.3 fps |
| WATCH DRESS | 26.1 | 25 | dial 7.8 |
| WATCH CELESTIAL | 31.4 | 23 | |
| WATCH DIVER | 37.8 | 20 | |
| WATCH MINIMAL | 40.2 | 15 | `clock_face` 26.4; was 53-55 ms at 8.7 fps |
| WATCH PILOT | 41.7 | 19 | |
| WATCH FUTURE | 44.8-46.2 | 17 | |
| WATCH SKELETON | 47.4 | 17 | `clock_face` 30.9 |
| WEATHER | 35.3 | 20 | |
| STATUS | 38.2 | 26 | |
| DESK | 31.0 | 23 | |
| ACTIVITY | 23.5 | 23 | |

The dial layer is 7.4-8.0 ms on the six baked dials (8.3-9.1 before the copy
was limited to the glass).

**Fading a ring screen by one dim pass is slower, not faster.** Knob bit 0
draws the screen at full strength and dims the finished rows once. Mean ms per
frame over the 0.9 s after each trigger, two runs each, alternating:

| Transition | knob 0 | bit 0 | `space_screen` knob 0 | bit 0 |
|---|---|---|---|---|
| WEATHER > STATUS | 61.6 / 64.8 | 71.8 / 72.3 | 35.5 / 36.2 | 41.5 / 40.6 |
| STATUS > DESK | 56.8 / 60.7 | 65.3 / 44.9 | 34.0 / 34.8 | 43.0 / 15.6 |
| DESK > ACTIVITY | 35.7 / 40.3 | 41.5 / 40.8 | 14.0 / 19.0 | 17.8 / 18.6 |
| WATCH > WEATHER | 49.3 / 54.5 | 57.7 / 59.9 | | |
| Home > WATCH | 56.6 / 56.4 | 59.3 / 59.6 | (bit 0 does not touch it) | |

The host had counted it 10-20 % more instructions and was right this time. So
blending at partial strength is not what makes a transition frame expensive,
and the fifth flash's reading of its own table was wrong. What the numbers fit
instead: STATUS's `space_screen` was 42.6 ms before its ring was recorded and
is 21 settled now; mid-entry it is 35-63, the unrecorded cost, because a
record is replayed only when its content is settled and an entering ring
sweeps. The shade is the same (26 settled, 37-72 opening). Not yet proven by a
profile.

`n15c-fixes.png`, read back from the fourth image under a lease: the lease caption
no longer overprints the shade's hint line, and the shade shows 95 %, the
owner's setting. The ACTIVITY rows still read `FORGIVE ME, SIR. I.` and
`I'M NOT QUITE.`: they were stored already cut four hours earlier, and the
new rule applies when a row is recorded, so that fix is unproven on the
device until something new is said.

## The boot that froze the touch, and its fix

The owner lifted the device from deep sleep and found it frozen. It was
booting, slowly. The glance after a lift puts a watch dial on the glass from
the first second. `gfx_render` runs at priority 4 on core 0 and `app_main` at
priority 1 on the same core, and since N14.4 (341735d) a frame that overruns
its period yields instead of sleeping a tick. A dial frame overruns every
period, so the render task never slept and boot got what was left.

| Measured on the device | Value |
|---|---|
| `gfx_render` share of core 0, WATCH dial settled | 96.5 % (27.8 % at home) |
| `IDLE0` on the dial | about 0 (66 % at home) |
| `jr_imu` on the dial | 0.7 % (3.4 % at home) |
| WakeNet start-up in that boot | 48.9 s (50 ms on a boot that lands on home) |
| "boot complete" in that boot | 69.25 s |
| Touches during it | queued, then `input queue overflow; dropped=16`, the rest replayed at the end |

Fix, 71d66ad: the render cadence is 6 fps (`RENDER_FPS_BOOT`) from before the
display starts until the voice loop's first cadence tick, which sets the mood's
rate as it does on every pass. Proof on the device, a forced deep sleep
(`POST /api/debug/sleep?now=1&wake_s=N`) and the owner's lift, the dial on the
glass: boot complete at 6.06 s, a tap handled at 6.4 s, no overflow line. The
owner confirmed the touch by hand.

Not fixed by this, and open as N15.11: in normal use a dial still takes all of
core 0, so everything else on that core at a lower priority (the IMU task
among them) runs in the gaps. Whether the freeze predates N14.4 was not tested
on an older image.

## Seventh flash the same day: three replay switches, all small wins

Image b0e490d (2,437,184 bytes, boot complete at 5.5 s, valid). The profile
behind it corrected the sixth flash's hypothesis: a record bypassed in motion
explains STATUS and DESK only; the watch and the shade pay for per-pixel fades.
Three candidates rode behind the knob. Mean ms per frame over the 0.9 s after
each trigger, knob 0 against the bit, alternating rounds:

| Bit | Transition | knob 0 | bit | part, knob 0 > bit | worst interval, knob 0 > bit |
|---|---|---|---|---|---|
| 0 ring replay | WEATHER > STATUS | 59.4 / 57.7 | 52.1 / 60.7 | `space_screen` 36.3 / 33.2 > 29.6 / 34.4 | 99 / 100 > 67 / 74 |
| 0 | STATUS > DESK | 59.0 / 69.4 | 49.8 / 53.2 | 34.8 / 44.5 > 27.2 / 29.0 | 91 / 93 > 57 / 72 |
| 0 | DESK > ACTIVITY | 34.4 / 37.1 | 37.6 / 36.7 | 14.5 / 15.4 > 13.5 / 13.8 | 42 / 41 > 45 / 44 |
| 3 veil memo | shade open over home | 44.8 / 50.3 | 41.9 / 49.5 | `space_veil` 8.1 / 9.3 > 5.8 / 6.7 | 67 / 83 > 56 / 71 |
| 3 | shade open over MINIMAL | 112.6 / 111.1 | 94.8 / 100.2 | 12.8 / 11.7 > 6.2 / 6.3 | 173 / 176 > 160 / 188 |
| 3 | shade close over WATCH | 65.8 / 67.2 | 72.7 / 63.5 | 4.1 / 4.2 > 2.2 / 2.4 | 130 / 141 > 112 / 134 |
| 2 disc memo | style PILOT > MINIMAL | 68.1 | 55.1 | `clock_face` 37.2 > 30.6 | 162 > 70 |
| 2 | style MINIMAL > FUTURE | 61.3 | 47.6 | 27.9 > 27.2 | 64 > 50 |
| 2 | the six other style changes | 28-69 | 28-64 | inside the run-to-run spread | |

Bits 2 and 3 together, one round, the shade over MINIMAL: open 91.6 > 89.7
(veil 10.0 > 5.1), close 71.5 > 65.6 (worst 125 > 84).

All three win where they apply and none loses elsewhere, so all three are
kept. They are worth 10 to 20 % of a transition frame, and the ring replay
takes about 30 % off the worst interval of a ring entry. The ring replay costs
49,152 bytes of PSRAM for the second sweep's records. The stutter is not
solved: the shade opening over a dial is still 90-100 ms a frame (the dial
alone, settled, is 40), and a style change onto a baked dial is still 50-65.

After the walks: knob 0, largest internal block 22,528 bytes, `flush_errors 0`.

## Eighth flash the same day: the three winners as the only paths

Image ced022c (2,436,912 bytes, boot complete at 5.5 s, valid after 56 s). The
knob has no bits again; the paths the three replaced are deleted (d6bc14e) and
no frame hash changed, settled or in motion. The veil's fold had no test once
its knob-0 reference was gone, so one was added that compares it with a
per-pixel fold built inside the test. Knob 0, lease held, two passes:

| Transition | mean ms | part | worst interval | b0e490d knob 0, worst |
|---|---|---|---|---|
| WATCH > WEATHER | 48.2 / 43.1 | `space_screen` 11.9 / 9.5 | 62 / 56 | |
| WEATHER > STATUS | 48.1 / 46.7 | 23.8 / 23.5 | 61 / 68 | 99 / 100 |
| STATUS > DESK | 47.1 / 47.9 | 24.4 / 25.3 | 55 / 59 | 91 / 93 |
| DESK > ACTIVITY | 34.0 / 32.2 | 11.5 / 12.1 | 39 / 39 | 42 / 41 |
| Shade open over home | 53.2 / 54.2 | `space_veil` 5.7 / 6.8 | 66 / 76 | 67 / 83 |
| Shade open over PILOT | 92.5 / 92.2 | 5.8 / 5.7 | 97 / 121 | (MINIMAL: 173 / 176) |
| Shade close over WATCH | 62.6 / 65.8 | 2.8 / 2.8 | 111 / 145 | 130 / 141 |
| Style PILOT > MINIMAL | 51.3 / 50.5 | `clock_face` 27.0 / 29.7 | 57 / 64 | 162 |
| Style MINIMAL > FUTURE | 50.7 / 39.5 | 29.5 / 17.6 | 53 / 44 | 64 |
| Style FUTURE > SKELETON | 60.1 / 72.1 | 26.7 / 29.6 | 75 / 72 | |

The ring entries came out better than the bit measured on b0e490d (mean 47-48
against 50-60). The likely reason, not proven: switching the bit on there
made the first entry also pay for allocating and filling its records, and here
they exist before the walk starts. After the walks: largest
internal block 22,528 bytes, `flush_errors 0`.

## Wave N16, ninth and tenth flash: the tilt stars and the panel dip

**Tilt (9403dc3, 2,438,704 bytes, valid after 60 s).** CELESTIAL with its 14
foreground stars: 32.4-33.4 ms a frame at 20-21 fps, hud layer 21-22 ms
(31.4 ms before the stars); DRESS 25.0-25.2 ms as before. `"tilt"` reads
`[0,0]` with the device still on its charger and `[0,0]` off CELESTIAL. Not
yet shown by a hand: that the stars follow a tilt, and which way.

An update was refused twice first (`503`): the battery was at 22 % with no
USB, and the preflight wants USB or 30 %. The owner plugged it in.

**The dip (74b3ecf, 2,440,960 bytes, boot complete at 5.5 s, valid after
60 s).** Knob 0 against knob 1 (`JR_KNOB_PANEL_DIP`), lease held, mean ms per
frame over the 0.9 s after each trigger, two alternating rounds:

| Transition | knob 0 mean | knob 1 mean | brightness writes | dark hold, ms |
|---|---|---|---|---|
| WATCH > WEATHER | 44.0 / 42.3 | 41.7 / 44.6 | 15 / 18 | 138 / 130 |
| WEATHER > STATUS | 49.5 / 47.1 | 50.4 / 49.5 | 18 / 18 | 113 / 90 |
| STATUS > DESK | 47.4 / 47.5 | 51.2 / 50.7 | 18 / 18 | 134 / 116 |
| DESK > ACTIVITY | 36.5 / 33.8 | 35.3 / 37.0 | 17 / 19 | 83 / 101 |
| ACTIVITY > home | 25.5 / 25.1 | 26.4 / 23.9 | 15 / 15 | 110 / 154 |

The dip does not make a transition frame cheaper on the device: what is left
in a transition frame is the new screen's own entry motion, as the host count
had already said (2-14 % of the script). What it changes is the number of
steps: 15 to 19 brightness writes across a change of screen, against the 7 or
8 frames the pixel fade gets in the same time. The dark lasts 83-154 ms.
`flush_errors 0` with up to three writes inside a frame. Whether it looks
better is the owner's eye; the capture sees pixels only.

**A style change stalls in the dark, on either path.** The dip's hold clock
measured it (one round, name = the style being left):

| Leaving | onto | dark, ms |
|---|---|---|
| DRESS | PILOT | 506 |
| PILOT | MINIMAL | 185 |
| MINIMAL | FUTURE | 664 |
| FUTURE | SKELETON | 269 |
| SKELETON | CELESTIAL | 264 |
| CELESTIAL | JARVIS | 124 |
| JARVIS | DIVER | 565 |
| DIVER | DRESS | 600 |

The hold is capped at 300 ms, yet reads 500-660: the render task was not
running. `dial_decode` (`jr_display.c`) decodes the new dial into the one
shared frame while holding the render lock, after `clip_load` has read the
clip from flash. The four dials in the asset partition take about 0.5-0.66 s,
the two embedded in the app about 0.27 s, the drawn ones 0.12-0.19 s. Knob 0
draws as few frames in the same 0.9 s window (7-14 on the baked dials, on
either knob), which fits the same stall; it has no clock of its own and the
gap had been read as "a costly fade". 5.1 MB of PSRAM is free and a decoded dial is
434 KB. N16.5.

## The owner's eye: the layers are off (image 74b3ecf)

The owner compared the panel dip with the pixel fade and saw no difference.
What he saw instead was layers that do not sit right. Every capture in this
file up to here was taken under an operator lease, which forces the awake mood
and a violet rim; he sees the device with no lease, at rest, on its charger.
Fifteen captures taken that way (knob 0, no lease) are summarised in
`audit-before/`: `sheet0.png` home, the shade and the ring screens;
`sheet1.png` WATCH on arrival; `sheet2.png` and `sheet3.png` the eight styles;
`zoom0.png` the rim over a dial. They are the capture mirror: pixels, not
brightness.

| # | Where | What is off |
|---|---|---|
| 1 | DIVER, DRESS, PILOT, SKELETON | The shell's rim, a one-pixel aliased circle near r 221, runs across the dial art |
| 2 | every dial, and home | The charging mark, square cyan dots from 12 to 3 o'clock, sits on the dial art and is blocky everywhere |
| 3 | PILOT, FUTURE | The sun arc sweeps over the tick track and the rings |
| 4 | JARVIS style | Three hands and a hub on plain black: no face |
| 5 | WATCH on arrival, at rest | A small bitmap-font clock (`BAT 25%`, `CHARGING`) instead of the dial; the dial appears only after a style change |
| 6 | the shade over home | The face's bright core shows through the mute button |
| 7 | FUTURE | The weather word is cut: `82° MAINLY.` |
| 8 | DIVER | The date is in the bitmap font inside photographic art |

The panel dip is not kept (N16.1). The fixes are N16.6; the same captures will
be taken again, without a lease, after its flash.

## Eleventh flash: a decoded frame per dial (image d5ffa17, 2,443,472 bytes)

Same walk as the table above (perf knob bit 0 on, so the dip's hold clock
times the dark; two rounds, the second with every dial cached; under a lease).

| Onto | Dark before, ms | Dark now, ms | Frames in 0.9 s, before | now |
|---|---|---|---|---|
| DIVER | 565 | 233 | 7-14 | 17 |
| DRESS | 600 | 130 | 7-14 | 19 |
| PILOT | 506 | 190 | 7-14 | 17 |
| MINIMAL (drawn) | 185 | 187 | | 13 |
| FUTURE | 664 | 241 | 7-14 | 16 |
| SKELETON | 269 | 189 | | 13 |
| CELESTIAL | 264 | 186 | | 13 |
| JARVIS (drawn) | 124 | 127 | | 19 |

`dial_cache`: 6 dials, 2,605,872 bytes, 22 hits, 3 misses, 3 warm-ups, no
evictions, last decode 69 ms. Free PSRAM 2.89 MB (4.22 MB before the first
dial on this boot), largest internal block 19,456 bytes at boot and after the
walk, `flush_errors 0`, worst frame 80 ms.

The decode no longer shows. The 150 ms gate is not met: MINIMAL never decoded
anything and sits at 187 ms, so the remainder is the new dial's first frames
at 50-70 ms each. Not looked at: the second hand during a warm-up, and a
change made within 1.5 s of the last.

## Twelfth flash: a dial owns the glass (image f7e151b, 2,456,400 bytes)

The same walk as `audit-before/`, with no lease, knob 0, the microphone muted,
on the charger, plus rest captures after 12 s of stillness. Sheets in
`audit-after/`: `sheetA.png` home, the shade, WATCH on arrival, the JARVIS
style; `sheetB.png` DIVER, DRESS, PILOT, FUTURE; `sheetC.png` CELESTIAL awake
and at rest, home awake and at rest; `sheetD.png` MINIMAL, SKELETON, CELESTIAL,
the shade over a dial. The capture mirror: pixels, not brightness.

| # | Defect in `audit-before/` | On the device now |
|---|---|---|
| 1 | The rim across the dial art | No rim on any dial while muted |
| 2 | The blocky charging mark on dials and home | Gone from dials; home has one anti-aliased ring, the battery its lit part |
| 3 | The sun arc over PILOT and FUTURE | Gone from baked dials |
| 4 | The JARVIS style without a face | Tick ring, beads, hairline rings, tapered hands, the date |
| 5 | A bitmap clock instead of the dial at rest | WATCH arrives on the dial and keeps it at rest by day; home at rest shows `18:16`, `100% USB` in the glass font |
| 6 | The face through the shade's button | Opaque |
| 7 | FUTURE's cut weather word | Not seen: no weather held (`NO WEATHER`); the host render reads `83° CLEAR` |
| 8 | DIVER's date in the bitmap font | Glass digits |

A resting DIVER drew 1 frame in 30 s (CELESTIAL about 6 a second: it keeps
animating at rest). Boot complete came at 12.8 s on this flash against 5.5 s:
the log shows a Wi-Fi disconnect (reason 205) and a retry, not the firmware.
After the capture run the dial cache had held four dials and evicted three:
the capture mirror takes PSRAM that an unobserved device keeps for the cache
(4.66 MB free with no dial held). Largest internal block 21,504 bytes,
`flush_errors 0`.

Not exercised on the device: the privacy dot and the session ring (the
microphone stayed muted), the night face, frame times under this image.
