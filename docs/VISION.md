# JarvisNano — The Vision

> A pocket AI companion the size of a watch face. You talk, it thinks, it answers — with a
> character that lives on a round AMOLED and a body that wakes when you lift it, sleeps when you
> set it down, and mutes when you turn it face-down. **Not a dev board. A product.**

---

## The one-liner

**JarvisNano turns a $14 ESP32-S3 + a 1.75″ round AMOLED into a desk companion with the soul of a
tiny smartwatch OS** — WiFi-direct Gemini Live voice, hardware-gesture control, four power "moods,"
and an interactive, animated, round-native UI.

## Why it's special

| Most ESP32 voice gadgets | JarvisNano |
|---|---|
| A board with a mic | A *character* you interact with |
| Static screen or a blinking ring | An animated, round-native OS with personality |
| Always-on or always-asleep | **Four power moods** that each feel intentional |
| Tap a button to talk | Lift to wake · double-tap · "Hey Jarvis" · flip to mute |
| Shows text | **Asks you questions you tap to answer** on the rim |

Everything is built on **silicon already soldered to the board** — the QMI8658 IMU, AXP2101 PMIC,
PCF85063 RTC, and a 2-mic AEC array — that today's firmware doesn't yet use. The vision is mostly a
*firmware + design* effort, not a hardware redesign. See [`UPGRADE_RESEARCH.md`](UPGRADE_RESEARCH.md).

---

## See it now — the interactive showpiece

A faithful, clickable simulation of the whole experience runs in a browser:

```bash
# from the repo root
python3 -m http.server 8771 --directory docs/prototype
# then open http://localhost:8771/jarvisnano-os.html
```

Or just open [`docs/prototype/jarvisnano-os.html`](prototype/jarvisnano-os.html) directly.

It renders the real 466×466 round screen with: the live arc-reactor **voice-state faces**
(idle / listening / thinking / speaking), the **four power moods** with their wake-bloom and
sleep-iris transitions, **tap-to-answer choice arcs**, the **radial dial menu**, **swipeable
notification cards**, the **ambient watch face** (clock + battery rim), and personality
(flip-to-mute, lift-to-wake). Hit **Demo Reel** and it performs the whole thing on its own.

---

## The 60-second demo script

1. **It's asleep** on the desk (DREAM — black screen, µA). *"It's basically off — but it's
   watching for motion."*
2. **Pick it up** → a point of light **blooms** into the arc-reactor ring, WiFi connects on an
   orbiting dot. *"Good morning, Sir."*
3. **Say "Hey Jarvis"** → the face bursts into the **listening** waveform, a rim ring counts down.
4. It **thinks** (a dot orbits the center), then **speaks** — the waveform reacts and the face
   **leans toward you** (it heard which side you spoke from).
5. **It asks a question** — *"Reply to Sarah?"* — and three **choice arcs** wrap the bezel.
   **Tap "Yes."** The arc fills and confirms. *"Done."*
6. **Spin the radial menu** with a flick (or tilt the device like a dial). **Swipe through cards.**
7. **Turn it face-down** → it **winks and mutes**. *"It knows when you want privacy."*
8. **Set it down** → the face settles, the clock fades in, then an **iris closes** to sleep.

That's the product. Eight beats, every one of them delightful.

---

## From simulation to silicon — what's real

| Layer | Today | The vision adds |
|---|---|---|
| Voice | ✅ WiFi-direct Gemini Live (16 k in / 24 k out), on-device VAD, on-device brain | wake-word entry, command words |
| Face | ✅ reactive waveform (baked `.aaf` + `gfx_motion`) | full state-driven UI + transitions |
| Screen | animated face only | watch face, menus, choice arcs, cards (LVGL @ 200–300 fps) |
| Motion | ❌ QMI8658 disabled | tap / lift / flip / shake (hardware engines) |
| Power | always full | 4 moods (AXP2101 rails + ESP sleep), lift-to-wake |
| Battery | reads chip but ADC off | enable ADC → real %, low-batt UX |

**Architecture stays put:** the device talks to Gemini Live directly over WiFi. No phone brain, no
BLE audio service. (On-phone Gemma is a *future* privacy option, out of scope.)

## Build order (front-loads the silicon already on the board)

1. AXP2101 ADC-enable → real battery (the live device proves the chip ACKs; the ADC is just off)
2. Four-mood power state machine + transitions
3. QMI8658 enable → gestures + lift-to-wake (INT2 on `GPIO21`, schematic-confirmed)
4. Ambient watch face
5. "Hey Jarvis" wake word (esp-sr WakeNet + the on-board AEC)
6. LVGL interactive layer — choice arcs, cards, radial menu
7. State-driven UI + transitions
8. Personality + polish

Full detail, code, and library recs in [`UPGRADE_RESEARCH.md`](UPGRADE_RESEARCH.md). Every vendor
datasheet, the board schematic, and the official Waveshare driver examples are mirrored locally
under [`docs/reference/vendor/`](reference/vendor/INDEX.md) (incl. ESP-IDF reference code for the
AXP2101 ADC read and QMI8658 wake-on-motion — the first two build steps).

---

*The hardware is a $14 chip. The vision is what you do with the silicon that's already on it.*
