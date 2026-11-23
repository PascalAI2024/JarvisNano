# Input Map

**Companion to [`docs/GLASS_DESIGN.md`](GLASS_DESIGN.md).**
Last reconciled with live firmware (`main/main.c`, `components/jr_hal/src/input_touch.c`, `components/jr_power/src/jr_power.c`, `components/jr_imu/src/jr_imu.c`): **2026-11-23**.

---

## 1. Governing principles

> **Buttons address the DEVICE. Glass addresses the CONVERSATION. Body addresses the SITUATION.**

| Channel | Scope | Why it belongs there |
|---|---|---|
| **Buttons** (`PWR`, `BOOT`) | Device-level escape hatches | Context-free and always available — even when the glass is dark, sleeping, or showing a modal. |
| **Glass** (CST9217 touch) | Contextual UI & levels | **Place is scope:** inner disc (`r < 168`) owns the conversation & privacy commit; outer rim (`r ≥ 168`) owns sliding device knobs. |
| **Body** (QMI8658C IMU) | Physical reality | Self-explaining physical states (face-down = quiet, shake = dismiss/cancel, lift = wake). |

**Coverage invariant:** Every daily action is reachable by voice or by the five core moves (*talk · tap what you see · hold inner glass to mute · slide the rim for levels · flip face-down for quiet*). The only deliberately physical-only actions are **BOOT hold pairing** (proof of presence) and **physical privacy unmute** (remote APIs can never unmute).

---

## 2. Live physical bindings

### 2.1 Buttons (`PWR` and `BOOT`)

The 1.75C has two programmable buttons and **no hardware reset button**.

| Input | Hardware path | Live firmware action | Voice equivalent |
|---|---|---|---|
| **PWR short press** | AXP2101 `INTSTS2` (500 ms I²C poll) | Dismiss a ringing timer; otherwise wake/re-arm `LISTENING` (never mutes) | Say `"Jarvis"` / `"cancel timer"` |
| **PWR long press** | AXP2101 `INTSTS2` | Power off completely via `jr_power_off()` (drops all rails; hold PWR 1 s to start; refused during OTA probation) | — |
| **BOOT short** (`200–1500 ms`) | `GPIO0` (~10 Hz poll) | Open or close the control shade | `"set volume / brightness"` |
| **BOOT hold** (`1.5–5.0 s`) | `GPIO0` | Display random 6-digit pairing code and open 60 s claim window | *Physical-only (security)* |
| **BOOT hold** (`≥ 5.0 s`) | `GPIO0` | Panic-home: clear all modals/overlays and return to Home | Double-tap glass |
| **BOOT held during reset** | `GPIO0` strapping | ESP32-S3 ROM downloader (hardware boot mode, not app input) | — |

> **Note on PWR double-tap:** Not supported — AXP2101 `INTSTS2` is polled at 500 ms over I²C, so a ~400 ms double-tap cannot be resolved. Physical privacy mute therefore lives on the inner-disc glass hold and face-down flip.

### 2.2 Glass (CST9217 single-point touch, software-classified)

Touch thresholds (`components/jr_hal/src/input_touch.c`): tap/hold drift slop = **42 px**, swipe travel = **42 px**, long press = **850 ms**, double-tap window = **400 ms**, inner-disc vs rim annulus = **`r = 168 px`** from centre `(232, 232)`.

