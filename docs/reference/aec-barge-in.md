# AEC + Barge-In Design (Echo Cancellation, Hands-Free Interrupt)

**What it is** — Implementation-ready design for STABILITY_PLAN P3.3 (AEC bring-up) and
P3.4 (hands-free barge-in): the user speaks over the assistant and playback stops within
~500 ms, with no echo-triggered self-interruption. Built on esp-sr **v2.4.6** (direct
`esp_aec.h` API, ESP-IDF ≥5.0, compatible with our v5.5.1) and the ES7210's
schematic-verified hardware echo-reference channel. Synthesized 2026-06-12 from three
research passes (board/schematic, esp-sr, Gemini Live API); every claim cited below.

**How we use it here** — `cap_gemini_live` keeps the mic streaming during playback,
cancels the speaker echo against a hardware loopback reference (ES7210 **MIC3**), and
lets Gemini's server VAD (primary) or the local RMS VAD (fallback) interrupt playback via
the P3.1/P3.2 flush path. This document assumes the **post-sprint** architecture
(STABILITY_PLAN P2.1–P2.4, P3.1–P3.2: session task = decode/control, PCM playback ring +
dedicated feeder task, capture task decoupled from sender task, codec open all session).

> ⚠️ `cap_gemini_live.c` line numbers below were re-verified against `main` on
> 2026-06-12 but the file is being restructured by the stability sprint **right now** —
> anchor on the cited symbol names, not raw line numbers.

---

## Decision summary

| # | Decision | Choice |
|---|----------|--------|
| D1 | Echo reference source | **Hardware: ES7210 MIC3** (TDM lane index 2). Software playback-ring ref = fallback/diagnostic only. |
| D2 | Capture config | Open record dev `channel=4, bits=16, 16 kHz, channel_mask=0`; demux lanes in software. Never use `channel_mask` to pick lanes. |
| D3 | AEC engine | esp-sr **direct `esp_aec.h` API** (not the AFE framework), `AEC_MODE_FD_LOW_COST`, `filter_length=4`, `mic_num=1`, NLP `AEC_NLP_LEVEL_AGGR`. No models, no model partition, no internal task. |
| D4 | Pipeline placement | `aec_process()` runs synchronously inside the post-P2.3 **capture task**, on 32 ms / 512-sample frames; cleaned mono goes to the bounded sender queue. |
| D5 | Interruption | **Hybrid**: local VAD on the AEC-cleaned mic kills playback instantly (P3.1/P3.2 flush primitive); Gemini **server VAD re-enabled** (auto mode) owns turn semantics + history truncation. Manual-mode `activityStart` path kept compiled as `GL_USE_SERVER_VAD=0` fallback. |
| D6 | Build | Pin `espressif/esp-sr: "^2.4.6"` in the **canonical** `firmware/components/cap_gemini_live/idf_component.yml`. No partition change, no mandatory sdkconfig change. |

---

## 1. Reference source: hardware MIC3 (D1)

### Schematic facts (all verified from the vendor PDF, page 1, read at 1200 dpi)

The board's `PA&SPEAKER&MIC` section contains an explicit **AEC sub-block**: ES8311
line-out `OUTP/OUTN` → differential attenuator (C75/C82 0.47 µF DC-block → R38/R45 10k →
R39/R46 20k series, R40 4.3k differential shunt [R41 NC/3.3k option], C79 10 nF + C81
2.2 nF + C80 100 pF filtering → C76/C83 0.22 µF) → nets `ADC_MIC3_P/N` → **ES7210 pins
31/32 (MIC3P/MIC3N)**. The chip's four inputs are therefore: **MIC1 = MEMS mic (pins
15/16), MIC2 = MEMS mic (pins 19/20), MIC3 = echo reference, MIC4 = unconnected**
(AC-terminated to AGND; its nets appear exactly once in the schematic text).
Source: `docs/reference/vendor/schematic/ESP32-S3-Touch-AMOLED-1.75-schematic.pdf` p.1.

The tap is **post-DAC-volume, pre-PA**: the same OUTP/OUTN nets feed both the AEC pad and
the NS4150B PA input network; PA enable is GPIO46 (`boards/waveshare/esp32s3_touch_amoled_1_75/board_peripherals.yaml:42-48`).
Consequences: (a) muting the PA does not remove the ref; (b) `esp_codec_dev_set_out_vol()`
scales ref and speaker identically — AEC linearity tracks volume changes; (c) the ref
inherently includes the `GL_OUT_GAIN`×4 + soft-knee limiter (`cap_gemini_live.c:110-111`,
applied before the DAC write) — exactly what the speaker emits.

### Why hardware ref beats the software playback-ring ref

| Property | HW MIC3 ref | SW ring ref |
|---|---|---|
| Ref↔echo skew | ~1 ms scale, **constant** (PA + acoustic flight ~0.1–0.3 ms + speaker/enclosure impulse tail) — ref and echo are digitized in the **same ES7210 TDM frame** | 0–90 ms **variable** (I2S TX DMA pool: `I2S_CHANNEL_DEFAULT_CONFIG` = 6 desc × 240 frames = 1440 frames = 90 ms @16 kHz, `periph_i2s.c:101`), plus ring-depth jitter + 0–20 ms RX read quantization |
| esp-sr alignment requirement | Trivially met | Violated unless tapped at the I2S-write boundary with an explicit measured fixed delay |
| Nonlinearities included | Yes (gain+limiter run pre-DAC) | Only if tapped post-limiter |

