# JarvisNano — Hardware Activation & Polish Research

**Board:** Waveshare **ESP32-S3-Touch-AMOLED-1.75** · **Runtime:** ESP-Claw (Lua) ·
**LLM:** Gemini Live (WiFi-direct) · **Date:** 2026-06-11

> **Scope.** Make the *existing* device 10× better by **switching on the silicon that's already
> on the board but currently unused** — the QMI8658 IMU, the AXP2101 PMIC, sleep modes, an
> on-device wake word — and, above all, by turning the round AMOLED into **a tiny smartwatch-class
> OS with personality**: interactive, animated, and delightful (see **§4**, the centerpiece). This
> is a **firmware + interaction-design** effort.
>
> **Architecture is unchanged and stays unchanged.** The device runs the **Gemini Live
> WebSocket directly over WiFi**. We are *not* moving inference to a phone, *not* adding a BLE
> GATT audio service, *not* replacing WiFi. On-phone Gemma is a *future* privacy option, out of
> scope here. Every feature below plugs into the current WiFi-direct flow.

---

## Validation addendum — 2026-06-11 (cross-referenced against code + live device)

This doc was reviewed against the actual codebase and the **live device** (queried over HTTP
while USB-powered). The design is sound; three factual corrections that change implementation:

1. **`/api/battery` is NOT a stub — it already reads the AXP2101, but the read is wrong.**
   The endpoint exists (`http_server_status_api.c::battery_handler`, registered at
   `/api/battery`) and already does real I²C reads of regs `0x00/0x01/0x34/0x35/0xA4`. Live
   device returns `{"wired":false,"mV":16381,"pct":0,"state":"not_wired","source":"axp2101"}`.
   The `source:"axp2101"` proves the chip ACKs. **`mV:16381` ≈ `0x3FFD` (14-bit all-ones) = the
   VBAT ADC channel is never enabled**, so the result register latches full-scale garbage; `pct:0`
   = fuel-gauge/battery-detect never enabled. Root cause: `axp2101` is `init_skip: true`, so its
   ADC + battery-detect blocks are off at boot. **The real fix (§2.1) is to enable them**, not to
   "replace a stub." (And `wired:false` is honest — there's no battery attached on the desk; the
   device runs on USB-C.)
2. **Register correction.** ADC channels are enabled via **reg `0x30` (ADC_CHANNEL_CTRL)** —
   bit0=VBAT, bit2=VBUS, bit3=VSYS — and **battery presence detection via reg `0x68[0]`
   (BAT_DET_CTRL)**, not "reg `0x18[3]`". (Verified against lewisxhe/XPowersLib.) Status decode
   that *is* correct: STATUS1`[3]`=battery present, STATUS1`[5]`=VBUS good, STATUS2`[6:5]`=charge
   direction (1=charging, 2=discharging).
3. **The real read lives in a dedicated component (`firmware/components/jarvis_pmic/`).** It
   borrows the board-manager I²C bus, adds the AXP2101 at `0x34`, **enables the VBAT ADC (reg
   `0x30[0]`) and battery-presence detection (reg `0x68[0]`)**, and decodes status/voltage/percent —
   never touching a power rail. `copy_jarvis_pmic` + `apply_battery_real_read_patch` in
   `bootstrap.sh` vendor it and swap the phase-2 stub for the real read, so it survives clean builds.
   **Verified on hardware 2026-06-11:** after flashing, `/api/battery` returns
   `{"wired":false,"mV":0,"pct":0,"charging":false,"usb":true,"state":"no_battery","source":"axp2101"}`
   — the `16381 mV` garbage is gone and USB-present is detected (no battery attached on the desk).

Everything else validated: no-canvas `esp_emote_gfx` constraint, baked-only toolset, asset
pipeline, render tuning (-O2/30 fps/PSRAM strips), I²S shared-clock, and the open hardware
questions in §7 (IMU INT / AXP IRQ GPIOs still need the schematic) are all accurate as written.

---

## 0. The opportunity — dormant silicon

Almost everything that would make this feel like a finished product is **already soldered on**.
It needs firmware, not parts.

| Chip | I²C addr | Gives us | Status today |
|---|---|---|---|
| **QMI8658** 6-axis IMU | `0x6A`/`0x6B`* | hardware **tap / double-tap**, wake-on-motion, flip, shake, orientation; **INT2 on `GPIO21`** (RTC-capable) | **not declared** in `board_devices.yaml` — must be *added*, not un-skipped |
| **AXP2101** PMIC | `0x34` | battery **fuel gauge** (✅ live via `jarvis_pmic`), power-button (PWRON), switchable rails for sleep | `init_skip: true` — kept; read-only fuel-gauge access added |
| **PCF85063** RTC | (I²C) | persistent clock, alarms → on-screen time, Pomodoro | unused |
| **ES7210 + 2 MEMS mics** | `0x80`† | 2-mic array with **hardware AEC** → ideal for wake word | capture only |

