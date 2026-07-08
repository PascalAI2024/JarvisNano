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

/* ---- composition-root controls beyond the two port surfaces ---- */

/* Clear the fast-kill mute (maps JR_CMD_UNMUTE_DAC). mute is asserted via the
 * sink's mute_now(). */
void jr_audio_dac_unmute(void);

/* Flush the playback ring without muting (maps JR_CMD_FLUSH_PLAYBACK_RING). */
void jr_audio_flush_playback(void);

/* Live tuning for /api/debug/gain. Any arg < 0 is left unchanged. mic_db/ref_db
 * are ES7210 per-channel PGA (chip-mic indices); out_vol is the ES8311 0..100
 * volume. */
void jr_audio_set_gains(int mic_db, int ref_db, int out_vol);

#ifdef __cplusplus
}
#endif

#endif /* JR_AUDIO_JR_AUDIO_H */
