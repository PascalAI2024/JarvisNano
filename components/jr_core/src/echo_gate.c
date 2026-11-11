/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/echo_gate.c - see echo_gate.h.
 */
#include "jr_core/echo_gate.h"

#include <string.h>

void jr_echo_gate_init(jr_echo_gate_t *g)
{
    memset(g, 0, sizeof *g);
}

bool jr_echo_gate_silence(jr_echo_gate_t *g, uint32_t now_ms, bool audible,
                          bool enabled)
{
    if (audible) {
        g->seen = true;
        g->last_audible_ms = now_ms;
    }
    /* Tracked even when the switch is off, so turning it on mid-reply still
     * honours the tail. */
    bool silence = false;
    if (enabled) {
        silence = audible ||
                  (g->seen && (uint32_t)(now_ms - g->last_audible_ms) < JR_ECHO_TAIL_MS);
    }
    if (silence) {
        g->gated_frames++;
    } else {
        g->passed_frames++;
    }
    return silence;
}

void jr_reply_cut_init(jr_reply_cut_t *c)
{
    c->cutting = false;
    c->last_chunk_ms = 0u;
    c->dropped_chunks = 0u;
    c->cuts = 0u;
}

void jr_reply_cut_begin(jr_reply_cut_t *c, uint32_t now_ms)
{
    c->last_chunk_ms = now_ms;
    c->cutting = true;
    c->cuts++;
}

bool jr_reply_cut_drop_audio(jr_reply_cut_t *c, uint32_t now_ms)
{
    if (!c->cutting) {
        return false;
    }
    if ((uint32_t)(now_ms - c->last_chunk_ms) > JR_REPLY_CUT_GAP_MS) {
        c->cutting = false;     /* the boundary never came: this is a new reply */
        return false;
    }
    c->last_chunk_ms = now_ms;
    c->dropped_chunks++;
    return true;
}

void jr_reply_cut_boundary(jr_reply_cut_t *c)
{
    c->cutting = false;
}
