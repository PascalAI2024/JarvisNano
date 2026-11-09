/* SPDX-License-Identifier: Apache-2.0 */
#include "jr_dsp/audio_power.h"
#include <string.h>

bool jr_audio_power_reconcile(jr_audio_power_t *p, bool microphone_enabled,
                              bool playback_pending, bool sleeping,
                              const jr_audio_power_ops_t *ops)
{
    if (p == NULL || ops == NULL) return false;
    const bool want_adc = microphone_enabled && !sleeping;
    /* TX is the ADC's clock peer, even when there is no audible playback. */
    const bool want_dac = !sleeping && (want_adc || playback_pending);
    const bool want_pa = !sleeping && playback_pending;
    bool ok = true;
    bool opened_dac = false;
    if (p->adc_open && !want_adc) {
        if (ops->close_adc(ops->ctx)) p->adc_open = false;
        else ok = false;
    }
    if (p->dac_open && !want_dac) {
        if (ops->close_dac(ops->ctx)) p->dac_open = false;
        else ok = false;
    }
    if (!p->adc_open && want_adc) {
        if (ops->open_adc(ops->ctx)) p->adc_open = true;
        else ok = false;
    }
    if (!p->dac_open && want_dac) {
        if (ops->open_dac(ops->ctx)) {
            p->dac_open = true;
            opened_dac = true;
        } else ok = false;
    }
    const bool pa = want_pa && p->dac_open;
    /* Opening ES8311 can assert its PA pin. Always apply the desired latch
     * after open, including the silent clock-peer case. */
    if (p->pa_on != pa || opened_dac) ops->set_pa(ops->ctx, pa);
    p->pa_on = pa;
    return ok;
}

jr_audio_capture_result_t jr_audio_power_capture(
    bool microphone_enabled, bool adc_open, int16_t *frame, size_t samples,
    const jr_audio_capture_ops_t *ops)
{
    if (frame == NULL || samples == 0U || ops == NULL)
        return JR_AUDIO_CAPTURE_NONE;
    if (!microphone_enabled) {
        memset(frame, 0, samples * sizeof(*frame));
        ops->wait_frame(ops->ctx, 32U);
        return JR_AUDIO_CAPTURE_SILENT;
    }
    if (!adc_open || !ops->read_hardware(ops->ctx))
        return JR_AUDIO_CAPTURE_NONE;
    /* A privacy edge during the blocking hardware read discards that frame
     * before DSP or diagnostic publication. The read already paced it. */
    if (!ops->microphone_enabled(ops->ctx)) {
        memset(frame, 0, samples * sizeof(*frame));
        return JR_AUDIO_CAPTURE_SILENT;
    }
    return JR_AUDIO_CAPTURE_LIVE;
}

void jr_audio_uplink_batch_reset(jr_audio_uplink_batch_t *batch,
                                 int16_t *pcm, size_t capacity)
{
    memset(pcm, 0, capacity * sizeof(*pcm));
    batch->filled = 0U;
}

void jr_audio_uplink_batch_guard(jr_audio_uplink_batch_t *batch,
                                 int16_t *pcm, size_t capacity,
                                 bool upload_enabled, bool microphone_enabled)
{
    if (!upload_enabled || batch->microphone_enabled != microphone_enabled)
        jr_audio_uplink_batch_reset(batch, pcm, capacity);
    batch->microphone_enabled = microphone_enabled;
}

void jr_audio_tap_history_clear(size_t *head, size_t *count, uint64_t *total,
                                bool reset_cursor)
{
    *head = 0U;
    *count = 0U;
    if (reset_cursor) *total = 0U;
}

bool jr_audio_microphone_export_allowed(bool privacy_paused, bool flip_muted,
                                        bool microphone_enabled)
{
    return !privacy_paused && !flip_muted && microphone_enabled;
}
