# The Glass — an OS/UX design for JarvisNano 1.75C

Design document, read-only pass over `main` @ `80636399`.

**Status 2026-08-29:** the two Stage-1 ordering/direction fixes named in §H are
**shipped** (`20a921c5`): an open ask now outranks double-tap-home, and both
level slabs increase on UP. Everything else here is still a **proposal** —
nothing in §B, §F or §G is implemented.

Every claim about current behaviour carries a `file:line`. Every claim that is a
proposal is marked **PROPOSAL**. Everything I could not verify is listed at the
end.

This document **completes** two documents already in the tree rather than
paralleling them:

- `docs/INTERACTION_MODEL.md` — audited against this tree; already owns the
  interaction axes, the feedback contract, the layer-stack refactor (its §4,
  "Phase A") and the A–G ladder. §D and §H adopt and extend it; where I disagree
  I say so.
- `docs/ARCHIVE/JARVISNANO_OS_PLAN.md` — owns D1 (no LVGL), the measured
  negative-space bands, the internal-RAM table. (The brief cites this as
  `docs/JARVISNANO_OS_PLAN.md`; it is **archived**.)

---

## 0. What the brief gets wrong, and what the code is actually doing

Designing on the brief's premises would produce a wrong design. Six corrections
first — four of them are the design's raw material.

### 0.1 The physical grammar is six inputs, not one button

The brief says "GPIO0 button". The live grammar is **PWR (AXP2101 PKEY) + BOOT
(GPIO0) + left slab + right slab + glass hold + flip**
(`docs/ARCHITECTURE.md:111-137`).

- PWR is read over I²C, not GPIO: `jr_power_pkey_take()`
  (`components/jr_power/include/jr_power/jr_power.h:57`), latched at 2 Hz
  (`jr_power.c:214`). Short → wake/listen, long → speak battery
  (`main/main.c:6044-6066`).
- BOOT is a **hand-rolled polled tick**, not `iot_button` — `boot_button_tick()`
  (`main.c:310-344`), `GPIO_INTR_DISABLE` (`main.c:7287-7299`), called at ~10 Hz
  (`main.c:6067`). `INTERACTION_MODEL.md §1` implies `iot_button`'s multi-
  threshold callback is available to us; **it is not linked** — zero `iot_button`
  hits in the tree. Two thresholds exist by hand: 200–1500 ms → shade,
  1500–5000 ms → 60 s pairing (`main.c:322-339`). **≥5000 ms falls off the end of
  the if-chain with no binding.**

### 0.2 Three of the brief's six friction items are already fixed

| Item | Status | Cite |
|---|---|---|
| #2 shade trap | **fixed** — either vertical exits, and it captions `SHADE - UP TO CLOSE` | `main.c:6721-6736`, `6767-6772`; commit `80636399` |
| #3 31–41 px dead band | **fixed** — `TOUCH_TAP_SLOP_PX` and `TOUCH_SWIPE_MIN_TRAVEL_PX` are both **42**, with a comment forbidding them from diverging | `components/jr_hal/src/input_touch.c:41-53` |
| #4 rim taps read as swipes | **fixed** — an open ask claims `SWIPE` as well as `TAP` and hit-tests the **landing** point | `main.c:6632-6633`, `6652`; commit `232fe985` |

So the live friction is **#1 (the spaces you cannot act from)** and **#5
(discoverability)**. Those are what this design has to solve. The rest must not
be re-solved.

### 0.3 The free outer band is smaller than the brief says, and it is full

The brief cites `r215-239`. The code corrects itself twice
(`components/jr_display/src/hud_render.c:570-587`): **the glass ends at r232.5**,
and the usable band already has three tenants at disjoint radii:

```
r215-220   battery rim + ERROR ring      (OV_R_BATT 218)
r221-222   persistent gold privacy ring  (OV_R_PRIVACY_IN/OUT)
r223-231   choice arcs                   (OV_R_CHOICE_IN/OUT)
```
`hud_render.c:581-590`. **There is no unclaimed outer band.** A fourth element
must share a tenant's radii under mutual exclusion — which is already how
`apply_shell_overlay`'s 8 agent rim segments at **r224-230**
(`jr_display.c:2922`) survive inside the choice-arc band: they are safe only
because `apply_hud_overlay` skips the shell whenever an ask is up
(`jr_display.c:2878-2882`).

### 0.4 "Top-edge down opens the shade" is not true

The HAL has a strict top-edge gate — `start_y < 72` **and** `delta_y >= 70`
**and** 135 % vertical dominance → `DIRECTION_DOWN` with
`JR_INPUT_FLAG_TOP_EDGE` (`input_touch.c:152-163`).

But **`main.c` contains no branch that tests `TOP_EDGE` positively.** The flag is
used only *negatively*, at `main.c:6695`, to keep a top-edge pull out of the
volume/brightness slabs. Consequence: **any** DOWN swipe starting between
x=141 and x=325 opens the shade (`main.c:6750-6751`).

This matters for the brief's "the top edge is a thin crescent (9.8 %)" concern:
the crescent is **not** load-bearing today. The real primary is a centre-down
swipe, which is a large, comfortable target. The design should keep it that way.

### 0.5 Two live defects the design should eliminate structurally

**(a) Double-tap outranks the ask, so a quick retry after a missed arc is
stolen.** The double-tap→home branch (`main.c:6607-6619`, 400 ms window) sits
**above** the ask branch (`main.c:6632`) and above the 600 ms trailing-tap grace
(`main.c:6682-6686`). I read all three.

To be precise about the damage, because it is narrower than it first looks: the
ask is **not** dismissed. `jr_display_nav_home()` only sets the nav word
(`jr_display.c:3780-3784`); the ask lives in separate choices state (`s_choice_n`)
and session state (`JR_ST_ASKING`), and `apply_hud_overlay` draws a live ask
regardless of space (`jr_display.c:2874-2876`). Nor can a user answer twice —
exactly one response per call id is enforced by `ask->answered`
(`main.c:1847-1864`).

The real defect: **a user who misses an arc and immediately retries has the retry
stolen.** The second contact never reaches `main.c:6632`; instead it fires
`nav_home()`, a bloom, and the caption `JARVIS - RIGHT WATCH` — over a live
question. Missing an arc is exactly the situation this session was debugging, and
retrying fast is exactly what a hand does. The 600 ms grace can therefore only
ever act in the **400–600 ms** window.

**(b) Right-slab brightness is inverted relative to left-slab volume.** Left slab:
UP → +5 volume (`main.c:6696-6704`). Right slab: **DOWN → +5 brightness**
(`main.c:6705-6713`). Two adjacent vertical gestures on the same glass move
opposite ways. This is a predictability defect, not a preference.