\* QMI8658 silk-note address is `0x6B` (CS→VCC3V3 = I²C mode); Waveshare sample code uses `0x6A`. Probe `WHO_AM_I`==`0x05` at both. † ES7210 codec address per `board_devices.yaml`. **See §7 for all schematic-verified pins/addresses (2026-06-11).**

### Two hard constraints that shape every design below

1. **One shared I²C bus.** SDA = **GPIO15**, SCL = **GPIO14**, 400 kHz (schematic-verified). It
   carries TCA9554, AXP2101, ES8311, ES7210, CST9217 touch, QMI8658, PCF85063 — *all of them*.
   The touch driver already takes a lock and does multi-retry tx/rx. **Rule: interrupt-driven,
   never polled.** Let the IMU/PMIC INT pins raise a GPIO; only touch the bus when something
   happened. Never stand up a second always-on poller.

2. **I²S0 is full-duplex with a shared clock** (`SOC_I2S_HW_VERSION_1`). The mic (RX) only
   captures while the speaker (TX) channel is co-enabled, and both share a sample rate. This is
   already handled in the Gemini Live path (half-duplex turn-taking with reconfigure). It matters
   here because **low-power voice listening still needs I²S running** (TX co-enabled, silent) — so
   "always listening" is not free; gate it behind motion (§3.4).

---

## 1. QMI8658 IMU — gestures & motion

The QMI8658 has **hardware motion engines** — tap detection, wake-on-motion, and motion
classification run *inside the chip* and raise an INT pin. We mostly **configure and react**, not
run DSP on the ESP32.

Integrated engines (QST datasheet): **Pedometer, Tap (single & double), Any-Motion, No-Motion,
Significant-Motion**, with a 1536-byte FIFO, all routable to INT1/INT2.

### 1.1 Gesture → action map

| Gesture | Detection | Action |
|---|---|---|
| **Double-tap** (on case/desk) | QMI8658 Tap engine | start/stop a Gemini Live turn (a hands-free alternative to the touch tap) |
| **Pick up / lift** | Any-Motion + orientation | wake from sleep → show face → arm listening |
| **Set down / still** | No-Motion (e.g. 30 s) | dim → ambient clock → light/deep sleep |
| **Flip face-down** | accel Z sign | **privacy mute** — physically silence Jarvis (a great product moment) |
| **Shake** | Significant-Motion | **dismiss / cancel** the current turn or notification |
| **Tilt** | orientation | scroll menus without touching the screen (§4) |

### 1.2 Enabling it (firmware)
1. The QMI8658 is **not declared in `board_devices.yaml`** at all — so this is an *add*, not an
   un-skip. Either add a device entry (custom type, addr `0x6A`/`0x6B`, shared `i2c_master`) or,
   simplest and consistent with `jarvis_pmic`, drive it from a small `jarvis_imu` component that
   borrows the shared bus handle and does raw I²C — interrupt-only, no polling task.
2. Configure the Tap + Wake-on-Motion + No-Motion engines; route them to **INT2**.
3. **INT2 is wired to `GPIO21`** (direct ESP32-S3, RTC-capable) — verified from the schematic (§7).
   Attach the ISR there; the *same* line doubles as the EXT0 deep-sleep wake source (§2). (INT1 is
   on the expander `EXIO6` — avoid it for the ISR path.) Confirm the strap address by probing
   `WHO_AM_I`==`0x05`.
4. ISR sets a FreeRTOS event bit; a task reads the status register **once per interrupt**.
5. Surface gestures as **Lua events** (`on_double_tap`, `on_flip`, `on_shake`) so new behaviors
   drop onto the SD card with no reflash — consistent with the existing brain model.
4. ISR sets a FreeRTOS event bit; a task reads the status register **once per interrupt**.
5. Surface gestures as **Lua events** (`on_double_tap`, `on_flip`, `on_shake`) so new behaviors
   drop onto the SD card with no reflash — consistent with the existing brain model.

```c
// Interrupt: do NOTHING on the I2C bus here — just signal the task.
static void IRAM_ATTR qmi8658_int_isr(void *arg) {
    BaseType_t hp = pdFALSE;
    xEventGroupSetBitsFromISR(s_imu_events, IMU_INT_BIT, &hp);
    if (hp) portYIELD_FROM_ISR();
}

// Task: runs only when the INT fired, reads the status reg once, fans out.
static void imu_task(void *arg) {
    for (;;) {
        xEventGroupWaitBits(s_imu_events, IMU_INT_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        uint8_t st = qmi8658_read_reg(QMI8658_REG_STATUS1);   // single I2C transaction
        if (st & QMI8658_INT_TAP) {
            uint8_t tap = qmi8658_read_reg(QMI8658_REG_TAP_STATUS);
            lua_emit_event(tap & TAP_DOUBLE ? "on_double_tap" : "on_single_tap");
        }
        if (st & QMI8658_INT_ANYMOTION)  app_on_motion();
        if (st & QMI8658_INT_NOMOTION)   app_schedule_idle_dim();
        if (st & QMI8658_INT_SIGMOTION)  lua_emit_event("on_shake");
        // Flip: read accel once, check Z sign (cheap, only on INT)
        int16_t az = qmi8658_read_accel_z();
        if (az < FLIP_THRESHOLD) app_set_privacy_mute(true);
    }
}
```

