/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_core/session.h — L3 Session / Turn State Machine (the domain heart).
 *
 * transition(state, event, clock) -> (next_state, command[]) is a PURE
 * function: no ESP-IDF, no globals, no blocking, no wall-clock, no I/O. It NEVER
 * performs an action; it only RETURNS a list of commands for the single owner
 * task to execute later. FreeRTOS, sockets, the codec, and the model protocol
 * all live OUTSIDE as adapters injected at L6. A broken dependency edge (this
 * file reaching for a driver/network/model header) fails the host build
 * immediately — that is the enforcement.
 *
 * This is the full Phase-1 CORE contract from
 * gigabrain/specs/phase1-core-state-machine.md:
 *   - 8 states, Live has 3 substates -> 10 configurations (jr_state_t)
 *   - 22 typed events                              (jr_event_kind_t)
 *   - 27 typed commands                            (jr_cmd_kind_t)
 *   - 86 legal transition rows + a shared DEATH handler
 *   - the ReconnectPolicy (capped exp backoff + park) used inside DEATH
 *
 * The reducer takes SessionState BY VALUE and returns the next SessionState +
 * an ordered command list; the owner replaces its state with the result.
 */
#ifndef JR_CORE_SESSION_H
#define JR_CORE_SESSION_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "jr_ports/jr_types.h"   /* jr_pcm_t, jr_err_t — pure port vocabulary */
#include "jr_ports/clock.h"      /* jr_clock_t — the only injected time source */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== *
 *  1. States (8 states; Live has 3 substates -> 10 configurations)         *
 * ======================================================================== */
typedef enum {
    JR_ST_IDLE = 0,      /* parked; only resting state reachable by UserStop */
    JR_ST_CONNECTING,    /* Connect emitted; WSS opening; keepalive(connect) */
    JR_ST_HANDSHAKING,   /* socket up; SendSetup emitted; awaiting setup     */
    JR_ST_LISTENING,     /* Live.Listening — capture streaming, DAC clear    */
    JR_ST_THINKING,      /* Live.Thinking  — turn committed, NoReply armed   */
    JR_ST_SPEAKING,      /* Live.Speaking  — server audio -> playback ring   */
    JR_ST_DRAINING,      /* graceful teardown on the UserStop path only      */
    JR_ST_RECONNECTING,  /* automatic recovery after involuntary death       */
    JR_ST_BACKOFF,       /* throttled/parked (storm or quota cool-off)       */
    JR_ST_FATAL,         /* unrecoverable / non-retryable                    */
    JR_ST__COUNT,        /* == 10 configurations                             */
} jr_state_t;

/* The L4 VAD strategy injected into L3 — a FIELD, not a state (spec §0). */
typedef enum {
    JR_VAD_MANUAL_LOCAL_RMS = 0, /* manual/PTT: activityStart/activityEnd     */
    JR_VAD_SERVER,               /* auto-VAD: audioStreamEnd keepalive        */
} jr_vad_mode_t;

/* Provider-neutral server-error classification (ServerError payload). */
typedef enum {
    JR_ERRK_QUOTA = 0,   /* -> Backoff, ~45s forced cool-off                  */
    JR_ERRK_AUTH,        /* -> Backoff (Fatal-adjacent park)                  */
    JR_ERRK_PROTOCOL,    /* permanent protocol reject -> park                 */
    JR_ERRK_TRANSIENT,   /* the default involuntary-death kind -> Reconnecting*/
    JR_ERRK_UNKNOWN,
} jr_error_kind_t;

/* TransportClosed payload discriminator. */
typedef enum {
    JR_CLOSE_CLEAN = 0,
    JR_CLOSE_ABRUPT,
} jr_close_kind_t;

/* ======================================================================== *
 *  2. Events (typed inputs — 22)                                           *
 * ======================================================================== */
