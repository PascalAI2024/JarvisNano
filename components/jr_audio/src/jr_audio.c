/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_audio — L1 Audio Pipeline adapter. Phase 0 placeholder so the component
 * graph is complete and REQUIRES resolve. No real audio path yet.
 *
 * TODO(Phase 1): two one-way lanes on the pinned I2S0 duplex clock —
 *   capture: TDM read -> demux -> aec_process -> gain/knee -> 24->16 -> queue
 *   playback: RX ring -> resample 24k->device -> soft-knee -> I2S write
 * exposed as jr_audio_source_t / jr_audio_sink_t (with mute_now fast-kill).
 */
#include "jr_audio/jr_audio.h"

void jr_audio_todo_phase1(void)
{
    /* intentionally empty */
}
