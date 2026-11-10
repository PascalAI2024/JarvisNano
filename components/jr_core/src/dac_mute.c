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

/* THE POLICY TABLE. Every tone that existed before the interface cues sounds
 * under the owner mute exactly as it always did; only the new cues stay silent.
 * To make the ask-miss sweep or the chime respect the mute, flip its row. */
bool jr_dac_mute_sounds_under_owner_mute(jr_tone_kind_t kind)
{
    switch (kind) {
    case JR_TONE_OWNER_ACK:  return true;
    case JR_TONE_DIAG_CHIRP: return true;
    case JR_TONE_SWEEP:      return true;
    case JR_TONE_CHIME:      return true;
    case JR_TONE_UI_CUE:     return false;
    }
    return false;
}

bool jr_dac_mute_tone_begin(jr_dac_mute_t *m, bool owner_muted,
                            jr_tone_kind_t kind)
{
    return jr_dac_mute_cue_begin(
        m, owner_muted && !jr_dac_mute_sounds_under_owner_mute(kind));
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
