/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/mic_watch.c - see mic_watch.h.
 */
#include "jr_core/mic_watch.h"

#include <string.h>

void jr_mic_watch_init(jr_mic_watch_t *w)
{
    memset(w, 0, sizeof *w);
    w->backoff_ms = JR_MIC_BACKOFF_MIN_MS;
}

bool jr_mic_watch_frame(jr_mic_watch_t *w, uint32_t now_ms, float raw_rms,
                        bool adc_open, bool meant_open)
{
    if (!adc_open || !meant_open) {
        /* Not judged: a closed or muted microphone is not a dead one. */
        w->quiet = false;
        w->dead = false;
        return false;
    }
    if (raw_rms >= JR_MIC_DEAD_RMS) {
        w->quiet = false;
        w->dead = false;
        w->backoff_ms = JR_MIC_BACKOFF_MIN_MS;
        return false;
    }
    if (!w->quiet) {
        w->quiet = true;
        w->quiet_since_ms = now_ms;
    }
    if ((uint32_t)(now_ms - w->quiet_since_ms) < JR_MIC_DEAD_MS) {
        return false;
    }
    w->dead = true;
    if (w->have_next && (int32_t)(now_ms - w->next_ok_ms) < 0) {
        return false;
    }
    w->recoveries++;
    w->have_next = true;
    w->next_ok_ms = now_ms + w->backoff_ms;
    if (w->backoff_ms < JR_MIC_BACKOFF_MAX_MS / 2u) {
        w->backoff_ms *= 2u;
    } else {
        w->backoff_ms = JR_MIC_BACKOFF_MAX_MS;
    }
    /* The reopen gets a fresh window to prove itself. */
    w->quiet = false;
    return true;
}
