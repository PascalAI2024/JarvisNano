# Tilt parallax

**What it is** — The first use of the accelerometer as a display input: `jr_imu` publishes a low-passed gravity direction in one lock-free word, `jr_display` shapes it into a lean (re-centred, dead-zoned, clamped) and CELESTIAL's twinkling stars slide by it in three depths. QMI8658 accelerometer only; the gyro stays off.

**How we use it here** — The signal is built where the samples are taken, shaped where the frame is drawn, and wired in `main/`, so `jr_imu` knows no display and the display knows no sensor. The shaper (`tilt.h`) is pure and header-only so the same integer maths is tested on the host and runs on the device. A glint on the hands or a flick gesture can read the same word and the same shaper.

---

## Findings & gotchas

**[2026-11-10] The signal: sensor-frame milli-g, one pole, one word**
`imu_task` already reads `ax/ay/az` every 10 ms (`components/jr_imu/src/jr_imu.c:396`, the motion window is untouched). After each sample it low-passes the in-plane X and Y with gain 1/8 (about an 80 ms time constant; `TILT_LP_DIV`, `jr_imu.c:116`), converts LSB to milli-g (`ax * 125 / 512`, 4096 LSB/g) and stores both as signed 16-bit halves of one `uint32_t` (x in bits 0-15, y in bits 16-31) with a release store (`jr_imu.c:428`). `jr_imu_tilt_read()` (`jr_imu.c:521`) is an acquire load and a validity flag: no mutex, no bus, callable from any task. Values are the UP vector's components, +1000 meaning the axis points straight up, in SENSOR axes (Z is already flipped at the read seam for this board, `jr_imu.c:313`, but X and Y are as the chip reports them). Unit: milli-g.

**[2026-11-10] The shape: neutral is wherever it is held**
`components/jr_display/include/jr_display/tilt.h`. Once a frame the render task gives `jr_tilt_step` the reading and the frame's dt. The neutral pose (`cx_q8`, `cy_q8`, milli-g x 256) eases toward the reading with tau 3000 ms (`dt / (tau + dt)`, the `hud_light_ease` form, never overshoots); the offset is `reading - neutral`; inside +-12 mg the output is exactly 0 (about 0.7 degree, so a still device never shimmers); beyond it the dead zone is subtracted and the result is scaled so 350 mg past the dead zone reads +-256 and clamps there. Output unit: -256..+256, where 256 is the clamp, whatever the sensor unit was. A step of 150 mg reads about 100 on its first frame and is back to exactly 0 in about 8 s (150 * e^(-t/3) falls under the 12 mg dead zone at 7.6 s). A gap over 1000 ms between steps, or a missing source, reseeds the neutral at the current pose, so coming back to the dial starts at rest. Integer maths only: the truncating ease stalls within 0.3 mg of the pose, 40 times inside the dead zone, so ten minutes of a held pose never drifts (`test_tilt_no_drift_in_ten_minutes`).

**[2026-11-10] The wiring: a source function, asked once a frame, only on CELESTIAL**
`main/main.c:59` `tilt_source()` is the only place the two meet; it reads `jr_imu_tilt_read` and applies `JR_TILT_IMU_X_SIGN/Y_SIGN` (`main.c:53`), registered with `jr_display_set_tilt_source` (`main.c:4517`). The display calls it from `tilt_tick` (`components/jr_display/src/jr_display.c:4163`, called from `overlay_fade_tick` beside `light_tick`, `jr_display.c:1428`) only while `s_style_shown` is CELESTIAL, the panel is lit (`s_panel_off & 3 == 0`) and the render fps is the awake cadence. Any other screen resets the shaper and never calls the source, so the sensor is not a display input elsewhere and `jr_mood` and the stillness rules are untouched: nothing here feeds back into wake or sleep. The shaped lean reaches the dial as `hud_watch_t.tilt_x/tilt_y` (`jr_display.h`/`hud_render.h`), and one packed word (`s_tilt_word`, `jr_display.c:4156`, same x/y layout) feeds `GET /api/display` as `"tilt":[x,y]` (`main/http_routes.c`, after the `light` object; the buffer grew from 1280 to 1408 bytes because the response was already close to its old size).

