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

The live 1.75C firmware now uses nearly every product-relevant chip already
soldered to the board: QMI8658 motion sensing, AXP2101 battery/PKEY telemetry,
the dual-microphone ES7210/ES8311 audio path, touch, and the AMOLED. The
remaining hardware work is narrower and measurable: move motion wake from
polling to QMI8658 INT2, prove safe low-power states, and use the microphone
array for directional expression. The 1.75C has no PCF85063 RTC or microSD;
those belong only to the original 1.75 hardware.

---

## Explore the design reference

The browser prototype preserves the visual design space:

```bash
python3 -m http.server 8771 --directory docs/prototype
# open http://localhost:8771/jarvisnano-os.html
```

[`docs/prototype/jarvisnano-os.html`](prototype/jarvisnano-os.html) is a
prototype, not firmware evidence. It deliberately includes unshipped ideas such
as the radial menu, swipeable notification stack, directional face lean,
automatic Watch transition, and iris sleep. Use the live-truth table below when
deciding what the device does today.

## The live 60-second demonstration

1. Start on the calm reactor face; show the 466×466 submission mirror beside the
   physical panel and call out that it is a software mirror, not panel readback.
2. Press **PWR** once. The device shows **LISTENING** and never mutes.
3. Ask a natural question. Listening becomes Thinking, then Speaking; the halo
   and waveform are driven by live audio state.
4. Interrupt the reply by voice or tap. Playback stops and the same session
   returns to Listening.
5. Trigger a real Gemini choice. Three round-safe choice arcs appear and a
   physical tap resolves one.
6. Swipe vertically through the centre to walk Watch, Power, Desk, and Tools. Use the
   left and right edges to change volume and brightness without leaving the
   current surface.
7. Press **BOOT** or swipe from the top edge to open controls. The glass itself
   repeats the map: `L VOL`, `R LIGHT`, `PWR LISTEN`, `BOOT CLOSE`, and
   MUTE/LISTEN.
8. Hold the glass to enter physical privacy. The persistent gold ring and muted
   caption agree; hold again to resume.

That is the implemented product loop: voice, interruption, tools, round-screen
interaction, physical privacy, and observable recovery—without a phone brain or
a hidden second control grammar. Final PWR/BOOT hand proof remains an explicit
release item in [`../PLAN.md`](../PLAN.md).

## From simulation to silicon — current truth

| Layer | Live on 1.75C | Next bounded refinement |
|---|---|---|
| Voice | Gemini Live, WakeNet9 “Jarvis,” on-device VAD/AEC, barge-in | long-session recovery and measured reconnect headroom |
| Face | five baked reactive faces plus one procedural overlay compositor | iris sleep, Wi-Fi orbit, and measured directional lean |
| Screen | explicit Watch, choice arcs, minimal controls, captions, remote canvas, bounded Desk surfaces | card navigation only after reliability gates |
| Input | global edge volume/brightness, horizontal spaces, top-edge controls, centre detail, physical hold/flip privacy, PWR listen, BOOT controls/pairing | enclosure legends and gesture receipt polish |
| Motion | QMI8658 at 125 Hz ODR with a 100 Hz sampler: flip, shake, lift, orientation | INT2 any/no-motion wake so DREAM can stop polling |
| Power | four visual moods, brightness slew, battery telemetry, listen-only PWR | measure dynamic-frequency/light-sleep safely; rail gating only with wake proof |
| Storage | 32 MB flash, dual 4 MB OTA slots, emote/model partitions | decide whether the unused FAT partition becomes durable device memory |

**Architecture stays put:** the device talks to Gemini Live directly over Wi-Fi.
No phone brain and no concurrent BLE audio service. On-phone local inference is
a future privacy route, not part of the live product.

## Refinement order

1. Protect the product loop: OTA recovery, session longevity, and internal
   contiguous-memory headroom.
2. Replace 100 Hz IMU polling with QMI8658 INT2 wake while preserving the
   measured flip/shake/lift behavior.
3. Measure CPU frequency scaling and light sleep against voice latency, Wi-Fi
   stability, and idle current before enabling either by default.
4. Add the missing transition signatures: iris sleep and Wi-Fi orbit wake.
5. Prototype two-microphone direction-of-arrival only if raw-lane captures
   prove stable left/right separation on the physical enclosure.
6. Add radial menu/cards through the existing compositor; do not introduce a
   second UI engine or spend the internal DMA block voice requires.
7. Give the otherwise-unused FAT partition a deliberate role or reclaim it;
   do not create a storage API with no durable product behavior.

Historical research and the original phase plan remain in
[`ARCHIVE/UPGRADE_RESEARCH.md`](ARCHIVE/UPGRADE_RESEARCH.md) and
[`ARCHIVE/JARVISNANO_OS_PLAN.md`](ARCHIVE/JARVISNANO_OS_PLAN.md). Vendor sources
are mirrored under [`reference/vendor/`](reference/vendor/INDEX.md).

---

*The hardware is a $14 chip. The vision is what you do with the silicon that's already on it.*
