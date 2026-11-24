/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The adapter serializes lifecycle calls with both channel guards. Ordinary
 * ADC reads and DAC writes retain separate guards and run concurrently. */
typedef struct {
    bool adc_open;
    bool dac_open;
    bool pa_on;
} jr_audio_power_t;

typedef struct {
    void *ctx;
    bool (*open_adc)(void *ctx);
    bool (*close_adc)(void *ctx);
    bool (*open_dac)(void *ctx);
    bool (*close_dac)(void *ctx);
    void (*set_pa)(void *ctx, bool on);
} jr_audio_power_ops_t;

/* Shared S3 duplex I2S cannot stop either DMA channel until both codecs close.
 * Privacy closes the ADC analog path immediately; queued playback keeps the
 * DAC until its final DMA tail has drained. An unmuted WakeNet keeps clocks. */
bool jr_audio_power_reconcile(jr_audio_power_t *power, bool microphone_enabled,
                              bool playback_pending, bool sleeping,
                              const jr_audio_power_ops_t *ops);

typedef enum {
    JR_AUDIO_CAPTURE_NONE,
    JR_AUDIO_CAPTURE_SILENT,
    JR_AUDIO_CAPTURE_LIVE,
} jr_audio_capture_result_t;

typedef struct {
    void *ctx;
    bool (*read_hardware)(void *ctx);
    void (*wait_frame)(void *ctx, uint32_t milliseconds);
    bool (*microphone_enabled)(void *ctx);
} jr_audio_capture_ops_t;

/* Called at the actual source-read boundary, before any DSP or diagnostic
 * tap. Muted text turns receive paced silence without reading hardware. */
jr_audio_capture_result_t jr_audio_power_capture(
    bool microphone_enabled, bool adc_open, int16_t *frame, size_t samples,
    const jr_audio_capture_ops_t *ops);

/* The voice owner keeps the existing two-frame buffer. Partial batches belong
 * to one microphone mode and one uninterrupted capture/transport window. */
typedef struct {
    size_t filled;
    bool microphone_enabled;
} jr_audio_uplink_batch_t;

void jr_audio_uplink_batch_reset(jr_audio_uplink_batch_t *batch,
                                 int16_t *pcm, size_t capacity);
void jr_audio_uplink_batch_guard(jr_audio_uplink_batch_t *batch,
                                 int16_t *pcm, size_t capacity,
                                 bool upload_enabled, bool microphone_enabled);

/* Automatic privacy clearing discards history, preserving the absolute cursor.
 * Only an explicit diagnostics reset starts a new cursor epoch. */
void jr_audio_tap_history_clear(size_t *head, size_t *count, uint64_t *total,
                                bool reset_cursor);

bool jr_audio_microphone_export_allowed(bool privacy_paused, bool flip_muted,
                                        bool microphone_enabled);
