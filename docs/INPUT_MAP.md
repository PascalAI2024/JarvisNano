# The Input Map — every control on JarvisNano, and why it means what it means

**Companion to `docs/GLASS_DESIGN.md`.** That doc decides *what surfaces exist*.
This one decides *what every physical input does*, across every context, and —
more importantly — **why a user can guess it.**

Status: **plan.** Rows marked ✅ ship today; ⚠️ needs work; 🆕 is new. Every
"today" claim carries a `file:line`; verify before trusting.

---

## 1. The governing principle

A map with thirty bindings is only usable if the bindings are not arbitrary.
One rule does the heavy lifting:

> ### Buttons address the DEVICE. Glass addresses the CONVERSATION. Body addresses the SITUATION.

| Channel | Addresses | Property that makes it the right channel |
|---|---|---|
| **Buttons** (PWR, BOOT) | the device itself | **Context-free and always available.** They work when the glass is confusing, dark, asleep, or showing a modal. They are the escape hatches, so they must never depend on what is on screen. |
| **Glass** (touch) | what is currently on screen | **Contextual by nature.** You are pointing at a thing. If nothing is there, it should say so rather than act. |
| **Body** (IMU) | physical reality | **Unambiguous and self-explaining.** Face-down *is* privacy. Picking it up *is* waking it. These need no teaching because the gesture is the meaning. |

Three sub-rules keep each channel internally predictable:

- **Buttons:** *tap = the common thing · double-tap = its opposite/escape · hold = the rare or consequential thing.* Escalating commitment, same on both buttons.
- **Glass:** *place = scope* — **centre is the conversation, the rim is the device's knobs.** The rim rolls under a finger (measured: 44 swipes to 12 intended taps, `232fe985`), so it carries *sliding* meanings, never pressing ones.
- **Body:** physical states only. Never a "gesture you perform" — only a fact about where the device is.

### The rule that makes a big map safe

> **Coverage invariant: no capability is reachable ONLY by a memorised input.**
> Every binding below is a **shortcut** for something also reachable by *asking*
> or by one of the five core moves. A binding with no non-gesture equivalent is
> a bug.

This is why we can afford thirty bindings where Xiaozhi ships two. Undiscoverable
**accelerators** harm nobody. Undiscoverable **gates** are the defect — and this
rule makes them impossible by construction. It is already half-true: `set_volume`
and `set_brightness` are registered Gemini tools executed on-device
(`main.c:175-198`), and PWR-long already speaks the battery (`main.c:6058-6066`).

**Enforce it or it rots:** one host-test table listing every binding and its
non-gesture equivalent; a binding with no equivalent fails the build.

### The five moves a user must actually know

1. **Talk to it.** 2. **Tap what you see.** 3. **Hold to commit — let go to cancel.**
4. **Slide the rim to change a level.** 5. **Turn it face-down to go quiet.**

Everything else in this document is an accelerator on top of those five.

---

## 2. What the hardware can actually produce

Planning against imagined inputs is how you get a map you cannot build.

### Buttons — two, both programmable

Waveshare, 1.75C: *"Onboard PWR and BOOT programmable buttons for easy custom
function development."* **There is no reset button.**

| Input | Source | Today |
|---|---|---|
| PWR short / long | AXP2101 `INTSTS2` over I²C, latched at 2 Hz (`jr_power.c:214`) | ✅ two levels, hardware-provided |
| BOOT press duration | GPIO0, **hand-rolled polled tick** at ~10 Hz, `GPIO_INTR_DISABLE` (`main.c:310-344`, `7287-7299`) | ✅ two bands: 200–1500 ms, 1500–5000 ms |
| BOOT ≥5000 ms | — | ⚠️ **falls off the end of the if-chain, unbound** |
| Button double-tap | — | 🆕 not detected; needs an app-level window like the glass already uses |

> ⚠️ **`iot_button` is NOT linked in this tree** (zero hits). It would give
> single/double/multiple-click and multi-threshold holds for free, and it is
> what Xiaozhi uses on this exact board. **Adopting it is the cheapest way to
> get every button row below** — otherwise each is hand-rolled.

### Glass — one contact, no hardware gestures

The CST9217 reports **no gesture IDs** and we read a single point, so every
gesture is our own classification (`input_touch.c`).

| Kind | Emitted by | Today |
|---|---|---|
| `JR_INPUT_TAP` | drift ≤42 px | ✅ |
| `JR_INPUT_LONG_PRESS` | ≥850 ms, drift ≤48 px | ✅ fires **once, mid-hold**; release is then suppressed |
| `JR_INPUT_SWIPE` + direction | travel ≥42 px, larger axis wins | ✅ |
| `JR_INPUT_FLAG_TOP_EDGE` | `start_y<72` ∧ `Δy≥70` ∧ 135 % vertical | ✅ emitted, ⚠️ **only ever tested negatively** (`main.c:6695`) |
| Double-tap | app-level 400 ms window (`main.c:6618`) | ✅ |
| **Contact-down / hold progress** | — | 🆕 **does not exist.** The HAL emits only terminal events. **A filling ring has nothing to drive it** — this is the one new HAL event the plan needs. |
| Rim-vs-centre | — | 🆕 today's "edges" are **vertical slabs** (`start_x≤140`, `≥326`) reaching r≈93 — *over the reactor core*. Not an annulus. |

