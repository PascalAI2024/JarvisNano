/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_transport — L2 Transport adapter. Phase 0 placeholder.
 *
 * TODO(Phase 1): Gemini Live WSS impl of jr_realtime_voice_client_t. Owns WS
 * bytes + JSON only, never turn semantics. would-block => JR_ERR_WOULD_BLOCK
 * (drop-newest), never a socket teardown. Every server frame (incl. heartbeat)
 * is a liveness tick. Reconnect is an L3 transition, not a flag here.
 */
#include "jr_transport/jr_transport.h"

void jr_transport_todo_phase1(void)
{
    /* intentionally empty */
}
