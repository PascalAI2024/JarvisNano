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