### Body — QMI8658

| Gesture | Today |
|---|---|
| Flip face-down (6 sustained polls ≈600 ms) | ✅ privacy mute |
| Shake (2 polls, 1.5 s cooldown) | ✅ cancels an active turn |
| Tilt (pitch/roll) | ✅ sampled, ⚠️ used only for HUD env; `hud_tilt_offset` is **disabled** |
| Lift / any-motion | 🆕 needs the on-chip engine + **INT2 pin, UNVERIFIED on the C** |
| Knock (case tap) | 🆕 QMI8658 tap engine, unconfigured |

---

## 3. The map

### 3.1 Buttons — global, context-free, identical everywhere

They mean the same thing on every surface. That is the entire point of a button.

| Input | Meaning | Today | Voice equivalent (coverage) |
|---|---|---|---|
| **PWR tap** | **Wake / listen.** Never mutes. | ✅ `main.c:6044-6057` | say "Jarvis" |
| **PWR double-tap** | **Privacy toggle.** Mute/unmute, always captioned. | 🆕 | "mute" / "unmute" |
| **PWR hold ~1 s** | **Speak status** — battery, link, state. Out loud, not on glass. | ✅ battery only (`main.c:6058-6066`); extend | "how's your battery" |
| **PWR hold ~5 s** | **Power off / deepest rest.** | 🆕 | "go to sleep" |
| **BOOT tap** | **Summon the Dial** (levels), showing the target before changing anything. | ⚠️ today toggles the shade *and clears the caption* (`main.c:327`) | "volume 40" |
| **BOOT double-tap** | **Say that again** — repeat the last reply. | 🆕 | "what did you say" |
| **BOOT hold 1.5–5 s** | **Pairing window, 60 s.** | ✅ `main.c:332-339` | — *(deliberately physical-only: security)* |
| **BOOT hold ≥5 s** | **Panic-home.** Clear every modal, dismiss surfaces, return to the face. | 🆕 fills the unbound gap | double-tap the glass |

**Why this split.** PWR is the *system* button — presence, privacy, power, status. BOOT is the *content* button — levels, repetition, escape. Both follow *tap → common, double → opposite, hold → consequential*. Neither depends on what is on screen, so both survive a confused glass.

**Pairing is the one deliberate exception to coverage** — a physical-presence proof must not be sayable. That is the point of it.

### 3.2 Glass — contextual, and place is scope