> **Tap config tip:** the QMI8658 tap engine exposes peak/window/quiet thresholds. Tune for a
> *case* tap (firmer, lower sensitivity) vs a *desk* tap (sharper) so double-tap is reliable
> without false-firing on bumps.

---

## 2. AXP2101 PMIC — battery, power button, sleep

The AXP2101 is a full power-management IC. Today it's `init_skip: true` *on purpose* — the factory
defaults already bring up the display and audio rails. So the work is **add monitoring + sleep
control without disturbing the working power-on defaults**: initialize it read/configure-only,
don't re-sequence the rails that are already correct.

### 2.1 Battery monitoring (✅ IMPLEMENTED — `jarvis_pmic` component, 2026-06-11)
> **Status:** done and durable. The non-durable in-tree reader was replaced by the git-tracked
> `firmware/components/jarvis_pmic` component (mirrored into the build by `copy_jarvis_pmic`); the
> `/api/battery` handler is rewired to it by `apply_battery_real_read_patch` in `bootstrap.sh`. The
> PMIC stays `init_skip: true` — `jarvis_pmic` only reads + flips the measurement/detect bits below,
> never a power rail. Charge state is read from `STATUS2[7:5]` (not the old `&0x07`).
>
> Earlier symptom (now fixed): with the ADC + battery-detect blocks left off (PMIC `init_skip`),
> VBAT read `~0x3FFD` garbage and `pct` read 0. The fix enables those blocks once on init.
- **Enable first (the actual fix):** ADC channels via reg `0x30` (bit0 VBAT, bit2 VBUS, bit3 VSYS)
  and battery presence via reg `0x68[0]`. Without these the reads below return full-scale garbage.
- **Fuel gauge:** battery % (reg `0xA4`), voltage (reg `0x34/0x35`, 14-bit, `((h&0x3F)<<8)|l` = mV).
- **14-bit SAR ADC:** BAT / VBUS / VSYS voltages → real "% + charging? + source".
- **Low-battery IRQ:** set the warning level (reg `0x1A`); the PMIC pulls IRQ when below → drive
  a low-batt face/LED state from the interrupt instead of polling.

```c
typedef struct { uint8_t percent; uint16_t mv; bool charging; bool usb_present; } batt_t;

batt_t axp2101_read_battery(void) {
    batt_t b = {0};
    b.percent     = axp2101_read_reg(AXP2101_REG_BAT_PERCENT);     // 0xA4
    b.mv          = axp2101_read_adc(AXP2101_ADC_BAT_VOLTAGE);     // 0x34/0x35
    uint8_t st    = axp2101_read_reg(AXP2101_REG_STATUS);
    b.charging    = st & AXP2101_STAT_CHARGING;
    b.usb_present = st & AXP2101_STAT_VBUS_GOOD;
    return b;
}
```
Wire this into the existing `/api/battery` endpoint, the dashboard, and a screen widget (§4).

### 2.2 Power button (POK)
The AXP2101 POK pin gives **short-press** and **long-press** events over IRQ for free — a real
hardware button. Map: short-press → wake / sleep toggle or open menu; long-press → power off
(`axp2101_power_off()`). Route the AXP IRQ to an RTC GPIO so it can also wake from deep sleep.

