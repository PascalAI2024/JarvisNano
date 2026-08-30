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
- One overlay compositor with baked EAF faces, listening halo, Watch, Desk,
  Tools, Settings, controls, captions, and remote canvas.
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
| N6.2 | P0 | Add playback-gap and ring-low-watermark telemetry | PENDING | Live diagnostics expose underruns, maximum empty gap, low-watermark, and DAC failures; 60 s reply has no unexplained gap >120 ms |
| N6.3 | P0 | Byte-budget JarvisMCP results with cursor projection | TEMPORARY 3-result cap | Every normalized result is valid JSON ≤3071 bytes with `has_more` and cursor; voice search plus one returned read-only tool execute without `bad_response` |
| N6.4 | P0 | Run uninterrupted powered conversation soak | PENDING N6.2/N6.3 | 30 min with TX drops/deaths/flush errors 0, playback gaps ≤120 ms, largest internal block ≥16 KB, and final state Listening |
| N6.5 | P1 | Split `main/main.c` at existing ownership seams | PENDING | Input/buttons, HTTP diagnostics, and voice/power policy become three modules; `main.c` <4,000 lines; all builds/suites pass |
| N6.6 | P1 | Make one canonical host-test command | PARTIAL — `scripts/host-tests.sh` | One command runs core, transport, display, tool-template, desk CLI, and shell suites; CI calls it |
| N6.7 | P1 | Remove repository/document truth drift | IN PROGRESS | Root plan is current-only; historical plans are archived/labeled; all relative links resolve; no obsolete interaction claims remain |
| N6.8 | P1 | Consolidate visual interaction specification | DOCS ALIGNED; final capture set pending | Vision, hardware, and protocol agree; Jarvis, Watch, controls, privacy, and side-space captures match |
| N6.9 | P1 | Raise settled controls cadence | PENDING | Full controls remain present at ≥14 FPS, zero flush errors, largest internal block ≥24 KB |
| N6.10 | P1 | Prewarm face assets without blocking short turns | PENDING | First Think and Speak apply in <150 ms; stale requested phases never flash late |
| N6.11 | P2 | Add rolling frame/network-burst diagnostics | PENDING | Health exposes 1 s frame-gap max/p95 and rate-limited backpressure bursts; healthy Listening p95 ≤75 ms |
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
| N7.19 | P2 | Re-home the OTA ring, then delete the SETTINGS renderer | OPEN | OTA progress stays visible without `nav_set(SETTINGS)`; only then may `sp_focal_settings` go. **Do not delete first** — it would blind a firmware update |
| N7.20 | P2 | Remove the orphaned shell setters | OPEN | `jr_display_desk_set_task` / `tools_set` / `space_set_label` have no production callers but are exercised by `test_shell.c`; remove implementations and their tests in one change |
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
**Items 1-5: DONE.** Evidence below; the rest of the table stands.

| Done | Item | Commit | Panel evidence |
|---|---|---|---|
| 1-2 | SETTINGS + shade clipping | `1f0f9a1` | Now "V100% L100%" (11 of 12) and "R LGT 100%" (10 of 10). The old test asserted `strstr(label,"VOL")`, which stayed green while brightness was cut off; it now stages 100/100 and pins the exact strings — and immediately caught a wrong expectation in its own commit (`sp_pct` appends `%`) |
| 3 | Tool petals used canonical ids (`RECALL_MEMOR`) | `1f0f9a1` | Labels are a separate `char[13]` table, so an over-long label is a COMPILE error; a `_Static_assert` ties it to the catalog and confirmed it is exactly 8 tools |
| 4 | Agent titles: 48 chars into a 13-byte cache | `1f0f9a1` | `title_shorten()` backs to a word boundary and always marks the cut |
| 5 | Agent rim painted over the choice band | `de0d2a5` | **Measured.** Rim alone: 1324 lit, all violet (173,0,255). Rim + ask open: 76 lit, all cyan (0,255,255), **zero violet**. Exactly one tenant |
| N8.19 | Expired agent links never cleared | `eee9bb1c` | **Measured.** With `ttl_s=30` the rim went 1324 -> 0. Before the fix the expiry branch was empty and it stayed lit indefinitely |

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
| 13 | **SETTINGS rebuild** — four saturated arcs (cyan/orange/magenta/green) plus a yellow ring plus overlapping text, in a build whose language is amber and gold elsewhere | A sheet where SETTINGS is visibly the same product as the other five screens |
| 14 | N8.18 TOOLS is three unlabelled arcs now the redundant tools are gone | Cut or repurposed with a stated reason; not left as decoration |
| 15 | N8.17 **Weather screen (Fort Lauderdale)** | Seventh ring screen. Settle the data path first: device-pull via the jarvismcp bridge vs host-push via the brain surface. City in code; endpoint and key in NVS, never the repo. Refresh on entering the screen, never a timer that drags Wi-Fi out of min-modem during rest. **Render the data age** — it will be stale after rest |
| 16 | N8.13 `mood.h` says WHISPER/DREAM switch the microphone off; the code does not | Comment corrected, then a decision recorded on whether behaviour should match it |
| 17 | N8.12 **DREAM becomes a real power mode** | Today WHISPER and DREAM are electrically identical — same radios, I2S, 24 fps compositor, 100 Hz IMU poll — differing only in brightness, so DREAM is a caption. Stop the emote, paint near-black, refresh a few clock pixels once a minute. **Only from genuine idle**: asks, busy and USB keep the glass lit, per `db3bb607`. Measure battery percent drop over a fixed soak, before and after |
| 18 | Companion: mic ring is 2 s, so the leaseholder must poll every ~1.5 s or drop audio | Longer ring or a streaming endpoint; a 30 s utterance survives without gaps |
| 19 | Companion: a reply path back to the glass during a lease | The leaseholder can speak or caption without borrowing the Gemini session |
| 20 | Re-run the full sheet and re-read it | `screens.py --extras` after the batch; every screen shares one visual language, and the sheet is captioned correctly |

**Standing rules for this queue.** Host suites do not cover arc geometry or
layout — they were green throughout the POWER arc bug — so a rendering claim is
evidenced by a panel measurement, never by a green suite. Every fix builds,
runs both suites, and is OTA'd and re-measured before the next item starts.
`JR_DEV_OPEN_DIAGNOSTICS` must return to 0 before any release tag.

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
