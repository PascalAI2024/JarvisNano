# Tilt parallax

**What it is** — The first use of the accelerometer as a display input: `jr_imu` publishes a low-passed gravity direction in one lock-free word, `jr_display` shapes it into a lean (re-centred, dead-zoned, clamped) and a drawn foreground of 14 near stars slides over CELESTIAL's baked dial by it, in three depths. QMI8658 accelerometer only; the gyro stays off.

**How we use it here** — The signal is built where the samples are taken, shaped where the frame is drawn, and wired in `main/`, so `jr_imu` knows no display and the display knows no sensor. The shaper (`tilt.h`) is pure and header-only so the same integer maths is tested on the host and runs on the device. A glint on the hands or a flick gesture can read the same word and the same shaper.

---

## Findings & gotchas

**[2026-11-10] The signal: sensor-frame milli-g, one pole, one word**
`imu_task` already reads `ax/ay/az` every 10 ms (`components/jr_imu/src/jr_imu.c:396`, the motion window is untouched). After each sample it low-passes the in-plane X and Y with gain 1/8 (about an 80 ms time constant; `TILT_LP_DIV`, `jr_imu.c:116`), converts LSB to milli-g (`ax * 125 / 512`, 4096 LSB/g) and stores both as signed 16-bit halves of one `uint32_t` (x in bits 0-15, y in bits 16-31) with a release store (`jr_imu.c:428`). `jr_imu_tilt_read()` (`jr_imu.c:521`) is an acquire load and a validity flag: no mutex, no bus, callable from any task. Values are the UP vector's components, +1000 meaning the axis points straight up, in SENSOR axes (Z is already flipped at the read seam for this board, `jr_imu.c:313`, but X and Y are as the chip reports them). Unit: milli-g.

**[2026-11-10] The shape: neutral is wherever it is held**
`components/jr_display/include/jr_display/tilt.h`. Once a frame the render task gives `jr_tilt_step` the reading and the frame's dt. The neutral pose (`cx_q8`, `cy_q8`, milli-g x 256) eases toward the reading with tau 3000 ms (`dt / (tau + dt)`, the `hud_light_ease` form, never overshoots); the offset is `reading - neutral`; inside +-12 mg the output is exactly 0 (about 0.7 degree, so a still device never shimmers); beyond it the dead zone is subtracted and the result is scaled so 350 mg past the dead zone reads +-256 and clamps there. Output unit: -256..+256, where 256 is the clamp, whatever the sensor unit was. A step of 150 mg reads about 100 on its first frame and is back to exactly 0 in about 8 s (150 * e^(-t/3) falls under the 12 mg dead zone at 7.6 s). A gap over 1000 ms between steps, or a missing source, reseeds the neutral at the current pose, so coming back to the dial starts at rest. Integer maths only: the truncating ease stalls within 0.3 mg of the pose, 40 times inside the dead zone, so ten minutes of a held pose never drifts (`test_tilt_no_drift_in_ten_minutes`).

**[2026-11-10] The wiring: a source function, asked once a frame, only on CELESTIAL**
`main/main.c:59` `tilt_source()` is the only place the two meet; it reads `jr_imu_tilt_read` and applies `JR_TILT_IMU_X_SIGN/Y_SIGN` (`main.c:53`), registered with `jr_display_set_tilt_source` (`main.c:4517`). The display calls it from `tilt_tick` (`components/jr_display/src/jr_display.c:4163`, called from `overlay_fade_tick` beside `light_tick`, `jr_display.c:1428`) only while `s_style_shown` is CELESTIAL, the panel is lit (`s_panel_off & 3 == 0`) and the render fps is the awake cadence. Any other screen resets the shaper and never calls the source, so the sensor is not a display input elsewhere and `jr_mood` and the stillness rules are untouched: nothing here feeds back into wake or sleep. The shaped lean reaches the dial as `hud_watch_t.tilt_x/tilt_y` (`jr_display.h`/`hud_render.h`), and one packed word (`s_tilt_word`, `jr_display.c:4156`, same x/y layout) feeds `GET /api/display` as `"tilt":[x,y]` (`main/http_routes.c`, after the `light` object; the buffer grew from 1280 to 1408 bytes because the response was already close to its old size).

**[2026-11-10] What is drawn: a foreground of 14 stars that are not in the dial art**
The first cut slid CELESTIAL's twinkle glints by depth and failed on the glass: the dial art carries hundreds of baked stars, so a two-pixel glint moving 1 to 6 px among them read as nothing, and a glint detached from its baked star is wrong in itself. The glints are back to being pinned to their baked stars (`hud_celestial_twinkle` is the original function). The parallax is a separate drawn layer, `hud_celestial_foreground` (`components/jr_display/src/hud_render.c:2487-2640`): 14 four-point sparkles at positions that are in no baked star, chosen from the dial art (`s_fg`, `hud_render.c:2514`): off the hub, the hour beads and the bars at 12, and clear of the constellations. The baked field is the far sky and these are the near ones; the depth is the contrast between a still background and moving foreground.

