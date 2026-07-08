/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * jr_transport/gemini_live.h — the L2 Gemini Live transport.
 *
 * Three host-testable units, all ESP-IDF-free (they depend only on jr_ports
 * headers, jr_core types, libc, and the vendored cJSON — the SAME source builds
 * the host suite and the device build):
 *
 *   1. BUILDERS  — setup / activityStart / activityEnd / audioStreamEnd / audio.
 *                  Shapes harvested from firmware `gl_send_setup` (:1522) and
 *                  verified against docs/reference/gemini-live-api-v5.md.
 *   2. FRAMER    — the would-block-as-backpressure send path. A would-block /
 *                  0-byte / partial send buffers-or-drops (drop-newest, bounded)
 *                  and KEEPS the connection. It NEVER aborts (v4's worst bug).
 *   3. PARSER    — inbound server JSON -> typed events (server-audio, turn signals,
 *                  goAway{seconds}, sessionResumptionUpdate, toolCall/cancel, error).
 *
 * Upper seam: this impl satisfies the provider-neutral `jr_realtime_voice_client_t`
 * port (jr_ports/realtime_voice_client.h). Lower seam: it drives a byte-level
 * `jr_ws_transport_t` (jr_ports/ws_transport.h) — a device adapter on hardware,
 * a fake on host. Reconnect is NOT here; it is an L3 transition (spec §5.1).
 */
#ifndef JR_TRANSPORT_GEMINI_LIVE_H
#define JR_TRANSPORT_GEMINI_LIVE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "jr_ports/jr_types.h"
#include "jr_ports/clock.h"
#include "jr_ports/ws_transport.h"
#include "jr_ports/realtime_voice_client.h"
#include "jr_core/monitors.h"   /* the Run-2 monitors this transport feeds */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== *
 *  Model pin (docs/reference/gemini-live-api-v5.md §2, ADR 2026-07-07)      *
 * ======================================================================== */
/* Primary: real + current flagship Live/native-audio preview (released
 * 2026-03-11, in Google's own get-started samples). Retest before device
 * commit — v4 saw a stale 404 on this string (unresolved); fall back if so. */
#define JR_GEMINI_MODEL_PRIMARY  "models/gemini-3.1-flash-live-preview"
/* Fallback: successor to the confirmed-dead -09-2025 string. */
#define JR_GEMINI_MODEL_FALLBACK "models/gemini-2.5-flash-native-audio-preview-12-2025"

/* Uplink is 16 kHz PCM16 mono; the model's downlink is a fixed 24 kHz. */
#define JR_GEMINI_TX_RATE 16000u
#define JR_GEMINI_RX_RATE 24000u

/* ======================================================================== *
 *  BUILDERS                                                                 *
 * ======================================================================== */

/* A single declared function tool (name + description + one string arg). The
 * harvested setup declares several of these alongside googleSearch; the builder
 * proves the two tool types coexist. */
typedef struct {
    const char *name;
    const char *description;
    const char *arg_name;   /* NULL => a no-arg declaration */
    const char *arg_desc;
} jr_gemini_fn_decl_t;

typedef struct {
    const char *url;                /* wss endpoint (contains the API key at
                                     * runtime — NEVER hardcode in-repo); passed
                                     * to ws.connect(). NULL => "" for host tests*/
    const char *model;              /* NULL => JR_GEMINI_MODEL_PRIMARY        */
    const char *system_instruction; /* NULL => omit systemInstruction         */
    const char *thinking_level;     /* NULL => "low"; e.g. "minimal"/"low"    */
    jr_vad_mode_t vad_mode;         /* MANUAL => PTT (disabled+NO_INTERRUPTION)*/
    bool  google_search;            /* include the googleSearch tool          */
    bool  input_transcription;      /* include inputAudioTranscription {}      */
    const jr_gemini_fn_decl_t *fns; /* functionDeclarations (may be NULL)      */
    size_t fn_count;
} jr_gemini_config_t;

/* Build the setup message. Returns a malloc'd NUL-terminated JSON string the
 * caller frees, or NULL on OOM. thinkingLevel is nested under
 * generationConfig.thinkingConfig (NOT flat). Manual mode sets
 * automaticActivityDetection.disabled=true + activityHandling=NO_INTERRUPTION.
 * NEVER contains audioStreamEnd. */
char *jr_gemini_build_setup(const jr_gemini_config_t *cfg);

/* Small realtimeInput control frames (malloc'd; caller frees). */
char *jr_gemini_build_activity_start(void);   /* {"realtimeInput":{"activityStart":{}}} */
char *jr_gemini_build_activity_end(void);     /* {"realtimeInput":{"activityEnd":{}}}   */
char *jr_gemini_build_audio_stream_end(void); /* auto-VAD ONLY — never in manual mode   */
char *jr_gemini_build_text_turn(const char *text); /* clientContent user turn, turnComplete */

/* Encode a PCM16 mono frame as a realtimeInput audio blob. `samples` is the
 * count of int16 samples; `rate` is the mimeType rate (JR_GEMINI_TX_RATE).
 * Returns malloc'd JSON, or NULL on OOM. */
char *jr_gemini_build_audio_chunk(const jr_pcm_t *pcm, size_t samples, uint32_t rate);

/* ======================================================================== *
 *  PARSER — inbound server frame -> typed events                           *
 * ======================================================================== */

/* Provider-neutral error classification (mirrors jr_core's jr_error_kind_t but
 * lives in the transport layer; the L3 adapter maps one to the other). */
typedef enum {
    JR_GEMINI_ERRK_QUOTA = 0,  /* 429 / RESOURCE_EXHAUSTED / "quota"          */
    JR_GEMINI_ERRK_AUTH,       /* 401 / 403 / UNAUTHENTICATED / PERMISSION    */
    JR_GEMINI_ERRK_PROTOCOL,   /* 400 / INVALID_ARGUMENT                      */
    JR_GEMINI_ERRK_TRANSIENT,  /* the default involuntary-death kind          */
    JR_GEMINI_ERRK_UNKNOWN,
} jr_gemini_error_kind_t;

typedef enum {
    JR_GEV_SETUP_COMPLETE = 0,
    JR_GEV_AUDIO_CHUNK,          /* pcm / pcm_len / sample_rate (decoded)      */
    JR_GEV_TEXT,                 /* text                                       */
    JR_GEV_INTERRUPTED,          /* serverContent.interrupted == true          */
    JR_GEV_TURN_COMPLETE,        /* serverContent.turnComplete == true         */
    JR_GEV_GENERATION_COMPLETE,  /* serverContent.generationComplete == true   */
    JR_GEV_TOOL_CALL,            /* call_id / tool_name / tool_args            */
    JR_GEV_TOOL_CANCEL,          /* call_id (first cancelled id)               */
    JR_GEV_GO_AWAY,              /* go_away_seconds (protobuf Duration decoded)*/
    JR_GEV_RESUMPTION_UPDATE,    /* heartbeat: resumable + resumption_token    */
    JR_GEV_ERROR,                /* error_kind / code / message                */
    JR_GEV_UNKNOWN,              /* unrecognized top-level frame               */
} jr_gemini_event_kind_t;

typedef struct {
    jr_gemini_event_kind_t kind;

    /* AUDIO_CHUNK: decoded 24 kHz mono PCM16. Owned by the jr_gemini_parse_t;
     * valid until jr_gemini_parse_free(). */
    jr_pcm_t *pcm;
    size_t    pcm_len;      /* sample count */
    uint32_t  sample_rate;

    /* TEXT */
    const char *text;       /* points into the retained cJSON tree */

    /* TOOL_CALL / TOOL_CANCEL */
    uint32_t    call_id;
    const char *tool_name;
    const char *tool_args;

    /* GO_AWAY */
    uint32_t go_away_seconds;

    /* RESUMPTION_UPDATE: token is nonzero ONLY when resumable && handle set
     * (spec: only usable handles are stored). */
    bool        resumable;
    uint32_t    resumption_token;
    const char *handle;

    /* ERROR */
    jr_gemini_error_kind_t error_kind;
    int         code;
    const char *message;
} jr_gemini_event_t;

/* A frame may demux into several events (e.g. modelTurn audio + turnComplete). */
#define JR_GEMINI_MAX_EVENTS 8

typedef struct {
    jr_gemini_event_t events[JR_GEMINI_MAX_EVENTS];
    size_t            count;
    void             *_root;   /* retained cJSON tree (internal; do not touch) */
} jr_gemini_parse_t;

/* Parse ONE inbound server frame. Returns true if `json` was valid JSON (even
 * an unknown frame yields one JR_GEV_UNKNOWN event and true); false only on a
 * parse failure. Call jr_gemini_parse_free() when done to release the retained
 * tree + any decoded audio. Uses cJSON — never a hand-rolled untrusted parser. */
bool jr_gemini_parse(const char *json, jr_gemini_parse_t *out);
void jr_gemini_parse_free(jr_gemini_parse_t *out);

/* ======================================================================== *
 *  CLIENT — the RealtimeVoiceClient impl over a ws_transport (framer + pump)*
 * ======================================================================== */

/* Bounded outbound buffer: drop-newest past capacity keeps memory hard-bounded
 * under a starved consumer. Each slot holds one (possibly partial) frame. */
#ifndef JR_GEMINI_TXQ_DEPTH
#define JR_GEMINI_TXQ_DEPTH 8
#endif
#ifndef JR_GEMINI_TXQ_SLOT
#define JR_GEMINI_TXQ_SLOT 4096
#endif
/* Max inbound WS text frame the pump reads at once (a 24 kHz audio chunk in
 * base64 is the large case). */
#ifndef JR_GEMINI_RX_MAX
#define JR_GEMINI_RX_MAX 16384
#endif

typedef struct {
    char   buf[JR_GEMINI_TXQ_SLOT];
    size_t len;
    size_t off;   /* bytes already written (partial-frame resume) */
} jr_gemini_txframe_t;

/* Independent uplink/downlink liveness so a dead-uplink/live-downlink
 * split-brain is observable (spec §5.3). */
typedef struct {
    uint64_t last_rx_ms;              /* downlink: last inbound frame time     */
    uint32_t consecutive_tx_failures; /* uplink: soft-fails in a row (not w-b) */
    uint32_t tx_would_block;          /* backpressure count (NOT a failure)    */
    uint32_t tx_drops;                /* frames dropped-newest under overflow  */
    bool     socket_open;
} jr_gemini_liveness_t;

/* Rich event out-channel (carries the full parser contract). Distinct from the
 * neutral jr_rvc_event_cb, which the client ALSO drives for the port. */
typedef void (*jr_gemini_event_cb)(void *user, const jr_gemini_event_t *ev);

typedef struct jr_gemini_client {
    jr_ws_transport_t  ws;
    jr_clock_t         clk;
    jr_gemini_config_t cfg;

    /* outbound framer state */
    jr_gemini_txframe_t txq[JR_GEMINI_TXQ_DEPTH];
    size_t head, tail, depth;

    jr_gemini_liveness_t live;

    /* optional injected monitors this transport feeds (may be NULL) */
    jr_keepalive_monitor_t   *keepalive;
    jr_dead_uplink_monitor_t *dead_uplink;

    /* event out-channels */
    jr_gemini_event_cb rich_cb;  void *rich_user;
    jr_rvc_event_cb    rvc_cb;   void *rvc_user;
} jr_gemini_client_t;

/* Initialize a client over a byte transport + clock. `cfg` is copied by value
 * (its string pointers must outlive the client). */
void jr_gemini_client_init(jr_gemini_client_t *c, jr_ws_transport_t ws,
                           jr_clock_t clk, const jr_gemini_config_t *cfg);

/* Wire the injected monitors this transport feeds (optional). */
void jr_gemini_client_set_monitors(jr_gemini_client_t *c,
                                   jr_keepalive_monitor_t *ka,
                                   jr_dead_uplink_monitor_t *du);

/* Register the rich event out-channel. */
void jr_gemini_client_set_event_cb(jr_gemini_client_t *c,
                                   jr_gemini_event_cb cb, void *user);

/* A provider-neutral RealtimeVoiceClient VIEW of this client (upper seam). The
 * returned struct's ctx points at `c`; it stays valid while `c` does. */
jr_realtime_voice_client_t jr_gemini_client_as_rvc(jr_gemini_client_t *c);

/* THE FRAMER (the single most important correctness win): send one already-built
 * frame. Returns:
 *   JR_OK             — fully sent now
 *   JR_ERR_WOULD_BLOCK— backpressure: buffered or drop-newest; connection KEPT
 *   JR_ERR_CLOSED     — socket reported closed: the death path
 * A soft failure (socket still open) is counted for the DeadUplinkMonitor and
 * reported as JR_ERR_WOULD_BLOCK (never a single-send teardown). Never aborts. */
jr_err_t jr_gemini_send_frame(jr_gemini_client_t *c, const char *json, size_t len);

/* Try to drain buffered frames onto a freed socket (partial-write aware). Call
 * from the owner's pump. Returns JR_OK / JR_ERR_WOULD_BLOCK / JR_ERR_CLOSED. */
jr_err_t jr_gemini_flush(jr_gemini_client_t *c);

/* Pump one inbound frame (if any): recv -> parse -> emit events + liveness tick.
 * Returns true if a frame was processed. */
bool jr_gemini_pump_rx(jr_gemini_client_t *c);

/* Depth of the outbound buffer (bounded-memory assertions in tests). */
static inline size_t jr_gemini_txq_depth(const jr_gemini_client_t *c) { return c->depth; }

#ifdef __cplusplus
}
#endif

#endif /* JR_TRANSPORT_GEMINI_LIVE_H */
