# Audio: ES8311 (DAC) + ES7210 (ADC)

**What it is** — The audio codec chain on the Waveshare AMOLED-1.75 board. The ES8311 is a mono speaker DAC; the ES7210 is a 4-channel ADC with on-chip hardware acoustic echo cancellation (AEC). Both are driven via I2C control + I2S data, managed through `esp_board_manager` and `esp_codec_dev`.

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

**[2026-05-21] 16 kHz capture in / 24 kHz playback out — no resample needed for Gemini Live**

Gemini Live expects 16 kHz PCM in and returns 24 kHz PCM out. Our codec chain matches exactly: ES7210 captures at 16 kHz, ES8311 plays back at 24 kHz. No software resampling is needed if I2S rates are set correctly.

Reference: EchoEar (xiaozhi `boards/esp-vocat/config.h`) also uses `AUDIO_INPUT_SAMPLE_RATE 24000` / `AUDIO_OUTPUT_SAMPLE_RATE 24000` — note that EchoEar uses 24 kHz input because it does not feed Gemini Live. Our input must stay at 16 kHz for the Gemini API.

**[2026-05-21] ES7210 AEC: channel mask `0111` — Mic0 is labeled NA**

`board_devices.yaml` sets `adc_channel_mask: "0111"` with labels `['NA', 'REF', 'MIC1', 'MIC2']`. Channel 0 (labeled NA) is excluded; channels 1–3 are enabled. Channel 1 is the AEC reference loopback from the speaker output. Channels 2 and 3 are the two MEMS microphones.

Source: `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:62-65`.

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

- [board-manager.md](./board-manager.md) — how to acquire codec handles without the double-deref crash.
- [gemini-live-api.md](./gemini-live-api.md) — 16 kHz / 24 kHz I/O contract with the Gemini API.
- [waveshare-amoled-175.md](./waveshare-amoled-175.md) — physical hardware context (PA chip, rail voltages).