typedef enum {
    /* --- 2.1 from transport (L2 RealtimeVoiceClient) — 10 --- */
    JR_EV_CONNECTED = 0,       /* WS/TCP open                                */
    JR_EV_SETUP_COMPLETE,      /* Gemini setupComplete {resumption_token?}   */
    JR_EV_SERVER_AUDIO_CHUNK,  /* a chunk of model audio (24 kHz)            */
    JR_EV_SERVER_INTERRUPTED,  /* Gemini interrupted (may lack turnComplete) */
    JR_EV_SERVER_TURN_COMPLETE,/* Gemini turnComplete/generationComplete     */
    JR_EV_SERVER_TOOL_CALL,    /* toolCall or toolCallCancellation           */
    JR_EV_TOOL_RESULT_READY,   /* worker result; owner must send response    */
    JR_EV_SERVER_GO_AWAY,      /* graceful server disconnect + resume handle */
    JR_EV_SERVER_ERROR,        /* server error frame {error_kind}            */
    JR_EV_TRANSPORT_CLOSED,    /* socket closed, any reason                  */
    JR_EV_HEARTBEAT,           /* any server frame = a liveness tick         */
    /* --- 2.2 from audio / turn-policy (L4) — 3 --- */
    JR_EV_SPEECH_STARTED,      /* isSpeech rising edge                       */
    JR_EV_SPEECH_ENDED,        /* end-of-turn silence past hangover          */
    JR_EV_BARGE_DETECTED,      /* adaptive barge gate crossed during Speaking*/
    /* --- 2.3 from monitors (injected resilience collaborators) — 5 --- */
    JR_EV_UPLINK_DEAD,         /* DeadUplinkMonitor threshold crossed        */
    JR_EV_STALE_DEADLINE,      /* KeepaliveMonitor: no server frame in time  */
    JR_EV_NO_REPLY_TIMEOUT,    /* NoReplyWatchdog: Thinking age > 20 s       */
    JR_EV_BACKOFF_ELAPSED,     /* scheduled backoff/cool-off timer fired     */
    JR_EV_CAPTURE_PAUSE_ELAPSED,/* auto-VAD capture pause > 1 s              */
    /* --- 2.4 from user / app (L6 / UI) — 3 --- */
    JR_EV_USER_START,          /* explicit arm (tap, /api/debug/say, boot)   */
    JR_EV_USER_STOP,           /* explicit stop — the ONLY route to Idle     */
    JR_EV_TAP,                 /* raw touch (arms / accelerates / no-op)     */
    JR_EV__COUNT,              /* == 22                                      */
} jr_event_kind_t;

/* The Event value struct. Only the fields relevant to `kind` are meaningful;
 * everything else is zero. Use jr_event() to build a zeroed event, then set the
 * payload fields the kind needs. */
typedef struct {
    jr_event_kind_t kind;

    /* ServerError */
    jr_error_kind_t error_kind;
    int             code;            /* ServerError / TransportClosed        */
    jr_close_kind_t close_kind;      /* TransportClosed                      */

    /* ServerToolCall */
    bool            is_cancellation;
    uint32_t        call_id;
    const char     *call_id_text;
    const char     *tool_name;
    const char     *tool_args;
    const char     *tool_response;  /* ToolResultReady: JSON object text       */
    uint32_t        session_gen;    /* ToolResultReady generation tag          */

    /* SetupComplete / ServerGoAway / Heartbeat: opaque resume handle,
     * 0 == none. Modeled as a small integer for the pure host machine. */
    uint32_t        resumption_token;
    const char     *reason;          /* ServerGoAway                         */

    /* ServerAudioChunk */
    const jr_pcm_t *pcm;
    size_t          pcm_len;
    uint32_t        sample_rate;
    uint32_t        seq;

    /* SpeechStarted / SpeechEnded / BargeDetected */
    float           rms;
    float           playback_peak;
    uint32_t        ts;
    uint32_t        silence_ms;

    /* monitors: UplinkDead / StaleDeadline / NoReplyTimeout / CapturePause */
    uint32_t        consecutive_tx_failures;
    uint32_t        age_ms;
} jr_event_t;

/* ======================================================================== *
 *  3. Commands (typed outputs — 27; the machine EXECUTES none of them)     *
 * ======================================================================== */