**[2026-11-10] The stars: three depths from the index, whole pixels**
`hud_celestial_star_depth(i) = i % 3`: 0 near, 1 middle, 2 far (11 / 11 / 10 stars; no new asset, stable across builds because the baked table is). At full lean the glint slides 6 / 3 / 1 px (`HUD_STAR_PARALLAX_PX`, `hud_render.c:2443`), rounded to the nearest whole pixel (`hud_celestial_star_shift`, `hud_render.c:2450`). A shift that would put the glint outside radius 210 or inside radius 24 of the centre is dropped (the glass edge and the hands' hub); on today's 32 stars the largest radius is about 181 and the smallest 35, so the guard never fires, and a test pins the invariant for every star at every lean on a 32-step grid. With lean (0, 0) `hud_celestial_twinkle` is the same call with zeros and the existing frame hashes do not move (`tilt (0,0) is not the plain twinkle`, and the 22 suites). Strips equal the whole frame under lean.

**[2026-11-10] What moves is the glint, not the baked star**
The 32 stars are glints added over stars that are baked into the dial art (`gen_celestial_stars.py` picks them from the baked frame), not sprites. A lean slides the glint; the baked star stays under it. With no clean backdrop to restore, a baked star cannot be lifted off the dial without a second asset (the dial without stars), so this slides the twinkle only: a near glint at full lean sits 6 px from its star, which reads as a sparkle drifting off it, and a far one stays on its star. Moving the stars themselves is a new-asset job (a star-less base plus a star atlas); this page records that rather than faking it. Sub-pixel AA was not done: the glint is a soft five-pixel cross already, a bilinear split would cost four times the blends on every lit star for a motion of 1 to 6 px, so whole pixels stay.

**[2026-11-10] Cost**
Host callgrind over the preview (celestial, 365 frames at 24 fps, stand-in dials; compare builds against each other, `glass-preview.md`): mean instructions per frame 2,080,949 at 3725992, 2,083,489 with tilt wired and no lean, 2,088,640 under the scripted lean. The lean costs about 5 k instructions a frame (0.25 %), the whole feature about 7.7 k, roughly 0.03 ms at 240 MHz against a +2 ms budget; the worst frame (the first, 11.95 M) is unchanged. Not measured on the device.

**[2026-11-10] Preview**
`scripts/glass-preview.py --script celestial-tilt` plays `probe/preview/celestial-tilt.script`: a slow circle of tilt (`tilt orbit 4000 200`, one turn 200 mg across, from the origin and back), then a hold at `tilt 260 0` and `tilt -260 120`; the dial re-centres in about 3 s after each hold. The preview registers the same kind of source (`glass_preview.c`, `pv_tilt_source`) and runs the real `tilt.h`. `tilt X Y` is the in-plane UP vector in milli-g, screen axes (x right, y down).

---

## Primary sources

| Source | Notes |
|--------|-------|
| `components/jr_imu/src/jr_imu.c:116-117, 396, 428, 521` | The low-pass, the word and the lock-free read. |
| `components/jr_display/include/jr_display/tilt.h` | The pure shaper, its constants and the diag word. |
| `components/jr_display/src/jr_display.c:1428, 4156-4178` | Where the display asks, and the gates. |
| `components/jr_display/src/hud_render.c:2443-2520` | Depths, shift, the glint draw. |
| `main/main.c:53-66, 4517` | The only wiring between sensor and display, and the axis signs. |
| `components/jr_display/tests/test_hud_render.c` (`test_tilt_*`, `test_star_depths_and_shift`, `test_celestial_tilt_moves_only_the_glints`) | Shaper and frame tests. |
| `components/jr_display/tests/test_shell.c` (`test_tilt_word_layout_is_pinned`, `test_tilt_is_read_only_on_a_lit_awake_celestial`) | The word's layout and the gates. |

---

## Open questions

- Axis signs, never judged on the glass. `JR_TILT_IMU_X_SIGN (+1)` and `JR_TILT_IMU_Y_SIGN (-1)` copy the light's mapping (IMU +y toward 12 o'clock, IMU +x assumed toward 3 o'clock, `main.c:39-46`), which is itself unverified for X. The stars slide by +lean, that is toward where the change of UP points on the glass; whether that feels like a window onto a sky or like the sky stuck to the glass is a taste for the device. Flip both signs to reverse it.
- Is 350 mg past the dead zone (about 20 degrees) the right full lean for a wrist turn, and is 3 s the right re-centre? Both are one constant in `tilt.h`.
- Is the 80 ms low-pass enough to hide hand tremor at 24 fps, or does a lit glint shimmer at the 1 px far depth?
- Moving the baked stars themselves needs the star-less dial asset above.

---

## See also

- [power-modes.md](./power-modes.md) — the wake-on-motion configuration this does not touch.
- [glass-preview.md](./glass-preview.md) — the preview tool and `--script`.
- [frame-cost.md](./frame-cost.md) — how the frame cost is counted.
