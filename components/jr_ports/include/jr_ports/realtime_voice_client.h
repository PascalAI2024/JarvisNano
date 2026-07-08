/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_ports/realtime_voice_client.h — RealtimeVoiceClient port (L2 transport).
 *
 * Owns WS bytes + JSON (de)serialization ONLY, never turn semantics. Gemini
 * Live WSS is one impl; a host `mock` and a future OpenAI-Realtime/WebRTC impl
 * are others. The command surface (connect / send / close) is what the core
 * calls; the event surface (a provider-neutral out-channel) is what the impl
 * calls back with. Reconnect is NOT a flag here — it is an L3 transition
 * (architecture.md §L2).
 */
#ifndef JR_PORTS_REALTIME_VOICE_CLIENT_H
#define JR_PORTS_REALTIME_VOICE_CLIENT_H

#include "jr_ports/jr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Provider-neutral control frames the core may ask the transport to send. */
typedef enum {
    JR_RVC_CTRL_ACTIVITY_START, /* manual-VAD: user speech began */
    JR_RVC_CTRL_ACTIVITY_END,   /* manual-VAD: user speech ended  */
    JR_RVC_CTRL_AUDIO_STREAM_END, /* auto-VAD keepalive: capture paused >1s */
} jr_rvc_control_t;

/* Provider-neutral inbound events. Every frame (incl. heartbeat) is also a
 * liveness tick for the KeepaliveMonitor. */
typedef enum {
    JR_RVC_EV_SETUP_COMPLETE,
    JR_RVC_EV_AUDIO_CHUNK,
    JR_RVC_EV_INTERRUPTED,
    JR_RVC_EV_TOOL_CALL,
    JR_RVC_EV_TURN_COMPLETE,
    JR_RVC_EV_ERROR,
    JR_RVC_EV_CLOSE,
    JR_RVC_EV_HEARTBEAT,
} jr_rvc_event_kind_t;

typedef struct {
    jr_rvc_event_kind_t kind;
    /* Valid for JR_RVC_EV_AUDIO_CHUNK: 24 kHz mono PCM, owned by the transport
     * for the duration of the callback. */
    const jr_pcm_t *audio;
    size_t          audio_samples;
    /* Valid for JR_RVC_EV_ERROR: provider-neutral classification. */
    jr_err_t        error;
} jr_rvc_event_t;

typedef void (*jr_rvc_event_cb)(void *user, const jr_rvc_event_t *ev);

typedef struct jr_realtime_voice_client {
    void *ctx;
    /* Register the neutral event out-channel before connect(). */
    void      (*set_event_cb)(void *ctx, jr_rvc_event_cb cb, void *user);
    jr_err_t  (*connect)(void *ctx);
    /* 16 kHz mono PCM uplink. Would-block => JR_ERR_WOULD_BLOCK (drop-newest,
     * NEVER teardown). */
    jr_err_t  (*send_audio)(void *ctx, const jr_pcm_t *frame, size_t samples);
    jr_err_t  (*send_text)(void *ctx, const char *text);
    jr_err_t  (*send_control)(void *ctx, jr_rvc_control_t ctl);
    void      (*close)(void *ctx);
} jr_realtime_voice_client_t;

#ifdef __cplusplus
}
#endif

#endif /* JR_PORTS_REALTIME_VOICE_CLIENT_H */