typedef enum {
    /* --- 3.1 transport (8) --- */
    JR_CMD_CONNECT = 0,             /* Connect{ resumption_token? }          */
    JR_CMD_SEND_SETUP,
    JR_CMD_SEND_ACTIVITY_START,     /* manual-VAD turn boundary              */
    JR_CMD_SEND_ACTIVITY_END,       /* manual-VAD turn boundary              */
    JR_CMD_SEND_AUDIO_STREAM_END,   /* auto-VAD keepalive ONLY (never manual)*/
    JR_CMD_SEND_TEXT,               /* SendText{ text } — /api/debug/say     */
    JR_CMD_SEND_AUDIO,              /* SendAudio{ frame } — pre-roll/inject  */
    JR_CMD_SEND_TOOL_RESPONSE,      /* SendToolResponse{ id,name,response }  */
    JR_CMD_CLOSE_TRANSPORT,         /* CloseTransport{ graceful }            */
    /* --- 3.2 audio / DAC (6) --- */
    JR_CMD_START_CAPTURE,
    JR_CMD_PAUSE_CAPTURE,
    JR_CMD_MUTE_DAC_NOW,            /* synchronous low-latency ES8311 kill    */
    JR_CMD_UNMUTE_DAC,              /* clears the kill flag (MUST on resume)  */
    JR_CMD_FLUSH_PLAYBACK_RING,
    JR_CMD_FEED_PLAYBACK,           /* FeedPlayback{ chunk }                  */
    /* --- 3.3 timers / monitors (8) --- */
    JR_CMD_SCHEDULE_BACKOFF,        /* ScheduleBackoff{ delay_ms }           */
    JR_CMD_CANCEL_BACKOFF,
    JR_CMD_ARM_KEEPALIVE,           /* ArmKeepalive{ deadline_ms }           */
    JR_CMD_DISARM_KEEPALIVE,
    JR_CMD_ARM_NO_REPLY_WATCHDOG,   /* ArmNoReplyWatchdog{ timeout_ms }      */
    JR_CMD_DISARM_NO_REPLY_WATCHDOG,
    JR_CMD_ARM_CAPTURE_PAUSE_TIMER, /* ArmCapturePauseTimer{ timeout_ms }    */
    JR_CMD_DISARM_CAPTURE_PAUSE_TIMER,
    /* --- 3.4 tools (2) --- */
    JR_CMD_DISPATCH_TOOL_CALL,      /* DispatchToolCall{ ..., session_gen }  */
    JR_CMD_CANCEL_TOOL_CALL,        /* CancelToolCall{ call_id }             */
    /* --- 3.5 observability (2) --- */
    JR_CMD_PUBLISH_SNAPSHOT,        /* PublishSnapshot{ phase, counters }    */
    JR_CMD_EMIT_DIAG,               /* EmitDiag{ event_type, reason, kind? } */
    JR_CMD__COUNT,                  /* == 27                                 */
} jr_cmd_kind_t;

/* The Command value struct. Only the fields relevant to `kind` are meaningful. */
typedef struct {
    jr_cmd_kind_t kind;

    uint32_t        resumption_token; /* Connect                             */
    bool            graceful;         /* CloseTransport                      */
    const char     *text;             /* SendText                            */

    uint32_t        delay_ms;         /* ScheduleBackoff                     */
    uint32_t        deadline_ms;      /* ArmKeepalive                        */
    uint32_t        timeout_ms;       /* ArmNoReplyWatchdog/ArmCapturePause  */

    uint32_t        call_id;          /* Dispatch/CancelToolCall             */
    const char     *call_id_text;     /* Dispatch/Cancel/SendToolResponse    */
    const char     *tool_name;        /* DispatchToolCall                    */
    const char     *tool_args;        /* DispatchToolCall                    */
    const char     *tool_response;    /* SendToolResponse JSON object        */
    uint32_t        session_gen;      /* DispatchToolCall (generation-tagged)*/

    const jr_pcm_t *pcm;              /* FeedPlayback / SendAudio            */
    size_t          pcm_len;

    jr_state_t      snapshot_phase;   /* PublishSnapshot                     */
    uint16_t        fail_count;       /* PublishSnapshot                     */

    jr_event_kind_t event_type;       /* EmitDiag                            */
    const char     *reason;           /* EmitDiag                            */
    jr_error_kind_t error_kind;       /* EmitDiag                            */
} jr_command_t;

/* Worst-case command fan-out is the DEATH handler:
 * MuteDacNow, FlushPlaybackRing, PauseCapture, CloseTransport, DisarmKeepalive,
 * DisarmNoReplyWatchdog, DisarmCapturePauseTimer, EmitDiag, ScheduleBackoff,
 * + the implicit PublishSnapshot = 10. 12 leaves headroom. */
