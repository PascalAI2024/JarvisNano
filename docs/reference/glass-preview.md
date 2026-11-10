# Glass preview: the real compositor, animated, on the desk

**What it is** — `components/jr_display/tests/probe/glass_preview.c` is a host probe that plays a script against the production compositor (`jr_display.c`, `#include`d over the same stubs the frame tests use) and writes every composed frame. `scripts/glass-preview.py` builds it in the `jarvisnano-hosttests` Docker image and assembles the frames into an animation.

**How we use it here** — To watch screen entries, the control shade, watch style changes and the face move before a flash, instead of judging motion from single captured frames. Transitions come from the production state machine (`nav_step`, `sp_fade_tick`, `overlay_fade_tick`) reacting to a stub clock advanced one frame interval at a time. No frame is interpolated by the tool.

---

## Run it

```bash
python scripts/glass-preview.py --script ring --out /tmp/ring.gif --contact-sheet 3
python scripts/glass-preview.py --script shade --slow 4 --scale 2
python scripts/glass-preview.py --script watch --check      # runs twice, requires identical frame hashes
```

Options: `--script NAME|PATH`, `--out PATH`, `--scale 1|2`, `--slow N` (play at 1/N speed), `--fps 2..24` (render cadence, see below), `--format apng|gif|webp` (which file is reported as main), `--contact-sheet [K]` (every K-th frame, labelled with time and the script step that fired), `--check`, `--work DIR`. Needs Docker, Python 3 with Pillow and numpy.

Outputs beside `--out`: `.gif` (always; 256 colours, so the glows band, and GIF stores 10 ms units so durations carry the rounding to keep the clip length exact), `.png` (APNG, 24-bit, the default main file because it does not band), `.webp` (lossless, when Pillow has WebP), `_sheet.png`.

Cadence: `JR_DISPLAY_RENDER_FPS` is 24 (`jr_display.c:60`, the measured panel ceiling). `main/app.h:85-88` lowers it by mood: AWAKE 24, AMBIENT 12, WHISPER 6, DREAM 3. The default is 24; `--fps 12` previews the ambient cadence.

## Script format

One step per line; `#` starts a comment. Scripts live in `components/jr_display/tests/probe/preview/NAME.script`.

| Step | Meaning |
|------|---------|
| `scene NAME` | First line. A state from `frame_scenes.inc` (`home`, `watch-diver`, `status-74`, ...; `probe frame list` prints them). Settles 1.5 s unrecorded. |
| `wait MS` | Play MS of clock, frames recorded. |
| `swipe up\|down\|left\|right` | A centre-glass swipe, routed as `main.c` does (see below). |
| `tap X Y` | `jr_display_hit`; the privacy button toggles mute. The action id is printed. |
| `open shade`, `close shade` | The BOOT-button door into the shade, and the way out (`shell_dismiss`). |
| `home` | Double tap: `nav_home`. |
| `set battery N`, `set charging on\|off`, `set mute on\|off`, `set agent N`, `set caption TEXT` | State changes at the current frame. |

Steps other than `wait` take no time. Extending is adding lines; a new verb is one `else if` in `main()` of the probe.

## What is real and what is a stand-in

Real: the compositor layers in `panel_flush` order (dial, HUD, shell, surface), the nav state machine, every ease and clock, captions, the watch hands and their light, the orbit rail.

Stand-ins, printed at the end of every run:

- **Home face.** On the device the face is a decoded EAF in the graphics engine, which the stubs replace with a test pattern. The preview shows the 30 frames of `firmware/mascot/reactive/preview/rwave_idle.png`, a contact sheet (8 columns, pitch 474 px, the 466 px frame 8 px in, measured from the black gutters), looped at 24 fps. It is the idle face, not the decoded asset, and the device binds a different face by voice state (`shown_face`); the preview always shows IDLE.
- **Baked dials.** The six dials are `dial_*_preview.png` composited on black and converted to RGB565, blitted through the real `apply_dial`. They are the source art the dial EAFs are cut from, not a decode of the EAFs (`eaf_frame_decode` is a stub that writes a test pattern, so the host cannot decode them; the stubs only read the SKELETON and CELESTIAL files byte for byte). Hands, caption plate, bloom and date come from the real compositor. WATCH styles JARVIS and MINIMAL have no baked art on the device either.
- **Face and dial binding is instant.** On the device a face change is an engine rebind with its own latency. The preview switches the background in the frame the nav word changes. A pop that appears exactly where a background changes may be this, so check the layer that moved before calling it firmware.
- **Input is replayed, not routed.** `main.c` does not link on the host. The verbs call the same `jr_display_nav_*`, `jr_display_watch_style_step`, `jr_display_bloom` and `jr_display_caption_set` functions in the order of the swipe handler (`main.c:3541-3700`, `shell_vertical_navigation`, `shell_dismiss`). Not modelled: edge swipes (volume/brightness), left/right on the shade (volume), the 10 s watch peek, cues and sounds, the mood ladder and rest/dream compositing, the room-light IMU input.
- **The clock** shows 10:08 plus the stub clock's seconds, switched on only while the nav space is WATCH, as `main.c:2793` does.

## Findings & gotchas

**[2026-11-10] The tool cannot see the panel**
A framebuffer is not the glass: the CO5300 AMOLED black, gamma and the QSPI tearing are not here, and frame times are the stub clock's, not the device's. A preview that looks smooth says the eases are right, not that the device holds 24 fps (`frame-cost.md`).

**[2026-11-10] Determinism**
The same script twice gives byte-identical frames (`--check` compares an FNV hash per frame). It is not in `host-tests.sh`: the check needs a Docker build of one more target plus two runs, about ten seconds.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `components/jr_display/tests/probe/glass_preview.c` | The player. |
| `components/jr_display/tests/frame_scenes.inc` | `fc_setup`, the scenes, `fc_hash`. |
| `components/jr_display/src/jr_display.c:1252` | `overlay_fade_tick`, which advances every ease once a frame. |
| `main/main.c:3541` | The swipe handler the verbs mirror. |

---

## Open questions

- Decoding the dial and face EAFs on the host would replace both stand-ins with the exact device pixels.
- Whether the face rebind on the device hides the one-frame cut to black that the preview shows when entering WATCH.

---

## See also

- [Frame cost](./frame-cost.md) — the probe and the scenes this builds on.
