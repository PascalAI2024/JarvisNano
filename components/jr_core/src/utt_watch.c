/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/utt_watch.c - see utt_watch.h.
 */
#include "jr_core/utt_watch.h"

#include <string.h>

void jr_utt_init(jr_utt_watch_t *w)
{
    memset(w, 0, sizeof *w);
}

void jr_utt_speech_start(jr_utt_watch_t *w, uint32_t now_ms)
{
    w->start_ms = now_ms;
    w->pardon_turn = false;     /* the owner is speaking again: that turn is over */
}

void jr_utt_speech_end(jr_utt_watch_t *w, uint32_t now_ms, bool listening,
                       bool text_turn_pending)
{
    if (listening && !text_turn_pending &&
        now_ms - w->start_ms >= JR_UTT_MIN_MS) {
        w->armed = true;
        w->deadline_ms = now_ms + JR_UTT_REPLY_WAIT_MS;
    }
}

void jr_utt_server_alive(jr_utt_watch_t *w)
{
    w->armed = false;
}

void jr_utt_answered(jr_utt_watch_t *w)
{
    w->armed = false;
    if (w->pardon_turn) {
        return;     /* the model answering our own text turn says nothing about
                     * whether it hears the owner's voice */
    }
    w->misses = 0u;
    w->answered_total++;
}

jr_utt_action_t jr_utt_poll(jr_utt_watch_t *w, uint32_t now_ms)
{
    if (!w->armed || (int32_t)(now_ms - w->deadline_ms) < 0) {
        return JR_UTT_NONE;
    }
    w->armed = false;
    w->misses++;
    w->misses_total++;
    if (w->misses >= JR_UTT_DEAF_MISSES) {
        w->misses = 0u;
        return JR_UTT_RECONNECT;
    }
    w->pardon_turn = true;
    return JR_UTT_PARDON;
}