### 0.6 Every touch is already acknowledged — identically, whatever happens

`main.c:6459` ripples on **every** tap and `main.c:6474` ripples on **every**
classified swipe, both before any dispatch. The comment at `6469-6472` is
explicit: "A classified swipe must acknowledge immediately, even when its
semantic result … would otherwise look unchanged."

So the W3 problem is **not silence** — it is that the acknowledgement **does not
discriminate**. A swipe that did nothing draws the same cyan ripple as a swipe
that did something. That reframing changes §D materially, and it is why "no
gesture ends in silent nothing" was closed without the owner feeling any
different.

### 0.7 Verified myself: the device never sleeps, and never throttles

`grep -rn esp_sleep main components` → nothing. No `esp_pm`, no
`CONFIG_PM_*` in `sdkconfig.defaults`; the only frequency line is
`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` (`sdkconfig.defaults:10`).
`components/jr_core/include/jr_core/mood.h:12` states the intent: *"Deep-sleep
and rail gating stay out."*

But a **rest ladder already exists** and is more advanced than the brief implies:
`JR_MOOD_AWAKE / AMBIENT (20 s) / WHISPER (5 min, mic OFF) / DREAM (15 min)`
(`mood.h:32-41`), driving a ±4/tick brightness slew (`main.c:6000-6007`) and
captions `AMBIENT` / `RESTING - TAP TO WAKE` / `ASLEEP - TAP TO WAKE`
(`main.c:6008-6020`). Wi-Fi modem sleep is the one real lever wired
(`jr_net_set_power_save()`, `main.c:5994`).

That changes F3 and F4 below from "build a rest state" to "the rest state exists
and shows the wrong thing, and its deepest rung has no teeth."

---

## A. The organising idea

> **The glass is a hand, not a phone.**
>
> It exists for the three things a voice is bad at — **picking one of several,
> saying yes to something you cannot take back, and being read without being
> asked.** It must never become a place you travel to.

The corollary is the whole design: **a screen you navigate is a screen that has
failed.** If the only way to reach something is to remember a gesture and walk
there, it should have been spoken. The device has a mouth, and the mouth is the
general-purpose interface. The glass is the special-purpose one, and its
speciality is *the moment the conversation needs a body.*

**On Xiaozhi, directly.** Xiaozhi is right about navigation and wrong about the
moment. Its one button and two gestures are correct because nothing it does
needs a screen — and our four-space shell is a worse version of the thing it
correctly declined to build. We should be **poorer than we are today in
navigation** (Xiaozhi has zero destinations; so should we) and **richer than
Xiaozhi in exactly one dimension**: the handful of moments where speech cannot
carry the interaction — *choose, confirm, dial, glance, gauge.* That is a
five-item list, not an OS. Everything outside it is decoration or a filing
cabinet, and this design deletes both.

---

## B. The surface model

### The rule that earns a surface

A surface exists only if it passes **both**:

1. **Summon test.** Something in the world brings it up — a question from Gemini,
   a risky action, stillness, a level moving under a thumb. If the only way to
   reach it is a remembered gesture, it fails.
2. **Voice test.** Could this have been said, or been told to me? If yes, it is
   not a surface. It is a caption, or it is Jarvis's job.

### Today's surfaces against the rule

| Surface | Summon | Voice | Verdict | Cite |
|---|---|---|---|---|
| **Face** (5 baked `rwave_*.eaf`, 24 fps) | ✅ it *is* the ground state | ✅ liveness is not sayable | **KEEP** | `jr_display.c:213-231` |
| **Caption chip** (2 × 19 chars, scale 2, rows 360-430) | ✅ follows the conversation | ✅ | **KEEP** | `jr_display.c:1109`, `1951-1952`, `2005` |
| **Ask arcs** (≤3, r223-231, question 2 × 24 chars) | ✅ Gemini summons it | ✅ choosing among 3 by voice is genuinely bad | **KEEP — the flagship** | `gemini_live.h:121-146`; `hud_render.c:1203`; `jr_display.c:1065` |
| **Privacy ring** (gold r221-222) | ✅ state, not a place | ✅ must be visible unasked | **KEEP** | `hud_render.c:802-810` |
| **Battery rim** (r218) | ✅ ambient | ✅ | **KEEP** | `hud_render.c:752-774` |
| **Listen halo** (32-point sparse, r185-194) | ✅ state | ✅ | **KEEP** | `hud_render.c:817-836` |
| **Watch peek** (10 s, swipe-right, JARVIS only) | ❌ you swipe for it | ✅ time is *the* canonical glance | **KEEP the content, DELETE the summon** | `main.c:6739-6744`; `hud_render.c:1464` |
| **Control shade** (vol ±, privacy button) | ❌ a place you enter | ⚠️ continuous values do beat speech | **DEMOTE to a transient dial** | `jr_display.c:2666-2748`, `3857-3872` |
| **DESK** — `STATE / DONE% / JOB` | ❌ navigated to | ❌ telemetry | **DELETE** | `jr_display.c:1531-1544` |
| **TOOLS** — `READY n / LAST x` + 4 petals | ❌ navigated to | ❌ | **DELETE** | `jr_display.c:1545-1567` |
| **SETTINGS** — `PRIVACY / LINK / POWER / UPDATE / SLOT` | ❌ navigated to | ❌ a diagnostics page wearing a face | **DELETE** | `jr_display.c:1568-1631` |
| **DETAIL sheet** (≤6 rows × 10 chars) | ❌ swipe-up inside a place | ❌ | **DELETE** | `jr_display.c:1279-1280`, `2615` |

The owner's verdict is confirmed by the hit test, not just by taste. **JARVIS
claims no taps at all** (`jr_display.c:3883-3885`), and DESK / TOOLS / SETTINGS
each accept exactly **one** target — the focal object, `r ≤ SP_FOCAL_HIT 116`
(`jr_display.c:2069`, `3887-3889`) — whose action is `ACT_FOCUS`, handled at
`main.c:6849-6853` as **`nav_up()` plus a caption**. That is navigation. So:
across three of the four spaces there is literally **not one action that is not
navigation.** "You cannot ACT from them" is structurally true.

### The critical call: delete the destinations, keep the renderer

This is the sharp decision in the document, and "delete the four spaces" and
"delete the spatial shell" are very different instructions. Only one is right.

The shell is **good code**. Strip-local, allocates nothing per frame, holds no
unbounded string, clips every primitive to one hard radius so it *structurally
cannot* collide with the privacy ring, and at rest in JARVIS costs "one boolean
test per strip" (`jr_display.h:333-346`). And its primitives are exactly what the
powerful surfaces in §F need:

