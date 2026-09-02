# JarvisNano product plan

Current plan, reconciled **2026-08-28**. This file contains only active work and
release gates. Completed interaction and reliability waves are preserved in
[`docs/ARCHIVE/PLAN-2026-08-27-waves-1-5.md`](docs/ARCHIVE/PLAN-2026-08-27-waves-1-5.md).

The live product is the 32 MB **Waveshare ESP32-S3-Touch-AMOLED-1.75C** running
plain ESP-IDF v5 from `main/` and `components/jr_*`.

## Shipped baseline

- Native 24 kHz full-duplex codec path with 16 kHz AEC-cleaned Gemini uplink.
- Direct Gemini Live transport, server VAD, reconnect/re-arm policy, and
  privacy-safe recovery.
- One overlay compositor with baked EAF faces, listening halo, Watch, Power,
  Desk, Tools, controls, captions, and remote canvas.
- One physical grammar: global left volume, global right brightness, PWR
  listen/battery, BOOT controls, glass hold privacy.
- Operator lease, bounded diagnostics, incident log, Wi-Fi OTA probation, and
  deterministic host suites.
- Typed JarvisMCP server policy and catalog search. Device-side result projection
  is still constrained by a temporary three-result limit.

## Active wave — cleanup, reliability, refinement

Every behavioral row closes on physical 1.75C evidence, not compilation alone.

