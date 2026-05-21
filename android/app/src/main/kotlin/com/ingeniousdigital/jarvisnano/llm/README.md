# Local LLM — Phase 3

Status: interface only. No real binding yet.

This is the **canonical** Phase-3 runtime description for JarvisNano. The protocol doc (`docs/PROTOCOL.md` §7) and the device-side hand-off docs reference these values directly.

## Target stack

- Engine: [llama.cpp](https://github.com/ggerganov/llama.cpp) built as an **Android NDK** library and bound through JNI from the Kotlin/Compose companion in `/android`. **Not** `llama.rn` — this app is native Android, not React Native.
- Model: `unsloth/gemma-4-E4B-it-GGUF` quantized at `Q4_K_M`.
- Disk: ~3 GB.
- RAM peak: ~3.5 GB at runtime.
- Native multimodal: text **and** audio-in via the model's audio encoder. PCM frames arriving on BLE `audio_in` (16 kHz mono S16LE, 20 ms / 320 samples per frame) feed straight into the model graph — no separate STT round-trip.

## Why Gemma 4 E4B / Q4_K_M?

- `Q4_K_M` is the sweet spot between quality drop and RAM headroom on a 6–8 GB
  phone. `Q3_K` saves ~700 MB but the instruction-following falls off a cliff.
- Native audio-in lets the device hand a raw mic clip straight to the model with
  no Whisper round-trip — the audio encoder is part of the model graph.
- Apache-2.0 weights, no usage restrictions for the open-source companion.

## Why interface-only for now?

The first ship of JarvisNano targets phones with a working LAN connection to the
device. The cloud LLM (configured per-device, OpenAI / Anthropic / Mistral) does
all the heavy lifting through `/api/webim`. Local inference is the second
"private mode" toggle and pulls in an NDK build + 3 GB asset download — too much
for a v1.0 install.

## Phase 3 wiring sketch

1. Vendor the llama.cpp NDK build under `app/src/main/jniLibs/`.
2. Build a JNI bridge with `external fun nativeLoad(modelPath: String): Long` and
   `external fun nativeStream(handle: Long, prompt: String, audio: ByteArray?): Flow<String>`.
3. Implement `LocalLlm` over that bridge.
4. Add a Settings toggle: "Run model on this phone instead of the device's
   provider" — defaults to off.