| Built for | Re-homed to |
|---|---|
| DESK's segmented progress ring + big percentage (`sp_focal_desk`, `jr_display.c:2357`) | **the gauge** (F5) |
| SETTINGS' four cardinal gauges (`sp_focal_settings`, `jr_display.c:2458`) | **the dial readout** (F2) |
| `jr_display_hit()` → `ACT_*` (`jr_display.c:3841-3889`) | the hit path every summoned surface needs |
| `sp_annulus_row` / `sp_dot_row` / `sp_text_row` / `sp_veil` (`jr_display.c:2217-2325`) | unchanged, reused |
| `JR_DISPLAY_SAFE_R 168` clip discipline (`jr_display.h:349`) | unchanged, applied to new surfaces |

**Delete the four destinations, the nav axis and the telemetry composers. Keep
the drawing primitives and re-point them at summoned surfaces.** That turns a
filing cabinet into a toolkit without discarding the work.

### PROPOSAL — the surface set

Six. Five already have their pixels drawn today.

| # | Surface | Summoned by | Lives at | Exits by |
|---|---|---|---|---|
| 1 | **Face** | always | full glass, baked EAF | — |
| 2 | **Caption** | anything worth saying | rows 360-430, 2 × 19 | fades itself |
| 3 | **Rest** | the mood ladder, from **AMBIENT** (20 s) down | clock r ≤ 192 + battery rim + privacy ring | any touch, any voice |
| 4 | **Ask** | Gemini `ask_user` | arcs r223-231 + question r ≤ 168 | tap an arc, or the 120 s timeout |
| 5 | **Dial** | thumb on the rim, or any level changing | rim arc + value in the caption band | lift the thumb |
| 6 | **Commit** | a risky or irreversible action | filling ring r223-231 | complete it, or let go |

Nothing else. No spaces, no sheets, no shade-as-a-place, no cards, no radial
menu. **Five of six auto-dismiss.** The only surface you can be *in* is Ask, and
Ask has an owner and a 120 s timeout (`JR_ASK_TIMEOUT_MS`,
`jr_core/session.h:335-370`). The shade trap becomes structurally impossible
rather than patched.

Note surfaces 4, 5 and 6 all want **r223-231** and are mutually exclusive by
construction — the same discipline that already keeps the agent rim segments and
the choice arcs apart (`jr_display.c:2878-2882`). That is a feature: it means the
outer band never has to grow.

### PROPOSAL — explicitly rejected

`docs/ROADMAP.md` "Next" still queues *"compositor-native cards and radial
controls"*. Strike it.

- **Notification/card stack** — fails the summon test and duplicates the
  caption. A device that talks should tell you.
- **Radial menu** — it is a navigator, and it needs a stationary press, which the
  rim cannot give (`232fe985`).

---

## C. The gesture map

### The principle — three axes, and place is the missing one

`INTERACTION_MODEL.md §2` states two axes, and they are right:

> Direction encodes category. Duration encodes commitment.

They are insufficient, because they say nothing about **where** on a round glass
you touched — and on this device that is the most physically forced distinction
available. The third axis:

> **PROPOSAL — Place encodes scope. Centre is the conversation; the rim is the
> device's own knobs.**

This is not arbitrary; it falls out of the hardware and out of a defect:

- The rim **rolls**. Measured this session: 44 swipes to 12 intended taps while
  answering a 3-option ask (`main.c:6638-6651`). A surface where contacts
  inevitably slide should carry **sliding** meanings, not pressing ones.
- **The current "edges" are not edges.** Volume is `start_x <= 140` and
  brightness is `start_x >= 326` (`main.c:6696`, `6705`) — vertical **slabs**,
  not an annulus. On a round glass, a point at x=140, y=233 is r ≈ 93 from
  centre: *inside the baked face's core band* (core r0-94, `hud_render.c:564`).
  So today a vertical swipe across the reactor core changes the volume. The
  machine's knobs bleed into the conversation's space, and there is no way for a
  user to see the boundary.

So the rule a user can carry:

| Axis | Rule | Why it is guessable |
|---|---|---|
| **WHERE** | centre = the conversation · rim = the device's knobs | the rim *is* the bezel; knobs live on bezels |
| **WHICH WAY** | vertical = the device itself · horizontal = time/content | already true in the tree (`jr_display.h:294-302`) |
| **HOW LONG** | tap = do it · hold = commit, with a ring you can abandon | commitment costs time; universal |

Round-glass corollaries the map obeys:

- **No corners, ever.** The only geometrically distinct edge is the rim *as a
  ring* — a 1-D continuum. Use it as one continuum, not as slabs.
- **The top crescent stays non-load-bearing.** `start_y < 72` is 9.8 % of the
  glass. It is *already* not required to open the shade (§0.4), and it must never
  become required.
- **One contact.** No pinch, no two-finger. Anything wanting a second finger is
  out of scope by hardware.
- **Never require a stationary rim press.** Hit-test the landing point —
  `main.c:6652` already does, with an asymmetric hit annulus r215-255
  (`hud_render.c:908-909`).

### The map — PROPOSAL, beside the current binding

| Input | Today (cite) | Proposed | Why |
|---|---|---|---|
| Voice | Gemini Live + WakeNet9 "Jarvis" (`main.c:7094-7109`) | unchanged | the product |
| PWR short | wake/listen, never mutes (`main.c:6044-6057`) | unchanged | correct |
| PWR long | speaks battery (`main.c:6058-6066`) | unchanged | telemetry belongs in the mouth — the model this design generalises |
| BOOT short (0.2–1.5 s) | shade toggle, and **clears the caption** (`main.c:322-331`) | **summon the Dial**; never clear the caption | same intent, no room to be trapped in |
| BOOT hold (1.5–5 s) | 60 s pairing (`main.c:332-339`) | unchanged | security; explicit and rare |
| BOOT hold ≥5 s | **no binding** | leave unbound | a hold with no ceiling is a hold with no feedback |
| Tap, centre | `jr_display_hit()` (`main.c:6812`) | unchanged | "tap what you see" |
| Tap/swipe, rim, ask up | answers the arc under the landing point (`main.c:6632-6676`) | unchanged — but nothing may sit above it; double-tap moves below | `232fe985`; §0.5(a) |
| Tap, rim, no ask | falls through to attention caption | **summon the Dial**, showing what it will change, changing nothing | the rim announces itself before it acts |
| Slide along rim | left slab = volume ±5 UP; right slab = brightness ±5 **DOWN** (`main.c:6696-6713`) | **one continuous rim dial**, one direction convention, live readout | fixes §0.5(b) and the slab geometry |
| Hold, centre (850 ms) | privacy toggle (`main.c:6781-6808`) | **commit** — privacy moves to flip + the Dial's mute control | see F1 and the note below |
| Double tap (400 ms) | `nav_home` + bloom (`main.c:6607-6619`) | keep as **the global escape**; **move it below the ask** | fixes §0.5(a) |
| Swipe ↕ centre | up = DETAIL, down = SHADE (`main.c:6748-6751`) | **nothing** — both destinations deleted | §G |
| Swipe ↔ | space next/prev; right on JARVIS = 10 s watch peek (`main.c:6737-6747`) | **nothing** — spaces deleted; the watch becomes Rest | §G, F3 |
| Flip face-down (600 ms) | privacy (`main.c:5903-5938`) | unchanged | the best gesture on the device: physical, unambiguous, self-explaining |
| Shake (2 polls, 1.5 s cooldown) | cancel an active turn (`main.c:5866-5901`) | unchanged | |
| Lift | — | **wake** (needs QMI8658 INT2) | F4 |

