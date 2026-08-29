/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_audio — L1 Audio Pipeline adapter (device implementation).
 *
 * Two one-way lanes on the pinned I2S0 duplex clock (24 kHz shared), behind the
 * pure AudioSource / AudioSink ports:
 *
 *   capture (AudioSource): esp_codec_dev 4-lane TDM read (24 kHz) -> 24->16
 *       downsample (per lane) -> demux (mic = buffer lane 1, echo-ref = lane 0,
 *       the MEASURED order) -> esp-sr AEC (FD_LOW_COST, filter_length 4, 16 kHz)
 *       -> digital gain + soft-knee -> 16 kHz mono frames the core pulls.
 *
 *   playback (AudioSink): 24 kHz mono chunks the core pushes into a drop-newest
 *       PSRAM ring; a feeder task drains it to the ES8311 via esp_codec_dev_write.
 *       mute_now() is the synchronous fast-kill (ES8311 mute + ring flush) callable
 *       from the capture task on a barge — it must not block.
 *
 * Everything ESP-IDF (codec_dev / esp-sr / board_manager) stays on this side of
 * the port; the core never sees a lane, a sample rate, or an esp_codec type.
 */
#ifndef JR_AUDIO_JR_AUDIO_H
#define JR_AUDIO_JR_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "jr_ports/audio_source.h"
#include "jr_ports/audio_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Acquire the ES7210/ES8311 codec handles (double-cast, spec §codec-bringup),
 * open ADC (24 kHz, 4-ch, mask 0) then DAC (24 kHz, muted), apply per-channel
 * PGA, create the AEC, allocate the playback ring, and start the feeder task.
 * Returns ESP_OK when the codec path is up. A degraded return still leaves the
 * ports safe to call (they no-op). */
esp_err_t jr_audio_init(void);

/* The two port factories the composition root injects into the core. Valid
 * after jr_audio_init(). */
jr_audio_source_t jr_audio_source(void);
jr_audio_sink_t   jr_audio_sink(void);

/* Last measured aec_process() cost in microseconds (the on-device <10 ms/32 ms
 * gate reads this). 0 until the first AEC frame runs. */
uint32_t jr_audio_last_aec_us(void);

/* AEC-clean mic RMS (jr_dsp_rms scale, 0..32768), sampled before the uplink
 * make-up gain. The buffer handed to the transport is amplified ~6x so the
 * model can hear the user; judging voice activity on that same buffer scales
 * the room's noise bed up with it. This is the unamplified view. */
float jr_audio_clean_rms(void);

/* True while decoded PCM is queued, a codec write is in flight, or the final
 * DMA/physical-output tail is still expected to be audible. The composition
 * root uses this to defer Gemini's terminal marker until playback is really
 * drained, preventing the speaker tail from reopening a phantom user turn. */
bool jr_audio_playback_pending(void);

/* ---- composition-root controls beyond the two port surfaces ---- */

/* Clear the fast-kill mute (maps JR_CMD_UNMUTE_DAC). mute is asserted via the
 * sink's mute_now(). */
void jr_audio_dac_unmute(void);

/* True while the fast-kill mute is asserted (feeder discards everything).
 * Diagnostic visibility: a stuck mute is "no sound with green counters". */
bool jr_audio_dac_muted(void);

/* Flush the playback ring without muting (maps JR_CMD_FLUSH_PLAYBACK_RING). */
void jr_audio_flush_playback(void);

/* Live tuning for /api/debug/gain. Any arg < 0 is left unchanged. mic_db/ref_db
 * are ES7210 per-channel PGA (chip-mic indices); out_vol is the ES8311 0..100
 * volume. */
void jr_audio_set_gains(int mic_db, int ref_db, int out_vol);

/* Runtime tune for the SPEAKING mic gain (the low PGA used while the model
 * speaks so the echo stays unclipped for the AEC). < 0 leaves it unchanged. */
