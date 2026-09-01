# JarvisRobot — Interaction Model

**Audited against `01dded8e` (1.75C), 2026-08-29.** Where this doc and the code
disagree, the code wins — an earlier draft of this file was written against an
Aug-15 tree and asserted several things the 1.75C work had already solved.

**Scope:** the whole input surface — touch, IMU, the GPIO0 button — and the
feedback owed back for every gesture.
**Companion:** `docs/JARVISNANO_OS_PLAN.md` owns the `GEST-` IDs.

---

## 1. What already exists

Most of the gesture vocabulary is built. Recorded here so nobody re-solves it:

| Capability | Where |
|---|---|
| **TWO physical buttons.** GPIO0 **BOOT** (`boot_button_tick()`, `main.c`) and the AXP2101 **PWR/PKEY**, read over I2C from `INTSTS2` (`jr_power.c:25-33`). Waveshare's own 1.75C product page lists both and says PWR *"supports custom function"*. Older docs claimed PWR was blocked behind an all-output TCA9554 — **that expander does not exist on the C** (`board_info.yaml`: "no TCA9554 expander, no PCF85063 RTC, no microSD slot"), and the vendor's C hardware table lists no expander and no RTC either. That was 1.75 lore. | `main.c`, `jr_power.c` |
| **Double-tap attention** | `s_last_tap_ms`, `main.c:306` |
| **Swipe-right watch peek / side pages** | `s_watch_peek_until_ms`, `s_side_page_until_ms` |
| **Attention beat (sight + sound + words)** | `jr_display_bloom()` + a 160 ms rising note |
| **Flip-to-mute, shake-to-cancel** | GEST-02 / GEST-03, IMU polled at 10 Hz |
| **Shade with its own gesture language** | swipe from top edge; `ui_shade_control_handler` |
| **Synthetic-input refusal on `ask_user`** | the `physical` flag — a remote tap cannot answer a question |
| **Pairing-token auth on mutating diagnostics** | `/api/debug/input`, `/api/audio/self-test` |

Two consequences worth stating plainly, because both contradict the earlier draft:

> **The IMU convention is CORRECT.** `orientation_from_g()` returns `face_up`
> for **negative** gz on the 1.75C, and that is right — screen-up measures
> `gz = -1.026` on the device. A previous session "fixed" this after reading
> `+1.027`; that reading came from a **1.75 (non-C) image flashed onto a 1.75C
> board**, where the Z axis reads inverted. **Do not flip this.** Any device
> measurement taken under a wrong-variant image is worthless — see
> `docs/reference/build-toolchain.md` and check `BOARD_NAME` before trusting a
> number.

> **Xiaozhi is the reference worth knowing.** [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32)
> ships a board file for *our exact hardware* and drives the same GPIO0 button
> — via Espressif's **`iot_button`**, whose `iot_button_register_cb()` accepts a
> per-callback `long_press.press_time`, i.e. multiple hold thresholds on one
> button. ⚠️ **`iot_button` is NOT linked in OUR tree** (zero hits in `main/` and
> `components/`): our BOOT handling is a hand-rolled polled tick
> (`boot_button_tick()`, `main.c:310-344`, `GPIO_INTR_DISABLE`, ~10 Hz) with two
> thresholds by hand — 200-1500 ms → shade, 1500-5000 ms → pairing — and a hold
> of **≥5000 ms falls off the end of that chain with no binding**. It is an
> option to adopt, not something we already have. It hands touch wholly to LVGL, which D1 rules out
> here, so our own touch classifier is necessary work rather than reinvention.
> Its whole model is one button and two gestures — worth taking seriously as an
> argument for restraint.