**Two ordering fixes fall straight out of the map**, and both are cheap:

- Move the double-tap branch **below** the ask branch, so an open question owns
  the glass. Today it does not (`main.c:6607` precedes `main.c:6632`).
- Give the two vertical rim gestures one direction convention. Up increases.

**On moving privacy off the centre-hold.** This is the one binding in current use
that I am reassigning, so it needs a defence. Wave 3 W1 records glass-hold-mute
as *undiscovered* — the owner's long-press counter read **0**
(`docs/ARCHIVE/PLAN-2026-08-27-waves-1-5.md:40`). A hidden gesture guarding a
security-critical state is the worst possible combination. Under this design
privacy keeps **two discoverable paths**: flip the device (physical, obvious,
shipped) and the mute control on the Dial (visible the instant you touch the
rim). That frees the 850 ms hold for commitment, which has no substitute.
**If the owner prefers, swap them** — commit on the rim, privacy on the
centre-hold. The design survives either assignment. What it cannot survive is
both meanings on one gesture.

### The five things a user must know

1. **Talk to it.**
2. **Tap what you see.**
3. **Hold to commit — let go to cancel.**
4. **Slide the rim to change a level.**
5. **Turn it face-down to go quiet.**

Every other binding is a **shortcut**. §E turns that word into an enforceable
rule.

---

## D. The feedback contract

`INTERACTION_MODEL.md §3` states the contract and it is right. Three extensions,
one of them a correction to how the problem has been framed.

### What exists — verified

| Event | Visual | Audio | Cite |
|---|---|---|---|
| **Any** tap | expanding cyan ripple, r4→r56, 400 ms | — | `main.c:6459`; `hud_render.c:1330` |
| **Any** classified swipe | the same ripple | — | `main.c:6474` |
| Arc miss during an ask | contracting dim ring, half level | falling 700→300 Hz, 100 ms | `main.c:6670-6671` |
| Wake word | bloom r10→r150, 600 ms + caption `YES?` | rising 400→2000 Hz, 160 ms | `main.c:7094-7109`; `hud_render.c:1408` |
| Codex release | bloom | rising 160 ms | `main.c:3743` |
| Audio self-test | — | rising 700 ms | `main.c:6266` |
| Any state | caption ≤ 2 × 19 chars | — | `jr_display.c:1095-1117` |
| Mood | brightness slew ±4/tick, target published, applied **only** from the render task | — | `main.c:6000-6007`; `docs/reference/display-emote-gfx.md` |

**The whole cue inventory is 4 call sites and 2 distinct sounds.**
`jr_audio_play_sweep(start_hz, end_hz, ms, level)`
(`components/jr_audio/include/jr_audio/jr_audio.h:153-154`) clamps to 100–1500 ms,
level 1–30, 80–8000 Hz, and **returns `ESP_ERR_INVALID_STATE` during playback** —
so a cue structurally cannot talk over a reply. That decision is correct and this
design does not touch it. **There are no haptics**, so the visual channel carries
every acknowledgement that lands during speech and must be legible alone.

### The reframing: the problem is not silence, it is uniformity

W3 was written as *"a near-miss shows nothing, reading as 'swipes don't work'"*.
That is no longer the failure. `main.c:6469-6472` deliberately ripples **every**
classified swipe *"even when its semantic result … would otherwise look
unchanged."*

So a swipe that did nothing and a swipe that did something produce the **same
cyan ripple**. The acknowledgement is universal and **undiscriminating** — which
is why closing "no gesture ends in silent nothing" changed how the device felt
not at all. Fixing this is §D.3, and it is also the engine of §E.

### PROPOSAL — three additions

**1. A fourth cue class: ADJUST.** A level moving under a thumb must **not**
ripple. At ~16 fps with a value changing every frame, a ripple per step is noise
and hides the number you are reading. The value's own movement is the feedback;
add a short tick only at the ends of travel, so "am I at the end" is answerable —
which is exactly why the dial is clamped rather than wrapped
(`jr_display.h:289-292`).

**2. ABANDON is not REFUSE.** When hold-to-commit lands (F1), releasing early
must **drain the ring silently**. Refuse means *the device said no*; abandon
means *you changed your mind*. Punishing the second with a falling note teaches
people not to explore, which is precisely the behaviour §E is trying to create.
The distinction is free: refuse = contract + fall; abandon = drain + silence.

**3. The unbound gesture answers with the legend.** The rule:

> **PROPOSAL — A recognised gesture with no binding *here* draws a distinct
> neutral ack — not the accept ripple — and captions what this surface does
> accept.**

e.g. `RIM LEVEL · HOLD OK` (18 chars, fits the 19-char line). No tone (nothing
was refused). Cost: one caption plus one existing overlay; no new surface, no new
state. It fires at the exact moment of curiosity, which is the only moment
teaching works.

### PROPOSAL — the invariant

> **Any surface that captures input captions its own exit, for as long as it is
> up.**

Commit `80636399` generalised from a fix into a rule. Two places already violate
it and should be brought in line: opening the shade via a DISMISS tap sets **no
caption at all** (`main.c:6841-6859`), and **BOOT-short explicitly calls
`jr_display_caption_clear()`** when it opens the shade (`main.c:327`) — the exact
behaviour the shade fix removed from the swipe path, still live on the button
path.

---

## E. Discoverability

W4: *"every gesture tonight was learned via chat"*
(`waves-1-5.md:43`). Its proposed fix is *"Shade gains a one-card gesture guide;
consider first-boot card."*

**Drop that fix.** A reference card is something you must already know how to
find, on the surface you visit least, in a **5×7 uppercase-only font with 51
glyphs** (`jr_display.c:638-714`), in a **10-character column**
(`SP_COL_MAX 11`, `jr_display.c:1279`), on a watch-sized glass. To read the
gesture guide you must first know the gesture that opens it. A first-boot card is
read once, on the day you understand least.

