/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_dsp/dsp.c — pure DSP math. Links only the C standard library.
 */
#include "jr_dsp/dsp.h"
#include <math.h>
#include <string.h>

float jr_dsp_rms(const int16_t *samples, size_t n)
{
    if (samples == NULL || n == 0) {
        return 0.0f;
    }
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double s = (double)samples[i];
        acc += s * s;
    }
    return (float)sqrt(acc / (double)n);
}

/* ------------------------- playback jitter stepper ---------------------- */

void jr_jitter_init(jr_jitter_t *j, uint32_t preroll_ms)
{
    memset(j, 0, sizeof *j);
    j->preroll_ms = preroll_ms;
    j->adaptive = true;
}

void jr_jitter_pin(jr_jitter_t *j, uint32_t preroll_ms)
{
    if (preroll_ms == 0u) {
        j->preroll_ms = JR_JITTER_PREROLL_FLOOR_MS;
        j->adaptive = true;                 /* back to the walk */
    } else {
        j->preroll_ms = preroll_ms > 3000u ? 3000u : preroll_ms;
        j->adaptive = false;                /* pinned by hand */
    }
}

/* The walk, run once as a reply ends. */
static void jitter_adapt(jr_jitter_t *j, jr_jitter_out_t *out)
{
    if (!j->adaptive) {
        return;
    }
    const uint32_t pre = j->preroll_ms;
    uint32_t next = pre;
    if (j->reply_holes > 0u) {
        j->clean_replies = 0u;
        next = pre + JR_JITTER_STEP_UP_MS > JR_JITTER_PREROLL_CEIL_MS
                   ? JR_JITTER_PREROLL_CEIL_MS : pre + JR_JITTER_STEP_UP_MS;
    } else if (++j->clean_replies >= JR_JITTER_CLEAN_TO_STEP) {
        j->clean_replies = 0u;
        next = pre > JR_JITTER_PREROLL_FLOOR_MS + JR_JITTER_STEP_DOWN_MS
                   ? pre - JR_JITTER_STEP_DOWN_MS : JR_JITTER_PREROLL_FLOOR_MS;
    }
    if (next != pre) {
        j->preroll_ms = next;
        out->preroll_changed = true;
        out->preroll_prev_ms = pre;
    }
}

void jr_jitter_feed(jr_jitter_t *j, uint32_t now_ms, uint32_t avail_samples,
                    bool tail_active, bool earcon_outstanding,
                    jr_jitter_out_t *out)
{
    memset(out, 0, sizeof *out);

    /* An earcon owns the ring. Its own starvation holes and its own DAC tail
     * are not evidence about the network, so nothing below may see them. The
     * tail starts when the last tone sample has been written, which is the
     * first feed that reports no outstanding samples. */
    if (earcon_outstanding) {
        j->earcon_seen = true;
    } else if (j->earcon_seen) {
        j->earcon_seen = false;
        j->earcon_tail_until_ms = now_ms + JR_JITTER_TAIL_MS;
    }
    if (earcon_outstanding ||
        (int32_t)(j->earcon_tail_until_ms - now_ms) > 0) {
        out->quiet = true;
        out->preroll_ms = j->preroll_ms;
        return;
    }

    if (avail_samples > 0u) {
        /* Data is back (held or not): close any hole that was open. The gap
         * is measured to ARRIVAL, so a refill hold is not counted as more
         * hole than the network actually left. */
        j->empty = false;
        j->reply_open = true;
        if (j->in_gap) {
            const uint32_t gap = now_ms - j->gap_start_ms;
            j->in_gap = false;
            if (gap <= JR_JITTER_REPLY_GAP_MS) {
                j->reply_holes++;
                out->hole_booked = true;
                out->hole_ms = gap;
            }
        }
    } else {
        if (!j->in_gap && tail_active) {
            /* The ring ran dry while the tail of the last write was still
             * audible: a hole. Whatever arrives next must rebuild a bigger
             * lead before playing, so the NEXT hole is absorbed. */
            j->in_gap = true;
            j->gap_start_ms = now_ms;
            out->gap_opened = true;
        } else if (j->in_gap &&
                   now_ms - j->gap_start_ms > JR_JITTER_REPLY_GAP_MS) {
            j->in_gap = false;          /* silence followed: a reply ended */
            out->gap_ended = true;
        }
        /* Once the ring has been empty long enough that whatever comes next
         * is a new reply, re-arm the smaller start-of-reply lead. */
        if (!j->empty) {
            j->empty = true;
            j->empty_since_ms = now_ms;
        } else if (now_ms - j->empty_since_ms > JR_JITTER_REPLY_GAP_MS) {
            out->idle = true;
            if (j->reply_open) {        /* once, as the reply ends */
                j->reply_open = false;
                out->reply_ended = true;
                out->reply_holes = j->reply_holes;
                jitter_adapt(j, out);
                j->reply_holes = 0u;
            }
        }
    }
    out->preroll_ms = j->preroll_ms;
}