> **Physical facts, settled 2026-08-29 against the vendor's own 1.75C page and
> `board_info.yaml`, because 1.75 lore keeps leaking onto this board:**
>
> - **Two buttons, both programmable** — Waveshare: *"Onboard PWR and BOOT
>   programmable buttons for easy custom function development."* PWR/PKEY is the
>   AXP2101 key read over I2C; BOOT is GPIO0.
> - **No reset button.** It is not in the vendor's 14-item callout. Reset is the
>   USB path or a PWR long-press through the PMIC.
> - **The small hole in the case is a MICROPHONE PORT**, not reset —
>   `hardware/enclosure/amoled-1_75/README.md:23`: *"MIC1 + MIC2, edge-mounted
>   MEMS mics, far-field AEC pair."* Covering it degrades wake-word range and the
>   AEC reference. Worth saying out loud in any enclosure or grip design.
> - **No TCA9554 expander, no PCF85063 RTC, no microSD** on the C.

---

## 2. The model

Two axes carry the whole system, and both are already visible in the code:

> **Direction encodes category. Duration encodes commitment.**

Vertical is *system* (down from the top edge opens the shade); horizontal is
*content* (watch peek, side pages); tap acts on what is under the finger. A user
who internalises the axes can predict a binding nobody taught them.

The gap is **duration**. Touch has a single 850 ms threshold, so a hold is
all-or-nothing with no preview — see Phase C.

---

## 3. The feedback contract

**Every gesture is acknowledged, and a refusal never looks like a success.**

| Event | Visual | Audio |
|---|---|---|
| Tap accepted | expanding cyan ripple | — |
| **Tap refused** | **contracting dim ring** (`jr_display_ripple_reject`) | **falling note** 700→300 Hz |
| Attention / wake | bloom + caption | rising note 400→2000 Hz |
| Hold stage fires | *(Phase C)* ring completes | rising note |

The device has **one ES8311 and no mixer**, so every cue rides the bounded sweep
path and is *dropped* while a reply plays. That is intended: a reply talking over
its own feedback tone is worse than a silent tone.

`jr_audio_play_sweep(start_hz, end_hz, ms, level)` exposes the endpoints the
chirp body always had, so a cue can **fall** as well as rise — the cheapest
unmistakable "no" on a device with no haptics. `jr_audio_diag_play_chirp()` is
now a wrapper passing `(400, 2000)`, bit-identical for every existing caller.

---

## 4. Precedence — the open structural problem

Touch dispatch in `voice_task()` is **~430 lines** (`main.c:6403-6832`) with
20-plus `continue` statements. The order is correct, but it exists *only* as
source order: you cannot see it, test it, or extend it without re-reading every
branch. It has roughly doubled as the gesture vocabulary grew, and each new
surface (`codex_mode`, `watch_peek`, side pages) added another guard mid-chain.

The fix is a declared layer stack — each layer answers "is this event mine?" and
returns `CONSUMED` or `PASS`:

| # | Layer | Claims |
|---|---|---|
| 0 | telemetry + universal ack | *observes only, never consumes* |
| 1 | touch challenge | everything, while active |
| 2 | demo reel | any touch |
| 3 | operator / codex mode | its own gestures |
| 4 | brain surface | tap on a hit |
| 5 | ask arcs + answer grace | tap |
| 6 | shade | tap, swipe-up |
| 7 | base | everything remaining |

This is Phase A. It is a **pure refactor** — no binding changes — and it is the
prerequisite for C and G landing without deepening the chain further.

---

## 5. Affordances that must survive any remap

- [ ] Touch-challenge abort · demo stop · brain-surface tap
- [ ] Ask-arc answer + the 600 ms trailing-tap grace
- [ ] **Synthetic input refused for `ask_user`** (security, not convenience)
- [ ] Tap-wake from rest, honouring `!s_flip_muted`
- [ ] Shade open / close / dismiss and its three controls
- [ ] Mute toggle in every phase, `JR_ST_DRAINING` excluded
- [ ] **Privacy stop** and the **pairing claim window** (both security)
- [ ] Watch peek, side pages, double-tap attention, PKEY

---

## 6. Hardware utilisation

Audited against the live device. Nearly every remaining friction is a capability
the board already has and the firmware does not reach for.