| # | Priority | Deliverable | Status | Acceptance gate |
|---|---|---|---|---|
| N6.1 | P0 | Ship one interaction grammar; delete circular-rotation experiment | LIVE; PWR/BOOT hand proof pending | No rotate symbols; PWR short listens without muting; BOOT short toggles controls; BOOT hold opens one 60-second pairing claim; edge levels work from every surface |
| N6.2 | P0 | Add playback-gap and ring-low-watermark telemetry | LIVE 2026-09-01, and it found the choppiness | `/api/device/health` → `playback:{underruns,max_gap_ms,low_water_ms,dac_failures,replies}`. An underrun is the feeder finding the ring empty mid-reply with audio resuming within 1.5 s (end-of-reply silence is not counted); low-water is the emptiest the ring was when a chunk ARRIVED during a reply, sampled producer-side. Remaining gate: a 60 s reply with no unexplained gap >120 ms |
| N6.3 | P0 | Byte-budget JarvisMCP results with cursor projection | **DONE 2026-09-01 — and execute_tool works at last** | Root cause of "no search": on this device's legacy `/act` route `execute_tool` always returned "requires typed device gateway", and three raw search matches (4–5 KB) were cut mid-JSON. Now: `search_tools` projects eight matches to `{tool, what, params}` (~1.5 KB) with `has_more` and the basics (websearch, weather, wiki) pinned; `execute_tool` generates a bracket-path, positional-first call on the gateway from a **device-side read-only allowlist** (the legacy key has full authority — a probe ran `dokploy.project.all()` with it). **Proven by voice:** one tool call each — SpaceX headline from the live web, Fort Lauderdale 83 °F overcast, Bitcoin $77,123. Cursor paging still open; a typed route remains the right long-term policy boundary |
| N6.4 | P0 | Run uninterrupted powered conversation soak | **RUN 2026-09-01 (USB, 18 spoken turns / 30 min)** — partial pass | Passed: 18/18 turns spoke, TX drops 0, flush errors 0, rx drops 0, largest internal block ≥31.7 KB throughout, final state Listening. **Deaths 3 — every one a server `goAway` (~every 10 min), reconnected in 2.0 s with no turn lost; the gate should exclude those.** Failed: playback holes — 0 for eleven turns, then 7 (max 955 ms) at reply starts where a 1.3 s server stall exceeded the 600 ms pre-roll. Leads raised to 1000/1500; an 8-turn re-run then met a 2.16 s stall (5 holes, max 478 ms). The source paces near real time and stalls for seconds; a lead ≥ the stall is the only cure and costs the same in first-word latency. Remaining gate: agree the ≤120 ms figure against measured server pacing, or accept ≤1 hole/turn |
| N6.5 | P1 | Split `main/main.c` at existing ownership seams | PENDING | Input/buttons, HTTP diagnostics, and voice/power policy become three modules; `main.c` <4,000 lines; all builds/suites pass |
| N6.6 | P1 | Make one canonical host-test command | PARTIAL — `scripts/host-tests.sh` | One command runs core, transport, display, tool-template, desk CLI, and shell suites; CI calls it |
| N6.7 | P1 | Remove repository/document truth drift | IN PROGRESS | Root plan is current-only; historical plans are archived/labeled; all relative links resolve; no obsolete interaction claims remain |
| N6.8 | P1 | Consolidate visual interaction specification | DOCS ALIGNED; final capture set pending | Vision, hardware, and protocol agree; Jarvis, Watch, controls, privacy, and side-space captures match |
| N6.9 | P1 | Raise settled controls cadence | PENDING | Full controls remain present at ≥14 FPS, zero flush errors, largest internal block ≥24 KB |
| N6.10 | P1 | Prewarm face assets without blocking short turns | PENDING | First Think and Speak apply in <150 ms; stale requested phases never flash late |
| N6.11 | P2 | Add rolling frame/network-burst diagnostics | NETWORK HALF LIVE 2026-09-01 | `/api/device/health` → `rx:{frames,max_gap_ms,queue_wait_max_ms,queue_hwm,drops}`; `POST /api/debug/audio-stats?reset=1` zeroes rx + playback. Display frame-gap p95 still pending |
| N6.16 | P0 | **Adaptive jitter buffer — "choppy" fixed and measured** | SHIPPED 2026-09-01 | Cause: Gemini paces reply audio near real time with 0.8-1.34 s stalls mid-sentence (transcript words arrive on the audio's cadence; a hole sat in a 1.75 s silence between two words). Before: one turn = 7 underruns, 834 ms hole, ring at 8 ms; a 4-sentence reply also DROPPED 8 frames at the 24-deep WS queue. After (600 ms pre-roll, 1000 ms refill after a hole, queue 96): **0 underruns and 0 drops across four turns**, absorbing stalls to 1.34 s, for +0.3 s before the first word. Live knobs `/api/debug/gain?preroll=&refill=`. Remaining gate for N6.4: a 30 min soak reading these counters |
| N6.12 | P2 | Move stillness detection to the QMI8658 wake engine on **INT1** | PIN RESOLVED; PROBE PENDING | Pin was INT2 in this row and is wrong: the C schematic routes **INT1** (`QMI_INT2` appears once in the whole document, so it has no second endpoint) — see `docs/reference/imu-interrupt-routing.md`, which also carries the sourced WoM register sequence. Gate: scope-prove GPIO21 toggles; preserve flip/shake/lift **across the mode switch** (WoM outputs no data, so it disables both while armed); still task wakes <2/s |
| N6.13 | P0 release | Complete authenticated, signed OTA | BLOCKED ON RELEASE CREDENTIALS | Wrong-key/unsigned images fail before write; upload is authenticated; Secure Boot eFuse remains separately attended |
| N6.14 | P0 release | Protect Wi-Fi and cloud credentials at rest | BLOCKED ON ATTENDED KEY PROVISIONING | NVS/flash encryption is enabled through a separately attended procedure; a physical flash read cannot recover credentials; recovery is documented and tested |
| N6.15 | P0 release | Ship complete third-party notices and dependency inventory | PENDING | The exact resolved firmware dependencies, versions, licenses, and required notice texts are generated after build and attached to every binary release |

## Wave N7 — the glass: one interaction grammar, endless screens

Design of record: [`docs/GLASS_DESIGN.md`](docs/GLASS_DESIGN.md) (what surfaces
exist and why) and [`docs/INPUT_MAP.md`](docs/INPUT_MAP.md) (what every input
does, and why it is guessable). `scripts/gesture-doctor.py` lints four of these
invariants from the tree and must stay green.

### Shipped — on the device, evidence in the commit

| # | Deliverable | Commit | Evidence |
|---|---|---|---|
| N7.1 | Close the tap/swipe dead band | `232fe985` | Drift 31–41 px emitted NO event; tap slop now equals swipe min travel. Doctor asserts no gap |
| N7.2 | An open ask claims rim swipes, hit-tested at the landing point | `232fe985` | Arcs sit at r215–255 where a press always rolls; measured 44 swipes to 12 taps while answering |
| N7.3 | Un-trap the shade; every capturing surface names its exit | `80636399`, `956d194a` | Was: only UP escaped, DOWN did nothing, caption cleared. Doctor asserts ≤2 `caption_clear()` sites |
| N7.4 | Refusal ≠ acceptance ≠ unbound | `1e585570`, `617d37e3` | Contracting dim ring + falling 700→300 Hz; neutral ack for unbound; ADJUST no longer ripples |
| N7.5 | An open ask outranks double-tap-home | `20a921c5` | A fast retry after a missed arc was fired as `nav_home` over a live question. Doctor asserts the guard |
| N7.6 | Both level slabs increase on UP | `20a921c5` | Brightness rose on DOWN while volume rose on UP |
| N7.7 | Delete the side pages and the fake TOOLS feed | `6de40bd2`, `1a7485dd` | `{SEARCH, MEMORY, WEATHER, MORE}` was seeded once with `recent` pinned to 0 |
| N7.8 | HAL contact lifecycle (`PRESS_DOWN`/`PRESS_UP`) | `1d07f621` | Every prior event was terminal, so a filling ring had nothing to drive it. Queue 8→16; excluded from gesture telemetry |
| N7.9 | Hold-to-commit ring, abandonable | `1a081e7e`, `617d37e3` | Owner's long-press counter read 0 — the gesture was invisible. Previews at 400 ms, fills to 850 ms, silent abandon |
| N7.10 | Rim as a true annulus (r ≥ 168) | `3cbab992` | Slabs reached r≈93, over the reactor core. Doctor asserts both predicates agree |
| N7.11 | BOOT ≥5 s → panic-home | `77824301` | The band was unbound; a hold past 5 s did nothing |
| N7.12 | Endless mode ring, vertical slide | `d66db45a`, `5f8d12bd` | Wraps both ways; slide axis corrected from horizontal to vertical to match the finger |
| N7.13 | Dev mode: no pairing token on diagnostics | `2be528e8` | ⚠️ **Set `JR_DEV_OPEN_DIAGNOSTICS 0` before any release.** Boot logs warn every boot |

| N7.14 | Give TOOLS real content | `2dc86bf4` | Petals are the real declared catalog (`s_device_tool_fns`); lit petal is the tool that actually ran; nothing lit when nothing has run |
| N7.15 | WATCH / POWER / MOTION on the ring | `2dc86bf4`, `2700d113` | Live data only. WATCH uses the EXISTING `hud_overlay_clock` — an earlier pass wrongly replaced it with a home-made two-arc clock |
| N7.16 | Position indicator survives a wrap | `708dd280` | Was worse than recorded: `(2*i-3)*7` was tuned for 4 screens, so 5 and 6 fell outside the rail. Now evenly spaced on the full dial, active mark takes the signed shortest path |
| N7.23 | Nav word held only 4 screens | `307c8d24` | `NAV_SPACE_MASK` was 2 bits, so DESK/TOOLS/SETTINGS were unreachable. Widened to 3 + `_Static_assert` so a 9th screen fails the build |
| N7.24 | One canonical host-test command | `scripts/host-tests.sh` | Runs BOTH suites and asserts a positive count — "nothing ran" exits 2. Written because only one suite was being run and 8 failures sat unnoticed |

### Open

| # | Priority | Deliverable | Status | Acceptance gate |
|---|---|---|---|---|
| ~~N7.14~~ | — | done — see shipped table | DONE | TOOLS renders a tool that actually ran (name + outcome) within 2 s of a real tool call, or the screen is removed from the ring. **No hardcoded lists** — an empty screen telling the truth beats a full one that lies |
| ~~N7.15~~ | — | done — see shipped table | DONE | Each shows only live data: WATCH via `hud_overlay_clock` (exists, strip-tested); POWER via `jr_power` (%, charging, mV); MOTION via `jr_imu` (live tilt). Each passes strip invariance and holds ≥14 FPS |
| ~~N7.16~~ | — | done — see shipped table | DONE | Wrapping must not jump the indicator the width of the dial. Draw position as a **rotating** mark; the caption naming each screen is the interim answer |
| N7.17 | P1 | Input layer stack (`CONSUMED`/`PASS`) | OPEN — sequenced after deletions | Dispatch is ~430 lines with 20+ `continue`s. Precedence becomes a declared table; every affordance in `INTERACTION_MODEL.md` §5 still reachable; no binding changes |
| N7.18 | P2 | Enforce the coverage invariant in a host test | OPEN | A table lists every binding and its non-gesture (voice) equivalent; a binding with no equivalent fails the build. Pairing is the one allowed exception (physical-presence proof) |
| N7.19 | P2 | Re-home the OTA ring, then delete the SETTINGS renderer | DONE — this pass | The ring is shell-wide (`apply_ota_ring`, drawn above the watch, gated on the OTA word alone) so it reaches JARVIS at rest; UPDATE/SLOT rows moved to the POWER sheet; `nav_set(SETTINGS)` in the upload path and the SETTINGS space, focal, composer, headline and gauge helpers are gone. Host tests ring every space and prove JARVIS at rest still draws nothing when idle; mutation (gating the ring on `s_space_on`) fails two tests |
| N7.20 | P2 | Remove the orphaned shell setters | REFUTED | `jr_display_desk_set_task` and `jr_display_tools_set` HAVE a production caller — `publish_shell_state()` in `main/main.c` feeds both every status tick — so they are not orphans. Only `jr_display_space_set_label` is test-only; it is the caller-override hook the headline tests rely on. Nothing to remove |
| N7.21 | P2 | PWR double-tap for privacy | BLOCKED BY MEASUREMENT | The AXP2101 PKEY latch polls at 500 ms (`jr_power.c:33`), so a ~400 ms window cannot be resolved; `iot_button` cannot help (GPIO/ADC only). Needs a faster PKEY poll (costs shared-bus traffic) or an AXP multi-press feature. Until then privacy stays on the glass hold |
| N7.22 | P2 | Auto-upright the procedural overlay | OPEN | 4-way quadrant snap with hysteresis + debounce, procedural layer only. **The choice-arc hit test must take the same offset** or taps target pre-rotation positions. Baked faces stay fixed; `hud_tilt_offset()` is the wrong hook (translation, wrong axes) |

## Wave N8 — coherence pass: one visual grammar across the ring

Sources: a labelled sweep of all six screens (`scripts/screens.py`), a Grok
`grok-4.6` review of the renderers, and a Codex `gpt-5.6-sol` review. Findings
are only listed here once they survived a check against the code or the glass —
the bench proposes, the tree decides.

**Two QA-tooling defects were found first, and they came before any firmware
finding, because both had manufactured false evidence:**

| # | Defect | Status |
|---|---|---|
| N8.0a | `screens.py` kept a hand-written ring list that had drifted — it named a `MOTION` screen the firmware has never had, so every tile from the fourth on wore the wrong caption and the sweep looked like the ring skipped a screen and wrapped early | **fixed** `db3bb607` — the ring is now parsed from `jr_display_space_t` |
| N8.0b | `screens.py::post()` swallowed every failure. `/api/ui/shade?open=1` is the wrong parameter (the handler wants `?action=open`) and returns HTTP 400, so the shade never opened and a plain JARVIS screen was captioned CONTROLS and read as a render defect | **fixed** — `post()` raises by default; callers that are genuinely optional opt out |

The lesson both share: a silent failure in a QA tool does not leave a gap in the
evidence, it produces confident WRONG evidence. Anything the sweep asserts must
fail loudly or be derived from the tree.

### Verified against the device or the code

| # | Finding | Evidence | Fix |
|---|---|---|---|
| N8.1 | **POWER renders a nearly-full battery as a small wedge.** `sp_in_span` intersects two half-planes, which cannot express a wedge >180°; above 50% the fill SHRINKS as charge rises, and at 100% both edges coincide and it collapses to a ray | **Measured.** Predicted arc for pct>50 is `360 − pct×3.6` degrees starting at `pct×3.6 − 180`. At ~80% that is 70° starting at 110°; the panel returned **70° starting at 111°**. The full-looking gold ring in the sweep is the dim *track* underneath, not the fill | Segment the fill at 96 units as DESK and the OTA ring already do (`2421-2430`, `2616-2627`) |
| N8.2 | **WATCH wipes the battery arc and the gold privacy ring.** `apply_clock_overlay` `memset`s the whole strip after `hud_overlay_frame` has painted the persistent outer band (r215–222) | Code read: the `memset` is unconditional and runs after the frame. Consequence: **mute state is invisible on the one screen you glance at** — which bears directly on "privacy not working well" | Clear only inside the clock bbox (hands reach r190), or repaint battery + privacy after the clear |
| N8.3 | WATCH printed the word `WATCH` under the hands — the caption is the ring's position indicator, but a clock face is the one screen that identifies itself | Seen in the sweep; hands reach y≈407–423, inside the caption band y360–430, so it also collided with the art | **fixed** — `mode_name[WATCH]` is `""`; `caption_set("")` routes to `caption_clear()` |

### Reported by the bench, not yet validated — validate before scheduling

| # | Finding | Why it is plausible |
|---|---|---|
| N8.4 | POWER treats USB-present as "charging": focal + headline test bit 24 (USB) where the SETTINGS sheet correctly tests bit 25 (charging) | Same quantity rendered two ways in two places; a full plugged-in device would read `100% CHG` |
| N8.5 | A missing fuel gauge (`0xFF`) clamps to `pct = 100` in the focal renderer while the headline says `NO BATTERY` — a full ring above the words "no battery" | Clamp and headline disagree by construction; compounds N8.1 |
| N8.6 | `SETTINGS` headline `"VOL 100%  100%"` is 14 glyphs against `SP_LABEL_CAP` 13, so brightness loses a digit whenever volume is 100 | Pure arithmetic; check the glyph box still clears the OTA ring if the cap is raised |
| N8.7 | Shade `"R LIGHT 100%"` is 12 glyphs against `SP_COL_MAX` 11 → `"R LIGHT 10"`. `"L VOL 100%"` is exactly 10 and survives | Two controls of the same kind with different format budgets |
| N8.8 | The agent rim (r224–230) paints over the choice arcs and commit ring (r223–231) with no ask/commit gate | That band is documented exclusive in `GLASS_DESIGN.md`; an in-flight tool during an ask stacks two tenants |
| N8.9 | Tapping WATCH or POWER opens a sheet headed `SETTINGS` — both fall through to the `default` case in the detail composer. JARVIS's own `SESSION` sheet is unreachable | Detail composer only has cases for JARVIS/DESK/TOOLS |
| N8.10 | The same percentage is drawn in three type systems across DESK, POWER and SETTINGS | Visible in the sweep: DESK sets `0%` large in-ring, POWER sets `85%` small under the arc |

### Fixed since the sweep — verified on the panel

| # | Fix | Commit | Evidence |
|---|---|---|---|
| N8.1 | POWER segments its arc; `sp_arc_segments()` replaces three open-coded copies | `f456a027` | Before: 79% battery drew **70 deg at 111 deg**. After: 77% battery draws **278 deg, against 277 expected**. Measured on the panel after a confirmed fresh boot |
| N8.2 | WATCH's clear is bounded by `JR_DISPLAY_SHELL_R_MAX` | `f456a027` | Gold privacy ring rgb(222,158,33) present at r221 (161/360 lit) and the cyan battery arc at r217, while r190/r210 stay fully cleared. Both were previously blanked |
| N8.4 | `" CHG"` and the charging core decode bit 25, not bit 24 (USB) | `f456a027` | Packing confirmed at `jr_display_power_set`; the SETTINGS sheet already tested bit 25 |
| N8.5 | An unanswered fuel gauge draws the bare track, never a full ring | `f456a027` | Was a full ring beneath the headline "NO BATTERY" |
| N8.19 | **Expired agent links now clear the glass.** The TTL branch in `publish_shell_state()` had an EMPTY body, so `active` was never cleared: DESK kept rendering a dead task and the agent rim stayed lit indefinitely with no way to dismiss it | `eee9bb1c` | A stale surface that outlives its owner reads as a frozen device. Strongly suspected cause of "there's something on screen stuck, can't even click it" |
| N8.20 | **Live captions show the newest words.** The accumulator holds 128 chars but the band renders 2x19=38 and `hud_wrap2` fills from the FRONT, so a reply over 38 characters displayed its opening and froze | `eee9bb1c` | Caption only moved once the buffer overflowed 128 and dropped from the head — far too late, and it reads as a device that stopped listening mid-answer |

### Also reported by gpt-5.6-sol — validate before scheduling

| # | Finding |
|---|---|
| N8.21 | The transient-surface card is a rectangle whose corner (52,72) sits at ~r242 while the glass ends at r232.5, so the bezel chops all four corners. Wants a round-native plate |
| N8.22 | Every transient surface prints the fixed header `CODEX DESK`, including local `SAVE MEMORY?` consent prompts, which are not Codex Desk content |
| N8.23 | Canonical tool ids are used directly as labels: `recall_memory` is 13 glyphs against a 12-glyph shell, rendering `RECALL-MEMOR`. Needs display aliases separate from canonical names |
| N8.24 | Agent Link titles accept 48 characters into a 13-byte display cache — silent truncation with no wrap or ellipsis |
| N8.25 | Only the first four catalog tools are displayable; every later tool maps to recent `-1`, so after `execute_tool` / `set_volume` / `set_brightness` TOOLS shows a blank headline and `LAST NONE` |
| N8.26 | The orbit rail occupies r184-196 where the measured free band is r185-194, overwriting baked art at r184 and r195-196 |
| N8.27 | POWER uses amber below 20% while the persistent battery arc uses red — the same battery state in two alarm hues at once |

Both benches independently reported N8.2, N8.4 and N8.5. Two models converging
on the same lines is why those were treated as confirmed rather than plausible.

### Adversarial sweep — 34 agents, 26 findings, 6 refuted, 20 confirmed

Eight independent lenses over the firmware, every finding then handed to a
verifier told to REFUTE it and to default to refuted when uncertain. Six died
there, which is the point of running it that way.

**Fixed and on the device:**

| # | Finding | Commit | Evidence |
|---|---|---|---|
| S1 | **The clock clear still ate the control shade.** The previous guard tested only `s_detail_ease`, so on WATCH the shade drew and was then wiped: a black dial, the caption "CONTROLS - UP TO CLOSE", and an invisible-but-LIVE volume arc and privacy button that `jr_display_hit` still routed taps to. A blind tap in the lower disc could flip the microphone | `2093da40` | **Mutation-verified.** Reverting the guard: `0 of 123556 shell pixels survived`. Pinned by `test_clock_clear_spares_open_shell_surfaces` (`ac936a92`) |
| S2 | **Gold in the outer band meant two different things.** The battery rim (r215-220) painted gold while charging, one pixel from the gold privacy ring (r221-222), so a charging device read as a muted one | `2093da40` | **Measured while charging.** r=217 gold=0, cyan over an 18 deg wedge (the battery arc); the gold at r219-223 spans 0..359 deg, i.e. the privacy ring, not a wedge |
| S3 | POWER drew the alarm-amber core when the fuel gauge did not answer — an amber "low battery" core floating in a deliberately empty ring | `2093da40` | Core is now track-coloured when `have_gauge` is false |
| S4 | **Codex-mode tap precedence was inverted in both directions.** The TAP branch set `codex_tap_pending` and then fell straight onto `codex_tap_pending = false` in the same iteration, so the flag could never be true when the next tap tested it: the double-tap escape never fired and the deferred tap was never dispatched. Meanwhile the `else` arm `continue`d, swallowing swipes — the opposite of the comment beneath it | `3175d822` | Direct cause of "there's something on screen, can't even click it". I had previously read this block, found the 400 ms flush, and wrongly concluded taps were deferred rather than discarded |
| S5 | `mood.h` claimed three times that WHISPER and DREAM "switch the microphone OFF". They close the Gemini session; the codec keeps sampling and WakeNet keeps running, which is what lets "Jarvis" wake the device from DREAM | `3175d822` | Rest is a power state, privacy is a capability state. A header that conflates them is trusted instead of the code |

**Confirmed, still open** — each carries a file:line and a verifier's evidence
in the run transcript:

| # | Finding | Severity |
|---|---|---|
| ~~S6~~ | **fixed 2026-09-01** — `JR_DISPLAY_TOOLS_MAX` is 8, petal width follows the count. **Panel:** 8 distinct arcs at r86 (was 4). Host test lights the eighth petal and counts eight arcs; mutation to 4 fails all four assertions | high |
| ~~S7~~ | **fixed 2026-09-01** — the marked title IS the DESK sheet's head (12 glyphs, exactly what `title_shorten()` yields); the JOB row is gone. **Panel:** sheet heads `DEPLOY.` for "DEPLOY STAGING BUILD TO THE FLEET", mark intact | high |
| ~~S8~~ | **fixed 2026-09-01** — `panic_home_clear_glass()` stops the demo reel, tears down the touch challenge, turns the test pattern off and clears the canvas before the overlays. Not panel-provable without a BOOT hand hold; code-verified | high |
| ~~S9~~ | **fixed 2026-09-01** — same helper takes `s_brain_lock`, resolves a local consent prompt as a timeout (denial), and clears `s_brain_surface.active`; falls back to clearing the glass alone with a warning if the lock is busy | high |
| ~~S10~~ | **fixed 2026-09-01** — the span test now uses the same `dy` as the annulus. Host test draws POWER at rest and 60 px into the slide and requires the focal band to be a pure translation; the old code fails it by 979 pixels | medium |
| ~~S11~~ | **fixed** `569fadc2` — the card is a round plate bounded by `JR_DISPLAY_SHELL_R_MAX`; the outside is left alone | medium |
| ~~S12~~ | **fixed 2026-09-01** — `caption_reset()` at peek expiry and at the input-drain clear. **Panel:** peek showed `4:09 PM`; 12.5 s later the caption band held only the baked face | medium |
| ~~S13~~ | **fixed 2026-09-01** — the double-tap guard also yields to an open shade (both doors: nav overlay and the HTTP shell bit). Found on the way: ONE tap on an HTTP-opened shade re-derived "closed" and shut it, because the re-derivation read only the nav door; each door now closes its own. **Panel:** taps 270 ms apart stepped volume 60→70→80 with the shade still open | medium |
| S14 | A contact shorter than one 40 ms poll produces NO event at all — not even PRESS_UP | **left alone, deliberately (2026-09-01):** `TOUCH_PRESS_CONFIRM_SAMPLES 2` is the debounce — the IRQ wakes the task on the down edge and a second read 40 ms later confirms. A contact under ~50 ms is below a finger tap (80–150 ms) and is what the debounce exists to ignore. Revisit only with a measured lost-tap count from `/api/touch` |
| ~~S15~~ | **fixed 2026-09-01** — `jr_display_caption_pin()`: while the OTA warning is pinned, every other set/clear is ignored until the upload path unpins (both failure exits and the success exit do). Host test pins, writes "LISTENING", asserts the warning survives. The chip is still multi-writer for ordinary captions; that is by design, they are ephemeral | medium |
| ~~S16~~ | **fixed 2026-09-01** — gated like the other nineteen. **Measured:** a POST without `X-JarvisNano-Control` now answers 403 | medium |
| ~~S17~~ | **fixed 2026-09-01** — `jr_audio` exports readbacks for mic/ref/out-vol/speak-mic and the endpoint reports them (plus `speakmic`, which was applied but never echoed). **Measured:** a bare POST reads `mic 24, ref 12, vol 100, speakmic 21` | medium |
| ~~S18~~ | **fixed 2026-09-01** — `demo_stop()` dismisses only arcs the reel put up (`s_demo_owns_choices`); a real ask's arcs survive the reel yielding to it | low |
| S19 | The pushed-canvas buffer is rewritten in place from the HTTP task while the render task reads it at full opacity | low — **design noted 2026-09-01:** a second 434 KB PSRAM buffer with a pointer swap is the clean fix but PSRAM sits at ~1.9 MB free mid-session; the cheap alternative (hide the canvas for the rewrite, one frame of face) trades a tear for a flicker. Decide when a host actually re-pushes canvases at a rate where either shows |
| ~~S20~~ | **fixed 2026-09-01** — the handler answers `queued:false` with `reason: running` or `reason: phase` (+ the phase name); the consumer logs the rare late drop. **Measured:** second POST while running → `{"queued":false,"reason":"running"}` | low |

**Open question raised by the S2 measurement, not yet a finding:** at 8%
battery the persistent rim rendered CYAN, but `ov_battery` paints red below
20%. That branch was not touched by S2's fix, so the `batt_pct` reaching the
HUD env word may disagree with the fuel gauge. Measure before assuming.

### S21 — "an operator lease FREEZES the display composition" — REFUTED as a render bug; real cause fixed

**Resolution (2026-09-01).** The composition never froze. Measured on the
panel: `actual_fps` 19 with and without a lease, `flush_errors` 0, and the
idle face's structural delta under a lease (0.49) sat at the no-lease noise
floor (0.31). What both demonstration sweeps had actually hit was
`main.c`'s Codex guard, which dropped EVERY synthetic input kind under a
lease — including the synthetic swipes `screens.py` walks the ring with. The
sweep's navigation was being refused, and the six identical frames were the
honest result. The guard now refuses only the kinds that can escape or act on
a lease (tap, double-tap, hold); a synthetic swipe walks the ring exactly as a
finger does. **Re-measured after the fix:** swipe deltas under a lease
4.7 / 12.5 / 7.0 against 4.3 / 12.5 / 7.9 without one. `choices_active` is now
exposed in `/api/cockpit` under `display`, as the candidate below asked for.

The original record follows, kept for the method note, which is still right.

Found while demonstrating companion mode, and it is not the bug I thought I
had already fixed.

**Measured.** Coarse 10x10 luminance signature of the panel mirror, which
survives the face pulsing but moves when the layout changes:

| condition | signature delta | reading |
|---|---|---|
| same screen, no input (noise floor) | 1.9 | the face animating |
| four swipes, NO lease | 2.7, 9.1, 3.5, 5.3 | navigation reaches the glass |
| four swipes, UNDER a lease | 0.1, 0.2, 0.1, 0.2 | **below the noise floor** |
| no input at all, under a lease | 0.38 over 4 s | composition is frozen |

Under a lease the glass does not merely ignore swipes — it stops repainting
altogether, including the idle face animation. Navigation may well be
happening internally and never reaching the panel.

**This is NOT the input-routing bug fixed in `3175d822`.** That one was real
(taps were discarded and non-tap events were swallowed by a stray `continue`)
and its fix stands, but it addressed routing, not painting. Two demonstration
runs walked the ring under a lease and returned six visually identical frames
each time.

**Method note worth keeping.** The first three attempts to test this reported
"NAV WORKS" and were all worthless: they compared raw MD5 hashes of the panel
mirror, and the arc-reactor face animates, so consecutive frames differ
whether or not anything navigated. The instrument measured animation and was
read as navigation. Any future display assertion must be structural — a coarse
signature with a measured noise floor — never a pixel hash.

**Candidate, unconfirmed:** `main.c:6944` swallows synthetic TAP and SWIPE
whenever `jr_display_choices_active()` is true, which would explain lost input
but NOT a frozen idle face. `choices_active` is not exposed in `/api/cockpit`,
so this could not be confirmed or ruled out from the host. Expose it, or
instrument the render path, before proposing a fix.

### Power management — answering "there are 4 modes on this chip"

Research: Grok `grok-4.6` against the firmware as it actually runs, not generic
ESP32 advice. The four SoC modes are **Active, Modem-sleep, Light-sleep,
Deep-sleep**.

| # | Deliverable | Finding |
|---|---|---|
| N8.11 | **Do NOT enable `CONFIG_PM_ENABLE` + tickless light sleep** | I2S is opened once in `jr_audio_init()` and never closed, which holds an APB lock; WakeNet then reads 24 kHz every 32 ms. A core that must wake 31×/second to keep a codec clocked cannot light-sleep. The datasheet's 240 µA is real and unreachable as built |
| N8.12 | **Make DREAM an actual power mode.** Today WHISPER and DREAM are electrically identical — same radios, same I2S, same 24 fps compositor, same 100 Hz IMU poll. Only brightness differs (22 vs 8), so DREAM is a caption | The AMOLED win is emission: stop the emote, paint near-black, refresh a few dim clock pixels once a minute instead of 24×/second. Must fire only from genuine idle — asks, busy and USB keep the glass lit, per `db3bb607` |
| N8.13 | **`mood.h` claims WHISPER/DREAM switch the microphone off. The code does not.** Stale comment on a privacy-relevant claim | Correct the comment, then decide whether the behaviour should match it |
| N8.14 | USB-present is folded into `user_busy`, so a plugged-in device never rests — fine for a desk toy, fatal for a battery soak over USB | Rest under mute is consequently **unverified on battery**; every mood measurement so far was taken on USB |
| N8.15 | SHELL: an opt-in deep-sleep rung below DREAM (BOOT / touch INT / IMU INT1 are all RTC GPIOs and can wake it) | **A different product, not a deeper DREAM** — it kills the wake word, and there is no RTC on the C board, so the clock dies on reboot. Do not ship it as DREAM |

### Queued features

| # | Deliverable | Notes |
|---|---|---|
| N8.16 | **Companion mode must route voice to the leaseholder, not Gemini** | The oldest undelivered explicit request. Under an operator lease the mic uplink should reach the agent holding the lease and the Gemini uplink should suspend, so talking to the device during companion mode talks to the companion. Audio-path work — does not belong in a display batch |
| N8.17 | Weather screen (Fort Lauderdale) as a seventh ring screen | Enum growth is mechanically safe: 7 fits the 3-bit nav field, the `_Static_assert` guards it, and `screens.py` now tracks the enum. Settle the data path first (device-pull via the jarvismcp bridge vs host-push via the brain surface). City in code; endpoint and keys in NVS per the repo rule; refresh on entering the screen, never a timer that drags Wi-Fi out of min-modem during rest; render the data age, because it will be stale after rest |
| N8.18 | TOOLS may now be dead weight — with the redundant tools removed the screen is three unlabelled arcs | Propose cut or repurpose. Subtraction is a legitimate deliverable; do not delete a ring screen unilaterally |

### N8 execution queue — worked top to bottom, no check-in between items

Every row has a concrete acceptance test. "Looks better" is not one. Items are
ordered by user-visible impact, with cheap high-impact fixes first so the glass
improves continuously rather than in one late lump.

| Order | Item | Acceptance test |
|---|---|---|
**Items 1-12: DONE** (6-7 by the two commits before this pass, 8 refuted, 9-12 on 2026-09-01). Evidence below; the rest of the table stands.

| Done | Item | Commit | Panel evidence |
|---|---|---|---|
| 1-2 | SETTINGS + shade clipping | `543fe75c` | Now "V100% L100%" (11 of 12) and "R LGT 100%" (10 of 10). The old test asserted `strstr(label,"VOL")`, which stayed green while brightness was cut off; it now stages 100/100 and pins the exact strings — and immediately caught a wrong expectation in its own commit (`sp_pct` appends `%`) |
| 3 | Tool petals used canonical ids (`RECALL_MEMOR`) | `efe6504d` | Labels are a separate `char[13]` table, so an over-long label is a COMPILE error; a `_Static_assert` ties it to the catalog and confirmed it is exactly 8 tools |
| 4 | Agent titles: 48 chars into a 13-byte cache | `efe6504d` | `title_shorten()` backs to a word boundary and always marks the cut |
| 5 | Agent rim painted over the choice band | `e5544763` | **Measured.** Rim alone: 1324 lit, all violet (173,0,255). Rim + ask open: 76 lit, all cyan (0,255,255), **zero violet**. Exactly one tenant |
| N8.19 | Expired agent links never cleared | `eee9bb1c` | **Measured.** With `ttl_s=30` the rim went 1324 -> 0. Before the fix the expiry branch was empty and it stayed lit indefinitely |
| 6 | N8.9 WATCH / POWER opened a SETTINGS sheet | `16ffb530` | Both compose their own sheet; host test asserts no space borrows SETTINGS |
| 7 | N8.21 Card corners outside the glass | `569fadc2` | Round plate bounded by `JR_DISPLAY_SHELL_R_MAX`; the outside is left alone so the battery rim and privacy ring survive |
| 8 | N8.22 `CODEX DESK` header on local consent | — | **Refuted.** No such string exists anywhere in `main/`, `components/` or `scripts/`; the local consent surface is titled `SAVE MEMORY?` and remote surfaces carry the title the host sent |
| 9 | N8.26 Orbit rail r184-196 vs the r185-194 band | this pass | **Panel:** lit fraction at r184 0.72 -> 0.01 and at r195 0.69 -> 0.01; r186-190 unchanged at 0.72-0.74. Host test pins every shell pixel outside `JR_DISPLAY_SAFE_R` to r185-194 |
| 10 | N8.27 POWER amber vs rim red below 20% | this pass | One rule, `HUD_BATT_LOW_PCT`, one hue (`SP_C_RED`), both renderers, and neither alarms while charging. Host tests on both renderers at 8%, charging and not. Not panel-provable at 77% charge |
| 11 | N8.10 one numeric style | — | **Left alone, deliberately.** DESK sets its percentage inside the ring at scale 3 because it is the focal object; POWER's number sits under the arc because the arc is; SETTINGS lists values in a sheet. Three placements, one font. Re-open only with a contact sheet that reads wrong |
| 12 | N8.25 Later tools show `LAST NONE` | this pass | Same fix as S6 — the cap was the bug |
| 13 | SETTINGS rebuild | this pass | **Closed by subtraction** (N7.19). `GLASS_DESIGN.md` already called it a diagnostics page wearing a face; rebuilding four gauges in the house palette would have kept a screen whose only production visitor was the OTA updater. What it carried is re-homed: the update ring draws on every space, UPDATE/SLOT live on POWER, volume/light read on the shade, privacy is gold everywhere, LINK is on the SESSION sheet. NET and MEM are diagnostics and stay at `/api/cockpit` |

**API schemas learned the hard way, recorded so the next session does not
re-derive them.** `/api/brain/inbox` requires EXACTLY `v, type, seq, session,
id, ttl_ms, payload` (no extras, none missing — dismiss included, which is why
a dismiss without `payload` returns 422), `payload` requires exactly `kind,
title, body, actions`, `actions` are objects of exactly `{id, label}`, `kind`
is one of notice/progress/result/choice/consent, and `seq` must equal the
session's `next_inbox_seq`, starting at 1. `/api/agent/link` requires
`task_id, revision, state, progress, title, summary`; `revision` must equal
`hwm + 1` (409 otherwise), `ttl_s` is optional but bounded **30..3600** and
defaults to **900**, and `state` must be a known name — "done" is not one.

| 1 | N8.6 SETTINGS headline clips at volume 100 (`VOL 100%  10`) | Set vol=100, brt=100; headline renders both numbers complete, and a host assertion pins the worst-case length |
| 2 | N8.7 Shade brightness clips (`R LIGHT 10`) | Same two levels at 100; shade shows a complete percentage |
| 3 | N8.23 `recall_memory` renders `RECALL-MEMOR` | Display aliases exist separately from canonical tool ids; no label exceeds the 12-glyph shell |
| 4 | N8.24 Agent-link titles: 48 chars into a 13-byte cache, silent truncation | Long title either wraps 2x19 or carries an explicit short display title; nothing is cut mid-word without a mark |
| 5 | N8.8 Agent rim (r224-230) paints over choice/commit arcs (r223-231) | With an ask open AND a tool in flight, sample the band: exactly one tenant present |
| 6 | N8.9 Tapping WATCH or POWER opens a sheet headed `SETTINGS` | WATCH and POWER get explicit cases or `ACT_NONE`; the default becomes an unreachable fallback |
| 7 | N8.21 Card corners sit at ~r242, outside the r232.5 glass, so the bezel chops them | Round-native plate inside the glass; sample the four corners for clipped pixels |
| 8 | N8.22 Every transient surface prints `CODEX DESK`, including local `SAVE MEMORY?` consent | Local consent surfaces carry no foreign header |
| 9 | N8.26 Orbit rail occupies r184-196 against a measured free band of r185-194 | Rail constrained to r185-194, or the band formally re-reserved and documented |
| 10 | N8.27 POWER uses amber below 20% while the battery arc uses red | One battery-state palette, one threshold, both renderers |
| 11 | N8.10 The same percentage in three type systems (DESK, POWER, SETTINGS) | One numeric style for one kind of readout; verified on a fresh contact sheet |
| 12 | N8.25 Only the first four tools are displayable; later ones show `LAST NONE` | After `execute_tool`, TOOLS names the tool actually used |
| 13 | ~~**SETTINGS rebuild**~~ — closed by subtraction, see the done table | The screen is gone; a contact sheet shows four screens in one language |
| 14 | N8.18 TOOLS is three unlabelled arcs now the redundant tools are gone | Cut or repurposed with a stated reason; not left as decoration |
| 15 | N8.17 **Weather screen (Fort Lauderdale)** | Seventh ring screen. Settle the data path first: device-pull via the jarvismcp bridge vs host-push via the brain surface. City in code; endpoint and key in NVS, never the repo. Refresh on entering the screen, never a timer that drags Wi-Fi out of min-modem during rest. **Render the data age** — it will be stale after rest |
| ~~16~~ | N8.13 `mood.h` says WHISPER/DREAM switch the microphone off; the code does not | **Done** `3175d822` (S5): the header now says rest is a power state and privacy a capability state. Decision: behaviour stays — WakeNet keeping the codec sampling is what lets "Jarvis" wake the device from DREAM; the microphone is turned off by privacy, never by rest |
| 17 | N8.12 **DREAM becomes a real power mode** | Today WHISPER and DREAM are electrically identical — same radios, I2S, 24 fps compositor, 100 Hz IMU poll — differing only in brightness, so DREAM is a caption. Stop the emote, paint near-black, refresh a few clock pixels once a minute. **Only from genuine idle**: asks, busy and USB keep the glass lit, per `db3bb607`. Measure battery percent drop over a fixed soak, before and after |
| 18 | Companion: mic ring is 2 s, so the leaseholder must poll every ~1.5 s or drop audio | Longer ring or a streaming endpoint; a 30 s utterance survives without gaps |
| 19 | Companion: a reply path back to the glass during a lease | The leaseholder can speak or caption without borrowing the Gemini session |
| 20 | Re-run the full sheet and re-read it | `screens.py --extras` after the batch; every screen shares one visual language, and the sheet is captioned correctly |

**Standing rules for this queue.** Host suites do not cover arc geometry or
layout — they were green throughout the POWER arc bug — so a rendering claim is
evidenced by a panel measurement, never by a green suite. Every fix builds,
runs both suites, and is OTA'd and re-measured before the next item starts.
`JR_DEV_OPEN_DIAGNOSTICS` must return to 0 before any release tag.

## Wave N9 — a glass that is useful, and alive (2026-09-01, in progress)

Owner's direction, verbatim in spirit: *no search, no JarvisMCP, still some
useless screens — make it cool and useful; don't waste a screen on power, one
status screen; be creative, use the hardware for a one-of-a-kind experience
that is useful, fun, and feels alive.*

| # | Deliverable | Status | Acceptance |
|---|---|---|---|
| N9.1 | **Search and tools work by voice** | DONE `ac3af81c` | One tool call per question; SpaceX headline, Fort Lauderdale weather, Bitcoin price answered aloud |
| N9.2 | **WEATHER on the ring** — Fort Lauderdale, live, aged | **ON THE GLASS** `11de51d3` + data path |  Temperature, condition, hi/lo, rain, age from `jr_display_weather_t`; fetched once after boot and on entering the screen (≥10 min old); never a timer; `NO WEATHER` when nothing was ever fetched |
| N9.3 | **ACTIVITY replaces TOOLS** — what Jarvis actually did | **ON THE GLASS** (three rows after three turns: SAID / WEATHER / PRICE) | Last three turns: kind (WEB / WEATHER / PRICE / MEMORY / TIME / ASK / SAID) + the first words of the reply; `NOTHING YET` when empty |
| N9.4 | **STATUS replaces POWER** — one screen for everything you'd look up | SUPERSEDED by N10.1 the same evening — the owner called the first cut "this junk screen" | Battery arc stays; sheet carries level, volts, USB, charge, update, slot, link, mic, uptime |
| N9.5 | **DESK only while live** | **ON THE GLASS** — the sweep walks five screens with no lease, six with one | Ring skips DESK unless an agent link or lease is active; no stranding when it goes quiet |
| N9.6 | **The glass can talk about what it shows** | **PROVEN** — a tap on the open WEATHER sheet spoke "around 83 degrees, high 86, dropping to 76" | Tap an open sheet on WEATHER / WATCH / ACTIVITY → the assistant speaks it (a text turn; the mic is untouched) |
| N9.7 | **Lift to glance** | flashed; needs a hand test: rest the device 5+ min, pick it up | Picked up after a rest → weather for 8 s → home by itself; any input keeps the screen |
| N9.8 | **Rain today** | flashed; fires once per boot when today's rain ≥10 mm | First good fetch with ≥10 mm today → one caption line, no speech |
| N9.10 | Frame rate on the ring | MEASURED 2026-09-01 | JARVIS 19, WATCH 14, WEATHER 13, STATUS 12, ACTIVITY 13 fps with the mic idle, after the veil stopped folding black pixels (was 10-11). The shell still costs ~5 fps: the veil, the orbit rail and the focal are recomputed every strip of every frame; caching the veil under a static space is the next win. Health's `display-fault` threshold moved 12→10 so a healthy ring screen no longer reads as a fault |
| N9.11 | Violet means two things | FINDING | The shell-wide update ring in PROBATION (violet, r140-154) and the companion agent rim (violet, r224-230) share a hue. Not a defect, but a first glance after every OTA reads as "a companion is in". Pick a second hue for one of them |
| N10.1 | **STATUS shows the device**: connections, battery, radio | BUILT, host-tested (556 checks, 3 mutations caught); see the glass row below | Closed: LINK/TOOLS lamps, battery arc, power word, big percentage, Wi-Fi bars + dBm, headline = worst thing or `UP 6H 10M`. Open: BATTERY / POWER / WIFI / IP / LINK / TOOLS / DESK / RADIO / UPDATE. Feed: `jr_display_links_set()` at 1 Hz from `publish_shell_state` |
| N10.2 | **The chip's four power modes** | FINDING 2026-09-01 | The ESP32-S3 offers active, modem-sleep, light-sleep and deep-sleep. This firmware uses TWO: active (CPU pinned at 240 MHz, `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`, no `CONFIG_PM_ENABLE`) and Wi-Fi modem-sleep (`jr_net_set_power_save`, driven by the mood ladder: realtime while voice, OTA or a companion is active, `WIFI_PS_MIN_MODEM` otherwise). No `esp_pm`, no light sleep, no deep sleep anywhere in `main/` or `components/`. STATUS's RADIO row now reports the one mode in force |
| N10.3 | **Give DREAM teeth** (GLASS_DESIGN §F4) | BUILT 2026-09-01 (owner: "if not being used it should deep sleep"); `jr_mood_sleep_due` host-tested; wake sources proven from the desk with `POST /api/debug/sleep?now=1&wake_s=N` — see the evidence row below; the lift wake itself still needs a hand | Light sleep is incompatible with always-ready listening (the I2S capture holds an APB lock, so automatic light sleep never engages) and DFS gains are unmeasurable without a current reading (the AXP2101 exposes none). The real lever is deep sleep from DREAM on battery: after N min face-down or still with no USB, `esp_deep_sleep_start()` with wake on the QMI8658 any-motion interrupt (INT1 → GPIO21, RTC-capable; the engine is unconfigured today) and the touch INT; PKEY cannot wake the S3 from deep sleep on this board. Cost: a 3–5 s cold boot and Wi-Fi reconnect on lift, no "Jarvis" wake word while asleep. AC: percent-over-time on battery halves overnight; lift wakes within 5 s; face-up on USB never sleeps |
| N10.4 | Deep sleep evidence | MEASURED 2026-09-01 | Forced sleep at 80 s uptime with a 45 s timer: gone in 10 s, back in 51 s, `wake: timer`, `armed: lift true, touch true`, IMU sampler live after the wake (revision 0x7C). Two findings on the way: (1) a sleep during OTA probation rolls the image back — the route now answers 409 and `enter_deep_sleep` refuses; (2) the QMI8658 never sets STATUSINT.CmdDone on this board, with either CTRL8 handshake type — the command still lands, so the handshake is logged, not fatal, and the engine is cleared at boot by a soft reset. NOT yet proven by hand: the lift wake itself (needs a hand and a device off USB) |
| N10.5 | **Deaf session watchdog** | BUILT 2026-09-01, not yet seen firing | Log evidence: three utterances (4:50–5:28 after boot) with no reply, no death, no reconnect, then answers again at 5:56. Now: an utterance ≥800 ms ending in Listening arms a 7 s clock; any server frame or phase change clears it; two misses inject StaleDeadline (reconnect with resume). Watch for `voice: utterance unanswered` / `session is deaf` in the log; if ambient chatter trips it too often, raise UTT_DEAF_COUNT |
| N10.6 | Heat | MEASURED 43 °C die at rest on the cell | No clock change today or ever: CPU pinned 240 MHz. Warmth = WakeNet + AEC always on, Wi-Fi realtime while armed, AMOLED, and charging. STATUS shows CHIP now; `RUNNING HOT` at 70 °C. The real lever is N10.3 (it sleeps now) and, later, DFS under measurement |
| N10.7 | **Persona scope and the life/work tools** | BUILT 2026-09-01, proven by four spoken turns | Refusals traced to the identity line ("British AI butler … serve Sir"). Now: personal AI for the owner's life and work, never out-of-role, answer when in doubt. Allowlist + `memory.capture`, `coordination`, `butlercrm`, `overwatch`; device denies `delete*/remove*/destroy*/purge*/wipe*/archive*` names; apps and one-object methods are called named-first; the board gets the device's identity. `remember` no longer intercepted for a tap (`REMEMBER_NEEDS_TAP 0`); `recall_memory` projected (5 hits, 200 chars). Proven: Minecraft question answered, lease note saved and recalled with its source, calendar read, board reached. Open: `coordination.portfolio` is 7 KB and gets truncated — project it |
| N10.8 | **Privacy gated the re-arm, not the mic** | FIXED 2026-09-01 | Owner: "it's on privacy mode but look, it's listening to me". Cockpit agreed: `privacy_paused true`, `capturing true`, session open, answering. Cause: the flag only blocked the always-ready re-arm; `/api/debug/say` (and any other road into a session) uplinked the mic under the gold ring. Fix in the capture loop: while paused the frame is zeroed before the VAD and the uplink (dropping it entirely made a text turn wait 47 s for the server's turn detection). Proven: session open under privacy, `mic_rms 0.0`, `vad_starts 0`, text turn answered in 3.1 s. Open: the audio self-test (`/api/debug/audio-diag`) still records under privacy — operator-triggered, but the same rule should apply |
| N10.9 | **First unanswered utterance gets a nudge** | BUILT 2026-09-01 | Log: PWR listen, connect, a clear 3 s question 1.5 s after the greeting, `utterance unanswered (1 in a row)`, then nothing — one miss is under the reconnect threshold and the owner waited at a device that looked fine. Now the first miss sends a text nudge (the model still holds the audio) to answer what it heard or ask for a repeat; the second miss still reconnects. Latency of a search turn, measured: decision 1.2 s, tool 1.8 s (the search backend itself; not a browser), second pass 1.3 s, pre-roll 1.0 s → first word ~4.9 s after the question ends. Levers: pre-roll 1000→600 ms, TLS reuse to JarvisMCP (~0.3 s), a faster search backend |
| N10.10 | Pre-roll 1000 → 600 ms | DONE 2026-09-01, owner's call | Four text turns after the change: first word 2.0–2.2 s for plain replies (5.9 s for a search turn that also opened the session), 0 underruns, max gap 0 ms. Refill after a hole stays 1500 ms. If holes return at reply starts, `/api/debug/gain?preroll=` moves it live |
| N10.11 | **CPU gears + battery saver + adaptive pre-roll** | BUILT and measured 2026-09-01 | `CONFIG_PM_ENABLE` (no DFS init, no tickless), `cpu_gear_set()` max=min: 240 live / 160 rest-on-cell; saver below 20 % divides the ladder by 4 (host-tested); pre-roll walks 600–1500 (stepped 600→900 on its own after a hole). Measured: 160 MHz keeps 16 fps and a clean reply (one 12 ms hole); core 1 96 % idle muted, 72 % with a session; ≈5 KB internal RAM cost, largest block unchanged. Open: the rest gear has not yet engaged on its own (the cell was on USB); watch `cpu gear: 160` in the log the next time it rests unplugged. The renderer is the real daytime load — a rest cadence for it is the next battery win |
| N10.12 | **PWR hold = off completely** | BUILT 2026-09-01 | `jr_power_off()`: AXP2101 0x27 ON-level → 1 s, 0x10 bit0 soft power-off. PWR long (PMIC IRQ, ~1.5 s) → caption, panel off, rails off. `POST /api/debug/sleep?off=1` proved the off half from a desk (rails gone in 5 s with USB present); the on half — a 1 s hold of PWR — was NOT yet observed at push time. If a 2 s hold does nothing, a USB replug powers the PMIC on; then change the ON-level to a plain press (0x27 bits 1:0 = 0) |
| N9.9 | Ideas not yet built | — | Tilt parallax on the procedural layer (N7.22 first); a sunrise/sunset arc on WATCH (needs the fields from the gateway); a soft completion chime; proactive hourly rain warning (needs hourly data) |

## Wave N11 — hands elsewhere (2026-09-02, built, not yet flashed)

Owner's direction: *Jarvis doesn't do work; he has a tool that calls agents
on other machines (a small Docker first, swapped later), and they do tasks.*

| # | Deliverable | Status | Acceptance |
|---|---|---|---|
| N11.1 | `delegate_task(goal)` + `delegated_tasks()` | PROVEN 2026-09-02 on the 1.75C: a text turn "Delegate this task: …" → `createWorkItem` in 6.3 s, Gemini said it was queued; board results are projected inside execute_tool too (a raw list was 7.2 KB) | Spoken "have someone …" → one `createWorkItem` on `jarvisnano-desk` with the device identity, tool returns `{id,title,status}` in < 3 s, Gemini says it is queued. Priority is not an argument (one-string template contract) |
| N11.2 | The announce loop (`board_poll`, 90 s, awake + Wi-Fi, not DREAM) | PROVEN 2026-09-02: item completed from a desk at ~uptime 330 s, `board: done …` logged at the 303 s poll's successor, `DONE: <title>` caption on the muted glass (evidence `docs/evidence/20260902-glass-done.png`); spoken path with an open, unmuted session still unobserved | Complete an item on the board from a desk; within 90 s the device speaks "<title> is done: …" with a session open, or shows `DONE: <title>` muted; ACTIVITY shows a TASK row; a reboot does not re-announce it (first poll seeds the ring) |
| N11.3 | `tools/board-worker/worker.py` | REFERENCE ONLY since the evening of 2026-09-02 (its `completeWorkItem` shape corrected); not required by N11.4 | On one machine with `claude` on PATH: claims the item from N11.1, runs it, `completeWorkItem` with a ≤300-char summary, or `blockWorkItem` with the reason; leases never overlap |
| N11.4 | The worker lives in the gateway (owner, 2026-09-02: "scrap the devbox, use the sandbox agent in JarvisMCP") | PROVEN 2026-09-02 on the 1.75C: "Delegate this task: find out what the CO5300 … supports for brightness control" → `createWorkItem` in 4.0 s, "queued, Sir"; the next poll claimed, researched (cited, 5 sources), filed the answer as a company-brain inbox event and completed it in 26.0 s, and announced `board: done Find technical specs for the CO5300…` in that same poll. 26 s is close to the 30 s cap, so research now runs 2 queries / 5 sources, and the spoken 300 characters are stripped of markdown and `[n]` citations. Repo path and the expired-lease recovery are built, not yet exercised | One spoken job with no repo: completed and announced within two polls, the full cited answer in the company brain under `jarvisnano-desk`; one spoken job naming an allowlisted repo: a branch delivered and announced; an unlisted repo blocks the item with `repo_not_allowed`; a poll killed at 30 s leaves an expired lease that a later poll recovers |
| N11.5 | `project_id` in `/api/tools/config` | BUILT 2026-09-02 | GET shows it, POST with a bad charset answers 422, `""` restores `jarvisnano-desk`, the next delegate uses the new id (log shows the create against it) |

## Execution order

1. **Correctness:** N6.1–N6.4.
2. **The glass, content first:** N7.14–N7.16. The ring is live but two of its
   four screens have nothing worth reading; that is the same failure the side
   pages died of, so it outranks refactoring.
3. **Maintainability:** N7.17 (layer stack) then N6.5–N6.8. Deletion before
   refactor: N7.19/N7.20 remove code that N7.17 would otherwise re-home.
4. **Performance/hardware:** N6.9–N6.12, N7.22.
5. **Before any release:** N7.13 — set `JR_DEV_OPEN_DIAGNOSTICS 0`.
4. **Public release:** N6.13–N6.15 after credentials and attended device access exist.

## Release gates

- One clean 30-minute voice soak with the counters in N6.4.
- Physical PWR, BOOT, both edge controls, glass privacy, flip privacy, and ROM
  downloader recovery witnessed on the target board.
- Host build/test command and release ESP-IDF build both green from a clean
  checkout.
- **`JR_DEV_OPEN_DIAGNOSTICS` is 0** (N7.13). While it is 1 the pairing token is
  not required on `/api/logs`, `/api/cockpit`, the audio taps or
  `/api/debug/input`, so anything on the LAN can read logs, hear the mic and
  inject input. `grep -rn JR_DEV_OPEN_DIAGNOSTICS main/` before tagging.
- `scripts/gesture-doctor.py` reports **healthy** against the release build.
- Relative-link, conflict-marker, secret, and generated-artifact checks clean.
- Signed image verification and authenticated OTA proven with both positive and
  negative cases.
- At-rest credential protection proven against a physical flash/NVS read.
- Exact third-party notice bundle and dependency inventory attached to the
  firmware release.

## Safety invariants

- Synthetic input cannot clear privacy, answer asks, approve consent, or escape
  operator ownership.
- Remote repair never clears physical hold/flip privacy.
- Brightness is a user ceiling; moods may dim below it, never exceed it.
- Low contiguous memory refuses canvas, snapshot, OTA, or self-test before it
  endangers TLS/voice.
- Screenshots are software submission mirrors, not panel readback. PCM taps prove
  codec-write data, not audible output.
- No keys, tokens, private/device-specific endpoints or addresses, SSIDs, NVS
  images, or device logs enter the repository.