The real fix is structural. Four mechanisms, descending power.

### 1. The coverage invariant — the actual answer

> **PROPOSAL — No capability is reachable only by a memorised gesture. Every
> gesture is a *shortcut* for something also reachable by (a) asking, or (b) the
> five things in §C.**

This is nearly free, because the mouth and the tool bus already exist.
`set_volume` and `set_brightness` are **registered Gemini tools executed locally
on the device** (`main.c:175-198`, `handle_local_level_tool()`,
`main.c:1323-1380`) — so "turn it down" already works without a gesture. PWR-long
already speaks the battery (`main.c:6058-6066`). The pattern is in the tree; the
invariant just makes it universal.

Then undiscoverable **accelerators** are fine — nobody is harmed by not knowing a
keyboard shortcut. Undiscoverable **gates** are the bug, and this makes them
impossible by construction. It also makes the five-item list in §C honest: it is
genuinely sufficient, not a starter set.

**Enforcement, so it survives contact with the codebase:** one host-test table
listing every binding and its non-gesture equivalent; a binding with no
equivalent fails the build. A principle without a gate is a comment.

### 2. The unbound gesture teaches (§D.3)

Teaching at the moment of curiosity, in the caption, with no navigation.
Replaces both W3 and the card half of W4 with one mechanism.

### 3. Every capturing surface names its exit (§D invariant)

Already proven on this device: `80636399`.

### 4. Jarvis says it, once, in context

The device talks, and the tree already uses that as a channel —
`jarvis-brief` has Jarvis speak first (commit `d11a6e4e`), and the wake path
already captions `YES?` (`main.c:7108`). So:

- First ask ever: *"Tap one."* Once, then never.
- First commit ring: *"Hold until the ring closes."*
- First wake from rest: *"I'm awake."*

Spoken, contextual, one-shot, zero glass real estate. This is what the card was
reaching for and it is better on every axis.

**Persistence:** one NVS bit per lesson, three lessons. The `"app"` namespace
already carries small scalars written directly from `main.c` — `out_vol`,
`bright_cap`, `ota_attempt` (`main.c:370-436`) — so this is an established
pattern, not new plumbing.

### What I am not proposing

No tutorial mode, no gesture card, no onboarding flow. All three are places you
travel to, which §A rules out.

---

## F. Five capabilities the glass should gain

Each is tested in writing against four gates, because the constraints kill most
ideas and it should be visible which and why:

- **(i) Band** — fits a free or shared radius? (`hud_render.c:581-590`)
- **(ii) Strip** — drawable in 12-row DMA strips with a ragged 10-row tail and
  **no framebuffer**, obeying the frame-start latch so no ease bands across a
  seam? (`jr_display.c:58`, `3065-3069`; `tests/test_hud_render.c:11-12,64-72`)
- **(iii) RAM** — no internal DMA buffers. Largest contiguous internal block is
  the binding constraint; TLS died between 7,680 and 10,752 B
  (`ARCHIVE/JARVISNANO_OS_PLAN.md`, RAM table). Also: the render task's own stack
  is 5,120 B with 1,784 B measured peak (`jr_display.c:42-47`) — a new renderer
  must not be deep.
- **(iv) Voice** — would talking to it be better?

A fifth, implicit gate: **FPS.** The vocabulary here is FPS deltas, not
microseconds. The settled panel runs ~16 fps (`jr_display.c:865`), and a solid
listen annulus once cost **16 → 12 fps** and had to be replaced by a 32-point
sparse halo (`hud_render.c:818-822`). Every proposal below is an **arc or a
sparse dot set**, never a filled area, for exactly that reason.

---

### F1. Hold-to-commit — physical consent

**What.** A ring in r223-231 fills while the finger stays down (~850 ms, reusing
`TOUCH_LONG_PRESS_MS`, `input_touch.c:48`, which already fires **mid-hold** at
`input_touch.c:341-355` — so the fill can be driven from a real event, not
guessed). Complete → the action fires with a rising note. Release early → drains
silently. The caption names the action throughout.

**Why the glass, not the mouth.** Here voice is not merely worse, it is
**disqualified**: "yes" is ambiguous, spoofable by a television, and cannot
distinguish the owner from a room. The project already knows this — PLAN.md's
first safety invariant is *"Synthetic input cannot clear privacy, answer asks,
approve consent, or escape operator ownership"*, and the `physical` flag
(`main.c:6448-6449`) enforces it on the ask, the touch challenge, Codex mode and
the privacy hold (`main.c:6493`, `6557`, `6634`, `6782`).

**And the precedent already exists as a one-off.** The `remember` tool is
intercepted for on-device approval: a consent panel, a 15 s timeout
(`TOOL_CONSENT_TIMEOUT_MS`, `main.c:207`), `physical_confirmed` set only after a
real tap (`main.c:1075`), and a check that rejects a synthetic or stale tap
(`main.c:3631-3652`). **F1 is that mechanism generalised from one tool into a
primitive** — which is why it is the cheapest item on this list relative to its
value.

**What it gates.** OTA accept. Operator-lease handover. Any tool the policy marks
consequential. Clearing privacy. Pairing.

| Gate | Verdict |
|---|---|
| (i) Band | shares r223-231; mutually exclusive with an ask by construction | ✅ |
| (ii) Strip | an arc sweep — same family as `choice_arc` (`hud_render.c:955`), bbox-culled | ✅ |
| (iii) RAM | one angle, one deadline; nothing allocated | ✅ |
| (iv) Voice | voice is disqualified, not merely worse | ✅ decisive |
| **(v) Input plumbing** | **the one real cost — see below** | ⚠️ |

**The honest cost: the HAL cannot currently drive a filling ring.** `main.c` sees
only terminal classified events — tap and swipe on release, and long-press fired
**once** mid-hold at 850 ms (`input_touch.c:341-355`), after which `action_sent`
suppresses the release report. There is **no contact-down event and no hold
progress**. So a ring that fills from 0 to 850 ms has nothing to drive it. F1
needs one of:

- **(a)** a new HAL event at `gesture_confirmed` (~2 samples in,
  `input_touch.c:335-339`) plus a release report, so the ring can be driven from
  real contact state; or
- **(b)** a two-phase design where the existing 850 ms long-press *opens* the
  ring and a further sustained hold completes it — which still needs release
  reported to distinguish abandon from commit.

(a) is cleaner and is the only new plumbing this whole design asks for. It is
small, but it is real, and it belongs in the estimate.

**Narrowing `INTERACTION_MODEL.md`'s Phase C.** That phase proposes *"preview →
commit → system"*, i.e. three thresholds on one hold. I recommend **one**. A
three-stage hold is a hidden menu with a timer — the memorised-gesture failure
mode §E exists to prevent. Hold means one thing: *yes, I mean it.*

