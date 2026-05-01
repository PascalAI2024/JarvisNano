package com.ingeniousdigital.jarvisnano.llm

import kotlinx.coroutines.flow.Flow

/**
 * Phase 3 — on-device inference.
 *
 * The companion app will eventually run a multimodal model fully on the phone so the
 * device can keep working without an LLM key, without a network, and without leaking
 * the user's voice to a third-party API.
 *
 * Plan: bind https://github.com/ggerganov/llama.cpp's Android port (jllama / llama.cpp
 * NDK build) and load `unsloth/gemma-4-E4B-it-GGUF` Q4_K_M (~3 GB on disk, ~3.5 GB
 * RAM peak; native multimodal incl. audio in).
 *
 * Roadmap:
 *   - Phase 3a: text-only chat. Token streaming via [generate].
 *   - Phase 3b: audio-in → text via the model's native audio encoder.
 *   - Phase 3c: tool-use bridge so the local model can poke the device's
 *               capabilities the same way the cloud LLM does.
 */
interface LocalLlm {
    /**
     * Streams generated tokens. The flow completes when the model emits an EOS token
     * or [maxTokens] is reached.
     *
     * @param prompt user / system text concatenated per the model's chat template.
     * @param audio optional 16 kHz mono PCM S16LE bytes, fed into the audio encoder
     *              when the loaded model supports it.
     */
    suspend fun generate(
        prompt: String,
        audio: ByteArray? = null,
        maxTokens: Int = 512,
    ): Flow<String>

    /** True once weights are loaded into RAM and inference is ready. */
    val isReady: Boolean
}