| depth | stars | glow radius | arm length | travel at full lean | colour |
|---|---|---|---|---|---|
| near | 4 | 3 px | 10 px | 14 px | warm white |
| middle | 5 | 2.25 px | 6.5 px | 8 px | white, slightly cool |
| far | 5 | 1.75 px | 4 px | 4 px | pale blue |

(`s_fg_depth`, `hud_render.c:2506`.) Each star is a tight bright core (a squared falloff of a soft disc) plus a horizontal and a vertical arm one pixel thick that fall straight to the tip, drawn additively. The centre is kept in 1/16 px (`hud_celestial_fg_pos` returns it: lean times travel / 16) and the arms are a one-pixel tent in the cross direction, so a slide across a pixel boundary keeps the star's energy and glides; a test pins the centroid at half-pixel and one-pixel leans and the energy within 5 %. The layer is drawn at rest too, with lean 0, so nothing pops in when the hand moves, and the near stars breathe slowly out of phase while the dial is alive (brightness 168..255, periods 4 to 5.5 s). A slide that would put a star's arms outside radius 210 or inside radius 32 of the centre is held at rest (`HUD_FG_GLASS_R`, `HUD_FG_HUB_R`); on today's table the guard never fires at any lean (a test walks every star over a 17 x 17 lean grid and asserts the ring). A resting watch, any other dial and a lean of 0 draw the layer without moving it. Depth comes from the table, not from an index, so the baked glint table and its tests are unchanged.

Because CELESTIAL's settled frame changed, the five `watch-celestial` rows of `tests/frame_hashes.inc` were regenerated; no other scene's hash moved.

**[2026-11-10] Cost**
Host callgrind over the preview (celestial, 365 frames at 24 fps, stand-in dials; compare builds against each other, `glass-preview.md`): mean instructions per frame 2,080,949 at 3725992, 2,240,345 with the layer at rest (+159 k), 2,253,215 under the scripted lean (+172 k). About 0.15 per cent of a frame on average; the device budget is +2 ms, about 500 k instructions. The worst frame (the first, 11.95 M) is unchanged. Not measured on the device.

**[2026-11-10] Preview**
`scripts/glass-preview.py --script celestial-tilt` plays `probe/preview/celestial-tilt.script`: a slow circle of tilt (`tilt orbit 4000 300`, one turn 300 mg across, from the origin and back), then a hold at full lean, `tilt 380 0` and `tilt -380 160` (380 mg is 12 past the dead zone plus the 350 clamp, so it reads +-256); the dial re-centres in about 3 s after each hold. The preview registers the same kind of source (`glass_preview.c`, `pv_tilt_source`) and runs the real `tilt.h`. `tilt X Y` is the in-plane UP vector in milli-g, screen axes (x right, y down).

---

## Primary sources

| Source | Notes |
|--------|-------|
| `components/jr_imu/src/jr_imu.c:116-117, 396, 428, 521` | The low-pass, the word and the lock-free read. |
| `components/jr_display/include/jr_display/tilt.h` | The pure shaper, its constants and the diag word. |
| `components/jr_display/src/jr_display.c:1428, 4156-4178` | Where the display asks, and the gates. |
| `components/jr_display/src/hud_render.c:2487-2640` | The foreground layer: table, depths, position, draw. |
| `main/main.c:53-66, 4517` | The only wiring between sensor and display, and the axis signs. |
| `components/jr_display/tests/test_hud_render.c` (`test_tilt_*`, `test_fg_depths_travel_and_guards`, `test_fg_is_antialiased_at_sixteenth_pixels`, `test_fg_changes_only_its_pixels_and_gates`) | Shaper and frame tests. |
| `components/jr_display/tests/test_shell.c` (`test_tilt_word_layout_is_pinned`, `test_tilt_is_read_only_on_a_lit_awake_celestial`) | The word's layout and the gates. |

---

## Open questions

- Axis signs, never judged on the glass. `JR_TILT_IMU_X_SIGN (+1)` and `JR_TILT_IMU_Y_SIGN (-1)` copy the light's mapping (IMU +y toward 12 o'clock, IMU +x assumed toward 3 o'clock, `main.c:39-46`), which is itself unverified for X. The stars slide by +lean, that is toward where the change of UP points on the glass; whether that feels like a window onto a sky or like the sky stuck to the glass is a taste for the device. Flip both signs to reverse it.
- Is 350 mg past the dead zone (about 20 degrees) the right full lean for a wrist turn, and is 3 s the right re-centre? Both are one constant in `tilt.h`.
- Is the 80 ms low-pass enough to hide hand tremor at 24 fps, or does a far star shimmer at its 4 px travel?
- Sizes, travels and colours are tuned in the host preview against the stand-in dial art, not on the AMOLED: judge the near stars' 10 px arms and the breathing on the glass.
- Whether 14 foreground stars is the right density, and whether the warm near stars should sit nearer the constellation's brightness.

---

## See also

- [power-modes.md](./power-modes.md) — the wake-on-motion configuration this does not touch.
- [glass-preview.md](./glass-preview.md) — the preview tool and `--script`.
- [frame-cost.md](./frame-cost.md) — how the frame cost is counted.
