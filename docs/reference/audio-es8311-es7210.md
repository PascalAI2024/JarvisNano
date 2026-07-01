# Audio: ES8311 (DAC) + ES7210 (ADC)

**What it is** — The audio codec chain on the Waveshare AMOLED-1.75 board. The ES8311 is a mono speaker DAC; the ES7210 is a 4-channel ADC that digitizes the two MEMS mics plus a hardware echo-reference loopback (it has **no on-chip AEC** — echo cancellation is software, esp-sr; see [aec-barge-in.md](./aec-barge-in.md)). Both are driven via I2C control + I2S data, managed through `esp_board_manager` and `esp_codec_dev`.

**How we use it here** — 16 kHz mono PCM from the ES7210 is sent to Gemini Live as the microphone stream. 24 kHz mono PCM returned from Gemini Live is played back through the ES8311. The `cap_gemini_live` component acquires both codec handles via `esp_board_manager_get_device_handle`, opens them, and runs a half-duplex I2S loop.

---

## Findings & gotchas

**[2026-05-21] `esp_codec_dev_set_out_vol(100)` is unity gain — not the loudest setting**

The default `esp_codec_dev` volume curve maps `[1, 100] → [-49.5 dB, 0 dB]` at 0.5 dB/step. `0 → -96 dB`. Maximum is 0 dB (unity). There is **no positive gain** in the default curve. Setting vol 100 and still hearing quiet output is expected — the ceiling is unity gain, not +6 dB.

Options to go louder:
- Install a custom curve via `esp_codec_dev_set_vol_curve` with `esp_codec_dev_vol_map_t[]` entries that map high volume to positive dB values (if the codec/PA headroom allows).
- Raise hardware gain: set `hw_gain.pa_voltage` and `hw_gain.codec_dac_voltage` in the `es8311_codec_cfg_t`. Xiaozhi ships `pa_voltage = 5.0` / `codec_dac_voltage = 3.3` as production values. Our `board_devices.yaml` does NOT set `pa_voltage` / `codec_dac_voltage` — this is the next lever if software gain is insufficient.
- ES8311 DAC register gain: last resort, via the raw `es8311.c` register driver.

Source: `esp-adf` `esp_codec_dev` component README; `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:43-44` (dac_init_gain=0, pa_gain=6 set, but not pa_voltage/codec_dac_voltage).

**[2026-05-21] High-crest-factor speech: flat gain hard-clips, use soft-knee limiter**

Measured PCM from the Gemini Live 24 kHz output: peaks ~80% full-scale (~26000/32767) but RMS only ~15% (~3000–6600). This is normal high-crest speech. A flat 4× gain clips those peaks into square-wave distortion, which is perceived as "can't understand" — not volume.

The correct approach: vol=100 plus software make-up gain with a soft-knee limiter. Tuned values in `cap_gemini_live.c`:
- `GL_OUT_GAIN 3` (make-up gain multiplier)
- `GL_LIMIT_KNEE 24000` (knee threshold in PCM counts)
- 4:1 compression above the knee

Tune `GL_OUT_GAIN` up or down on user feedback. The limiter protects the peaks regardless.

Source: `firmware/components/cap_gemini_live/src/cap_gemini_live.c:85-95` (limiter defines and comment).

**[2026-05-21] Choppy audio root cause: single shared `rx_buf` overwritten by fast frames**

The original WS receive path used a single shared `rx_buf` + `xTaskNotifyGive`. Fast-arriving Gemini audio frames overwrote `rx_buf` before the blocking-playback consumer had read them. Result: choppy / partial playback.

Fix: PSRAM-backed frame queue, depth 128. At that depth, drops go to zero under normal network conditions. A depth of 16 dropped frames at approximately every 12 seconds.

Source: memory file `project_waveform_solo_state.md`.

**[2026-05-21] ~~16 kHz capture in / 24 kHz playback out — no resample needed~~ DISPROVEN 2026-06-10: the shared I2S duplex clock cannot hold 16 kHz RX + 24 kHz TX**

Original claim: ES7210 captures at 16 kHz, ES8311 plays at 24 kHz, no software
resampling needed. **Wrong on this board.** Both codecs sit on ONE I2S port in
STD duplex (same BCLK/WS), so TX and RX share a clock. `gl_open_dac(24000)`
succeeds, but the next record-path (re)init reconfigures the pair and slams TX
back to 16 kHz — device log: `I2S_IF: STD: TX, sample_rate_hz: 16000`
immediately after `enter_speaking: gl_open_dac(24000) = 0`. Result: 24 kHz PCM
played on a 16 kHz clock — 1.5× slow, pitched down, write backpressure →
intermittent cutoffs. Symptom waxes/wanes with open ORDER, which made it look
random.