### 2.3 Switchable rails = your sleep lever
The AXP2101 has 4 DC-DC + 11 LDO rails. During sleep, **cut the rails** to the AMOLED backlight and
audio codecs so *board* draw drops, not just chip draw. Restore them on wake. (Leave the rails that
the factory default powers for boot alone unless you're deliberately sleeping.)

```c
void rails_for_sleep(bool sleeping) {
    axp2101_set_rail(AXP2101_RAIL_AMOLED, !sleeping);   // backlight / panel
    axp2101_set_rail(AXP2101_RAIL_AUDIO,  !sleeping);   // ES8311 + ES7210
    // keep PSRAM + RTC + IMU rails up so wake sources survive
}
```

### 2.4 Sleep modes & the power state machine
ESP32-S3 sleep (chip-ideal; real draw is higher with PSRAM/AMOLED, which is why §2.3 matters):

| Mode | ~Current | Retains | Wake |
|---|---|---|---|
| Modem-sleep | 20–30 mA | CPU on, WiFi radio dozes between beacons | instant |
| **Light-sleep** | ~240 µA | RAM + peripherals, **resumes in place** | ~1 ms |
| **Deep-sleep (RTC on)** | ~7 µA | RTC + RTC-mem + ULP only | reboot from RTC mem |

**Wake sources available here:** timer; **EXT0** (1 RTC GPIO — QMI8658 INT → wake-on-motion);
EXT1 (multiple — IMU **or** AXP button); GPIO (light-sleep, any pin); touch; ULP.

There are **four power modes**, each mapped to a named *experience* (full design in §4.3):

```
┌─ AWAKE ── full power · AMOLED bright · mic hot · WiFi+Gemini live ───────────┐  ~80–250 mA
│     ▲ "Hey Jarvis" / double-tap / touch / pick-up                            │
│     │            ▼ end of turn OR No-Motion 30 s                             │
├─ AMBIENT ── dim watch face on black · wake-word listening · WiFi off ────────┤  ~tens mA
│     ▲ tap / pick-up            ▼ No-Motion 2 min                             │
├─ WHISPER ── screen OFF (true black) · wake-word + tap only · WiFi off ───────┤  ~tens mA (mic only)
│     ▲ "Hey Jarvis" / tap       ▼ No-Motion 10 min OR long-press             │
└─ DREAM ── deep sleep · AMOLED+audio+mic off · only IMU/button/RTC armed ─────┘  ~µA
      ▲ wake: QMI8658 motion INT (EXT0) · AXP button (EXT1) · RTC timer
```

This is the heart of "not always running full blast": **WiFi and the AMOLED only come up when
there's a reason.** Note the honest power floor — **AMBIENT and WHISPER both run the wake-word
mic (tens of mA); only DREAM is µA-class.** The big saving between AMBIENT and WHISPER is the
**emissive AMOLED**: black pixels are physically *off*, so "screen off" is genuinely free and a
dim face on black is cheap. The Gemini Live flow is untouched — it just starts from a wake event.

```c
void enter_deep_sleep(void) {
    rails_for_sleep(true);
    qmi8658_enable_wake_on_motion(/*threshold_mg=*/200);          // INT toggles on motion
    esp_sleep_enable_ext0_wakeup(QMI8658_INT_GPIO, 1);            // motion → wake  (pin from schematic)
    esp_sleep_enable_ext1_wakeup(1ULL << AXP_IRQ_GPIO, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_sleep_enable_timer_wakeup(30ULL * 1000000);              // refresh clock every 30 s
    esp_deep_sleep_start();
}
```

---

## 3. Wake word — "Hey Jarvis" on-device (esp-sr WakeNet)

The point of an on-device wake word: **wake from low-power listening into the WiFi Gemini Live
session without touching WiFi until needed.** The device idles cheap; "Hey Jarvis" brings up WiFi
and starts the existing Gemini turn. No architecture change — just a new *entry point* to the
current flow (alongside touch tap and double-tap).

### 3.1 Use esp-sr WakeNet (native, free, AEC-aware)
This board is ideal for it: the **ES7210 gives a 2-mic array with hardware AEC**, which is exactly
what Espressif's AFE wants (it's Amazon-Alexa-qualified).

- **WakeNet9 / 9s:** always-listening detector, low memory, runs inside the AFE.
- **Custom "Hey Jarvis":** trainable via Espressif's TTS-sample pipeline (no recording thousands
  of samples). English is supported by TTS Pipeline V3.
- **AFE bonus features** you get for free: AEC (echo cancel using the speaker reference), VAD/VADNet
  (endpointing — already used in the current hands-free flow), NS (noise suppression), and DOA
  (direction of arrival → "which side spoke," usable for the face in §4).

> **WakeNet over Picovoice Porcupine on this silicon:** Porcupine's microcontroller SDK targets
> **Arm Cortex-M**; the ESP32-S3 is **Xtensa**, and Picovoice's ESP32 support isn't a current
> first-class target (and needs an AccessKey). WakeNet is native, free, and reuses the on-board
> AEC. (openWakeWord is far too slow on the S3 — seconds per frame.) Keep Porcupine only as a
> fallback if you ever change silicon.

### 3.2 Integration with the existing Gemini Live flow
```c
// Wake-word task feeds the AFE; on detection, it triggers the SAME path tap-to-talk uses today.
static void wakeword_task(void *arg) {
    afe_data_t *afe = afe_create(/*2-mic + AEC config*/);
    for (;;) {
        afe->feed(afe, i2s_read_frame());                 // mic frames (TX co-enabled per I2S rule)
        afe_fetch_result_t *r = afe->fetch(afe);
        if (r && r->wakeup_state == WAKENET_DETECTED) {
            emote_face_set_state(EMOTE_LISTENING);
            cap_gemini_live_start();                      // <-- existing WiFi-direct Gemini entry point
        }
    }
}
```

### 3.3 Settings
Expose **"Always listening" vs "Lift / tap to listen"** as a user setting (NVS / `POST /api/config`)
— the privacy-conscious default can be tap/lift, with always-on opt-in.

### 3.4 Power posture — motion gates the mic
WakeNet listening is **not** µA-class (mic + DMA + a small NN ≈ tens of mA), and the I²S
shared-clock rule means the mic path keeps I²S running. So the right design is **two-stage**:

```
deep sleep (~µA) ──[QMI8658 motion / pick-up]──► WakeNet listening (tens of mA) ──["Hey Jarvis"]──► WiFi + Gemini
```
On the desk and untouched → deep sleep, mic off. Pick it up → it starts listening. This gives
tap-to-listen battery life with hands-free feel once it's in your hand.

---

## 4. Interaction Design — a tiny round OS with personality