#define JR_MAX_CMDS 12

typedef struct {
    jr_command_t cmds[JR_MAX_CMDS];
    size_t       count;
} jr_cmd_list_t;

/* ======================================================================== *
 *  0. SessionState — the value struct carried by the single owner task     *
 * ======================================================================== */
typedef struct {
    jr_state_t    phase;           /* the enumerated state / substate        */
    uint16_t      fail_count;      /* consecutive involuntary deaths          */
    uint64_t      last_success_ts; /* time of last Handshaking->Live.Listening*/
    uint32_t      resumption_token;/* last server-issued resume handle (0=none)*/
    uint32_t      session_gen;     /* ++ on entry to Connecting; stamps jobs  */
    jr_vad_mode_t vad_mode;        /* the injected L4 strategy (field)        */
} jr_session_t;

/* Outcome of a transition: the full next SessionState + the ordered command
 * list. `illegal` is set (with next == input, no effectful commands, one
 * EmitDiag(illegal)) when (phase,event) is not a legal pair. */
typedef struct {
    jr_session_t  next;
    jr_cmd_list_t cmds;
    bool          illegal;
} jr_outcome_t;

/* ======================================================================== *
 *  9. Constants (single source of truth; runtime-tunable seams on device)  *
 * ======================================================================== */
#define JR_BASE_MS               1000u
#define JR_CAP_MS                16000u
#define JR_PARK_AFTER            6u
#define JR_HEALTHY_UPTIME_MS     15000u
#define JR_QUOTA_COOLOFF_MS      45000u
#define JR_NOREPLY_MS            20000u
#define JR_CAPTURE_PAUSE_MS      1000u
#define JR_TX_FAIL_RESUME        25u
#define JR_LIVE_DEADLINE_MS      45000u
#define JR_CONNECT_DEADLINE_MS   10000u  /* Phase 1 tune */
#define JR_HANDSHAKE_DEADLINE_MS 10000u  /* Phase 1 tune */
#define JR_DRAIN_DEADLINE_MS     2000u   /* Phase 1 tune (~2000) */

/* ======================================================================== *
 *  The pure transition function + collaborators                            *
 * ======================================================================== */

/* The pure reducer. Given the current SessionState and one Event (with the
 * injected Clock for the DEATH handler's uptime/backoff math), returns the next
 * SessionState and the ordered list of commands to execute. ZERO I/O. */
jr_outcome_t jr_transition(jr_session_t s, jr_event_t e, jr_clock_t clk);

/* ReconnectPolicy (spec §5.1): capped exponential backoff + park. Pure; exposed
 * so the backoff curve / storm-park / healthy-uptime reset are host-testable
 * directly as well as through DEATH. */
typedef struct {
    uint32_t delay_ms;
    bool     should_park;
} jr_reconnect_decision_t;

jr_reconnect_decision_t jr_reconnect_policy(uint16_t fail_count,
                                            uint64_t last_success_uptime_ms,
                                            jr_error_kind_t error_kind);

/* Generation-tagging (spec §3.4 / T15): a result stamped with a stale gen is
 * dropped by the owner. True == stale (drop it). */
static inline bool jr_session_gen_is_stale(uint32_t current_gen, uint32_t tagged_gen)
{
    return tagged_gen != current_gen;
}

/* Backpressure classifier (spec §5.6 / T25): a would-block is NOT a death; the
 * framer drops one recoverable frame and stays Live. True == recoverable
 * backpressure (never call DEATH). */
static inline bool jr_err_is_backpressure(jr_err_t e)
{
    return e == JR_ERR_WOULD_BLOCK;
}

/* Zeroed builders for readable tests. */
jr_event_t   jr_event(jr_event_kind_t kind);
jr_session_t jr_session_init(jr_vad_mode_t vad_mode); /* Idle, counters zeroed */

/* Convenience predicate: a "Live" state is producing/consuming audio. */
bool jr_state_is_live(jr_state_t state);

/* Human-readable names for logs/diag/test failures. */
const char *jr_state_name(jr_state_t state);
const char *jr_event_name(jr_event_kind_t event);
const char *jr_cmd_name(jr_cmd_kind_t cmd);

#ifdef __cplusplus
}
#endif

#endif /* JR_CORE_SESSION_H */