void jr_audio_set_speak_mic_db(int speak_db);

/* Playback digital make-up gain as a percent of unity (100 == 1.0x). Applied
 * before the soft-knee limiter at the sink-write seam. Board default: 400 on
 * the original 1.75, 200 on the hotter 1.75C chain. Clamped to 25..800; out
 * of range is ignored. Live-tunable via /api/debug/gain?pbgain=. */
void jr_audio_set_playback_gain_percent(int percent);
int  jr_audio_playback_gain_percent(void);

/* Select the capture PGA state. The app calls this on phase transitions:
 * true while the model is SPEAKING (low mic gain -> unclipped echo -> the AEC
 * cancels it, ~180 residual instead of a railed ~9000), false otherwise (full
 * mic gain for the user's own voice). This is v4's barge-enabling fix. */
void jr_audio_set_speaking(bool speaking);

/* Rolling two-second diagnostics taps. They are fed at the existing single
 * codec read/write seams—never by a competing I2S owner. Diagnostic producers
 * are best-effort and never wait for a reader, so an export cannot stall the
 * realtime audio path. MIC_CLEAN is the exact gained 16 kHz uplink PCM;
 * PLAYBACK is PCM successfully accepted by the codec write seam (after any
 * codec-dev software-volume mutation, but before analogue gain/speaker).
 *
 * jr_audio_diag_copy() requires capacity_samples >= available_samples at copy
 * time. Allocate info.capacity_samples to avoid a producer racing the export.
 * On ESP_ERR_INVALID_SIZE it copies nothing and returns the current requirement
 * through out_info. Successful copies are chronological (oldest to newest). */
typedef enum {
    JR_AUDIO_TAP_MIC_CLEAN = 0,
    JR_AUDIO_TAP_MIC_RAW,
    JR_AUDIO_TAP_REFERENCE,
    JR_AUDIO_TAP_PLAYBACK,
    JR_AUDIO_TAP_COUNT,
} jr_audio_tap_kind_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t available_samples;
    uint32_t capacity_samples;
    uint64_t total_samples;
    int16_t peak;
    float rms;
    uint32_t clipped_samples;
} jr_audio_tap_info_t;

void jr_audio_diag_reset(void);
esp_err_t jr_audio_diag_get_info(jr_audio_tap_kind_t kind,
                                 jr_audio_tap_info_t *out_info);
esp_err_t jr_audio_diag_copy(jr_audio_tap_kind_t kind, int16_t *dst,
                             size_t capacity_samples,
                             jr_audio_tap_info_t *out_info);

/* Queue a bounded low-volume 400–2000 Hz linear chirp through the real playback
 * ring. duration_ms is clamped to 100..1500 and level_percent to 1..30. The
 * call is all-or-nothing, rejects active playback with ESP_ERR_INVALID_STATE,
 * and unmutes the DAC only after the complete chirp has been queued. */
esp_err_t jr_audio_diag_play_chirp(uint32_t duration_ms,
                                   uint8_t level_percent);

/* The same bounded, low-volume sweep with its endpoints exposed. duration_ms
 * clamps to 100..1500, level_percent to 1..30, and the frequencies to
 * 80..8000 Hz; it rejects active playback exactly as the chirp does, so it can
 * never talk over a reply.
 *
 * This exists so a cue can FALL as well as rise. The attention beat is a
 * rising note, and until now every cue rode that same sweep — which meant a
 * refused gesture sounded identical to an accepted one. A descending note is
 * the cheapest unmistakable "no" available on a device with one speaker and
 * no haptics. See docs/INTERACTION_MODEL.md §7. */
esp_err_t jr_audio_play_sweep(uint16_t start_hz, uint16_t end_hz,
                              uint32_t duration_ms, uint8_t level_percent);

#ifdef __cplusplus
}
#endif

#endif /* JR_AUDIO_JR_AUDIO_H */