> The 466×466 round AMOLED is the soul of this product. Treat it like a **smartwatch OS with a
> character living inside it** — circular by nature, animated by default, and *acknowledging every
> interaction*. Nothing static, nothing rectangular, nothing silent. This section is the design
> brief; §4.1 keeps it honest about what the engine can actually render.

### 4.1 Rendering reality — what the engine *can* do (read this first)
Earlier drafts assumed a runtime pixel canvas. **That path is dead and proven dead on hardware:**
`esp_emote_gfx` has **no `gfx_canvas`**; feeding a runtime CPU-drawn buffer to `gfx_img_set_src`
renders garbage (`WAVEFORM_ENABLED = 0`; see [`docs/reference/display-emote-gfx.md`](../reference/display-emote-gfx.md)).
So the whole interaction language below is built from the **sanctioned, flash-baked toolset** —
which, used well, is plenty:

| Tool | What it gives the UI | Interaction use |
|---|---|---|
| `gfx_anim` (baked `.aaf`) | frame sequences; drive `fps`/segment at runtime | canned transitions, personality loops, orbital spinners |
| **`gfx_motion`** (affine) | **runtime rotate / scale / translate of a baked image** | the workhorse: **rotate** a baked ring = orbital; **scale** a baked mask = iris/bloom; **translate** = card slide; rotate a baked hand = analog clock |
| `gfx_motion_scene` | compose several moving baked objects | orbiting status pips, multi-element wake bloom |
| `gfx_button` | baked image + **native touch hit-test** | tap targets without a hand-rolled poller |
| `gfx_label` | text | clock digits, captions, menu labels |
| `gfx_qrcode` | QR | on-device WiFi/pairing |
| `gfx_mesh_img` | mesh-warp a baked image | "breathing"/jelly micro-deforms |

**Two render engines, one panel (arbitrated):**
- **emote_gfx** → the **personality face + canned transitions** (baked `.aaf` + `gfx_motion`). Keep
  the tuned voice-state face here.
- **LVGL as a second display owner** → the **interactive UI** (menus, cards, radial dial, Q&A
  choices). LVGL has native `arc`, `roller` (a literal rotary), gesture/swipe events, and an
  animation timeline — and Waveshare's own `05_LVGL_WITH_RAM` demo hits **200–300 fps on this exact
  board**, so smoothness is not the bottleneck. Crib their bringup.
- The render pipeline is already tuned for this (2026-06-10): **-O2**, **30 fps engine**, PSRAM
  quarter-strip double-buffered DMA. Animate **regions, not full frames** — CO5300 QSPI bandwidth
  is the limit, so use `gfx_motion` tweens (cheap) over full-frame `gfx_anim` where possible.

### 4.2 The round-native design language
Lean into the circle. A vocabulary to reuse everywhere so the whole OS feels coherent:

- **Orbit** — status lives on the **rim**: a battery arc, a connection dot that travels the edge,
  notification pips that orbit in. (`gfx_motion` rotate of baked sprites.)
- **Iris / Bloom** — transitions open and close from the **center**: wake = an arc-reactor bloom
  outward; sleep = an iris closing inward to black. (`gfx_motion` scale of a baked ring/mask.)
- **Concentric rings** — nested meaning: outer ring = battery, middle ring = level/progress,
  center = face or content. The eye reads it at a glance.
- **Radial menu / rotary** — choices arranged on the circle, the active one parked at 12 o'clock;
  **spin the dial** with a swipe-arc or an IMU tilt (§1) — the bezel *is* the control.
- **Conversational compass** — DOA (§3) points a highlight arc toward whoever spoke; the face
  "turns" to you. The device feels *aware of the room*.
- **No corners, ever** — content sits in a safe circular inset; labels curve or center; the design
  never fights the bezel.

### 4.3 Four power modes = four moods (the experience ladder)
Each AXP2101 power state (§2.4) is a distinct *personality state*, with its own screen, sensors, and
**a deliberate transition animation** in and out. This is what makes power management feel like a
feature, not a battery-saver.

| Mode | Mood | Screen | Mic / WiFi | Wakes on |
|---|---|---|---|---|
| **AWAKE** | *Present* | full-bright waveform face, captions, choice arcs, DOA lean | mic hot · WiFi+Gemini live | — |
| **AMBIENT** | *At ease* | dim **watch face on black** — clock, battery rim, weather glyph, last-command | wake-word listening · WiFi off | "Hey Jarvis" / tap / pick-up |
| **WHISPER** | *Resting* | **off (true black)** — maybe a single breathing pixel-dim "pulse" every few s | wake-word + tap only · WiFi off | "Hey Jarvis" / tap |
| **DREAM** | *Asleep* | off | everything off; only IMU/button/RTC armed | double-tap / button / motion |

The transitions are the magic:
- **DREAM → AWAKE** (you pick it up / double-tap): a quick **boot bloom** — a point of light blooms
  from center into the arc-reactor ring, the face "inhales," WiFi-connecting shown as a single dot
  **orbiting** the rim until Gemini is live. Optional voice: *"Good morning, Sir."*
