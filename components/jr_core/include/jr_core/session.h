/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/session.h — L3 Session / Turn State Machine (the domain heart).
 *
 * transition(state, event) -> (next_state, command[]) is a PURE function: no
 * ESP-IDF, no globals, no blocking, no I/O. FreeRTOS, sockets, the codec, the
 * model protocol all live OUTSIDE as adapters injected at L6. A broken
 * dependency edge (this file reaching for a driver header) fails the host
 * build immediately — that is the enforcement (architecture.md §"The one rule").
 *
 * Phase 0 scope: the state enum, the typed event enum, the command enum, and a
 * jr_transition() skeleton that implements the happy-path forward spine +
 * UserStop->Idle + one death->Reconnecting edge, and returns IDENTITY (same
 * state, no commands) for every other pair. The full legal-transition table
 * and all resilience routing land in Phase 1.
 */
#ifndef JR_CORE_SESSION_H
#define JR_CORE_SESSION_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle states. Names kept from v4 gl_state_t; the scattered EventGroup-bit
 * implementation is deliberately NOT harvested (it is the disease). Live =
 * {Listening, Thinking, Speaking}. */
typedef enum {
    JR_ST_IDLE = 0,
    JR_ST_CONNECTING,
    JR_ST_HANDSHAKING,   /* transport open, awaiting setupComplete (READY) */
    JR_ST_LISTENING,     /* Live */
    JR_ST_THINKING,      /* Live */
    JR_ST_SPEAKING,      /* Live */
    JR_ST_DRAINING,      /* flushing playback before a clean stop/reconnect  */
    JR_ST_RECONNECTING,
    JR_ST_BACKOFF,       /* parked between reconnect attempts               */
    JR_ST_FATAL,
    JR_ST__COUNT,
} jr_state_t;

/* Typed events. Involuntary death (TransportClosed/UplinkDead/StaleReconnect)
 * is DISTINCT from UserStop — the structural cure for "doesn't talk unless I
 * type": death routes to Reconnecting, only UserStop routes to Idle. */
typedef enum {
    JR_EV_NONE = 0,
    JR_EV_CONNECT,            /* user/boot asks to start a session      */
    JR_EV_TRANSPORT_OPEN,     /* WS connected                           */
    JR_EV_SETUP_COMPLETE,     /* server setupComplete (handshake done)  */
    JR_EV_SPEECH_START,       /* L4: user speech detected               */
    JR_EV_SPEECH_END,         /* L4: user speech ended                  */
    JR_EV_AUDIO_CHUNK,        /* first model audio of a turn            */
    JR_EV_TURN_COMPLETE,      /* server turnComplete                    */
    JR_EV_INTERRUPTED,        /* server interrupted (barge/interrupt)   */
    JR_EV_TOOL_CALL,          /* server toolCall                        */
    JR_EV_USER_STOP,          /* the ONLY route to Idle                 */
    JR_EV_TRANSPORT_CLOSED,   /* involuntary: WS close/error            */
    JR_EV_UPLINK_DEAD,        /* DeadUplinkMonitor: tx failures crossed */
    JR_EV_STALE_RECONNECT,    /* KeepaliveMonitor: heartbeat gap        */
    JR_EV_NO_REPLY,           /* NoReply watchdog: 20s silent Thinking  */
    JR_EV_QUOTA_ERROR,        /* provider quota/rate error -> Backoff   */
    JR_EV_BACKOFF_ELAPSED,    /* ReconnectPolicy delay elapsed          */
    JR_EV__COUNT,
} jr_event_t;

/* Commands the core emits for adapters to execute. The core NEVER performs I/O
 * itself; it returns intent. */
typedef enum {
    JR_CMD_NONE = 0,
    JR_CMD_OPEN_TRANSPORT,
    JR_CMD_SEND_SETUP,
    JR_CMD_START_CAPTURE,
    JR_CMD_STOP_CAPTURE,
    JR_CMD_FLUSH_PLAYBACK,
    JR_CMD_MUTE_NOW,          /* fast-kill barge shortcut               */
    JR_CMD_RECONNECT,
    JR_CMD_PARK,              /* enter backoff (ReconnectPolicy)        */
    JR_CMD_RESUME_LISTENING,
    JR_CMD_SEND_AUDIO_STREAM_END,
    JR_CMD_PUBLISH_SNAPSHOT,
    JR_CMD__COUNT,
} jr_cmd_t;

/* A transition never emits more than a handful of commands. */
#define JR_MAX_CMDS 4

typedef struct {
    jr_state_t next;
    jr_cmd_t   cmds[JR_MAX_CMDS];
    size_t     cmd_count;
} jr_transition_result_t;

/* The pure transition function. Illegal/unknown pairs return identity (next ==
 * state, cmd_count == 0) in Phase 0; Phase 1 makes illegal pairs asserts. */
jr_transition_result_t jr_transition(jr_state_t state, jr_event_t event);

/* Convenience predicate: a "Live" state is producing/consuming audio. Used by
 * the composition root and the presenter. */
bool jr_state_is_live(jr_state_t state);

/* Human-readable name for logs/diag. */
const char *jr_state_name(jr_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_SESSION_H */
