/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_dac_mute — see the header. Pure.
 */
#include "jr_core/dac_mute.h"

bool jr_dac_mute_cue_begin(jr_dac_mute_t *m, bool owner_muted)
{
    if (owner_muted) {
        return false;
    }
    m->restore = m->restore || m->muted;
    m->muted = false;
    return true;
}

bool jr_dac_mute_tone_begin(jr_dac_mute_t *m, bool owner_muted, bool owner_ack)
{
    return jr_dac_mute_cue_begin(m, owner_muted && !owner_ack);
}

void jr_dac_mute_unmute(jr_dac_mute_t *m)
{
    m->muted = false;
    m->restore = false;
}

void jr_dac_mute_mute(jr_dac_mute_t *m)
{
    m->muted = true;
    m->restore = false;
}

bool jr_dac_mute_cue_settle(jr_dac_mute_t *m, bool playback_pending)
{
    if (!m->restore || playback_pending) {
        return false;
    }
    m->muted = true;
    m->restore = false;
    return true;
}