Fix (2026-06-10, `cap_gemini_live.c`): `gl_resolve_playback_rate()` returns
`GL_TX_SAMPLE_RATE` (16 kHz) whenever a capture path exists, and the playback
path resamples model 24 kHz → 16 kHz with `gl_resample_pcm16_linear()`
(linear interpolation; the old nearest-neighbour decimation caused metallic
aliasing). The I2S clock then never moves during a session. Also fixed: the
playback entry no longer drops chunks that arrive before `enter_speaking`
opens the DAC (start-of-utterance clipping) — it opens on demand.

Reference: EchoEar (xiaozhi `boards/esp-vocat/config.h`) runs BOTH directions
at 24 kHz for the same shared-clock reason. Our input must stay 16 kHz for the
Gemini API, so we hold the bus at 16 kHz and resample the downlink instead.

**[2026-06-14] Native 24 kHz output (current): common 24 kHz clock + capture downsample**

Supersedes the 16 kHz lock + downlink resample. Run the *entire* duplex I2S/codec
at 24 kHz (board_peripherals.yaml + gl_open_* at GL_CAP_SAMPLE_RATE). Model
24 kHz PCM plays natively to the ES8311 (crisp, no resample in the main path).
Mic capture reads 768 samples, immediately downsampled 24→16 (linear per-lane
`gl_downsample_capture_24to16`) so AEC (esp-sr requires 16 k), VAD, barge, and
Gemini uplink frames remain byte-identical 512-sample 16 kHz.

`gl_resolve_playback_rate` forces 24 kHz whenever the capture path is live (shared
clock invariant). Playback resample path is kept only as a fallback for raw or
mismatch cases.

Board yaml + audio level sampler updated to 24 kHz for consistency. Feeder chunk
made duration-aware. See `cap_gemini_live.c:220 (defines), 1816 (downsample),
2037 (feeder), 2399 (capture path), 3168 (enter speaking)` and the native 24 kHz
commit.

This is the production path that made "connected" 24 kHz voice work on hardware.

**[2026-06-10] Local VAD tuning: speech 1000 / silence 500 / min-speech 240 ms — and reset accumulators on commit**

Field calibration (post-6x-gain RMS): idle ceiling ~939, speech band 1000–4900.
The original `GL_VAD_SPEECH_RMS 1200` sat *inside* the speech band, so quiet or
distant speech never latched a turn — the user experience was "it never
replies" with no error anywhere. 1000 hugs the ambient ceiling. Don't go below
~950 without re-measuring the room floor. `GL_VAD_MIN_SPEECH_MS` 300→240:
short utterances ("yes") sat right at the boundary.

Separate bug, same UX: after `vad_commit_request = true` the TX task kept its
`speech_seen`/`silence_ms` accumulators, and the session task clears the
request flag while the state is still LISTENING — the very next 20 ms frame
re-fired a second commit (two "end of speech" logs 20 ms apart → double
`end_input` per turn). Accumulators now reset at request time.

Also: taps during THINKING are ignored for 10 s (`cap_gemini_live_toggle`).
Live trace showed a 16 s grounding delay → user tapped three times → killed
their own pending reply. After 10 s the tap stops the session as before.

**[2026-06-10] BLE GATT service disabled — Wi-Fi/BT coexistence starves streaming audio**

`main.c` started a NimBLE GATT companion service (`ble_gatt_init()`) at boot.
With the BT controller active, ESP32-S3 software coexistence time-slices the
single 2.4 GHz radio between BT and Wi-Fi — the boot log showed repeated
`wifi: Coexist: Wi-Fi connect fail, apply reconnect coex policy` and
`Coexist!!! Wi-Fi station would only keep waked when available`. For a
WSS-streaming voice device this manifests as intermittent audio stalls.
Nothing pairs over BLE today, so the init call is replaced with a log line
(grep `BLE GATT service disabled` in `main.c`). Restore the call if a BLE
companion app ever ships — and revisit coexistence tuning then.