- **AWAKE → AMBIENT** (turn ends / set down): the waveform bars **settle** into a calm breathing
  line, the line **dissolves** into the clock face, brightness eases down over ~1.2 s. Feels like
  exhaling.
- **AMBIENT → WHISPER** (No-Motion ~2 min): the clock **fades**, one slow **blink**, then an
  **iris closes** inward to black. The character is going to sleep, gracefully.
- **WHISPER → AMBIENT** (tap / pick-up): **iris opens** from the touch point, clock fades in.
- **→ DREAM** (long stillness / long-press): a final slow "goodnight" **pulse** dims to nothing.

All of these are `gfx_motion` scale/translate of one or two baked assets (iris ring, face disc,
orbit dot) — cheap, region-only, 30 fps. No per-frame baking of whole screens.

### 4.4 State-driven UI (the screen always reflects what Jarvis is doing)
Content is a **function of agent state** — the user never wonders what's happening:

| State | What the round screen shows |
|---|---|
| IDLE / AMBIENT | watch face + battery rim + a soft idle breath |
| LISTENING | waveform reacting to mic RMS · a **rim ring that drains** as the listen window counts down · "tap to stop" |
| THINKING | a single dot **orbiting** the center (round-native spinner) over a dimmed face |
| SPEAKING | waveform on output RMS · live **caption** in the lower arc · face **leans toward you** (DOA) |
| ASKING | **choice arcs** (see §4.5) — the interactive moment |
| ERROR / no-WiFi | a red rim **shudder** + a plain-language line ("can't reach the network") |

### 4.5 Interactive patterns (the part Pascal wants most)
1. **Tap-to-answer choice arcs.** When Gemini asks a yes/no or multiple-choice question, render the
   options as **arc-segment buttons hugging the rim** — 2 choices = left/right hemispheres, 3 =
   thirds, 4 = quadrants. Huge, glanceable, round-native touch targets (`gfx_button` or LVGL arc
   buttons). Tap an arc to answer; the chosen arc **fills and pulls to center** as confirmation.
   This turns the device into a true two-way interface, not just a mic.
2. **Swipeable card stack.** Notifications, weather, timers, status = a **deck of round cards**.
   Swipe left/right (touch Δx from the single owner) to flip with momentum; **swipe up to dismiss**;
   cards `gfx_motion`-translate with a slight scale on the incoming card. A tactile, phone-familiar
   gesture on a tiny screen.
3. **Radial dial menu.** Settings as a **rotary**: items on a circle, spin by swipe-arc **or IMU
   tilt** (turn the device like a knob), tap center to select. Theme · voice on/off · **wake-word
   mode** (§3.3) · brightness · **WiFi QR** (`gfx_qrcode`). The active item sits at 12 o'clock and
   scales up.
4. **Pull-from-rim shade.** Swipe inward from the top bezel → a quick **control shade** (brightness,
   mute, DND, battery) — the round analog of a phone's control center.
5. **Micro-feedback on *every* touch.** A **ripple** radiates from the exact touch point + a haptic
   tick (§5) + a subtle scale-bounce of the tapped element. Wake = bloom; confirm = arc fill; reject
   = shudder. The device should never feel like it ignored you.

### 4.6 Personality moments (the "special" layer)
Small, rare, delightful — never gimmicky:
- **Idle life:** occasional blink, a slow "look around," a stretch when first picked up after a long
  sleep.
- **Reacts to you:** turns toward your voice (DOA); a content settle when it recognizes "Hey
  Jarvis"; a little nod when it understood a command word (§5 MultiNet).
- **Reacts to handling:** **flip face-down → a wink, then sleep** (privacy mute, §1); **shake →** a
  dizzy wobble before it cancels.
- **Reacts to its body:** a **tired droop** + a single yawn animation at low battery; a happy
  "charging" sparkle on the rim when USB is plugged.
- **Time of day:** warmer palette + dimmer at night (RTC-scheduled, §4.7); a sunrise tint in the
  morning watch face.

### 4.7 Implementation notes
- **Gestures from the single touch owner.** Tap / double-tap / **long-press** / **swipe (Δ +
  direction)** / drag are all derived from the *one* CST9217 read loop — never add a second poller
  (I²C contention). Feed gesture events to both LVGL (when a menu owns the screen) and the face.
- **Asset/partition budget.** New baked `.aaf` sequences (transitions, personality loops) live in
  the `emote` flash partition (recently bumped 6 → 6.875 MB). Prefer `gfx_motion` tweens of a few
  reusable baked sprites over baking many full-screen sequences — saves flash and keeps it smooth.
  Pack via `esp_mmap_assets` (see [`docs/reference/asset-pipeline.md`](../reference/asset-pipeline.md));
  byte-swap with `image_converter.py --swap16` for the CO5300 QSPI panel.
- **Theming & brightness.** Palette + brightness in `identity.md` / `POST /api/config`; **night
  dimming on the RTC schedule** + AXP2101 backlight-rail control (no ambient-light sensor on board;
  a cheap I²C ALS is a later option — mind the shared bus).
- **Display arbiter.** emote_gfx and LVGL share the panel under the existing arbiter — one owner at
  a time; transition ownership on menu open/close so the face and the UI never fight for the bus.

