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
> per-callback `long_press.press_time`, i.e. **multiple hold thresholds on one
> button, already built**. It hands touch wholly to LVGL, which D1 rules out
> here, so our own touch classifier is necessary work rather than reinvention.
> Its whole model is one button and two gestures — worth taking seriously as an
> argument for restraint.

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
| **CPU sleep** | ❌ **`esp_sleep` has zero uses**. But Wi-Fi **modem** sleep is already mood-driven (`WIFI_PS_MIN_MODEM` at rest), so the gap is CPU sleep specifically — not "no power management" | Battery anxiety. Caveat: the audio codec is never closed, so it holds an APB lock and automatic light sleep would save ~nothing as built. |
| **QMI8658 interrupt engines** (any/no-motion, tap). ⚠️ **INT2→GPIO21 is 1.75 routing and is UNVERIFIED on the C** — the vendor BSP sets `BSP_CAPS_IMU 0` and its examples poll. Resolve from the C schematic PDF before writing any wake code. | ❌ a comment only; we poll at 10 Hz | The wake gesture you never have to learn — pick it up, it is ready. Also the *prerequisite* for sleeping at all. |
| **Auto-upright from pitch/roll** | ❌ zero rotation uses | Reading the screen at any angle. Round glass means no aspect change, and the data already crosses the HUD boundary unused. |
| Second mic / DOA | ❌ unused (ES7210 is 4-channel) | Knowing *who* spoke. Optional, not planned. |
| Multi-touch | ❌ capped at 1 point by our own call | Nothing needs it yet. |
| DAC for UI sound | ✅ used — cues now rise *and* fall | |
| Buttons / battery / tilt-to-HUD | ✅ used | |
| Hardware RTC | 🚫 **does not exist on the 1.75C** (no PCF85063). Wall time is SNTP-only, so it resets across any reboot-class wake — this constrains deep sleep | |

---

## 7. Build order

| Phase | What | Status |
|---|---|---|
| **B** | Refusal feedback: contracting ring + falling note | ✅ **done** |
| **D** | GPIO0 button ladder | ✅ done (PKEY) |
| **E** | Double-tap + side pages | ✅ done |
| **A** | Layer stack, `CONSUMED`/`PASS`, no behaviour change | open — §4; unblocks the rest |
| **C** | Staged hold: preview → commit → system, with a filling ring | open — touch has one 850 ms threshold and no preview. `iot_button` already does this for the *button* |
| **F** | IMU interrupt engines + sleep | open — the biggest hardware win (§6) |
| **G** | Auto-upright overlay rotation | open — render-side only |

**F is where "use the hardware right" actually cashes out**: the interrupt
engines and sleep are one item, because the engines are what make sleeping
possible, and lift-to-wake is the gesture nobody has to be taught.