| Gesture | Inner disc (`r < 168`) | Outer rim (`r ≥ 168`) |
|---|---|---|
| **Tap** | Dismiss a ringing timer → act on rim choice arc or shade control → speak an open detail sheet aloud | Same hit-test for choice arcs (`r223–231`) when an ask or Desk card is active |
| **Double-tap** | **Home** — dismiss overlays and immediately release any Codex/ZeroChat operator lease (never outranks a live ask) | — *(rim rolls under a thumb)* |
| **Hold (850 ms)** | **Physical privacy mute / unmute** — commit ring fills to 850 ms; lifting early cancels in silence. Rising chime on unmute, descending chime on mute. | **Ignored** — neutral ripple + legend `RIM - SLIDE FOR VOLUME OR LIGHT` so a slow level slide can never accidentally mute |
| **Vertical slide (↑ / ↓)** | Walk the 5-screen spatial ring (`JARVIS ↔ WATCH ↔ WEATHER ↔ STATUS ↔ (DESK) ↔ ACTIVITY`, wrapping); open/close the current screen's detail sheet | **Left rim (`x ≤ 232`):** Volume ±5 with live rim arc<br>**Right rim (`x > 232`):** Brightness ±5 with live rim arc<br>*(UP increases on both halves; top-edge pull excluded)* |
| **Horizontal swipe (← / →)** | **10 s WATCH peek** from any ring screen; on **`WATCH`**, cycles all **8 luxury watch styles** (`JARVIS · DIVER · DRESS · PILOT · MINIMAL · FUTURE · SKELETON · CELESTIAL`, persisted to NVS); on open shade, adjusts volume ±10 | Same horizontal gesture |

### 2.3 Body (QMI8658C accelerometer)

| Physical motion | Live firmware action |
|---|---|
| **Flip face-down** (~600 ms sustained) | Enter flip privacy mute (gold ring, closed slit). Turning face-up clears **only** a flip-origin mute — never a glass-hold mute. |
| **Shake** (2 polls, 1.5 s cooldown) | Dismiss a ringing timer; otherwise cancel the active voice turn. |
| **Lift after rest** | Weather glance (8 s) while awake/resting; wakes the ESP32-S3 from deep sleep via QMI8658 Wake-on-Motion (`INT1 → GPIO21`). |
| **Tilt** | Smooth specular bevel/hand lighting on watch dials and 14-star foreground parallax on `CELESTIAL`. **Never** triggers a control action. |

---

## 3. Precedence rules

When multiple surfaces are active, inputs resolve strictly in this order (highest first):

| Priority | Owner | Behavior |
|---|---|---|
| **1** | **Buttons (`PWR`, `BOOT`)** | Always active regardless of screen/modal state. |
| **2** | **Body (Flip / Shake)** | Physical reality outranks glass state; face-down silences the mic immediately. |
| **3** | **Commit ring** | An 850 ms inner-disc hold in progress cannot be stolen mid-hold. |
| **4** | **Active Ask / Consent / Codex card** | Owns taps and choice arcs (`r223–231`); double-tap exits Codex lease but never steals a live local ask. |
| **5** | **Ringing Timer** | Physical tap, shake, or PWR short dismisses the ringing alarm cleanly before normal screen actions run. |
| **6** | **Rim level arcs (`r ≥ 168`)** | Transient left-volume / right-brightness vertical slides from any screen. |
| **7** | **Ring & Shade navigation** | Spatial ring swipes, detail sheets, watch style stepping, and shade controls. |

---

## 4. Feedback classes & synthetic provenance

Every input answers with a distinct visual and audio class (toggleable via `set_ui_sounds` or Command Deck, except privacy/alarm safety tones):

| Feedback class | Visual on glass | Audio cue |
|---|---|---|
| **Accept / Nav** | Expanding cyan ripple | Subtle tick (`JR_CUE_TICK`) |
| **Dismiss** | Surface closes | Soft dismiss (`JR_CUE_DISMISS`) |
| **Adjust (rim levels)** | Live rim arc + readout (no per-step ripple) | Endpoint tick at limits (`10` / `100`) |
| **Commit (privacy)** | Filling commit ring closes | Rising sweep (`500→1100 Hz`) on unmute; falling (`1100→500 Hz`) on mute |
| **Abandon hold** | Commit ring drains | Silence (never punish exploration) |
| **Unbound (e.g. rim hold)** | Neutral ripple + hint caption | Silence |
| **Refuse / Limit** | Dim feedback | Falling refusal / limit cue |

**Physical authority boundary:** HTTP endpoints (`/api/debug/input`, `/api/input/tap`) inject events tagged `synthetic`. Synthetic inputs may walk the ring and open sheets for diagnostics, but firmware refuses to let synthetic events clear physical privacy, approve consent, dismiss physical safety prompts, or override a companion lease.