---

## 5. Product polish

The difference between prototype and product is in the seams:

- **Boot & first-run.** Already boots straight into the IDLE face (good). Add: a clean splash, a
  first-boot WiFi-QR onboarding on-device, and a "ready" cue (chime + face settle).
- **Connection resilience.** Clear on-screen states for *connecting / no-WiFi / Gemini error /
  reconnecting* (not just a frozen face). Auto-retry with backoff; show it.
- **Feedback on every action.** Tap, wake, mute, error → a face transition + optional **haptic**
  tick (see note) + sound. Nothing should happen silently.
- **Battery UX.** Rim arc always available; low-batt toast + face state from the AXP IRQ; "charging"
  affordance when USB is in.
- **Idle dignity.** Ambient clock at rest, dim after inactivity, sleep on No-Motion — it should
  look intentional when idle, not just "on."
- **Privacy affordance.** Flip-to-mute (§1) + a visible "mic off" indicator on screen. People trust
  a device that *shows* when it isn't listening.
- **Settings that persist.** Theme, brightness, wake-word mode, voice on/off in NVS, surfaced in the
  on-device menu (§4.3) *and* the dashboard.
- **Haptics (optional new part).** The board has no haptic motor; a **PWM-driven LRA** (GPIO + small
  MOSFET, off the I²C bus) adds tactile confirmation cheaply. A DRV2605L (I²C) gives richer effects
  but adds bus load — prefer the PWM LRA unless you want expressive haptics.

---

## 6. Recommended build order

1. ✅ **AXP2101 battery monitoring** → real `/api/battery` (`jarvis_pmic`). *Done 2026-06-11.* Low-batt
   IRQ + dashboard rim widget still to add (IRQ is behind the expander `EXIO5` — poll the gauge, or
   move `EXIO5` to an input; see §7).
2. **QMI8658 enable** → tap/double-tap, wake-on-motion, flip-to-mute, shake. INT2 is on **`GPIO21`**
   (RTC-capable, §7) so the same line doubles as the EXT0 deep-sleep wake. *(Reordered ahead of the
   power state machine — the verified `GPIO21` wake pin unblocks DREAM-mode, so land the IMU first.)*
3. **Four-mode power state machine** (§2.4 + §4.3) → AWAKE / AMBIENT / WHISPER / DREAM, rails cut per
   mode. Wake sources: QMI8658 motion (EXT0/`GPIO21`), BOOT/Key1 (EXT1/`GPIO0`), RTC timer; PWR/Key2
   does hardware power-off/on via the AXP. Wire each transition to a `gfx_motion` animation.
4. **Ambient watch face + battery rim** (baked face + `gfx_motion` hands) → the AMBIENT-mode screen.
5. **Wake word** (esp-sr WakeNet + AEC), motion-gated → new entry point into the existing Gemini flow.
6. **LVGL second display owner** → interactive layer: choice arcs (§4.5), swipeable cards, radial
   dial menu, control shade. Arbitrate with the emote face.
7. **State-driven UI + transitions** (§4.4) → every agent state has its screen; every mode change
   has its animation. This is where it starts to feel like a product.
8. **Personality + polish pass** (§4.6, §5) → idle life, DOA look-at-you, haptics, connection states,
   privacy indicator, micro-feedback ripples.

Most behaviors can live as **Lua skills on the SD card** (no reflash), consistent with the existing
"thin durable body, evolving brain" design.

---

## 7. Hardware questions — RESOLVED from the schematic (2026-06-11)

Read directly off the official Waveshare schematic PDF
(`files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75/ESP32-S3-Touch-AMOLED-1.75.pdf`,
sheets "6/9-Axis", "AXP2101", "KEYS", "RTC"). Net labels confirmed against the known
anchors (SDA15/SCL14, touch INT GPIO11, etc.).

| Question | Answer (verified) | Source |
|---|---|---|
| **QMI8658 INT GPIO** | **INT2 → `GPIO21`** (a direct ESP32-S3 pin, **RTC-capable** → usable for hardware ISR *and* EXT0/EXT1 deep-sleep wake-on-motion). **INT1 → `EXIO6`** (TCA9554 expander, not directly wakeable). | schematic "6/9-Axis" net block `QMI_INT2 → GPIO21`, `QMI_INT1 → EXIO6` |
| **AXP2101 IRQ GPIO** | **IRQ → `EXIO5`** (TCA9554 expander — **NOT a direct ESP32 GPIO**, so the PMIC IRQ is *not* a usable deep-sleep wake source). Power button **PWRON → Key2** (silk-labeled "RST"); the AXP handles short/long-press + power on/off internally and raises IRQ over I²C. | schematic "AXP2101" (`IRQ → EXIO5`, `PWRON`), "KEYS" (`Key2 → PWRON`) |
| **QMI8658 I²C address strap** | CS tied to `VCC3V3` (I²C mode). Silk note says **`0x6B`**; Waveshare example code uses `0x6A`. **Probe `WHO_AM_I` (reg `0x00` == `0x05`) at both addresses** at init and latch the responder. | schematic "6/9-Axis" (`0X6B` annotation, `CS → VCC3V3`) |
| **AMOLED / audio rail IDs on AXP2101** | Rail table: **DCDC1→VCC3V3 (3.3V main)**, DCDC2→0.9V, DCDC3→1.2V, DCDC4→1.8V, DCDC5→NC; **ALDO1/2/3→3.3V (VL1/VL2/VL3)**, ALDO4→1.8V; BLDO1→1.2V, BLDO2→2.8V; CPUSLDO→1.2V. Exact panel-vs-codec channel mapping (which of VL1/2/3) still needs a continuity check before any sleep rail-cut. | schematic "AXP2101" left rail table |