**[2026-05-21] ~~ES7210 AEC: channel mask `0111` — Mic0 is labeled NA~~ CORRECTED 2026-06-12: ref is MIC3 (TDM lane 2); MIC1/MIC2 are the mics; the chip has no on-chip AEC**

The original claim ("Channel 0 excluded; channel 1 is the AEC reference; channels 2–3
are the mics") was wrong on every channel assignment. Schematic-verified facts
([aec-barge-in.md](./aec-barge-in.md), vendor PDF p.1, 2026-06-12):

- `adc_channel_mask: "0111"` = binary 0x7 = `ES7210_SEL_MIC1|MIC2|MIC3` — the three
  **lowest** channels are enabled, not the highest.
- **MIC1 + MIC2 (TDM lanes 0/1) = the two MEMS microphones. MIC3 (lane 2) = the speaker
  echo-reference loopback** (ES8311 OUTP/OUTN via ~−23.5 dB pad → ES7210 pins 31/32).
  MIC4 (lane 3) is unconnected.
- The ES7210 only **digitizes** the reference; echo cancellation is esp-sr software.
  There is no on-chip AEC.

`board_devices.yaml` labels were fixed accordingly: `['MIC1', 'MIC2', 'REF', 'NC']`
(index = zero-based TDM lane). Labels are informational only — codegen'd into a string
field, never parsed at runtime — so this was a label-only change. An AEC implementation
demuxing lane 1 as the reference would feed a microphone as the ref and converge on
nothing; **demux `ref = lane 2`**.

Source: `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:54-68`;
`docs/reference/aec-barge-in.md` §1 (schematic citations).

**[2026-05-21] Board-manager handle cast: use `dev_audio_codec_handles_t*` directly**

See [board-manager.md](./board-manager.md). The LoadStoreError crash in the audio init was the same double-deref bug: `cap_gemini_live.c` was reading `handle->device_handle` off the wrapper instead of casting directly to `dev_audio_codec_handles_t*`. Post-fix, codec handle addresses are in RAM (`0x3c24...`), not IRAM (`0x420b...`).

Source: `cap_gemini_live.c:147-161`.

---

## Primary sources

| Source | Notes |
|--------|-------|
| `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:33-71` | ES8311 (`audio_dac`) and ES7210 (`audio_adc`) declarations, channel masks, gain config. |
| `firmware/components/cap_gemini_live/src/cap_gemini_live.c:85-95` | Make-up gain + soft-knee limiter defines. |
| `cap_gemini_live.c:147-161` | Correct board-manager handle acquisition for both codecs. |
| [`espressif/esp-adf esp_codec_dev`](https://github.com/espressif/esp-adf/tree/master/components/esp_codec_dev) | Authoritative codec/volume API. `set_vol_curve`, `set_out_vol`, dB curve spec. |
| [`xiaozhi-esp32 es8311_audio_codec.cc`](https://github.com/78/xiaozhi-esp32/blob/main/main/audio/codecs/es8311_audio_codec.cc) | Production hw_gain values: `pa_voltage=5.0`, `codec_dac_voltage=3.3`. |
| [ES8311 register driver](https://github.com/espressif/esp-adf/blob/master/components/audio_hal/driver/es8311/es8311.c) | Raw DAC gain registers, last-resort option. |
| [ES8311 datasheet](https://dl.espressif.com/dl/schematics/Audio_ES8311.pdf) | DAC gain register map. **(not fetch-verified)** |

---

## Open questions

- What is the actual PA rail voltage on this board? If it is not 5.0 V, the xiaozhi `pa_voltage=5.0` value does not apply and output will be consistently under the expected level.
- Is `esp_codec_dev_set_vol_curve` safe to call after the codec is already open, or must it be set before `esp_codec_dev_open`?
- Should the `GL_OUT_GAIN` / `GL_LIMIT_KNEE` diagnostic `ESP_LOGI` be removed once volume is confirmed dialed in?

---

## See also

- [aec-barge-in.md](./aec-barge-in.md) — AEC + barge-in design; schematic-verified ES7210 channel map (mics = lanes 0/1, echo ref = lane 2).
- [board-manager.md](./board-manager.md) — how to acquire codec handles without the double-deref crash.
- [gemini-live-api.md](./gemini-live-api.md) — 16 kHz / 24 kHz I/O contract with the Gemini API.
- [waveshare-amoled-175.md](./waveshare-amoled-175.md) — physical hardware context (PA chip, rail voltages).