esp-sr's alignment window is **tight**: "the recording signal is delayed by around
0–10 ms compared to the corresponding reference (playback) signal"
([esp-adf algo-stream docs](https://docs.espressif.com/projects/esp-adf/en/latest/api-reference/streams/index.html),
fetch-verified 2026-06-12 research pass). The hardware ref satisfies this by
construction; the software ref needs a delay estimator spanning ~0–120 ms. **Decision:
MIC3 hardware ref primary; software ring ref retained only as a bring-up cross-check
and as the fallback if the loopback trace proves dead on hardware (see Risks R1).**

### Documentation error to fix (Phase 1)

`board_devices.yaml:65` labels `['NA','REF','MIC1','MIC2']` and
`docs/reference/audio-es8311-es7210.md:98-102` ("Channel 1 is the AEC reference") are
**wrong**. `adc_channel_mask: "0111"` parses as binary 0x7 → `ES7210_SEL_MIC1|MIC2|MIC3`
(`gen_board_device_config.c:118`; `es7210.c` mic-select). Per schematic the enabled set
is exactly right (2 mics + ref) but the ref is **MIC3 = TDM lane index 2 (zero-based)**,
not lane 1. Correct labels: `['MIC1','MIC2','REF','NC']`. The same doc page also
overstates the ES7210 as having "on-chip AEC" — it only **digitizes** the reference;
cancellation is esp-sr software (confirmed by [esp-bsp M5Stack Tab5 README](https://github.com/esp-bsp/m5stack_tab5)
usage of ES7210 as "AEC front end" — see Primary sources).

---

## 2. Capture/TDM config delta (D2)

Current state: ES7210 runs TDM (3 mics selected ≥ `ENABLE_TDM_MAX_NUM=3` → REG12=0x02
"Enable TDM mode", `es7210.c:16,185,230-231` — verified), but the app opens the record
dev **mono** (`GL_CHANNELS=1`, `cap_gemini_live.c:102`), and `audio_codec_data_i2s.c`
turns that into STD Philips / 16-bit slots / left-slot-only — only the **first** TDM lane
(MIC1) ever reaches the app. MIC2 and the MIC3 ref are clocked out and discarded.

### The change

Open the record codec once per session (post-P2.4, codec held open all session):

```c
esp_codec_dev_sample_info_t fs = {
    .sample_rate     = 16000,
    .channel         = 4,        /* all four TDM lanes */
    .bits_per_sample = 16,
    .channel_mask    = 0,        /* see warning below */
};
esp_codec_dev_open(s_gl.adc, &fs);
```

What happens inside (verified, `audio_codec_data_i2s.c` `set_drv_fs` STD branch): for
`channel > 2` the layer widens slots — `slot_bits = 16*4/2 = 32`, `active_channel = 2`,
both slots — so each I2S frame carries 4×16-bit samples packed as 2×32-bit STD slots:
**`[MIC1][MIC2][MIC3=ref][MIC4=junk]`, 8 bytes/frame, 128 kB/s.** Expected lane order:
`[MIC1|MIC2]` in the WS-low slot, `[MIC3|MIC4]` in WS-high — **verify empirically with a
tone test before trusting the demux indices** (Phase 2; this is the one thing code
analysis cannot prove read-only). This 4-ch open is the canonical esp_codec_dev ES7210
pattern (its own test app opens `channel=4`: `test_apps/codec_dev_test/main/test_board.c:1164-1169`).

> ⚠️ **Never use `channel_mask` to hardware-select the mic+ref pair.** The STD branch
> clamps the mask to the 2 physical slots — verified:
> `slot_cfg.slot_mask = slot_mask & I2S_STD_SLOT_BOTH; // Only support 2 slots` —
> so requesting TDM lanes {0,2} (`0b0101 & 0b11 = 0b01`) collapses to "left slot only"
> and **silently drops the ref**. Read all 4 lanes; demux in software
> (`mic = lane 0` [optionally 0+1 averaged], `ref = lane 2`).

### Duplex-clock behavior

RX at 4×16 = 64 bits/frame forces BCLK = 64×fs while TX (mono 16-bit) was sized for 32.
`audio_codec_data_i2s.c` `check_fs_compatible` handles this by extending the peer's slot
bits (disable/reconfig/re-enable of the paired channel) and `set_fs` reconfigures the TX
clock when RX changes in full duplex. With P2.4's always-open codec, **do this once at
session start — open RX 4-ch first (or accept the automatic peer extension) — so the
shared 16 kHz clock never moves mid-session** (the clock-stability lesson of the
2026-06-10 resampler fix, `audio-es8311-es7210.md:43-65`). Untested on this board: see
Risks R5.

### Gain corrections (required for AEC linearity)

1. **The ref channel will clip at high volume without this.** `es7210_open`
   unconditionally applies +30 dB analog PGA to ALL enabled channels
   (`_es7210_set_channel_gain(codec, 0xF, 30.0)`, `es7210.c:457` — verified), and
   `cap_gemini_live` re-asserts `esp_codec_dev_set_in_gain(adc, 30.0f)`
   (`cap_gemini_live.c:827`, also all-channel). The AEC pad attenuates ≈ −23.5 dB
   (4.3k/(10k+20k+10k+20k+4.3k)), so the ref nets ≈ **+6.5 dB above ES8311 line-out
   level**. Fix with the per-channel API (exists:
   `esp_codec_dev_set_in_channel_gain`, hooked to `es7210_set_channel_gain` →
   MIC3 gain register REG45, `es7210.c:347-358,507-516,603` — verified):

   ```c
   /* mask bit2 = MIC3 = ref lane */
   esp_codec_dev_set_in_channel_gain(s_gl.adc, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2), 0.0f);
   /* keep 30 dB on the MEMS mics */
   esp_codec_dev_set_in_channel_gain(s_gl.adc,
       ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1), 30.0f);
   ```

   Target: ref peaks −3 to −5 dBFS at max volume (esp-sr mic design guide figure).
   Calibrate 0/3/6 dB in Phase 2.
2. **No AGC/ALC anywhere in the ES7210 driver** (full read of `es7210.c`: no ALC register
   writes; PGA is static) — capture is linear apart from clipping. Good for AEC.
3. **The digital 6× mic gain + 4:1 soft-knee** (`cap_gemini_live.c` ~`:1399-1405`) is
   nonlinear and must run **after** AEC, on the cleaned mic only — never on the ref,
   never pre-AEC.

---

## 3. AEC engine + pipeline insertion (D3, D4)

### Why the direct `esp_aec.h` API, not the AFE framework

- **No model files, no `model` partition** — AEC is purely algorithmic (all modes incl.
  the 2026-04 full-duplex FD modes). The AFE route also works model-free (production
  precedent: xiaozhi `afe_audio_processor.cc` passes `models=NULL`), but the direct API
  is strictly smaller. This matters: the 16 MB flash table is **completely full** — the
  just-landed layout ends flush at 0x1000000 with the 64 K coredump tail
  (`scripts/bootstrap.sh:2560-2612`, `coredump, data, coredump, 0xFF0000, 0x10000` —
  verified). Model partitions are a P4/WakeNet problem.
- **No internal task** — `aec_process()` runs synchronously in the caller, which slots
  exactly into the post-P2.3 capture task and keeps core/priority placement in our
  hands. (The AFE route spawns its own task via `afe_create()` with `afe_perferred_core`
  / `afe_perferred_priority` [sic] knobs — unnecessary indirection + an extra ringbuffer
  hop of latency.)
- **Mode**: `AEC_MODE_FD_LOW_COST` — FD modes add nonlinear residual suppression and are
  documented as "suitable for Full-Duplex dialogue scenarios", with FD_LOW_COST the
  recommended balance. SR_* modes are linear-only (wake/ASR front-ends); VOIP_* are for
  calls. NLP residual suppression (`AEC_NLP_LEVEL_AGGR`, default) is only effective in
  FD mode. ([esp-sr AEC README](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html),
  fetch-verified in research pass.)

### Creation

```c
#include "esp_aec.h"   /* esp-sr include/esp32s3/esp_aec.h */

aec_config_t cfg = {
    .mic_num       = 1,                      /* MIC1 only for v1; MIC2 deferred */
    .ref_num       = 1,
    .out_num       = 1,
    .filter_length = 4,                      /* documented recommended value */
    .sample_rate   = 16000,                  /* aec_create: "must be 16000" */
    .caps          = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,  /* see RAM budget */
    .mode          = AEC_MODE_FD_LOW_COST,
    .nlp_level     = AEC_NLP_LEVEL_AGGR,
};
aec_handle_t *aec = aec_create_from_config(&cfg);
int frame = aec_get_chunksize(aec);          /* 512 samples = 32 ms in FD/SR modes */
```

Per-frame: `aec_process(aec, mic, ref, out)` with **separate** (not interleaved) int16
buffers, **16-byte aligned** (`heap_caps_aligned_alloc`). 16 kHz is a hard requirement
and is exactly our bus rate — the shared I2S duplex clock already holds 16 kHz both ways
and the Gemini 24 kHz downlink is resampled on-device (`gl_resolve_playback_rate`,
`cap_gemini_live.c:560`; commit 66413b1) — **no extra resampling anywhere**.

### Pipeline (post-sprint shape)

```
ES7210 4-ch TDM read (512 frames × 8 B = 4096 B per 32 ms)
        │  capture task (P2.3), synchronous
        ├─ demux: mic = lane 0, ref = lane 2          (lane order verified Phase 2)
        ├─ aec_process(mic, ref, clean)               (~2.3–8 ms target, measure)
        ├─ 6× digital gain + soft-knee (clean only)   (moved from pre- to post-AEC)
        ├─ local VAD on clean mic
        │     ├─ LISTENING: turn-commit logic (constants re-expressed in frames)
        │     └─ SPEAKING:  barge-in detector → gl_interrupt_playback()
        └─ push 512-sample clean chunk → bounded send queue → sender task (P2.3)
```

- **Cadence change 20 ms → 32 ms**: read 512 frames per iteration instead of 320
  (`GL_TX_CHUNK_MS=20`, `cap_gemini_live.c:99`). VAD accumulator constants re-expressed
  in frames: `GL_VAD_MIN_SPEECH_MS` 240 → **8 frames = 256 ms**; silence 500 →
  **16 frames = 512 ms** (`GL_VAD_SPEECH_RMS`/`GL_VAD_MIN_SPEECH_MS` at
  `cap_gemini_live.c:155,163`). End-of-speech commit shifts ~30–80 ms later; the
  server-side 700 ms `silenceDurationMs` margin absorbs this. **Acceptance: turn-commit
  behavior unchanged in field test.**
- **Task placement**: capture task keeps its existing priority; pin to **core 1** (away
  from the Wi-Fi/lwIP core); stack +4 KB for the AEC call. AEC adds no task of its own.
- **VAD threshold split**: keep `GL_VAD_SPEECH_RMS 1000` for LISTENING turn-commit; add
  a separate `GL_BARGE_RMS` (calibrate in Phase 4 — residual echo raises the floor
  during playback) with a short latch (~3 frames ≈ 96 ms) for the SPEAKING barge
  detector.

### RAM/CPU budget (do not re-break P2.2)

| Item | Internal | PSRAM | Notes |
|---|---|---|---|
| AEC FD_LOW_COST state | ~30.9 KB | ~90.0 KB | official table; prefer `caps=MALLOC_CAP_INTERNAL` for working state if headroom (min_free_heap floor was ~192 KB), else PSRAM |
| Frame buffers (mic/ref/clean ×512×2 B, aligned) | 3 KB | — | |
| 4-ch read buffer (512×8 B) | — | 4 KB | |
| **Total** | **~34 KB** | **~94 KB** | <10 % of the ~1 MB post-P2.2 PSRAM headroom |

**Explicitly NOT budgeted: AFE + vadnet/wakenet models (740–814 KB PSRAM + 49–60 KB
internal)** — that would consume essentially all remaining PSRAM and is not needed for
P3.3/P3.4. CPU: official table says 19.6 % of one 240 MHz core for FD_LOW_COST
(2.29 ms→… per 32 ms frame class) — but the benchmark assumes 64 KB data cache while
this build runs 32 KB data cache + XIP-from-PSRAM + ~13 MB/s display PSRAM traffic, so
**expect higher; measure on device with a per-frame timer, gate at <10 ms/frame**
(Risks R3).

---

## 4. Interruption strategy (D5)

### Why server VAD gets a second chance (history, verified from git)

- ae0ed99 (2026-05-25) enabled server VAD; 4639cbb (2026-05-26) disabled it
  ("on Waveshare hardware the server never returns serverContent... mic looks alive,
  zero reply"); babc381 (2026-05-28) built local VAD instead.
- **Root cause was mic level, not echo, and it is already fixed but never retested**:
  the SAME commit that disabled server VAD (4639cbb) added the 6× digital gain, with the
  author's own diagnosis in the comment — raw conversation RMS 50–300 (≈ −50…−41 dBFS,
  effectively silence to a server VAD). `git log -S GL_USE_SERVER_VAD` shows the flag was
  only ever touched by those two commits: **server VAD never ran with the gained
  signal**. Echo is also ruled out for the original failure: capture was hard-paused
  during playback at the time, and the failure occurred on the *first* spoken turn before
  any model audio existed. (STABILITY_PLAN P3.4's "confounded by echo" hypothesis is
  therefore likely wrong about the original failure — but the retest stands, with the
  changed variable being gain.) babc381's hardware verification proved the gained stream
  is server-intelligible (manual `end_input` produced a full reply).
- Telemetry: `interrupted_hits` is wired (`cap_gemini_live.c:2118-2123,2789`) and reads
  **0 for the device's lifetime** — barge-in has never fired, because capture pauses
  during SPEAKING by design (`gl_enter_speaking` lineage, commit 7e70b3c).

### Primary: server `automaticActivityDetection` (auto mode, `GL_USE_SERVER_VAD=1`)

API facts (all from [https://ai.google.dev/api/live](https://ai.google.dev/api/live) and
[live-guide](https://ai.google.dev/gemini-api/docs/live-guide), fetch-verified in the
research pass):

- `realtimeInput` audio **may stream continuously during model output by design**
  ("Can be sent continuously without interruption to model generation"); barge-in is a
  documented key feature. With auto VAD + default `activityHandling:
  START_OF_ACTIVITY_INTERRUPTS`, detected speech alone triggers the server-side
  interruption — no client trigger message exists or is needed.
- On interruption the server sends `{"serverContent":{"interrupted":true}}` ("a good
  signal to stop and empty the current playback queue"), **stops generating** (turn ends
  via interrupted→turnComplete with NO `generationComplete`), discards pending tool
  calls and sends `toolCallCancellation` with their ids.
- Mutual exclusivity: `activityStart`/`activityEnd` are **forbidden** in auto mode (the
  firmware already models this — `gl_send_activity_start/end` no-op when
  `GL_USE_SERVER_VAD=1`, `cap_gemini_live.c:1031-1043`).
- Auto-mode obligation: any capture pause >1 s requires
  `{"realtimeInput":{"audioStreamEnd":true}}` to flush server-cached audio. The string
  is currently **absent from the codebase** (grep verified) — add it (Phase 5).

Exact session-config delta (the emitter already exists at `cap_gemini_live.c:961-967`;
change `prefixPaddingMs` 100→200 and add explicit `activityHandling`):

```json
"realtimeInputConfig": {
  "automaticActivityDetection": {
    "disabled": false,
    "startOfSpeechSensitivity": "START_SENSITIVITY_HIGH",
    "endOfSpeechSensitivity": "END_SENSITIVITY_LOW",
    "prefixPaddingMs": 200,
    "silenceDurationMs": 700
  },
  "activityHandling": "START_OF_ACTIVITY_INTERRUPTS"
}
```

Tuning rule: `prefixPaddingMs` is the anti-false-positive guard against residual echo
(raise toward 300 if self-interruptions appear) but adds directly to server barge-in
latency (lower toward 100 if the 500 ms gate fails) — the hybrid below makes this knob
non-critical for perceived latency. `silenceDurationMs` 700 matches the guide's
500–800 ms recommendation and the previously tuned value.

### The hybrid (how 500 ms is actually met)

**Playback stop is local; conversation truncation is server-side.** Both VAD paths
converge on one primitive — the P3.1/P3.2 flush path:

```
gl_interrupt_playback():
    flush PCM playback ring          (P2.x ring)
    signal feeder-task abort         (stop feeding I2S; optionally write one
                                      silence block to cut the ≤90 ms DMA tail)
    state → LISTENING
    s_gl.interrupted-side bookkeeping
```

- **Auto mode (primary)**: during SPEAKING, the barge detector (local RMS VAD on the
  AEC-cleaned mic, ~3-frame latch) calls `gl_interrupt_playback()` immediately — do NOT
  wait for the server. Continuous streaming means the server independently detects the
  same speech, cancels generation, truncates history, and sends `interrupted` —
  treated as confirmation (increment `interrupted_hits`, honor `toolCallCancellation`).
  No `activityStart` is sent (forbidden in auto mode).
- **Manual mode (fallback, `GL_USE_SERVER_VAD=0`)**: same local detector; on trigger:
  (1) `gl_interrupt_playback()`, (2) send `activityStart` — documented as a first-class
  client-side interrupt ("start of activity will interrupt the model's response (also
  called 'barge in')") — and begin streaming the cleaned mic, (3) treat any
  `interrupted` frame as confirmation. This is STABILITY_PLAN P3.1 with local VAD
  replacing the tap. Design must NOT depend on the `interrupted` frame arriving (if the
  model already finished generating and the device is only draining buffer, there is
  nothing to interrupt server-side).
- **P3.2 prerequisite (hard)**: today's `interrupted` handler calls
  `gl_resume_listening("interrupted")` (`cap_gemini_live.c:2123`) but does **not** flush
  queued audio — the backlog re-enters SPEAKING and plays anyway. The flush primitive
  must land first.

### Latency budget vs the 500 ms gate

| Path | Components | Total |
|---|---|---|
| Local kill (what the user perceives) | ≤32 ms frame fill + ~96 ms VAD latch (3 frames) + flush <10 ms + ≤90 ms I2S DMA tail (or ~32 ms if silence-stamped) | **~170–230 ms** ✅ |
| Server confirmation (history truncation) | prefixPadding 200 + server detection + Wi-Fi RTT (~150–400 ms) | ~350–700 ms (not user-perceived; playback already stopped) |

---

## 5. Build-system deltas (D6)

1. **Component pin** — add to the **canonical**
   `firmware/components/cap_gemini_live/idf_component.yml`
   (currently only `esp_websocket_client ^1.4.0` + `esp_codec_dev ~1.5` — verified):

   ```yaml
   espressif/esp-sr: "^2.4.6"
   ```

   Never edit the esp-claw copy — `bootstrap.sh copy_cap_gemini_live()`
   (`scripts/bootstrap.sh:64-80`) overwrites it every build. Minimum acceptable version
   **2.4.5** (2.4.3 added the FD AEC modes; 2.4.5 fixed S3 esp-dl coexistence + IDF
   v5.5.1/v5.5.2 issues; 2.4.6 is current). esp-sr's manifest requires only `idf >=5.0`
   — fine on v5.5.1; the standard caret constraint resolves under the project's
   `idf-component-manager==2.4.10` pin. Transitive deps: `esp-dsp 1.8.0`,
   `dl_fft >=0.2.0`, `cjson ^1.7.19`
   ([component registry](https://components.espressif.com/components/espressif/esp-sr) +
   [idf_component.yml](https://raw.githubusercontent.com/espressif/esp-sr/master/idf_component.yml),
   fetch-verified in research pass). Watch the cjson transitive dep vs IDF's bundled
   `json` component at first build (Risks R6).
2. **Partitions** — **no change**. Direct-API AEC needs no model files and no `model`
   partition. The current table (rewritten by `apply_emote_partition_resize_patch`,
   `scripts/bootstrap.sh:2560-2612`) is full to 0x1000000 including the 64 K coredump
   tail at 0xFF0000 — there is no room anyway. WakeNet/VADNet models (~1–3 MB) are a P4
   problem (candidates: shrink `storage` again, or esp-sr srmodel path on the SD card).
   Verify `ota_0` (4 MB) still fits the app after esp-sr+esp-dsp code is linked (R6).
3. **sdkconfig** — **no mandatory additions** for the direct AEC API (the esp-sr
   menuconfig model list only covers NS/VAD/WakeNet/MultiNet; leave unset). Known
   divergence to record, not change: the esp-sr benchmark assumes
   `CONFIG_ESP32S3_DATA_CACHE_64KB`+`LINE_64B`; this project builds 32 KB data cache +
   16 KB instruction cache (`esp-claw/application/edge_agent/sdkconfig:1705-1719`) —
   expect higher CPU than the published table. Do not flip cache geometry in this pass;
   measure first.

---

## 6. Phased implementation checklist

Preconditions (owned by the in-flight stability sprint, not this work): P2.2 (PSRAM
budget), P2.3 (capture/sender decouple), P2.4 (codec open all session — **also an AEC
prerequisite**: per-turn codec close/reopen would reset echo-path convergence every
turn), P3.1 (interrupt/flush primitive), P3.2 (interrupted flushes queued audio).

| Phase | Work | Acceptance criteria |
|---|---|---|
| 1 | Fix the channel-label docs: `board_devices.yaml:65` → `['MIC1','MIC2','REF','NC']`; correct `audio-es8311-es7210.md` (ref = MIC3/lane 2; ES7210 does not cancel echo on-chip) | Docs match schematic; no code change |
| 2 | 4-ch capture bring-up (no AEC yet): open `channel=4/16-bit/16 kHz`, demux lanes, per-channel gain (ref→0 dB, mics→30 dB). Tone test: play a known tone, record all 4 lanes to SD/WAV | Lane order confirmed (which lane carries the tone); ref level measured across volume: peaks −3…−5 dBFS at vol 100, **zero clipping**; mics unchanged vs today; shared clock stable (no `i2s_channel_disable` errors, playback pitch correct) |
| 3 | AEC integration: esp-sr pin, `aec_create_from_config` (FD_LOW_COST), 20→32 ms rechunk, move 6× gain post-AEC, re-express VAD constants in frames, per-frame compute timer | Echo visibly suppressed in `/api/audio/level` during playback (P3.3 criterion); `aec_process` < 10 ms/32 ms frame sustained with display active; turn-commit timing unchanged across 10 turns; PSRAM/internal deltas match §3 budget |
| 4 | Continuous capture during SPEAKING + local barge detector (`GL_BARGE_RMS` calibration) wired to `gl_interrupt_playback()`; manual-mode `activityStart` fallback complete | Speaking over her stops playback (measured onset→silence < 300 ms); **zero** self-interruptions over 10 turns at vol 100 with long replies |
| 5 | Server VAD re-enable (`GL_USE_SERVER_VAD=1`): config JSON of §4, `audioStreamEnd` on >1 s pause, `toolCallCancellation` honored, `interrupted` = confirmation; A/B vs Phase 4 fallback | `interrupted_hits` increments in the field for the first time ever; transcript shows history truncation at the barge point; THINKING face-flip heuristic re-validated under auto VAD |
| 6 | **User-level gate** | Speaking over her stops playback **< 500 ms hands-free**; **zero echo-triggered self-interruptions during a 10-turn test**; VAD turn-commit timing unchanged |

---

## 7. Risks, ranked

| # | Risk | Detection | Fallback |
|---|---|---|---|
| R1 | Ref demux index wrong, or the MIC3 loopback trace is dead/unpopulated on real hardware (schematic-verified, never captured) | Phase 2 tone test: which lane carries the speaker tone; correlation scan across all 4 lanes | Software ref: tap PCM at the **I2S-write boundary in the feeder task** (post-ring, post-limiter), apply a fixed measured delay ≈ TX DMA depth (~90 ms) to land in esp-sr's 0–10 ms window |
| R2 | Ref clipping at high volume (+6.5 dB net over line-out with default 30 dB PGA) breaks AEC convergence | Phase 2 peak meter on the ref lane at vol 100 | MIC3 per-channel gain 0 dB (then 3/6 dB only if under-driven); cap `set_out_vol` ceiling as last resort |
| R3 | CPU overrun: FD_LOW_COST's 19.6 % quoted on 64 KB-cache config; we run 32 KB cache + XIP-PSRAM + display DMA contention | Per-frame `aec_process` timer (Phase 3 gate <10 ms); `drops` counter; face-fps regression | `AEC_MODE_SR_HIGH_PERF` (linear-only, lighter) and lean on server-VAD robustness; move AEC buffers internal; reduce display fps during SPEAKING |
| R4 | Residual echo (AEC ≈ 20–30 dB suppression, not total) still trips server VAD → self-interruption | Phase 5 10-turn test: `interrupted_hits` with nobody speaking | Raise `prefixPaddingMs` → 300; `START_SENSITIVITY_LOW`; or ship manual mode (Phase 4) where local `GL_BARGE_RMS` threshold is fully ours |
| R5 | Duplex slot-extension (TX mono 16-bit + RX 4-ch/64-bit BCLK on the shared clock) misbehaves on this board-manager path (handled in code, untested here) | Phase 2: playback pitch test + I2S error logs at session start | Fix open order (RX before TX); or open TX with widened slots to match RX from the start |
| R6 | esp-sr link-time surprises: app overflows 4 MB `ota_0`; transitive `cjson` collides with IDF `json` | First Phase 3 build: size report + duplicate-symbol errors | Strip unused esp-sr features; `-Os` on the component; exclude its cjson in favor of IDF's |
| R7 | Server barge path misses 500 ms (prefixPadding + detection + RTT unpublished) | Phase 5 stopwatch: speech onset (local VAD timestamp) → `interrupted` receipt | Non-blocking by design: the hybrid's **local** kill (~170–230 ms) owns perceived latency; server timing only affects history truncation |
| R8 | 32 ms cadence shifts turn-commit feel | Phase 3 A/B: 10-turn conversation vs current build | Tune frame-count constants (8/16) ±1 frame; keep 500 ms server silence margin |

---

## 8. As-built barge-in (2026-06-13/14) — the working tuning

> Sections 1–7 are the **design**. This section is **what actually shipped** after
> on-hardware tuning. Where they differ, this section wins. All values here are
> baked defaults in `cap_gemini_live.c` **and** live-tunable via `/api/debug/gain`
> (no reflash) — read them back from `/api/gemini/live`.

### What changed vs the design

- **Manual mode, not server VAD (`GL_USE_SERVER_VAD=0`).** Server VAD was retested
  with the gained signal (as section 4 predicted it should be) and still failed on
  this user's hardware: normal-volume voice reads ~230 RMS at the mic — below
  Google's server-VAD speech floor — so the server heard silence and never replied
  (40 s of speech → 0 turns). The local RMS detector + manual `activityStart` is the
  shipped path. Server VAD remains compiled behind the flag for a future SNR effort.
- **Hands-free turn-commit thresholds (local VAD, LISTENING):** `GL_VAD_SPEECH_RMS
  150`, `GL_VAD_SILENCE_RMS 130`. The original 1000/500 never committed a turn for
  this user (no hands-free reply). The **silence** threshold was the actual bug — at
  80 it sat *below* this user's inter-word/pause level (~93–117), so a real
  end-of-turn pause never read as silence and the turn never ended. 130 sits above
  the pause level. Validated live (user spoke normally → THINKING → SPEAKING, no tap).

### The barge stop is fast because the capture task mutes directly (the "fast-kill")

The original "huge delay" on barge was **not** the detector — it fired fine
(`barge_hits` climbing). It was that `GL_CMD_INTERRUPT` queued behind PCM decode on
the busy session task, so the flush/mute lagged the detection by a variable
135–262 ms (the variance tracked ring fill — full ring = busy session task = slower
service). Fix (`s_playback_kill`):

- The **capture task** sets `s_playback_kill` and calls `gl_dac_mute(true)` **the
  instant the latch arms** — `esp_codec_dev_set_out_mute` is an I2C write to the
  ES8311 `DAC_REG31` mute bit, *in front of the DMA*, so the analog output goes
  silent in ~1 ms regardless of the ~90 ms still in the I2S DMA. Verified safe:
  `esp_codec_dev` shares **no lock** between `_write` (I2S data) and
  `_set_out_mute` (I2C control), so this never blocks behind the feeder's blocking
  write (adversarial-review + esp-codec source, 2026-06-14).
- The **feeder** honours `s_playback_kill` and stops feeding the DMA.
- The **session task** still does the full `gl_pcm_ring_flush` + `gl_drain_rx_queue`
  on the cmd; the flag is cleared on **every** exit path (interrupt-service,
  `gl_enter_speaking`, `gl_try_ws_resume`, `gl_reset_audio_path_state`) so it can
  never stick true and muzzle a later reply.
- Latch `GL_BARGE_LATCH_FRAMES = 2` (64 ms). Measured perceived stop: **~79 ms**,
  down from 135–262 ms and now *consistent* (no longer session-load-dependent).

### Self-barge: the guard + the adaptive gate (the hard part)

The echo is **usually tiny** (post-AEC `clean_rms` ~3–22) but throws **brief
transients up to ~382 RMS** during loud playback that the AEC can't cancel in real
time. With a fixed threshold these overlap the user's own talk-over level — no single
number separates "user" from "her echo." Two mechanisms fix it:

1. **Guard window `GL_BARGE_GUARD_MS = 500`** (was 200). The cold AEC needs
   ~300–500 ms to converge below the barge threshold; at 200 ms the convergence-window
   residual self-barged her own reply ~280 ms in. 500 ms covers it; genuine talk-over
   after she's a few words in still fires at ~79 ms.
2. **Adaptive (proportional) gate** — the effective threshold is
   `max(GL_BARGE_RMS floor, GL_BARGE_RATIO_PCT% × peak-held playback level)`:
   - Echo residual is **proportional to playback level**; the user's voice is
     **independent** of it. So the floor rises only as loud as her own playback
     demands (rejecting echo) and drops to the absolute floor in her pauses (a soft
     barge fires).
   - **Peak-hold over `GL_BARGE_PLAY_WIN = 4` frames (128 ms)** is essential: the
     echo at the mic *lags* the playback by the DAC/DMA + acoustic delay (~60–100 ms),
     so the instantaneous `s_out_rms` collapses while the echo tail is still loud
     (observed `mic=736` echo while `play` had already dropped to 141). A
     sliding-window max holds the floor up exactly long enough to cover the tail,
     then releases cleanly. (An exponential decay over-held — 25 % of a ~13000 peak
     stays high ~400 ms and blocks pause-barges.)

### The real reason barge "only worked if you shouted": SPEAKING mic gain

`GL_MIC_PGA_SPEAK_DB` was **9 dB** (dropped from the 24 dB LISTENING gain to keep the
echo from clipping the AEC). But that same −15 dB knocked the **user's** talk-over down
to ~30–50 RMS — below any safe floor — so a normal barge was undetectable and the user
had to shout (their shout measured 428). Raised to **18 dB**: the user's barge returns
to ~65–130 while the AEC stays healthy (measured atten 8–24 dB, no clipping at this
echo level). The louder echo is handled by the adaptive gate (which tracks playback),
**not** by starving the mic. Lesson: never fix an echo problem by throwing away the
mic gain that also carries the barge.

### Runtime tuning endpoints (no reflash)

`GET /api/debug/gain?…` — all live, read back from `/api/gemini/live`:

| Param | Sets | Diag field | Baked default |
|---|---|---|---|
| `mic=` / `ref=` / `vol=` | ES7210 mic PGA / ref PGA / ES8311 out vol | `mic_pga_db`/`ref_pga_db`/`out_vol` | 24 / 12 / 100 |
| `speak=` | SPEAKING mic PGA (barge sensitivity vs echo) | `speak_pga_db` | **18** |
| `barge=` | adaptive-gate absolute floor (0 = detector off) | `barge_rms_threshold` | **80** |
| `ratio=` | adaptive-gate % of peak playback (0 = floor only) | `barge_ratio_pct` | **10** |
| `guard=` | post-SPEAKING guard window (ms) | `barge_guard_ms` | **500** |
| `vadspeech=` / `vadsilence=` | hands-free turn-commit thresholds | `vad_speech_rms`/`vad_silence_rms` | **150 / 130** |
| `interrupt=` | `START_OF_ACTIVITY_INTERRUPTS` vs `NO_INTERRUPTION` (next session) | `activity_handling` | interrupts on |

Barge log line (every fire) prints the full decision:
`Barge-in: mic=<M> >= eff_thr=<E> (floor=<F> prop=<P> peak=<K> play=<V>) <n> frames`.

### Known hard limit (not a bug)

Barging during her **loudest** moments still requires a raised voice — the user's
voice must physically exceed the echo residual, and the adaptive floor is high there
by design. Pause-barges and normal-volume talk-over during normal-level speech work at
~79 ms. Closing the loud-playback gap needs better AEC (longer filter / ref alignment /
lower playback volume), a separate effort.

---

## Findings & gotchas

> Hard-won facts from the 2026-06-12 three-track research pass (schematic, esp-sr,
> Gemini API). Trust these over intuition.

**[2026-06-12] The echo reference is ES7210 MIC3 (lane 2), not lane 1 — the YAML labels are wrong**
Schematic AEC sub-block routes ES8311 OUTP/OUTN through a ~−23.5 dB differential pad
into ES7210 pins 31/32 (MIC3); MIC1/MIC2 are the MEMS mics, MIC4 floats.
`board_devices.yaml` labels `['NA','REF','MIC1','MIC2']` and the prior
`audio-es8311-es7210.md` claim "channel 1 is the AEC reference" mislabel it. An AEC
implementation demuxing lane 1 would feed a microphone as the reference and converge on
nothing.

**[2026-06-12] `channel_mask` silently drops TDM lanes in STD mode**
`audio_codec_data_i2s.c` clamps the mask to the two physical slots
(`slot_mask & I2S_STD_SLOT_BOTH`) — selecting lanes {0,2} collapses to "left slot only".
Open `channel=4` and demux in software; never mask.

**[2026-06-12] +30 dB PGA is applied to the ref lane too — it will clip**
`es7210_open` gains ALL enabled channels 30 dB (`es7210.c:457`) and the app re-asserts
it all-channel (`cap_gemini_live.c:827`). With the pad's −23.5 dB the ref nets +6.5 dB
over line-out. Per-channel gain (`esp_codec_dev_set_in_channel_gain`, mask bit2 → REG45)
is mandatory before AEC bring-up.

**[2026-06-12] "Server VAD broken on this board" was a level problem, already fixed, never retested**
The commit that disabled server VAD (4639cbb) is the same commit that added the 6× mic
gain whose comment diagnoses raw RMS 50–300 (≈ −50 dBFS — silence to a server VAD).
`git log -S GL_USE_SERVER_VAD`: the flag never ran with the gained signal. The original
failure also predates any playback (first-turn, capture-paused era) — echo was not the
confound.

**[2026-06-12] esp-sr AEC alignment window is 0–10 ms (mic after ref)**
Hardware MIC3 ref meets it by construction (same TDM frame). A software ring ref floats
0–90 ms behind the I2S TX DMA pool (6×240 frames) — usable only if tapped at the
I2S-write boundary with a fixed measured delay.

**[2026-06-12] Direct `esp_aec.h` needs no models, no partition, no task**
`aec_create_from_config` → `aec_get_chunksize` → synchronous `aec_process(mic, ref,
out)` on 16-byte-aligned non-interleaved int16 buffers. The flash table being full is a
non-issue for P3.3; it becomes the P4 WakeNet problem.

**[2026-06-12] In auto-VAD mode, `activityStart` is forbidden and `audioStreamEnd` is required on >1 s pauses**
The firmware already no-ops activity signals under `GL_USE_SERVER_VAD=1`
(`cap_gemini_live.c:1031-1043`) but has no `audioStreamEnd` anywhere (grep-verified).

**[2026-06-12] Today's `interrupted` handler does not flush — the backlog plays anyway**
`gl_resume_listening("interrupted")` (`cap_gemini_live.c:2123`) reopens listening but
queued playback re-enters SPEAKING. P3.2's flush is a hard prerequisite for any barge-in.

**[2026-06-14] The SPEAKING mic-gain drop (9 dB) starved the barge, not just the echo**
Dropping mic PGA to 9 dB during SPEAKING (to protect AEC linearity) also knocked the
*user's* talk-over to ~30–50 RMS — undetectable, so barge "only worked if you shouted."
Raised to 18 dB; the louder echo is rejected by the adaptive gate, not by starving the
mic. Never fix an echo problem with the gain that also carries the barge. See §8.

**[2026-06-14] Barge stop must be muted from the CAPTURE task, not via the cmd queue**
`GL_CMD_INTERRUPT` queues behind PCM decode on the busy session task → 135–262 ms
variable lag. `esp_codec_dev_set_out_mute` from the capture task at latch is ~1 ms (I2C,
in front of the DMA) and shares no lock with the feeder's `_write`. Perceived stop
dropped to ~79 ms, consistent. Clear the kill flag on every exit path. See §8.

**[2026-06-14] The echo lags playback ~60–100 ms — a proportional gate must peak-hold**
A barge floor proportional to the *instantaneous* playback level self-barges on the echo
tail (seen: `mic=736` echo while `play` already dropped to 141), because the echo at the
mic is from audio played ~one DMA-depth ago. Hold the playback max over ~4 frames
(128 ms) so the floor covers the tail, then releases into her pauses. See §8.

**[2026-06-14] Server VAD still can't hear this user even with gain — voice ~230 RMS**
Retested as §4 recommended: normal-volume voice (~230 RMS at the mic) is below Google's
server-VAD floor, so the server hears silence and never replies. Manual mode (local RMS
detector + `activityStart`) shipped. Server VAD stays behind `GL_USE_SERVER_VAD=0`.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `docs/reference/vendor/schematic/ESP32-S3-Touch-AMOLED-1.75-schematic.pdf` p.1 | AEC pad → ADC_MIC3_P/N → ES7210 pins 31/32; MIC1/MIC2 = MEMS; MIC4 floating; PA tap topology. Read at 1200 dpi, 2026-06-12. |
| `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:54-71` | `adc_channel_mask "0111"`, mislabeled `adc_channel_labels` (fix in Phase 1). |
| `firmware/components/cap_gemini_live/src/cap_gemini_live.c` (`:79,99-111,144,155,163,560,827,961-967,1031-1043,1399-1405,2118-2123,2789` as of 2026-06-12) | Model id, chunk/gain/VAD constants, server-VAD flag + config emitter, activity no-ops, 6× gain, interrupted handler, telemetry. **Mid-sprint — anchor on symbols.** |
| `esp-claw/.../managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c` (`set_drv_fs` STD branch; duplex `check_fs_compatible`/`set_fs`) | channel>2 slot widening; `slot_mask & I2S_STD_SLOT_BOTH` clamp; TX slot extension on RX reconfig. Verified locally 2026-06-12. |
| `esp-claw/.../espressif__esp_codec_dev/device/es7210/es7210.c:16,185,230-231,347-358,457,507-516,603` | TDM enable at ≥3 mics; REG45 MIC3 gain; all-channel 30 dB at open; per-channel gain API hookup. Verified locally. |
| `esp-claw/.../espressif__esp_codec_dev/test_apps/codec_dev_test/main/test_board.c:1164-1169` | Canonical 4-channel ES7210 open precedent. |
| `esp-claw/.../espressif__esp_board_manager/peripherals/periph_i2s/periph_i2s.c:101` | `I2S_CHANNEL_DEFAULT_CONFIG` → 6×240-frame TX DMA pool = 90 ms. |
| `scripts/bootstrap.sh:64-80,2560-2612` | Canonical-source copy rule; full 16 MB partition table incl. 64 K coredump tail. Verified locally. |
| `docs/STABILITY_PLAN.md` P2.1–P2.4, P3.1–P3.4 | Post-sprint architecture this design plugs into. |
| [esp-sr AEC README](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html) | Modes, NLP levels, resource table, frame sizes. Fetch-verified in 2026-06-12 research pass. |
| [esp_aec.h](https://raw.githubusercontent.com/espressif/esp-sr/master/include/esp32s3/esp_aec.h) | `aec_config_t`, 16 kHz requirement, buffer alignment, `filter_length=4` guidance. Fetch-verified in research pass. |
| [esp-sr CHANGELOG](https://raw.githubusercontent.com/espressif/esp-sr/master/CHANGELOG.md) | 2.4.3 FD modes; 2.4.5 S3/IDF-5.5.x fixes; 2.4.6 current. Fetch-verified in research pass. |
| [esp-sr on Component Registry](https://components.espressif.com/components/espressif/esp-sr) | Version, `idf >=5.0`, transitive deps. Fetch-verified in research pass. |
| [Gemini Live API reference](https://ai.google.dev/api/live) | `AutomaticActivityDetection`, `ActivityHandling`, `interrupted`, `toolCallCancellation`, full client/server message inventory. Fetch-verified in research pass. |
| [Gemini Live guide](https://ai.google.dev/gemini-api/docs/live-guide) | VAD parameter guidance (silence 500–800 ms), interruption handling example, `audioStreamEnd` rule, model comparison. Fetch-verified in research pass. |
| [esp-adf algo-stream docs](https://docs.espressif.com/projects/esp-adf/en/latest/api-reference/streams/index.html) | The 0–10 ms ref-alignment requirement + delay-measurement workflow. Fetch-verified in research pass. |
| [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (`box_audio_codec.cc`, `afe_audio_processor.cc`, `afe_wake_word.cc`) | ESP32-S3-BOX precedent: same codec pair, hardware-looped ref, model-free AFE+AEC. Fetch-verified in research pass. |
| [i2s_es7210_tdm IDF example](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/i2s/i2s_codec/i2s_es7210_tdm) | 4-slot ES7210 TDM capture reference. **(not fetch-verified)** |
| [esp-bsp M5Stack Tab5 README](https://github.com/espressif/esp-bsp/blob/master/bsp/m5stack_tab5/README.md) | ES7210 described as AEC front end (digitizer role). Fetch-verified in research pass. |

---

## Open questions

- **Lane order/aliveness (R1, blocking Phase 2→3)**: do the 4 demuxed 16-bit lanes
  arrive `[MIC1][MIC2][MIC3][MIC4]`, and does the MIC3 loopback trace actually carry the
  speaker signal? Five-minute tone test, impossible read-only.
- **Exact pad attenuation**: −23.5 dB computed from the legible 10k/20k/4.3k network;
  R37/R44 (2.2k) topology not fully legible in the schematic render. Confirm by
  measuring the captured ref level on-device (changes the gain pick, not the design).
- **ES8311 line-out full-scale vs ES7210 input full-scale at 0 dB PGA** — needed to pick
  MIC3 gain among 0/3/6 dB. ES7210 datasheet is absent from
  `docs/reference/vendor/datasheets` (per INDEX); extract from datasheets or just
  measure in Phase 2.
- **Real `aec_process` cost on THIS build config** (32 KB data cache, XIP-PSRAM, display
  DMA): benchmark table is indicative only (its own platform note self-contradicts —
  upstream doc bug). Gate at <10 ms/frame.
- **`filter_length=4` echo-tail coverage**: no official ms-per-unit mapping published; is
  it enough for this enclosure's speaker→mic coupling? Raise (with documented resource
  cost) if convergence is poor.
- **Does Gemini server VAD reject post-AEC residual echo** (20–30 dB suppression typical)
  at `START_SENSITIVITY_HIGH`? Phase 5 answers; knobs are sensitivity + prefixPadding.
- **Manual-mode `activityStart` during pure client-side buffer drain** (model turn
  already complete): server behavior undocumented — design already avoids depending on
  an `interrupted` reply.
- **Duplex slot extension in practice**: `check_fs_compatible` handles TX-mono +
  RX-4ch in code, but it is untested on this board's board-manager YAML path; does open
  order matter?
- **`inputAudioTranscription` "finished" heuristic under auto VAD**: the transcription
  stream is unordered relative to other server messages — re-validate the THINKING
  face-flip in Phase 5.
- **P4/WakeNet model storage**: flash is full; SD-card srmodel loading latency/wear vs
  shrinking `storage` again — out of scope here, decide at the P4 milestone.
- No official Google guidance on client-side AEC requirements was found in the Live API
  docs — the echo/self-barge-in risk is engineering inference, not documented API
  constraint. If a cookbook page exists, it was not located in this pass.

---

## See also

- [audio-es8311-es7210.md](./audio-es8311-es7210.md) — codec chain; **contains the
  channel-label error this doc corrects (fix in Phase 1)**.
- [gemini-live-api.md](./gemini-live-api.md) — WS protocol, model ids, 16/24 kHz contract.
- [waveshare-amoled-175.md](./waveshare-amoled-175.md) — board hardware context (PA, PMIC, interrupts).
- [build-toolchain.md](./build-toolchain.md) — component-manager pins, bootstrap copy rules.
- `docs/STABILITY_PLAN.md` — the sprint this design layers onto (P2.x preconditions, P3.x tasks).