| Input | Centre (the conversation) | Rim (the device's knobs) |
|---|---|---|
| **Tap** | act on what is under the finger — an arc, a card action, a control | **summon the Dial**, showing what it will change, changing nothing yet |
| **Double-tap** | **home / clear everything** ⚠️ must never outrank a live ask — fixed in `20a921c5` | — *(the rim rolls; a reliable double-tap there is not available)* |
| **Hold 850 ms** | **commit**, with a ring that fills and can be abandoned by lifting | — |
| **Slide** | — | **the level dial**, live readout under the thumb, clamped not wrapped |
| **Swipe ↑** | **expand** — more detail on what was just said | — |
| **Swipe ↓** | **dismiss / stop** | — |
| **Swipe ← →** | **through the conversation** — previous / next reply | — |

**Vertical is the device's own axis; horizontal is time and content.** That is
already the codebase's stated convention (`jr_display.h:294-302`) — this keeps it
and drops the four destinations that made it meaningless.

Note what horizontal swipe becomes: **not navigation between pages, but movement
through the conversation.** Same gesture, and now it addresses the thing the
device is actually for.

⚠️ **Do not make the top crescent load-bearing.** `start_y<72` is 9.8 % of a
round glass. It is *already* not required to open the shade (`TOP_EDGE` is only
tested negatively), and a centre-down swipe is a far larger, more comfortable
target. Keep the generous target; use the flag only to *exclude* a top pull from
the rim dial.

### 3.3 Body — physical truth, no teaching required

| Input | Meaning | Today |
|---|---|---|
| **Flip face-down** | Privacy mute. The best gesture on the device: physical, unambiguous, self-explaining. | ✅ |
| **Shake** | Cancel the current turn. | ✅ |
| **Lift** | **Wake.** The gesture nobody has to be taught — the affordance is picking the thing up. | 🆕 gated on INT2 |
| **Knock ×2 on the case** | Push-to-talk / attention. Works with the device in a pocket or face-down. | 🆕 gated on the tap engine |
| **Tilt** | **Not a control.** Auto-upright only — a control that fires while you carry the device is a bug. | 🆕 |

---

## 4. Precedence — the rule that stops the map fighting itself

Thirty bindings need a declared priority or they collide. Highest first:

| # | Owner | Claims | Rationale |
|---|---|---|---|
| 1 | **Buttons** | always | An escape hatch that a modal can swallow is not an escape hatch. |
| 2 | **Body** (flip / shake) | always | Physical facts outrank screen state. Face-down means quiet, whatever is up. |
| 3 | **Commit ring** | the held contact | A consent gesture in flight must not be pre-empted. |
| 4 | **Ask** | every glass contact | A question owns the glass. **Fixed in `20a921c5`** — double-tap used to steal the retry. |
| 5 | **Dial** | rim contacts | Transient and self-dismissing. |
| 6 | **Base** | everything else | |

**Invariant:** any surface that captures input **captions its own exit**, for as
long as it is up. Established by `80636399`; two violations remain
(`main.c:327`, `main.c:6841-6859`).

---

## 5. Feedback — every input answers, and the answers differ

The current failure is **not silence**: every tap *and* every classified swipe
already ripples (`main.c:6459`, `6474`). The failure is **uniformity** — a swipe
that did nothing looks identical to one that did.

| Class | Visual | Audio |
|---|---|---|
| **Accept** | expanding cyan ripple | — |
| **Refuse** (the device said no) | contracting dim ring | falling 700→300 Hz ✅ |
| **Abandon** (you changed your mind) | ring drains | **silence** — punishing a cancelled hold teaches people not to explore |
| **Adjust** (a level moving) | the value itself moves | tick at the ends of travel only — **never ripple per step** |
| **Unbound here** | distinct neutral ack | none — nothing was refused; **caption what this surface does accept** |
| **Commit fires** | ring closes | rising note |

That last row is the engine of discoverability: **an unbound gesture answers with
the legend**, at the exact moment of curiosity, with no navigation. It replaces
the gesture-card idea entirely — a card is something you must already know how to
find, on the surface you visit least, in a 10-character uppercase font.

---

## 6. What has to be built, in order

| Stage | Work | Status |
|---|---|---|
| **1** | Feedback classes + the two exit-caption fixes | ✅ **shipped** `956d194a` — ADJUST no longer ripples; every capturing surface names its exit |
| **2** | Delete the side pages and the fake feeds | ✅ **shipped** `6de40bd2`, `1a7485dd` — destinations gone, hardcoded TOOLS list deleted, unbound swipe now teaches |
| **4** | HAL contact lifecycle + the commit ring | ✅ **shipped** `1d07f621`, `1a081e7e` — `PRESS_DOWN`/`PRESS_UP`; the 850 ms hold now fills a ring and can be abandoned |
| **5** | Rim as a true annulus (r ≥ 168), replacing the x-slabs | ✅ **shipped** `3cbab992` — the knobs no longer reach over the reactor core |
| **3a** | BOOT ≥5 s → panic-home | ✅ **shipped** `77824301` — filled a binding that fell off the end of the chain |
| **3b** | `iot_button` adoption; **PWR double-tap** | ⛔ **blocked, by measurement.** PWR is an I²C latch polled at **500 ms** (`jr_power.c:33`), so a ~400 ms double-tap window is undetectable — two presses inside one poll are indistinguishable from one. `iot_button` cannot help: it drives GPIO/ADC, not an I²C latch. Needs a faster PKEY poll (more traffic on a shared bus) or an AXP multi-press feature. **Privacy therefore stays on the glass hold** — moving it would strand it. |
| **6** | QMI8658 wake engines on **INT1** + sleep | 🔓 **unblocked, not built.** Pin resolved from the schematic (**INT1**, not INT2) and the WoM register sequence sourced — both in `docs/reference/imu-interrupt-routing.md`. Gated on hands-on: WoM mode produces **no data output**, so it disables flip-to-mute and shake while armed. |

**Sequencing note:** delete (2) before refactoring the dispatch chain. Refactoring
first means carefully re-homing layers that stage 2 then deletes.

---

## 7. Open decisions — yours

1. ~~Privacy on PWR-double-tap, or on the glass hold?~~ **Settled by measurement, not preference:** PWR double-tap is undetectable at the 500 ms PKEY poll, so privacy stays on the glass hold — which now at least *shows* itself, via the commit ring. Revisit if the poll rate is raised.
1b. **Privacy on PWR-double-tap, or on the glass hold?** The map puts it on PWR and frees the 850 ms hold for commit. Evidence for moving it: the owner's long-press counter read **0** — nobody discovered it. It works either way; it cannot be both.
2. **Adopt `iot_button`, or keep hand-rolling?** Adopting gives §3.1 nearly free and matches the board's most-deployed firmware; it adds a managed dependency.
3. **Does horizontal swipe move through the conversation, or stay unbound?** It is the largest new capability here and the least proven.
