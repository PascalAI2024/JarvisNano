/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/src/session.c — L3 transition table (Phase 0 skeleton).
 *
 * PURE. Includes only its own header + the C standard library. No IDF, no
 * driver, no network header — the host build proves it.
 */
#include "jr_core/session.h"

/* Small builder so each edge reads as data, not control flow. */
static jr_transition_result_t identity(jr_state_t s)
{
    jr_transition_result_t r;
    r.next = s;
    r.cmd_count = 0;
    return r;
}

static jr_transition_result_t to(jr_state_t next,
                                 jr_cmd_t c0, jr_cmd_t c1)
{
    jr_transition_result_t r;
    r.next = next;
    r.cmd_count = 0;
    if (c0 != JR_CMD_NONE) { r.cmds[r.cmd_count++] = c0; }
    if (c1 != JR_CMD_NONE) { r.cmds[r.cmd_count++] = c1; }
    return r;
}

jr_transition_result_t jr_transition(jr_state_t state, jr_event_t event)
{
    /* Involuntary death from ANY non-terminal state routes to Reconnecting.
     * Only JR_EV_USER_STOP (handled below) routes to Idle. This asymmetry is
     * the structural cure for "doesn't talk unless I type". */
    if ((event == JR_EV_TRANSPORT_CLOSED ||
         event == JR_EV_UPLINK_DEAD ||
         event == JR_EV_STALE_RECONNECT) &&
        state != JR_ST_IDLE && state != JR_ST_FATAL) {
        return to(JR_ST_RECONNECTING, JR_CMD_FLUSH_PLAYBACK, JR_CMD_RECONNECT);
    }

    /* UserStop from any live/connecting state -> Idle. */
    if (event == JR_EV_USER_STOP && state != JR_ST_IDLE) {
        return to(JR_ST_IDLE, JR_CMD_STOP_CAPTURE, JR_CMD_NONE);
    }

    /* Happy-path forward spine. */
    switch (state) {
    case JR_ST_IDLE:
        if (event == JR_EV_CONNECT) {
            return to(JR_ST_CONNECTING, JR_CMD_OPEN_TRANSPORT, JR_CMD_NONE);
        }
        break;
    case JR_ST_CONNECTING:
        if (event == JR_EV_TRANSPORT_OPEN) {
            return to(JR_ST_HANDSHAKING, JR_CMD_SEND_SETUP, JR_CMD_NONE);
        }
        break;
    case JR_ST_HANDSHAKING:
        if (event == JR_EV_SETUP_COMPLETE) {
            return to(JR_ST_LISTENING, JR_CMD_START_CAPTURE, JR_CMD_PUBLISH_SNAPSHOT);
        }
        break;
    case JR_ST_LISTENING:
        if (event == JR_EV_SPEECH_END) {
            return to(JR_ST_THINKING, JR_CMD_PUBLISH_SNAPSHOT, JR_CMD_NONE);
        }
        break;
    case JR_ST_THINKING:
        if (event == JR_EV_AUDIO_CHUNK) {
            return to(JR_ST_SPEAKING, JR_CMD_PUBLISH_SNAPSHOT, JR_CMD_NONE);
        }
        if (event == JR_EV_NO_REPLY) {
            return to(JR_ST_LISTENING, JR_CMD_RESUME_LISTENING, JR_CMD_PUBLISH_SNAPSHOT);
        }
        break;
    case JR_ST_SPEAKING:
        if (event == JR_EV_TURN_COMPLETE) {
            return to(JR_ST_LISTENING, JR_CMD_PUBLISH_SNAPSHOT, JR_CMD_NONE);
        }
        if (event == JR_EV_INTERRUPTED) {
            return to(JR_ST_LISTENING, JR_CMD_MUTE_NOW, JR_CMD_PUBLISH_SNAPSHOT);
        }
        break;
    case JR_ST_RECONNECTING:
        if (event == JR_EV_BACKOFF_ELAPSED) {
            return to(JR_ST_CONNECTING, JR_CMD_OPEN_TRANSPORT, JR_CMD_NONE);
        }
        if (event == JR_EV_QUOTA_ERROR) {
            return to(JR_ST_BACKOFF, JR_CMD_PARK, JR_CMD_NONE);
        }
        break;
    case JR_ST_BACKOFF:
        if (event == JR_EV_BACKOFF_ELAPSED) {
            return to(JR_ST_CONNECTING, JR_CMD_OPEN_TRANSPORT, JR_CMD_NONE);
        }
        break;
    default:
        break;
    }

    /* Phase 0: unknown pair is identity (Phase 1 will assert on illegal). */
    return identity(state);
}

bool jr_state_is_live(jr_state_t state)
{
    return state == JR_ST_LISTENING ||
           state == JR_ST_THINKING ||
           state == JR_ST_SPEAKING;
}

const char *jr_state_name(jr_state_t state)
{
    switch (state) {
    case JR_ST_IDLE:         return "Idle";
    case JR_ST_CONNECTING:   return "Connecting";
    case JR_ST_HANDSHAKING:  return "Handshaking";
    case JR_ST_LISTENING:    return "Listening";
    case JR_ST_THINKING:     return "Thinking";
    case JR_ST_SPEAKING:     return "Speaking";
    case JR_ST_DRAINING:     return "Draining";
    case JR_ST_RECONNECTING: return "Reconnecting";
    case JR_ST_BACKOFF:      return "Backoff";
    case JR_ST_FATAL:        return "Fatal";
    default:                 return "?";
    }
}