| Capability | Status | Friction it removes |
|---|---|---|
| **CPU sleep** | ✅ **deep sleep, 2026-09-01**: ten minutes into DREAM on battery (`jr_mood_sleep_due`, `enter_deep_sleep`), wake on lift, touch or a 4 h timer. Wi-Fi **modem** sleep stays mood-driven. Light sleep remains out: the audio codec is never closed, so it holds an APB lock and automatic light sleep would save ~nothing as built. See `docs/reference/power-modes.md` | Battery anxiety. |
| **QMI8658 interrupt engines** (any/no-motion, tap). Pin resolved from the C schematic: **INT1 → GPIO21**, INT2 goes nowhere (`docs/reference/imu-interrupt-routing.md`). | ✅ **Wake-on-Motion armed on INT1 before every deep sleep** (`jr_imu_arm_wake_on_motion`); the 100 Hz sampler still runs while awake. Tap engine unused. | The wake gesture you never have to learn — pick it up, it is ready. |
| **Auto-upright from pitch/roll** | ❌ zero rotation uses | Reading the screen at any angle. Round glass means no aspect change, and the data already crosses the HUD boundary unused. |
| Second mic / DOA | ❌ unused (ES7210 is 4-channel) | Knowing *who* spoke. Optional, not planned. |
| Multi-touch | ❌ capped at 1 point by our own call | Nothing needs it yet. |
| DAC for UI sound | ✅ used — cues now rise *and* fall | |
| Buttons / battery / tilt-to-HUD | ✅ used | |
| Hardware RTC | 🚫 **does not exist on the 1.75C** (no PCF85063). Wall time is SNTP-only, so it resets across any reboot-class wake; a deep-sleep wake re-syncs on Wi-Fi like any boot | |

---

## 7. Build order

| Phase | What | Status |
|---|---|---|
| **B** | Refusal feedback: contracting ring + falling note; neutral ack for unbound | ✅ **done** `1e585570`, `617d37e3` |
| **D** | Physical buttons | ✅ mostly — BOOT gained panic-home at ≥5 s (`77824301`). ⛔ **PWR double-tap is undetectable**: the PKEY latch polls at 500 ms, so a ~400 ms window cannot be resolved, and `iot_button` cannot help (it drives GPIO/ADC, not an I²C latch) |
| **E** | Double-tap + horizontal swipe | ✅ done — and the side pages were **deleted** (`6de40bd2`); horizontal now peeks the watch |
| **A** | Layer stack, `CONSUMED`/`PASS`, no behaviour change | **open** — §4. Deliberately sequenced AFTER the deletions: refactoring first means carefully re-homing layers that Stage 2 then removes |
| **C** | Hold-to-commit: **one** stage with a filling ring, not three | ✅ **done** `1d07f621`, `1a081e7e`, `617d37e3`. The HAL now emits `PRESS_DOWN`/`PRESS_UP`; the ring previews at 400 ms, fills to 850 ms, and lifting early abandons in silence. Still completes into the privacy toggle — see **D** for why it cannot move to PWR yet |
| **F** | IMU wake engines + sleep | ✅ **done 2026-09-01** `37bf45db` — deep sleep ten minutes into DREAM on battery; WoM on **INT1** armed only for the sleep itself (the sampler runs while awake, so flip and shake are unaffected); touch INT and a 4 h timer as the other roads back; a sleep during OTA probation is refused. Lift wake proven armed and quiet from the desk, not yet by a hand |
| **R** | Rim as a true annulus (r ≥ 168) replacing the x-slabs | ✅ **done** `3cbab992` — the knobs no longer reach over the reactor core |
| **G** | Auto-upright overlay rotation | **open** — render-side only. Note `hud_tilt_offset()` is a *deliberately* documented dead end (disabled, returns 0,0) and is the wrong hook: it emits a translation, not a rotation, and pairs the wrong axes |

**F is where "use the hardware right" actually cashes out**: the interrupt
engines and sleep are one item, because the engines are what make sleeping
possible, and lift-to-wake is the gesture nobody has to be taught.
