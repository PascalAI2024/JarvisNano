# Device evidence

Tracked, durable proof that firmware actually ran on hardware — boot logs,
serial captures, screen snapshots, measured numbers.

**Why this directory exists:** `.build_logs/` is gitignored (`.gitignore:8`), so
every device artifact ever captured there was invisible to git, to CI, and to
anyone auditing the repo. That is the mechanical reason the project reached
2026-07-18 with no in-repo evidence that the v5 image had ever booted, despite
it booting fine. Evidence that isn't tracked isn't evidence.

**The rule** (see `docs/ARCHIVE/JARVISNANO_OS_PLAN.md`): no phase closes without
a photo, video, or log of the device, committed here. A green test suite does not
substitute — the project had 90 passing host tests and a 3,000-turn soak while
the display gap went unnoticed.

## Redaction — required before committing

Device-specific identifiers are banned from this repo (see `CLAUDE.md`). Strip
them from every capture: IPv4/IPv6 addresses, MAC addresses, SSIDs, bearer
tokens, API keys. Run `./scripts/check-secrets.sh` before committing.

The scanner explicitly excludes `jr_memory`’s detector implementation and
synthetic guard test vectors; those files contain the literals they are designed
to reject.

## Contents

| File | What it proves |
|---|---|
| `20260901-ring.png` | **The ring as shipped 2026-09-01**, five stops photographed in one walk by `scripts/screens.py` on the live panel, off USB: JARVIS, WATCH, WEATHER (Fort Lauderdale, fetched by the device, aged "5M AGO"), STATUS (LINK/TOOLS lamps, the cell at 89 %, four Wi-Fi bars at −40 dBm, `UP 5M`), ACTIVITY (the last spoken turn). DESK is absent because no companion was live — the ring admits it only then. |
| `20260901-status-closed.png` | STATUS at a glance, closed: the battery arc with the percentage inside it, the power word, Wi-Fi bars with the dBm, and the headline that says the worst thing or the uptime. The open sheet is not committed: its IP row is a device identifier. |
| `20260719-choice-arcs.png` | **STATE-05/06 choice arcs on glass — the centerpiece.** Three arcs hugging the bezel at r223-231 with the 12 o'clock gap reserved for the question, drawn entirely in the baked art's free band. Hit geometry verified against the rendered pixels: a sweep at r=227 returns 9 samples each for arcs 0/1/2 and 9 for the gap; a centre tap and a tap on the face both correctly refuse to answer. |
| `20260719-hud-speaking.png` | The 48-bar reactive waveform live on the panel at amplitude 246 — spoke length is driven by real playback audio, not a loop. Measured cost: 1.7 fps (18.0 -> 16.3), down from a 4 fps hit before per-strip culling was added. |
| `20260719-hud-idle.png` | Idle breathing ring — the calm state. |
| `20260719-thinking-spinner.png` | **STATE-03 on glass — the first element of the JarvisNano OS design to render on the physical panel.** A real panel-submission-mirror frame captured mid-THINKING: the baked reactor face underneath, the procedural dim cyan track ring at r=150, and the orbiting comet with its white-hot head. Verified numerically, not just visually — 0 cyan pixels on the r=150 ring when not thinking, 274 while thinking, clustered at 141-142 deg (a localized comet, not a ring artifact). |
| `20260719-watch-face.png` | Ambient watch face (UI-01). |
| `20260719-demo-reel.png` | Attract reel (`POST /api/demo`). |
| `20260719-tap-ripple.png` | Touch ripple (TRANS-05). |
| `20260902-glass-done.png` | The first announced delegation: `DONE: WRITE A TWO-LINE…` caption on the muted glass at the poll after a desk completed the item on the board (caption wrap fixed the same evening). |
| `20260902-dial-concepts-codex-diver.png` | Codex image-gen concept sheet (2×2, dial only, no hands) for the diver family; the owner chose the top-left (black sunburst, gold-rimmed lume dots, gold-framed date window). 1024 px copy of the 1244 px original; the generator reads this file. |
| `20260902-dial-concepts-codex-dress.png` | Codex concept sheet for the dress family; chosen: top-left (brushed champagne, applied gold batons). |
| `20260902-dial-concepts-codex-pilot.png` | Codex concept sheet for the pilot family; chosen: top-left (matte black, white Arabic numerals, three empty recessed sub-dial rings — no printed scales, so live needles read cleanly). |
| `20260902-dial-concepts-codex-future.png` | Codex concept sheet for the futuristic HUD family; chosen: top-right (cyan rings, gold accents, arc-reactor centre, four empty data cells). |
| `20260902-dial-diver.png` | The baked DIVER dial exactly as the palette EAF stores it (466 px, 255 colours, RAW/RLE per block): centre hole filled along the radius, the date window's paper blanked at x 376–416 / y 231–256 for the live day. A preview of the clip bytes, not a panel photograph. |
| `20260902-dial-dress.png` | The baked DRESS dial (255 colours, RAW/RLE per block). |
| `20260902-dial-pilot.png` | The baked PILOT dial (RLE under a black floor of 28): sub-dial rings measured at (117,233) r59, (349,233) r59, (233,337) r60. |
| `20260902-dial-future.png` | The baked FUTURE dial (RLE, floor 28): cells measured at x 136–328 / y 91–132 (top), x 136–328 / y 337–379 (bottom), x 36–107 / y 210–251 (9), x 357–429 / y 210–251 (3). |
| `20260902-watch2-diver.png` | The baked DIVER dial on the glass after `jarvisctl art` (2026-09-02 evening): the Codex sunburst with gold-rimmed lume markers under the anti-aliased Mercedes/sword/lollipop hands, the live day `02` in the blanked window at 3, caption `WATCH - DIVER`. Captured muted at WHISPER (brightness 22), which is why the art reads darker than `20260902-dial-diver.png`. |
| `20260902-watch2-dress.png` | The baked DRESS dial on the glass: brushed champagne, applied batons, gold dauphine hands, no seconds. Muted/WHISPER capture. |
| `20260902-watch2-pilot.png` | The baked PILOT dial on the glass: matte black, white numerals, three recessed sub-dials with live needles (6 seconds, 9 battery, 3 temperature). Muted/WHISPER capture. |
| `20260902-watch2-future.png` | The baked FUTURE dial on the glass with the four live cells: `WED 02 SEP`, `77° OVERCAST / 87 / 76`, `100%` with the battery arc, Wi-Fi bars + `MUTED`; slim cyan hands, hairline gold seconds, the art's arc-reactor centre under the hub. |
| `20260902-watch2-jarvis.png` | JARVIS style on the same image and art — the black disc and the original hands, unchanged. |
| `20260902-watch2-minimal.png` | MINIMAL style — the procedural warm-white disc, no clip. |
| `20260902-watch-jarvis.png` | The WATCH screen as arrived at from JARVIS on the 2026-09-02 watch-styles image, style JARVIS (cyan hour, white minute, gold seconds; pixel-identical to the shipped face, checksum-pinned in `test_watch_jarvis_is_pixel_identical`). The violet ring is OTA probation, not a companion. |
| `20260902-watch-diver.png` | Style DIVER after one `POST /api/debug/input?kind=swipe&dir=right` on WATCH: cream triangle at 12, bars at 3/6/9, discs elsewhere, broad hands with the lume disc, the lollipop seconds; caption `WATCH - DIVER`. Retaken after the triangle became a per-row solve (the first cut combed). |
| `20260902-watch-digital.png` | Style DIGITAL, one more swipe right: seven-segment 24-hour `02:08` in cyan and the seconds as gold dots filling clockwise from 12 at r=182; caption `WATCH - DIGITAL`. |
| `20260902-watch-minimal.png` | Style MINIMAL, one more swipe right: thin white hands, twelve dots, a tiny hub, no seconds hand; caption `WATCH - MINIMAL`. A fourth swipe right wrapped to JARVIS and two lefts walked back to DIGITAL (`display.watch_style` in `/api/cockpit`), and DIGITAL came back after `jarvisctl reboot` (`watch style restored: DIGITAL`). |
| `20260902-hands-jarvis.png` … `20260902-hands-future.png` | The six watches of the second cut as the device rendered them on 2026-09-02 (framebuffer mirrors, muted, in probation — the violet ring): JARVIS unchanged; DIVER, DRESS, PILOT and FUTURE over their baked dials with anti-aliased bevelled hands, DIVER's live day in the art's date window, PILOT's three needles (temperature, seconds, battery), FUTURE's date, weather with hi/lo, battery and link cells; MINIMAL on its drawn warm-white dial. Render cost per style is in `docs/GLASS_DESIGN.md`. |
| `20260902-muted-watch.png` | Muted is a watch: the framebuffer six seconds after a muted boot on USB — hands and hub over a black dial inside the gold privacy ring, the ladder at WHISPER (log: `mood -> WHISPER brightness=22`, `cpu gear: 160 MHz`), 5 fps measured, and the image confirmed valid at 56 s under the cadence-following fps floor. A framebuffer does not prove the panel brightness; the CO5300 log lines in the same capture do. |
| `20260902-face-muted.png` | The muted face on the 1.75C after the art partition landed over `/api/ota/assets`: the closed gold iris, the gold bezel, and the muted caption from the same snapshot. |
| `20260719-live-caption.png` | Live captions on glass. |
| `20260719-muted-caption.png` | MUTED status chip. |
| `20260719-ask-real-gemini.png` | Real Gemini `ask_user` on glass. |
| `20260719-ask-long-labels.png` | Long choice labels still hit-testable. |
| `20260719-hud-listening.png` | Listening state on the baked face. |
| `20260814-mood-rtc-flash-report.md` | Historical hardware flash report for the mood ladder and PCF85063 RTC, including four defects found and their on-device re-verification. |
| `20260718-v5-boot-after-phase0.log` | The Phase 0 cleanup did not regress the device: identical boot path after removing the `jr_transport.c` placeholder, the `jr_vad_*` stub and `jr_dsp_resample_linear`, and after adding `hud_render.c` to the `jr_display` SRCS. |
| `20260718-v5-boot.log` | Historical boot proof from the earlier 1.75-inch board revision: PSRAM, SD, CST9217, CO5300, EAF assets, Wi-Fi, codec/AEC, and Gemini Live reached Listening. Its SD observation is not a 1.75C hardware claim; the primary 1.75C has no microSD slot. The log also preserves the then-open association retries and `transport_poll_write(0)` misclassification. |