**Extra wins found in the same pass:**
- **Key1 / BOOT → `GPIO0`** (RTC-capable) — a second directly-readable button, usable as a user input and an EXT1 deep-sleep wake.
- **PCF85063 RTC** at I²C **`0x51`**, INT → `EXIO3` (expander) — confirms the clock is reachable for the ambient watch face.
- **TCA9554 is configured all-OUTPUT today** (`board_devices.yaml`: `output_io_mask:[0..7]`, `input_io_mask:NULL`). To read the `EXIO3/5/6` INT lines (RTC / AXP / QMI-INT1) you must first move those pins to inputs. The clean interrupt path that avoids this entirely is **QMI_INT2 on the direct `GPIO21`**.

> **Net effect on the plan:** the IMU *can* be fully interrupt-driven (and deep-sleep
> wakeable) via `GPIO21` — the §1 "interrupt-driven, never polled" design holds. The AXP
> IRQ cannot be a deep-sleep wake (it's behind the expander), so DREAM-mode wake sources are
> **QMI8658 motion (EXT0/GPIO21)**, **BOOT/Key1 (EXT1/GPIO0)**, and the **RTC timer**; the
> PWR button works at the AXP hardware level for hard power-off/on.

---

## 8. Library / component reference

| Need | Component | Notes |
|---|---|---|
| Sleep / wake | ESP-IDF `esp_sleep_*`, `driver/rtc_io` | built-in |
| **Wake word + AEC + VAD + DOA** | **espressif/esp-sr** (WakeNet, AFE, VADNet) | native, free, matches ES7210 2-mic. Primary. |
| IMU | QMI8658 driver (`lahavg/QMI8658-Arduino-Library` as reference → port to IDF I²C) | hardware tap/motion engines |
| PMIC | AXP2101 (M5Stack CoreS3 driver as reference) | I²C `0x34`; fuel gauge, rails, POK IRQ |
| RTC | PCF85063 driver | clock + alarms |
| DSP / FFT visualizer | espressif/esp-dsp | accelerated FFT |
| **Interactive UI** | **LVGL** (2nd display owner) | native `arc` / `roller` / gesture / animation; 200–300 fps on this board (Waveshare `05_LVGL_WITH_RAM`). Choice arcs, cards, radial dial. |
| Personality face + transitions | espressif2022/esp_emote_gfx | **baked only** — `gfx_anim` (.aaf) + `gfx_motion` (affine). **No runtime canvas** (proven dead). |
| Baked animation packer | esp_mmap_assets (esp-iot-solution) + `image_converter.py --swap16` | builds `.aaf` sequences for the `emote` partition |
| Haptics (optional) | PWM LRA (off-bus) **or** DRV2605L (I²C) | LRA simplest |

---

## 9. Sources

**Project (local):** `boards/waveshare/esp32s3_touch_amoled_1_75/{board_info,board_devices,board_peripherals}.yaml`,
`docs/reference/display-emote-gfx.md` (**no-canvas constraint + sanctioned baked paths**),
`docs/reference/asset-pipeline.md`, `docs/HARDWARE.md`, `docs/DISPLAY_UX_PLAN.md`,
`docs/ANIMATION_OPTIMIZATION.md` (render pipeline tuning: -O2 / 30 fps / PSRAM strips),
`docs/GEMINI_LIVE_PLAN.md` (I²S shared-clock constraint), `docs/BRAIN_ARCHITECTURE.md`, `docs/ROADMAP.md`.

**Web:**
- ESP32-S3 Sleep Modes — https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/sleep_modes.html
- esp-sr (WakeNet / AFE / VADNet) — https://github.com/espressif/esp-sr · WakeNet docs — https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html
- QMI8658A datasheet — https://www.qstcorp.com/upload/pdf/202301/13-52-25%20QMI8658A%20Datasheet%20Rev%20A.pdf
- QMI8658 driver reference — https://github.com/lahavg/QMI8658-Arduino-Library
- AXP2101 datasheet — https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/K128%20CoreS3/AXP2101_Datasheet_V1.0_en.pdf
- AXP2101 driver reference — https://github.com/nanoframework/nanoframework.IoT.Device/blob/main/devices/Axp2101/README.md
- LVGL — https://docs.lvgl.io
- Picovoice Porcupine (fallback, Xtensa caveat) — https://github.com/Picovoice/porcupine