---

### F2. The rim as one dial, with the value under the thumb

**What.** Touch the rim anywhere: the arc fills to the current value and the
caption reads the target and the number (`VOL 40`) the instant the finger lands —
**before** anything moves. Slide to change. Lift to dismiss. Clamped, which is
already the decided behaviour and for a stated reason (`jr_display.h:289-292`).

**What it replaces, and why that is not a preference.** Today's controls have
four separate defects, all cited above: the zones are **vertical slabs that reach
r ≈ 93, over the reactor core** (§C); the two directions are **inverted relative
to each other** (§0.5b); there is **no readout under the finger**; and this exact
mechanism is what silently nudged volume while the owner tried six times to
escape the shade (`80636399` commit body). The failure was never the binding — it
was a control that acted without ever saying what it was.

**Why the glass, not the mouth.** Direct manipulation of a continuous value is
the textbook case: "a bit louder" is a round trip, a guess, and an interruption
of the thing being adjusted. A dial closes the loop at panel rate.

**The modal risk, stated plainly.** *Which* value the dial owns should follow the
moment (reply playing → volume; at rest → brightness). That is modality, and
modality is how a device becomes unpredictable. **The mitigation is
non-negotiable: caption the target on landing, before any movement is accepted.**
You always see what you are about to change. **Fallback if it still feels
unpredictable in the hand:** keep a fixed assignment and ship only the readout
and the direction fix. That is 80 % of the win with none of the risk.

| Gate | Verdict |
|---|---|
| (i) Band | shares r223-231, exclusive with ask and commit | ✅ |
| (ii) Strip | arc sweep + the existing caption path | ✅ |
| (iii) RAM | one value, one target enum | ✅ |
| (iv) Voice | continuous adjustment is voice's worst case | ✅ |

Also deletes the shade-as-a-place, which is where the trap lived.

---

### F3. Rest shows the watch

**What.** From the **AMBIENT** rung down (20 s of stillness, `mood.h:32`), the
glass shows the **clock**, not a caption.

**AMBIENT, not WHISPER — the rung matters.** Since G4 deletes the swipe-right
watch peek, choosing WHISPER (5 min) would leave a five-minute window in which
the time is unreachable by any means except asking, which would undercut this
item's own argument. AMBIENT at 20 s means: stop talking to it for twenty
seconds and it becomes a watch.

**Why this is nearly free.** Both halves already exist and are simply not
connected:

- `hud_overlay_clock` draws hour/minute/second hands with a hub, r0-192,
  strip-invariant and pinned by a host test (`hud_render.c:1464`;
  `tests/test_hud_render.c:1584`), wrapped by `apply_clock_overlay`
  (`jr_display.c:2019`).
- The mood ladder already fires at 20 s / 5 min / 15 min, already slews
  brightness, and already captions `RESTING - TAP TO WAKE` / `ASLEEP - TAP TO
  WAKE` (`mood.h:32-41`; `main.c:6000-6020`).

Today the clock is reachable **only** by a swipe-right watch peek that lasts
10 seconds and works only on JARVIS with no overlay (`main.c:6739-6744`). That is
backwards. Time is the single most-glanced quantity in the history of
wrist-sized objects; it should be what the device shows when nobody is talking to
it, not something you perform a gesture to borrow for ten seconds.

**Why the glass, not the mouth.** Nobody wants to *ask* what time it is. This is
the purest glance case there is, and it is the whole argument for putting a round
screen on a voice assistant.

| Gate | Verdict |
|---|---|
| (i) Band | r ≤ 192, inside `JR_DISPLAY_SAFE_R 168` for text; rim tenants untouched | ✅ |
| (ii) Strip | already ships and already passes its own strip test | ✅ |
| (iii) RAM | **zero new** — a scheduling change, not a rendering one | ✅ |
| (iv) Voice | decisively better | ✅ |

Cheapest item here, among the most felt, and the visible half of F4.

---

### F4. Sleep — give DREAM teeth

**What.** Light sleep at the deepest mood rung; wake on the QMI8658 any-motion
interrupt.

**Verified state.** No `esp_sleep`, no `esp_pm`, CPU pinned at 240 MHz (§0.7).
`mood.h:12` says the quiet part out loud: *"Deep-sleep and rail gating stay
out."* WHISPER already turns the **mic** off at 5 minutes, so the product
already has a power posture — it just has no power *mechanism* under it. And the
IMU is **polled at 100 Hz** by a dedicated task (`jr_imu.c:52,231`), with the
QMI8658's own engines unconfigured: init writes exactly three registers
(`jr_imu.c:149-160`), no INT1/INT2 enable, no ISR. The header names this as
future work (`jr_imu.h:29-31`).

**Why it belongs in a UX document.** Two reasons; the second is the one that
matters here.

1. A companion that is flat when you reach for it has failed at being a
   companion, whatever is on the glass.
2. **Lift-to-wake is the gesture nobody has to be taught.** It is the ideal §E
   gesture: the affordance is picking the thing up. With F3, the whole idle
   behaviour becomes self-explaining — set it down, it quiets and shows the time;
   pick it up, it is ready.

| Gate | Verdict |
|---|---|
| (iii) RAM | **improves** it: an interrupt-driven IMU retires the 100 Hz sampler task | ✅ |
| (iv) Voice | voice cannot wake a sleeping radio; this is what physical wake is *for* | ✅ |

**Gated, and correctly so.** PLAN.md N6.12 is *"Move stillness detection to
proven QMI8658 INT2 — PENDING HARDWARE PROBE — scope-prove pin first; preserve
flip/shake/lift"*, and `docs/ROADMAP.md` says *"measure real current before
enabling dynamic frequency scaling or light sleep."* Both hold. This is last in
§H for that reason, not because it matters least.

---

### F5. The gauge — let an answer have a shape

**What.** One primitive plus one tool declaration:
`show_gauge(label, value, max)`. A filled arc plus the label in the caption. That
is the whole feature.

**For.** A timer counting down. A long tool call's progress. A proportion. A
distance. Anything that is *one number with a magnitude*.

**Why the glass, not the mouth — stated precisely, because it is easy to
overreach here.** **Speech is serial and gone.** "The build is forty percent
done" is true for one second and then you must ask again. Drawn, it is ambient:
you glance, you know, you never interrupt. A timer is the most-used
voice-assistant capability in the world and the one that most wants a face,
because the entire value is *checking without asking*.

**And the device cannot do timers at all today.** There is not one
`esp_timer_create` or `xTimerCreate` in `main/` or `components/` — all timing is
deadline arithmetic in the app loop — and the RTC explicitly has *"No IRQ or
alarm support"* (`components/jr_rtc/include/jr_rtc/jr_rtc.h:9`). There is no
timer, alarm or reminder tool in the 12-entry catalog (`main.c:99-203`). So F5
plus a deadline in the existing 10 Hz app-loop block is the cheapest possible
route to the single most-requested assistant feature, and the gauge is what makes
it worth having on a screen.

**Scope discipline — one number, never a chart.** No axes, no series, no history.
If it needs a legend it is a document, and documents belong on `/api/`. That line
is what keeps F5 from re-growing the filing cabinet §G deletes. It is also forced
by the font: 51 glyphs, uppercase only, two sizes, max two lines
(`jr_display.c:638-729`; `hud_wrap2`, `hud_render.c:1059`).

| Gate | Verdict |
|---|---|
| (i) Band | `sp_focal_desk`'s geometry re-homed (`jr_display.c:2357`), or the free r185-194 breathing band | ✅ |
| (ii) Strip | arc sweep — identical to what DESK renders today | ✅ |
| (iii) RAM | two ints and a fixed-capacity label, per the shell's existing string discipline | ✅ |
| (iv) Voice | speech is serial and transient; a magnitude is neither | ✅ |

**Cost.** One entry in a bounded, policy-gated catalog
(`main.c:99-203`; `ARCHITECTURE.md:156-178`) — local, non-destructive,
display-only, the cheapest class of tool there is.

---

### Rejected, with reasons

| Idea | Why not |
|---|---|
| A 4×4 mood/state asset matrix | does not fit; settled in `ARCHIVE/JARVISNANO_OS_PLAN.md` |
| Charts / multi-series | needs a legend, needs lowercase, needs a framebuffer. Fails (ii) and §A |
| Notification stack, radial menu | §B — places you travel to |
| More than 3 choices | `JR_GEMINI_ASK_USER_MAX_CHOICES 3` (`gemini_live.h:125`) with options ≤16 chars is the right cap for this rim; growing it shrinks each arc toward the roll radius that caused `232fe985` |
| A listen-countdown rim | **there is nothing to count.** `s_listen_idle_deadline_ms` is assigned **only ever `0`** — 4 sites (`main.c:6195, 6381, 6384, 7002`) — and read by two diag handlers (`main.c:2105, 4444`), so `auto_idle_ms` is permanently zero. Verified myself |
| DOA / who-spoke | answers a question nobody asks aloud; ROADMAP correctly gates it on >90 % enclosure classification |
| Enabling `googleSearch` | out of scope here, but flagged: the emitter is conditional on `cfg->google_search` (`gemini_live.c:194-198`) and that field is **never assigned** (`main.c:7433-7449`), so it is silently off |

---

## G. What to delete

Deletion is the largest single improvement available, and most of it is
subtraction of **destinations**, not of drawing code (§B).

| # | Delete | Where | Why |
|---|---|---|---|
| G1 | The **DESK / TOOLS / SETTINGS** telemetry composers | `jr_display.c:1531-1544`, `1545-1567`, `1568-1631` | fail both surface tests; 10-glyph label/value pairs lose to a spoken sentence every time |
| G2 | The **DETAIL sheet** and its statics | `jr_display.c:1279-1280`, `1315-1317`, `sp_draw_detail` `:2615` | reachable only from G1, and inert — a tap inside it returns `ACT_NONE` (`jr_display.c:3875-3879`) |
| G3 | The **nav axis** — `JR_DISPLAY_SPACE_*`, `JR_DISPLAY_OVERLAY_DETAIL`, `nav_next/prev/up/down/set/space`, `nav_step` | `jr_display.h:355-359`, `363-366`, `388-400`; `jr_display.c:3739-3826` | with one space there is nothing to navigate; and this is the mechanism by which a rolled rim press walked the owner off a live question (`232fe985`) |
| G4 | **Horizontal swipe = move space**; **centre swipe ↕ = detail/shade**; the `space_hint[]` table; `s_side_page_until_ms`; the 12 s auto-return | `main.c:6737-6751`, `6755-6764`, `6088-6100` | their destinations are gone |
| G5 | The **shade as a persistent surface** — `JR_DISPLAY_OVERLAY_SHADE`, `s_ui_shade_open`, `s_shade_vol`/`s_shade_light`, `sp_draw_shade`, and the `POST /api/ui/shade` route | `jr_display.h:365`; `jr_display.c:1320-1321`, `2666-2748`; `main.c:3153-3184`, route `:5552` | replaced by the transient Dial (F2). This deletes the **room**, not the controls. It also removes the HTTP/nav door mismatch below |
| G6 | **`s_listen_idle_deadline_ms`** and its two diag readers | `main.c:748`, `2105`, `4444`, `6195`, `6381`, `6384`, `7002` | assigned only `0`, forever; keeping it invites someone to build a countdown for it |
| G7 | **`TOUCH_SWIPE_DOMINANCE_PCT`** | `components/jr_hal/src/input_touch.c:51` | defined once, **referenced nowhere** — the general dominance gate was removed (`input_touch.c:170-182`); only the shade's 135 % gate survives, and G5 retires that too |
| G8 | The **"compositor-native cards and radial controls"** roadmap line | `docs/ROADMAP.md` "Next" | §B rejects both; leaving it queued keeps costing plan attention |

**G5 also closes a real bug.** Every touch closer manipulates the **nav overlay**;
`POST /api/ui/shade` manipulates only the **legacy `JR_DISPLAY_SHELL_SHADE`
bit** — the two are OR-ed at `jr_display.c:1407-1415`. So an HTTP-opened shade
needs **two** dismiss taps (the first takes the `else` at `main.c:6845-6847` and
calls `nav_down()`, *opening* the nav shade), and an UP swipe on it misses the
`overlay == SHADE` branch at `main.c:6714` and lands in DETAIL instead. One door
per surface; G5 leaves one door.

**Expected structural effect.** G1–G5 remove most of the shell's *content* and
*routing* while keeping its *primitives*, and they collapse the dispatch chain
`INTERACTION_MODEL.md §4` is trying to tame. Of the **16 `continue` branches** in
`main.c:6440-6906`, G3–G5 retire the space-nav tail, the shade branches and the
overlay-swallow guard (`main.c:6861-6863`) outright. That chain gets materially
shorter **before** it is refactored — which is why §H sequences deletion ahead of
the refactor, contradicting `INTERACTION_MODEL.md §7`'s ordering on purpose.

### What I am NOT deleting, and why

- **The five baked EAF clips** (~3.9 MB of ~7.4 MB PSRAM, `jr_display.c:112-116`).
  The archived plan is explicit: the procedural idle is *"a downgrade in richness
  today"*; retire them only if flash/PSRAM is needed, not for looks (decision
  D4). Nothing here needs their space. **Trigger, not decision:** revisit D4 only
  if a future asset genuinely will not fit.
- **`hud_render_rows()` and the whole procedural face path**, despite having
  **zero call sites outside its own header, source and tests**. It is the D4
  escape hatch and it is the only thing the strip-invariance test exercises. Keep
  it, and keep it compiled.
- **The spatial shell's primitives, clip discipline and hit path** (§B).
- **Double-tap home.** With no spaces it means "clear everything" — a better
  meaning than it has now. It just needs to stop outranking the ask (§0.5a).
- **Anything security-shaped:** the `physical` flag, the pairing window, the
  privacy paths, the operator lease, the `remember` consent panel. F1
  *strengthens* all of them.

---

## H. The staged path

Six stages, each independently shippable, each felt. Where a stage maps to
existing plan rows I say so — **one ladder, not three** — so these slot into
`PLAN.md`'s N6 wave and `INTERACTION_MODEL.md §7`'s A–G rather than competing.

**The ordering principle, stated because it contradicts the tree.**
`INTERACTION_MODEL.md §7` makes Phase A (the layer stack) the prerequisite for
everything. Structurally that is right, but if you refactor first you will
carefully re-home eight layers, several of which Stage 2 then deletes. **Delete
first; refactor what survives.**

---

**Stage 1 — Nothing is uniform, and nothing traps you.**
*Days. No refactor required. Pure feedback work.*

- The unbound-gesture legend (§D.3): a distinct neutral ack plus a caption naming
  what this surface accepts. This is the actual close of W3 — `main.c:6469-6474`
  currently gives an unbound swipe the same ripple as a bound one.
- Fix the two exit-caption violations: `main.c:327` (BOOT clears the caption) and
  `main.c:6841-6859` (DISMISS-tap opens the shade captionless).
- The ADJUST cue class and the ABANDON/REFUSE distinction (§D.1, §D.2).
- The two ordering fixes from §C: move double-tap below the ask
  (`main.c:6607` vs `6632`), and make the right slab increase on UP
  (`main.c:6705-6713`).

*Felt:* every touch gets an answer that means something, a fast retry after a
missed arc reaches the arc instead of being stolen by double-tap-home, and the
two level gestures stop disagreeing.
*Plan rows:* W3, W4 (partial).

---

**Stage 2 — The screen stops being a filing cabinet.**
*The big one. Mostly deletion.*

- G1–G7.
- F3: Rest shows the watch — wire `apply_clock_overlay` to the AMBIENT rung of
  the existing mood ladder.

*Felt:* the rows the owner called useless are gone, four screens become one, and
after twenty seconds the glass tells the time by itself. **Negative net line
count.**
*Plan rows:* contributes to N6.5 (`main.c` < 4,000 lines, from 7,524 today) —
the nav/swipe/telemetry deletions are a few hundred lines; the module split
remains the bulk of that row.

---

**Stage 3 — Phase A: the layer stack, on what survives.**
*Pure refactor, now much smaller.*

`INTERACTION_MODEL.md §4`: each layer answers `CONSUMED` / `PASS`. After Stage 2
the chain is missing its space-nav tail, both shade branches and the
overlay-swallow guard.

*Felt:* **the weakest stage on felt improvement, and I will not pretend
otherwise.** What the owner feels is the absence of recurrence — three of this
session's bugs (`232fe985`, `80636399`, and the double-tap/ask ordering in
§0.5a) are precedence bugs in a chain that exists only as source order. This is
the stage that makes that class structurally impossible. If it needs something
visible attached, ship it with Stage 4.
*Plan rows:* Phase A; the remainder of N6.5.

---

**Stage 4 — The rim answers, and hold means yes.**
- F2: the rim dial with a landed-target readout; G5's shade demotion completes.
- F1: hold-to-commit, one stage, generalising the existing `remember` consent
  panel (`main.c:1736-1760`, `3631-3652`) into a primitive, wired to OTA accept,
  lease handover, consequential tools and clearing privacy.
- Privacy's two discoverable paths (flip + Dial mute) land here, per §C.

*Felt:* levels stop changing silently and stop changing the wrong way; the device
gains a way to ask "are you sure" that a television cannot answer.
*Plan rows:* Phase C, narrowed to one stage.

---

**Stage 5 — The answer gets a shape.**
- F5: `show_gauge(label, value, max)` + the arc re-homed from `sp_focal_desk`,
  plus a timer built on app-loop deadline arithmetic (no `esp_timer` exists).
- Jarvis's three one-shot spoken lessons (§E.4), now that all three surfaces they
  teach exist.

*Felt:* timers and long jobs become glanceable; the screen becomes part of the
answer instead of decoration. This is the most direct response to *"make the
actually cool stuff we need cool."*

---

**Stage 6 — It sleeps, and it wakes when you pick it up.**
*Gated on hardware, correctly.*

- Scope-prove QMI8658 INT2 (PLAN N6.12).
- Measure real current before DFS or light sleep (ROADMAP "Next").
- Then: light sleep at DREAM, any-motion wake, lift-to-wake; retire the 100 Hz
  polling task while preserving flip, shake and lift.

*Felt:* the largest change in what the device *is* — from a thing that must stay
awake to stay ready, to a thing that is ready because you picked it up.
*Plan rows:* Phase F; N6.12.

---

## What I could not verify

- **The HTTP/nav shade door mismatch (§G5)** was traced from the cited lines, not
  run on hardware. The code paths are unambiguous; the intent is undocumented.
- **NVS namespace for the three one-shot-lesson bits (§E.4).** The `"app"`
  namespace pattern is established (`main.c:370-436`) but I did not confirm it is
  the right home.
- **The ~16 fps settled figure** comes from a comment (`jr_display.c:865`) and
  `ARCHITECTURE.md:143-146`, not a measurement I took. PLAN.md N6.9 ("raise
  settled controls cadence") implies the controls path is meaningfully slower —
  Stage 4's dial must be measured against that, not assumed cheap.
- **Whether the owner accepts moving privacy off the centre-hold (§C).** This is
  the one binding in current use that I reassign; the design works with either
  assignment, but not with both meanings on one gesture.
- **Three stale comments found in passing**, flagged so nobody designs against
  them: `jr_display.c:1101-1105` describes the caption as scale 1 at y=403 when
  the code renders scale 2 at y=374/394/402 in a 360-430 band; `hud_render.c:953`
  says the choice annulus is r217-236 when the constants are 223-231;
  `hud_render.h:120-121` describes a thinking track ring at r=150 that
  `hud_render.c:611-617` deliberately does not draw (the comet is at r142/145).
